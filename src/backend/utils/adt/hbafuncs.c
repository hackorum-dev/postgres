/*-------------------------------------------------------------------------
 *
 * hbafuncs.c
 *	  Support functions for SQL views of authentication files.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/hbafuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>

#include "access/htup_details.h"
#include "catalog/objectaddress.h"
#include "common/ip.h"
#include "funcapi.h"
#include "libpq/hba.h"
#include "libpq/libpq.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/config_json.h"
#include "utils/guc.h"
#include "utils/tuplestore.h"


static ArrayType *get_hba_options(HbaLine *hba);
static void fill_hba_line(Tuplestorestate *tuple_store, TupleDesc tupdesc,
						  int rule_number, char *filename, int lineno,
						  const char *node_path, HbaLine *hba,
						  const char *err_msg);
static void fill_hba_view(Tuplestorestate *tuple_store, TupleDesc tupdesc);
static void fill_ident_line(Tuplestorestate *tuple_store, TupleDesc tupdesc,
							int map_number, char *filename, int lineno,
							IdentLine *ident, const char *err_msg);
static void fill_ident_view(Tuplestorestate *tuple_store, TupleDesc tupdesc);
static void fill_hosts_entry(Tuplestorestate *tuple_store, TupleDesc tupdesc,
							 int entry_number, const char *filename,
							 int lineno, const char *hostname,
							 HostsLine *host, const char *err_msg);
static void fill_hosts_view(Tuplestorestate *tuple_store, TupleDesc tupdesc);


/*
 * This macro specifies the maximum number of authentication options
 * that are possible with any given authentication method that is supported.
 * Currently LDAP supports 12, and there are 3 that are not dependent on
 * the auth method here.  It may not actually be possible to set all of them
 * at the same time, but we'll set the macro value high enough to be
 * conservative and avoid warnings from static analysis tools.
 */
#define MAX_HBA_OPTIONS 15

/*
 * Create a text array listing the options specified in the HBA line.
 * Return NULL if no options are specified.
 */
static ArrayType *
get_hba_options(HbaLine *hba)
{
	int			noptions;
	Datum		options[MAX_HBA_OPTIONS];

	noptions = 0;

	if (hba->auth_method == uaGSS || hba->auth_method == uaSSPI)
	{
		if (hba->include_realm)
			options[noptions++] =
				CStringGetTextDatum("include_realm=true");

		if (hba->krb_realm)
			options[noptions++] =
				CStringGetTextDatum(psprintf("krb_realm=%s", hba->krb_realm));
	}

	if (hba->usermap)
		options[noptions++] =
			CStringGetTextDatum(psprintf("map=%s", hba->usermap));

	if (hba->clientcert != clientCertOff)
		options[noptions++] =
			CStringGetTextDatum(psprintf("clientcert=%s", (hba->clientcert == clientCertCA) ? "verify-ca" : "verify-full"));

	if (hba->pamservice)
		options[noptions++] =
			CStringGetTextDatum(psprintf("pamservice=%s", hba->pamservice));

	if (hba->auth_method == uaLDAP)
	{
		if (hba->ldapserver)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapserver=%s", hba->ldapserver));

		if (hba->ldapport)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapport=%d", hba->ldapport));

		if (hba->ldapscheme)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapscheme=%s", hba->ldapscheme));

		if (hba->ldaptls)
			options[noptions++] =
				CStringGetTextDatum("ldaptls=true");

		if (hba->ldapprefix)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapprefix=%s", hba->ldapprefix));

		if (hba->ldapsuffix)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapsuffix=%s", hba->ldapsuffix));

		if (hba->ldapbasedn)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapbasedn=%s", hba->ldapbasedn));

		if (hba->ldapbinddn)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapbinddn=%s", hba->ldapbinddn));

		if (hba->ldapbindpasswd)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapbindpasswd=%s",
											 hba->ldapbindpasswd));

		if (hba->ldapsearchattribute)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapsearchattribute=%s",
											 hba->ldapsearchattribute));

		if (hba->ldapsearchfilter)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapsearchfilter=%s",
											 hba->ldapsearchfilter));

		if (hba->ldapscope)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapscope=%d", hba->ldapscope));
	}

	if (hba->auth_method == uaOAuth)
	{
		if (hba->oauth_issuer)
			options[noptions++] =
				CStringGetTextDatum(psprintf("issuer=%s", hba->oauth_issuer));

		if (hba->oauth_scope)
			options[noptions++] =
				CStringGetTextDatum(psprintf("scope=%s", hba->oauth_scope));

		if (hba->oauth_validator)
			options[noptions++] =
				CStringGetTextDatum(psprintf("validator=%s", hba->oauth_validator));

		if (hba->oauth_skip_usermap)
			options[noptions++] =
				CStringGetTextDatum(psprintf("delegate_ident_mapping=true"));
	}

	/* If you add more options, consider increasing MAX_HBA_OPTIONS. */
	Assert(noptions <= MAX_HBA_OPTIONS);

	if (noptions > 0)
		return construct_array_builtin(options, noptions, TEXTOID);
	else
		return NULL;
}

