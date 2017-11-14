/*-------------------------------------------------------------------------
 *
 * pg_buffercache_pages.c
 *	  display some contents of the buffer cache
 *
 *	  contrib/pg_buffercache/pg_buffercache_pages.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"

#define NUM_BUFFERCACHE_PAGES_MIN_ELEM		8
#define NUM_BUFFERCACHE_PAGES_ELEM		9
#define NUM_BUFFERCACHE_PAGES_VERSION_1_4_ELEM	10

#define NUM_TYPE_OF_STATE			10
#define LENGTH_OF_STATE_NAME                    25
PG_MODULE_MAGIC;

/*
 * Record structure holding the to be exposed cache data.
 */
typedef struct
{
	uint32		bufferid;
	Oid			relfilenode;
	Oid			reltablespace;
	Oid			reldatabase;
	ForkNumber	forknum;
	BlockNumber blocknum;
	bool		isvalid;
	bool		isdirty;
	uint16		usagecount;

	/*
	 * An int32 is sufficiently large, as MAX_BACKENDS prevents a buffer from
	 * being pinned by too many backends and each backend will only pin once
	 * because of bufmgr.c's PrivateRefCount infrastructure.
	 */
	int32		pinning_backends;

	uint32		buffer_state;
} BufferCachePagesRec;


/*
 * Function context for data persisting over repeated calls.
 */
typedef struct
{
	TupleDesc	tupdesc;
	BufferCachePagesRec *record;
} BufferCachePagesContext;

struct BufferStateInfomation
{
    uint32	state_value;
    const char	state_name[LENGTH_OF_STATE_NAME];
};

/*
 * Function returning data from the shared buffer cache - buffer number,
 * relation node/tablespace/database/blocknum and dirty indicator.
 */
PG_FUNCTION_INFO_V1(pg_buffercache_pages);

