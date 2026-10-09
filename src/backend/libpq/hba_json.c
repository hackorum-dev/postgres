/*-------------------------------------------------------------------------
 *
 * hba_json.c
 *	  Compile the JSON hba tree and ident maps.
 *
 * The "hba" section of a JSON configuration is a tree of nodes.  Each node
 * narrows the match (transport, ssl, gss, databases, users, address) and
 * carries authentication options (auth) that its children inherit.  We
 * walk the tree depth first, children before the node's own rule, and emit
 * one TokenizedAuthLine per node that has an authentication method, with
 * the fields in the classic pg_hba.conf order.  parse_hba_line() then
 * validates them exactly as it does for a legacy file, so the two formats
 * cannot drift apart in what they accept.
 *
 * The "ident" section is flat: "maps" is an object of maps, each an object
 * from system user name to PostgreSQL user name, or to an array of them.
 * Every pair becomes one (map, system user, pg user) line for
 * parse_ident_line().
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/hba_json.c
 *
 * NOTES
 *
 * Inheritance works on an "effective node", built from the parent's
 * effective node by applying the child's own keys:
 *
 * - match fields replace the inherited value wholesale;
 * - "auth" merges key by key, unless the child sets "method", in which
 *   case the child's "auth" replaces the parent's entirely, since the
 *   parent's options belong to a method that is no longer in use.
 *
 * Emission order is what makes the tree readable as "the most specific
 * node wins": a subtree is emitted before the node's own rule, so a child
 * that narrows the match is tried first, and a flat pg_hba.conf is the
 * special case of a tree of depth one.  A node without an effective
 * method emits nothing of its own; it only groups its children.
 *
 * Import nodes splice a legacy file in at their position.  Legacy lines
 * are complete rules and inherit nothing.  The "import" list of the
 * ident section works the same way.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>

#include "common/ip.h"
#include "libpq/hba.h"
#include "miscadmin.h"
#include "utils/conffiles.h"
#include "utils/config_json.h"
#include "utils/memutils.h"

/*
 * Everything the tokenize_*_json() functions allocate while compiling
 * lives here.
 */
static MemoryContext hba_json_context = NULL;

/*
 * Effective (inherited) view of one node.  Match fields point at the JSON
 * value that set them, so that an error can tell an inherited constraint
 * from the node's own; NULL means the default.
 */
typedef struct HbaNodeState
{
	JsonConfNode *transport;	/* NULL = default "tcp" */
	JsonConfNode *ssl;			/* NULL = no constraint */
	JsonConfNode *gss;			/* NULL = no constraint */
	JsonConfNode *databases;	/* NULL = "all" */
	JsonConfNode *users;		/* NULL = "all" */
	JsonConfNode *address;		/* NULL = "all" */
	List	   *auth_keys;		/* char *, "method" included */
	List	   *auth_vals;		/* JsonConfNode * */
} HbaNodeState;

static void walk_node(JsonConfNode *node, const HbaNodeState *parent,
					  List **tok_lines, int elevel);


/*
 * Replace or append a key in a pair of parallel lists.  Insertion order is
 * kept, so inherited keys stay in front of the node's own.
 */
static void
kv_put(List **keys, List **vals, const char *key, void *val)
{
	ListCell   *k,
			   *v;

	forboth(k, *keys, v, *vals)
	{
		if (strcmp(lfirst(k), key) == 0)
		{
			lfirst(v) = val;
			return;
		}
	}
	*keys = lappend(*keys, pstrdup(key));
	*vals = lappend(*vals, val);
}

static void *
kv_get(List *keys, List *vals, const char *key)
{
	ListCell   *k,
			   *v;

	forboth(k, keys, v, vals)
	{
		if (strcmp(lfirst(k), key) == 0)
			return lfirst(v);
	}
	return NULL;
}

/*
 * A line that carries the node's identity, but no fields yet.  raw_line is
 * the node path until the caller knows the rule text.
 */
static TokenizedAuthLine *
new_tok_line(JsonConfNode *node)
{
	TokenizedAuthLine *tok = palloc0_object(TokenizedAuthLine);

	tok->file_name = pstrdup(node->file);
	tok->line_num = node->line;
	tok->node_path = config_json_path(node);
	tok->raw_line = pstrdup(tok->node_path);
	return tok;
}

/*
 * Error context callback for anything reported while working on a node:
 * the location in the document and the file position, as the tokenizer
 * gives the line of a legacy file.
 */
static void
node_error_callback(void *arg)
{
	(void) config_json_errcontext((JsonConfNode *) arg);
}

