/*-------------------------------------------------------------------------
 * worker.c
 *	   PostgreSQL logical replication worker (apply)
 *
 * Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/worker.c
 *
 * NOTES
 *	  This file contains the worker which applies logical changes as they come
 *	  from remote logical replication stream.
 *
 *	  The main worker (apply) is started by logical replication worker
 *	  launcher for every enabled subscription in a database. It uses
 *	  walsender protocol to communicate with publisher.
 *
 *	  This module includes server facing code and shares libpqwalreceiver
 *	  module with walreceiver for providing the libpq specific functionality.
 *
 *
 * STREAMED TRANSACTIONS
 * ---------------------
 * Streamed transactions (large transactions exceeding a memory limit on the
 * upstream) are applied using one of two approaches:
 *
 * 1) Write to temporary files and apply when the final commit arrives
 *
 * This approach is used when the user has set the subscription's streaming
 * option as on.
 *
 * Unlike the regular (non-streamed) case, handling streamed transactions has
 * to handle aborts of both the toplevel transaction and subtransactions. This
 * is achieved by tracking offsets for subtransactions, which is then used
 * to truncate the file with serialized changes.
 *
 * The files are placed in tmp file directory by default, and the filenames
 * include both the XID of the toplevel transaction and OID of the
 * subscription. This is necessary so that different workers processing a
 * remote transaction with the same XID doesn't interfere.
 *
 * We use BufFiles instead of using normal temporary files because (a) the
 * BufFile infrastructure supports temporary files that exceed the OS file size
 * limit, (b) provides a way for automatic clean up on the error and (c) provides
 * a way to survive these files across local transactions and allow to open and
 * close at stream start and close. We decided to use FileSet
 * infrastructure as without that it deletes the files on the closure of the
 * file and if we decide to keep stream files open across the start/stop stream
 * then it will consume a lot of memory (more than 8K for each BufFile and
 * there could be multiple such BufFiles as the subscriber could receive
 * multiple start/stop streams for different transactions before getting the
 * commit). Moreover, if we don't use FileSet then we also need to invent
 * a new way to pass filenames to BufFile APIs so that we are allowed to open
 * the file we desired across multiple stream-open calls for the same
 * transaction.
 *
 * 2) Parallel apply workers.
 *
 * This approach is used when the user has set the subscription's streaming
 * option as parallel. See logical/applyparallelworker.c for information about
 * this approach.
 *
 * TWO_PHASE TRANSACTIONS
 * ----------------------
 * Two phase transactions are replayed at prepare and then committed or
 * rolled back at commit prepared and rollback prepared respectively. It is
 * possible to have a prepared transaction that arrives at the apply worker
 * when the tablesync is busy doing the initial copy. In this case, the apply
 * worker skips all the prepared operations [e.g. inserts] while the tablesync
 * is still busy (see the condition of should_apply_changes_for_rel). The
 * tablesync worker might not get such a prepared transaction because say it
 * was prior to the initial consistent point but might have got some later
 * commits. Now, the tablesync worker will exit without doing anything for the
 * prepared transaction skipped by the apply worker as the sync location for it
 * will be already ahead of the apply worker's current location. This would lead
 * to an "empty prepare", because later when the apply worker does the commit
 * prepare, there is nothing in it (the inserts were skipped earlier).
 *
 * To avoid this, and similar prepare confusions the subscription's two_phase
 * commit is enabled only after the initial sync is over. The two_phase option
 * has been implemented as a tri-state with values DISABLED, PENDING, and
 * ENABLED.
 *
 * Even if the user specifies they want a subscription with two_phase = on,
 * internally it will start with a tri-state of PENDING which only becomes
 * ENABLED after all tablesync initializations are completed - i.e. when all
 * tablesync workers have reached their READY state. In other words, the value
 * PENDING is only a temporary state for subscription start-up.
 *
 * Until the two_phase is properly available (ENABLED) the subscription will
 * behave as if two_phase = off. When the apply worker detects that all
 * tablesyncs have become READY (while the tri-state was PENDING) it will
 * restart the apply worker process. This happens in
 * process_syncing_tables_for_apply.
 *
 * When the (re-started) apply worker finds that all tablesyncs are READY for a
 * two_phase tri-state of PENDING it start streaming messages with the
 * two_phase option which in turn enables the decoding of two-phase commits at
 * the publisher. Then, it updates the tri-state value from PENDING to ENABLED.
 * Now, it is possible that during the time we have not enabled two_phase, the
 * publisher (replication server) would have skipped some prepares but we
 * ensure that such prepares are sent along with commit prepare, see
 * ReorderBufferFinishPrepared.
 *
 * If the subscription has no tables then a two_phase tri-state PENDING is
 * left unchanged. This lets the user still do an ALTER SUBSCRIPTION REFRESH
 * PUBLICATION which might otherwise be disallowed (see below).
 *
 * If ever a user needs to be aware of the tri-state value, they can fetch it
 * from the pg_subscription catalog (see column subtwophasestate).
 *
 * We don't allow to toggle two_phase option of a subscription because it can
 * lead to an inconsistent replica. Consider, initially, it was on and we have
 * received some prepare then we turn it off, now at commit time the server
 * will send the entire transaction data along with the commit. With some more
 * analysis, we can allow changing this option from off to on but not sure if
 * that alone would be useful.
 *
 * Finally, to avoid problems mentioned in previous paragraphs from any
 * subsequent (not READY) tablesyncs (need to toggle two_phase option from 'on'
 * to 'off' and then again back to 'on') there is a restriction for
 * ALTER SUBSCRIPTION REFRESH PUBLICATION. This command is not permitted when
 * the two_phase tri-state is ENABLED, except when copy_data = false.
 *
 * We can get prepare of the same GID more than once for the genuine cases
 * where we have defined multiple subscriptions for publications on the same
 * server and prepared transaction has operations on tables subscribed to those
 * subscriptions. For such cases, if we use the GID sent by publisher one of
 * the prepares will be successful and others will fail, in which case the
 * server will send them again. Now, this can lead to a deadlock if user has
 * set synchronous_standby_names for all the subscriptions on subscriber. To
 * avoid such deadlocks, we generate a unique GID (consisting of the
 * subscription oid and the xid of the prepared transaction) for each prepare
 * transaction on the subscriber.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/table.h"
#include "access/tableam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_tablespace.h"
#include "common/file_utils.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/execPartition.h"
#include "executor/nodeModifyTable.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_relation.h"
#include "pgstat.h"
#include "port/pg_crc32c.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "postmaster/walwriter.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/logicallauncher.h"
#include "replication/logicalproto.h"
#include "replication/logicalrelation.h"
#include "replication/logicalworker.h"
#include "replication/origin.h"
#include "replication/reorderbuffer.h"
#include "replication/snapbuild.h"
#include "replication/walreceiver.h"
#include "replication/worker_internal.h"
#include "rewrite/rewriteHandler.h"
#include "storage/buffile.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/dynahash.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/syscache.h"
#include "utils/timeout.h"
#include "utils/usercontext.h"

#define NAPTIME_PER_CYCLE 1000	/* max sleep time between cycles (1s) */

typedef struct FlushPosition
{
	dlist_node	node;
	XLogRecPtr	local_end;
	XLogRecPtr	remote_end;
} FlushPosition;

static dlist_head lsn_mapping = DLIST_STATIC_INIT(lsn_mapping);

typedef struct ApplyExecutionData
{
	EState	   *estate;			/* executor state, used to track resources */

	LogicalRepRelMapEntry *targetRel;	/* replication target rel */
	ResultRelInfo *targetRelInfo;	/* ResultRelInfo for same */

	/* These fields are used when the target relation is partitioned: */
	ModifyTableState *mtstate;	/* dummy ModifyTable state */
	PartitionTupleRouting *proute;	/* partition routing info */
} ApplyExecutionData;

/* Struct for saving and restoring apply errcontext information */
typedef struct ApplyErrorCallbackArg
{
	LogicalRepMsgType command;	/* 0 if invalid */
	LogicalRepRelMapEntry *rel;

	/* Remote node information */
	int			remote_attnum;	/* -1 if invalid */
	TransactionId remote_xid;
	XLogRecPtr	finish_lsn;
	char	   *origin_name;
} ApplyErrorCallbackArg;

/*
 * The action to be taken for the changes in the transaction.
 *
 * TRANS_LEADER_APPLY:
 * This action means that we are in the leader apply worker or table sync
 * worker. The changes of the transaction are either directly applied or
 * are read from temporary files (for streaming transactions) and then
 * applied by the worker.
 *
 * TRANS_LEADER_SERIALIZE:
 * This action means that we are in the leader apply worker or table sync
 * worker. Changes are written to temporary files and then applied when the
 * final commit arrives.
 *
 * TRANS_LEADER_SEND_TO_PARALLEL:
 * This action means that we are in the leader apply worker and need to send
 * the changes to the parallel apply worker.
 *
 * TRANS_LEADER_PARTIAL_SERIALIZE:
 * This action means that we are in the leader apply worker and have sent some
 * changes directly to the parallel apply worker and the remaining changes are
 * serialized to a file, due to timeout while sending data. The parallel apply
 * worker will apply these serialized changes when the final commit arrives.
 *
 * We can't use TRANS_LEADER_SERIALIZE for this case because, in addition to
 * serializing changes, the leader worker also needs to serialize the
 * STREAM_XXX message to a file, and wait for the parallel apply worker to
 * finish the transaction when processing the transaction finish command. So
 * this new action was introduced to keep the code and logic clear.
 *
 * TRANS_PARALLEL_APPLY:
 * This action means that we are in the parallel apply worker and changes of
 * the transaction are applied directly by the worker.
 */
typedef enum
{
	/* The action for non-streaming transactions. */
	TRANS_LEADER_APPLY,

	/* Actions for streaming transactions. */
	TRANS_LEADER_SERIALIZE,
	TRANS_LEADER_SEND_TO_PARALLEL,
	TRANS_LEADER_PARTIAL_SERIALIZE,
	TRANS_PARALLEL_APPLY
} TransApplyAction;

/* errcontext tracker */
ApplyErrorCallbackArg apply_error_callback_arg =
{
	.command = 0,
	.rel = NULL,
	.remote_attnum = -1,
	.remote_xid = InvalidTransactionId,
	.finish_lsn = InvalidXLogRecPtr,
	.origin_name = NULL,
};

ErrorContextCallback *apply_error_context_stack = NULL;

MemoryContext ApplyMessageContext = NULL;
MemoryContext ApplyContext = NULL;

/* per stream context for streaming transactions */
static MemoryContext LogicalStreamingContext = NULL;

WalReceiverConn *LogRepWorkerWalRcvConn = NULL;

Subscription *MySubscription = NULL;
static bool MySubscriptionValid = false;

static List *on_commit_wakeup_workers_subids = NIL;

bool		in_remote_transaction = false;
static XLogRecPtr remote_final_lsn = InvalidXLogRecPtr;

/* fields valid only when processing streamed transaction */
static bool in_streamed_transaction = false;

static TransactionId stream_xid = InvalidTransactionId;

/*
 * The number of changes applied by parallel apply worker during one streaming
 * block.
 */
static uint32 parallel_stream_nchanges = 0;

/*
 * We enable skipping all data modification changes (INSERT, UPDATE, etc.) for
 * the subscription if the remote transaction's finish LSN matches the subskiplsn.
 * Once we start skipping changes, we don't stop it until we skip all changes of
 * the transaction even if pg_subscription is updated and MySubscription->skiplsn
 * gets changed or reset during that. Also, in streaming transaction cases (streaming = on),
 * we don't skip receiving and spooling the changes since we decide whether or not
 * to skip applying the changes when starting to apply changes. The subskiplsn is
 * cleared after successfully skipping the transaction or applying non-empty
 * transaction. The latter prevents the mistakenly specified subskiplsn from
 * being left. Note that we cannot skip the streaming transactions when using
 * parallel apply workers because we cannot get the finish LSN before applying
 * the changes. So, we don't start parallel apply worker when finish LSN is set
 * by the user.
 */
static XLogRecPtr skip_xact_finish_lsn = InvalidXLogRecPtr;
#define is_skipping_changes() (unlikely(!XLogRecPtrIsInvalid(skip_xact_finish_lsn)))

/* BufFile handle of the current streaming file */
static BufFile *stream_fd = NULL;

typedef struct SubXactInfo
{
	TransactionId xid;			/* XID of the subxact */
	int			fileno;			/* file number in the buffile */
	off_t		offset;			/* offset in the file */
} SubXactInfo;

/* Sub-transaction data for the current streaming transaction */
typedef struct ApplySubXactData
{
	uint32		nsubxacts;		/* number of sub-transactions */
	uint32		nsubxacts_max;	/* current capacity of subxacts */
	TransactionId subxact_last; /* xid of the last sub-transaction */
	SubXactInfo *subxacts;		/* sub-xact offset in changes file */
} ApplySubXactData;

static ApplySubXactData subxact_data = {0, 0, InvalidTransactionId, NULL};

/* XXX macros for time-delayed logical replicaiton */

/* DELAYED_DIR stores files that contains changes of delayed transactions. */
#define DELAYED_DIR	"pg_logical/delayed_txns"

/*
 * The filename consists of the following, dash separated, components:
 * 1) subscription oid
 * 2) xid of delayed transaction on publisher
 * 3) status of the delaying transaction
 * 4) upper 32bit of the commit_lsn
 * 5) lower 32bit of the commit_lsn
 * 6) upper 32bit of the end_lsn
 * 7) lower 32bit of the end_lsn
 * 8) committime
 */
#define DELAYED_FORMAT "delayed-%x-%x-%c-%X-%X-%X-%X-" INT64_FORMAT
#define DELAYED_TXN_COMMITTED 'c'
#define DELAYED_TXN_PREPARED 'p'
#define DELAYED_TXN_UNKNOWN 'u'

#define DELAY_MAGIC		((uint32) 0xEE816C) /* format identifier */


/* List entry to map xid and commit time */
typedef struct DelayedTxnListEntry
{
	TransactionId xid;
	LogicalRepCommitData commit_data;
}			DelayedTxnListEntry;

/*
 * Replication message on-disk data structure
 */
typedef struct ReplicationMessageOnDisk
{
	/* Data not covered by checksum */
	uint32		magic;
	pg_crc32c	checksum;

	/* Data covered by checksum */
	int			length;			/* length of actual message, action is not
								 * included */
	char		action;

	/* Actual message follows, it is also covered by checksum */
} ReplicationMessageOnDisk;

/* Size of the part not covered by the checksum */
#define ReplicationMessageOnDiskNotChecksummedSize \
	offsetof(ReplicationMessageOnDisk, length)
/* Size of the part covered by the checksum */
#define ReplicationMessageOnDiskChecksummedSize \
	sizeof(ReplicationMessageOnDisk) - ReplicationMessageOnDiskNotChecksummedSize

/*
 * An entry is appended when the we receives commit message and time-delayed
 * logical replication is requested. The entry will be deleted after contents
 * are applied.
 */
static List *DelayedTxnList = NIL;

/* fields valid only when time-delayed logical replication is requested */
static bool in_delayed_transaction = false;

static TransactionId delayed_xid = InvalidTransactionId;

/*
 * Store flushed lsn for time-delayed logical replication. This is used when
 * we send a feedback message to the publisher.
 */
static XLogRecPtr last_flushed = InvalidXLogRecPtr;

/*
 * FIXME: global file descriptor may be not sufficient. There is a possibility
 * that non-streaming transactions are come concurrently. At that time
 * create_delay_file() for the second transaction will be failed...
 */
static int	delayed_fd = -1;

static inline void subxact_filename(char *path, Oid subid, TransactionId xid);
static inline void changes_filename(char *path, Oid subid, TransactionId xid);

/*
 * Information about subtransactions of a given toplevel transaction.
 */
static void subxact_info_write(Oid subid, TransactionId xid);
static void subxact_info_read(Oid subid, TransactionId xid);
static void subxact_info_add(TransactionId xid);
static inline void cleanup_subxact_info(void);

/*
 * Serialize and deserialize changes for a toplevel transaction.
 */
static void stream_open_file(Oid subid, TransactionId xid,
							 bool first_segment);
static void stream_write_change(char action, StringInfo s);
static void stream_open_and_write_change(TransactionId xid, char action, StringInfo s);
static void stream_close_file(void);

static void send_feedback(XLogRecPtr recvpos, bool force, bool requestReply);

static void DisableSubscriptionAndExit(void);

static void apply_handle_commit_internal(LogicalRepCommitData *commit_data);
static void apply_handle_insert_internal(ApplyExecutionData *edata,
										 ResultRelInfo *relinfo,
										 TupleTableSlot *remoteslot);
static void apply_handle_update_internal(ApplyExecutionData *edata,
										 ResultRelInfo *relinfo,
										 TupleTableSlot *remoteslot,
										 LogicalRepTupleData *newtup,
										 Oid localindexoid);
static void apply_handle_delete_internal(ApplyExecutionData *edata,
										 ResultRelInfo *relinfo,
										 TupleTableSlot *remoteslot,
										 Oid localindexoid);
static bool FindReplTupleInLocalRel(EState *estate, Relation localrel,
									LogicalRepRelation *remoterel,
									Oid localidxoid,
									TupleTableSlot *remoteslot,
									TupleTableSlot **localslot);
static void apply_handle_tuple_routing(ApplyExecutionData *edata,
									   TupleTableSlot *remoteslot,
									   LogicalRepTupleData *newtup,
									   CmdType operation);

/* Compute GID for two_phase transactions */
static void TwoPhaseTransactionGid(Oid subid, TransactionId xid, char *gid, int szgid);

/* Functions for skipping changes */
static void maybe_start_skipping_changes(XLogRecPtr finish_lsn);
static void stop_skipping_changes(void);
static void clear_subscription_skip_lsn(XLogRecPtr finish_lsn);

/* Functions for apply error callback */
static inline void set_apply_error_context_xact(TransactionId xid, XLogRecPtr lsn);
static inline void reset_apply_error_context_info(void);

static TransApplyAction get_transaction_apply_action(TransactionId xid,
													 ParallelApplyWorkerInfo **winfo);

static void begin_replication_step(void);
static void end_replication_step(void);

/* Functions for time-delayed logical replicaiton */
static void cache_commit_data(LogicalRepCommitData *commit_data, TransactionId xid);
static void flush_delayed_changes(LogicalRepCommitData *commit_data);
static void delay_file_name(char *path, Oid subid, TransactionId xid,
							char status, XLogRecPtr commit_lsn,
							XLogRecPtr end_lsn, TimestampTz committime);
static bool is_given_transaction_delayed(Oid subid, TransactionId xid);
static void create_delay_file(TransactionId xid);
static bool handle_delayed_transaction(char action, StringInfo s);

/*
 * Cache commit_data into the list
 */
static void
cache_commit_data(LogicalRepCommitData *commit_data, TransactionId xid)
{
	MemoryContext old;
	DelayedTxnListEntry *entry;

	old = MemoryContextSwitchTo(ApplyContext);

	entry = palloc0(sizeof(DelayedTxnListEntry));

	/* Contruct an entry and append it */
	entry->xid = xid;
	memcpy(&entry->commit_data, commit_data, sizeof(LogicalRepCommitData));
	DelayedTxnList = lappend(DelayedTxnList, entry);

	MemoryContextSwitchTo(old);

	elog(DEBUG1, "transaction %u is cached", xid);

}

/*
 * Flush given changes, rename and close the file. This will be called at the
 * end of the transaction.
 */
static void
flush_delayed_changes(LogicalRepCommitData *commit_data)
{
	char		old_path[MAXPGPATH];
	char		new_path[MAXPGPATH];

	Assert(delayed_fd > 0);
	Assert(TransactionIdIsValid(delayed_xid));

	/* Cache given commit_data into the list */
	cache_commit_data(commit_data, delayed_xid);

	/*
	 * Close file. No need to flush here because it will be done in
	 * durable_rename().
	 */
	CloseTransientFile(delayed_fd);

	/* Construct old/new filename */
	delay_file_name(old_path, MyLogicalRepWorker->subid, delayed_xid,
					DELAYED_TXN_UNKNOWN, InvalidXLogRecPtr, InvalidXLogRecPtr,
					0);
	delay_file_name(new_path, MyLogicalRepWorker->subid, delayed_xid,
					DELAYED_TXN_COMMITTED, commit_data->commit_lsn,
					commit_data->end_lsn, commit_data->committime);

	/* And do actual rename */
	if (durable_rename(old_path, new_path, PANIC))
		abort();

	/* Store flushed lsn */
	last_flushed = commit_data->end_lsn;

	/* Cleanup */
	delayed_fd = -1;
	delayed_xid = InvalidTransactionId;
	in_delayed_transaction = false;
}

/*
 * Get formal filename from needed information
 */
static void
delay_file_name(char *path, Oid subid, TransactionId xid, char status,
				XLogRecPtr commit_lsn, XLogRecPtr end_lsn,
				TimestampTz committime)
{
	snprintf(path, MAXPGPATH, DELAYED_DIR "/" DELAYED_FORMAT, subid, xid,
			 status, LSN_FORMAT_ARGS(commit_lsn), LSN_FORMAT_ARGS(end_lsn),
			 committime);
}

/*
 * Check whether the given transaction is delayed. This is done by checking the
 * delay file.
 */
static bool
is_given_transaction_delayed(Oid subid, TransactionId xid)
{
	struct stat st;
	char		path[MAXPGPATH];

	delay_file_name(path, subid, xid, DELAYED_TXN_PREPARED, InvalidXLogRecPtr,
					InvalidXLogRecPtr, 0);

	return stat(path, &st) == 0;
}

/*
 * Apply the delayed transaction. In the function a delayed file is opened and
 * read. Apply worker applies written changes.
 */
