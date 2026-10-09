/*-------------------------------------------------------------------------
 *
 * be-secure-common.c
 *
 * common implementation-independent SSL support code
 *
 * While be-secure.c contains the interfaces that the rest of the
 * communications code calls, this file contains support routines that are
 * used by the library-specific implementations such as be-secure-openssl.c.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/libpq/be-secure-common.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "common/percentrepl.h"
#include "common/string.h"
#include "libpq/libpq.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/config_json.h"
#include "utils/guc.h"
#include "utils/memutils.h"

/*
 * Run ssl_passphrase_command
 *
 * prompt will be substituted for %p.  is_server_start determines the loglevel
 * of error messages from executing the command, the loglevel for failures in
 * param substitution will be ERROR regardless of is_server_start.  The actual
 * command used depends on the configuration for the current host.
 *
 * The result will be put in buffer buf, which is of size size.  The return
 * value is the length of the actual result.
 */
int
run_ssl_passphrase_command(const char *cmd, const char *prompt,
						   bool is_server_start, char *buf, int size)
{
	int			loglevel = is_server_start ? ERROR : LOG;
	char	   *command;
	FILE	   *fh;
	int			pclose_rc;
	size_t		len = 0;

	Assert(prompt);
	Assert(size > 0);
	buf[0] = '\0';

	command = replace_percent_placeholders(cmd, "ssl_passphrase_command", "p", prompt);

	fh = OpenPipeStream(command, "r");
	if (fh == NULL)
	{
		ereport(loglevel,
				(errcode_for_file_access(),
				 errmsg("could not execute command \"%s\": %m",
						command)));
		goto error;
	}

	if (!fgets(buf, size, fh))
	{
		if (ferror(fh))
		{
			explicit_bzero(buf, size);
			ereport(loglevel,
					(errcode_for_file_access(),
					 errmsg("could not read from command \"%s\": %m",
							command)));
			goto error;
		}
	}

	pclose_rc = ClosePipeStream(fh);
	if (pclose_rc == -1)
	{
		explicit_bzero(buf, size);
		ereport(loglevel,
				(errcode_for_file_access(),
				 errmsg("could not close pipe to external command: %m")));
		goto error;
	}
	else if (pclose_rc != 0)
	{
		char	   *reason;

		explicit_bzero(buf, size);
		reason = wait_result_to_str(pclose_rc);
		ereport(loglevel,
				(errcode_for_file_access(),
				 errmsg("command \"%s\" failed",
						command),
				 errdetail_internal("%s", reason)));
		pfree(reason);
		goto error;
	}

	/* strip trailing newline and carriage return */
	len = pg_strip_crlf(buf);

error:
	pfree(command);
	return len;
}


/*
 * Check permissions for SSL key files.
 */
bool
check_ssl_key_file_permissions(const char *ssl_key_file, bool isServerStart)
{
	int			loglevel = isServerStart ? FATAL : LOG;
	struct stat buf;

	if (stat(ssl_key_file, &buf) != 0)
	{
		ereport(loglevel,
				(errcode_for_file_access(),
				 errmsg("could not access private key file \"%s\": %m",
						ssl_key_file)));
		return false;
	}

	/* Key file must be a regular file */
	if (!S_ISREG(buf.st_mode))
	{
		ereport(loglevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("private key file \"%s\" is not a regular file",
						ssl_key_file)));
		return false;
	}

	/*
	 * Refuse to load key files owned by users other than us or root, and
	 * require no public access to the key file.  If the file is owned by us,
	 * require mode 0600 or less.  If owned by root, require 0640 or less to
	 * allow read access through either our gid or a supplementary gid that
	 * allows us to read system-wide certificates.
	 *
	 * Note that roughly similar checks are performed in
	 * src/interfaces/libpq/fe-secure-openssl.c so any changes here may need
	 * to be made there as well.  The environment is different though; this
	 * code can assume that we're not running as root.
	 *
	 * Ideally we would do similar permissions checks on Windows, but it is
	 * not clear how that would work since Unix-style permissions may not be
	 * available.
	 */
