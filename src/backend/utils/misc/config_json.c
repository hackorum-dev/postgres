/*-------------------------------------------------------------------------
 *
 * config_json.c
 *	  Loader for JSON-format configuration files.
 *
 * A JSON client authentication file, named by hba_file, ident_file or
 * hosts_file in place of the legacy file, is one object holding a single
 * section ("hba", "ident" or "hosts") and, optionally, an "include" key
 * naming further files like it.  We parse the document with the common
 * JSON parser into a small tree that keeps key order and source
 * positions, resolve includes, and hand back the documents in load order,
 * so that the loader of the section can pick out its parts.
 *
 * Errors about a value carry the value's location as error context: its
 * path in the document, such as $.include[1] or $["pg_stat_statements.max"],
 * and the file and line.  Where the message has to be one string, as in
 * the views, the location is appended to it in parentheses.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/config_json.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>

#include "common/jsonapi.h"
#include "guc_internal.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "storage/fd.h"
#include "utils/conffiles.h"
#include "utils/config_json.h"
#include "utils/guc_tables.h"
#include "utils/json.h"

/*
 * State of the tree builder while pg_parse_json walks one document.
 */
typedef struct JsonConfParseState
{
	JsonLexContext *lex;
	const char *file;
	JsonConfNode *root;
	JsonConfNode *current;		/* innermost open container */
	int			pending_line;	/* line captured at field/element start */
	char	   *err;			/* set on semantic failure */
	char	   *err_path;		/* where, for a semantic failure */
} JsonConfParseState;

static bool load_document(const char *path, const char *calling_file,
						  bool strict, int depth, int elevel,
						  const char *section,
						  ConfigVariable **head_p, ConfigVariable **tail_p,
						  List **documents_p, JsonConfNode *directive);
static void append_path_key(StringInfo buf, const char *key);


/*
 * Tree builder callbacks for pg_parse_json.
 *
 * The lexer's line_number is the line of the token it has most recently
 * read.  When object_field_start or array_element_start fires, that is the
 * first token of the value, which is what we want to remember for the
 * node.  By the time the scalar callback fires the lexer has moved on, so
 * the line is taken from pending_line instead.  The root container has no
 * such hint and uses the current line.
 */
static JsonConfNode *
new_node(JsonConfParseState *st, JsonConfNodeType type)
{
	JsonConfNode *n = palloc0_object(JsonConfNode);

	n->type = type;
	n->file = st->file;
	n->line = st->pending_line ? st->pending_line : st->lex->line_number;
	n->index = -1;
	return n;
}

/* Attach a freshly made node under the current container. */
static void
attach_node(JsonConfParseState *st, JsonConfNode *n)
{
	JsonConfNode *parent = st->current;

	if (parent == NULL)
	{
		st->root = n;
		return;
	}
	n->parent = parent;
	if (parent->type == JCN_OBJECT)
	{
		/* key was pushed by jc_object_field_start */
		n->key = llast(parent->keys);
	}
	else
		n->index = list_length(parent->values);
	parent->values = lappend(parent->values, n);
}

static JsonParseErrorType
jc_object_start(void *state)
{
	JsonConfParseState *st = state;
	JsonConfNode *n = new_node(st, JCN_OBJECT);

	attach_node(st, n);
	st->current = n;
	st->pending_line = 0;
	return JSON_SUCCESS;
}

static JsonParseErrorType
jc_array_start(void *state)
{
	JsonConfParseState *st = state;
	JsonConfNode *n = new_node(st, JCN_ARRAY);

	attach_node(st, n);
	st->current = n;
	st->pending_line = 0;
	return JSON_SUCCESS;
}

static JsonParseErrorType
jc_container_end(void *state)
{
	JsonConfParseState *st = state;

	st->current = st->current->parent;
	return JSON_SUCCESS;
}