/* Number of columns in pg_hba_file_rules view */
#define NUM_PG_HBA_FILE_RULES_ATTS	 12

/*
 * fill_hba_line
 *		Build one row of pg_hba_file_rules view, add it to tuplestore.
 *
 * tuple_store: where to store data
 * tupdesc: tuple descriptor for the view
 * rule_number: unique identifier among all valid rules
 * filename: configuration file name (must always be valid)
 * lineno: line number of configuration file (must always be valid)
 * node_path: JSON node the rule was compiled from (NULL for a legacy line)
 * hba: parsed line data (can be NULL, in which case err_msg should be set)
 * err_msg: error message (NULL if none)
 *
 * Note: leaks memory, but we don't care since this is run in a short-lived
 * memory context.
 */
static void
fill_hba_line(Tuplestorestate *tuple_store, TupleDesc tupdesc,
			  int rule_number, char *filename, int lineno,
			  const char *node_path, HbaLine *hba, const char *err_msg)
{
	Datum		values[NUM_PG_HBA_FILE_RULES_ATTS];
	bool		nulls[NUM_PG_HBA_FILE_RULES_ATTS];
	char		buffer[NI_MAXHOST];
	HeapTuple	tuple;
	int			index;
	ListCell   *lc;
	const char *typestr;
	const char *addrstr;
	const char *maskstr;
	ArrayType  *options;

	Assert(tupdesc->natts == NUM_PG_HBA_FILE_RULES_ATTS);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	index = 0;

	/* rule_number, nothing on error */
	if (err_msg)
		nulls[index++] = true;
	else
		values[index++] = Int32GetDatum(rule_number);

	/* file_name */
	values[index++] = CStringGetTextDatum(filename);

	/* line_number */
	values[index++] = Int32GetDatum(lineno);

	if (hba != NULL)
	{
		/* type */
		/* Avoid a default: case so compiler will warn about missing cases */
		typestr = NULL;
		switch (hba->conntype)
		{
			case ctLocal:
				typestr = "local";
				break;
			case ctHost:
				typestr = "host";
				break;
			case ctHostSSL:
				typestr = "hostssl";
				break;
			case ctHostNoSSL:
				typestr = "hostnossl";
				break;
			case ctHostGSS:
				typestr = "hostgssenc";
				break;
			case ctHostNoGSS:
				typestr = "hostnogssenc";
				break;
		}
		if (typestr)
			values[index++] = CStringGetTextDatum(typestr);
		else
			nulls[index++] = true;

		/* database */
		if (hba->databases)
		{
			/*
			 * Flatten AuthToken list to string list.  It might seem that we
			 * should re-quote any quoted tokens, but that has been rejected
			 * on the grounds that it makes it harder to compare the array
			 * elements to other system catalogs.  That makes entries like
			 * "all" or "samerole" formally ambiguous ... but users who name
			 * databases/roles that way are inflicting their own pain.
			 */
			List	   *names = NIL;

			foreach(lc, hba->databases)
			{
				AuthToken  *tok = lfirst(lc);

				names = lappend(names, tok->string);
			}
			values[index++] = PointerGetDatum(strlist_to_textarray(names));
		}
		else
			nulls[index++] = true;

		/* user */
		if (hba->roles)
		{
			/* Flatten AuthToken list to string list; see comment above */
			List	   *roles = NIL;

			foreach(lc, hba->roles)
			{
				AuthToken  *tok = lfirst(lc);

				roles = lappend(roles, tok->string);
			}
			values[index++] = PointerGetDatum(strlist_to_textarray(roles));
		}
		else
			nulls[index++] = true;

		/* address and netmask */
		/* Avoid a default: case so compiler will warn about missing cases */
		addrstr = maskstr = NULL;
		switch (hba->ip_cmp_method)
		{
			case ipCmpMask:
				if (hba->hostname)
				{
					addrstr = hba->hostname;
				}
				else
				{
					/*
					 * Note: if pg_getnameinfo_all fails, it'll set buffer to
					 * "???", which we want to return.
					 */
					if (hba->addrlen > 0)
					{
						if (pg_getnameinfo_all(&hba->addr, hba->addrlen,
											   buffer, sizeof(buffer),
											   NULL, 0,
											   NI_NUMERICHOST) == 0)
							clean_ipv6_addr(hba->addr.ss_family, buffer);
						addrstr = pstrdup(buffer);
					}
					if (hba->masklen > 0)
					{
						if (pg_getnameinfo_all(&hba->mask, hba->masklen,
											   buffer, sizeof(buffer),
											   NULL, 0,
											   NI_NUMERICHOST) == 0)
							clean_ipv6_addr(hba->mask.ss_family, buffer);
						maskstr = pstrdup(buffer);
					}
				}
				break;
			case ipCmpAll:
				addrstr = "all";
				break;
			case ipCmpSameHost:
				addrstr = "samehost";
				break;
			case ipCmpSameNet:
				addrstr = "samenet";
				break;
		}
		if (addrstr)
			values[index++] = CStringGetTextDatum(addrstr);
		else
			nulls[index++] = true;
		if (maskstr)
			values[index++] = CStringGetTextDatum(maskstr);
		else
			nulls[index++] = true;

		/* auth_method */
		values[index++] = CStringGetTextDatum(hba_authname(hba->auth_method));

		/* options */
		options = get_hba_options(hba);
		if (options)
			values[index++] = PointerGetDatum(options);
		else
			nulls[index++] = true;
	}
	else
	{
		/* no parsing result, so set the fields derived from it to nulls */
		memset(&nulls[3], true, (NUM_PG_HBA_FILE_RULES_ATTS - 5) * sizeof(bool));
		index = NUM_PG_HBA_FILE_RULES_ATTS - 2;
	}

	/* node_path, known even when the rule did not parse */
	if (node_path)
		values[index++] = CStringGetTextDatum(node_path);
	else
		nulls[index++] = true;

	/* error */
	Assert(index == NUM_PG_HBA_FILE_RULES_ATTS - 1);
	if (err_msg)
		values[index] = CStringGetTextDatum(err_msg);
	else
		nulls[index] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	tuplestore_puttuple(tuple_store, tuple);
}

