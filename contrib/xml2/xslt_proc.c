/*
 * contrib/xml2/xslt_proc.c
 *
 * XSLT processing functions (requiring libxslt)
 *
 * John Gray, for Torchbox 2003-04-01
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/xml.h"
#include "utils/array.h"
#include "utils/memutils.h"
#include "mb/pg_wchar.h"

#ifdef USE_LIBXSLT

/* libxml includes */

#include <libxml/xpath.h>
#include <libxml/tree.h>
#include <libxml/xmlmemory.h>

/* libxslt includes */

#include <libxslt/xslt.h>
#include <libxslt/xsltInternals.h>
#include <libxslt/security.h>
#include <libxslt/transform.h>
#include <libxslt/xsltutils.h>
#endif							/* USE_LIBXSLT */


#ifdef USE_LIBXSLT

/* declarations to come from xpath.c */
extern PgXmlErrorContext *pgxml_parser_init(PgXmlStrictness strictness);

/* local defs */
static xmltype *xslt_process_internal(xmltype *doct, xmltype *ssheet, const char **params);
static const char **parse_params(text *paramstr);
#endif							/* USE_LIBXSLT */

/*
 * FIXME: This cannot easily be exposed in xml.h.
 * Perhaps there should be an xml-internal.h?
 */
xmlDocPtr	xml_parse(text *data, XmlOptionType xmloption_arg,
					  bool preserve_whitespace, int encoding,
					  XmlOptionType *parsed_xmloptiontype, xmlNodePtr *parsed_nodes,
					  Node *escontext);

PG_FUNCTION_INFO_V1(xslt_process);

Datum
xslt_process(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXSLT

	text	   *doct = PG_GETARG_TEXT_PP(0);
	text	   *ssheet = PG_GETARG_TEXT_PP(1);
	const char **params = NULL;
	text	   *result;

	if (fcinfo->nargs == 3)
	{
		text	   *paramstr = PG_GETARG_TEXT_PP(2);

		params = parse_params(paramstr);
	}

	result = xslt_process_internal(doct, ssheet, params);

	PG_RETURN_TEXT_P(result);

#else							/* !USE_LIBXSLT */

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("xslt_process() is not available without libxslt")));
	PG_RETURN_NULL();

#endif							/* USE_LIBXSLT */
}

PG_FUNCTION_INFO_V1(xslt_process_xmltype);

Datum
xslt_process_xmltype(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXSLT

	xmltype    *doct = PG_GETARG_XML_P(0);
	xmltype    *ssheet = PG_GETARG_XML_P(1);
	const char **params = NULL;
	xmltype    *result;

	/*
	 * Parameters are key-value pairs. The values are XPath expressions, so
	 * strings will have to be escaped with single or double quotes. Even
	 * `xsltproc --stringparam` does nothing else than adding single or double
	 * quotes and fails if the value contains both.
	 */
	if (fcinfo->nargs == 3)
	{
		ArrayType  *paramarray = PG_GETARG_ARRAYTYPE_P(2);
		Datum	   *arr_datums;
		bool	   *arr_nulls;
		int			arr_count;
		int			i,
					j;

		deconstruct_array_builtin(paramarray, TEXTOID, &arr_datums, &arr_nulls, &arr_count);

		if ((arr_count % 2) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_ELEMENT_ERROR),
					 errmsg("number of stylesheet parameters (%d) must be a multiple of 2",
							arr_count)));

		params = palloc_array(const char *, arr_count + 1);

		for (i = 0, j = 0; i < arr_count; i++)
		{
			char	   *cstr;

			if (arr_nulls[i])
				continue;

			cstr = TextDatumGetCString(arr_datums[i]);
			params[j++] = (char *) pg_do_encoding_conversion((unsigned char *) cstr,
															 strlen(cstr),
															 GetDatabaseEncoding(),
															 PG_UTF8);
		}
		params[j] = NULL;
	}

	result = xslt_process_internal(doct, ssheet, params);

	PG_RETURN_XML_P(result);

#else							/* !USE_LIBXSLT */

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("xslt_process() is not available without libxslt")));
	PG_RETURN_NULL();

#endif							/* USE_LIBXSLT */
}

#ifdef USE_LIBXSLT

