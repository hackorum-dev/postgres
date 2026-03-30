/*
 * cash_exchange.c
 *		Automatic exchange rate conversion for the money type.
 *
 * When money_source_currency is set to an ISO 4217 code (e.g. "USD")
 * and the session's lc_monetary locale implies a different currency,
 * cash_exchange_rate() returns a conversion factor that cash_out()
 * applies at display time.  Rates are fetched from the European Central
 * Bank via the Frankfurter API and cached per-session for one hour.
 *
 * src/backend/utils/adt/cash_exchange.c
 */
#include "postgres.h"

#include <ctype.h>
#include <math.h>

#include "common/jsonapi.h"
#include "mb/pg_wchar.h"
#include "utils/cash_exchange.h"
#include "utils/guc.h"
#include "utils/pg_locale.h"
#include "utils/timestamp.h"

#ifdef USE_LIBCURL
#include <curl/curl.h>
#endif

/* GUC variable */
char	   *money_source_currency = "";

/*
 * Session-local exchange rate cache.  We keep only one currency pair
 * cached, which is fine because a session typically uses one lc_monetary
 * setting throughout.
 */
typedef struct FXRateCache
{
	char		from_currency[4];	/* ISO 4217 source */
	char		to_currency[4];		/* ISO 4217 target */
	float8		rate;
	TimestampTz fetch_time;
	bool		valid;
} FXRateCache;

static FXRateCache fx_cache = {.valid = false};

/* Cache TTL: 1 hour */
#define FX_CACHE_TTL_USEC	(INT64CONST(3600) * USECS_PER_SEC)

/* ----------------------------------------------------------------
 *		GUC check hook
 * ----------------------------------------------------------------
 */
bool
check_money_source_currency(char **newval, void **extra, GucSource source)
{
	const char *val = *newval;

	/* Empty string disables conversion */
	if (val[0] == '\0')
		return true;

	/* Must be exactly 3 ASCII alphabetic characters */
	if (strlen(val) != 3 ||
		!isalpha((unsigned char) val[0]) ||
		!isalpha((unsigned char) val[1]) ||
		!isalpha((unsigned char) val[2]))
	{
		GUC_check_errdetail("Value must be a 3-letter ISO 4217 currency code.");
		return false;
	}

	return true;
}

#ifdef USE_LIBCURL

/* ----------------------------------------------------------------
 *		libcurl response buffer
 * ----------------------------------------------------------------
 */
typedef struct CurlBuffer
{
	char	   *data;
	size_t		len;
	size_t		capacity;
} CurlBuffer;

#define CURL_BUF_INIT_SIZE	1024
#define CURL_BUF_MAX_SIZE	(64 * 1024)

static size_t
curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	CurlBuffer *buf = (CurlBuffer *) userdata;
	size_t		bytes = size * nmemb;

	if (buf->len + bytes > CURL_BUF_MAX_SIZE)
		return 0;				/* signal error to curl */

	if (buf->len + bytes >= buf->capacity)
	{
		buf->capacity = Max(buf->capacity * 2, buf->len + bytes + 1);
		buf->data = repalloc(buf->data, buf->capacity);
	}

	memcpy(buf->data + buf->len, ptr, bytes);
	buf->len += bytes;
	buf->data[buf->len] = '\0';
	return bytes;
}

/* ----------------------------------------------------------------
 *		JSON response parser
 *
 * We parse the Frankfurter API response which looks like:
 *   {"amount":1.0,"base":"USD","date":"2026-03-28","rates":{"EUR":0.92}}
 *
 * We track the current field name and look for a scalar value whose
 * field name matches the target currency code.
 * ----------------------------------------------------------------
 */
typedef struct FXParseState
{
	char		last_field[8];
	float8		rate;
	bool		found_rate;
	const char *target_currency;
	int			depth;			/* nesting depth inside "rates" object */
	bool		in_rates;
} FXParseState;