/*
 * fill_hba_view
 *		Read the hba configuration and fill the tuplestore with view records.
 *
 * The rules come from hba_file, in either format.
 */
static void
fill_hba_view(Tuplestorestate *tuple_store, TupleDesc tupdesc)
{
	bool		use_json = (config_json_auth_source(HbaFileName) != NULL);
	FILE	   *file = NULL;
	List	   *hba_lines = NIL;
	ListCell   *line;
	int			rule_number = 0;
	MemoryContext hbacxt;
	MemoryContext oldcxt;

	/*
	 * In the unlikely event that we can't open or load the file, we throw an
	 * error, rather than trying to report it via some sort of view entry.
	 * (Most other error conditions should result in a message in a view
	 * entry.)
	 */
	if (use_json)
	{
		List	   *documents;
		char	   *file_err;

		/*
		 * Only tokenize_hba_json() creates the contexts that
		 * free_json_section_tokens() releases; after a failed load of the
		 * documents there is nothing of ours to free, and the statics may
		 * still point at memory an earlier, aborted call has lost to the
		 * per-query reset.
		 */
		if (!config_json_load_section("hba", HbaFileName, DEBUG3,
									  &documents, &file_err))
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not load configuration file \"%s\"",
							HbaFileName),
					 errdetail_internal("%s", file_err)));
		if (!tokenize_hba_json(documents, &hba_lines, DEBUG3, &file_err))
		{
			free_json_section_tokens();
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not load configuration file \"%s\"",
							HbaFileName),
					 errdetail_internal("%s", file_err)));
		}
	}
	else
	{
		file = open_auth_file(HbaFileName, ERROR, 0, NULL);

		tokenize_auth_file(HbaFileName, file, &hba_lines, DEBUG3, 0);
	}

	/* Now parse all the lines */
	hbacxt = AllocSetContextCreate(CurrentMemoryContext,
								   "hba parser context",
								   ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(hbacxt);
	foreach(line, hba_lines)
	{
		TokenizedAuthLine *tok_line = (TokenizedAuthLine *) lfirst(line);
		HbaLine    *hbaline = NULL;

		/* don't parse lines that already have errors */
		if (tok_line->err_msg == NULL)
			hbaline = parse_hba_line(tok_line, DEBUG3);

		/* No error, set a new rule number */
		if (tok_line->err_msg == NULL)
			rule_number++;

		fill_hba_line(tuple_store, tupdesc, rule_number,
					  tok_line->file_name, tok_line->line_num,
					  tok_line->node_path, hbaline, tok_line->err_msg);
	}

	/* Free tokenizer memory */
	if (use_json)
		free_json_section_tokens();
	else
		free_auth_file(file, 0);
	/* Free parse_hba_line memory */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(hbacxt);
}