/*
 * Record a per-node error: report it at elevel, with the location of "at"
 * as context, and add a line with no fields and the message in err_msg, so
 * that the views show it in place.  "at" is the node the message is about,
 * which may be a value inside "node"; the line itself is attributed to
 * "node".  The message is translated here.
 */
static void hba_node_error(JsonConfNode *node, JsonConfNode *at,
						   List **tok_lines, int elevel,
						   const char *fmt, ...) pg_attribute_printf(5, 6);

static void
hba_node_error(JsonConfNode *node, JsonConfNode *at, List **tok_lines,
			   int elevel, const char *fmt, ...)
{
	TokenizedAuthLine *tok = new_tok_line(node);
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
	tok->err_msg = msg.data;
	ereport(elevel,
			(errcode(ERRCODE_CONFIG_FILE_ERROR),
			 errmsg_internal("%s", msg.data),
			 config_json_errcontext(at)));
	*tok_lines = lappend(*tok_lines, tok);
}

/* The keys of an hba node other than an import node. */
static const char *const node_keys[] = {
	"transport", "ssl", "gss", "databases", "users", "address", "auth",
	"children", NULL
};

/* Is the string one of the given keywords?  The list ends with NULL. */
static bool
string_in(const char *s, const char *const *keywords)
{
	for (int i = 0; keywords[i] != NULL; i++)
	{
		if (strcmp(s, keywords[i]) == 0)
			return true;
	}
	return false;
}

/*
 * Check the JSON type, and for the enumerations the value, of one key of a
 * node.  Returns NULL if acceptable, else the message.  Checking the
 * enumerations here rather than at emission reports a bad value on a
 * grouping node even when every child overrides it.
 */
static char *
check_key_type(const char *key, JsonConfNode *val)
{
	static const char *const transports[] = {"local", "tcp", NULL};
	static const char *const constraints[] = {"require", "forbid", NULL};

	if (strcmp(key, "transport") == 0 || strcmp(key, "ssl") == 0 ||
		strcmp(key, "gss") == 0)
	{
		if (val->type != JCN_STRING)
			return psprintf(_("\"%s\" must be a string"), key);
		if (!string_in(val->strval,
					   strcmp(key, "transport") == 0 ? transports : constraints))
			return psprintf(_("invalid value for \"%s\": \"%s\""), key,
							val->strval);
	}
	else if (strcmp(key, "databases") == 0 || strcmp(key, "users") == 0 ||
			 strcmp(key, "address") == 0)
	{
		bool		ok = (val->type == JCN_STRING);

		if (val->type == JCN_ARRAY && val->values != NIL)
		{
			ListCell   *lc;

			ok = true;
			foreach(lc, val->values)
			{
				if (((JsonConfNode *) lfirst(lc))->type != JCN_STRING)
					ok = false;
			}
		}
		if (!ok)
			return psprintf(_("\"%s\" must be a string or a non-empty array of strings"),
							key);
	}
	else if (strcmp(key, "auth") == 0)
	{
		if (val->type != JCN_OBJECT)
			return psprintf(_("\"%s\" must be an object"), key);
	}
	else if (strcmp(key, "children") == 0)
	{
		if (val->type != JCN_ARRAY)
			return psprintf(_("\"%s\" must be an array"), key);
	}
	else
		return psprintf(_("unrecognized key \"%s\""), key);

	return NULL;
}

/*
 * Build the effective state of "node" from its parent's state, applying
 * the inheritance rules described in the file header.  Returns false with
 * *err set and *at pointing at the offending value for structural errors
 * (unrecognized keys, wrong JSON types, bad enumeration values); how the
 * match fields combine is checked later, when the type keyword is
 * synthesized.
 */