static void
apply_delayed_transaction(TransactionId xid, LogicalRepCommitData *commit_data)
{
	StringInfoData s2;
	int			nchanges;
	char		path[MAXPGPATH];
	char	   *buffer = NULL;
	MemoryContext oldcxt;
	ResourceOwner oldowner;

	/* Make sure we have an open transaction */
	begin_replication_step();

	/*
	 * Allocate file handle and memory required to process all the messages in
	 * TopTransactionContext to avoid them getting reset after each message is
	 * processed.
	 */
	oldcxt = MemoryContextSwitchTo(TopTransactionContext);

	/* Open the spool file for the committed transaction */
	delay_file_name(path, MyLogicalRepWorker->subid, xid,
					DELAYED_TXN_COMMITTED, commit_data->commit_lsn,
					commit_data->end_lsn, commit_data->committime);
	elog(DEBUG1, "replaying changes from file \"%s\"", path);

	/*
	 * Make sure the file is owned by the toplevel transaction so that the
	 * file will not be accidentally closed when aborting a subtransaction.
	 */
	oldowner = CurrentResourceOwner;
	CurrentResourceOwner = TopTransactionResourceOwner;

	/* Open the specified file */
	delayed_fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);

	Assert(delayed_fd > 0);

	CurrentResourceOwner = oldowner;

	buffer = palloc(BLCKSZ);
	initStringInfo(&s2);

	MemoryContextSwitchTo(oldcxt);

	set_apply_error_context_xact(xid, commit_data->commit_lsn);

	remote_final_lsn = commit_data->end_lsn;

	maybe_start_skipping_changes(commit_data->commit_lsn);

	/*
	 * Make sure the handle apply_dispatch methods are aware we're in a remote
	 * transaction.
	 */
	in_remote_transaction = true;
	pgstat_report_activity(STATE_RUNNING, NULL);

	end_replication_step();

	/*
	 * Read the entries one by one and pass them through the same logic as in
	 * apply_dispatch.
	 */
	nchanges = 0;
	while (true)
	{
		ReplicationMessageOnDisk ondisk;

		CHECK_FOR_INTERRUPTS();

		/* read the on-disk record */
		if (!read(delayed_fd, &ondisk, sizeof(ondisk)))
			break;

		/* verify magic */
		if (ondisk.magic != DELAY_MAGIC)
			ereport(PANIC,
					errcode(ERRCODE_DATA_CORRUPTED),
					errmsg("delayed file \"%s\" has wrong magic number: %u instead of %u",
						   path, ondisk.magic, DELAY_MAGIC));

		buffer = repalloc(buffer, ondisk.length);
		read(delayed_fd, buffer, ondisk.length);

		/* Verify the if required */
#ifdef	USE_ASSERT_CHECKING
		{
			pg_crc32c	checksum;

			INIT_CRC32C(checksum);
			COMP_CRC32C(checksum,
						(char *) &ondisk + ReplicationMessageOnDiskNotChecksummedSize,
						ReplicationMessageOnDiskChecksummedSize);
			COMP_CRC32C(checksum, buffer, ondisk.length);
			FIN_CRC32C(checksum);

			if (!EQ_CRC32C(checksum, ondisk.checksum))
				ereport(PANIC,
						(errmsg("checksum mismatch for delayed transaction file \"%s\": is %u, should be %u",
								path, checksum, ondisk.checksum)));
		}
#endif

		/* copy the buffer to the stringinfo and call apply_dispatch */
		resetStringInfo(&s2);
		appendStringInfoChar(&s2, ondisk.action);
		appendBinaryStringInfo(&s2, buffer, ondisk.length);

		apply_dispatch(&s2);

		MemoryContextReset(ApplyMessageContext);

		MemoryContextSwitchTo(oldcxt);

		nchanges++;

		if (nchanges % 1000 == 0)
			elog(DEBUG1, "replayed %d changes from file \"%s\"",
				 nchanges, path);
	}

	CloseTransientFile(delayed_fd);
	delayed_fd = -1;

	apply_handle_commit_internal(commit_data);

	durable_unlink(path, LOG);

	elog(DEBUG1, "replayed %d (all) changes from file \"%s\"",
		 nchanges, path);

	return;
}

/*
 * Create a file that will be written changes.
 */
static void
create_delay_file(TransactionId xid)
{
	char		path[MAXPGPATH];
	int			fd;

	Assert(TransactionIdIsValid(xid));
	Assert(delayed_fd < 0);

	/*
	 * Construct filename. Other information like commit_lsn will be filled
	 * when it will be committed.
	 */
	delay_file_name(path, MyLogicalRepWorker->subid, xid, DELAYED_TXN_UNKNOWN,
					InvalidXLogRecPtr, InvalidXLogRecPtr, 0);

	elog(DEBUG1, "creating a file \"%s\" for time-delayed logical replication",
		 path);

	fd = OpenTransientFile(path, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | PG_BINARY);

	if (fd < 0)
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not create file \"%s\": %m",
					   path));

	delayed_fd = fd;
}

/*
 * Create a directory that holds delayed files
 */
static void
initialize_delay_directory(void)
{
	char		path[MAXPGPATH];

	snprintf(path, MAXPGPATH, DELAYED_DIR);
	if (MakePGDirectory(path) < 0 && errno != EEXIST)
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not create directory \"%s\": %m",
					   path));

	START_CRIT_SECTION();
	fsync_fname(path, true);
	END_CRIT_SECTION();
}

/*
 * Transform information from commit_prepared style to commit style.
 */
static void
ConstructCommitFromCommitPrepared(LogicalRepCommitData *commit,
								  LogicalRepCommitPreparedTxnData *prepare_data)
{
	commit->commit_lsn = prepare_data->commit_lsn;
	commit->committime = prepare_data->commit_time;
	commit->end_lsn = prepare_data->end_lsn;
}

/*
 * Restore the delayed transaction from given information.
 *
 * This return false only when the status is unknown, which measn that the
 * worker was shutted down before receiving the COMMIT/PREPARE/COMMIT PREPARED
 * message. In this case we must receive whole the messages and write them into
 * file again.
 */
static bool
RestoreDelayedTxn(char status, XLogRecPtr commit_lsn, XLogRecPtr end_lsn,
				  TimestampTz committime, TransactionId xid)

{
	switch (status)
	{
		case DELAYED_TXN_UNKNOWN:
			return false;

		case DELAYED_TXN_COMMITTED:
			{
				LogicalRepCommitData commit_data = {
					.commit_lsn = commit_lsn,
					.committime = committime,
					.end_lsn = end_lsn,
				};

				cache_commit_data(&commit_data, xid);
				break;
			}

		case DELAYED_TXN_PREPARED:
			/* Do nothing */
			break;

		default:
			Assert(false);
			return false;		/* Keep compiler quiet */
	}

	/* Update last_flushed to avoid to recevie same transaction again */
	last_flushed = end_lsn;

	return true;
}

/*
 * list_sort() comparator for sorting DelayedTxnList in commitime order.
 */
static int
file_sort_by_committime(const ListCell *a_p, const ListCell *b_p)
{
	DelayedTxnListEntry *a = (DelayedTxnListEntry *) lfirst(a_p);
	DelayedTxnListEntry *b = (DelayedTxnListEntry *) lfirst(b_p);

	if (a->commit_data.committime < b->commit_data.committime)
		return -1;
	else if (a->commit_data.committime > b->commit_data.committime)
		return 1;
	return 0;
}


/*
 * Restore all the delayed transactions to memory.
 */
static void
RestoreDelayedTxns(void)
{
	DIR		   *delayed_dir;
	struct dirent *delayed_de;

	/* Read all the file step-by-step */
	delayed_dir = AllocateDir(DELAYED_DIR);
	while ((delayed_de = ReadDir(delayed_dir, DELAYED_DIR)) != NULL)
	{
		Oid			subid = InvalidOid;
		TransactionId xid = InvalidTransactionId;
		char		status = 0;
		XLogRecPtr	commit_lsn = InvalidXLogRecPtr,
					end_lsn = InvalidXLogRecPtr;
		TimestampTz committime = 0;
		uint32		commit_hi = 0,
					commit_lo = 0,
					end_hi = 0,
					end_lo = 0;

		if (strcmp(delayed_de->d_name, ".") == 0 ||
			strcmp(delayed_de->d_name, "..") == 0)
			continue;

		/* Ignore files that aren't ours */
		if (strncmp(delayed_de->d_name, "delayed-", 8) != 0)
			continue;

		/* Parse filename */
		if (sscanf(delayed_de->d_name, DELAYED_FORMAT, &subid, &xid, &status, &commit_hi,
				   &commit_lo, &end_hi, &end_lo, &committime) != 8)
			elog(ERROR, "could not parse filename \"%s\"", delayed_de->d_name);

		/* Skip if the file has been generated by other subscriptions */
		if (MyLogicalRepWorker->subid != subid)
			continue;

		elog(DEBUG1, "start to restore from %s", delayed_de->d_name);

		commit_lsn = ((uint64) commit_hi) << 32 | commit_lo;
		end_lsn = ((uint64) end_hi) << 32 | end_lo;

		/*
		 * Do actual restore here. If the server was shutted down while
		 * receiving transactions, the status is UNKNOWN and
		 * RestoreDelayedTxn() returns false. At that time we must remove the
		 * file once and receive changes again.
		 */
		if (!RestoreDelayedTxn(status, commit_lsn, end_lsn, committime, xid))
		{
			char		path[MAXPGPATH];

			snprintf(path, MAXPGPATH, DELAYED_DIR "/%s", delayed_de->d_name);
			durable_unlink(path, LOG);
		}
	}
	FreeDir(delayed_dir);

	list_sort(DelayedTxnList, file_sort_by_committime);
}

/*
 * Restore delayed transactions, or initialize the directory
 */
static void
InitializeDelayedTxn(void)
{
	struct stat st;
	char		path[MAXPGPATH];

	snprintf(path, MAXPGPATH, DELAYED_DIR);

	/*
	 * If the given directory does not exist, create one. Otherwise start to
	 * restore.
	 */
	if (stat(path, &st) != 0)
	{
		initialize_delay_directory();
		return;
	}

	RestoreDelayedTxns();
}

/*
 * Write a given message to a file. This is called for every message.
 * This returns true only when changes are written into file.
 *
 * The format of the serialized changes is same as the streamed one. This
 * has a length (not including the length), action code (identifying the
 * message type) and message contents (without the subxact TransactionId
 * value).
 */
static bool
handle_delayed_transaction(char action, StringInfo s)
{
	ReplicationMessageOnDisk ondisk;
	pg_crc32c	checksum = 0;

	/* Return if we are not in delay */
	if (!in_delayed_transaction)
		return false;

	Assert(delayed_fd > 0);
	Assert(TransactionIdIsValid(delayed_xid));

	ondisk.magic = DELAY_MAGIC;
	ondisk.length = (s->len - s->cursor);
	ondisk.action = action;

	/* Calculate CRC if required */
#ifdef	USE_ASSERT_CHECKING
	INIT_CRC32C(checksum);
	COMP_CRC32C(checksum,
				(char *) &ondisk + ReplicationMessageOnDiskNotChecksummedSize,
				ReplicationMessageOnDiskChecksummedSize);
	COMP_CRC32C(checksum, &s->data[s->cursor], ondisk.length);
	FIN_CRC32C(checksum);
#endif

	ondisk.checksum = checksum;

	/* Write header part */
	if (write(delayed_fd, &ondisk, sizeof(ondisk)) != sizeof(ondisk))
	{
		int			save_errno = errno;
		char		path[MAXPGPATH];

		CloseTransientFile(delayed_fd);
		delay_file_name(path, MyLogicalRepWorker->subid, delayed_xid,
						DELAYED_TXN_UNKNOWN, InvalidXLogRecPtr,
						InvalidXLogRecPtr, 0);

		/* if write didn't set errno, assume problem is no disk space */
		errno = save_errno ? save_errno : ENOSPC;
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not write to file \"%s\": %m",
					   path));
		return false;			/* Keep compiler quiet */
	}

	/* Write actual message */
	if (write(delayed_fd, &s->data[s->cursor], ondisk.length) != ondisk.length)
	{
		int			save_errno = errno;
		char		path[MAXPGPATH];

		CloseTransientFile(delayed_fd);
		delay_file_name(path, MyLogicalRepWorker->subid, delayed_xid,
						DELAYED_TXN_UNKNOWN, InvalidXLogRecPtr,
						InvalidXLogRecPtr, 0);

		/* if write didn't set errno, assume problem is no disk space */
		errno = save_errno ? save_errno : ENOSPC;
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not write to file \"%s\": %m",
					   path));
		return false;			/* Keep compiler quiet */
	}

	return true;
}

/*
 * Check the delayed transactions and apply if we elapsed sufficient time
 */
static void
check_delayed_transaction(void)
{
	TimestampTz now;
	ListCell   *lc;
	int			n = 0;

	if (in_streamed_transaction)
		return;

	now = GetCurrentTimestamp();

	/* Read cache on-by-one */
	foreach(lc, DelayedTxnList)
	{
		DelayedTxnListEntry *entry = (DelayedTxnListEntry *) lfirst(lc);
		LogicalRepCommitData *commit_data = &entry->commit_data;
		TimestampTz delayUntil;
		long		diffms;

		delayUntil = TimestampTzPlusMilliseconds(commit_data->committime,
												 MySubscription->minapplydelay);

		diffms = TimestampDifferenceMilliseconds(now, delayUntil);

		/*
		 * The cache is aligned the commit ordering, so we do not have to
		 * check latter entries if we find transactions that should not be
		 * applied.
		 */
		if (diffms > 0)
			break;

		elog(DEBUG1, "started to apply transaction %u", entry->xid);

		apply_delayed_transaction(entry->xid, commit_data);

		delayed_xid = InvalidTransactionId;
		in_delayed_transaction = false;
		n++;
	}
	/* Discards applied entries */
	DelayedTxnList = list_delete_first_n(DelayedTxnList, n);
}

/*
 * Return the name of the logical replication worker.
 */
static const char *
get_worker_name(void)
{
	if (am_tablesync_worker())
		return _("logical replication table synchronization worker");
	else if (am_parallel_apply_worker())
		return _("logical replication parallel apply worker");
	else
		return _("logical replication apply worker");
}

/*
 * Form the origin name for the subscription.
 *
 * This is a common function for tablesync and other workers. Tablesync workers
 * must pass a valid relid. Other callers must pass relid = InvalidOid.
 *
 * Return the name in the supplied buffer.
 */
void
ReplicationOriginNameForLogicalRep(Oid suboid, Oid relid,
								   char *originname, Size szoriginname)
{
	if (OidIsValid(relid))
	{
		/* Replication origin name for tablesync workers. */
		snprintf(originname, szoriginname, "pg_%u_%u", suboid, relid);
	}
	else
	{
		/* Replication origin name for non-tablesync workers. */
		snprintf(originname, szoriginname, "pg_%u", suboid);
	}
}

/*
 * Should this worker apply changes for given relation.
 *
 * This is mainly needed for initial relation data sync as that runs in
 * separate worker process running in parallel and we need some way to skip
 * changes coming to the leader apply worker during the sync of a table.
 *
 * Note we need to do smaller or equals comparison for SYNCDONE state because
 * it might hold position of end of initial slot consistent point WAL
 * record + 1 (ie start of next record) and next record can be COMMIT of
 * transaction we are now processing (which is what we set remote_final_lsn
 * to in apply_handle_begin).
 *
 * Note that for streaming transactions that are being applied in the parallel
 * apply worker, we disallow applying changes if the target table in the
 * subscription is not in the READY state, because we cannot decide whether to
 * apply the change as we won't know remote_final_lsn by that time.
 *
 * We already checked this in pa_can_start() before assigning the
 * streaming transaction to the parallel worker, but it also needs to be
 * checked here because if the user executes ALTER SUBSCRIPTION ... REFRESH
 * PUBLICATION in parallel, the new table can be added to pg_subscription_rel
 * while applying this transaction.
 */
static bool
should_apply_changes_for_rel(LogicalRepRelMapEntry *rel)
{
	if (am_tablesync_worker())
		return MyLogicalRepWorker->relid == rel->localreloid;
	else if (am_parallel_apply_worker())
	{
		/* We don't synchronize rel's that are in unknown state. */
		if (rel->state != SUBREL_STATE_READY &&
			rel->state != SUBREL_STATE_UNKNOWN)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical replication parallel apply worker for subscription \"%s\" will stop",
							MySubscription->name),
					 errdetail("Cannot handle streamed replication transactions using parallel apply workers until all tables have been synchronized.")));

		return rel->state == SUBREL_STATE_READY;
	}
	else
		return (rel->state == SUBREL_STATE_READY ||
				(rel->state == SUBREL_STATE_SYNCDONE &&
				 rel->statelsn <= remote_final_lsn));
}

/*
 * Begin one step (one INSERT, UPDATE, etc) of a replication transaction.
 *
 * Start a transaction, if this is the first step (else we keep using the
 * existing transaction).
 * Also provide a global snapshot and ensure we run in ApplyMessageContext.
 */
static void
begin_replication_step(void)
{
	SetCurrentStatementStartTimestamp();

	if (!IsTransactionState())
	{
		StartTransactionCommand();
		maybe_reread_subscription();
	}

	PushActiveSnapshot(GetTransactionSnapshot());

	MemoryContextSwitchTo(ApplyMessageContext);
}

/*
 * Finish up one step of a replication transaction.
 * Callers of begin_replication_step() must also call this.
 *
 * We don't close out the transaction here, but we should increment
 * the command counter to make the effects of this step visible.
 */
static void
end_replication_step(void)
{
	PopActiveSnapshot();

	CommandCounterIncrement();
}

/*
 * Handle streamed transactions for both the leader apply worker and the
 * parallel apply workers.
 *
 * In the streaming case (receiving a block of the streamed transaction), for
 * serialize mode, simply redirect it to a file for the proper toplevel
 * transaction, and for parallel mode, the leader apply worker will send the
 * changes to parallel apply workers and the parallel apply worker will define
 * savepoints if needed. (LOGICAL_REP_MSG_RELATION or LOGICAL_REP_MSG_TYPE
 * messages will be applied by both leader apply worker and parallel apply
 * workers).
 *
 * Returns true for streamed transactions (when the change is either serialized
 * to file or sent to parallel apply worker), false otherwise (regular mode or
 * needs to be processed by parallel apply worker).
 *
 * Exception: If the message being processed is LOGICAL_REP_MSG_RELATION
 * or LOGICAL_REP_MSG_TYPE, return false even if the message needs to be sent
 * to a parallel apply worker.
 */
static bool
handle_streamed_transaction(LogicalRepMsgType action, StringInfo s)
{
	TransactionId current_xid;
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;
	StringInfoData original_msg;

	apply_action = get_transaction_apply_action(stream_xid, &winfo);

	/* not in streaming mode */
	if (apply_action == TRANS_LEADER_APPLY)
		return false;

	Assert(TransactionIdIsValid(stream_xid));

	/*
	 * The parallel apply worker needs the xid in this message to decide
	 * whether to define a savepoint, so save the original message that has
	 * not moved the cursor after the xid. We will serialize this message to a
	 * file in PARTIAL_SERIALIZE mode.
	 */
	original_msg = *s;

	/*
	 * We should have received XID of the subxact as the first part of the
	 * message, so extract it.
	 */
	current_xid = pq_getmsgint(s, 4);

	if (!TransactionIdIsValid(current_xid))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("invalid transaction ID in streamed replication transaction")));

	switch (apply_action)
	{
		case TRANS_LEADER_SERIALIZE:
			Assert(stream_fd);

			/* Add the new subxact to the array (unless already there). */
			subxact_info_add(current_xid);

			/* Write the change to the current file */
			stream_write_change(action, s);
			return true;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			/*
			 * XXX The publisher side doesn't always send relation/type update
			 * messages after the streaming transaction, so also update the
			 * relation/type in leader apply worker. See function
			 * cleanup_rel_sync_cache.
			 */
			if (pa_send_data(winfo, s->len, s->data))
				return (action != LOGICAL_REP_MSG_RELATION &&
						action != LOGICAL_REP_MSG_TYPE);

			/*
			 * Switch to serialize mode when we are not able to send the
			 * change to parallel apply worker.
			 */
			pa_switch_to_partial_serialize(winfo, false);

			/* fall through */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			stream_write_change(action, &original_msg);

			/* Same reason as TRANS_LEADER_SEND_TO_PARALLEL case. */
			return (action != LOGICAL_REP_MSG_RELATION &&
					action != LOGICAL_REP_MSG_TYPE);

		case TRANS_PARALLEL_APPLY:
			parallel_stream_nchanges += 1;

			/* Define a savepoint for a subxact if needed. */
			pa_start_subtrans(current_xid, stream_xid);
			return false;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			return false;		/* silence compiler warning */
	}
}

/*
 * Executor state preparation for evaluation of constraint expressions,
 * indexes and triggers for the specified relation.
 *
 * Note that the caller must open and close any indexes to be updated.
 */
static ApplyExecutionData *
create_edata_for_relation(LogicalRepRelMapEntry *rel)
{
	ApplyExecutionData *edata;
	EState	   *estate;
	RangeTblEntry *rte;
	List	   *perminfos = NIL;
	ResultRelInfo *resultRelInfo;

	edata = (ApplyExecutionData *) palloc0(sizeof(ApplyExecutionData));
	edata->targetRel = rel;

	edata->estate = estate = CreateExecutorState();

	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(rel->localrel);
	rte->relkind = rel->localrel->rd_rel->relkind;
	rte->rellockmode = AccessShareLock;

	addRTEPermissionInfo(&perminfos, rte);

	ExecInitRangeTable(estate, list_make1(rte), perminfos);

	edata->targetRelInfo = resultRelInfo = makeNode(ResultRelInfo);

	/*
	 * Use Relation opened by logicalrep_rel_open() instead of opening it
	 * again.
	 */
	InitResultRelInfo(resultRelInfo, rel->localrel, 1, NULL, 0);

	/*
	 * We put the ResultRelInfo in the es_opened_result_relations list, even
	 * though we don't populate the es_result_relations array.  That's a bit
	 * bogus, but it's enough to make ExecGetTriggerResultRel() find them.
	 *
	 * ExecOpenIndices() is not called here either, each execution path doing
	 * an apply operation being responsible for that.
	 */
	estate->es_opened_result_relations =
		lappend(estate->es_opened_result_relations, resultRelInfo);

	estate->es_output_cid = GetCurrentCommandId(true);

	/* Prepare to catch AFTER triggers. */
	AfterTriggerBeginQuery();

	/* other fields of edata remain NULL for now */

	return edata;
}

/*
 * Finish any operations related to the executor state created by
 * create_edata_for_relation().
 */
static void
finish_edata(ApplyExecutionData *edata)
{
	EState	   *estate = edata->estate;

	/* Handle any queued AFTER triggers. */
	AfterTriggerEndQuery(estate);

	/* Shut down tuple routing, if any was done. */
	if (edata->proute)
		ExecCleanupTupleRouting(edata->mtstate, edata->proute);

	/*
	 * Cleanup.  It might seem that we should call ExecCloseResultRelations()
	 * here, but we intentionally don't.  It would close the rel we added to
	 * es_opened_result_relations above, which is wrong because we took no
	 * corresponding refcount.  We rely on ExecCleanupTupleRouting() to close
	 * any other relations opened during execution.
	 */
	ExecResetTupleTable(estate->es_tupleTable, false);
	FreeExecutorState(estate);
	pfree(edata);
}

/*
 * Executes default values for columns for which we can't map to remote
 * relation columns.
 *
 * This allows us to support tables which have more columns on the downstream
 * than on the upstream.
 */