/*
 * pg_hba_file_rules
 *
 * SQL-accessible set-returning function to return all the entries in the
 * pg_hba.conf file.
 */
Datum
pg_hba_file_rules(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi;

	/*
	 * Build tuplestore to hold the result rows.  We must use the Materialize
	 * mode to be safe against HBA file changes while the cursor is open. It's
	 * also more efficient than having to look up our current position in the
	 * parsed list every time.
	 */
	InitMaterializedSRF(fcinfo, 0);

	/* Fill the tuplestore */
	rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	fill_hba_view(rsi->setResult, rsi->setDesc);

	PG_RETURN_NULL();
}

/* Number of columns in pg_ident_file_mappings view */
#define NUM_PG_IDENT_FILE_MAPPINGS_ATTS	 7

/*
 * fill_ident_line: build one row of pg_ident_file_mappings view, add it to
 * tuplestore
 *
 * tuple_store: where to store data
 * tupdesc: tuple descriptor for the view
 * map_number: unique identifier among all valid maps
 * filename: configuration file name (must always be valid)
 * lineno: line number of configuration file (must always be valid)
 * ident: parsed line data (can be NULL, in which case err_msg should be set)
 * err_msg: error message (NULL if none)
 *
 * Note: leaks memory, but we don't care since this is run in a short-lived
 * memory context.
 */
