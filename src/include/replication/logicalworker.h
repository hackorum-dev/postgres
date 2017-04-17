/*-------------------------------------------------------------------------
 *
 * logicalworker.h
 *	  Exports for logical replication workers.
 *
 * Portions Copyright (c) 2016-2017, PostgreSQL Global Development Group
 *
 * src/include/replication/logicalworker.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGICALWORKER_H
#define LOGICALWORKER_H

extern int apply_worker_timeout;

extern void ApplyWorkerMain(Datum main_arg);

#endif   /* LOGICALWORKER_H */
