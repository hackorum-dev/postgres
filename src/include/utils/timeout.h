/*-------------------------------------------------------------------------
 *
 * timeout.h
 *	  Routines to multiplex SIGALRM interrupts for multiple timeout reasons.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/timeout.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TIMEOUT_H
#define TIMEOUT_H

#include "datatype/timestamp.h"

/*
 * Identifiers for timeout reasons.  Note that in case multiple timeouts
 * trigger at the same time, they are serviced in the order of this enum.
 */
typedef int TimeoutId;

	/* Predefined timeout reasons */
const int STARTUP_PACKET_TIMEOUT = 1;
const int DEADLOCK_TIMEOUT = 2;
const int LOCK_TIMEOUT = 3;
const int STATEMENT_TIMEOUT = 4;
const int STANDBY_DEADLOCK_TIMEOUT = 5;
const int STANDBY_TIMEOUT = 6;
const int STANDBY_LOCK_TIMEOUT = 7;
const int IDLE_IN_TRANSACTION_SESSION_TIMEOUT = 8;
	/* First user-definable timeout reason */
const int USER_TIMEOUT = 9;
	/* Maximum number of timeout reasons */
const int MAX_TIMEOUTS = 16;

/* callback function signature */
typedef void (*timeout_handler_proc) (void);

/*
 * Parameter structure for setting multiple timeouts at once
 */
typedef enum TimeoutType
{
	TMPARAM_AFTER,
	TMPARAM_AT
} TimeoutType;

typedef struct
{
	TimeoutId	id;				/* timeout to set */
	TimeoutType type;			/* TMPARAM_AFTER or TMPARAM_AT */
	int			delay_ms;		/* only used for TMPARAM_AFTER */
	TimestampTz fin_time;		/* only used for TMPARAM_AT */
} EnableTimeoutParams;

/*
 * Parameter structure for clearing multiple timeouts at once
 */
typedef struct
{
	TimeoutId	id;				/* timeout to clear */
	bool		keep_indicator; /* keep the indicator flag? */
} DisableTimeoutParams;

/* timeout setup */
extern void InitializeTimeouts(void);
extern TimeoutId RegisterTimeout(TimeoutId id, timeout_handler_proc handler);
extern void reschedule_timeouts(void);

/* timeout operation */
extern void enable_timeout_after(TimeoutId id, int delay_ms);
extern void enable_timeout_at(TimeoutId id, TimestampTz fin_time);
extern void enable_timeouts(const EnableTimeoutParams *timeouts, int count);
extern void disable_timeout(TimeoutId id, bool keep_indicator);
extern void disable_timeouts(const DisableTimeoutParams *timeouts, int count);
extern void disable_all_timeouts(bool keep_indicators);

/* accessors */
extern bool get_timeout_indicator(TimeoutId id, bool reset_indicator);
extern TimestampTz get_timeout_start_time(TimeoutId id);
extern TimestampTz get_timeout_finish_time(TimeoutId id);

#endif   /* TIMEOUT_H */