static void
fill_ident_line(Tuplestorestate *tuple_store, TupleDesc tupdesc,
				int map_number, char *filename, int lineno, IdentLine *ident,
				const char *err_msg)
{
	Datum		values[NUM_PG_IDENT_FILE_MAPPINGS_ATTS];
	bool		nulls[NUM_PG_IDENT_FILE_MAPPINGS_ATTS];
	HeapTuple	tuple;
	int			index;

	Assert(tupdesc->natts == NUM_PG_IDENT_FILE_MAPPINGS_ATTS);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	index = 0;

	/* map_number, nothing on error */
	if (err_msg)
		nulls[index++] = true;
	else
		values[index++] = Int32GetDatum(map_number);

	/* file_name */
	values[index++] = CStringGetTextDatum(filename);

	/* line_number */
	values[index++] = Int32GetDatum(lineno);

	if (ident != NULL)
	{
		values[index++] = CStringGetTextDatum(ident->usermap);
		values[index++] = CStringGetTextDatum(ident->system_user->string);
		values[index++] = CStringGetTextDatum(ident->pg_user->string);
	}
	else
	{
		/* no parsing result, so set relevant fields to nulls */
		memset(&nulls[3], true, (NUM_PG_IDENT_FILE_MAPPINGS_ATTS - 4) * sizeof(bool));
	}

	/* error */
	if (err_msg)
		values[NUM_PG_IDENT_FILE_MAPPINGS_ATTS - 1] = CStringGetTextDatum(err_msg);
	else
		nulls[NUM_PG_IDENT_FILE_MAPPINGS_ATTS - 1] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	tuplestore_puttuple(tuple_store, tuple);
}

/*
 * fill_ident_view
 *		Read the user name maps and fill the tuplestore with view records.
 *
 * The maps come from ident_file, in either format.
 */
static void
fill_ident_view(Tuplestorestate *tuple_store, TupleDesc tupdesc)
{
	bool		use_json = (config_json_auth_source(IdentFileName) != NULL);
	FILE	   *file = NULL;
	List	   *ident_lines = NIL;
	ListCell   *line;
	int			map_number = 0;
	MemoryContext identcxt;
	MemoryContext oldcxt;

	/*
	 * In the unlikely event that we can't open or load the file, we throw an
	 * error, rather than trying to report it via some sort of view entry.
	 * (Most other error conditions should result in a message in a view
	 * entry.)
	 */
	if (use_json)
	{
		List	   *documents;
		char	   *file_err;

		/* as in fill_hba_view(), free only what tokenizing created */
		if (!config_json_load_section("ident", IdentFileName, DEBUG3,
									  &documents, &file_err))
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not load configuration file \"%s\"",
							IdentFileName),
					 errdetail_internal("%s", file_err)));
		if (!tokenize_ident_json(documents, &ident_lines, DEBUG3, &file_err))
		{
			free_json_section_tokens();
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not load configuration file \"%s\"",
							IdentFileName),
					 errdetail_internal("%s", file_err)));
		}
	}
	else
	{
		file = open_auth_file(IdentFileName, ERROR, 0, NULL);

		tokenize_auth_file(IdentFileName, file, &ident_lines, DEBUG3, 0);
	}

	/* Now parse all the lines */
	identcxt = AllocSetContextCreate(CurrentMemoryContext,
									 "ident parser context",
									 ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(identcxt);
	foreach(line, ident_lines)
	{
		TokenizedAuthLine *tok_line = (TokenizedAuthLine *) lfirst(line);
		IdentLine  *identline = NULL;

		/* don't parse lines that already have errors */
		if (tok_line->err_msg == NULL)
			identline = parse_ident_line(tok_line, DEBUG3);

		/* no error, set a new mapping number */
		if (tok_line->err_msg == NULL)
			map_number++;

		fill_ident_line(tuple_store, tupdesc, map_number,
						tok_line->file_name, tok_line->line_num,
						identline, tok_line->err_msg);
	}

	/* Free tokenizer memory */
	if (use_json)
		free_json_section_tokens();
	else
		free_auth_file(file, 0);
	/* Free parse_ident_line memory */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(identcxt);
}

/*
 * SQL-accessible SRF to return all the entries in the pg_ident.conf file.
 */
Datum
pg_ident_file_mappings(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi;

	/*
	 * Build tuplestore to hold the result rows.  We must use the Materialize
	 * mode to be safe against HBA file changes while the cursor is open. It's
	 * also more efficient than having to look up our current position in the
	 * parsed list every time.
	 */
	InitMaterializedSRF(fcinfo, 0);

	/* Fill the tuplestore */
	rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	fill_ident_view(rsi->setResult, rsi->setDesc);

	PG_RETURN_NULL();
}