static JsonParseErrorType
jc_object_field_start(void *state, char *fname, bool isnull)
{
	JsonConfParseState *st = state;
	JsonConfNode *obj = st->current;
	ListCell   *lc;

	/*
	 * JSON itself allows repeated keys, with unspecified meaning.  For a
	 * configuration file silently taking either occurrence would hide
	 * mistakes, so refuse.
	 */
	foreach(lc, obj->keys)
	{
		if (strcmp((char *) lfirst(lc), fname) == 0)
		{
			StringInfoData path;

			initStringInfo(&path);
			appendStringInfoString(&path, config_json_path(obj));
			append_path_key(&path, fname);
			st->err = psprintf(_("duplicate key \"%s\""), fname);
			st->err_path = path.data;
			return JSON_SEM_ACTION_FAILED;
		}
	}
	obj->keys = lappend(obj->keys, fname);
	st->pending_line = st->lex->line_number;
	return JSON_SUCCESS;
}

static JsonParseErrorType
jc_array_element_start(void *state, bool isnull)
{
	JsonConfParseState *st = state;

	st->pending_line = st->lex->line_number;
	return JSON_SUCCESS;
}

static JsonParseErrorType
jc_scalar(void *state, char *token, JsonTokenType tokentype)
{
	JsonConfParseState *st = state;
	JsonConfNode *n;

	switch (tokentype)
	{
		case JSON_TOKEN_STRING:
			n = new_node(st, JCN_STRING);
			n->strval = token;
			break;
		case JSON_TOKEN_NUMBER:
			n = new_node(st, JCN_NUMBER);
			n->strval = token;
			break;
		case JSON_TOKEN_TRUE:
		case JSON_TOKEN_FALSE:
			n = new_node(st, JCN_BOOL);
			n->boolval = (tokentype == JSON_TOKEN_TRUE);
			break;
		default:
			n = new_node(st, JCN_NULL);
			break;
	}
	st->pending_line = 0;
	attach_node(st, n);
	return JSON_SUCCESS;
}

/*
 * Look for \u escapes for characters above U+007F, which we reject.
 *
 * The JSON lexer converts such escapes to the server encoding, which is
 * SQL_ASCII in the postmaster but the database encoding in a backend, so
 * the same file could be refused at startup yet accepted on reload.  The
 * file itself is UTF-8, so the character can always be written directly
 * instead.  Returns the line of the first such escape, or 0 if there is
 * none.  Malformed escapes are left for the lexer to report.
 */
static int
check_unicode_escapes(const char *data)
{
	int			line = 1;
	bool		in_string = false;

	for (const char *p = data; *p != '\0'; p++)
	{
		if (*p == '\n')
			line++;
		else if (!in_string)
		{
			if (*p == '"')
				in_string = true;
		}
		else if (*p == '"')
			in_string = false;
		else if (*p == '\\')
		{
			if (p[1] == 'u')
			{
				int			cp = 0;
				int			i;

				for (i = 2; i <= 5; i++)
				{
					int			c = (unsigned char) p[i];

					if (c >= '0' && c <= '9')
						cp = cp * 16 + (c - '0');
					else if (c >= 'a' && c <= 'f')
						cp = cp * 16 + (c - 'a' + 10);
					else if (c >= 'A' && c <= 'F')
						cp = cp * 16 + (c - 'A' + 10);
					else
						break;
				}
				if (i > 5 && cp > 0x7F)
					return line;
			}
			/* skip the escaped character, so an escaped quote is not a close */
			if (p[1] != '\0')
				p++;
		}
	}
	return 0;
}

/*
 * Parse one file into a tree.  Returns NULL and sets *err_msg on failure,
 * with *err_line the line it is about (0 if none), *err_path the location
 * in the document (NULL if none) and *err_hint a hint (NULL if none).  A
 * file that cannot be opened is reported with *err_msg == NULL and errno
 * set, leaving it to the caller to decide whether that is an error.
 */