static bool
merge_node(const HbaNodeState *parent, JsonConfNode *node,
		   HbaNodeState *out, char **err, JsonConfNode **at)
{
	ListCell   *k,
			   *v;
	JsonConfNode *val,
			   *auth;

	*out = *parent;
	out->auth_keys = list_copy(parent->auth_keys);
	out->auth_vals = list_copy(parent->auth_vals);

	forboth(k, node->keys, v, node->values)
	{
		*at = lfirst(v);
		if ((*err = check_key_type(lfirst(k), *at)) != NULL)
			return false;
	}

	/* match fields replace wholesale */
	if ((val = config_json_get(node, "transport")) != NULL)
		out->transport = val;
	if ((val = config_json_get(node, "ssl")) != NULL)
		out->ssl = val;
	if ((val = config_json_get(node, "gss")) != NULL)
		out->gss = val;
	if ((val = config_json_get(node, "databases")) != NULL)
		out->databases = val;
	if ((val = config_json_get(node, "users")) != NULL)
		out->users = val;
	if ((val = config_json_get(node, "address")) != NULL)
		out->address = val;

	/*
	 * auth merges key by key, unless the method changes: the inherited
	 * options were meant for the old method, so start over.
	 */
	auth = config_json_get(node, "auth");
	if (auth != NULL)
	{
		if (config_json_get(auth, "method") != NULL)
		{
			out->auth_keys = NIL;
			out->auth_vals = NIL;
		}
		forboth(k, auth->keys, v, auth->values)
		{
			JsonConfNode *o = lfirst(v);

			*at = o;
			if (strcmp(lfirst(k), "method") == 0)
			{
				if (o->type != JCN_STRING)
				{
					*err = psprintf(_("\"%s\" must be a string"), "method");
					return false;
				}
			}
			else if (o->type != JCN_STRING && o->type != JCN_NUMBER &&
					 o->type != JCN_BOOL)
			{
				*err = psprintf(_("\"%s\" must be a string, number, or boolean"),
								(char *) lfirst(k));
				return false;
			}
			kv_put(&out->auth_keys, &out->auth_vals, lfirst(k), o);
		}
	}

	return true;
}

/*
 * The token for one JSON string.  The string is an unquoted hba token:
 * "all", "+group" and friends keep their keyword meaning.  Wrapping the
 * string in double quotes marks a literal, as it does in pg_hba.conf, so
 * "\"all\"" is a role named all.
 */
static AuthToken *
make_json_auth_token(const char *s)
{
	size_t		len = strlen(s);

	if (len >= 2 && s[0] == '"' && s[len - 1] == '"')
	{
		char	   *unwrapped = pnstrdup(s + 1, len - 2);
		AuthToken  *tok = make_auth_token(unwrapped, true);

		pfree(unwrapped);
		return tok;
	}
	return make_auth_token(s, false);
}

/*
 * Tokens for one JSON string in a core field: as make_json_auth_token(),
 * except that "@file" pulls in the file's contents, relative to the JSON
 * file.  "at" is the node the string belongs to, which is its value node
 * or, for an object key, the key's value.  On error *err is set.
 */
static List *
append_json_tokens(List *tokens, const char *s, JsonConfNode *at,
				   int elevel, char **err)
{
	if (s[0] == '@' && s[1] != '\0')
	{
		ErrorContextCallback errcallback;

		/* the file's own problems get reported against this node */
		errcallback.callback = node_error_callback;
		errcallback.arg = at;
		errcallback.previous = error_context_stack;
		error_context_stack = &errcallback;
		tokens = hba_expand_auth_file(tokens, at->file, s + 1, elevel, err);
		error_context_stack = errcallback.previous;
		return tokens;
	}
	return lappend(tokens, make_json_auth_token(s));
}

/*
 * Tokens for a databases/users/address value; NULL means the default.
 * Returns NIL with *err set on error.
 */
static List *
string_list_tokens(JsonConfNode *val, int elevel, char **err)
{
	List	   *tokens = NIL;
	ListCell   *lc;

	*err = NULL;
	if (val == NULL)
		return list_make1(make_auth_token("all", false));
	if (val->type == JCN_STRING)
		tokens = append_json_tokens(tokens, val->strval, val, elevel, err);
	else
	{
		foreach(lc, val->values)
		{
			JsonConfNode *elem = lfirst(lc);

			tokens = append_json_tokens(tokens, elem->strval, elem, elevel,
										err);
			if (*err != NULL)
				break;
		}
	}
	return *err == NULL ? tokens : NIL;
}

/*
 * Synthesize the classic type keyword from transport, ssl and gss, whose
 * values merge_node() has checked.  Returns NULL with *err set when the
 * combination has no classic equivalent.  "node" is the node being
 * emitted, so that a constraint set by an ancestor can be named as such.
 */
