/*-------------------------------------------------------------------------
 * url/url.c
 *
 * By the WHATWG URL specification https://url.spec.whatwg.org/.
 * Supports Unicode ToASCII, ToUnicode.
 *
 * A complete URL entry consists of the following parts:
 *
 *     https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor
 *     |___|   |__| |____| |_________| |__||___________| |_____| |____|
 *       |      |     |        |        |        |          |      |
 *     scheme   |  password    |       port      |        query    |
 *              |              |                 |                 |
 *           username         host              path            fragment
 *
 *
 * Functions to get separate parts of a URL:
 *     scheme, username, password, host, host_unicode, port, path, query,
 *     fragment.
 *
 * Example:
 *     SELECT ('https://example.com/'::url).host;
 * Result:
 *     example.com
 *
 *
 * Functions to set separate parts of a URL:
 *     url_scheme_set, url_username_set, url_password_set, url_host_set,
 *     url_hostname_set, url_port_set, url_path_set, url_query_set,
 *     url_fragment_set.
 *
 * Example:
 *     SELECT url_host_set('https://example.com/'::url, 'postgresql.org');
 * Result:
 *     https://postgresql.org/
 *
 * All URL modification functions return the full modified URL.
 * All URL functions will return NULL if a NULL value is passed as the URL.
 *
 *
 * The url_base() function:
 *     The function allows to create a new URL based on the base URL and
 *     relative URL.
 *
 * Example:
 *     SELECT url_base('https://example.com/path/to'::url, '/new/path#and-fragment');
 *     SELECT url_base('https://example.com/path/to'::url, 'wss://postgresql.org/new/path#and-fragment');
 *     SELECT url_base('https://example.com/path/to/home/'::url, 'world');
 *     SELECT url_base('https://example.com/path/to/home/'::url, '../world');
 * Result:
 *     https://example.com/new/path#and-fragment
 *     wss://postgresql.org/new/path#and-fragment
 *     https://example.com/path/to/home/world
 *     https://example.com/path/to/world
 *
 * More information about functions can be found in the README.
 *
 *-------------------------------------------------------------------------
 */
#include <postgres.h>
#include <varatt.h>
#include <utils/builtins.h>
#include <mb/pg_wchar.h>

#include "lexbor/url/url.h"


PG_MODULE_MAGIC;


#define PG_RETURN_URL_P(p)		PG_RETURN_POINTER(p)
#define PG_GETARG_URL_P(n)		PG_DETOAST_DATUM(PG_GETARG_DATUM(n))

#define URL_LXB_STR_ARGS(str)	(const char *) (str)->data, (str)->length
#define URL_VARDATA(vardata)	((uint8_t *) VARDATA(vardata))
#define URL_HEAD_SIZE			(URL_LAST_ENTRY * sizeof(uint32_t))


typedef enum URLIndex
{
	URL_SCHEME = 0x00,
	URL_USERNAME,
	URL_PASSWORD,
	URL_HOST,
	URL_PATH,
	URL_QUERY,
	URL_FRAGMENT,
	URL_SUM,
	URL_PORT,
	URL_LAST_ENTRY
} URLIndex;

typedef struct URLCallbackContext
{
	char	*result;
	size_t	length;
} URLCallbackContext;

typedef struct varlena URL;

typedef lxb_status_t
(*URLSetFunc)(lxb_url_t *url, lxb_url_parser_t *parser,
			  const lxb_char_t *data, size_t length);


/*
 * External dynamically-loaded functions.
 */
PG_FUNCTION_INFO_V1(url_in);
PG_FUNCTION_INFO_V1(url_out);

PG_FUNCTION_INFO_V1(url_scheme);
PG_FUNCTION_INFO_V1(url_username);
PG_FUNCTION_INFO_V1(url_password);
PG_FUNCTION_INFO_V1(url_host);
PG_FUNCTION_INFO_V1(url_host_unicode);
PG_FUNCTION_INFO_V1(url_port);
PG_FUNCTION_INFO_V1(url_path);
PG_FUNCTION_INFO_V1(url_query);
PG_FUNCTION_INFO_V1(url_fragment);
PG_FUNCTION_INFO_V1(url_create);
PG_FUNCTION_INFO_V1(url_base);