static JsonConfNode *
parse_json_file(const char *path, char **err_msg, int *err_line,
				char **err_path, const char **err_hint)
{
	FILE	   *fp;
	StringInfoData buf;
	char		chunk[8192];
	size_t		nread;
	JsonConfParseState st;
	JsonSemAction sem;
	JsonLexContext *lex;
	JsonParseErrorType res;
	int			line;

	*err_msg = NULL;
	*err_line = 0;
	*err_path = NULL;
	*err_hint = NULL;
	fp = AllocateFile(path, "r");
	if (fp == NULL)
		return NULL;
	initStringInfo(&buf);
	while ((nread = fread(chunk, 1, sizeof(chunk), fp)) > 0)
		appendBinaryStringInfo(&buf, chunk, nread);
	if (ferror(fp))
	{
		*err_msg = psprintf(_("could not read file \"%s\": %m"), path);
		FreeFile(fp);
		return NULL;
	}
	FreeFile(fp);

	line = check_unicode_escapes(buf.data);
	if (line != 0)
	{
		*err_msg = pstrdup(_("Unicode escape sequences above U+007F are not supported"));
		*err_line = line;
		*err_hint = _("Write the character in UTF-8 instead.");
		return NULL;
	}

	memset(&st, 0, sizeof(st));
	st.file = path;
	/* need_escapes, or the callbacks would not see the key and value text */
	lex = makeJsonLexContextCstringLen(NULL, buf.data, buf.len, PG_UTF8, true);
	st.lex = lex;

	memset(&sem, 0, sizeof(sem));
	sem.semstate = &st;
	sem.object_start = jc_object_start;
	sem.object_end = jc_container_end;
	sem.array_start = jc_array_start;
	sem.array_end = jc_container_end;
	sem.object_field_start = jc_object_field_start;
	sem.array_element_start = jc_array_element_start;
	sem.scalar = jc_scalar;

	res = pg_parse_json(lex, &sem);
	if (res != JSON_SUCCESS)
	{
		if (st.err)
		{
			*err_msg = st.err;
			*err_path = st.err_path;
		}
		else
			*err_msg = psprintf(_("invalid JSON: %s"),
								json_errdetail(res, lex));
		*err_line = lex->line_number;
		freeJsonLexContext(lex);
		return NULL;
	}
	freeJsonLexContext(lex);

	if (st.root == NULL || st.root->type != JCN_OBJECT)
	{
		*err_msg = pstrdup(_("the top level must be a JSON object"));
		return NULL;
	}
	return st.root;
}


/*
 * Public helpers for walking a parsed document.
 */

/* Is this file name one we treat as a JSON configuration file? */
bool
config_is_json(const char *filename)
{
	const char *dot = strrchr(filename, '.');

	return dot != NULL && strcmp(dot, ".json") == 0;
}

/* Look up a key in an object node; NULL if absent or not an object. */
JsonConfNode *
config_json_get(JsonConfNode *obj, const char *key)
{
	ListCell   *k,
			   *v;

	if (obj == NULL || obj->type != JCN_OBJECT)
		return NULL;
	forboth(k, obj->keys, v, obj->values)
	{
		if (strcmp((char *) lfirst(k), key) == 0)
			return (JsonConfNode *) lfirst(v);
	}
	return NULL;
}

/*
 * Append one key to a path: ".key" for a key that looks like an
 * identifier, else the bracket form with the key as a JSON string, so that
 * a key with a dot in it, as in a custom parameter name, stays one step.
 */
static void
append_path_key(StringInfo buf, const char *key)
{
	bool		plain = (key[0] != '\0' && !isdigit((unsigned char) key[0]));

	for (const char *p = key; *p != '\0' && plain; p++)
	{
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			  (*p >= '0' && *p <= '9') || *p == '_'))
			plain = false;
	}
	if (plain)
		appendStringInfo(buf, ".%s", key);
	else
	{
		appendStringInfoChar(buf, '[');
		escape_json(buf, key);
		appendStringInfoChar(buf, ']');
	}
}

/*
 * Path of a node from the document root, such as "$.hba.children[2]" or
 * "$[\"pg_stat_statements.max\"]".
 */
char *
config_json_path(JsonConfNode *node)
{
	StringInfoData buf;
	List	   *chain = NIL;
	ListCell   *lc;

	for (; node != NULL; node = node->parent)
		chain = lcons(node, chain);
	initStringInfo(&buf);
	appendStringInfoChar(&buf, '$');
	foreach(lc, chain)
	{
		JsonConfNode *n = lfirst(lc);

		if (n->key != NULL)
			append_path_key(&buf, n->key);
		else if (n->index >= 0)
			appendStringInfo(&buf, "[%d]", n->index);
	}
	return buf.data;
}

const char *
config_json_type_name(JsonConfNodeType type)
{
	switch (type)
	{
		case JCN_OBJECT:
			return "object";
		case JCN_ARRAY:
			return "array";
		case JCN_STRING:
			return "string";
		case JCN_NUMBER:
			return "number";
		case JCN_BOOL:
			return "boolean";
		case JCN_NULL:
			return "null";
	}
	return "unknown";
}