static const char *
type_keyword(JsonConfNode *node, const HbaNodeState *st, char **err)
{
	const char *sslv = st->ssl ? st->ssl->strval : NULL;
	const char *gssv = st->gss ? st->gss->strval : NULL;

	if (st->transport != NULL && strcmp(st->transport->strval, "local") == 0)
	{
		/*
		 * A Unix socket has no address and no transport encryption.  An
		 * explicitly set constraint is an error even when inherited, since
		 * silently dropping it would widen the match; the default "all" for
		 * the address is fine.  Name the inherited key, as the reader of this
		 * node cannot see it.
		 */
		JsonConfNode *own[3] = {st->address, st->ssl, st->gss};
		const char *names[3] = {"address", "ssl", "gss"};
		const char *inherited = NULL;

		for (int i = 0; i < lengthof(own); i++)
		{
			if (own[i] == NULL)
				continue;
			if (own[i]->parent == node)
			{
				*err = pstrdup(_("\"address\", \"ssl\" and \"gss\" cannot be used with transport \"local\""));
				return NULL;
			}
			if (inherited == NULL)
				inherited = names[i];
		}
		if (inherited != NULL)
		{
			*err = psprintf(_("inherited \"%s\" cannot be used with transport \"local\""),
							inherited);
			return NULL;
		}
		return "local";
	}

	/* the classic keywords constrain one of the two, never both */
	if (sslv != NULL && gssv != NULL)
	{
		*err = pstrdup(_("\"ssl\" and \"gss\" cannot both be constrained"));
		return NULL;
	}
	if (sslv != NULL)
		return strcmp(sslv, "require") == 0 ? "hostssl" : "hostnossl";
	if (gssv != NULL)
		return strcmp(gssv, "require") == 0 ? "hostgssenc" : "hostnogssenc";
	return "host";
}

/*
 * The classic text of a rule built from the given fields, for the "line
 * matched" messages: the JSON node alone does not show what it inherited.
 */
static char *
rule_text(List *fields)
{
	StringInfoData buf;
	ListCell   *f;

	initStringInfo(&buf);
	foreach(f, fields)
	{
		ListCell   *t;
		bool		first = true;

		if (buf.len > 0)
			appendStringInfoChar(&buf, ' ');
		foreach(t, (List *) lfirst(f))
		{
			AuthToken  *tok = lfirst(t);

			if (!first)
				appendStringInfoChar(&buf, ',');
			first = false;
			if (tok->quoted)
				appendStringInfo(&buf, "\"%s\"", tok->string);
			else
				appendStringInfoString(&buf, tok->string);
		}
	}
	return buf.data;
}

/*
 * Is the address token a bare IP address?  parse_hba_line() would take the
 * next field, the method, for its netmask, which a JSON rule cannot have.
 * Host names and the keywords do not parse as numeric addresses.
 */
static bool
is_bare_ip_address(const AuthToken *addr)
{
	struct addrinfo hints;
	struct addrinfo *result = NULL;
	int			ret;

	if (strchr(addr->string, '/') != NULL)
		return false;
	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_NUMERICHOST;
	hints.ai_family = AF_UNSPEC;
	ret = pg_getaddrinfo_all(addr->string, NULL, &hints, &result);
	if (result)
		pg_freeaddrinfo_all(hints.ai_family, result);
	return ret == 0;
}

/*
 * The element of an "address" value an address token came from, for the
 * error context; the value itself for a string, or a token from an @file.
 */
static JsonConfNode *
address_node(JsonConfNode *address, const AuthToken *addr)
{
	ListCell   *lc;

	if (address->type == JCN_ARRAY)
	{
		foreach(lc, address->values)
		{
			JsonConfNode *el = lfirst(lc);

			if (strcmp(el->strval, addr->string) == 0)
				return el;
		}
	}
	return address;
}

/*
 * Emit the node's own rule(s): one per address element, all sharing the
 * node path.  The fields follow the classic order, type, databases, users,
 * address (not for "local"), method, then name=value options, inherited
 * options first.
 */