static xmltype *
xslt_process_internal(xmltype *doct, xmltype *ssheet, const char **params)
{
	text	   *volatile result;
	PgXmlErrorContext *xmlerrcxt;
	volatile xsltStylesheetPtr stylesheet = NULL;
	volatile xmlDocPtr doctree = NULL;
	volatile xmlDocPtr restree = NULL;
	volatile xsltSecurityPrefsPtr xslt_sec_prefs = NULL;
	volatile xsltTransformContextPtr xslt_ctxt = NULL;
	volatile int resstat = -1;
	xmlChar    *volatile resstr = NULL;

	/* the previous libxslt error context */
	xmlGenericErrorFunc saved_errfunc;
	void	   *saved_errcxt;

	/* Setup parser */
	xmlerrcxt = pgxml_parser_init(PG_XML_STRICTNESS_ALL);

	/*
	 * Save the previous libxslt error context.
	 */
	saved_errfunc = xsltGenericError;
	saved_errcxt = xsltGenericErrorContext;
	xsltSetGenericErrorFunc(xmlerrcxt, xml_generic_error_handler);

	PG_TRY();
	{
		xmlDocPtr	ssdoc;
		bool		xslt_sec_prefs_error;
		int			reslen = 0;

		/*
		 * Parse document.
		 */
		doctree = xml_parse(doct, XMLOPTION_DOCUMENT, true,
							GetDatabaseEncoding(), NULL, NULL, NULL);

		if (doctree == NULL || pg_xml_error_occurred(xmlerrcxt))
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_XML_DOCUMENT,
						"error parsing XML document");

		/* Same for stylesheet */
		ssdoc = xml_parse(ssheet, XMLOPTION_DOCUMENT, true,
						  GetDatabaseEncoding(), NULL, NULL, NULL);

		if (ssdoc == NULL || pg_xml_error_occurred(xmlerrcxt))
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_XML_DOCUMENT,
						"error parsing stylesheet as XML document");

		/* After this call we need not free ssdoc separately */
		stylesheet = xsltParseStylesheetDoc(ssdoc);

		if (stylesheet == NULL || pg_xml_error_occurred(xmlerrcxt))
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_ARGUMENT_FOR_XQUERY,
						"failed to parse stylesheet");

		xslt_ctxt = xsltNewTransformContext(stylesheet, doctree);

		xslt_sec_prefs_error = false;
		if ((xslt_sec_prefs = xsltNewSecurityPrefs()) == NULL)
			xslt_sec_prefs_error = true;

		if (xsltSetSecurityPrefs(xslt_sec_prefs, XSLT_SECPREF_READ_FILE,
								 xsltSecurityForbid) != 0)
			xslt_sec_prefs_error = true;
		if (xsltSetSecurityPrefs(xslt_sec_prefs, XSLT_SECPREF_WRITE_FILE,
								 xsltSecurityForbid) != 0)
			xslt_sec_prefs_error = true;
		if (xsltSetSecurityPrefs(xslt_sec_prefs, XSLT_SECPREF_CREATE_DIRECTORY,
								 xsltSecurityForbid) != 0)
			xslt_sec_prefs_error = true;
		if (xsltSetSecurityPrefs(xslt_sec_prefs, XSLT_SECPREF_READ_NETWORK,
								 xsltSecurityForbid) != 0)
			xslt_sec_prefs_error = true;
		if (xsltSetSecurityPrefs(xslt_sec_prefs, XSLT_SECPREF_WRITE_NETWORK,
								 xsltSecurityForbid) != 0)
			xslt_sec_prefs_error = true;
		if (xsltSetCtxtSecurityPrefs(xslt_sec_prefs, xslt_ctxt) != 0)
			xslt_sec_prefs_error = true;

		if (xslt_sec_prefs_error)
			ereport(ERROR,
					(errmsg("could not set libxslt security preferences")));

		restree = xsltApplyStylesheetUser(stylesheet, doctree, params,
										  NULL, NULL, xslt_ctxt);

		if (restree == NULL || pg_xml_error_occurred(xmlerrcxt))
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_ARGUMENT_FOR_XQUERY,
						"failed to apply stylesheet");

		resstat = xsltSaveResultToString((xmlChar **) &resstr, &reslen,
										 restree, stylesheet);
		if (resstat < 0 || pg_xml_error_occurred(xmlerrcxt))
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_ARGUMENT_FOR_XQUERY,
						"failed to save result to string");
		result = cstring_to_text_with_len((char *) resstr, reslen);
	}
	PG_CATCH();
	{
		if (restree != NULL)
			xmlFreeDoc(restree);
		if (xslt_ctxt != NULL)
			xsltFreeTransformContext(xslt_ctxt);
		if (xslt_sec_prefs != NULL)
			xsltFreeSecurityPrefs(xslt_sec_prefs);
		if (stylesheet != NULL)
			xsltFreeStylesheet(stylesheet);
		if (doctree != NULL)
			xmlFreeDoc(doctree);
		if (resstr != NULL)
			xmlFree(resstr);
		xsltCleanupGlobals();

		xsltSetGenericErrorFunc(saved_errcxt, saved_errfunc);
		pg_xml_done(xmlerrcxt, true);

		PG_RE_THROW();
	}
	PG_END_TRY();

	xmlFreeDoc(restree);
	xsltFreeTransformContext(xslt_ctxt);
	xsltFreeSecurityPrefs(xslt_sec_prefs);
	xsltFreeStylesheet(stylesheet);
	xmlFreeDoc(doctree);
	xsltCleanupGlobals();

	if (resstr)
		xmlFree(resstr);

	xsltSetGenericErrorFunc(saved_errcxt, saved_errfunc);
	pg_xml_done(xmlerrcxt, false);

	return result;
}

static const char **
parse_params(text *paramstr)
{
	char	   *pos;
	char	   *pstr;
	char	   *nvsep = "=";
	char	   *itsep = ",";
	const char **params;
	int			max_params;
	int			nparams;

	pstr = text_to_cstring(paramstr);

	max_params = 20;			/* must be even! */
	params = (const char **) palloc((max_params + 1) * sizeof(char *));
	nparams = 0;

	pos = pstr;

	while (*pos != '\0')
	{
		if (nparams >= max_params)
		{
			max_params *= 2;
			params = (const char **) repalloc(params,
											  (max_params + 1) * sizeof(char *));
		}
		params[nparams++] = pos;
		pos = strstr(pos, nvsep);
		if (pos != NULL)
		{
			*pos = '\0';
			pos++;
		}
		else
		{
			/* No equal sign, so ignore this "parameter" */
			nparams--;
			break;
		}

		/* since max_params is even, we still have nparams < max_params */
		params[nparams++] = pos;
		pos = strstr(pos, itsep);
		if (pos != NULL)
		{
			*pos = '\0';
			pos++;
		}
		else
			break;
	}

	/* Add the terminator marker; we left room for it in the palloc's */
	params[nparams] = NULL;

	return params;
}

#endif							/* USE_LIBXSLT */