/*
 * Error context for a message about a node: its path, file and line.  For
 * use inside ereport().
 */
int
config_json_errcontext(JsonConfNode *node)
{
	return errcontext("node %s, line %d of configuration file \"%s\"",
					  config_json_path(node), node->line, node->file);
}

/*
 * A message about a node with its location appended, for where the message
 * has to stand alone, such as the error column of a view.
 */
char *
config_json_error_text(JsonConfNode *node, const char *msg)
{
	return psprintf(_("%s (node %s, line %d of file \"%s\")"),
					msg, config_json_path(node), node->line, node->file);
}

/* GUC text for a scalar: string as is, number as lexeme, bool as on/off. */
char *
config_json_scalar_text(JsonConfNode *node)
{
	switch (node->type)
	{
		case JCN_STRING:
		case JCN_NUMBER:
			return node->strval;
		case JCN_BOOL:
			return node->boolval ? "on" : "off";
		default:
			return NULL;
	}
}


/*
 * Error reporting.
 *
 * Problems are reported at elevel, and recorded as ConfigVariable error
 * items in the shape ParseConfigFp produces for a broken file, so that
 * the caller can take the first message out of the list.
 */

/*
 * Report a problem with a node at elevel, with the node's location as
 * context, and record it with the location appended.  The message is
 * translated here.
 */
static void json_setting_error(JsonConfNode *node, int elevel,
							   ConfigVariable **head_p,
							   ConfigVariable **tail_p,
							   const char *fmt, ...) pg_attribute_printf(5, 6);

static void
json_setting_error(JsonConfNode *node, int elevel, ConfigVariable **head_p,
				   ConfigVariable **tail_p, const char *fmt, ...)
{
	StringInfoData msg;

	fmt = _(fmt);
	initStringInfo(&msg);
	for (;;)
	{
		va_list		args;
		int			needed;

		va_start(args, fmt);
		needed = appendStringInfoVA(&msg, fmt, args);
		va_end(args);
		if (needed == 0)
			break;
		enlargeStringInfo(&msg, needed);
	}
	ereport(elevel,
			(errcode(ERRCODE_CONFIG_FILE_ERROR),
			 errmsg_internal("%s", msg.data),
			 config_json_errcontext(node)));
	record_config_file_error(config_json_error_text(node, msg.data),
							 node->file, node->line, head_p, tail_p);
}

/*
 * Report a problem that has no node to blame, such as a file that does not
 * parse, at elevel, and record it with the location appended.  "line" may
 * be 0 if there is none.
 */
static void
json_file_error(const char *file, int line, int elevel, const char *msg,
				const char *hint, ConfigVariable **head_p,
				ConfigVariable **tail_p)
{
	char	   *text;

	if (line > 0)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg_internal("%s", msg),
				 hint ? errhint_internal("%s", hint) : 0,
				 errcontext("line %d of configuration file \"%s\"",
							line, file)));
		text = psprintf(_("%s (line %d of file \"%s\")"), msg, line, file);
	}
	else
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg_internal("%s", msg),
				 hint ? errhint_internal("%s", hint) : 0,
				 errcontext("configuration file \"%s\"", file)));
		text = psprintf(_("%s (file \"%s\")"), msg, file);
	}
	record_config_file_error(text, file, line, head_p, tail_p);
}

/*
 * Does this element of a GUC_LIST_QUOTE list need double quotes?  Anything
 * but lowercase ASCII letters, digits and underscores gets them, so that
 * the element survives SplitGUCList unchanged.
 */
static bool
list_element_needs_quotes(const char *elem)
{
	if (elem[0] == '\0')
		return true;
	for (const char *p = elem; *p != '\0'; p++)
	{
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
			  *p == '_'))
			return true;
	}
	return false;
}

/*
 * config_json_setting_text
 *		The text of a setting given as JSON, as postgresql.conf would have it.
 *
 * A scalar is its text (config_json_scalar_text()).  An array becomes a
 * comma-separated list, which is how list parameters are written in
 * postgresql.conf.  For a GUC_LIST_QUOTE parameter the elements are quoted
 * as needed, so that an element may contain any character.  Other lists
 * have no quoting convention, so an element containing a comma would
 * silently turn into two; refuse it instead.
 *
 * Returns NULL for a value of the wrong shape, with *err_msg set to a
 * translated message and *err_at to the offending node.
 */