static void
emit_rule(JsonConfNode *node, const HbaNodeState *st, List **tok_lines,
		  int elevel)
{
	char	   *err = NULL;
	const char *type;
	List	   *databases,
			   *users,
			   *addresses = NIL;
	ListCell   *lc;
	JsonConfNode *method = kv_get(st->auth_keys, st->auth_vals, "method");

	if ((type = type_keyword(node, st, &err)) == NULL)
	{
		hba_node_error(node, node, tok_lines, elevel, "%s", err);
		return;
	}

	/*
	 * The lists are shared by the emitted lines; parse_hba_line() copies the
	 * tokens it keeps, so that is safe.
	 */
	databases = string_list_tokens(st->databases, elevel, &err);
	if (err == NULL)
		users = string_list_tokens(st->users, elevel, &err);
	if (err == NULL)
	{
		if (strcmp(type, "local") == 0)
			addresses = list_make1(NULL);
		else
			addresses = string_list_tokens(st->address, elevel, &err);
	}
	if (err != NULL)
	{
		/* the @file error has been logged with the node's context already */
		TokenizedAuthLine *tok = new_tok_line(node);

		tok->err_msg = err;
		*tok_lines = lappend(*tok_lines, tok);
		return;
	}

	/*
	 * An empty @file yields no tokens.  Like the legacy tokenizer, drop the
	 * field rather than keep an empty one, so that parse_hba_line() rejects
	 * the line rather than the rule silently matching nothing.
	 */
	if (addresses == NIL)
		addresses = list_make1(NULL);

	foreach(lc, addresses)
	{
		AuthToken  *addr = lfirst(lc);
		TokenizedAuthLine *tok = new_tok_line(node);
		List	   *fields;
		ListCell   *k,
				   *v;

		/*
		 * A bare IP address is rejected here, for each element, rather than
		 * leaving parse_hba_line() to complain about the method as a netmask.
		 */
		if (addr != NULL && is_bare_ip_address(addr))
		{
			tok->err_msg = psprintf(_("address \"%s\" must be in CIDR notation"),
									addr->string);
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg_internal("%s", tok->err_msg),
					 errhint("Add \"/32\" for an IPv4 address or \"/128\" for an IPv6 address."),
					 config_json_errcontext(address_node(st->address, addr))));
			*tok_lines = lappend(*tok_lines, tok);
			continue;
		}

		fields = list_make1(list_make1(make_auth_token(type, false)));
		if (databases != NIL)
			fields = lappend(fields, databases);
		if (users != NIL)
			fields = lappend(fields, users);
		if (addr != NULL)
			fields = lappend(fields, list_make1(addr));
		fields = lappend(fields,
						 list_make1(make_auth_token(method->strval, false)));
		forboth(k, st->auth_keys, v, st->auth_vals)
		{
			JsonConfNode *o = lfirst(v);
			const char *text;

			if (strcmp(lfirst(k), "method") == 0)
				continue;

			/*
			 * Boolean options are "1" or "0" in pg_hba.conf, not on/off, so
			 * do not use config_json_scalar_text() here.
			 */
			text = (o->type == JCN_BOOL) ? (o->boolval ? "1" : "0") : o->strval;

			fields = lappend(fields,
							 list_make1(make_auth_token(psprintf("%s=%s",
																 (char *) lfirst(k),
																 text),
														false)));
		}
		tok->fields = fields;
		tok->raw_line = rule_text(fields);
		*tok_lines = lappend(*tok_lines, tok);
	}
}

/*
 * Read a legacy file for an import.  "fileval" is the string node naming
 * it, relative to the JSON file that contains the node.  Returns false
 * when there is nothing to splice in: an optional file that does not
 * exist, with *err left NULL, or a file that could not be opened, with
 * *err set.
 *
 * The file is read at depth 1, as if included from pg_hba.conf: the
 * tokenizer then uses the context tokenize_hba_json() or
 * tokenize_ident_json() created rather than making its own.  An open
 * failure is reported by open_auth_file() itself; the callback adds which
 * node asked for the file.
 */
static bool
read_import_file(JsonConfNode *fileval, bool optional, List **lines,
				 int elevel, char **err)
{
	char	   *path = AbsoluteConfigLocation(fileval->strval, fileval->file);
	FILE	   *file;
	ErrorContextCallback errcallback;

	*lines = NIL;
	*err = NULL;

	/* a missing optional file is not worth a log line */
	if (optional)
	{
		struct stat st_buf;

		if (stat(path, &st_buf) != 0 && errno == ENOENT)
			return false;
	}

	errcallback.callback = node_error_callback;
	errcallback.arg = fileval;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;
	file = open_auth_file(path, elevel, 1, err);
	error_context_stack = errcallback.previous;
	if (file == NULL)
		return false;
	tokenize_auth_file(path, file, lines, elevel, 1);
	free_auth_file(file, 1);
	return true;
}

/*
 * Splice a legacy file in at the import node's position.  The legacy lines
 * are complete rules and inherit nothing.
 */
static void
import_node(JsonConfNode *node, List **tok_lines, int elevel)
{
	JsonConfNode *imp = config_json_get(node, "import");
	JsonConfNode *opt = config_json_get(node, "optional");
	ListCell   *k,
			   *v;
	List	   *lines;
	char	   *err;

	/* an import node is a leaf with exactly these two keys */
	forboth(k, node->keys, v, node->values)
	{
		const char *key = lfirst(k);

		if (strcmp(key, "import") == 0 || strcmp(key, "optional") == 0)
			continue;
		if (string_in(key, node_keys))
			hba_node_error(node, lfirst(v), tok_lines, elevel,
						   "import node cannot have \"%s\"", key);
		else
			hba_node_error(node, lfirst(v), tok_lines, elevel,
						   "unrecognized key \"%s\"", key);
		return;
	}
	if (imp->type != JCN_STRING)
	{
		hba_node_error(node, imp, tok_lines, elevel,
					   "\"%s\" must be a string", "import");
		return;
	}
	if (opt != NULL && opt->type != JCN_BOOL)
	{
		hba_node_error(node, opt, tok_lines, elevel,
					   "\"%s\" must be a boolean", "optional");
		return;
	}
	if (!read_import_file(imp, opt != NULL && opt->boolval, &lines, elevel,
						  &err))
	{
		if (err != NULL)
		{
			TokenizedAuthLine *tok = new_tok_line(node);

			tok->err_msg = err;
			*tok_lines = lappend(*tok_lines, tok);
		}
		return;
	}

	*tok_lines = list_concat(*tok_lines, lines);
}

