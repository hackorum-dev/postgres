/*
 * cash_exchange.h
 *		Automatic exchange rate conversion for the money type.
 *
 * When money_source_currency is set to an ISO 4217 currency code and
 * the session's lc_monetary implies a different currency, money values
 * are transparently converted at display time using live exchange rates.
 *
 * src/include/utils/cash_exchange.h
 */
#ifndef CASH_EXCHANGE_H
#define CASH_EXCHANGE_H

#include "postgres.h"

/* GUC variable: ISO 4217 code of stored money currency (e.g. "USD") */
extern PGDLLIMPORT char *money_source_currency;

/* Returns the exchange rate to apply in cash_out(); 1.0 means no conversion */
extern float8 cash_exchange_rate(void);

#endif							/* CASH_EXCHANGE_H */