char *
config_json_setting_text(const char *name, JsonConfNode *node,
						 JsonConfNode **err_at, char **err_msg)
{
	*err_at = NULL;
	*err_msg = NULL;
	if (node->type == JCN_ARRAY)
	{
		StringInfoData buf;
		ListCell   *lc;
		struct config_generic *record;
		bool		quote;

		record = find_option(name, false, true, DEBUG5);
		quote = (record != NULL && (record->flags & GUC_LIST_QUOTE) != 0);

		initStringInfo(&buf);
		foreach(lc, node->values)
		{
			JsonConfNode *el = lfirst(lc);
			char	   *t = config_json_scalar_text(el);

			if (t == NULL)
			{
				*err_at = el;
				*err_msg = psprintf(_("\"%s\" must be a string, number, boolean, or array of these"),
									name);
				return NULL;
			}
			if (buf.len > 0)
				appendStringInfoString(&buf, ", ");
			if (quote && list_element_needs_quotes(t))
			{
				appendStringInfoChar(&buf, '"');
				for (const char *p = t; *p != '\0'; p++)
				{
					if (*p == '"')
						appendStringInfoChar(&buf, '"');
					appendStringInfoChar(&buf, *p);
				}
				appendStringInfoChar(&buf, '"');
			}
			else if (!quote && strchr(t, ',') != NULL)
			{
				*err_at = el;
				*err_msg = psprintf(_("elements of \"%s\" must not contain commas"),
									name);
				return NULL;
			}
			else
				appendStringInfoString(&buf, t);
		}
		return buf.data;
	}
	if (node->type == JCN_OBJECT || node->type == JCN_NULL)
	{
		*err_at = node;
		*err_msg = psprintf(_("\"%s\" must be a string, number, boolean, or array of these"),
							name);
		return NULL;
	}
	return config_json_scalar_text(node);
}

/*
 * Process one include entry: a string, or an object with "file" and an
 * optional "optional" flag.  The file name may carry wildcards in its last
 * component, in which case all matching files are loaded in name order and
 * no match at all is fine.
 */
