#include "postgres.h"
#include "fmgr.h"
#include "foreign/foreign.h"
#include "nodes/makefuncs.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(dummy_user_mapping_handler);

Datum
dummy_user_mapping_handler(PG_FUNCTION_ARGS)
{
	UserMapping *um = (UserMapping *)PG_GETARG_POINTER(0);
	List *options = NIL;

	/* handler can use the options from catalog */
	(void)um->options;

	options = lappend(options, makeDefElem("user", (Node *)makeString("dummy_test"), -1));
	options = lappend(options, makeDefElem("password", (Node *)makeString("123"), -1));

	PG_RETURN_POINTER(options);
}