Datum
pg_buffercache_pages(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Datum		result;
	MemoryContext oldcontext;
	BufferCachePagesContext *fctx;	/* User function context. */
	TupleDesc	tupledesc;
	TupleDesc	expected_tupledesc;
	HeapTuple	tuple;

	if (SRF_IS_FIRSTCALL())
	{
		int			i;

		funcctx = SRF_FIRSTCALL_INIT();

		/* Switch context when allocating stuff to be used in later calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Create a user function context for cross-call persistence */
		fctx = (BufferCachePagesContext *) palloc(sizeof(BufferCachePagesContext));

		/*
		 * To smoothly support upgrades from version 1.0 of this extension
		 * transparently handle the (non-)existence of the pinning_backends
		 * column. We unfortunately have to get the result type for that... -
		 * we can't use the result type determined by the function definition
		 * without potentially crashing when somebody uses the old (or even
		 * wrong) function definition though.
		 */
		if (get_call_result_type(fcinfo, NULL, &expected_tupledesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		if (expected_tupledesc->natts < NUM_BUFFERCACHE_PAGES_MIN_ELEM ||
			expected_tupledesc->natts > NUM_BUFFERCACHE_PAGES_VERSION_1_4_ELEM)
			elog(ERROR, "incorrect number of output arguments");

		/* Construct a tuple descriptor for the result rows. */
		tupledesc = CreateTemplateTupleDesc(expected_tupledesc->natts, false);
		TupleDescInitEntry(tupledesc, (AttrNumber) 1, "bufferid",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 2, "relfilenode",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 3, "reltablespace",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 4, "reldatabase",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 5, "relforknumber",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 6, "relblocknumber",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 7, "isdirty",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 8, "usage_count",
						   INT2OID, -1, 0);

		if (expected_tupledesc->natts == NUM_BUFFERCACHE_PAGES_ELEM ||
		    expected_tupledesc->natts == NUM_BUFFERCACHE_PAGES_VERSION_1_4_ELEM)
			TupleDescInitEntry(tupledesc, (AttrNumber) 9, "pinning_backends",
							   INT4OID, -1, 0);

		if (expected_tupledesc->natts == NUM_BUFFERCACHE_PAGES_VERSION_1_4_ELEM)
                            TupleDescInitEntry(tupledesc, (AttrNumber) 10, "buffer_state",
							    INT8OID, -1, 0);
		fctx->tupdesc = BlessTupleDesc(tupledesc);
		/* Allocate NBuffers worth of BufferCachePagesRec records. */
		fctx->record = (BufferCachePagesRec *)
			MemoryContextAllocHuge(CurrentMemoryContext,
								   sizeof(BufferCachePagesRec) * NBuffers);

		/* Set max calls and remember the user function context. */
		funcctx->max_calls = NBuffers;
		funcctx->user_fctx = fctx;

		/* Return to original context when allocating transient memory */
		MemoryContextSwitchTo(oldcontext);

		/*
		 * Scan through all the buffers, saving the relevant fields in the
		 * fctx->record structure.
		 *
		 * We don't hold the partition locks, so we don't get a consistent
		 * snapshot across all buffers, but we do grab the buffer header
		 * locks, so the information of each buffer is self-consistent.
		 */
		for (i = 0; i < NBuffers; i++)
		{
			BufferDesc *bufHdr;
			uint32		buf_state;

			bufHdr = GetBufferDescriptor(i);
			/* Lock each buffer header before inspecting. */
			buf_state = LockBufHdr(bufHdr);

			fctx->record[i].bufferid = BufferDescriptorGetBuffer(bufHdr);
			fctx->record[i].relfilenode = bufHdr->tag.rnode.relNode;
			fctx->record[i].reltablespace = bufHdr->tag.rnode.spcNode;
			fctx->record[i].reldatabase = bufHdr->tag.rnode.dbNode;
			fctx->record[i].forknum = bufHdr->tag.forkNum;
			fctx->record[i].blocknum = bufHdr->tag.blockNum;
			fctx->record[i].usagecount = BUF_STATE_GET_USAGECOUNT(buf_state);
			fctx->record[i].pinning_backends = BUF_STATE_GET_REFCOUNT(buf_state);
			fctx->record[i].buffer_state = buf_state;

			if (buf_state & BM_DIRTY)
				fctx->record[i].isdirty = true;
			else
				fctx->record[i].isdirty = false;

			/* Note if the buffer is valid, and has storage created */
			if ((buf_state & BM_VALID) && (buf_state & BM_TAG_VALID))
				fctx->record[i].isvalid = true;
			else
				fctx->record[i].isvalid = false;

			UnlockBufHdr(bufHdr, buf_state);
		}
	}

	funcctx = SRF_PERCALL_SETUP();

	/* Get the saved state */
	fctx = funcctx->user_fctx;
	if (funcctx->call_cntr < funcctx->max_calls)
	{
		uint32		i = funcctx->call_cntr;
		Datum		values[NUM_BUFFERCACHE_PAGES_VERSION_1_4_ELEM];
		bool		nulls[NUM_BUFFERCACHE_PAGES_VERSION_1_4_ELEM];

		values[0] = Int32GetDatum(fctx->record[i].bufferid);
		nulls[0] = false;

		/*
		 * Set all fields except the bufferid to null if the buffer is unused
		 * or not valid.
		 */
		if (fctx->record[i].blocknum == InvalidBlockNumber ||
			fctx->record[i].isvalid == false)
		{
			nulls[1] = true;
			nulls[2] = true;
			nulls[3] = true;
			nulls[4] = true;
			nulls[5] = true;
			nulls[6] = true;
			nulls[7] = true;
			/* unused for v1.0 callers, but the array is always long enough */
			nulls[8] = true;
			nulls[9] = true;
		}
		else
		{
			values[1] = ObjectIdGetDatum(fctx->record[i].relfilenode);
			nulls[1] = false;
			values[2] = ObjectIdGetDatum(fctx->record[i].reltablespace);
			nulls[2] = false;
			values[3] = ObjectIdGetDatum(fctx->record[i].reldatabase);
			nulls[3] = false;
			values[4] = ObjectIdGetDatum(fctx->record[i].forknum);
			nulls[4] = false;
			values[5] = Int64GetDatum((int64) fctx->record[i].blocknum);
			nulls[5] = false;
			values[6] = BoolGetDatum(fctx->record[i].isdirty);
			nulls[6] = false;
			values[7] = Int16GetDatum(fctx->record[i].usagecount);
			nulls[7] = false;
			/* unused for v1.0 callers, but the array is always long enough */
			values[8] = Int32GetDatum(fctx->record[i].pinning_backends);
			nulls[8] = false;
			/* unused for v1.3 callers, but the array is always long enough */
			values[9] = Int64GetDatum((int64)fctx->record[i].buffer_state);
			nulls[9] = false;
		}
		/* Build and return the tuple. */
		tuple = heap_form_tuple(fctx->tupdesc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}
	else
		SRF_RETURN_DONE(funcctx);
}

PG_FUNCTION_INFO_V1(pg_buffercache_state_print);

static struct BufferStateInfomation state_list[NUM_TYPE_OF_STATE] =
{
    {(uint32)BM_LOCKED,		    "LOCKED"},
    {(uint32)BM_DIRTY,		    "DIRTY"},
    {(uint32)BM_VALID,		    "VALID"},
    {(uint32)BM_TAG_VALID,	    "TAG_VALID"},
    {(uint32)BM_IO_IN_PROGRESS,	    "IO_IN_PROGRESS"},
    {(uint32)BM_IO_ERROR,	    "IO_ERROR"},
    {(uint32)BM_JUST_DIRTIED,	    "JUST_DIRTIED"},
    {(uint32)BM_PIN_COUNT_WAITER,   "PIN_COUNT_WAITER"},
    {(uint32)BM_CHECKPOINT_NEEDED,  "CHECKPOINT_NEEDED"},
    {(uint32)BM_PERMANENT,	    "PERMANENT"},
};

Datum
pg_buffercache_state_print(PG_FUNCTION_ARGS)
{
    int64	    buffer_state = PG_GETARG_INT64(0);
    ArrayType	    *string_array;
    Datum	    *string_data;
    int		    array_size = 0;
    int		    list_num = 0;
    
    if( buffer_state == 0 )
    {
	string_array = construct_empty_array(TEXTOID);
	PG_RETURN_POINTER(string_array);
    }

    string_data = (Datum *) palloc(sizeof(Datum) * NUM_TYPE_OF_STATE );

    for( list_num = 0; list_num < NUM_TYPE_OF_STATE; list_num ++ )
    {
	if( buffer_state & state_list[list_num].state_value )
	    string_data[array_size++] = 
		CStringGetTextDatum(state_list[list_num].state_name);
    }
    
    string_array = construct_array( string_data, array_size, TEXTOID, -1, false, 'i');
	    
    pfree(string_data);
    PG_RETURN_POINTER(string_array);
}