static bool
process_include_entry(JsonConfNode *entry, const char *calling_file,
					  int depth, int elevel, const char *section,
					  ConfigVariable **head_p, ConfigVariable **tail_p,
					  List **documents_p)
{
	const char *spec;
	bool		optional = false;
	char	   *abs;

	if (entry->type == JCN_STRING)
		spec = entry->strval;
	else if (entry->type == JCN_OBJECT)
	{
		JsonConfNode *f = config_json_get(entry, "file");
		JsonConfNode *o = config_json_get(entry, "optional");
		ListCell   *k,
				   *v;

		forboth(k, entry->keys, v, entry->values)
		{
			const char *key = lfirst(k);

			if (strcmp(key, "file") != 0 && strcmp(key, "optional") != 0)
			{
				json_setting_error(lfirst(v), elevel, head_p, tail_p,
								   "unrecognized key \"%s\"", key);
				return false;
			}
		}
		if (f == NULL)
		{
			json_setting_error(entry, elevel, head_p, tail_p,
							   "missing required key \"%s\"", "file");
			return false;
		}
		if (f->type != JCN_STRING)
		{
			json_setting_error(f, elevel, head_p, tail_p,
							   "\"%s\" must be a string", "file");
			return false;
		}
		if (o != NULL)
		{
			if (o->type != JCN_BOOL)
			{
				json_setting_error(o, elevel, head_p, tail_p,
								   "\"%s\" must be a boolean", "optional");
				return false;
			}
			optional = o->boolval;
		}
		spec = f->strval;
	}
	else
	{
		json_setting_error(entry, elevel, head_p, tail_p,
						   "\"%s\" must be a string, an object, or an array of these",
						   "include");
		return false;
	}

	if (spec[0] == '\0')
	{
		json_setting_error(entry, elevel, head_p, tail_p,
						   "empty include file name");
		return false;
	}

	/*
	 * Look for wildcards in the name as written, not in the resolved path,
	 * since the directories it is resolved against may contain such
	 * characters legitimately.
	 */
	if (strpbrk(spec, "*?") != NULL)
	{
		char	   *dirspec = pstrdup(spec);
		char	   *pattern;
		char	   *dir;
		char	  **names;
		int			nnames;
		char	   *err = NULL;
		struct stat st;
		bool		ok = true;

		pattern = last_dir_separator(dirspec);
		if (pattern == NULL)
		{
			/* bare pattern: relative to the including file's directory */
			pattern = dirspec;
			dirspec = ".";
		}
		else
		{
			*pattern++ = '\0';
			if (dirspec[0] == '\0')
				dirspec = "/";
		}
		if (strpbrk(dirspec, "*?") != NULL)
		{
			json_setting_error(entry, elevel, head_p, tail_p,
							   "wildcards are only allowed in the file name part: \"%s\"",
							   spec);
			return false;
		}
		dir = AbsoluteConfigLocation(dirspec, calling_file);

		/*
		 * An optional pattern whose directory does not exist is the same as
		 * matching nothing.  Check that ourselves, since the directory
		 * scanner would raise an error at elevel, which is fatal at startup.
		 */
		if (optional && stat(dir, &st) != 0 && errno == ENOENT)
		{
			ereport(LOG,
					(errmsg("skipping missing configuration directory \"%s\"",
							dir)));
			return true;
		}

		names = GetConfFilesInDirPattern(dir, calling_file, pattern, elevel,
										 &nnames, &err);
		if (names == NULL)
		{
			/* already reported at elevel; just record it for the view */
			record_config_file_error(err, entry->file, entry->line,
									 head_p, tail_p);
			return false;
		}

		/*
		 * A pattern that matches nothing is treated like a missing file:
		 * silently reading nothing could drop a whole set of rules, such as
		 * a directory of authentication rejects, when its last file goes
		 * away.
		 */
		if (nnames == 0 && !optional)
		{
			json_setting_error(entry, elevel, head_p, tail_p,
							   "include pattern \"%s\" matches no files",
							   spec);
			return false;
		}
		for (int i = 0; i < nnames; i++)
		{
			if (!load_document(names[i], calling_file, true, depth + 1, elevel,
							   section, head_p, tail_p, documents_p, entry))
				ok = false;
		}
		return ok;
	}

	abs = AbsoluteConfigLocation(spec, calling_file);
	return load_document(abs, calling_file, !optional, depth + 1, elevel,
						 section, head_p, tail_p, documents_p, entry);
}

/*
 * Load one document and everything it includes, appending them to
 * *documents_p in that order: the document first, then its includes.
 * The document may hold the given section and an "include" key, nothing
 * else.
 */