/* Number of columns in pg_hosts_file_entries view */
#define NUM_PG_HOSTS_FILE_ENTRIES_ATTS	10

/*
 * fill_hosts_entry
 *		Build one row of pg_hosts_file_entries view, add it to tuplestore.
 *
 * tuple_store: where to store data
 * tupdesc: tuple descriptor for the view
 * entry_number: unique identifier among all valid entries
 * filename: configuration file name (must always be valid)
 * lineno: line number of configuration file (must always be valid)
 * hostname: the host name this row is for (NULL if none could be read)
 * host: parsed entry data (can be NULL, in which case err_msg should be set)
 * err_msg: error message (NULL if none)
 *
 * A legacy line with several host names gives one row per name, all with
 * the same entry_number.  With an error, only the host name is shown.
 *
 * Note: leaks memory, but we don't care since this is run in a short-lived
 * memory context.
 */
static void
fill_hosts_entry(Tuplestorestate *tuple_store, TupleDesc tupdesc,
				 int entry_number, const char *filename, int lineno,
				 const char *hostname, HostsLine *host, const char *err_msg)
{
	Datum		values[NUM_PG_HOSTS_FILE_ENTRIES_ATTS];
	bool		nulls[NUM_PG_HOSTS_FILE_ENTRIES_ATTS];
	HeapTuple	tuple;
	int			index;

	Assert(tupdesc->natts == NUM_PG_HOSTS_FILE_ENTRIES_ATTS);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	index = 0;

	/* entry_number, nothing on error */
	if (err_msg)
		nulls[index++] = true;
	else
		values[index++] = Int32GetDatum(entry_number);

	/* file_name */
	values[index++] = CStringGetTextDatum(filename);

	/* line_number */
	values[index++] = Int32GetDatum(lineno);

	/* hostname */
	if (hostname)
		values[index++] = CStringGetTextDatum(hostname);
	else
		nulls[index++] = true;

	if (host != NULL && err_msg == NULL)
	{
		values[index++] = CStringGetTextDatum(host->ssl_cert);
		values[index++] = CStringGetTextDatum(host->ssl_key);
		if (host->ssl_ca)
			values[index++] = CStringGetTextDatum(host->ssl_ca);
		else
			nulls[index++] = true;
		if (host->ssl_passphrase_cmd)
			values[index++] = CStringGetTextDatum(host->ssl_passphrase_cmd);
		else
			nulls[index++] = true;
		values[index++] = BoolGetDatum(host->ssl_passphrase_reload);
	}
	else
	{
		/* no parsing result, so set the fields derived from it to nulls */
		memset(&nulls[index], true,
			   (NUM_PG_HOSTS_FILE_ENTRIES_ATTS - index - 1) * sizeof(bool));
		index = NUM_PG_HOSTS_FILE_ENTRIES_ATTS - 1;
	}

	/* error */
	Assert(index == NUM_PG_HOSTS_FILE_ENTRIES_ATTS - 1);
	if (err_msg)
		values[index] = CStringGetTextDatum(err_msg);
	else
		nulls[index] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	tuplestore_puttuple(tuple_store, tuple);
}

/*
 * One row per host name of an entry; a problem that left no host name,
 * such as an import file that could not be read, gets one row without.
 */
static void
fill_hosts_entry_rows(Tuplestorestate *tuple_store, TupleDesc tupdesc,
					  int entry_number, const char *filename, int lineno,
					  List *hostnames, HostsLine *host, const char *err_msg)
{
	ListCell   *lc;

	if (hostnames == NIL)
	{
		fill_hosts_entry(tuple_store, tupdesc, entry_number, filename, lineno,
						 NULL, host, err_msg);
		return;
	}
	foreach(lc, hostnames)
		fill_hosts_entry(tuple_store, tupdesc, entry_number, filename, lineno,
						 lfirst(lc), host, err_msg);
}