static void
slot_fill_defaults(LogicalRepRelMapEntry *rel, EState *estate,
				   TupleTableSlot *slot)
{
	TupleDesc	desc = RelationGetDescr(rel->localrel);
	int			num_phys_attrs = desc->natts;
	int			i;
	int			attnum,
				num_defaults = 0;
	int		   *defmap;
	ExprState **defexprs;
	ExprContext *econtext;

	econtext = GetPerTupleExprContext(estate);

	/* We got all the data via replication, no need to evaluate anything. */
	if (num_phys_attrs == rel->remoterel.natts)
		return;

	defmap = (int *) palloc(num_phys_attrs * sizeof(int));
	defexprs = (ExprState **) palloc(num_phys_attrs * sizeof(ExprState *));

	Assert(rel->attrmap->maplen == num_phys_attrs);
	for (attnum = 0; attnum < num_phys_attrs; attnum++)
	{
		Expr	   *defexpr;

		if (TupleDescAttr(desc, attnum)->attisdropped || TupleDescAttr(desc, attnum)->attgenerated)
			continue;

		if (rel->attrmap->attnums[attnum] >= 0)
			continue;

		defexpr = (Expr *) build_column_default(rel->localrel, attnum + 1);

		if (defexpr != NULL)
		{
			/* Run the expression through planner */
			defexpr = expression_planner(defexpr);

			/* Initialize executable expression in copycontext */
			defexprs[num_defaults] = ExecInitExpr(defexpr, NULL);
			defmap[num_defaults] = attnum;
			num_defaults++;
		}
	}

	for (i = 0; i < num_defaults; i++)
		slot->tts_values[defmap[i]] =
			ExecEvalExpr(defexprs[i], econtext, &slot->tts_isnull[defmap[i]]);
}

/*
 * Store tuple data into slot.
 *
 * Incoming data can be either text or binary format.
 */
static void
slot_store_data(TupleTableSlot *slot, LogicalRepRelMapEntry *rel,
				LogicalRepTupleData *tupleData)
{
	int			natts = slot->tts_tupleDescriptor->natts;
	int			i;

	ExecClearTuple(slot);

	/* Call the "in" function for each non-dropped, non-null attribute */
	Assert(natts == rel->attrmap->maplen);
	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(slot->tts_tupleDescriptor, i);
		int			remoteattnum = rel->attrmap->attnums[i];

		if (!att->attisdropped && remoteattnum >= 0)
		{
			StringInfo	colvalue = &tupleData->colvalues[remoteattnum];

			Assert(remoteattnum < tupleData->ncols);

			/* Set attnum for error callback */
			apply_error_callback_arg.remote_attnum = remoteattnum;

			if (tupleData->colstatus[remoteattnum] == LOGICALREP_COLUMN_TEXT)
			{
				Oid			typinput;
				Oid			typioparam;

				getTypeInputInfo(att->atttypid, &typinput, &typioparam);
				slot->tts_values[i] =
					OidInputFunctionCall(typinput, colvalue->data,
										 typioparam, att->atttypmod);
				slot->tts_isnull[i] = false;
			}
			else if (tupleData->colstatus[remoteattnum] == LOGICALREP_COLUMN_BINARY)
			{
				Oid			typreceive;
				Oid			typioparam;

				/*
				 * In some code paths we may be asked to re-parse the same
				 * tuple data.  Reset the StringInfo's cursor so that works.
				 */
				colvalue->cursor = 0;

				getTypeBinaryInputInfo(att->atttypid, &typreceive, &typioparam);
				slot->tts_values[i] =
					OidReceiveFunctionCall(typreceive, colvalue,
										   typioparam, att->atttypmod);

				/* Trouble if it didn't eat the whole buffer */
				if (colvalue->cursor != colvalue->len)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
							 errmsg("incorrect binary data format in logical replication column %d",
									remoteattnum + 1)));
				slot->tts_isnull[i] = false;
			}
			else
			{
				/*
				 * NULL value from remote.  (We don't expect to see
				 * LOGICALREP_COLUMN_UNCHANGED here, but if we do, treat it as
				 * NULL.)
				 */
				slot->tts_values[i] = (Datum) 0;
				slot->tts_isnull[i] = true;
			}

			/* Reset attnum for error callback */
			apply_error_callback_arg.remote_attnum = -1;
		}
		else
		{
			/*
			 * We assign NULL to dropped attributes and missing values
			 * (missing values should be later filled using
			 * slot_fill_defaults).
			 */
			slot->tts_values[i] = (Datum) 0;
			slot->tts_isnull[i] = true;
		}
	}

	ExecStoreVirtualTuple(slot);
}

/*
 * Replace updated columns with data from the LogicalRepTupleData struct.
 * This is somewhat similar to heap_modify_tuple but also calls the type
 * input functions on the user data.
 *
 * "slot" is filled with a copy of the tuple in "srcslot", replacing
 * columns provided in "tupleData" and leaving others as-is.
 *
 * Caution: unreplaced pass-by-ref columns in "slot" will point into the
 * storage for "srcslot".  This is OK for current usage, but someday we may
 * need to materialize "slot" at the end to make it independent of "srcslot".
 */
static void
slot_modify_data(TupleTableSlot *slot, TupleTableSlot *srcslot,
				 LogicalRepRelMapEntry *rel,
				 LogicalRepTupleData *tupleData)
{
	int			natts = slot->tts_tupleDescriptor->natts;
	int			i;

	/* We'll fill "slot" with a virtual tuple, so we must start with ... */
	ExecClearTuple(slot);

	/*
	 * Copy all the column data from srcslot, so that we'll have valid values
	 * for unreplaced columns.
	 */
	Assert(natts == srcslot->tts_tupleDescriptor->natts);
	slot_getallattrs(srcslot);
	memcpy(slot->tts_values, srcslot->tts_values, natts * sizeof(Datum));
	memcpy(slot->tts_isnull, srcslot->tts_isnull, natts * sizeof(bool));

	/* Call the "in" function for each replaced attribute */
	Assert(natts == rel->attrmap->maplen);
	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(slot->tts_tupleDescriptor, i);
		int			remoteattnum = rel->attrmap->attnums[i];

		if (remoteattnum < 0)
			continue;

		Assert(remoteattnum < tupleData->ncols);

		if (tupleData->colstatus[remoteattnum] != LOGICALREP_COLUMN_UNCHANGED)
		{
			StringInfo	colvalue = &tupleData->colvalues[remoteattnum];

			/* Set attnum for error callback */
			apply_error_callback_arg.remote_attnum = remoteattnum;

			if (tupleData->colstatus[remoteattnum] == LOGICALREP_COLUMN_TEXT)
			{
				Oid			typinput;
				Oid			typioparam;

				getTypeInputInfo(att->atttypid, &typinput, &typioparam);
				slot->tts_values[i] =
					OidInputFunctionCall(typinput, colvalue->data,
										 typioparam, att->atttypmod);
				slot->tts_isnull[i] = false;
			}
			else if (tupleData->colstatus[remoteattnum] == LOGICALREP_COLUMN_BINARY)
			{
				Oid			typreceive;
				Oid			typioparam;

				/*
				 * In some code paths we may be asked to re-parse the same
				 * tuple data.  Reset the StringInfo's cursor so that works.
				 */
				colvalue->cursor = 0;

				getTypeBinaryInputInfo(att->atttypid, &typreceive, &typioparam);
				slot->tts_values[i] =
					OidReceiveFunctionCall(typreceive, colvalue,
										   typioparam, att->atttypmod);

				/* Trouble if it didn't eat the whole buffer */
				if (colvalue->cursor != colvalue->len)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
							 errmsg("incorrect binary data format in logical replication column %d",
									remoteattnum + 1)));
				slot->tts_isnull[i] = false;
			}
			else
			{
				/* must be LOGICALREP_COLUMN_NULL */
				slot->tts_values[i] = (Datum) 0;
				slot->tts_isnull[i] = true;
			}

			/* Reset attnum for error callback */
			apply_error_callback_arg.remote_attnum = -1;
		}
	}

	/* And finally, declare that "slot" contains a valid virtual tuple */
	ExecStoreVirtualTuple(slot);
}

/*
 * Handle BEGIN message.
 */
static void
apply_handle_begin(StringInfo s)
{
	LogicalRepBeginData begin_data;

	/* There must not be an active streaming transaction. */
	Assert(!TransactionIdIsValid(stream_xid));

	logicalrep_read_begin(s, &begin_data);
	set_apply_error_context_xact(begin_data.xid, begin_data.final_lsn);

	/*
	 * Prepare to write changes into file if time-delayed replication is
	 * requested.
	 */
	if (MySubscription->minapplydelay && AllTablesyncsReady())
	{
		in_delayed_transaction = true;

		create_delay_file(begin_data.xid);

		delayed_xid = begin_data.xid;
	}
	else
	{
		remote_final_lsn = begin_data.final_lsn;

		maybe_start_skipping_changes(begin_data.final_lsn);

		in_remote_transaction = true;

		pgstat_report_activity(STATE_RUNNING, NULL);
	}
}

/*
 * Handle COMMIT message.
 *
 * TODO, support tracking of multiple origins
 */
static void
apply_handle_commit(StringInfo s)
{
	LogicalRepCommitData commit_data;

	/* Save the message before it is consumed. */
	StringInfoData original_msg = *s;

	/*
	 * If we are applying the delayed transaction, skip here. Actual COMMIT
	 * will be done in apply_delayed_transaction()
	 */
	if (delayed_fd > 0 && !in_delayed_transaction)
		return;

	logicalrep_read_commit(s, &commit_data);

	/* If we are applying, skip here. */

	if (in_delayed_transaction)
	{
		/* Write a commit message into file and flush all of messages */
		handle_delayed_transaction(LOGICAL_REP_MSG_COMMIT, &original_msg);
		flush_delayed_changes(&commit_data);
	}
	else
	{
		if (commit_data.commit_lsn != remote_final_lsn)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					errmsg_internal("incorrect commit LSN %X/%X in commit message (expected %X/%X)",
									LSN_FORMAT_ARGS(commit_data.commit_lsn),
									LSN_FORMAT_ARGS(remote_final_lsn))));

		apply_handle_commit_internal(&commit_data);

		/* Process any tables that are being synchronized in parallel. */
		process_syncing_tables(commit_data.end_lsn);
	}

	pgstat_report_activity(STATE_IDLE, NULL);
	reset_apply_error_context_info();
}

/*
 * Handle BEGIN PREPARE message.
 */
static void
apply_handle_begin_prepare(StringInfo s)
{
	LogicalRepPreparedTxnData begin_data;

	/* Tablesync should never receive prepare. */
	if (am_tablesync_worker())
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("tablesync worker received a BEGIN PREPARE message")));

	/* There must not be an active streaming transaction. */
	Assert(!TransactionIdIsValid(stream_xid));

	logicalrep_read_begin_prepare(s, &begin_data);
	set_apply_error_context_xact(begin_data.xid, begin_data.prepare_lsn);

	/*
	 * Prepare to write changes into file if time-delayed replication is
	 * requested.
	 */
	if (MySubscription->minapplydelay && AllTablesyncsReady())
	{
		in_delayed_transaction = true;

		create_delay_file(begin_data.xid);

		delayed_xid = begin_data.xid;
	}
	else
	{
		remote_final_lsn = begin_data.prepare_lsn;

		maybe_start_skipping_changes(begin_data.prepare_lsn);

		in_remote_transaction = true;

		pgstat_report_activity(STATE_RUNNING, NULL);
	}
}

/*
 * Common function to prepare the GID.
 */
static void
apply_handle_prepare_internal(LogicalRepPreparedTxnData *prepare_data)
{
	char		gid[GIDSIZE];

	/*
	 * Compute unique GID for two_phase transactions. We don't use GID of
	 * prepared transaction sent by server as that can lead to deadlock when
	 * we have multiple subscriptions from same node point to publications on
	 * the same node. See comments atop worker.c
	 */
	TwoPhaseTransactionGid(MySubscription->oid, prepare_data->xid,
						   gid, sizeof(gid));

	/*
	 * BeginTransactionBlock is necessary to balance the EndTransactionBlock
	 * called within the PrepareTransactionBlock below.
	 */
	if (!IsTransactionBlock())
	{
		BeginTransactionBlock();
		CommitTransactionCommand(); /* Completes the preceding Begin command. */
	}

	/*
	 * Update origin state so we can restart streaming from correct position
	 * in case of crash.
	 */
	replorigin_session_origin_lsn = prepare_data->end_lsn;
	replorigin_session_origin_timestamp = prepare_data->prepare_time;

	PrepareTransactionBlock(gid);
}

/*
 * Handle PREPARE message.
 *
 * When time-delayed logical replication is requested, we just write a message
 * into file and return. This means that no transaction is prepared on
 * subscriber. This can avoid that the apply worker acquires locks for a long
 * time due to the long min_apply_time.
 *
 * Even if the transaction is applied from delayed file, the transaction is not
 * prepared. We just skip PREPARE message.
 */
static void
apply_handle_prepare(StringInfo s)
{
	LogicalRepPreparedTxnData prepare_data;

	/*
	 * If we are writing changes into delayed file, construct a modified
	 * message and write it. This is needed for avoiding to write gid into
	 * file. More detail, see atop ReadPreparedCommonRecord().
	 */
	if (in_delayed_transaction)
	{
		char		old_path[MAXPGPATH];
		char		new_path[MAXPGPATH];

		/* Cleanup */
		CloseTransientFile(delayed_fd);

		/*
		 * Construct old/new filename.
		 *
		 * Note that commit_lsn, end_lsn, and committime are not filled here.
		 * This is because when COMMIT PREPARED is come, we do no have a good
		 * way to indicate the related transaction file if they are filled.
		 */
		delay_file_name(old_path, MyLogicalRepWorker->subid, delayed_xid,
						DELAYED_TXN_UNKNOWN, InvalidXLogRecPtr, InvalidXLogRecPtr, 0);

		delay_file_name(new_path, MyLogicalRepWorker->subid, delayed_xid,
						DELAYED_TXN_PREPARED, InvalidXLogRecPtr, InvalidXLogRecPtr, 0);

		/* And do actual rename */
		if (durable_rename(old_path, new_path, PANIC))
			abort();

		/* Store flushed lsn */
		last_flushed = prepare_data.end_lsn;

		delayed_fd = -1;
		delayed_xid = InvalidTransactionId;
		in_delayed_transaction = false;
	}
	else
	{
		logicalrep_read_prepare(s, &prepare_data);

		if (prepare_data.prepare_lsn != remote_final_lsn)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					errmsg_internal("incorrect prepare LSN %X/%X in prepare message (expected %X/%X)",
									LSN_FORMAT_ARGS(prepare_data.prepare_lsn),
									LSN_FORMAT_ARGS(remote_final_lsn))));

		/*
		* Unlike commit, here, we always prepare the transaction even though no
		* change has happened in this transaction or all changes are skipped. It
		* is done this way because at commit prepared time, we won't know whether
		* we have skipped preparing a transaction because of those reasons.
		*
		* XXX, We can optimize such that at commit prepared time, we first check
		* whether we have prepared the transaction or not but that doesn't seem
		* worthwhile because such cases shouldn't be common.
		*/
		begin_replication_step();

		apply_handle_prepare_internal(&prepare_data);

		end_replication_step();
		CommitTransactionCommand();
		pgstat_report_stat(false);

		store_flush_position(prepare_data.end_lsn, XactLastCommitEnd);

		in_remote_transaction = false;

		/* Process any tables that are being synchronized in parallel. */
		process_syncing_tables(prepare_data.end_lsn);

		/*
		* Since we have already prepared the transaction, in a case where the
		* server crashes before clearing the subskiplsn, it will be left but the
		* transaction won't be resent. But that's okay because it's a rare case
		* and the subskiplsn will be cleared when finishing the next transaction.
		*/
		stop_skipping_changes();
		clear_subscription_skip_lsn(prepare_data.prepare_lsn);
	}
}

/*
 * Handle a COMMIT PREPARED of a previously PREPARED transaction.
 *
 * Note that we don't need to wait here if the transaction was prepared in a
 * parallel apply worker. In that case, we have already waited for the prepare
 * to finish in apply_handle_stream_prepare() which will ensure all the
 * operations in that transaction have happened in the subscriber, so no
 * concurrent transaction can cause deadlock or transaction dependency issues.
 */
static void
apply_handle_commit_prepared(StringInfo s)
{
	LogicalRepCommitPreparedTxnData prepare_data;
	char		gid[GIDSIZE];

	if (delayed_fd > 0 && !in_delayed_transaction)
		return;

	logicalrep_read_commit_prepared(s, &prepare_data);

	/*
	 * Check whether delayed file exists or not. If we have a file and we have
	 * not opened yet, it means that time-delayed logical replication has been
	 * requested. At that time we write the modified message.
	 * Otherwise, the transaction will be committed normally.
	 */
	if (delayed_fd < 0 &&
		is_given_transaction_delayed(MyLogicalRepWorker->subid, prepare_data.xid))
	{
		char		old_path[MAXPGPATH];
		char		new_path[MAXPGPATH];
		LogicalRepCommitData commit_data = {0};

		/*
		 * Open the delayed transaction file.
		 *
		 * Apart from RestoreDelayedTxns(), we don't want to read whole the
		 * directory to find the related file. That's why we use Invalid LSN
		 * and committime to indicate it.
		 */
		delay_file_name(old_path, MyLogicalRepWorker->subid, prepare_data.xid,
						DELAYED_TXN_PREPARED, InvalidXLogRecPtr,
						InvalidXLogRecPtr, 0);

		delayed_fd = OpenTransientFile(old_path, O_WRONLY | O_APPEND | PG_BINARY);
		if (delayed_fd < 0)
			ereport(ERROR,
					errcode_for_file_access(),
					errmsg("could not open file \"%s\": %m",
						   old_path));

		delay_file_name(new_path, MyLogicalRepWorker->subid,
						prepare_data.xid, DELAYED_TXN_COMMITTED,
						prepare_data.commit_lsn, prepare_data.end_lsn,
						prepare_data.commit_time);

		CloseTransientFile(delayed_fd);

		if (durable_rename(old_path, new_path, PANIC))
			abort();

		/* Store flushed lsn */
		last_flushed = prepare_data.end_lsn;

		delayed_fd = -1;
		delayed_xid = InvalidTransactionId;
		in_delayed_transaction = false;

		ConstructCommitFromCommitPrepared(&commit_data, &prepare_data);

		/* Cache the commited transaction */
		cache_commit_data(&commit_data, prepare_data.xid);
	}
	else
	{
		set_apply_error_context_xact(prepare_data.xid, prepare_data.commit_lsn);

		/* Compute GID for two_phase transactions. */
		TwoPhaseTransactionGid(MySubscription->oid, prepare_data.xid,
							gid, sizeof(gid));

		/* There is no transaction when COMMIT PREPARED is called */
		begin_replication_step();

		/*
		* Update origin state so we can restart streaming from correct position
		* in case of crash.
		*/
		replorigin_session_origin_lsn = prepare_data.end_lsn;
		replorigin_session_origin_timestamp = prepare_data.commit_time;

		FinishPreparedTransaction(gid, true);
		end_replication_step();
		CommitTransactionCommand();
		pgstat_report_stat(false);

		store_flush_position(prepare_data.end_lsn, XactLastCommitEnd);
		in_remote_transaction = false;

		/* Process any tables that are being synchronized in parallel. */
		process_syncing_tables(prepare_data.end_lsn);

		clear_subscription_skip_lsn(prepare_data.end_lsn);
	}
}

/*
 * Handle a ROLLBACK PREPARED of a previously PREPARED TRANSACTION.
 *
 * Note that we don't need to wait here if the transaction was prepared in a
 * parallel apply worker. In that case, we have already waited for the prepare
 * to finish in apply_handle_stream_prepare() which will ensure all the
 * operations in that transaction have happened in the subscriber, so no
 * concurrent transaction can cause deadlock or transaction dependency issues.
 */
static void
apply_handle_rollback_prepared(StringInfo s)
{
	LogicalRepRollbackPreparedTxnData rollback_data;
	char		gid[GIDSIZE];

	logicalrep_read_rollback_prepared(s, &rollback_data);

	/*
	 * If the delayed file exists, just remove it. The delayed transaction
	 * have never prepared, so it's OK not to call
	 * FinishPreparedTransaction().
	 */
	if (is_given_transaction_delayed(MyLogicalRepWorker->subid, rollback_data.xid))
	{
		char		path[MAXPGPATH];

		delay_file_name(path, MyLogicalRepWorker->subid, rollback_data.xid,
						DELAYED_TXN_PREPARED, InvalidXLogRecPtr,
						InvalidXLogRecPtr, 0);

		durable_unlink(path, LOG);
		clear_subscription_skip_lsn(rollback_data.rollback_end_lsn);
		return;
	}

	set_apply_error_context_xact(rollback_data.xid, rollback_data.rollback_end_lsn);

	/* Compute GID for two_phase transactions. */
	TwoPhaseTransactionGid(MySubscription->oid, rollback_data.xid,
						   gid, sizeof(gid));

	/*
	 * It is possible that we haven't received prepare because it occurred
	 * before walsender reached a consistent point or the two_phase was still
	 * not enabled by that time, so in such cases, we need to skip rollback
	 * prepared.
	 */
	if (LookupGXact(gid, rollback_data.prepare_end_lsn,
					rollback_data.prepare_time))
	{
		/*
		 * Update origin state so we can restart streaming from correct
		 * position in case of crash.
		 */
		replorigin_session_origin_lsn = rollback_data.rollback_end_lsn;
		replorigin_session_origin_timestamp = rollback_data.rollback_time;

		/* There is no transaction when ABORT/ROLLBACK PREPARED is called */
		begin_replication_step();
		FinishPreparedTransaction(gid, false);
		end_replication_step();
		CommitTransactionCommand();

		clear_subscription_skip_lsn(rollback_data.rollback_end_lsn);
	}

	pgstat_report_stat(false);

	store_flush_position(rollback_data.rollback_end_lsn, XactLastCommitEnd);
	in_remote_transaction = false;

	/* Process any tables that are being synchronized in parallel. */
	process_syncing_tables(rollback_data.rollback_end_lsn);

	pgstat_report_activity(STATE_IDLE, NULL);
	reset_apply_error_context_info();
}

/*
 * Handle STREAM PREPARE.
 */