/*
 * Compile one node and its subtree, appending lines in evaluation order.
 */
static void
walk_node(JsonConfNode *node, const HbaNodeState *parent, List **tok_lines,
		  int elevel)
{
	HbaNodeState st;
	char	   *err = NULL;
	JsonConfNode *at = NULL;
	JsonConfNode *children;
	bool		has_children;

	/* the tree is user input, and this recurses with it */
	check_stack_depth();

	if (node->type != JCN_OBJECT)
	{
		hba_node_error(node, node, tok_lines, elevel,
					   "hba node must be an object, not %s",
					   config_json_type_name(node->type));
		return;
	}

	if (config_json_get(node, "import") != NULL)
	{
		import_node(node, tok_lines, elevel);
		return;
	}

	if (!merge_node(parent, node, &st, &err, &at))
	{
		hba_node_error(node, at, tok_lines, elevel, "%s", err);
		return;
	}

	/* children first: the more specific match must be tried first */
	children = config_json_get(node, "children");
	has_children = (children != NULL && children->values != NIL);
	if (has_children)
	{
		ListCell   *lc;

		foreach(lc, children->values)
			walk_node(lfirst(lc), &st, tok_lines, elevel);
	}

	if (kv_get(st.auth_keys, st.auth_vals, "method") != NULL)
		emit_rule(node, &st, tok_lines, elevel);
	else if (!has_children)
	{
		/*
		 * A grouping node without a method is fine, but a leaf without one
		 * emits nothing and is almost certainly a mistake.  An empty children
		 * array is a leaf.
		 */
		hba_node_error(node, node, tok_lines, elevel,
					   "leaf node has no authentication method");
	}
}

/*
 * section_shape_error
 *		Report a value of the wrong shape in a section, which leaves nothing
 *		to compile for that section.
 *
 * Logs at elevel with the node's location as context, sets *file_err to
 * the message with the location appended, and returns false for the
 * caller to pass up.  The message is translated here, and allocated in
 * the current memory context.
 */
bool
section_shape_error(JsonConfNode *node, int elevel, char **file_err,
					const char *fmt, ...)
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
	if (file_err)
		*file_err = config_json_error_text(node, msg.data);
	return false;
}

/*
 * import_auth_lines
 *		Append the lines of the legacy files an "import" value names.
 *
 * The value is a string, or an array whose elements are strings or
 * {"file": ..., "optional": true} objects, like the "include" list of a
 * document; paths are relative to the JSON file.  A malformed value is a
 * section shape error.  A file that cannot be opened becomes a line with
 * err_msg, as a broken include does in a legacy file, unless it is
 * optional and does not exist.  The tokenizer context must exist.
 */
bool
import_auth_lines(JsonConfNode *imports, List **tok_lines, int elevel,
				  char **file_err)
{
	List	   *entries;
	ListCell   *lc;

	if (imports->type == JCN_ARRAY)
		entries = imports->values;
	else
		entries = list_make1(imports);

	foreach(lc, entries)
	{
		JsonConfNode *entry = lfirst(lc);
		JsonConfNode *fileval = entry;
		JsonConfNode *optval = NULL;
		List	   *lines;
		char	   *err;

		if (entry->type == JCN_OBJECT)
		{
			ListCell   *k,
					   *v;

			forboth(k, entry->keys, v, entry->values)
			{
				const char *key = lfirst(k);

				if (strcmp(key, "file") != 0 && strcmp(key, "optional") != 0)
					return section_shape_error(lfirst(v), elevel, file_err,
											   "unrecognized key \"%s\"", key);
			}
			fileval = config_json_get(entry, "file");
			optval = config_json_get(entry, "optional");
			if (fileval == NULL)
				return section_shape_error(entry, elevel, file_err,
										   "missing required key \"%s\"",
										   "file");
			if (fileval->type != JCN_STRING)
				return section_shape_error(fileval, elevel, file_err,
										   "\"%s\" must be a string", "file");
		}
		else if (entry->type != JCN_STRING)
			return section_shape_error(entry, elevel, file_err,
									   "\"%s\" must be a string, an object, or an array of these",
									   "import");
		if (optval != NULL && optval->type != JCN_BOOL)
			return section_shape_error(optval, elevel, file_err,
									   "\"%s\" must be a boolean",
									   "optional");

		if (!read_import_file(fileval, optval != NULL && optval->boolval,
							  &lines, elevel, &err))
		{
			if (err != NULL)
			{
				TokenizedAuthLine *tok = new_tok_line(fileval);

				tok->err_msg = err;
				*tok_lines = lappend(*tok_lines, tok);
			}
			continue;
		}
		*tok_lines = list_concat(*tok_lines, lines);
	}
	return true;
}