/*
 * fill_hosts_view
 *		Read the SNI hosts configuration and fill the tuplestore with view
 *		records.
 *
 * The entries come from hosts_file, in either format.
 */
static void
fill_hosts_view(Tuplestorestate *tuple_store, TupleDesc tupdesc)
{
	int			entry_number = 0;
	ListCell   *lc;
	MemoryContext hostscxt;
	MemoryContext oldcxt;

	/* Everything parsed here is short-lived */
	hostscxt = AllocSetContextCreate(CurrentMemoryContext,
									 "hosts parser context",
									 ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(hostscxt);

	/*
	 * As for the other views, a file that cannot be loaded at all is an error
	 * rather than a row; the problems of individual entries are rows.
	 */
	if (config_json_auth_source(HostsFileName) != NULL)
	{
		List	   *documents;
		List	   *entries;
		char	   *file_err;
		bool		found;

		if (!config_json_load_section("hosts", HostsFileName, DEBUG3,
									  &documents, &file_err))
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not load configuration file \"%s\"",
							HostsFileName),
					 errdetail_internal("%s", file_err)));
		entries = parse_hosts_json(documents, DEBUG3, &found, &file_err);
		if (file_err != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not load configuration file \"%s\"",
							HostsFileName),
					 errdetail_internal("%s", file_err)));

		foreach(lc, entries)
		{
			HostsLine  *host = lfirst(lc);

			/* No error, set a new entry number */
			if (host->err_msg == NULL)
				entry_number++;

			fill_hosts_entry_rows(tuple_store, tupdesc, entry_number,
								  host->sourcefile, host->linenumber,
								  host->hostnames, host, host->err_msg);
		}
	}
	else
	{
		struct stat st;
		FILE	   *file;
		List	   *hosts_lines = NIL;

		/* a missing file is an empty one, as load_hosts() treats it */
		if (stat(HostsFileName, &st) != 0 && errno == ENOENT)
		{
			MemoryContextSwitchTo(oldcxt);
			MemoryContextDelete(hostscxt);
			return;
		}

		file = open_auth_file(HostsFileName, ERROR, 0, NULL);
		tokenize_auth_file(HostsFileName, file, &hosts_lines, DEBUG3, 0);

		foreach(lc, hosts_lines)
		{
			TokenizedAuthLine *tok_line = (TokenizedAuthLine *) lfirst(lc);
			HostsLine  *host = NULL;
			List	   *hostnames = NIL;

			/* don't parse lines that already have errors */
			if (tok_line->err_msg == NULL)
				host = parse_hosts_line(tok_line, DEBUG3);

			/* No error, set a new entry number */
			if (tok_line->err_msg == NULL)
				entry_number++;

			/*
			 * For a line that did not parse, show the host names it did have,
			 * from the first field.
			 */
			if (host != NULL)
				hostnames = host->hostnames;
			else if (tok_line->fields != NIL)
			{
				ListCell   *t;

				foreach(t, (List *) linitial(tok_line->fields))
					hostnames = lappend(hostnames,
										((AuthToken *) lfirst(t))->string);
			}

			fill_hosts_entry_rows(tuple_store, tupdesc, entry_number,
								  tok_line->file_name, tok_line->line_num,
								  hostnames, host, tok_line->err_msg);
		}

		/* Free tokenizer memory */
		free_auth_file(file, 0);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(hostscxt);
}

/*
 * SQL-accessible SRF to return all the entries in the pg_hosts.conf file.
 */
Datum
pg_hosts_file_entries(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi;

	/*
	 * Build tuplestore to hold the result rows.  We must use the Materialize
	 * mode to be safe against file changes while the cursor is open.
	 */
	InitMaterializedSRF(fcinfo, 0);

	/* Fill the tuplestore */
	rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	fill_hosts_view(rsi->setResult, rsi->setDesc);

	PG_RETURN_NULL();
}