static void
apply_handle_stream_prepare(StringInfo s)
{
	LogicalRepPreparedTxnData prepare_data;
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;

	/* Save the message before it is consumed. */
	StringInfoData original_msg = *s;

	if (in_streamed_transaction)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("STREAM PREPARE message without STREAM STOP")));

	/* Tablesync should never receive prepare. */
	if (am_tablesync_worker())
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("tablesync worker received a STREAM PREPARE message")));

	logicalrep_read_stream_prepare(s, &prepare_data);
	set_apply_error_context_xact(prepare_data.xid, prepare_data.prepare_lsn);

	apply_action = get_transaction_apply_action(prepare_data.xid, &winfo);

	switch (apply_action)
	{
		case TRANS_LEADER_APPLY:
			/*
			 * If time-delayed is requested, start to write changes to
			 * permanent file instead of temporary one.
			 */
			if (MySubscription->minapplydelay)
			{
				in_delayed_transaction = true;

				create_delay_file(prepare_data.xid);

				delayed_xid = prepare_data.xid;
			}

			/*
			 * The transaction has been serialized to file, so replay all the
			 * spooled operations. Note that if time-delayed replication is
			 * requested, changes are written into permanent file here.
			 */
			apply_spooled_messages(MyLogicalRepWorker->stream_fileset,
								   prepare_data.xid, prepare_data.prepare_lsn);


			/*
			 * If time-delayed replication is requested, construct a modified
			 * message and write it. This is needed for avoiding to write gid
			 * into file. More detail, see atop ReadPreparedCommonRecord().
			 */
			if (MySubscription->minapplydelay)
			{
				char		old_path[MAXPGPATH];
				char		new_path[MAXPGPATH];

				CloseTransientFile(delayed_fd);

				delay_file_name(old_path, MyLogicalRepWorker->subid,
								prepare_data.xid, DELAYED_TXN_UNKNOWN,
								InvalidXLogRecPtr, InvalidXLogRecPtr, 0);

				delay_file_name(new_path, MyLogicalRepWorker->subid,
								prepare_data.xid, DELAYED_TXN_PREPARED,
								InvalidXLogRecPtr, InvalidXLogRecPtr, 0);

				if (durable_rename(old_path, new_path, PANIC))
					abort();

				/* Store flushed lsn */
				last_flushed = prepare_data.end_lsn;

				CloseTransientFile(delayed_fd);

				delayed_fd = -1;
				delayed_xid = InvalidTransactionId;
				in_delayed_transaction = false;
			}
			else
			{
				/* Mark the transaction as prepared. */
				apply_handle_prepare_internal(&prepare_data);
			}

			CommitTransactionCommand();

			store_flush_position(prepare_data.end_lsn, XactLastCommitEnd);

			in_remote_transaction = false;

			/* Unlink the files with serialized changes and subxact info. */
			stream_cleanup_files(MyLogicalRepWorker->subid, prepare_data.xid);

			elog(DEBUG1, "finished processing the STREAM PREPARE command");
			break;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			if (pa_send_data(winfo, s->len, s->data))
			{
				/* Finish processing the streaming transaction. */
				pa_xact_finish(winfo, prepare_data.end_lsn);
				break;
			}

			/*
			 * Switch to serialize mode when we are not able to send the
			 * change to parallel apply worker.
			 */
			pa_switch_to_partial_serialize(winfo, true);

			/* fall through */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			Assert(winfo);

			stream_open_and_write_change(prepare_data.xid,
										 LOGICAL_REP_MSG_STREAM_PREPARE,
										 &original_msg);

			pa_set_fileset_state(winfo->shared, FS_SERIALIZE_DONE);

			/* Finish processing the streaming transaction. */
			pa_xact_finish(winfo, prepare_data.end_lsn);
			break;

		case TRANS_PARALLEL_APPLY:

			/*
			 * If the parallel apply worker is applying spooled messages then
			 * close the file before preparing.
			 */
			if (stream_fd)
				stream_close_file();

			begin_replication_step();

			/* Mark the transaction as prepared. */
			apply_handle_prepare_internal(&prepare_data);

			end_replication_step();

			CommitTransactionCommand();

			MyParallelShared->last_commit_end = XactLastCommitEnd;

			pa_set_xact_state(MyParallelShared, PARALLEL_TRANS_FINISHED);
			pa_unlock_transaction(MyParallelShared->xid, AccessExclusiveLock);

			pa_reset_subtrans();

			elog(DEBUG1, "finished processing the STREAM PREPARE command");
			break;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			break;
	}

	pgstat_report_stat(false);

	if (list_length(DelayedTxnList) == 0)
	{
		/* Process any tables that are being synchronized in parallel. */
		process_syncing_tables(prepare_data.end_lsn);
	}

	/*
	 * Similar to prepare case, the subskiplsn could be left in a case of
	 * server crash but it's okay. See the comments in apply_handle_prepare().
	 */
	stop_skipping_changes();
	clear_subscription_skip_lsn(prepare_data.prepare_lsn);

	pgstat_report_activity(STATE_IDLE, NULL);

	reset_apply_error_context_info();
}

/*
 * Handle ORIGIN message.
 *
 * TODO, support tracking of multiple origins
 */
static void
apply_handle_origin(StringInfo s)
{
	/*
	 * ORIGIN message can only come inside streaming transaction or inside
	 * remote transaction and before any actual writes.
	 */
	if (!in_streamed_transaction &&
		(!in_remote_transaction ||
		 (IsTransactionState() && !am_tablesync_worker())))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("ORIGIN message sent out of order")));
}

/*
 * Initialize fileset (if not already done).
 *
 * Create a new file when first_segment is true, otherwise open the existing
 * file.
 */
void
stream_start_internal(TransactionId xid, bool first_segment)
{
	begin_replication_step();

	/*
	 * Initialize the worker's stream_fileset if we haven't yet. This will be
	 * used for the entire duration of the worker so create it in a permanent
	 * context. We create this on the very first streaming message from any
	 * transaction and then use it for this and other streaming transactions.
	 * Now, we could create a fileset at the start of the worker as well but
	 * then we won't be sure that it will ever be used.
	 */
	if (!MyLogicalRepWorker->stream_fileset)
	{
		MemoryContext oldctx;

		oldctx = MemoryContextSwitchTo(ApplyContext);

		MyLogicalRepWorker->stream_fileset = palloc(sizeof(FileSet));
		FileSetInit(MyLogicalRepWorker->stream_fileset);

		MemoryContextSwitchTo(oldctx);
	}

	/* Open the spool file for this transaction. */
	stream_open_file(MyLogicalRepWorker->subid, xid, first_segment);

	/* If this is not the first segment, open existing subxact file. */
	if (!first_segment)
		subxact_info_read(MyLogicalRepWorker->subid, xid);

	end_replication_step();
}

/*
 * Handle STREAM START message.
 */
static void
apply_handle_stream_start(StringInfo s)
{
	bool		first_segment;
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;

	/* Save the message before it is consumed. */
	StringInfoData original_msg = *s;

	if (in_streamed_transaction)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("duplicate STREAM START message")));

	/* There must not be an active streaming transaction. */
	Assert(!TransactionIdIsValid(stream_xid));

	/* notify handle methods we're processing a remote transaction */
	in_streamed_transaction = true;

	/* extract XID of the top-level transaction */
	stream_xid = logicalrep_read_stream_start(s, &first_segment);

	if (!TransactionIdIsValid(stream_xid))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("invalid transaction ID in streamed replication transaction")));

	set_apply_error_context_xact(stream_xid, InvalidXLogRecPtr);

	/* Try to allocate a worker for the streaming transaction. */
	if (first_segment)
		pa_allocate_worker(stream_xid);

	apply_action = get_transaction_apply_action(stream_xid, &winfo);

	switch (apply_action)
	{
		case TRANS_LEADER_SERIALIZE:

			/*
			 * Function stream_start_internal starts a transaction. This
			 * transaction will be committed on the stream stop unless it is a
			 * tablesync worker in which case it will be committed after
			 * processing all the messages. We need this transaction for
			 * handling the BufFile, used for serializing the streaming data
			 * and subxact info.
			 */
			stream_start_internal(stream_xid, first_segment);
			break;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			/*
			 * Once we start serializing the changes, the parallel apply
			 * worker will wait for the leader to release the stream lock
			 * until the end of the transaction. So, we don't need to release
			 * the lock or increment the stream count in that case.
			 */
			if (pa_send_data(winfo, s->len, s->data))
			{
				/*
				 * Unlock the shared object lock so that the parallel apply
				 * worker can continue to receive changes.
				 */
				if (!first_segment)
					pa_unlock_stream(winfo->shared->xid, AccessExclusiveLock);

				/*
				 * Increment the number of streaming blocks waiting to be
				 * processed by parallel apply worker.
				 */
				pg_atomic_add_fetch_u32(&winfo->shared->pending_stream_count, 1);

				/* Cache the parallel apply worker for this transaction. */
				pa_set_stream_apply_worker(winfo);
				break;
			}

			/*
			 * Switch to serialize mode when we are not able to send the
			 * change to parallel apply worker.
			 */
			pa_switch_to_partial_serialize(winfo, !first_segment);

			/* fall through */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			Assert(winfo);

			/*
			 * Open the spool file unless it was already opened when switching
			 * to serialize mode. The transaction started in
			 * stream_start_internal will be committed on the stream stop.
			 */
			if (apply_action != TRANS_LEADER_SEND_TO_PARALLEL)
				stream_start_internal(stream_xid, first_segment);

			stream_write_change(LOGICAL_REP_MSG_STREAM_START, &original_msg);

			/* Cache the parallel apply worker for this transaction. */
			pa_set_stream_apply_worker(winfo);
			break;

		case TRANS_PARALLEL_APPLY:
			if (first_segment)
			{
				/* Hold the lock until the end of the transaction. */
				pa_lock_transaction(MyParallelShared->xid, AccessExclusiveLock);
				pa_set_xact_state(MyParallelShared, PARALLEL_TRANS_STARTED);

				/*
				 * Signal the leader apply worker, as it may be waiting for
				 * us.
				 */
				logicalrep_worker_wakeup(MyLogicalRepWorker->subid, InvalidOid);
			}

			parallel_stream_nchanges = 0;
			break;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			break;
	}

	pgstat_report_activity(STATE_RUNNING, NULL);
}

/*
 * Update the information about subxacts and close the file.
 *
 * This function should be called when the stream_start_internal function has
 * been called.
 */
void
stream_stop_internal(TransactionId xid)
{
	/*
	 * Serialize information about subxacts for the toplevel transaction, then
	 * close the stream messages spool file.
	 */
	subxact_info_write(MyLogicalRepWorker->subid, xid);
	stream_close_file();

	/* We must be in a valid transaction state */
	Assert(IsTransactionState());

	/* Commit the per-stream transaction */
	CommitTransactionCommand();

	/* Reset per-stream context */
	MemoryContextReset(LogicalStreamingContext);
}

/*
 * Handle STREAM STOP message.
 */
static void
apply_handle_stream_stop(StringInfo s)
{
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;

	if (!in_streamed_transaction)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("STREAM STOP message without STREAM START")));

	apply_action = get_transaction_apply_action(stream_xid, &winfo);

	switch (apply_action)
	{
		case TRANS_LEADER_SERIALIZE:
			stream_stop_internal(stream_xid);
			break;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			/*
			 * Lock before sending the STREAM_STOP message so that the leader
			 * can hold the lock first and the parallel apply worker will wait
			 * for leader to release the lock. See Locking Considerations atop
			 * applyparallelworker.c.
			 */
			pa_lock_stream(winfo->shared->xid, AccessExclusiveLock);

			if (pa_send_data(winfo, s->len, s->data))
			{
				pa_set_stream_apply_worker(NULL);
				break;
			}

			/*
			 * Switch to serialize mode when we are not able to send the
			 * change to parallel apply worker.
			 */
			pa_switch_to_partial_serialize(winfo, true);

			/* fall through */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			stream_write_change(LOGICAL_REP_MSG_STREAM_STOP, s);
			stream_stop_internal(stream_xid);
			pa_set_stream_apply_worker(NULL);
			break;

		case TRANS_PARALLEL_APPLY:
			elog(DEBUG1, "applied %u changes in the streaming chunk",
				 parallel_stream_nchanges);

			/*
			 * By the time parallel apply worker is processing the changes in
			 * the current streaming block, the leader apply worker may have
			 * sent multiple streaming blocks. This can lead to parallel apply
			 * worker start waiting even when there are more chunk of streams
			 * in the queue. So, try to lock only if there is no message left
			 * in the queue. See Locking Considerations atop
			 * applyparallelworker.c.
			 *
			 * Note that here we have a race condition where we can start
			 * waiting even when there are pending streaming chunks. This can
			 * happen if the leader sends another streaming block and acquires
			 * the stream lock again after the parallel apply worker checks
			 * that there is no pending streaming block and before it actually
			 * starts waiting on a lock. We can handle this case by not
			 * allowing the leader to increment the stream block count during
			 * the time parallel apply worker acquires the lock but it is not
			 * clear whether that is worth the complexity.
			 *
			 * Now, if this missed chunk contains rollback to savepoint, then
			 * there is a risk of deadlock which probably shouldn't happen
			 * after restart.
			 */
			pa_decr_and_wait_stream_block();
			break;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			break;
	}

	in_streamed_transaction = false;
	stream_xid = InvalidTransactionId;

	/*
	 * The parallel apply worker could be in a transaction in which case we
	 * need to report the state as STATE_IDLEINTRANSACTION.
	 */
	if (IsTransactionOrTransactionBlock())
		pgstat_report_activity(STATE_IDLEINTRANSACTION, NULL);
	else
		pgstat_report_activity(STATE_IDLE, NULL);

	reset_apply_error_context_info();
}

/*
 * Helper function to handle STREAM ABORT message when the transaction was
 * serialized to file.
 */
static void
stream_abort_internal(TransactionId xid, TransactionId subxid)
{
	/*
	 * If the two XIDs are the same, it's in fact abort of toplevel xact, so
	 * just delete the files with serialized info.
	 */
	if (xid == subxid)
		stream_cleanup_files(MyLogicalRepWorker->subid, xid);
	else
	{
		/*
		 * OK, so it's a subxact. We need to read the subxact file for the
		 * toplevel transaction, determine the offset tracked for the subxact,
		 * and truncate the file with changes. We also remove the subxacts
		 * with higher offsets (or rather higher XIDs).
		 *
		 * We intentionally scan the array from the tail, because we're likely
		 * aborting a change for the most recent subtransactions.
		 *
		 * We can't use the binary search here as subxact XIDs won't
		 * necessarily arrive in sorted order, consider the case where we have
		 * released the savepoint for multiple subtransactions and then
		 * performed rollback to savepoint for one of the earlier
		 * sub-transaction.
		 */
		int64		i;
		int64		subidx;
		BufFile    *fd;
		bool		found = false;
		char		path[MAXPGPATH];

		subidx = -1;
		begin_replication_step();
		subxact_info_read(MyLogicalRepWorker->subid, xid);

		for (i = subxact_data.nsubxacts; i > 0; i--)
		{
			if (subxact_data.subxacts[i - 1].xid == subxid)
			{
				subidx = (i - 1);
				found = true;
				break;
			}
		}

		/*
		 * If it's an empty sub-transaction then we will not find the subxid
		 * here so just cleanup the subxact info and return.
		 */
		if (!found)
		{
			/* Cleanup the subxact info */
			cleanup_subxact_info();
			end_replication_step();
			CommitTransactionCommand();
			return;
		}

		/* open the changes file */
		changes_filename(path, MyLogicalRepWorker->subid, xid);
		fd = BufFileOpenFileSet(MyLogicalRepWorker->stream_fileset, path,
								O_RDWR, false);

		/* OK, truncate the file at the right offset */
		BufFileTruncateFileSet(fd, subxact_data.subxacts[subidx].fileno,
							   subxact_data.subxacts[subidx].offset);
		BufFileClose(fd);

		/* discard the subxacts added later */
		subxact_data.nsubxacts = subidx;

		/* write the updated subxact list */
		subxact_info_write(MyLogicalRepWorker->subid, xid);

		end_replication_step();
		CommitTransactionCommand();
	}
}

/*
 * Handle STREAM ABORT message.
 */
static void
apply_handle_stream_abort(StringInfo s)
{
	TransactionId xid;
	TransactionId subxid;
	LogicalRepStreamAbortData abort_data;
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;

	/* Save the message before it is consumed. */
	StringInfoData original_msg = *s;
	bool		toplevel_xact;

	if (in_streamed_transaction)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("STREAM ABORT message without STREAM STOP")));

	/* We receive abort information only when we can apply in parallel. */
	logicalrep_read_stream_abort(s, &abort_data,
								 MyLogicalRepWorker->parallel_apply);

	xid = abort_data.xid;
	subxid = abort_data.subxid;
	toplevel_xact = (xid == subxid);

	set_apply_error_context_xact(subxid, abort_data.abort_lsn);

	apply_action = get_transaction_apply_action(xid, &winfo);

	switch (apply_action)
	{
		case TRANS_LEADER_APPLY:

			/*
			 * We are in the leader apply worker and the transaction has been
			 * serialized to file.
			 */
			stream_abort_internal(xid, subxid);

			elog(DEBUG1, "finished processing the STREAM ABORT command");
			break;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			/*
			 * For the case of aborting the subtransaction, we increment the
			 * number of streaming blocks and take the lock again before
			 * sending the STREAM_ABORT to ensure that the parallel apply
			 * worker will wait on the lock for the next set of changes after
			 * processing the STREAM_ABORT message if it is not already
			 * waiting for STREAM_STOP message.
			 *
			 * It is important to perform this locking before sending the
			 * STREAM_ABORT message so that the leader can hold the lock first
			 * and the parallel apply worker will wait for the leader to
			 * release the lock. This is the same as what we do in
			 * apply_handle_stream_stop. See Locking Considerations atop
			 * applyparallelworker.c.
			 */
			if (!toplevel_xact)
			{
				pa_unlock_stream(xid, AccessExclusiveLock);
				pg_atomic_add_fetch_u32(&winfo->shared->pending_stream_count, 1);
				pa_lock_stream(xid, AccessExclusiveLock);
			}

			if (pa_send_data(winfo, s->len, s->data))
			{
				/*
				 * Unlike STREAM_COMMIT and STREAM_PREPARE, we don't need to
				 * wait here for the parallel apply worker to finish as that
				 * is not required to maintain the commit order and won't have
				 * the risk of failures due to transaction dependencies and
				 * deadlocks. However, it is possible that before the parallel
				 * worker finishes and we clear the worker info, the xid
				 * wraparound happens on the upstream and a new transaction
				 * with the same xid can appear and that can lead to duplicate
				 * entries in ParallelApplyTxnHash. Yet another problem could
				 * be that we may have serialized the changes in partial
				 * serialize mode and the file containing xact changes may
				 * already exist, and after xid wraparound trying to create
				 * the file for the same xid can lead to an error. To avoid
				 * these problems, we decide to wait for the aborts to finish.
				 *
				 * Note, it is okay to not update the flush location position
				 * for aborts as in worst case that means such a transaction
				 * won't be sent again after restart.
				 */
				if (toplevel_xact)
					pa_xact_finish(winfo, InvalidXLogRecPtr);

				break;
			}

			/*
			 * Switch to serialize mode when we are not able to send the
			 * change to parallel apply worker.
			 */
			pa_switch_to_partial_serialize(winfo, true);

			/* fall through */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			Assert(winfo);

			/*
			 * Parallel apply worker might have applied some changes, so write
			 * the STREAM_ABORT message so that it can rollback the
			 * subtransaction if needed.
			 */
			stream_open_and_write_change(xid, LOGICAL_REP_MSG_STREAM_ABORT,
										 &original_msg);

			if (toplevel_xact)
			{
				pa_set_fileset_state(winfo->shared, FS_SERIALIZE_DONE);
				pa_xact_finish(winfo, InvalidXLogRecPtr);
			}
			break;

		case TRANS_PARALLEL_APPLY:

			/*
			 * If the parallel apply worker is applying spooled messages then
			 * close the file before aborting.
			 */
			if (toplevel_xact && stream_fd)
				stream_close_file();

			pa_stream_abort(&abort_data);

			/*
			 * We need to wait after processing rollback to savepoint for the
			 * next set of changes.
			 *
			 * We have a race condition here due to which we can start waiting
			 * here when there are more chunk of streams in the queue. See
			 * apply_handle_stream_stop.
			 */
			if (!toplevel_xact)
				pa_decr_and_wait_stream_block();

			elog(DEBUG1, "finished processing the STREAM ABORT command");
			break;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			break;
	}

	reset_apply_error_context_info();
}

/*
 * Ensure that the passed location is fileset's end.
 */
static void
ensure_last_message(FileSet *stream_fileset, TransactionId xid, int fileno,
					off_t offset)
{
	char		path[MAXPGPATH];
	BufFile    *fd;
	int			last_fileno;
	off_t		last_offset;

	Assert(!IsTransactionState());

	begin_replication_step();

	changes_filename(path, MyLogicalRepWorker->subid, xid);

	fd = BufFileOpenFileSet(stream_fileset, path, O_RDONLY, false);

	BufFileSeek(fd, 0, 0, SEEK_END);
	BufFileTell(fd, &last_fileno, &last_offset);

	BufFileClose(fd);

	end_replication_step();

	if (last_fileno != fileno || last_offset != offset)
		elog(ERROR, "unexpected message left in streaming transaction's changes file \"%s\"",
			 path);
}

/*
 * Common spoolfile processing.
 */
void
apply_spooled_messages(FileSet *stream_fileset, TransactionId xid,
					   XLogRecPtr lsn)
{
	StringInfoData s2;
	int			nchanges;
	char		path[MAXPGPATH];
	char	   *buffer = NULL;
	MemoryContext oldcxt;
	ResourceOwner oldowner;
	int			fileno;
	off_t		offset;

	if (!am_parallel_apply_worker())
		maybe_start_skipping_changes(lsn);

	/* Make sure we have an open transaction */
	begin_replication_step();

	/*
	 * Allocate file handle and memory required to process all the messages in
	 * TopTransactionContext to avoid them getting reset after each message is
	 * processed.
	 */
	oldcxt = MemoryContextSwitchTo(TopTransactionContext);

	/* Open the spool file for the committed/prepared transaction */
	changes_filename(path, MyLogicalRepWorker->subid, xid);
	elog(DEBUG1, "replaying changes from file \"%s\"", path);

	/*
	 * Make sure the file is owned by the toplevel transaction so that the
	 * file will not be accidentally closed when aborting a subtransaction.
	 */
	oldowner = CurrentResourceOwner;
	CurrentResourceOwner = TopTransactionResourceOwner;

	stream_fd = BufFileOpenFileSet(stream_fileset, path, O_RDONLY, false);

	CurrentResourceOwner = oldowner;

	buffer = palloc(BLCKSZ);
	initStringInfo(&s2);

	MemoryContextSwitchTo(oldcxt);

	remote_final_lsn = lsn;

	/*
	 * Make sure the handle apply_dispatch methods are aware we're in a remote
	 * transaction.
	 */
	in_remote_transaction = true;
	pgstat_report_activity(STATE_RUNNING, NULL);

	end_replication_step();

	/*
	 * Read the entries one by one and pass them through the same logic as in
	 * apply_dispatch.
	 */
	nchanges = 0;
	while (true)
	{
		size_t		nbytes;
		int			len;

		CHECK_FOR_INTERRUPTS();

		/* read length of the on-disk record */
		nbytes = BufFileReadMaybeEOF(stream_fd, &len, sizeof(len), true);

		/* have we reached end of the file? */
		if (nbytes == 0)
			break;

		/* do we have a correct length? */
		if (len <= 0)
			elog(ERROR, "incorrect length %d in streaming transaction's changes file \"%s\"",
				 len, path);

		/* make sure we have sufficiently large buffer */
		buffer = repalloc(buffer, len);

		/* and finally read the data into the buffer */
		BufFileReadExact(stream_fd, buffer, len);

		BufFileTell(stream_fd, &fileno, &offset);

		/* copy the buffer to the stringinfo and call apply_dispatch */
		resetStringInfo(&s2);
		appendBinaryStringInfo(&s2, buffer, len);

		/* Ensure we are reading the data into our memory context. */
		oldcxt = MemoryContextSwitchTo(ApplyMessageContext);

		apply_dispatch(&s2);

		MemoryContextReset(ApplyMessageContext);

		MemoryContextSwitchTo(oldcxt);

		nchanges++;

		/*
		 * It is possible the file has been closed because we have processed
		 * the transaction end message like stream_commit in which case that
		 * must be the last message.
		 */
		if (!stream_fd)
		{
			ensure_last_message(stream_fileset, xid, fileno, offset);
			break;
		}

		if (nchanges % 1000 == 0)
			elog(DEBUG1, "replayed %d changes from file \"%s\"",
				 nchanges, path);
	}

	if (stream_fd)
		stream_close_file();

	elog(DEBUG1, "replayed %d (all) changes from file \"%s\"",
		 nchanges, path);

	return;
}