static JsonParseErrorType
fx_object_field_start(void *state, char *fname, bool isnull)
{
	FXParseState *s = (FXParseState *) state;

	if (fname)
	{
		strlcpy(s->last_field, fname, sizeof(s->last_field));

		if (pg_strcasecmp(fname, "rates") == 0)
			s->in_rates = true;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
fx_scalar(void *state, char *token, JsonTokenType tokentype)
{
	FXParseState *s = (FXParseState *) state;

	if (s->in_rates && tokentype == JSON_TOKEN_NUMBER &&
		pg_strcasecmp(s->last_field, s->target_currency) == 0)
	{
		char	   *endptr;

		s->rate = strtod(token, &endptr);
		if (endptr != token && s->rate > 0)
			s->found_rate = true;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
fx_object_end(void *state)
{
	FXParseState *s = (FXParseState *) state;

	/* Reset in_rates when leaving the rates object */
	s->in_rates = false;
	return JSON_SUCCESS;
}

/*
 * Fetch exchange rate from the Frankfurter API.
 * Returns true on success, storing the rate in *rate_out.
 */
static bool
fetch_exchange_rate(const char *from, const char *to, float8 *rate_out)
{
	CURL	   *curl;
	CURLcode	res;
	CurlBuffer	buf;
	char		url[256];
	JsonLexContext lex;
	JsonSemAction sem;
	FXParseState parse_state;
	JsonParseErrorType json_error;

	snprintf(url, sizeof(url),
			 "https://api.frankfurter.app/latest?from=%.3s&to=%.3s",
			 from, to);

	/* Initialize response buffer */
	buf.data = palloc(CURL_BUF_INIT_SIZE);
	buf.data[0] = '\0';
	buf.len = 0;
	buf.capacity = CURL_BUF_INIT_SIZE;

	curl = curl_easy_init();
	if (!curl)
	{
		pfree(buf.data);
		return false;
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "PostgreSQL");

	res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK)
	{
		pfree(buf.data);
		return false;
	}

	/* Parse JSON response */
	memset(&sem, 0, sizeof(sem));
	memset(&parse_state, 0, sizeof(parse_state));

	parse_state.target_currency = to;
	sem.semstate = &parse_state;
	sem.object_field_start = fx_object_field_start;
	sem.scalar = fx_scalar;
	sem.object_end = fx_object_end;

	makeJsonLexContextCstringLen(&lex, buf.data, buf.len,
								PG_UTF8, true);

	json_error = pg_parse_json(&lex, &sem);
	freeJsonLexContext(&lex);
	pfree(buf.data);

	if (json_error != JSON_SUCCESS || !parse_state.found_rate)
		return false;

	*rate_out = parse_state.rate;
	return true;
}

#endif							/* USE_LIBCURL */

/* ----------------------------------------------------------------
 *		Public API
 * ----------------------------------------------------------------
 */

/*
 * cash_exchange_rate
 *
 * Returns the exchange rate to multiply a stored money value by for
 * display in the client's currency.  Returns 1.0 if no conversion
 * is needed or possible.
 */
float8
cash_exchange_rate(void)
{
	struct lconv *lconvert;
	char		target[4];

	/* No source currency configured?  No conversion. */
	if (money_source_currency[0] == '\0')
		return 1.0;

	/* Extract target currency from session's lc_monetary */
	lconvert = PGLC_localeconv();
	if (strlen(lconvert->int_curr_symbol) < 3)
		return 1.0;				/* locale has no currency info (e.g. "C") */

	memcpy(target, lconvert->int_curr_symbol, 3);
	target[3] = '\0';

	/* Same currency?  No conversion needed. */
	if (pg_strcasecmp(money_source_currency, target) == 0)
		return 1.0;

	/* Check the session-local cache */
	if (fx_cache.valid &&
		pg_strcasecmp(fx_cache.from_currency, money_source_currency) == 0 &&
		pg_strcasecmp(fx_cache.to_currency, target) == 0 &&
		(GetCurrentTimestamp() - fx_cache.fetch_time) < FX_CACHE_TTL_USEC)
	{
		return fx_cache.rate;
	}

#ifdef USE_LIBCURL
	{
		float8		rate;

		if (fetch_exchange_rate(money_source_currency, target, &rate))
		{
			strlcpy(fx_cache.from_currency, money_source_currency, 4);
			strlcpy(fx_cache.to_currency, target, 4);
			fx_cache.rate = rate;
			fx_cache.fetch_time = GetCurrentTimestamp();
			fx_cache.valid = true;

			ereport(DEBUG1,
					(errmsg("fetched exchange rate %s/%s = %g",
							money_source_currency, target, rate)));

			return rate;
		}

		ereport(WARNING,
				(errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
				 errmsg("could not fetch exchange rate from %s to %s",
						money_source_currency, target),
				 errhint("The exchange rate service may be unavailable. "
						 "Displaying unconverted value.")));
	}
#endif							/* USE_LIBCURL */

	return 1.0;
}