PG_FUNCTION_INFO_V1(url_scheme_set);
PG_FUNCTION_INFO_V1(url_username_set);
PG_FUNCTION_INFO_V1(url_password_set);
PG_FUNCTION_INFO_V1(url_host_set);
PG_FUNCTION_INFO_V1(url_hostname_set);
PG_FUNCTION_INFO_V1(url_port_set);
PG_FUNCTION_INFO_V1(url_port_num_set);
PG_FUNCTION_INFO_V1(url_path_set);
PG_FUNCTION_INFO_V1(url_query_set);
PG_FUNCTION_INFO_V1(url_fragment_set);

/*
 * Internal declarations.
 */
static lxb_url_t *url_parse(char *data, size_t length,const lxb_url_t *base);
static URL *url_change_part(URL *var_url, char *data,
							size_t length, URLSetFunc set, const char *name);
static URL *url_new(char *data);
static lxb_status_t url_api_username_set(lxb_url_t *url, lxb_url_parser_t *parser,
										 const lxb_char_t *username, size_t length);
static lxb_status_t url_api_password_set(lxb_url_t *url, lxb_url_parser_t *parser,
										 const lxb_char_t *password, size_t length);
static URL *url_pack(lxb_url_t *url);
static uint32_t url_pack_size(lxb_url_t *url, uint32_t *head);
static void url_pack_string(const lexbor_str_t *str, uint8_t *data);
static void url_pack_scheme(const lxb_url_scheme_t *scheme, uint8_t *data);
static void url_pack_host(const lxb_url_host_t *host, uint8_t *data);
static void url_pack_path(const lxb_url_path_t *path, uint8_t *data);
static uint32_t url_string_size(const lexbor_str_t *str);
static uint32_t url_scheme_size(const lxb_url_scheme_t *scheme);
static uint32_t url_host_size(const lxb_url_host_t *host);
static uint32_t url_path_size(const lxb_url_path_t *path);
static void url_unpack(lxb_url_t *url, const uint8_t *data, lexbor_mraw_t *mraw);
static void url_unpack_string(lexbor_str_t *str, const uint8_t *data,
							  lexbor_mraw_t *mraw, URLIndex idx);
static void url_unpack_scheme(lxb_url_scheme_t *scheme, const uint8_t *data,
							  lexbor_mraw_t *mraw);
static void url_unpack_host(lxb_url_host_t *host, const uint8_t *data,
							lexbor_mraw_t *mraw);
static void url_unpack_port(lxb_url_t *url, const uint8_t *data);
static void url_unpack_path(lxb_url_path_t *path, const uint8_t *data,
							lexbor_mraw_t *mraw);
static void url_copy_data(lexbor_str_t *str, lexbor_mraw_t *mraw,
						  const uint8_t *data, uint32_t length);
static lxb_status_t url_callback(const lxb_char_t *data, size_t len, void *ctx);
static void *url_palloc0(size_t num, size_t size);

/*
 * Inline functions.
 */
static inline char *
url_encoding_encode(char *str, uint32_t length)
{
	return (char *) pg_do_encoding_conversion((unsigned char *) str, length,
											  PG_UTF8, GetDatabaseEncoding());
}

static inline char *
url_encoding_decode(char *str, uint32_t length)
{
	return (char *) pg_do_encoding_conversion((unsigned char *) str, length,
											  GetDatabaseEncoding(), PG_UTF8);
}

static inline const uint32_t *
url_head_entry(const uint8_t *data, URLIndex idx)
{
	return ((uint32_t *) data) + idx;
}

static inline uint32_t
url_entry_offset(const uint8_t *data, URLIndex idx)
{
	return *url_head_entry(data, idx);
}

static inline uint32_t
url_entry_length(const uint8_t *data, URLIndex idx)
{
	const uint32_t *off = url_head_entry(data, idx);
	return off[1] - off[0];
}


/*
 * All NULL arguments will be considered as empty value.
 */
static char url_empty_str[] = "";


/*
 * Module load callback.
 */
void
_PG_init(void)
{
	/* Lexbor supports overriding the allocation routines. */
	lexbor_memory_setup(palloc, repalloc, url_palloc0, pfree);
}

/*
 * Input/Output.
 */

/*
 * The Input function parses/validated the URL and packs the parsed URL into an
 * internal storage format.
 *
 * Format:
 * Head contains offsets for each part of the URL:
 *     Bergin: 0 byte. End: (sizeof(uint32_t) * URL_LAST_ENTRY) byte.
 *
 * To get the necessary offset we just need sizeof(uint32_t) * URLIndex.
 *
 * Body:
 *     After head, the body with the URL entries begins.
 *
 * URL_PORT stores the port directly, not the offset to the body.
 */