#if !defined(WIN32) && !defined(__CYGWIN__)
	if (buf.st_uid != geteuid() && buf.st_uid != 0)
	{
		ereport(loglevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("private key file \"%s\" must be owned by the database user or root",
						ssl_key_file)));
		return false;
	}

	if ((buf.st_uid == geteuid() && buf.st_mode & (S_IRWXG | S_IRWXO)) ||
		(buf.st_uid == 0 && buf.st_mode & (S_IWGRP | S_IXGRP | S_IRWXO)))
	{
		ereport(loglevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("private key file \"%s\" has group or world access",
						ssl_key_file),
				 errdetail("File must have permissions u=rw (0600) or less if owned by the database user, or permissions u=rw,g=r (0640) or less if owned by root.")));
		return false;
	}
#endif

	return true;
}

/*
 * Report a problem with a hosts line at elevel, and record it in the line,
 * so that the view can show it in place.  The message is translated here.
 */
static void hosts_line_error(TokenizedAuthLine *tok_line, int elevel,
							 const char *fmt, ...) pg_attribute_printf(3, 4);

static void
hosts_line_error(TokenizedAuthLine *tok_line, int elevel, const char *fmt, ...)
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
	tok_line->err_msg = msg.data;
	ereport(elevel,
			errcode(ERRCODE_CONFIG_FILE_ERROR),
			errmsg_internal("%s", msg.data),
			errcontext("line %d of configuration file \"%s\"",
					   tok_line->line_num, tok_line->file_name));
}

/*
 * parse_hosts_line
 *
 * Parses a loaded line from the pg_hosts.conf configuration and pulls out the
 * hostname, certificate, key and CA parts in order to build an SNI config in
 * the TLS backend. Validation of the parsed values is left for the TLS backend
 * to implement.
 *
 * Returns NULL on error, with tok_line->err_msg set to the message.  The
 * result is allocated in the current memory context.
 */
HostsLine *
parse_hosts_line(TokenizedAuthLine *tok_line, int elevel)
{
	HostsLine  *parsedline;
	List	   *tokens;
	ListCell   *field;
	AuthToken  *token;

	parsedline = palloc0_object(HostsLine);
	parsedline->sourcefile = pstrdup(tok_line->file_name);
	parsedline->linenumber = tok_line->line_num;
	parsedline->rawline = pstrdup(tok_line->raw_line);
	parsedline->hostnames = NIL;

	/* Initialize optional fields */
	parsedline->ssl_passphrase_cmd = NULL;
	parsedline->ssl_passphrase_reload = false;

	/* Hostname */
	field = list_head(tok_line->fields);
	tokens = lfirst(field);
	foreach_ptr(AuthToken, hostname, tokens)
	{
		if ((tokens->length > 1) &&
			(strcmp(hostname->string, "*") == 0 || strcmp(hostname->string, "/no_sni/") == 0))
		{
			hosts_line_error(tok_line, elevel,
							 "default and non-SNI entries cannot be mixed with other entries");
			return NULL;
		}

		parsedline->hostnames = lappend(parsedline->hostnames, pstrdup(hostname->string));
	}

	/* SSL Certificate (Required) */
	field = lnext(tok_line->fields, field);
	if (!field)
	{
		hosts_line_error(tok_line, elevel,
						 "missing entry at end of line");
		return NULL;
	}
	tokens = lfirst(field);
	if (tokens->length > 1)
	{
		hosts_line_error(tok_line, elevel,
						 "multiple values specified for SSL certificate");
		return NULL;
	}
	token = linitial(tokens);
	parsedline->ssl_cert = pstrdup(token->string);

	/* SSL key (Required) */
	field = lnext(tok_line->fields, field);
	if (!field)
	{
		hosts_line_error(tok_line, elevel,
						 "missing entry at end of line");
		return NULL;
	}
	tokens = lfirst(field);
	if (tokens->length > 1)
	{
		hosts_line_error(tok_line, elevel,
						 "multiple values specified for SSL key");
		return NULL;
	}
	token = linitial(tokens);
	parsedline->ssl_key = pstrdup(token->string);

	/* SSL CA (optional) */
	field = lnext(tok_line->fields, field);
	if (!field)
		return parsedline;
	tokens = lfirst(field);
	if (tokens->length > 1)
	{
		hosts_line_error(tok_line, elevel,
						 "multiple values specified for SSL CA");
		return NULL;
	}
	token = linitial(tokens);
	parsedline->ssl_ca = pstrdup(token->string);

	/* SSL Passphrase Command (optional) */
	field = lnext(tok_line->fields, field);
	if (field)
	{
		tokens = lfirst(field);
		if (tokens->length > 1)
		{
			hosts_line_error(tok_line, elevel,
							 "multiple values specified for SSL passphrase command");
			return NULL;
		}
		token = linitial(tokens);
		parsedline->ssl_passphrase_cmd = pstrdup(token->string);

		/*
		 * SSL Passphrase Command support reload (optional). This field is
		 * only supported if there was a passphrase command parsed first, so
		 * nest it under the previous token.
		 */
		field = lnext(tok_line->fields, field);
		if (field)
		{
			tokens = lfirst(field);
			token = linitial(tokens);

			/*
			 * There should be no more tokens after this, if there are break
			 * parsing and report error to avoid silently accepting incorrect
			 * config.
			 */
			if (lnext(tok_line->fields, field))
			{
				hosts_line_error(tok_line, elevel,
								 "extra fields at end of line");
				return NULL;
			}

			if (tokens->length > 1 || !parse_bool(token->string, &parsedline->ssl_passphrase_reload))
			{
				hosts_line_error(tok_line, elevel,
								 "incorrect syntax for boolean value SSL_passphrase_cmd_reload");
				return NULL;
			}
		}
	}

	return parsedline;
}