/*
 * Handle STREAM COMMIT message.
 */
static void
apply_handle_stream_commit(StringInfo s)
{
	TransactionId xid;
	LogicalRepCommitData commit_data;
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;

	/* Save the message before it is consumed. */
	StringInfoData original_msg = *s;

	if (in_streamed_transaction)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("STREAM COMMIT message without STREAM STOP")));

	xid = logicalrep_read_stream_commit(s, &commit_data);
	set_apply_error_context_xact(xid, commit_data.commit_lsn);

	apply_action = get_transaction_apply_action(xid, &winfo);

	switch (apply_action)
	{
		case TRANS_LEADER_APPLY:

			/*
			 * If time-delayed is requested, start to write changes to
			 * permanent file instead of temporary one.
			 */
			if (MySubscription->minapplydelay)
			{
				in_delayed_transaction = true;

				create_delay_file(xid);

				delayed_xid = xid;
			}

			/*
			 * The transaction has been serialized to file, so replay all the
			 * spooled operations. Note that if time-delayed replication is
			 * requested, changes are written into permanent file here.
			 */
			apply_spooled_messages(MyLogicalRepWorker->stream_fileset, xid,
								   commit_data.commit_lsn);


			/* Flush changes if time-delayed is requested */
			if (MySubscription->minapplydelay)
			{
				handle_delayed_transaction(LOGICAL_REP_MSG_COMMIT, &original_msg);
				flush_delayed_changes(&commit_data);
			}
			else
				apply_handle_commit_internal(&commit_data);

			/* Unlink the files with serialized changes and subxact info. */
			stream_cleanup_files(MyLogicalRepWorker->subid, xid);

			elog(DEBUG1, "finished processing the STREAM COMMIT command");

			break;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			if (pa_send_data(winfo, s->len, s->data))
			{
				/* Finish processing the streaming transaction. */
				pa_xact_finish(winfo, commit_data.end_lsn);
				break;
			}

			/*
			 * Switch to serialize mode when we are not able to send the
			 * change to parallel apply worker.
			 */
			pa_switch_to_partial_serialize(winfo, true);

			/* fall through */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			Assert(winfo);

			stream_open_and_write_change(xid, LOGICAL_REP_MSG_STREAM_COMMIT,
										 &original_msg);

			pa_set_fileset_state(winfo->shared, FS_SERIALIZE_DONE);

			/* Finish processing the streaming transaction. */
			pa_xact_finish(winfo, commit_data.end_lsn);
			break;

		case TRANS_PARALLEL_APPLY:

			/*
			 * If the parallel apply worker is applying spooled messages then
			 * close the file before committing.
			 */
			if (stream_fd)
				stream_close_file();

			apply_handle_commit_internal(&commit_data);

			MyParallelShared->last_commit_end = XactLastCommitEnd;

			/*
			 * It is important to set the transaction state as finished before
			 * releasing the lock. See pa_wait_for_xact_finish.
			 */
			pa_set_xact_state(MyParallelShared, PARALLEL_TRANS_FINISHED);
			pa_unlock_transaction(xid, AccessExclusiveLock);

			pa_reset_subtrans();

			elog(DEBUG1, "finished processing the STREAM COMMIT command");
			break;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			break;
	}

	if (list_length(DelayedTxnList) == 0)
	{
		/* Process any tables that are being synchronized in parallel. */
		process_syncing_tables(commit_data.end_lsn);
	}

	pgstat_report_activity(STATE_IDLE, NULL);

	reset_apply_error_context_info();
}

/*
 * Helper function for apply_handle_commit and apply_handle_stream_commit.
 */
static void
apply_handle_commit_internal(LogicalRepCommitData *commit_data)
{
	if (is_skipping_changes())
	{
		stop_skipping_changes();

		/*
		 * Start a new transaction to clear the subskiplsn, if not started
		 * yet.
		 */
		if (!IsTransactionState())
			StartTransactionCommand();
	}

	if (IsTransactionState())
	{
		/*
		 * The transaction is either non-empty or skipped, so we clear the
		 * subskiplsn.
		 */
		clear_subscription_skip_lsn(commit_data->commit_lsn);

		/*
		 * Update origin state so we can restart streaming from correct
		 * position in case of crash.
		 */
		replorigin_session_origin_lsn = commit_data->end_lsn;
		replorigin_session_origin_timestamp = commit_data->committime;

		CommitTransactionCommand();

		if (IsTransactionBlock())
		{
			EndTransactionBlock(false);
			CommitTransactionCommand();
		}

		pgstat_report_stat(false);

		store_flush_position(commit_data->end_lsn, XactLastCommitEnd);
	}
	else
	{
		/* Process any invalidation messages that might have accumulated. */
		AcceptInvalidationMessages();
		maybe_reread_subscription();
	}

	in_remote_transaction = false;
}

/*
 * Handle RELATION message.
 *
 * Note we don't do validation against local schema here. The validation
 * against local schema is postponed until first change for given relation
 * comes as we only care about it when applying changes for it anyway and we
 * do less locking this way.
 */
static void
apply_handle_relation(StringInfo s)
{
	LogicalRepRelation *rel;

	if (handle_streamed_transaction(LOGICAL_REP_MSG_RELATION, s) ||
		handle_delayed_transaction(LOGICAL_REP_MSG_RELATION, s))
		return;

	rel = logicalrep_read_rel(s);
	logicalrep_relmap_update(rel);

	/* Also reset all entries in the partition map that refer to remoterel. */
	logicalrep_partmap_reset_relmap(rel);
}

/*
 * Handle TYPE message.
 *
 * This implementation pays no attention to TYPE messages; we expect the user
 * to have set things up so that the incoming data is acceptable to the input
 * functions for the locally subscribed tables.  Hence, we just read and
 * discard the message.
 */
static void
apply_handle_type(StringInfo s)
{
	LogicalRepTyp typ;

	if (handle_streamed_transaction(LOGICAL_REP_MSG_TYPE, s) ||
		handle_delayed_transaction(LOGICAL_REP_MSG_TYPE, s))
		return;

	logicalrep_read_typ(s, &typ);
}

/*
 * Check that we (the subscription owner) have sufficient privileges on the
 * target relation to perform the given operation.
 */
static void
TargetPrivilegesCheck(Relation rel, AclMode mode)
{
	Oid			relid;
	AclResult	aclresult;

	relid = RelationGetRelid(rel);
	aclresult = pg_class_aclcheck(relid, GetUserId(), mode);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult,
					   get_relkind_objtype(rel->rd_rel->relkind),
					   get_rel_name(relid));

	/*
	 * We lack the infrastructure to honor RLS policies.  It might be possible
	 * to add such infrastructure here, but tablesync workers lack it, too, so
	 * we don't bother.  RLS does not ordinarily apply to TRUNCATE commands,
	 * but it seems dangerous to replicate a TRUNCATE and then refuse to
	 * replicate subsequent INSERTs, so we forbid all commands the same.
	 */
	if (check_enable_rls(relid, InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("user \"%s\" cannot replicate into relation with row-level security enabled: \"%s\"",
						GetUserNameFromId(GetUserId(), true),
						RelationGetRelationName(rel))));
}

/*
 * Handle INSERT message.
 */

static void
apply_handle_insert(StringInfo s)
{
	LogicalRepRelMapEntry *rel;
	LogicalRepTupleData newtup;
	LogicalRepRelId relid;
	UserContext		ucxt;
	ApplyExecutionData *edata;
	EState	   *estate;
	TupleTableSlot *remoteslot;
	MemoryContext oldctx;
	bool		run_as_owner;

	/*
	 * Quick return if we are skipping data modification changes or handling
	 * streamed transactions.
	 */
	if (is_skipping_changes() ||
		handle_streamed_transaction(LOGICAL_REP_MSG_INSERT, s) ||
		handle_delayed_transaction(LOGICAL_REP_MSG_INSERT, s))
		return;

	begin_replication_step();

	relid = logicalrep_read_insert(s, &newtup);
	rel = logicalrep_rel_open(relid, RowExclusiveLock);
	if (!should_apply_changes_for_rel(rel))
	{
		/*
		 * The relation can't become interesting in the middle of the
		 * transaction so it's safe to unlock it.
		 */
		logicalrep_rel_close(rel, RowExclusiveLock);
		end_replication_step();
		return;
	}

	/*
	 * Make sure that any user-supplied code runs as the table owner, unless
	 * the user has opted out of that behavior.
	 */
	run_as_owner = MySubscription->runasowner;
	if (!run_as_owner)
		SwitchToUntrustedUser(rel->localrel->rd_rel->relowner, &ucxt);

	/* Set relation for error callback */
	apply_error_callback_arg.rel = rel;

	/* Initialize the executor state. */
	edata = create_edata_for_relation(rel);
	estate = edata->estate;
	remoteslot = ExecInitExtraTupleSlot(estate,
										RelationGetDescr(rel->localrel),
										&TTSOpsVirtual);

	/* Process and store remote tuple in the slot */
	oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
	slot_store_data(remoteslot, rel, &newtup);
	slot_fill_defaults(rel, estate, remoteslot);
	MemoryContextSwitchTo(oldctx);

	/* For a partitioned table, insert the tuple into a partition. */
	if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		apply_handle_tuple_routing(edata,
								   remoteslot, NULL, CMD_INSERT);
	else
		apply_handle_insert_internal(edata, edata->targetRelInfo,
									 remoteslot);

	finish_edata(edata);

	/* Reset relation for error callback */
	apply_error_callback_arg.rel = NULL;

	if (!run_as_owner)
		RestoreUserContext(&ucxt);

	logicalrep_rel_close(rel, NoLock);

	end_replication_step();
}

/*
 * Workhorse for apply_handle_insert()
 * relinfo is for the relation we're actually inserting into
 * (could be a child partition of edata->targetRelInfo)
 */
static void
apply_handle_insert_internal(ApplyExecutionData *edata,
							 ResultRelInfo *relinfo,
							 TupleTableSlot *remoteslot)
{
	EState	   *estate = edata->estate;

	/* We must open indexes here. */
	ExecOpenIndices(relinfo, false);

	/* Do the insert. */
	TargetPrivilegesCheck(relinfo->ri_RelationDesc, ACL_INSERT);
	ExecSimpleRelationInsert(relinfo, estate, remoteslot);

	/* Cleanup. */
	ExecCloseIndices(relinfo);
}

/*
 * Check if the logical replication relation is updatable and throw
 * appropriate error if it isn't.
 */
static void
check_relation_updatable(LogicalRepRelMapEntry *rel)
{
	/*
	 * For partitioned tables, we only need to care if the target partition is
	 * updatable (aka has PK or RI defined for it).
	 */
	if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return;

	/* Updatable, no error. */
	if (rel->updatable)
		return;

	/*
	 * We are in error mode so it's fine this is somewhat slow. It's better to
	 * give user correct error.
	 */
	if (OidIsValid(GetRelationIdentityOrPK(rel->localrel)))
	{
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("publisher did not send replica identity column "
						"expected by the logical replication target relation \"%s.%s\"",
						rel->remoterel.nspname, rel->remoterel.relname)));
	}

	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("logical replication target relation \"%s.%s\" has "
					"neither REPLICA IDENTITY index nor PRIMARY "
					"KEY and published relation does not have "
					"REPLICA IDENTITY FULL",
					rel->remoterel.nspname, rel->remoterel.relname)));
}

/*
 * Handle UPDATE message.
 *
 * TODO: FDW support
 */
static void
apply_handle_update(StringInfo s)
{
	LogicalRepRelMapEntry *rel;
	LogicalRepRelId relid;
	UserContext		ucxt;
	ApplyExecutionData *edata;
	EState	   *estate;
	LogicalRepTupleData oldtup;
	LogicalRepTupleData newtup;
	bool		has_oldtup;
	TupleTableSlot *remoteslot;
	RTEPermissionInfo *target_perminfo;
	MemoryContext oldctx;
	bool		run_as_owner;

	/*
	 * Quick return if we are skipping data modification changes or handling
	 * streamed transactions.
	 */
	if (is_skipping_changes() ||
		handle_streamed_transaction(LOGICAL_REP_MSG_UPDATE, s) ||
		handle_delayed_transaction(LOGICAL_REP_MSG_UPDATE, s))
		return;

	begin_replication_step();

	relid = logicalrep_read_update(s, &has_oldtup, &oldtup,
								   &newtup);
	rel = logicalrep_rel_open(relid, RowExclusiveLock);
	if (!should_apply_changes_for_rel(rel))
	{
		/*
		 * The relation can't become interesting in the middle of the
		 * transaction so it's safe to unlock it.
		 */
		logicalrep_rel_close(rel, RowExclusiveLock);
		end_replication_step();
		return;
	}

	/* Set relation for error callback */
	apply_error_callback_arg.rel = rel;

	/* Check if we can do the update. */
	check_relation_updatable(rel);

	/*
	 * Make sure that any user-supplied code runs as the table owner, unless
	 * the user has opted out of that behavior.
	 */
	run_as_owner = MySubscription->runasowner;
	if (!run_as_owner)
		SwitchToUntrustedUser(rel->localrel->rd_rel->relowner, &ucxt);

	/* Initialize the executor state. */
	edata = create_edata_for_relation(rel);
	estate = edata->estate;
	remoteslot = ExecInitExtraTupleSlot(estate,
										RelationGetDescr(rel->localrel),
										&TTSOpsVirtual);

	/*
	 * Populate updatedCols so that per-column triggers can fire, and so
	 * executor can correctly pass down indexUnchanged hint.  This could
	 * include more columns than were actually changed on the publisher
	 * because the logical replication protocol doesn't contain that
	 * information.  But it would for example exclude columns that only exist
	 * on the subscriber, since we are not touching those.
	 */
	target_perminfo = list_nth(estate->es_rteperminfos, 0);
	for (int i = 0; i < remoteslot->tts_tupleDescriptor->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(remoteslot->tts_tupleDescriptor, i);
		int			remoteattnum = rel->attrmap->attnums[i];

		if (!att->attisdropped && remoteattnum >= 0)
		{
			Assert(remoteattnum < newtup.ncols);
			if (newtup.colstatus[remoteattnum] != LOGICALREP_COLUMN_UNCHANGED)
				target_perminfo->updatedCols =
					bms_add_member(target_perminfo->updatedCols,
								   i + 1 - FirstLowInvalidHeapAttributeNumber);
		}
	}

	/* Build the search tuple. */
	oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
	slot_store_data(remoteslot, rel,
					has_oldtup ? &oldtup : &newtup);
	MemoryContextSwitchTo(oldctx);

	/* For a partitioned table, apply update to correct partition. */
	if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		apply_handle_tuple_routing(edata,
								   remoteslot, &newtup, CMD_UPDATE);
	else
		apply_handle_update_internal(edata, edata->targetRelInfo,
									 remoteslot, &newtup, rel->localindexoid);

	finish_edata(edata);

	/* Reset relation for error callback */
	apply_error_callback_arg.rel = NULL;

	if (!run_as_owner)
		RestoreUserContext(&ucxt);

	logicalrep_rel_close(rel, NoLock);

	end_replication_step();
}

/*
 * Workhorse for apply_handle_update()
 * relinfo is for the relation we're actually updating in
 * (could be a child partition of edata->targetRelInfo)
 */
static void
apply_handle_update_internal(ApplyExecutionData *edata,
							 ResultRelInfo *relinfo,
							 TupleTableSlot *remoteslot,
							 LogicalRepTupleData *newtup,
							 Oid localindexoid)
{
	EState	   *estate = edata->estate;
	LogicalRepRelMapEntry *relmapentry = edata->targetRel;
	Relation	localrel = relinfo->ri_RelationDesc;
	EPQState	epqstate;
	TupleTableSlot *localslot;
	bool		found;
	MemoryContext oldctx;

	EvalPlanQualInit(&epqstate, estate, NULL, NIL, -1);
	ExecOpenIndices(relinfo, false);

	found = FindReplTupleInLocalRel(estate, localrel,
									&relmapentry->remoterel,
									localindexoid,
									remoteslot, &localslot);
	ExecClearTuple(remoteslot);

	/*
	 * Tuple found.
	 *
	 * Note this will fail if there are other conflicting unique indexes.
	 */
	if (found)
	{
		/* Process and store remote tuple in the slot */
		oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
		slot_modify_data(remoteslot, localslot, relmapentry, newtup);
		MemoryContextSwitchTo(oldctx);

		EvalPlanQualSetSlot(&epqstate, remoteslot);

		/* Do the actual update. */
		TargetPrivilegesCheck(relinfo->ri_RelationDesc, ACL_UPDATE);
		ExecSimpleRelationUpdate(relinfo, estate, &epqstate, localslot,
								 remoteslot);
	}
	else
	{
		/*
		 * The tuple to be updated could not be found.  Do nothing except for
		 * emitting a log message.
		 *
		 * XXX should this be promoted to ereport(LOG) perhaps?
		 */
		elog(DEBUG1,
			 "logical replication did not find row to be updated "
			 "in replication target relation \"%s\"",
			 RelationGetRelationName(localrel));
	}

	/* Cleanup. */
	ExecCloseIndices(relinfo);
	EvalPlanQualEnd(&epqstate);
}

/*
 * Handle DELETE message.
 *
 * TODO: FDW support
 */
static void
apply_handle_delete(StringInfo s)
{
	LogicalRepRelMapEntry *rel;
	LogicalRepTupleData oldtup;
	LogicalRepRelId relid;
	UserContext		ucxt;
	ApplyExecutionData *edata;
	EState	   *estate;
	TupleTableSlot *remoteslot;
	MemoryContext oldctx;
	bool		run_as_owner;

	/*
	 * Quick return if we are skipping data modification changes or handling
	 * streamed transactions.
	 */
	if (is_skipping_changes() ||
		handle_streamed_transaction(LOGICAL_REP_MSG_DELETE, s) ||
		handle_delayed_transaction(LOGICAL_REP_MSG_DELETE, s))
		return;

	begin_replication_step();

	relid = logicalrep_read_delete(s, &oldtup);
	rel = logicalrep_rel_open(relid, RowExclusiveLock);
	if (!should_apply_changes_for_rel(rel))
	{
		/*
		 * The relation can't become interesting in the middle of the
		 * transaction so it's safe to unlock it.
		 */
		logicalrep_rel_close(rel, RowExclusiveLock);
		end_replication_step();
		return;
	}

	/* Set relation for error callback */
	apply_error_callback_arg.rel = rel;

	/* Check if we can do the delete. */
	check_relation_updatable(rel);

	/*
	 * Make sure that any user-supplied code runs as the table owner, unless
	 * the user has opted out of that behavior.
	 */
	run_as_owner = MySubscription->runasowner;
	if (!run_as_owner)
		SwitchToUntrustedUser(rel->localrel->rd_rel->relowner, &ucxt);

	/* Initialize the executor state. */
	edata = create_edata_for_relation(rel);
	estate = edata->estate;
	remoteslot = ExecInitExtraTupleSlot(estate,
										RelationGetDescr(rel->localrel),
										&TTSOpsVirtual);

	/* Build the search tuple. */
	oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
	slot_store_data(remoteslot, rel, &oldtup);
	MemoryContextSwitchTo(oldctx);

	/* For a partitioned table, apply delete to correct partition. */
	if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		apply_handle_tuple_routing(edata,
								   remoteslot, NULL, CMD_DELETE);
	else
		apply_handle_delete_internal(edata, edata->targetRelInfo,
									 remoteslot, rel->localindexoid);

	finish_edata(edata);

	/* Reset relation for error callback */
	apply_error_callback_arg.rel = NULL;

	if (!run_as_owner)
		RestoreUserContext(&ucxt);

	logicalrep_rel_close(rel, NoLock);

	end_replication_step();
}

/*
 * Workhorse for apply_handle_delete()
 * relinfo is for the relation we're actually deleting from
 * (could be a child partition of edata->targetRelInfo)
 */
static void
apply_handle_delete_internal(ApplyExecutionData *edata,
							 ResultRelInfo *relinfo,
							 TupleTableSlot *remoteslot,
							 Oid localindexoid)
{
	EState	   *estate = edata->estate;
	Relation	localrel = relinfo->ri_RelationDesc;
	LogicalRepRelation *remoterel = &edata->targetRel->remoterel;
	EPQState	epqstate;
	TupleTableSlot *localslot;
	bool		found;

	EvalPlanQualInit(&epqstate, estate, NULL, NIL, -1);
	ExecOpenIndices(relinfo, false);

	found = FindReplTupleInLocalRel(estate, localrel, remoterel, localindexoid,
									remoteslot, &localslot);

	/* If found delete it. */
	if (found)
	{
		EvalPlanQualSetSlot(&epqstate, localslot);

		/* Do the actual delete. */
		TargetPrivilegesCheck(relinfo->ri_RelationDesc, ACL_DELETE);
		ExecSimpleRelationDelete(relinfo, estate, &epqstate, localslot);
	}
	else
	{
		/*
		 * The tuple to be deleted could not be found.  Do nothing except for
		 * emitting a log message.
		 *
		 * XXX should this be promoted to ereport(LOG) perhaps?
		 */
		elog(DEBUG1,
			 "logical replication did not find row to be deleted "
			 "in replication target relation \"%s\"",
			 RelationGetRelationName(localrel));
	}

	/* Cleanup. */
	ExecCloseIndices(relinfo);
	EvalPlanQualEnd(&epqstate);
}