Datum
url_in(PG_FUNCTION_ARGS)
{
	PG_RETURN_URL_P(url_new(PG_GETARG_CSTRING(0)));
}

Datum
url_out(PG_FUNCTION_ARGS)
{
	URL *vardata = PG_GETARG_URL_P(0);
	lxb_url_t url;
	URLCallbackContext ctx;

	url_unpack(&url, URL_VARDATA(vardata), NULL);

	ctx.result = palloc(lxb_url_length(&url, false) + 1);
	ctx.length = 0;

	lxb_url_serialize(&url, url_callback, &ctx, false);

	ctx.result[ctx.length] = 0x00;

	/*
	 * We will not convert the encoding (no matter what encoding is in the base)
	 * because the URL is always returned in ASCII.  All encodings that Postgres
	 * supports understand ASCII < 0x80.
	 */

	PG_RETURN_CSTRING(ctx.result);
}

/*
 * Getter functions for get parts of URL.
 * Scheme, username, password, host, host_unicode, port, path, query, fragment.
 */
Datum
url_scheme(PG_FUNCTION_ARGS)
{
	URL *vardata = PG_GETARG_URL_P(0);
	lxb_url_scheme_t scheme;

	url_unpack_scheme(&scheme, URL_VARDATA(vardata), NULL);

	if (scheme.type == LXB_URL_SCHEMEL_TYPE__UNDEF)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text_with_len(URL_LXB_STR_ARGS(&scheme.name)));
}

Datum
url_username(PG_FUNCTION_ARGS)
{
	URL *vardata = PG_GETARG_URL_P(0);
	lexbor_str_t str;

	url_unpack_string(&str, URL_VARDATA(vardata), NULL, URL_USERNAME);

	if (str.length == 0)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text_with_len(URL_LXB_STR_ARGS(&str)));
}

Datum
url_password(PG_FUNCTION_ARGS)
{
	URL *vardata = PG_GETARG_URL_P(0);
	lexbor_str_t str;

	url_unpack_string(&str, URL_VARDATA(vardata), NULL, URL_PASSWORD);

	if (str.length == 0)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text_with_len(URL_LXB_STR_ARGS(&str)));
}

Datum
url_host(PG_FUNCTION_ARGS)
{
	URL *vardata = PG_GETARG_URL_P(0);
	text *txt;
	size_t length;
	URLCallbackContext ctx;
	lxb_url_host_t host;

	url_unpack_host(&host, URL_VARDATA(vardata), NULL);

	if (host.type == LXB_URL_HOST_TYPE__UNDEF)
		PG_RETURN_NULL();

	length = lxb_url_host_length(&host);
	txt = (text *) palloc(length + VARHDRSZ);

	ctx.result = VARDATA(txt);
	ctx.length = 0;

	lxb_url_serialize_host(&host, url_callback, &ctx);

	SET_VARSIZE(txt, ctx.length + VARHDRSZ);

	PG_RETURN_TEXT_P(txt);
}

Datum
url_host_unicode(PG_FUNCTION_ARGS)
{
	URL *vardata = PG_GETARG_URL_P(0);
	char *result;
	text *txt;
	size_t length;
	URLCallbackContext ctx;
	lxb_url_host_t host;

	url_unpack_host(&host, URL_VARDATA(vardata), NULL);

	if (host.type == LXB_URL_HOST_TYPE__UNDEF)
		PG_RETURN_NULL();

	length = lxb_url_host_unicode_length(&host);

	txt = (text *) palloc(length + VARHDRSZ);

	ctx.result = VARDATA(txt);
	ctx.length = 0;

	lxb_url_serialize_host_unicode(&host, url_callback, &ctx);

	result = url_encoding_encode(ctx.result, ctx.length);

	if (ctx.result != result)
	{
		ctx.length = strlen(result);

		/*
		 * Perhaps we should check the size, if it has not changed, then do not
		 * reallocate memory.
		 */
		pfree(txt);
		txt = (text *) palloc(ctx.length + VARHDRSZ);

		memcpy(VARDATA(txt), result, ctx.length);
		pfree(result);
	}

	SET_VARSIZE(txt, ctx.length + VARHDRSZ);

	PG_RETURN_TEXT_P(txt);
}

