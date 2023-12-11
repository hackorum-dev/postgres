#ifndef AUTONOMOUS_H
#define AUTONOMOUS_H

#include "access/tupdesc.h"
#include "lib/ilist.h"
#include "nodes/pg_list.h"
#include "utils/guc.h"
#include "utils/resowner.h"

struct AutonomousSession;
typedef struct AutonomousSession AutonomousSession;

struct AutonomousPreparedStatement;
typedef struct AutonomousPreparedStatement AutonomousPreparedStatement;

typedef struct AutonomousResult
{
	TupleDesc	tupdesc;
	List	   *tuples;
	char	   *command;
} AutonomousResult;

AutonomousSession *AutonomousSessionGet(void);
void AutonomousSessionRelease(AutonomousSession *session);
void AutonomousPoolDestroyError(void);
AutonomousSession *AutonomousSessionPopStackSessionException(void *block);
AutonomousSession *AutonomousSessionPopStackSession(bool isBlockAutonomous);
AutonomousResult *AutonomousSessionExecute(AutonomousSession *session, const char *sql);
AutonomousPreparedStatement *AutonomousSessionPrepare(AutonomousSession *session, const char *sql, int16 nargs,
							  Oid argtypes[], const char *argnames[]);
AutonomousResult *AutonomousSessionExecutePrepared(AutonomousPreparedStatement *stmt, int16 nargs, Datum *values, bool *nulls);

void AutonomousSessionMain(Datum main_arg);

typedef struct AutonomousStackNode {
	slist_node node;
	AutonomousSession* session;
	void *block;
} AutonomousStackNode;

extern PGDLLIMPORT int autonomous_session_lifetime;

extern PGDLLIMPORT slist_head AutonomousStack;
extern PGDLLIMPORT MemoryContext AutonomousContext; /* NOTE: move to src/include/utils/memutils.h */
extern PGDLLIMPORT ResourceOwner AutonomousResourceOwner; /* NOTE: move to src/include/utils/resowner.h */


#endif /* AUTONOMOUS_H */