/*
 * Try to find a tuple received from the publication side (in 'remoteslot') in
 * the corresponding local relation using either replica identity index,
 * primary key, index or if needed, sequential scan.
 *
 * Local tuple, if found, is returned in '*localslot'.
 */
static bool
FindReplTupleInLocalRel(EState *estate, Relation localrel,
						LogicalRepRelation *remoterel,
						Oid localidxoid,
						TupleTableSlot *remoteslot,
						TupleTableSlot **localslot)
{
	bool		found;

	/*
	 * Regardless of the top-level operation, we're performing a read here, so
	 * check for SELECT privileges.
	 */
	TargetPrivilegesCheck(localrel, ACL_SELECT);

	*localslot = table_slot_create(localrel, &estate->es_tupleTable);

	Assert(OidIsValid(localidxoid) ||
		   (remoterel->replident == REPLICA_IDENTITY_FULL));

	if (OidIsValid(localidxoid))
		found = RelationFindReplTupleByIndex(localrel, localidxoid,
											 LockTupleExclusive,
											 remoteslot, *localslot);
	else
		found = RelationFindReplTupleSeq(localrel, LockTupleExclusive,
										 remoteslot, *localslot);

	return found;
}

/*
 * This handles insert, update, delete on a partitioned table.
 */
static void
apply_handle_tuple_routing(ApplyExecutionData *edata,
						   TupleTableSlot *remoteslot,
						   LogicalRepTupleData *newtup,
						   CmdType operation)
{
	EState	   *estate = edata->estate;
	LogicalRepRelMapEntry *relmapentry = edata->targetRel;
	ResultRelInfo *relinfo = edata->targetRelInfo;
	Relation	parentrel = relinfo->ri_RelationDesc;
	ModifyTableState *mtstate;
	PartitionTupleRouting *proute;
	ResultRelInfo *partrelinfo;
	Relation	partrel;
	TupleTableSlot *remoteslot_part;
	TupleConversionMap *map;
	MemoryContext oldctx;
	LogicalRepRelMapEntry *part_entry = NULL;
	AttrMap    *attrmap = NULL;

	/* ModifyTableState is needed for ExecFindPartition(). */
	edata->mtstate = mtstate = makeNode(ModifyTableState);
	mtstate->ps.plan = NULL;
	mtstate->ps.state = estate;
	mtstate->operation = operation;
	mtstate->resultRelInfo = relinfo;

	/* ... as is PartitionTupleRouting. */
	edata->proute = proute = ExecSetupPartitionTupleRouting(estate, parentrel);

	/*
	 * Find the partition to which the "search tuple" belongs.
	 */
	Assert(remoteslot != NULL);
	oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
	partrelinfo = ExecFindPartition(mtstate, relinfo, proute,
									remoteslot, estate);
	Assert(partrelinfo != NULL);
	partrel = partrelinfo->ri_RelationDesc;

	/*
	 * Check for supported relkind.  We need this since partitions might be of
	 * unsupported relkinds; and the set of partitions can change, so checking
	 * at CREATE/ALTER SUBSCRIPTION would be insufficient.
	 */
	CheckSubscriptionRelkind(partrel->rd_rel->relkind,
							 get_namespace_name(RelationGetNamespace(partrel)),
							 RelationGetRelationName(partrel));

	/*
	 * To perform any of the operations below, the tuple must match the
	 * partition's rowtype. Convert if needed or just copy, using a dedicated
	 * slot to store the tuple in any case.
	 */
	remoteslot_part = partrelinfo->ri_PartitionTupleSlot;
	if (remoteslot_part == NULL)
		remoteslot_part = table_slot_create(partrel, &estate->es_tupleTable);
	map = ExecGetRootToChildMap(partrelinfo, estate);
	if (map != NULL)
	{
		attrmap = map->attrMap;
		remoteslot_part = execute_attr_map_slot(attrmap, remoteslot,
												remoteslot_part);
	}
	else
	{
		remoteslot_part = ExecCopySlot(remoteslot_part, remoteslot);
		slot_getallattrs(remoteslot_part);
	}
	MemoryContextSwitchTo(oldctx);

	/* Check if we can do the update or delete on the leaf partition. */
	if (operation == CMD_UPDATE || operation == CMD_DELETE)
	{
		part_entry = logicalrep_partition_open(relmapentry, partrel,
											   attrmap);
		check_relation_updatable(part_entry);
	}

	switch (operation)
	{
		case CMD_INSERT:
			apply_handle_insert_internal(edata, partrelinfo,
										 remoteslot_part);
			break;

		case CMD_DELETE:
			apply_handle_delete_internal(edata, partrelinfo,
										 remoteslot_part,
										 part_entry->localindexoid);
			break;

		case CMD_UPDATE:

			/*
			 * For UPDATE, depending on whether or not the updated tuple
			 * satisfies the partition's constraint, perform a simple UPDATE
			 * of the partition or move the updated tuple into a different
			 * suitable partition.
			 */
			{
				TupleTableSlot *localslot;
				ResultRelInfo *partrelinfo_new;
				Relation	partrel_new;
				bool		found;

				/* Get the matching local tuple from the partition. */
				found = FindReplTupleInLocalRel(estate, partrel,
												&part_entry->remoterel,
												part_entry->localindexoid,
												remoteslot_part, &localslot);
				if (!found)
				{
					/*
					 * The tuple to be updated could not be found.  Do nothing
					 * except for emitting a log message.
					 *
					 * XXX should this be promoted to ereport(LOG) perhaps?
					 */
					elog(DEBUG1,
						 "logical replication did not find row to be updated "
						 "in replication target relation's partition \"%s\"",
						 RelationGetRelationName(partrel));
					return;
				}

				/*
				 * Apply the update to the local tuple, putting the result in
				 * remoteslot_part.
				 */
				oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
				slot_modify_data(remoteslot_part, localslot, part_entry,
								 newtup);
				MemoryContextSwitchTo(oldctx);

				/*
				 * Does the updated tuple still satisfy the current
				 * partition's constraint?
				 */
				if (!partrel->rd_rel->relispartition ||
					ExecPartitionCheck(partrelinfo, remoteslot_part, estate,
									   false))
				{
					/*
					 * Yes, so simply UPDATE the partition.  We don't call
					 * apply_handle_update_internal() here, which would
					 * normally do the following work, to avoid repeating some
					 * work already done above to find the local tuple in the
					 * partition.
					 */
					EPQState	epqstate;

					EvalPlanQualInit(&epqstate, estate, NULL, NIL, -1);
					ExecOpenIndices(partrelinfo, false);

					EvalPlanQualSetSlot(&epqstate, remoteslot_part);
					TargetPrivilegesCheck(partrelinfo->ri_RelationDesc,
										  ACL_UPDATE);
					ExecSimpleRelationUpdate(partrelinfo, estate, &epqstate,
											 localslot, remoteslot_part);
					ExecCloseIndices(partrelinfo);
					EvalPlanQualEnd(&epqstate);
				}
				else
				{
					/* Move the tuple into the new partition. */

					/*
					 * New partition will be found using tuple routing, which
					 * can only occur via the parent table.  We might need to
					 * convert the tuple to the parent's rowtype.  Note that
					 * this is the tuple found in the partition, not the
					 * original search tuple received by this function.
					 */
					if (map)
					{
						TupleConversionMap *PartitionToRootMap =
						convert_tuples_by_name(RelationGetDescr(partrel),
											   RelationGetDescr(parentrel));

						remoteslot =
							execute_attr_map_slot(PartitionToRootMap->attrMap,
												  remoteslot_part, remoteslot);
					}
					else
					{
						remoteslot = ExecCopySlot(remoteslot, remoteslot_part);
						slot_getallattrs(remoteslot);
					}

					/* Find the new partition. */
					oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
					partrelinfo_new = ExecFindPartition(mtstate, relinfo,
														proute, remoteslot,
														estate);
					MemoryContextSwitchTo(oldctx);
					Assert(partrelinfo_new != partrelinfo);
					partrel_new = partrelinfo_new->ri_RelationDesc;

					/* Check that new partition also has supported relkind. */
					CheckSubscriptionRelkind(partrel_new->rd_rel->relkind,
											 get_namespace_name(RelationGetNamespace(partrel_new)),
											 RelationGetRelationName(partrel_new));

					/* DELETE old tuple found in the old partition. */
					apply_handle_delete_internal(edata, partrelinfo,
												 localslot,
												 part_entry->localindexoid);

					/* INSERT new tuple into the new partition. */

					/*
					 * Convert the replacement tuple to match the destination
					 * partition rowtype.
					 */
					oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
					remoteslot_part = partrelinfo_new->ri_PartitionTupleSlot;
					if (remoteslot_part == NULL)
						remoteslot_part = table_slot_create(partrel_new,
															&estate->es_tupleTable);
					map = ExecGetRootToChildMap(partrelinfo_new, estate);
					if (map != NULL)
					{
						remoteslot_part = execute_attr_map_slot(map->attrMap,
																remoteslot,
																remoteslot_part);
					}
					else
					{
						remoteslot_part = ExecCopySlot(remoteslot_part,
													   remoteslot);
						slot_getallattrs(remoteslot);
					}
					MemoryContextSwitchTo(oldctx);
					apply_handle_insert_internal(edata, partrelinfo_new,
												 remoteslot_part);
				}
			}
			break;

		default:
			elog(ERROR, "unrecognized CmdType: %d", (int) operation);
			break;
	}
}

/*
 * Handle TRUNCATE message.
 *
 * TODO: FDW support
 */
static void
apply_handle_truncate(StringInfo s)
{
	bool		cascade = false;
	bool		restart_seqs = false;
	List	   *remote_relids = NIL;
	List	   *remote_rels = NIL;
	List	   *rels = NIL;
	List	   *part_rels = NIL;
	List	   *relids = NIL;
	List	   *relids_logged = NIL;
	ListCell   *lc;
	LOCKMODE	lockmode = AccessExclusiveLock;

	/*
	 * Quick return if we are skipping data modification changes or handling
	 * streamed transactions.
	 */
	if (is_skipping_changes() ||
		handle_streamed_transaction(LOGICAL_REP_MSG_TRUNCATE, s) ||
		handle_delayed_transaction(LOGICAL_REP_MSG_TRUNCATE, s))
		return;

	begin_replication_step();

	remote_relids = logicalrep_read_truncate(s, &cascade, &restart_seqs);

	foreach(lc, remote_relids)
	{
		LogicalRepRelId relid = lfirst_oid(lc);
		LogicalRepRelMapEntry *rel;

		rel = logicalrep_rel_open(relid, lockmode);
		if (!should_apply_changes_for_rel(rel))
		{
			/*
			 * The relation can't become interesting in the middle of the
			 * transaction so it's safe to unlock it.
			 */
			logicalrep_rel_close(rel, lockmode);
			continue;
		}

		remote_rels = lappend(remote_rels, rel);
		TargetPrivilegesCheck(rel->localrel, ACL_TRUNCATE);
		rels = lappend(rels, rel->localrel);
		relids = lappend_oid(relids, rel->localreloid);
		if (RelationIsLogicallyLogged(rel->localrel))
			relids_logged = lappend_oid(relids_logged, rel->localreloid);

		/*
		 * Truncate partitions if we got a message to truncate a partitioned
		 * table.
		 */
		if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			ListCell   *child;
			List	   *children = find_all_inheritors(rel->localreloid,
													   lockmode,
													   NULL);

			foreach(child, children)
			{
				Oid			childrelid = lfirst_oid(child);
				Relation	childrel;

				if (list_member_oid(relids, childrelid))
					continue;

				/* find_all_inheritors already got lock */
				childrel = table_open(childrelid, NoLock);

				/*
				 * Ignore temp tables of other backends.  See similar code in
				 * ExecuteTruncate().
				 */
				if (RELATION_IS_OTHER_TEMP(childrel))
				{
					table_close(childrel, lockmode);
					continue;
				}

				TargetPrivilegesCheck(childrel, ACL_TRUNCATE);
				rels = lappend(rels, childrel);
				part_rels = lappend(part_rels, childrel);
				relids = lappend_oid(relids, childrelid);
				/* Log this relation only if needed for logical decoding */
				if (RelationIsLogicallyLogged(childrel))
					relids_logged = lappend_oid(relids_logged, childrelid);
			}
		}
	}

	/*
	 * Even if we used CASCADE on the upstream primary we explicitly default
	 * to replaying changes without further cascading. This might be later
	 * changeable with a user specified option.
	 *
	 * MySubscription->runasowner tells us whether we want to execute
	 * replication actions as the subscription owner; the last argument to
	 * TruncateGuts tells it whether we want to switch to the table owner.
	 * Those are exactly opposite conditions.
	 */
	ExecuteTruncateGuts(rels,
						relids,
						relids_logged,
						DROP_RESTRICT,
						restart_seqs,
						!MySubscription->runasowner);
	foreach(lc, remote_rels)
	{
		LogicalRepRelMapEntry *rel = lfirst(lc);

		logicalrep_rel_close(rel, NoLock);
	}
	foreach(lc, part_rels)
	{
		Relation	rel = lfirst(lc);

		table_close(rel, NoLock);
	}

	end_replication_step();
}


/*
 * Logical replication protocol message dispatcher.
 */
void
apply_dispatch(StringInfo s)
{
	LogicalRepMsgType action = pq_getmsgbyte(s);
	LogicalRepMsgType saved_command;

	/*
	 * Set the current command being applied. Since this function can be
	 * called recursively when applying spooled changes, save the current
	 * command.
	 */
	saved_command = apply_error_callback_arg.command;
	apply_error_callback_arg.command = action;

	switch (action)
	{
		case LOGICAL_REP_MSG_BEGIN:
			apply_handle_begin(s);
			break;

		case LOGICAL_REP_MSG_COMMIT:
			apply_handle_commit(s);
			break;

		case LOGICAL_REP_MSG_INSERT:
			apply_handle_insert(s);
			break;

		case LOGICAL_REP_MSG_UPDATE:
			apply_handle_update(s);
			break;

		case LOGICAL_REP_MSG_DELETE:
			apply_handle_delete(s);
			break;

		case LOGICAL_REP_MSG_TRUNCATE:
			apply_handle_truncate(s);
			break;

		case LOGICAL_REP_MSG_RELATION:
			apply_handle_relation(s);
			break;

		case LOGICAL_REP_MSG_TYPE:
			apply_handle_type(s);
			break;

		case LOGICAL_REP_MSG_ORIGIN:
			apply_handle_origin(s);
			break;

		case LOGICAL_REP_MSG_MESSAGE:

			/*
			 * Logical replication does not use generic logical messages yet.
			 * Although, it could be used by other applications that use this
			 * output plugin.
			 */
			break;

		case LOGICAL_REP_MSG_STREAM_START:
			apply_handle_stream_start(s);
			break;

		case LOGICAL_REP_MSG_STREAM_STOP:
			apply_handle_stream_stop(s);
			break;

		case LOGICAL_REP_MSG_STREAM_ABORT:
			apply_handle_stream_abort(s);
			break;

		case LOGICAL_REP_MSG_STREAM_COMMIT:
			apply_handle_stream_commit(s);
			break;

		case LOGICAL_REP_MSG_BEGIN_PREPARE:
			apply_handle_begin_prepare(s);
			break;

		case LOGICAL_REP_MSG_PREPARE:
			apply_handle_prepare(s);
			break;

		case LOGICAL_REP_MSG_COMMIT_PREPARED:
			apply_handle_commit_prepared(s);
			break;

		case LOGICAL_REP_MSG_ROLLBACK_PREPARED:
			apply_handle_rollback_prepared(s);
			break;

		case LOGICAL_REP_MSG_STREAM_PREPARE:
			apply_handle_stream_prepare(s);
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid logical replication message type \"%c\"", action)));
	}

	/* Reset the current command */
	apply_error_callback_arg.command = saved_command;
}

/*
 * Figure out which write/flush positions to report to the walsender process.
 *
 * We can't simply report back the last LSN the walsender sent us because the
 * local transaction might not yet be flushed to disk locally. Instead we
 * build a list that associates local with remote LSNs for every commit. When
 * reporting back the flush position to the sender we iterate that list and
 * check which entries on it are already locally flushed. Those we can report
 * as having been flushed.
 *
 * The have_pending_txes is true if there are outstanding transactions that
 * need to be flushed.
 */
static void
get_flush_position(XLogRecPtr *write, XLogRecPtr *flush,
				   bool *have_pending_txes)
{
	dlist_mutable_iter iter;
	XLogRecPtr	local_flush = GetFlushRecPtr(NULL);

	*write = InvalidXLogRecPtr;
	*flush = InvalidXLogRecPtr;

	dlist_foreach_modify(iter, &lsn_mapping)
	{
		FlushPosition *pos =
		dlist_container(FlushPosition, node, iter.cur);

		*write = pos->remote_end;

		if (pos->local_end <= local_flush)
		{
			*flush = pos->remote_end;
			dlist_delete(iter.cur);
			pfree(pos);
		}
		else
		{
			/*
			 * Don't want to uselessly iterate over the rest of the list which
			 * could potentially be long. Instead get the last element and
			 * grab the write position from there.
			 */
			pos = dlist_tail_element(FlushPosition, node,
									 &lsn_mapping);
			*write = pos->remote_end;
			break;
		}
	}

	/* If change are written into file, report the LSN instead */
	if (last_flushed > *flush)
		*flush = last_flushed;

	*have_pending_txes = !dlist_is_empty(&lsn_mapping);
}

/*
 * Store current remote/local lsn pair in the tracking list.
 */
void
store_flush_position(XLogRecPtr remote_lsn, XLogRecPtr local_lsn)
{
	FlushPosition *flushpos;

	/*
	 * Skip for parallel apply workers, because the lsn_mapping is maintained
	 * by the leader apply worker.
	 */
	if (am_parallel_apply_worker())
		return;

	/* Need to do this in permanent context */
	MemoryContextSwitchTo(ApplyContext);

	/* Track commit lsn  */
	flushpos = (FlushPosition *) palloc(sizeof(FlushPosition));
	flushpos->local_end = local_lsn;
	flushpos->remote_end = remote_lsn;

	dlist_push_tail(&lsn_mapping, &flushpos->node);
	MemoryContextSwitchTo(ApplyMessageContext);
}


/* Update statistics of the worker. */
static void
UpdateWorkerStats(XLogRecPtr last_lsn, TimestampTz send_time, bool reply)
{
	MyLogicalRepWorker->last_lsn = last_lsn;
	MyLogicalRepWorker->last_send_time = send_time;
	MyLogicalRepWorker->last_recv_time = GetCurrentTimestamp();
	if (reply)
	{
		MyLogicalRepWorker->reply_lsn = last_lsn;
		MyLogicalRepWorker->reply_time = send_time;
	}
}

/*
 * Apply main loop.
 */
static void
LogicalRepApplyLoop(XLogRecPtr last_received)
{
	TimestampTz last_recv_timestamp = GetCurrentTimestamp();
	bool		ping_sent = false;
	TimeLineID	tli;
	ErrorContextCallback errcallback;

	/*
	 * Init the ApplyMessageContext which we clean up after each replication
	 * protocol message.
	 */
	ApplyMessageContext = AllocSetContextCreate(ApplyContext,
												"ApplyMessageContext",
												ALLOCSET_DEFAULT_SIZES);

	/*
	 * This memory context is used for per-stream data when the streaming mode
	 * is enabled. This context is reset on each stream stop.
	 */
	LogicalStreamingContext = AllocSetContextCreate(ApplyContext,
													"LogicalStreamingContext",
													ALLOCSET_DEFAULT_SIZES);

	/* mark as idle, before starting to loop */
	pgstat_report_activity(STATE_IDLE, NULL);

	/*
	 * Push apply error context callback. Fields will be filled while applying
	 * a change.
	 */
	errcallback.callback = apply_error_callback;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;
	apply_error_context_stack = error_context_stack;

	/* This outer loop iterates once per wait. */
	for (;;)
	{
		pgsocket	fd = PGINVALID_SOCKET;
		int			rc;
		int			len;
		char	   *buf = NULL;
		bool		endofstream = false;
		long		wait_time;

		CHECK_FOR_INTERRUPTS();

		MemoryContextSwitchTo(ApplyMessageContext);

		len = walrcv_receive(LogRepWorkerWalRcvConn, &buf, &fd);

		if (len != 0)
		{
			/* Loop to process all available data (without blocking). */
			for (;;)
			{
				CHECK_FOR_INTERRUPTS();

				if (len == 0)
				{
					break;
				}
				else if (len < 0)
				{
					ereport(LOG,
							(errmsg("data stream from publisher has ended")));
					endofstream = true;
					break;
				}
				else
				{
					int			c;
					StringInfoData s;

					/* Reset timeout. */
					last_recv_timestamp = GetCurrentTimestamp();
					ping_sent = false;

					/* Ensure we are reading the data into our memory context. */
					MemoryContextSwitchTo(ApplyMessageContext);

					s.data = buf;
					s.len = len;
					s.cursor = 0;
					s.maxlen = -1;

					c = pq_getmsgbyte(&s);

					if (c == 'w')
					{
						XLogRecPtr	start_lsn;
						XLogRecPtr	end_lsn;
						TimestampTz send_time;

						start_lsn = pq_getmsgint64(&s);
						end_lsn = pq_getmsgint64(&s);
						send_time = pq_getmsgint64(&s);

						if (last_received < start_lsn)
							last_received = start_lsn;

						if (last_received < end_lsn)
							last_received = end_lsn;

						UpdateWorkerStats(last_received, send_time, false);

						apply_dispatch(&s);
					}
					else if (c == 'k')
					{
						XLogRecPtr	end_lsn;
						TimestampTz timestamp;
						bool		reply_requested;

						end_lsn = pq_getmsgint64(&s);
						timestamp = pq_getmsgint64(&s);
						reply_requested = pq_getmsgbyte(&s);

						if (last_received < end_lsn)
							last_received = end_lsn;

						send_feedback(last_received, reply_requested, false);
						UpdateWorkerStats(last_received, timestamp, true);
					}
					/* other message types are purposefully ignored */

					MemoryContextReset(ApplyMessageContext);
				}

				len = walrcv_receive(LogRepWorkerWalRcvConn, &buf, &fd);
			}
		}

		/* confirm all writes so far */
		send_feedback(last_received, false, false);

		if (!in_remote_transaction && !in_streamed_transaction)
		{
			/*
			 * If we didn't get any transactions for a while there might be
			 * unconsumed invalidation messages in the queue, consume them
			 * now.
			 */
			AcceptInvalidationMessages();
			maybe_reread_subscription();

			/* Process any table synchronization changes. */
			if (list_length(DelayedTxnList) == 0)
				process_syncing_tables(last_received);
		}

		/* Check delayed transactions and apply them */
		check_delayed_transaction();

		/* Cleanup the memory. */
		MemoryContextResetAndDeleteChildren(ApplyMessageContext);
		MemoryContextSwitchTo(TopMemoryContext);

		/* Check if we need to exit the streaming loop. */
		if (endofstream)
			break;

		/*
		 * Wait for more data or latch.  If we have unflushed transactions,
		 * wake up after WalWriterDelay to see if they've been flushed yet (in
		 * which case we should send a feedback message).  Otherwise, there's
		 * no particular urgency about waking up unless we get data or a
		 * signal.
		 */
		if (!dlist_is_empty(&lsn_mapping))
			wait_time = WalWriterDelay;
		else
			wait_time = NAPTIME_PER_CYCLE;

		rc = WaitLatchOrSocket(MyLatch,
							   WL_SOCKET_READABLE | WL_LATCH_SET |
							   WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							   fd, wait_time,
							   WAIT_EVENT_LOGICAL_APPLY_MAIN);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (rc & WL_TIMEOUT)
		{
			/*
			 * We didn't receive anything new. If we haven't heard anything
			 * from the server for more than wal_receiver_timeout / 2, ping
			 * the server. Also, if it's been longer than
			 * wal_receiver_status_interval since the last update we sent,
			 * send a status update to the primary anyway, to report any
			 * progress in applying WAL.
			 */
			bool		requestReply = false;

			/*
			 * Check if time since last receive from primary has reached the
			 * configured limit.
			 */
			if (wal_receiver_timeout > 0)
			{
				TimestampTz now = GetCurrentTimestamp();
				TimestampTz timeout;

				timeout =
					TimestampTzPlusMilliseconds(last_recv_timestamp,
												wal_receiver_timeout);

				if (now >= timeout)
					ereport(ERROR,
							(errcode(ERRCODE_CONNECTION_FAILURE),
							 errmsg("terminating logical replication worker due to timeout")));

				/* Check to see if it's time for a ping. */
				if (!ping_sent)
				{
					timeout = TimestampTzPlusMilliseconds(last_recv_timestamp,
														  (wal_receiver_timeout / 2));
					if (now >= timeout)
					{
						requestReply = true;
						ping_sent = true;
					}
				}
			}

			send_feedback(last_received, requestReply, requestReply);

			/*
			 * Force reporting to ensure long idle periods don't lead to
			 * arbitrarily delayed stats. Stats can only be reported outside
			 * of (implicit or explicit) transactions. That shouldn't lead to
			 * stats being delayed for long, because transactions are either
			 * sent as a whole on commit or streamed. Streamed transactions
			 * are spilled to disk and applied on commit.
			 */
			if (!IsTransactionState())
				pgstat_report_stat(true);
		}
	}

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
	apply_error_context_stack = error_context_stack;

	/* All done */
	walrcv_endstreaming(LogRepWorkerWalRcvConn, &tli);
}

