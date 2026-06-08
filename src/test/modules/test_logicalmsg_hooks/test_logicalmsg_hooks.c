/*--------------------------------------------------------------------------
 *
 * test_logicalmsg_hooks.c
 *		Code for testing LogicalRepMessageHandle_hook
 *
 * The handler records every logical decoding message it receives into the
 * table public.test_logicalmsg_log, so that tests can assert on the contents
 * of a table rather than on the server log. Recording the messages this way
 * also exercises the transactional behavior of the hook: a transactional
 * message is recorded as part of the remote transaction that emitted it, and
 * therefore disappears together with it if that transaction is rolled back or
 * skipped, while a non-transactional message is recorded independently.
 *
 * The messages are also logged, which lets a test tell "the handler ran but
 * its work was rolled back" apart from "the handler was never called".
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/test_logicalmsg_hooks/test_logicalmsg_hooks.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "nodes/makefuncs.h"
#include "replication/worker_internal.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"

PG_MODULE_MAGIC;

#define LOG_SCHEMA	"public"
#define LOG_TABLE	"test_logicalmsg_log"

static void test_logical_message_handler(LogicalRepMessageData *msg);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	LogicalRepMessageHandle_hook = &test_logical_message_handler;
}

static void
test_logical_message_handler(LogicalRepMessageData *msg)
{
	RangeVar   *rv;
	Oid			argtypes[5] = {LSNOID, BOOLOID, TEXTOID, INT4OID, TEXTOID};
	Datum		values[5];
	int			ret;

	ereport(LOG,
			(errmsg("received message: LSN %X/%08X, prefix: %s, message: %s, transactional: %d",
					LSN_FORMAT_ARGS(msg->lsn),
					msg->prefix, msg->message, msg->transactional)));

	/*
	 * The log table is created by the test, not by this module, so it may not
	 * exist yet. Skip recording rather than throwing an error, which would
	 * put the apply worker into a restart loop and make the test hang instead
	 * of fail. No lock is needed for this probe; the INSERT below takes the
	 * proper locks.
	 */
	rv = makeRangeVar(LOG_SCHEMA, LOG_TABLE, -1);
	if (!OidIsValid(RangeVarGetRelid(rv, NoLock, true)))
		return;

	/*
	 * Pass the message as query parameters rather than interpolating it into
	 * the query text.  Message contents come from the publisher and are not
	 * to be trusted, and the payload may contain characters that would need
	 * quoting.  Note also that message_size, not strlen(), is authoritative
	 * for the payload length.
	 */
	values[0] = LSNGetDatum(msg->lsn);
	values[1] = BoolGetDatum(msg->transactional);
	values[2] = CStringGetTextDatum(msg->prefix);
	values[3] = Int32GetDatum((int32) msg->message_size);
	values[4] = PointerGetDatum(cstring_to_text_with_len(msg->message,
														 msg->message_size));

	SPI_connect();

	/*
	 * The apply worker runs with an empty search_path, so the table name must
	 * be schema-qualified.
	 */
	ret = SPI_execute_with_args("INSERT INTO " LOG_SCHEMA "." LOG_TABLE
								" (lsn, transactional, prefix, message_size, message)"
								" VALUES ($1, $2, $3, $4, $5)",
								5, argtypes, values, NULL, false, 0);
	if (ret != SPI_OK_INSERT)
		elog(ERROR, "could not record logical message: SPI returned %d", ret);

	SPI_finish();
}