Datum
url_port(PG_FUNCTION_ARGS)
{
	URL *vardata = PG_GETARG_URL_P(0);
	lxb_url_t url;

	url_unpack_port(&url, URL_VARDATA(vardata));

	if (!url.has_port)
		PG_RETURN_NULL();

	PG_RETURN_UINT16(url.port);
}

Datum
url_path(PG_FUNCTION_ARGS)
{
	URL *vardata = PG_GETARG_URL_P(0);
	lxb_url_path_t path;

	url_unpack_path(&path, URL_VARDATA(vardata), NULL);

	if (path.str.length == 0)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text_with_len(URL_LXB_STR_ARGS(&path.str)));
}

Datum
url_query(PG_FUNCTION_ARGS)
{
	URL *vardata = PG_GETARG_URL_P(0);
	lexbor_str_t str;

	url_unpack_string(&str, URL_VARDATA(vardata), NULL, URL_QUERY);

	if (str.length == 0)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text_with_len(URL_LXB_STR_ARGS(&str)));
}

Datum
url_fragment(PG_FUNCTION_ARGS)
{
	URL *vardata = PG_GETARG_URL_P(0);
	lexbor_str_t str;

	url_unpack_string(&str, URL_VARDATA(vardata), NULL, URL_FRAGMENT);

	if (str.length == 0)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text_with_len(URL_LXB_STR_ARGS(&str)));
}

Datum
url_create(PG_FUNCTION_ARGS)
{
	PG_RETURN_URL_P(url_new(TextDatumGetCString(PG_GETARG_DATUM(0))));
}

Datum
url_base(PG_FUNCTION_ARGS)
{
	char *str;
	URL *varbase;
	lxb_url_t *url, url_base;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	varbase = PG_GETARG_URL_P(0);
	str = (PG_ARGISNULL(1)) ? url_empty_str
							: TextDatumGetCString(PG_GETARG_DATUM(1));

	url_unpack(&url_base, URL_VARDATA(varbase), NULL);

	url = url_parse(str, strlen(str), &url_base);
	if (url == NULL)
		ereport(ERROR,
				errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("failed to parse the relative URL \"%s\"", str));

	varbase = url_pack(url);

	lxb_url_memory_destroy(url);

	PG_RETURN_URL_P(varbase);
}

/*
 * Setter functions for change parts of URL.
 * Scheme, username, password, host, hostname, port, path, query, fragment.
 */
#define URL_MAKE_SETTER_FUNCTION(name, func)	\
	Datum	\
	url_ ## name ## _set(PG_FUNCTION_ARGS)	\
	{	\
		char *str;	\
		URL *vardata;	\
	\
		if (PG_ARGISNULL(0))	\
			PG_RETURN_NULL();	\
	\
		vardata = PG_GETARG_URL_P(0);	\
		str = (PG_ARGISNULL(1)) ? url_empty_str	\
								: TextDatumGetCString(PG_GETARG_DATUM(1));	\
	\
		vardata = url_change_part(vardata, str, strlen(str), (func), #name);	\
	\
		PG_RETURN_URL_P(vardata);	\
	}

URL_MAKE_SETTER_FUNCTION(scheme, lxb_url_api_protocol_set)
URL_MAKE_SETTER_FUNCTION(username, url_api_username_set)
URL_MAKE_SETTER_FUNCTION(password, url_api_password_set)
URL_MAKE_SETTER_FUNCTION(host, lxb_url_api_host_set)
URL_MAKE_SETTER_FUNCTION(hostname, lxb_url_api_hostname_set)
URL_MAKE_SETTER_FUNCTION(port, lxb_url_api_port_set)
URL_MAKE_SETTER_FUNCTION(path, lxb_url_api_pathname_set)
URL_MAKE_SETTER_FUNCTION(query, lxb_url_api_search_set)
URL_MAKE_SETTER_FUNCTION(fragment, lxb_url_api_hash_set)

Datum
url_port_num_set(PG_FUNCTION_ARGS)
{
	URL *vardata = PG_GETARG_URL_P(0);
	uint32_t port = PG_GETARG_UINT32(1);
	int len;
	char buf[12]; /* 10 digits, '\0' */

	len = pg_ultoa_n(port, buf);

	vardata = url_change_part(vardata, buf, len, lxb_url_api_port_set, "port");

	PG_RETURN_URL_P(vardata);
}

/*
 * Utilities for parsing, packaging, and unpacking URLs.
 */
static lxb_url_t *
url_parse(char *data, size_t length, const lxb_url_t *base)
{
	char *dst;
	lxb_url_t *url;
	lxb_status_t status;
	lxb_url_parser_t parser;

	status = lxb_url_parser_init(&parser, NULL);
	if (status != LXB_STATUS_OK)
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("failed to create URL parser"));

	dst = url_encoding_decode(data, length);

	if (data == dst)
		url = lxb_url_parse(&parser, base, (const lxb_char_t *) data, length);
	else
	{
		url = lxb_url_parse(&parser, base,
							(const lxb_char_t *) dst, strlen(dst));
		pfree(dst);
	}

	lxb_url_parser_destroy(&parser, false);

	return url;
}