/*
 * The "hba.children" of every document, compiled in order, so that an
 * included file's rules follow the including file's.
 */
static bool
compile_hba_documents(List *documents, List **tok_lines, int elevel,
					  char **file_err)
{
	ListCell   *lc;
	HbaNodeState root = {0};

	foreach(lc, documents)
	{
		JsonConfNode *doc = lfirst(lc);
		JsonConfNode *hba = config_json_get(doc, "hba");
		JsonConfNode *children;
		ListCell   *c;

		if (hba == NULL)
			continue;
		if (hba->type != JCN_OBJECT ||
			(children = config_json_get(hba, "children")) == NULL ||
			children->type != JCN_ARRAY || list_length(hba->keys) != 1)
			return section_shape_error(hba, elevel, file_err,
									   "\"hba\" must be an object with a \"children\" array");
		foreach(c, children->values)
			walk_node(lfirst(c), &root, tok_lines, elevel);
	}
	return true;
}

/*
 * One (map, system user, pg user) line.  "pg" is the value node, or one
 * element of an array value; the line is attributed to it, so that the
 * view points at the mapping rather than at the map.  Both names may be
 * "@file" references, as in pg_ident.conf; parse_ident_line() rejects a
 * file that holds more than one name, as it does for a legacy line.
 */
static void
emit_mapping(const char *mapname, const char *sysname, JsonConfNode *pg,
			 List **tok_lines, int elevel)
{
	TokenizedAuthLine *tok;
	List	   *systoks,
			   *pgtoks = NIL;
	char	   *err = NULL;

	if (pg->type != JCN_STRING)
	{
		hba_node_error(pg, pg, tok_lines, elevel,
					   "\"%s\" must be a string or a non-empty array of strings",
					   sysname);
		return;
	}
	tok = new_tok_line(pg);
	systoks = append_json_tokens(NIL, sysname, pg, elevel, &err);
	if (err == NULL)
		pgtoks = append_json_tokens(NIL, pg->strval, pg, elevel, &err);
	if (err != NULL)
	{
		/* the @file error has been logged with the node's context already */
		tok->err_msg = err;
		*tok_lines = lappend(*tok_lines, tok);
		return;
	}

	/*
	 * An empty @file yields no tokens; like the legacy tokenizer, drop the
	 * field rather than keep an empty one, and parse_ident_line() reports the
	 * missing entry.
	 */
	tok->fields = list_make1(list_make1(make_auth_token(mapname, false)));
	if (systoks != NIL)
		tok->fields = lappend(tok->fields, systoks);
	if (pgtoks != NIL)
		tok->fields = lappend(tok->fields, pgtoks);
	tok->raw_line = rule_text(tok->fields);
	*tok_lines = lappend(*tok_lines, tok);
}

/*
 * The "ident" section of every document: the imports first, then every
 * key of "maps", so that maps of the same name across documents simply
 * follow each other, as repeated map names do in pg_ident.conf.
 */