/*
 * load_hosts_json
 *
 * The JSON counterpart of load_hosts(): the entries come from the "hosts"
 * sections of the JSON documents, those of hosts_file or of the server
 * configuration file.  No section anywhere is the
 * same as a missing file, a section without entries the same as an empty
 * one.  The documents and the compiler's scratch data are temporary; the
 * entries themselves are allocated in the caller's context, as the ones
 * from a legacy file are.
 */
static HostsFileLoadResult
load_hosts_json(List **hosts, char **err_msg)
{
	MemoryContext oldcxt = CurrentMemoryContext;
	MemoryContext jsoncxt;
	List	   *documents;
	List	   *entries;
	ListCell   *lc;
	char	   *err;
	bool		found;
	bool		ok = true;

	jsoncxt = AllocSetContextCreate(CurrentMemoryContext,
									"hosts json load context",
									ALLOCSET_START_SMALL_SIZES);
	MemoryContextSwitchTo(jsoncxt);
	if (!config_json_load_section("hosts", HostsFileName, LOG, &documents,
								  &err))
	{
		/* error already logged */
		MemoryContextSwitchTo(oldcxt);
		if (err_msg)
			*err_msg = pstrdup(err);
		MemoryContextDelete(jsoncxt);
		return HOSTSFILE_LOAD_FAILED;
	}
	MemoryContextSwitchTo(oldcxt);

	entries = parse_hosts_json(documents, LOG, &found, &err);
	MemoryContextDelete(jsoncxt);

	/*
	 * A broken section must never pass for a missing one, as that would
	 * silently fall back to the ssl_* parameters; so the error comes first.
	 */
	if (err != NULL)
	{
		if (err_msg)
			*err_msg = err;
		return HOSTSFILE_LOAD_FAILED;
	}
	if (!found)
		return HOSTSFILE_MISSING;

	/* entries with errors have been reported; any of them fails the load */
	foreach(lc, entries)
	{
		HostsLine  *entry = lfirst(lc);

		if (entry->err_msg != NULL)
			ok = false;
		else
			*hosts = lappend(*hosts, entry);
	}

	if (!ok)
	{
		if (err_msg)
			*err_msg = pstrdup(_("\"hosts\" section contains errors"));
		return HOSTSFILE_LOAD_FAILED;
	}

	if (*hosts == NIL)
		return HOSTSFILE_EMPTY;

	return HOSTSFILE_LOAD_OK;
}