static URL *
url_change_part(URL *var_url, char *data, size_t length,
				URLSetFunc set, const char *name)
{
	char *dst;
	URL *result;
	lxb_url_t url;
	lxb_status_t status;
	lexbor_mraw_t mraw;

	lexbor_mraw_init(&mraw, 4096);

	url_unpack(&url, URL_VARDATA(var_url), &mraw);

	dst = url_encoding_decode(data, length);

	if (data == dst)
		status = set(&url, NULL, (const lxb_char_t *) data, length);
	else
	{
		status = set(&url, NULL, (const lxb_char_t *) dst, strlen(dst));
		pfree(dst);
	}

	if (status != LXB_STATUS_OK)
		ereport(ERROR,
				errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("failed to parse \"%s\" part \"%.*s\" of URL",
					   name, (int) length, data));

	result = url_pack(&url);

	lexbor_mraw_destroy(&mraw, false);

	return result;
}

static URL *
url_new(char *data)
{
	URL *vardata;
	lxb_url_t *url;

	url = url_parse(data, strlen(data), NULL);
	if (url == NULL)
		ereport(ERROR,
				errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("failed to parse the URL \"%s\"", data));

	vardata = url_pack(url);

	lxb_url_memory_destroy(url);

	return vardata;
}

static lxb_status_t
url_api_username_set(lxb_url_t *url, lxb_url_parser_t *parser,
					 const lxb_char_t *username, size_t length)
{
	(void) parser;
	return lxb_url_api_username_set(url, username, length);
}

static lxb_status_t
url_api_password_set(lxb_url_t *url, lxb_url_parser_t *parser,
					 const lxb_char_t *password, size_t length)
{
	(void) parser;
	return lxb_url_api_password_set(url, password, length);
}

static URL *
url_pack(lxb_url_t *url)
{
	URL *vardata;
	uint8_t *data;
	uint32_t size;
	uint32_t head[URL_LAST_ENTRY];

	size = url_pack_size(url, head);

	vardata = palloc(VARHDRSZ + size);
	data = URL_VARDATA(vardata);

	/* Store head. */
	memcpy(data, head, URL_HEAD_SIZE);

	/* Store body. */
	url_pack_scheme(&url->scheme, data + head[URL_SCHEME]);
	url_pack_string(&url->username, data + head[URL_USERNAME]);
	url_pack_string(&url->password, data + head[URL_PASSWORD]);
	url_pack_host(&url->host, data + head[URL_HOST]);
	url_pack_path(&url->path, data + head[URL_PATH]);
	url_pack_string(&url->query, data + head[URL_QUERY]);
	url_pack_string(&url->fragment, data + head[URL_FRAGMENT]);

	SET_VARSIZE(vardata, VARHDRSZ + size);

	return vardata;
}

static uint32_t
url_pack_size(lxb_url_t *url, uint32_t *head)
{
	head[URL_SCHEME] = URL_HEAD_SIZE;
	head[URL_USERNAME] = head[URL_SCHEME] + url_scheme_size(&url->scheme);
	head[URL_PASSWORD] = head[URL_USERNAME] + url_string_size(&url->username);
	head[URL_HOST] = head[URL_PASSWORD] + url_string_size(&url->password);
	head[URL_PATH] = head[URL_HOST] + url_host_size(&url->host);
	head[URL_QUERY] = head[URL_PATH] + url_path_size(&url->path);
	head[URL_FRAGMENT] = head[URL_QUERY] + url_string_size(&url->query);
	head[URL_SUM] = head[URL_FRAGMENT] + url_string_size(&url->fragment);

	/*
	 * Port has size uint16_t there is no sense to write it separately, we can
	 * write it to the header at once.
	 */
	head[URL_PORT] = url->port << 8 | (uint8_t) url->has_port;

	return head[URL_SUM];
}