/*
 * Send a Standby Status Update message to server.
 *
 * 'recvpos' is the latest LSN we've received data to, force is set if we need
 * to send a response to avoid timeouts.
 */
static void
send_feedback(XLogRecPtr recvpos, bool force, bool requestReply)
{
	static StringInfo reply_message = NULL;
	static TimestampTz send_time = 0;

	static XLogRecPtr last_recvpos = InvalidXLogRecPtr;
	static XLogRecPtr last_writepos = InvalidXLogRecPtr;
	static XLogRecPtr last_flushpos = InvalidXLogRecPtr;

	XLogRecPtr	writepos;
	XLogRecPtr	flushpos;
	TimestampTz now;
	bool		have_pending_txes;

	/*
	 * If the user doesn't want status to be reported to the publisher, be
	 * sure to exit before doing anything at all.
	 */
	if (!force && wal_receiver_status_interval <= 0)
		return;

	/* It's legal to not pass a recvpos */
	if (recvpos < last_recvpos)
		recvpos = last_recvpos;

	get_flush_position(&writepos, &flushpos, &have_pending_txes);

	/*
	 * No outstanding transactions to flush, we can report the latest received
	 * position. This is important for synchronous replication.
	 *
	 * If the logical replication subscription has unprocessed changes then do
	 * not inform the publisher that the received latest LSN is already
	 * applied and flushed, otherwise, the publisher will make a wrong
	 * assumption about the logical replication progress. Instead, just send a
	 * feedback message to avoid a replication timeout during the delay.
	 */
	if (!have_pending_txes && (list_length(DelayedTxnList) == 0))
		flushpos = writepos = recvpos;

	if (writepos < last_writepos)
		writepos = last_writepos;

	if (flushpos < last_flushpos)
		flushpos = last_flushpos;

	now = GetCurrentTimestamp();

	/* if we've already reported everything we're good */
	if (!force &&
		writepos == last_writepos &&
		flushpos == last_flushpos &&
		!TimestampDifferenceExceeds(send_time, now,
									wal_receiver_status_interval * 1000))
		return;
	send_time = now;

	if (!reply_message)
	{
		MemoryContext oldctx = MemoryContextSwitchTo(ApplyContext);

		reply_message = makeStringInfo();
		MemoryContextSwitchTo(oldctx);
	}
	else
		resetStringInfo(reply_message);

	pq_sendbyte(reply_message, 'r');
	pq_sendint64(reply_message, recvpos);	/* write */
	pq_sendint64(reply_message, flushpos);	/* flush */
	pq_sendint64(reply_message, writepos);	/* apply */
	pq_sendint64(reply_message, now);	/* sendTime */
	pq_sendbyte(reply_message, requestReply);	/* replyRequested */

	elog(DEBUG2, "sending feedback (force %d) to recv %X/%X, write %X/%X, flush %X/%X",
		 force,
		 LSN_FORMAT_ARGS(recvpos),
		 LSN_FORMAT_ARGS(writepos),
		 LSN_FORMAT_ARGS(flushpos));

	walrcv_send(LogRepWorkerWalRcvConn,
				reply_message->data, reply_message->len);

	if (recvpos > last_recvpos)
		last_recvpos = recvpos;
	if (writepos > last_writepos)
		last_writepos = writepos;
	if (flushpos > last_flushpos)
		last_flushpos = flushpos;
}

/*
 * Exit routine for apply workers due to subscription parameter changes.
 */
static void
apply_worker_exit(void)
{
	if (am_parallel_apply_worker())
	{
		/*
		 * Don't stop the parallel apply worker as the leader will detect the
		 * subscription parameter change and restart logical replication later
		 * anyway. This also prevents the leader from reporting errors when
		 * trying to communicate with a stopped parallel apply worker, which
		 * would accidentally disable subscriptions if disable_on_error was
		 * set.
		 */
		return;
	}

	/*
	 * Reset the last-start time for this apply worker so that the launcher
	 * will restart it without waiting for wal_retrieve_retry_interval if the
	 * subscription is still active, and so that we won't leak that hash table
	 * entry if it isn't.
	 */
	if (!am_tablesync_worker())
		ApplyLauncherForgetWorkerStartTime(MyLogicalRepWorker->subid);

	proc_exit(0);
}

/*
 * Reread subscription info if needed. Most changes will be exit.
 */
void
maybe_reread_subscription(void)
{
	MemoryContext oldctx;
	Subscription *newsub;
	bool		started_tx = false;

	/* When cache state is valid there is nothing to do here. */
	if (MySubscriptionValid)
		return;

	/* This function might be called inside or outside of transaction. */
	if (!IsTransactionState())
	{
		StartTransactionCommand();
		started_tx = true;
	}

	/* Ensure allocations in permanent context. */
	oldctx = MemoryContextSwitchTo(ApplyContext);

	newsub = GetSubscription(MyLogicalRepWorker->subid, true);

	/*
	 * Exit if the subscription was removed. This normally should not happen
	 * as the worker gets killed during DROP SUBSCRIPTION.
	 */
	if (!newsub)
	{
		ereport(LOG,
		/* translator: first %s is the name of logical replication worker */
				(errmsg("%s for subscription \"%s\" will stop because the subscription was removed",
						get_worker_name(), MySubscription->name)));

		/* Ensure we remove no-longer-useful entry for worker's start time */
		if (!am_tablesync_worker() && !am_parallel_apply_worker())
			ApplyLauncherForgetWorkerStartTime(MyLogicalRepWorker->subid);
		proc_exit(0);
	}

	/* Exit if the subscription was disabled. */
	if (!newsub->enabled)
	{
		ereport(LOG,
		/* translator: first %s is the name of logical replication worker */
				(errmsg("%s for subscription \"%s\" will stop because the subscription was disabled",
						get_worker_name(), MySubscription->name)));

		apply_worker_exit();
	}

	/* !slotname should never happen when enabled is true. */
	Assert(newsub->slotname);

	/* two-phase should not be altered */
	Assert(newsub->twophasestate == MySubscription->twophasestate);

	/*
	 * Exit if any parameter that affects the remote connection was changed.
	 * The launcher will start a new worker but note that the parallel apply
	 * worker won't restart if the streaming option's value is changed from
	 * 'parallel' to any other value or the server decides not to stream the
	 * in-progress transaction.
	 */
	if (strcmp(newsub->conninfo, MySubscription->conninfo) != 0 ||
		strcmp(newsub->name, MySubscription->name) != 0 ||
		strcmp(newsub->slotname, MySubscription->slotname) != 0 ||
		newsub->binary != MySubscription->binary ||
		newsub->stream != MySubscription->stream ||
		newsub->passwordrequired != MySubscription->passwordrequired ||
		strcmp(newsub->origin, MySubscription->origin) != 0 ||
		newsub->owner != MySubscription->owner ||
		!equal(newsub->publications, MySubscription->publications) ||
		(newsub->minapplydelay == 0) != (MySubscription->minapplydelay == 0))
	{
		if (am_parallel_apply_worker())
			ereport(LOG,
					(errmsg("logical replication parallel apply worker for subscription \"%s\" will stop because of a parameter change",
							MySubscription->name)));
		else
			ereport(LOG,
			/* translator: first %s is the name of logical replication worker */
					(errmsg("%s for subscription \"%s\" will restart because of a parameter change",
							get_worker_name(), MySubscription->name)));

		apply_worker_exit();
	}

	/* Check for other changes that should never happen too. */
	if (newsub->dbid != MySubscription->dbid)
	{
		elog(ERROR, "subscription %u changed unexpectedly",
			 MyLogicalRepWorker->subid);
	}

	/* Clean old subscription info and switch to new one. */
	FreeSubscription(MySubscription);
	MySubscription = newsub;

	MemoryContextSwitchTo(oldctx);

	/* Change synchronous commit according to the user's wishes */
	SetConfigOption("synchronous_commit", MySubscription->synccommit,
					PGC_BACKEND, PGC_S_OVERRIDE);

	if (started_tx)
		CommitTransactionCommand();

	MySubscriptionValid = true;
}

/*
 * Callback from subscription syscache invalidation.
 */
static void
subscription_change_cb(Datum arg, int cacheid, uint32 hashvalue)
{
	MySubscriptionValid = false;
}

/*
 * subxact_info_write
 *	  Store information about subxacts for a toplevel transaction.
 *
 * For each subxact we store offset of it's first change in the main file.
 * The file is always over-written as a whole.
 *
 * XXX We should only store subxacts that were not aborted yet.
 */
static void
subxact_info_write(Oid subid, TransactionId xid)
{
	char		path[MAXPGPATH];
	Size		len;
	BufFile    *fd;

	Assert(TransactionIdIsValid(xid));

	/* construct the subxact filename */
	subxact_filename(path, subid, xid);

	/* Delete the subxacts file, if exists. */
	if (subxact_data.nsubxacts == 0)
	{
		cleanup_subxact_info();
		BufFileDeleteFileSet(MyLogicalRepWorker->stream_fileset, path, true);

		return;
	}

	/*
	 * Create the subxact file if it not already created, otherwise open the
	 * existing file.
	 */
	fd = BufFileOpenFileSet(MyLogicalRepWorker->stream_fileset, path, O_RDWR,
							true);
	if (fd == NULL)
		fd = BufFileCreateFileSet(MyLogicalRepWorker->stream_fileset, path);

	len = sizeof(SubXactInfo) * subxact_data.nsubxacts;

	/* Write the subxact count and subxact info */
	BufFileWrite(fd, &subxact_data.nsubxacts, sizeof(subxact_data.nsubxacts));
	BufFileWrite(fd, subxact_data.subxacts, len);

	BufFileClose(fd);

	/* free the memory allocated for subxact info */
	cleanup_subxact_info();
}

/*
 * subxact_info_read
 *	  Restore information about subxacts of a streamed transaction.
 *
 * Read information about subxacts into the structure subxact_data that can be
 * used later.
 */
static void
subxact_info_read(Oid subid, TransactionId xid)
{
	char		path[MAXPGPATH];
	Size		len;
	BufFile    *fd;
	MemoryContext oldctx;

	Assert(!subxact_data.subxacts);
	Assert(subxact_data.nsubxacts == 0);
	Assert(subxact_data.nsubxacts_max == 0);

	/*
	 * If the subxact file doesn't exist that means we don't have any subxact
	 * info.
	 */
	subxact_filename(path, subid, xid);
	fd = BufFileOpenFileSet(MyLogicalRepWorker->stream_fileset, path, O_RDONLY,
							true);
	if (fd == NULL)
		return;

	/* read number of subxact items */
	BufFileReadExact(fd, &subxact_data.nsubxacts, sizeof(subxact_data.nsubxacts));

	len = sizeof(SubXactInfo) * subxact_data.nsubxacts;

	/* we keep the maximum as a power of 2 */
	subxact_data.nsubxacts_max = 1 << my_log2(subxact_data.nsubxacts);

	/*
	 * Allocate subxact information in the logical streaming context. We need
	 * this information during the complete stream so that we can add the sub
	 * transaction info to this. On stream stop we will flush this information
	 * to the subxact file and reset the logical streaming context.
	 */
	oldctx = MemoryContextSwitchTo(LogicalStreamingContext);
	subxact_data.subxacts = palloc(subxact_data.nsubxacts_max *
								   sizeof(SubXactInfo));
	MemoryContextSwitchTo(oldctx);

	if (len > 0)
		BufFileReadExact(fd, subxact_data.subxacts, len);

	BufFileClose(fd);
}

/*
 * subxact_info_add
 *	  Add information about a subxact (offset in the main file).
 */
static void
subxact_info_add(TransactionId xid)
{
	SubXactInfo *subxacts = subxact_data.subxacts;
	int64		i;

	/* We must have a valid top level stream xid and a stream fd. */
	Assert(TransactionIdIsValid(stream_xid));
	Assert(stream_fd != NULL);

	/*
	 * If the XID matches the toplevel transaction, we don't want to add it.
	 */
	if (stream_xid == xid)
		return;

	/*
	 * In most cases we're checking the same subxact as we've already seen in
	 * the last call, so make sure to ignore it (this change comes later).
	 */
	if (subxact_data.subxact_last == xid)
		return;

	/* OK, remember we're processing this XID. */
	subxact_data.subxact_last = xid;

	/*
	 * Check if the transaction is already present in the array of subxact. We
	 * intentionally scan the array from the tail, because we're likely adding
	 * a change for the most recent subtransactions.
	 *
	 * XXX Can we rely on the subxact XIDs arriving in sorted order? That
	 * would allow us to use binary search here.
	 */
	for (i = subxact_data.nsubxacts; i > 0; i--)
	{
		/* found, so we're done */
		if (subxacts[i - 1].xid == xid)
			return;
	}

	/* This is a new subxact, so we need to add it to the array. */
	if (subxact_data.nsubxacts == 0)
	{
		MemoryContext oldctx;

		subxact_data.nsubxacts_max = 128;

		/*
		 * Allocate this memory for subxacts in per-stream context, see
		 * subxact_info_read.
		 */
		oldctx = MemoryContextSwitchTo(LogicalStreamingContext);
		subxacts = palloc(subxact_data.nsubxacts_max * sizeof(SubXactInfo));
		MemoryContextSwitchTo(oldctx);
	}
	else if (subxact_data.nsubxacts == subxact_data.nsubxacts_max)
	{
		subxact_data.nsubxacts_max *= 2;
		subxacts = repalloc(subxacts,
							subxact_data.nsubxacts_max * sizeof(SubXactInfo));
	}

	subxacts[subxact_data.nsubxacts].xid = xid;

	/*
	 * Get the current offset of the stream file and store it as offset of
	 * this subxact.
	 */
	BufFileTell(stream_fd,
				&subxacts[subxact_data.nsubxacts].fileno,
				&subxacts[subxact_data.nsubxacts].offset);

	subxact_data.nsubxacts++;
	subxact_data.subxacts = subxacts;
}

/* format filename for file containing the info about subxacts */
static inline void
subxact_filename(char *path, Oid subid, TransactionId xid)
{
	snprintf(path, MAXPGPATH, "%u-%u.subxacts", subid, xid);
}

/* format filename for file containing serialized changes */
static inline void
changes_filename(char *path, Oid subid, TransactionId xid)
{
	snprintf(path, MAXPGPATH, "%u-%u.changes", subid, xid);
}

/*
 * stream_cleanup_files
 *	  Cleanup files for a subscription / toplevel transaction.
 *
 * Remove files with serialized changes and subxact info for a particular
 * toplevel transaction. Each subscription has a separate set of files
 * for any toplevel transaction.
 */
void
stream_cleanup_files(Oid subid, TransactionId xid)
{
	char		path[MAXPGPATH];

	/* Delete the changes file. */
	changes_filename(path, subid, xid);
	BufFileDeleteFileSet(MyLogicalRepWorker->stream_fileset, path, false);

	/* Delete the subxact file, if it exists. */
	subxact_filename(path, subid, xid);
	BufFileDeleteFileSet(MyLogicalRepWorker->stream_fileset, path, true);
}

/*
 * stream_open_file
 *	  Open a file that we'll use to serialize changes for a toplevel
 * transaction.
 *
 * Open a file for streamed changes from a toplevel transaction identified
 * by stream_xid (global variable). If it's the first chunk of streamed
 * changes for this transaction, create the buffile, otherwise open the
 * previously created file.
 */
static void
stream_open_file(Oid subid, TransactionId xid, bool first_segment)
{
	char		path[MAXPGPATH];
	MemoryContext oldcxt;

	Assert(OidIsValid(subid));
	Assert(TransactionIdIsValid(xid));
	Assert(stream_fd == NULL);


	changes_filename(path, subid, xid);
	elog(DEBUG1, "opening file \"%s\" for streamed changes", path);

	/*
	 * Create/open the buffiles under the logical streaming context so that we
	 * have those files until stream stop.
	 */
	oldcxt = MemoryContextSwitchTo(LogicalStreamingContext);

	/*
	 * If this is the first streamed segment, create the changes file.
	 * Otherwise, just open the file for writing, in append mode.
	 */
	if (first_segment)
		stream_fd = BufFileCreateFileSet(MyLogicalRepWorker->stream_fileset,
										 path);
	else
	{
		/*
		 * Open the file and seek to the end of the file because we always
		 * append the changes file.
		 */
		stream_fd = BufFileOpenFileSet(MyLogicalRepWorker->stream_fileset,
									   path, O_RDWR, false);
		BufFileSeek(stream_fd, 0, 0, SEEK_END);
	}

	MemoryContextSwitchTo(oldcxt);
}

/*
 * stream_close_file
 *	  Close the currently open file with streamed changes.
 */
static void
stream_close_file(void)
{
	Assert(stream_fd != NULL);

	BufFileClose(stream_fd);

	stream_fd = NULL;
}

/*
 * stream_write_change
 *	  Serialize a change to a file for the current toplevel transaction.
 *
 * The change is serialized in a simple format, with length (not including
 * the length), action code (identifying the message type) and message
 * contents (without the subxact TransactionId value).
 */
static void
stream_write_change(char action, StringInfo s)
{
	int			len;

	Assert(stream_fd != NULL);

	/* total on-disk size, including the action type character */
	len = (s->len - s->cursor) + sizeof(char);

	/* first write the size */
	BufFileWrite(stream_fd, &len, sizeof(len));

	/* then the action */
	BufFileWrite(stream_fd, &action, sizeof(action));

	/* and finally the remaining part of the buffer (after the XID) */
	len = (s->len - s->cursor);

	BufFileWrite(stream_fd, &s->data[s->cursor], len);
}

/*
 * stream_open_and_write_change
 *	  Serialize a message to a file for the given transaction.
 *
 * This function is similar to stream_write_change except that it will open the
 * target file if not already before writing the message and close the file at
 * the end.
 */
static void
stream_open_and_write_change(TransactionId xid, char action, StringInfo s)
{
	Assert(!in_streamed_transaction);

	if (!stream_fd)
		stream_start_internal(xid, false);

	stream_write_change(action, s);
	stream_stop_internal(xid);
}

/*
 * Cleanup the memory for subxacts and reset the related variables.
 */
static inline void
cleanup_subxact_info()
{
	if (subxact_data.subxacts)
		pfree(subxact_data.subxacts);

	subxact_data.subxacts = NULL;
	subxact_data.subxact_last = InvalidTransactionId;
	subxact_data.nsubxacts = 0;
	subxact_data.nsubxacts_max = 0;
}

/*
 * Form the prepared transaction GID for two_phase transactions.
 *
 * Return the GID in the supplied buffer.
 */
static void
TwoPhaseTransactionGid(Oid subid, TransactionId xid, char *gid, int szgid)
{
	Assert(subid != InvalidRepOriginId);

	if (!TransactionIdIsValid(xid))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("invalid two-phase transaction ID")));

	snprintf(gid, szgid, "pg_gid_%u_%u", subid, xid);
}

/*
 * Execute the initial sync with error handling. Disable the subscription,
 * if it's required.
 *
 * Allocate the slot name in long-lived context on return. Note that we don't
 * handle FATAL errors which are probably because of system resource error and
 * are not repeatable.
 */
static void
start_table_sync(XLogRecPtr *origin_startpos, char **myslotname)
{
	char	   *syncslotname = NULL;

	Assert(am_tablesync_worker());

	PG_TRY();
	{
		/* Call initial sync. */
		syncslotname = LogicalRepSyncTableStart(origin_startpos);
	}
	PG_CATCH();
	{
		if (MySubscription->disableonerr)
			DisableSubscriptionAndExit();
		else
		{
			/*
			 * Report the worker failed during table synchronization. Abort
			 * the current transaction so that the stats message is sent in an
			 * idle state.
			 */
			AbortOutOfAnyTransaction();
			pgstat_report_subscription_error(MySubscription->oid, false);

			PG_RE_THROW();
		}
	}
	PG_END_TRY();

	/* allocate slot name in long-lived context */
	*myslotname = MemoryContextStrdup(ApplyContext, syncslotname);
	pfree(syncslotname);
}