/*
 * load_hosts
 *
 * Reads and parses the pg_hosts.conf configuration file and passes back a List
 * of HostsLine elements containing the parsed lines, or NIL in case of an empty
 * file.  The list is returned in the hosts parameter. The function will return
 * a HostsFileLoadResult value detailing the result of the operation.  When
 * the hosts configuration failed to load, the err_msg variable may have more
 * information in case it was passed as non-NULL.
 *
 * When hosts_file names a JSON file, or the server configuration is a JSON
 * document, the entries come from its "hosts" sections instead; see
 * load_hosts_json().
 */
HostsFileLoadResult
load_hosts(List **hosts, char **err_msg)
{
	FILE	   *file;
	ListCell   *line;
	List	   *hosts_lines = NIL;
	List	   *parsed_lines = NIL;
	HostsLine  *newline;
	bool		ok = true;

	/*
	 * If we cannot return results then error out immediately. This implies
	 * API misuse or a similar kind of programmer error.
	 */
	if (!hosts)
	{
		if (err_msg)
			*err_msg = psprintf("cannot load config from \"%s\", return variable missing",
								HostsFileName);
		return HOSTSFILE_LOAD_FAILED;
	}
	*hosts = NIL;

	if (config_json_auth_source(HostsFileName) != NULL)
		return load_hosts_json(hosts, err_msg);

	/*
	 * This is not an auth file per se, but it is using the same file format
	 * as the pg_hba and pg_ident files and thus the same code infrastructure.
	 * A future TODO might be to rename the supporting code with a more
	 * generic name?
	 */
	file = open_auth_file(HostsFileName, LOG, 0, err_msg);
	if (file == NULL)
	{
		if (errno == ENOENT)
			return HOSTSFILE_MISSING;

		return HOSTSFILE_LOAD_FAILED;
	}

	tokenize_auth_file(HostsFileName, file, &hosts_lines, LOG, 0);

	foreach(line, hosts_lines)
	{
		TokenizedAuthLine *tok_line = (TokenizedAuthLine *) lfirst(line);

		/*
		 * Mark processing as not-ok in case lines are found with errors in
		 * tokenization (.err_msg is set) or during parsing.
		 */
		if ((tok_line->err_msg != NULL) ||
			((newline = parse_hosts_line(tok_line, LOG)) == NULL))
		{
			ok = false;
			continue;
		}

		parsed_lines = lappend(parsed_lines, newline);
	}

	/* Free memory from tokenizer */
	free_auth_file(file, 0);
	*hosts = parsed_lines;

	if (!ok)
	{
		if (err_msg)
			*err_msg = psprintf("loading config from \"%s\" failed due to parsing error",
								HostsFileName);
		return HOSTSFILE_LOAD_FAILED;
	}

	if (parsed_lines == NIL)
		return HOSTSFILE_EMPTY;

	return HOSTSFILE_LOAD_OK;
}

/*
 * hosts_source_name
 *
 * Name of the file the hosts entries come from, for messages: the server
 * configuration file when its "hosts" section replaces hosts_file.
 */
const char *
hosts_source_name(void)
{
	const char *source = config_json_auth_source(HostsFileName);

	return source != NULL ? source : HostsFileName;
}

/*
 * apply_hosts_settings
 *
 * Apply the settings of the hosts entry chosen for a connection.  This runs
 * in the backend during the TLS handshake, before client authentication.
 * The settings were validated when the entry was loaded, so a failure here
 * is unexpected and only logged.  An hba rule applies its settings later and
 * from a higher source, so it overrides these on conflict.
 */
void
apply_hosts_settings(HostsLine *host)
{
	ListCell   *k,
			   *v;

	forboth(k, host->settings_keys, v, host->settings_vals)
		(void) set_config_option(lfirst(k), lfirst(v), PGC_HOSTS, PGC_S_HOSTS,
								 GUC_ACTION_SET, true, LOG, false);
}