static void
url_pack_string(const lexbor_str_t *str, uint8_t *data)
{
	if (str->length > 0)
		memcpy(data, str->data, str->length);
}

static void
url_pack_scheme(const lxb_url_scheme_t *scheme, uint8_t *data)
{
	if (scheme->type == LXB_URL_SCHEMEL_TYPE__UNDEF)
		return;

	/* type + string. */
	*data = (uint8_t) scheme->type;
	url_pack_string(&scheme->name, data + sizeof(uint8_t));
}

static void
url_pack_host(const lxb_url_host_t *host, uint8_t *data)
{
	*data = (uint8_t) host->type;
	data += sizeof(uint8_t);

	switch (host->type)
	{
		case LXB_URL_HOST_TYPE_DOMAIN:
		case LXB_URL_HOST_TYPE_OPAQUE:
			url_pack_string(&host->u.domain, data);
			break;

		case LXB_URL_HOST_TYPE_IPV4:
			memcpy(data, &host->u.ipv4, sizeof(host->u.ipv4));
			break;

		case LXB_URL_HOST_TYPE_IPV6:
			memcpy(data, host->u.ipv6, sizeof(host->u.ipv6));
			break;

		default:
			break;
	}
}

static void
url_pack_path(const lxb_url_path_t *path, uint8_t *data)
{
	if (path->length == 0)
		return;

	/* opaque + length + string. */
	*data = (uint8_t) path->opaque;
	data += sizeof(uint8_t);

	*((uint32_t *) data) = (uint32_t) path->length;
	data += sizeof(uint32_t);

	url_pack_string(&path->str, data);
}

static uint32_t
url_string_size(const lexbor_str_t *str)
{
	return (uint32_t) str->length;
}

static uint32_t
url_scheme_size(const lxb_url_scheme_t *scheme)
{
	if (scheme->type == LXB_URL_SCHEMEL_TYPE__UNDEF)
		return 0;

	/* type + string. */
	return sizeof(uint8_t) + url_string_size(&scheme->name);
}

static uint32_t
url_host_size(const lxb_url_host_t *host)
{
	uint32_t size;

	switch (host->type)
	{
		case LXB_URL_HOST_TYPE_DOMAIN:
		case LXB_URL_HOST_TYPE_OPAQUE:
			size = url_string_size(&host->u.domain);
			break;

		case LXB_URL_HOST_TYPE_IPV4:
			size = sizeof(host->u.ipv4);
			break;

		case LXB_URL_HOST_TYPE_IPV6:
			size = sizeof(host->u.ipv6);
			break;

		default:
			return 0;
	}

	/* type + data. */
	return sizeof(uint8_t) + size;
}

static uint32_t
url_path_size(const lxb_url_path_t *path)
{
	if (path->length == 0)
		return 0;

	/* opaque + length + string. */
	return sizeof(uint8_t) + sizeof(uint32_t) + url_string_size(&path->str);
}

static void
url_unpack(lxb_url_t *url, const uint8_t *data, lexbor_mraw_t *mraw)
{
	/*
	 * TODO: We should check the size of the header and body before unpacking.
	 * It is possible that the data will arrive broken and we will get “hello”.
	 */

	/*
	 * We can use the current memory partitioning to serialize/read lxb_url_t
	 * (all getter functions).
	 *
	 * To change the URL object, we need to access the URL parser, which has
	 * its own memory partition for lxb_url_t.  Therefore, we copy data for all
	 * setter functions.
	 */
	url->mraw = mraw;

	url_unpack_scheme(&url->scheme, data, mraw);
	url_unpack_string(&url->username, data, mraw, URL_USERNAME);
	url_unpack_string(&url->password, data, mraw, URL_PASSWORD);
	url_unpack_host(&url->host, data, mraw);
	url_unpack_port(url, data);
	url_unpack_path(&url->path, data, mraw);
	url_unpack_string(&url->query, data, mraw, URL_QUERY);
	url_unpack_string(&url->fragment, data, mraw, URL_FRAGMENT);
}