static bool
load_document(const char *path, const char *calling_file, bool strict,
			  int depth, int elevel, const char *section,
			  ConfigVariable **head_p, ConfigVariable **tail_p,
			  List **documents_p, JsonConfNode *directive)
{
	JsonConfNode *doc;
	char	   *err = NULL;
	int			err_line;
	char	   *err_path;
	const char *err_hint;
	ListCell   *k,
			   *v;
	bool		ok = true;
	JsonConfNode *include;

	if (depth > CONF_FILE_MAX_DEPTH)
	{
		ereport(elevel,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("could not open configuration file \"%s\": maximum nesting depth exceeded",
						path)));
		record_config_file_error("nesting depth exceeded", calling_file,
								 directive ? directive->line : 0, head_p, tail_p);
		return false;
	}

	/*
	 * An optional file may be missing, but not unreadable.  As for
	 * include_if_exists in pg_hba.conf, any other failure to open it is an
	 * error: silently dropping a file of rules that merely has the wrong
	 * permissions would change what the remaining rules mean.
	 */
	if (!strict)
	{
		FILE	   *fp = AllocateFile(path, "r");

		if (fp == NULL)
		{
			if (errno == ENOENT)
			{
				ereport(LOG,
						(errmsg("skipping missing configuration file \"%s\"",
								path)));
				return true;
			}
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not open configuration file \"%s\": %m",
							path)));
			record_config_file_error(psprintf("could not open file \"%s\"", path),
									 directive ? directive->file : path,
									 directive ? directive->line : 0,
									 head_p, tail_p);
			return false;
		}
		FreeFile(fp);
	}

	/* Only documents can be included; the root is known to be one. */
	if (!config_is_json(path))
	{
		json_setting_error(directive, elevel, head_p, tail_p,
						   "included file \"%s\" is not a JSON file", path);
		return false;
	}

	/*
	 * Reject direct recursion, which a wildcard matching the including file
	 * makes easy to write.  Indirect recursion is caught by the depth limit.
	 */
	if (calling_file != NULL && strcmp(path, calling_file) == 0)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("configuration file recursion in \"%s\"",
						calling_file)));
		record_config_file_error("configuration file recursion", calling_file,
								 directive ? directive->line : 0, head_p, tail_p);
		return false;
	}

	doc = parse_json_file(path, &err, &err_line, &err_path, &err_hint);
	if (doc == NULL)
	{
		if (err == NULL)
		{
			/* could not open it; blame the directive, if there is one */
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not open configuration file \"%s\": %m",
							path)));
			record_config_file_error(psprintf("could not open file \"%s\"", path),
									 directive ? directive->file : path,
									 directive ? directive->line : 0,
									 head_p, tail_p);
			return false;
		}
		if (err_path != NULL)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg_internal("%s", err),
					 errcontext("node %s, line %d of configuration file \"%s\"",
								err_path, err_line, path)));
			record_config_file_error(psprintf(_("%s (node %s, line %d of file \"%s\")"),
											  err, err_path, err_line, path),
									 path, err_line, head_p, tail_p);
		}
		else
			json_file_error(path, err_line, elevel, err, err_hint,
							head_p, tail_p);
		return false;
	}

	*documents_p = lappend(*documents_p, doc);

	forboth(k, doc->keys, v, doc->values)
	{
		const char *key = lfirst(k);

		/* a client authentication document holds one section */
		if (strcmp(key, "include") != 0 && strcmp(key, section) != 0)
		{
			json_setting_error(lfirst(v), elevel, head_p, tail_p,
							   "unrecognized key \"%s\"", key);
			ok = false;
		}
	}

	include = config_json_get(doc, "include");
	if (include != NULL)
	{
		if (include->type == JCN_STRING || include->type == JCN_OBJECT)
		{
			if (!process_include_entry(include, path, depth, elevel, section,
									   head_p, tail_p, documents_p))
				ok = false;
		}
		else if (include->type == JCN_ARRAY)
		{
			ListCell   *lc;

			foreach(lc, include->values)
			{
				if (!process_include_entry(lfirst(lc), path, depth, elevel,
										   section, head_p, tail_p,
										   documents_p))
					ok = false;
			}
		}
		else
		{
			json_setting_error(include, elevel, head_p, tail_p,
							   "\"%s\" must be a string, an object, or an array of these",
							   "include");
			ok = false;
		}
	}
	return ok;
}

/*
 * Load the documents of a client authentication file, the file first and
 * then its includes in load order, for the loader of the section, which
 * picks out its parts.  On failure *err_msg carries the first recorded
 * error text.
 */
bool
config_json_load_section(const char *section, const char *filename,
						 int elevel, List **documents_p, char **err_msg)
{
	ConfigVariable *head = NULL,
			   *tail = NULL;
	char	   *abs = AbsoluteConfigLocation(filename, NULL);
	bool		ok;

	*err_msg = NULL;
	*documents_p = NIL;
	ok = load_document(abs, NULL, true, CONF_FILE_START_DEPTH, elevel,
					   section, &head, &tail, documents_p, NULL);
	if (!ok)
	{
		for (ConfigVariable *item = head; item; item = item->next)
			if (item->errmsg)
			{
				*err_msg = pstrdup(item->errmsg);
				break;
			}
		if (*err_msg == NULL)
			*err_msg = psprintf(_("could not load configuration file \"%s\""),
								filename);
	}
	FreeConfigVariables(head);
	return ok;
}

/*
 * Which JSON document holds a client authentication section, given the
 * section's file parameter: the file itself when its name says it is
 * JSON, or NULL for the legacy format.
 */
const char *
config_json_auth_source(const char *filename)
{
	if (filename != NULL && config_is_json(filename))
		return filename;
	return NULL;
}