static bool
compile_ident_documents(List *documents, List **tok_lines, int elevel,
						char **file_err)
{
	ListCell   *lc;

	foreach(lc, documents)
	{
		JsonConfNode *doc = lfirst(lc);
		JsonConfNode *ident = config_json_get(doc, "ident");
		JsonConfNode *imports,
				   *maps;
		ListCell   *k,
				   *v;

		if (ident == NULL)
			continue;
		if (ident->type != JCN_OBJECT)
			return section_shape_error(ident, elevel, file_err,
									   "\"%s\" must be an object", "ident");
		forboth(k, ident->keys, v, ident->values)
		{
			const char *key = lfirst(k);

			if (strcmp(key, "import") != 0 && strcmp(key, "maps") != 0)
				return section_shape_error(lfirst(v), elevel, file_err,
										   "unrecognized key \"%s\"", key);
		}

		imports = config_json_get(ident, "import");
		if (imports != NULL &&
			!import_auth_lines(imports, tok_lines, elevel, file_err))
			return false;

		maps = config_json_get(ident, "maps");
		if (maps == NULL)
			continue;
		if (maps->type != JCN_OBJECT)
			return section_shape_error(maps, elevel, file_err,
									   "\"%s\" must be an object", "maps");
		forboth(k, maps->keys, v, maps->values)
		{
			const char *mapname = lfirst(k);
			JsonConfNode *map = lfirst(v);
			ListCell   *sk,
					   *sv;

			if (map->type != JCN_OBJECT)
				return section_shape_error(map, elevel, file_err,
										   "map \"%s\" must be an object",
										   mapname);
			forboth(sk, map->keys, sv, map->values)
			{
				const char *sysname = lfirst(sk);
				JsonConfNode *val = lfirst(sv);

				if (val->type == JCN_ARRAY && val->values != NIL)
				{
					ListCell   *pl;

					foreach(pl, val->values)
						emit_mapping(mapname, sysname, lfirst(pl), tok_lines,
									 elevel);
				}
				else
					emit_mapping(mapname, sysname, val, tok_lines, elevel);
			}
		}
	}
	return true;
}

/*
 * Set up the contexts a section compiler needs, and switch to the one its
 * output goes to.  Returns the caller's context; free_json_section_tokens()
 * undoes the rest.
 */
static MemoryContext
json_section_begin(void)
{
	hba_json_context = AllocSetContextCreate(CurrentMemoryContext,
											 "hba json context",
											 ALLOCSET_START_SMALL_SIZES);
	hba_tokenize_context_create();
	return MemoryContextSwitchTo(hba_json_context);
}

/*
 * Common frame of tokenize_hba_json() and tokenize_ident_json(): set up
 * the contexts, run the compiler, and move a file-level message out of
 * the context the caller is about to free.
 */
static bool
tokenize_json_section(bool (*compile) (List *, List **, int, char **),
					  List *documents, List **tok_lines, int elevel,
					  char **file_err)
{
	MemoryContext oldcxt;
	bool		ok;

	*tok_lines = NIL;
	if (file_err)
		*file_err = NULL;

	oldcxt = json_section_begin();
	ok = compile(documents, tok_lines, elevel, file_err);
	MemoryContextSwitchTo(oldcxt);

	if (!ok && file_err && *file_err != NULL)
		*file_err = pstrdup(*file_err);
	return ok;
}

/*
 * tokenize_hba_json
 *		Compile the "hba" sections of the given documents into lines.
 *
 * "documents" is the list from config_json_load_section(); the
 * "hba.children" of every document are compiled in order, so an included
 * file's rules follow the including file's.  Returns false with *file_err
 * set for file-level problems (an "hba" value of the wrong shape); per-node
 * problems are reported through err_msg on the lines, as the tokenizer
 * does for a legacy file.  No "hba" section anywhere yields an empty list,
 * which load_hba() rejects as a file without entries.
 *
 * The lines and everything they point to are allocated in a context of
 * their own, released by free_json_section_tokens(); callers copy what
 * they keep, as parse_hba_line() does.
 */
bool
tokenize_hba_json(List *documents, List **tok_lines, int elevel,
				  char **file_err)
{
	return tokenize_json_section(compile_hba_documents, documents, tok_lines,
								 elevel, file_err);
}

/*
 * tokenize_ident_json
 *		Compile the "ident" sections of the given documents into lines.
 *
 * As tokenize_hba_json(), for user name maps: each document's imports
 * come first, then one line per mapping of each map in "maps".  Maps with
 * the same name in several documents concatenate, which falls out of the
 * order.  A value that is neither a string nor a non-empty array of
 * strings is a per-mapping error; the shape of "ident", "maps" and the
 * maps themselves is a file-level error.  No "ident" section anywhere
 * yields an empty list, which load_ident() accepts.
 */
bool
tokenize_ident_json(List *documents, List **tok_lines, int elevel,
					char **file_err)
{
	return tokenize_json_section(compile_ident_documents, documents,
								 tok_lines, elevel, file_err);
}

/*
 * Release what tokenize_hba_json() or tokenize_ident_json() allocated,
 * including the tokenizer context the imports used.
 */
void
free_json_section_tokens(void)
{
	hba_tokenize_context_delete();
	if (hba_json_context != NULL)
		MemoryContextDelete(hba_json_context);
	hba_json_context = NULL;
}