/*
 * Run the apply loop with error handling. Disable the subscription,
 * if necessary.
 *
 * Note that we don't handle FATAL errors which are probably because
 * of system resource error and are not repeatable.
 */
static void
start_apply(XLogRecPtr origin_startpos)
{
	PG_TRY();
	{
		LogicalRepApplyLoop(origin_startpos);
	}
	PG_CATCH();
	{
		if (MySubscription->disableonerr)
			DisableSubscriptionAndExit();
		else
		{
			/*
			 * Report the worker failed while applying changes. Abort the
			 * current transaction so that the stats message is sent in an
			 * idle state.
			 */
			AbortOutOfAnyTransaction();
			pgstat_report_subscription_error(MySubscription->oid, !am_tablesync_worker());

			PG_RE_THROW();
		}
	}
	PG_END_TRY();
}

/*
 * Common initialization for leader apply worker and parallel apply worker.
 *
 * Initialize the database connection, in-memory subscription and necessary
 * config options.
 */
void
InitializeApplyWorker(void)
{
	MemoryContext oldctx;

	/* Run as replica session replication role. */
	SetConfigOption("session_replication_role", "replica",
					PGC_SUSET, PGC_S_OVERRIDE);

	/* Connect to our database. */
	BackgroundWorkerInitializeConnectionByOid(MyLogicalRepWorker->dbid,
											  MyLogicalRepWorker->userid,
											  0);

	/*
	 * Set always-secure search path, so malicious users can't redirect user
	 * code (e.g. pg_index.indexprs).
	 */
	SetConfigOption("search_path", "", PGC_SUSET, PGC_S_OVERRIDE);

	/* Load the subscription into persistent memory context. */
	ApplyContext = AllocSetContextCreate(TopMemoryContext,
										 "ApplyContext",
										 ALLOCSET_DEFAULT_SIZES);
	StartTransactionCommand();
	oldctx = MemoryContextSwitchTo(ApplyContext);

	MySubscription = GetSubscription(MyLogicalRepWorker->subid, true);
	if (!MySubscription)
	{
		ereport(LOG,
		/* translator: %s is the name of logical replication worker */
				(errmsg("%s for subscription %u will not start because the subscription was removed during startup",
						get_worker_name(), MyLogicalRepWorker->subid)));

		/* Ensure we remove no-longer-useful entry for worker's start time */
		if (!am_tablesync_worker() && !am_parallel_apply_worker())
			ApplyLauncherForgetWorkerStartTime(MyLogicalRepWorker->subid);
		proc_exit(0);
	}

	MySubscriptionValid = true;
	MemoryContextSwitchTo(oldctx);

	if (!MySubscription->enabled)
	{
		ereport(LOG,
		/* translator: first %s is the name of logical replication worker */
				(errmsg("%s for subscription \"%s\" will not start because the subscription was disabled during startup",
						get_worker_name(), MySubscription->name)));

		apply_worker_exit();
	}

	/* Setup synchronous commit according to the user's wishes */
	SetConfigOption("synchronous_commit", MySubscription->synccommit,
					PGC_BACKEND, PGC_S_OVERRIDE);

	/* Keep us informed about subscription changes. */
	CacheRegisterSyscacheCallback(SUBSCRIPTIONOID,
								  subscription_change_cb,
								  (Datum) 0);

	if (am_tablesync_worker())
		ereport(LOG,
				(errmsg("logical replication table synchronization worker for subscription \"%s\", table \"%s\" has started",
						MySubscription->name,
						get_rel_name(MyLogicalRepWorker->relid))));
	else
		ereport(LOG,
		/* translator: first %s is the name of logical replication worker */
				(errmsg("%s for subscription \"%s\" has started",
						get_worker_name(), MySubscription->name)));

	CommitTransactionCommand();
}

/* Logical Replication Apply worker entry point */
void
ApplyWorkerMain(Datum main_arg)
{
	int			worker_slot = DatumGetInt32(main_arg);
	char		originname[NAMEDATALEN];
	XLogRecPtr	origin_startpos = InvalidXLogRecPtr;
	char	   *myslotname = NULL;
	WalRcvStreamOptions options;
	int			server_version;

	/* Attach to slot */
	logicalrep_worker_attach(worker_slot);

	/* Setup signal handling */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/*
	 * We don't currently need any ResourceOwner in a walreceiver process, but
	 * if we did, we could call CreateAuxProcessResourceOwner here.
	 */

	/* Initialise stats to a sanish value */
	MyLogicalRepWorker->last_send_time = MyLogicalRepWorker->last_recv_time =
		MyLogicalRepWorker->reply_time = GetCurrentTimestamp();

	/* Load the libpq-specific functions */
	load_file("libpqwalreceiver", false);

	InitializeApplyWorker();

	/* Connect to the origin and start the replication. */
	elog(DEBUG1, "connecting to publisher using connection string \"%s\"",
		 MySubscription->conninfo);

	if (am_tablesync_worker())
	{
		start_table_sync(&origin_startpos, &myslotname);

		ReplicationOriginNameForLogicalRep(MySubscription->oid,
										   MyLogicalRepWorker->relid,
										   originname,
										   sizeof(originname));
		set_apply_error_context_origin(originname);
	}
	else
	{
		/* This is the leader apply worker */
		RepOriginId originid;
		TimeLineID	startpointTLI;
		char	   *err;
		bool		must_use_password;

		myslotname = MySubscription->slotname;

		/*
		 * This shouldn't happen if the subscription is enabled, but guard
		 * against DDL bugs or manual catalog changes.  (libpqwalreceiver will
		 * crash if slot is NULL.)
		 */
		if (!myslotname)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("subscription has no replication slot set")));

		/* Check delayed files or initialize directory */
		InitializeDelayedTxn();

		/* Setup replication origin tracking. */
		StartTransactionCommand();
		ReplicationOriginNameForLogicalRep(MySubscription->oid, InvalidOid,
										   originname, sizeof(originname));
		originid = replorigin_by_name(originname, true);
		if (!OidIsValid(originid))
			originid = replorigin_create(originname);
		replorigin_session_setup(originid, 0);
		replorigin_session_origin = originid;
		origin_startpos = replorigin_session_get_progress(false);

		/*
		 * If last_flushed exceeds origin_startpos, it means that some
		 * transactions are delaying. They have already been written into
		 * pernament file, so no need to recevie them again.
		 */
		if (origin_startpos < last_flushed)
			origin_startpos = last_flushed;

		/* Is the use of a password mandatory? */
		must_use_password = MySubscription->passwordrequired &&
			!superuser_arg(MySubscription->owner);

		/* Note that the superuser_arg call can access the DB */
		CommitTransactionCommand();

		LogRepWorkerWalRcvConn = walrcv_connect(MySubscription->conninfo, true,
												must_use_password,
												MySubscription->name, &err);
		if (LogRepWorkerWalRcvConn == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not connect to the publisher: %s", err)));

		/*
		 * We don't really use the output identify_system for anything but it
		 * does some initializations on the upstream so let's still call it.
		 */
		(void) walrcv_identify_system(LogRepWorkerWalRcvConn, &startpointTLI);

		set_apply_error_context_origin(originname);
	}

	/*
	 * Setup callback for syscache so that we know when something changes in
	 * the subscription relation state.
	 */
	CacheRegisterSyscacheCallback(SUBSCRIPTIONRELMAP,
								  invalidate_syncing_table_states,
								  (Datum) 0);

	/* Build logical replication streaming options. */
	options.logical = true;
	options.startpoint = origin_startpos;
	options.slotname = myslotname;

	server_version = walrcv_server_version(LogRepWorkerWalRcvConn);
	options.proto.logical.proto_version =
		server_version >= 160000 ? LOGICALREP_PROTO_STREAM_PARALLEL_VERSION_NUM :
		server_version >= 150000 ? LOGICALREP_PROTO_TWOPHASE_VERSION_NUM :
		server_version >= 140000 ? LOGICALREP_PROTO_STREAM_VERSION_NUM :
		LOGICALREP_PROTO_VERSION_NUM;

	options.proto.logical.publication_names = MySubscription->publications;
	options.proto.logical.binary = MySubscription->binary;

	/*
	 * Assign the appropriate option value for streaming option according to
	 * the 'streaming' mode and the publisher's ability to support that mode.
	 */
	if (server_version >= 160000 &&
		MySubscription->stream == LOGICALREP_STREAM_PARALLEL)
	{
		options.proto.logical.streaming_str = "parallel";
		MyLogicalRepWorker->parallel_apply = true;
	}
	else if (server_version >= 140000 &&
			 MySubscription->stream != LOGICALREP_STREAM_OFF)
	{
		options.proto.logical.streaming_str = "on";
		MyLogicalRepWorker->parallel_apply = false;
	}
	else
	{
		options.proto.logical.streaming_str = NULL;
		MyLogicalRepWorker->parallel_apply = false;
	}

	options.proto.logical.twophase = false;
	options.proto.logical.origin = pstrdup(MySubscription->origin);
	options.proto.logical.require_schema = false;


	if (!am_tablesync_worker())
	{
		if (server_version >= 160000)
			options.proto.logical.require_schema =
				MySubscription->minapplydelay > 0;

		/*
		 * Even when the two_phase mode is requested by the user, it remains
		 * as the tri-state PENDING until all tablesyncs have reached READY
		 * state. Only then, can it become ENABLED.
		 *
		 * Note: If the subscription has no tables then leave the state as
		 * PENDING, which allows ALTER SUBSCRIPTION ... REFRESH PUBLICATION to
		 * work.
		 */
		if (MySubscription->twophasestate == LOGICALREP_TWOPHASE_STATE_PENDING &&
			AllTablesyncsReady())
		{
			/* Start streaming with two_phase enabled */
			options.proto.logical.twophase = true;
			walrcv_startstreaming(LogRepWorkerWalRcvConn, &options);

			StartTransactionCommand();
			UpdateTwoPhaseState(MySubscription->oid, LOGICALREP_TWOPHASE_STATE_ENABLED);
			MySubscription->twophasestate = LOGICALREP_TWOPHASE_STATE_ENABLED;
			CommitTransactionCommand();
		}
		else
		{
			walrcv_startstreaming(LogRepWorkerWalRcvConn, &options);
		}

		ereport(DEBUG1,
				(errmsg_internal("logical replication apply worker for subscription \"%s\" two_phase is %s",
						MySubscription->name,
						MySubscription->twophasestate == LOGICALREP_TWOPHASE_STATE_DISABLED ? "DISABLED" :
						MySubscription->twophasestate == LOGICALREP_TWOPHASE_STATE_PENDING ? "PENDING" :
						MySubscription->twophasestate == LOGICALREP_TWOPHASE_STATE_ENABLED ? "ENABLED" :
						"?")));
	}
	else
	{
		/* Start normal logical streaming replication. */
		walrcv_startstreaming(LogRepWorkerWalRcvConn, &options);
	}

	/* Run the main loop. */
	start_apply(origin_startpos);

	proc_exit(0);
}

/*
 * After error recovery, disable the subscription in a new transaction
 * and exit cleanly.
 */
static void
DisableSubscriptionAndExit(void)
{
	/*
	 * Emit the error message, and recover from the error state to an idle
	 * state
	 */
	HOLD_INTERRUPTS();

	EmitErrorReport();
	AbortOutOfAnyTransaction();
	FlushErrorState();

	RESUME_INTERRUPTS();

	/* Report the worker failed during either table synchronization or apply */
	pgstat_report_subscription_error(MyLogicalRepWorker->subid,
									 !am_tablesync_worker());

	/* Disable the subscription */
	StartTransactionCommand();
	DisableSubscription(MySubscription->oid);
	CommitTransactionCommand();

	/* Ensure we remove no-longer-useful entry for worker's start time */
	if (!am_tablesync_worker() && !am_parallel_apply_worker())
		ApplyLauncherForgetWorkerStartTime(MyLogicalRepWorker->subid);

	/* Notify the subscription has been disabled and exit */
	ereport(LOG,
			errmsg("subscription \"%s\" has been disabled because of an error",
				   MySubscription->name));

	proc_exit(0);
}

/*
 * Is current process a logical replication worker?
 */
bool
IsLogicalWorker(void)
{
	return MyLogicalRepWorker != NULL;
}

/*
 * Is current process a logical replication parallel apply worker?
 */
bool
IsLogicalParallelApplyWorker(void)
{
	return IsLogicalWorker() && am_parallel_apply_worker();
}

/*
 * Start skipping changes of the transaction if the given LSN matches the
 * LSN specified by subscription's skiplsn.
 */
static void
maybe_start_skipping_changes(XLogRecPtr finish_lsn)
{
	Assert(!is_skipping_changes());
	Assert(!in_remote_transaction);
	Assert(!in_streamed_transaction);

	/*
	 * Quick return if it's not requested to skip this transaction. This
	 * function is called for every remote transaction and we assume that
	 * skipping the transaction is not used often.
	 */
	if (likely(XLogRecPtrIsInvalid(MySubscription->skiplsn) ||
			   MySubscription->skiplsn != finish_lsn))
		return;

	/* Start skipping all changes of this transaction */
	skip_xact_finish_lsn = finish_lsn;

	ereport(LOG,
			errmsg("logical replication starts skipping transaction at LSN %X/%X",
				   LSN_FORMAT_ARGS(skip_xact_finish_lsn)));
}

/*
 * Stop skipping changes by resetting skip_xact_finish_lsn if enabled.
 */
static void
stop_skipping_changes(void)
{
	if (!is_skipping_changes())
		return;

	ereport(LOG,
			(errmsg("logical replication completed skipping transaction at LSN %X/%X",
					LSN_FORMAT_ARGS(skip_xact_finish_lsn))));

	/* Stop skipping changes */
	skip_xact_finish_lsn = InvalidXLogRecPtr;
}

/*
 * Clear subskiplsn of pg_subscription catalog.
 *
 * finish_lsn is the transaction's finish LSN that is used to check if the
 * subskiplsn matches it. If not matched, we raise a warning when clearing the
 * subskiplsn in order to inform users for cases e.g., where the user mistakenly
 * specified the wrong subskiplsn.
 */
static void
clear_subscription_skip_lsn(XLogRecPtr finish_lsn)
{
	Relation	rel;
	Form_pg_subscription subform;
	HeapTuple	tup;
	XLogRecPtr	myskiplsn = MySubscription->skiplsn;
	bool		started_tx = false;

	if (likely(XLogRecPtrIsInvalid(myskiplsn)) || am_parallel_apply_worker())
		return;

	if (!IsTransactionState())
	{
		StartTransactionCommand();
		started_tx = true;
	}

	/*
	 * Protect subskiplsn of pg_subscription from being concurrently updated
	 * while clearing it.
	 */
	LockSharedObject(SubscriptionRelationId, MySubscription->oid, 0,
					 AccessShareLock);

	rel = table_open(SubscriptionRelationId, RowExclusiveLock);

	/* Fetch the existing tuple. */
	tup = SearchSysCacheCopy1(SUBSCRIPTIONOID,
							  ObjectIdGetDatum(MySubscription->oid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "subscription \"%s\" does not exist", MySubscription->name);

	subform = (Form_pg_subscription) GETSTRUCT(tup);

	/*
	 * Clear the subskiplsn. If the user has already changed subskiplsn before
	 * clearing it we don't update the catalog and the replication origin
	 * state won't get advanced. So in the worst case, if the server crashes
	 * before sending an acknowledgment of the flush position the transaction
	 * will be sent again and the user needs to set subskiplsn again. We can
	 * reduce the possibility by logging a replication origin WAL record to
	 * advance the origin LSN instead but there is no way to advance the
	 * origin timestamp and it doesn't seem to be worth doing anything about
	 * it since it's a very rare case.
	 */
	if (subform->subskiplsn == myskiplsn)
	{
		bool		nulls[Natts_pg_subscription];
		bool		replaces[Natts_pg_subscription];
		Datum		values[Natts_pg_subscription];

		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));
		memset(replaces, false, sizeof(replaces));

		/* reset subskiplsn */
		values[Anum_pg_subscription_subskiplsn - 1] = LSNGetDatum(InvalidXLogRecPtr);
		replaces[Anum_pg_subscription_subskiplsn - 1] = true;

		tup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls,
								replaces);
		CatalogTupleUpdate(rel, &tup->t_self, tup);

		if (myskiplsn != finish_lsn)
			ereport(WARNING,
					errmsg("skip-LSN of subscription \"%s\" cleared", MySubscription->name),
					errdetail("Remote transaction's finish WAL location (LSN) %X/%X did not match skip-LSN %X/%X.",
							  LSN_FORMAT_ARGS(finish_lsn),
							  LSN_FORMAT_ARGS(myskiplsn)));
	}

	heap_freetuple(tup);
	table_close(rel, NoLock);

	if (started_tx)
		CommitTransactionCommand();
}

/* Error callback to give more context info about the change being applied */
void
apply_error_callback(void *arg)
{
	ApplyErrorCallbackArg *errarg = &apply_error_callback_arg;

	if (apply_error_callback_arg.command == 0)
		return;

	Assert(errarg->origin_name);

	if (errarg->rel == NULL)
	{
		if (!TransactionIdIsValid(errarg->remote_xid))
			errcontext("processing remote data for replication origin \"%s\" during message type \"%s\"",
					   errarg->origin_name,
					   logicalrep_message_type(errarg->command));
		else if (XLogRecPtrIsInvalid(errarg->finish_lsn))
			errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" in transaction %u",
					   errarg->origin_name,
					   logicalrep_message_type(errarg->command),
					   errarg->remote_xid);
		else
			errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" in transaction %u, finished at %X/%X",
					   errarg->origin_name,
					   logicalrep_message_type(errarg->command),
					   errarg->remote_xid,
					   LSN_FORMAT_ARGS(errarg->finish_lsn));
	}
	else
	{
		if (errarg->remote_attnum < 0)
		{
			if (XLogRecPtrIsInvalid(errarg->finish_lsn))
				errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" for replication target relation \"%s.%s\" in transaction %u",
						   errarg->origin_name,
						   logicalrep_message_type(errarg->command),
						   errarg->rel->remoterel.nspname,
						   errarg->rel->remoterel.relname,
						   errarg->remote_xid);
			else
				errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" for replication target relation \"%s.%s\" in transaction %u, finished at %X/%X",
						   errarg->origin_name,
						   logicalrep_message_type(errarg->command),
						   errarg->rel->remoterel.nspname,
						   errarg->rel->remoterel.relname,
						   errarg->remote_xid,
						   LSN_FORMAT_ARGS(errarg->finish_lsn));
		}
		else
		{
			if (XLogRecPtrIsInvalid(errarg->finish_lsn))
				errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" for replication target relation \"%s.%s\" column \"%s\" in transaction %u",
						   errarg->origin_name,
						   logicalrep_message_type(errarg->command),
						   errarg->rel->remoterel.nspname,
						   errarg->rel->remoterel.relname,
						   errarg->rel->remoterel.attnames[errarg->remote_attnum],
						   errarg->remote_xid);
			else
				errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" for replication target relation \"%s.%s\" column \"%s\" in transaction %u, finished at %X/%X",
						   errarg->origin_name,
						   logicalrep_message_type(errarg->command),
						   errarg->rel->remoterel.nspname,
						   errarg->rel->remoterel.relname,
						   errarg->rel->remoterel.attnames[errarg->remote_attnum],
						   errarg->remote_xid,
						   LSN_FORMAT_ARGS(errarg->finish_lsn));
		}
	}
}

/* Set transaction information of apply error callback */
static inline void
set_apply_error_context_xact(TransactionId xid, XLogRecPtr lsn)
{
	apply_error_callback_arg.remote_xid = xid;
	apply_error_callback_arg.finish_lsn = lsn;
}

/* Reset all information of apply error callback */
static inline void
reset_apply_error_context_info(void)
{
	apply_error_callback_arg.command = 0;
	apply_error_callback_arg.rel = NULL;
	apply_error_callback_arg.remote_attnum = -1;
	set_apply_error_context_xact(InvalidTransactionId, InvalidXLogRecPtr);
}

/*
 * Request wakeup of the workers for the given subscription OID
 * at commit of the current transaction.
 *
 * This is used to ensure that the workers process assorted changes
 * as soon as possible.
 */
void
LogicalRepWorkersWakeupAtCommit(Oid subid)
{
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(TopTransactionContext);
	on_commit_wakeup_workers_subids =
		list_append_unique_oid(on_commit_wakeup_workers_subids, subid);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * Wake up the workers of any subscriptions that were changed in this xact.
 */
void
AtEOXact_LogicalRepWorkers(bool isCommit)
{
	if (isCommit && on_commit_wakeup_workers_subids != NIL)
	{
		ListCell   *lc;

		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
		foreach(lc, on_commit_wakeup_workers_subids)
		{
			Oid			subid = lfirst_oid(lc);
			List	   *workers;
			ListCell   *lc2;

			workers = logicalrep_workers_find(subid, true);
			foreach(lc2, workers)
			{
				LogicalRepWorker *worker = (LogicalRepWorker *) lfirst(lc2);

				logicalrep_worker_wakeup_ptr(worker);
			}
		}
		LWLockRelease(LogicalRepWorkerLock);
	}

	/* The List storage will be reclaimed automatically in xact cleanup. */
	on_commit_wakeup_workers_subids = NIL;
}

/*
 * Allocate the origin name in long-lived context for error context message.
 */
void
set_apply_error_context_origin(char *originname)
{
	apply_error_callback_arg.origin_name = MemoryContextStrdup(ApplyContext,
															   originname);
}

/*
 * Return the action to be taken for the given transaction. See
 * TransApplyAction for information on each of the actions.
 *
 * *winfo is assigned to the destination parallel worker info when the leader
 * apply worker has to pass all the transaction's changes to the parallel
 * apply worker.
 */
static TransApplyAction
get_transaction_apply_action(TransactionId xid, ParallelApplyWorkerInfo **winfo)
{
	*winfo = NULL;

	if (am_parallel_apply_worker())
	{
		return TRANS_PARALLEL_APPLY;
	}

	/*
	 * If we are processing this transaction using a parallel apply worker then
	 * either we send the changes to the parallel worker or if the worker is busy
	 * then serialize the changes to the file which will later be processed by
	 * the parallel worker.
	 */
	*winfo = pa_find_worker(xid);

	if (*winfo && (*winfo)->serialize_changes)
	{
		return TRANS_LEADER_PARTIAL_SERIALIZE;
	}
	else if (*winfo)
	{
		return TRANS_LEADER_SEND_TO_PARALLEL;
	}

	/*
	 * If there is no parallel worker involved to process this transaction then
	 * we either directly apply the change or serialize it to a file which will
	 * later be applied when the transaction finish message is processed.
	 */
	else if (in_streamed_transaction)
	{
		return TRANS_LEADER_SERIALIZE;
	}
	else
	{
		return TRANS_LEADER_APPLY;
	}
}
