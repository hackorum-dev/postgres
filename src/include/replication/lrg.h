/*-------------------------------------------------------------------------
 *
 * lrg.h
 *		  Constructs a logical replication group
 *
 *-------------------------------------------------------------------------
 */
#ifndef LRG_H
#define LRG_H

#include "postgres.h"

#include "storage/latch.h"
#include "storage/lock.h"
#include "storage/lwlock.h"

/*
 * enumeration for represents its status
 */
typedef enum
{
	LRG_STATE_INIT = 0,
	LRG_STATE_CREATE_PUBLICATION,
	LRG_STATE_CREATE_SUBSCRIPTION,
	LRG_STATE_READY,
	LRG_STATE_TO_BE_DETACHED,
	LRG_STATE_FORCE_DETACH,
} LRG_NODE_STATE;

/*
 * working space for each LRG worker.
 */
typedef struct LrgWorker {
	pid_t worker_pid;
	Oid dbid;
	Latch *worker_latch;
} LrgWorker;

/*
 * controller for lrg worker.
 * This will be hold by launcher.
 */
typedef struct LrgWorkerCtxStruct {
	LWLock lock;
	pid_t launcher_pid;
	Latch *launcher_latch;
	LrgWorker workers[FLEXIBLE_ARRAY_MEMBER];
} LrgWorkerCtxStruct;

extern LrgWorkerCtxStruct *LrgWorkerCtx;

/* lrg.c */
extern void LrgLauncherShmemInit(void);
extern void LrgLauncherRegister(void);
extern void lrg_add_nodes(const char *node_id, Oid group_id, LRG_NODE_STATE status,
						  const char *node_name, const char *local_connstring,
						  const char *upstream_connstring);
extern Oid get_group_info(char **group_name);
extern void construct_node_id(char *out_node_id, int size);
extern void update_node_status_by_nodename(const char *node_name, LRG_NODE_STATE state, bool is_in_txn);
extern void update_node_status_by_nodeid(const char *node_id, LRG_NODE_STATE state, bool is_in_txn);

/* lrg_launcher.c */
extern void lrg_launcher_main(Datum arg) pg_attribute_noreturn();
extern void lrg_launcher_wakeup(void);

/* lrg_worker.c */
extern void lrg_worker_main(Datum arg) pg_attribute_noreturn();
extern void lrg_worker_cleanup(LrgWorker *worker);

#endif /* LRG_H */