static void
url_unpack_string(lexbor_str_t *str, const uint8_t *data, lexbor_mraw_t *mraw,
				  URLIndex idx)
{
	lxb_char_t *begin;
	const uint32_t offset = url_entry_offset(data, idx);
	const uint32_t length = url_entry_length(data, idx);

	if (length == 0)
	{
		memset(str, 0x00, sizeof(lexbor_str_t));
		return;
	}

	begin = (lxb_char_t *) &data[offset];

	if (mraw == NULL)
	{
		str->data = begin;
		str->length = length;
	}
	else
		url_copy_data(str, mraw, begin, length);
}

static void
url_unpack_scheme(lxb_url_scheme_t *scheme, const uint8_t *data,
				  lexbor_mraw_t *mraw)
{
	lxb_char_t *begin;
	lexbor_str_t *str;
	const uint32_t offset = url_entry_offset(data, URL_SCHEME);
	uint32_t length = url_entry_length(data, URL_SCHEME);

	if (length == 0)
	{
		memset(scheme, 0x00, sizeof(lxb_url_scheme_t));
		return;
	}

	str = &scheme->name;
	length -= 1; /* skip type */

	scheme->type = (lxb_url_scheme_type_t) data[offset];
	begin = (lxb_char_t *) &data[offset + 1];

	if (mraw == NULL)
	{
		str->data = begin;
		str->length = length;
	} else
		url_copy_data(str, mraw, begin, length);
}

static void
url_unpack_host(lxb_url_host_t *host, const uint8_t *data, lexbor_mraw_t *mraw)
{
	lxb_char_t *begin;
	const uint32_t offset = url_entry_offset(data, URL_HOST);
	uint32_t length = url_entry_length(data, URL_HOST);

	if (length == 0)
	{
		memset(host, 0x00, sizeof(lxb_url_host_t));
		return;
	}

	length -= 1; /* skip type */

	host->type = (lxb_url_host_type_t) data[offset];
	begin = (lxb_char_t *) &data[offset + 1];

	switch (host->type)
	{
		case LXB_URL_HOST_TYPE_DOMAIN:
		case LXB_URL_HOST_TYPE_OPAQUE:
			if (mraw == NULL)
			{
				host->u.domain.data = begin;
				host->u.domain.length = length;
			} else
				url_copy_data(&host->u.domain, mraw, begin, length);
			break;

		case LXB_URL_HOST_TYPE_IPV4:
			memcpy(&host->u.ipv4, begin, length);
			break;

		case LXB_URL_HOST_TYPE_IPV6:
			memcpy(host->u.ipv6, begin, length);
			break;

		default:
			break;
	}
}

static void
url_unpack_port(lxb_url_t *url, const uint8_t *data)
{
	const uint32_t num = url_entry_offset(data, URL_PORT);

	url->port = num >> 8;
	url->has_port = num & 1;
}

static void
url_unpack_path(lxb_url_path_t *path, const uint8_t *data, lexbor_mraw_t *mraw)
{
	lxb_char_t *begin;
	lexbor_str_t *str;
	const uint32_t offset = url_entry_offset(data, URL_PATH);
	uint32_t length = url_entry_length(data, URL_PATH);

	if (length == 0)
	{
		memset(path, 0x00, sizeof(lxb_url_path_t));
		return;
	}

	str = &path->str;
	length = length - sizeof(uint32_t) - 1;

	/* +sizeof(uint32_t) skip path->length; +1 skip path->opaque */
	begin = (lxb_char_t *) &data[offset + sizeof(uint32_t) + 1];

	if (mraw == NULL)
	{
		str->data = begin;
		str->length = length;
	} else
		url_copy_data(str, mraw, begin, length);

	path->opaque = (lxb_url_host_type_t) data[offset];
	path->length = *((uint32_t *) &data[offset + 1]); /* +1 skip path->opaque */
}

static void
url_copy_data(lexbor_str_t *str, lexbor_mraw_t *mraw,
			  const uint8_t *data, uint32_t length)
{
	str->data = lexbor_mraw_alloc(mraw, length + 1);

	memcpy(str->data, data, length);

	str->data[length] = 0x00;
	str->length = length;
}

static lxb_status_t
url_callback(const lxb_char_t *data, size_t len, void *ctx)
{
	URLCallbackContext *context = ctx;

	memcpy(context->result + context->length, data, len);
	context->length += len;

	return LXB_STATUS_OK;
}

static void *
url_palloc0(size_t num, size_t size)
{
	return palloc0(size * num);
}
