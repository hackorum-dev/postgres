/*-------------------------------------------------------------------------
 *
 * session_variable.c
 *	  session variable creation/manipulation commands
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/session_variable.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_variable.h"
#include "commands/session_variable.h"
#include "executor/svariableReceiver.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "rewrite/rewriteHandler.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * Values of session variables are stored in the backend local memory
 * inside sessionvars hash table in binary format inside a dedicated memory
 * context SVariableMemoryContext.  The hash key is oid
 * of related entry in pg_variable table. But long term unambiguity of oid is
 * not guaranteed. As an example, for a value with one oid a session can be
 * inactive for long time, while in the meantime the related session variable
 * can be dropped in another session, assigned oid can be released, and
 * theoreticaly this oid can be assigned to different session variable.
 * At the end, the reading of value stored in old session should to fail,
 * because related entry in pg_variable will not be consistent with
 * stored value. This is reason why we do check consistency between stored
 * value and catalog by create_lsn value.
 *
 * Before any usage (not only read in transaction) we need to check consistency
 * with pg_variable entry. When there is not entry with stored oid, the related
 * variable was dropped, and stored value is not consistent. When entry with
 * known oid, but lsn number is different, entry of pg_variable was created
 * for different variable and stored value is not consistent again.
 */
typedef struct SVariableData
{
	Oid			varid;			/* pg_variable OID of the variable (hash key) */
	XLogRecPtr	create_lsn;

	bool		isnull;
	Datum		value;

	Oid			typid;
	int16		typlen;
	bool		typbyval;

	bool		is_domain;

	/*
	 * domain_check_extra holds an extra (cache) for domain check.
	 * This extra is usually stored in fn_mcxt. We do not have same
	 * memory context for session variables, but we can use
	 * TopTransactionContext instead. Fresh extra is forced when
	 * we detect we are in a different transaction (different
	 * local transaction id domain_check_extra_lxid).
	 */
	void	   *domain_check_extra;
	LocalTransactionId domain_check_extra_lxid;

	/*
	 * Stored value and type description can be outdated when we receive
	 * sinval message. We have to check always if the stored data are
	 * trustful.
	 */
	bool		is_valid;

	uint32		hashvalue;		/* used for pairing sinval message */
} SVariableData;

typedef SVariableData *SVariable;

static HTAB *sessionvars = NULL;	/* hash table for session variables */

static MemoryContext SVariableMemoryContext = NULL;

/*
 * Callback function for session variable invalidation.
 */
static void
pg_variable_cache_callback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	SVariable	svar;

	elog(DEBUG1, "pg_variable_cache_callback %u %u", cacheid, hashvalue);

	Assert(sessionvars);

	/*
	 * When the hashvalue is not specified, then we have to recheck all
	 * currently used session variables. Since we can't guarantee the exact
	 * session variable from its hashValue, we also have to iterate over all
	 * items of the sessionvars hash table.
	 */
	hash_seq_init(&status, sessionvars);

	while ((svar = (SVariable) hash_seq_search(&status)) != NULL)
	{
		if (hashvalue == 0 || svar->hashvalue == hashvalue)
		{
			svar->is_valid = false;
		}
	}
}

/*
 * Release stored value, free memory
 */
static void
free_session_variable_value(SVariable svar)
{
	/* Clean current value */
	if (!svar->isnull)
	{
		if (!svar->typbyval)
			pfree(DatumGetPointer(svar->value));

		svar->isnull = true;
	}

	svar->value = (Datum) 0;
}

/*
 * Returns true when the entry in pg_variable is consistent with
 * the given session variable.
 */
static bool
is_session_variable_valid(SVariable svar)
{
	HeapTuple	tp;
	bool		result = false;

	Assert(OidIsValid(svar->varid));

	tp = SearchSysCache1(VARIABLEOID, ObjectIdGetDatum(svar->varid));

	if (HeapTupleIsValid(tp))
	{
		/*
		 * In this case, the only oid cannot be used as unique identifier,
		 * because the oid counter can wraparound, and the oid can be used for
		 * new other session variable. We do a second check against 64bit
		 * unique identifier.
		 */
		if (svar->create_lsn == ((Form_pg_variable) GETSTRUCT(tp))->varcreate_lsn)
			result = true;

		ReleaseSysCache(tp);
	}

	return result;
}

/*
 * Update attributes cached in svar
 */
static void
setup_session_variable(SVariable svar, Oid varid)
{
	HeapTuple	tup;
	Form_pg_variable varform;

	Assert(OidIsValid(varid));

	tup = SearchSysCache1(VARIABLEOID, ObjectIdGetDatum(varid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for session variable %u", varid);

	varform = (Form_pg_variable) GETSTRUCT(tup);

	svar->varid = varid;
	svar->create_lsn = varform->varcreate_lsn;

	svar->typid = varform->vartype;

	get_typlenbyval(svar->typid, &svar->typlen, &svar->typbyval);

	svar->is_domain = (get_typtype(varform->vartype) == TYPTYPE_DOMAIN);
	svar->domain_check_extra = NULL;
	svar->domain_check_extra_lxid = InvalidLocalTransactionId;

	svar->isnull = true;
	svar->value = (Datum) 0;

	svar->is_valid = true;

	svar->hashvalue = GetSysCacheHashValue1(VARIABLEOID,
											ObjectIdGetDatum(varid));

	ReleaseSysCache(tup);
}

/*
 * Assign some content to the session variable. It's copied to
 * SVariableMemoryContext if necessary.
 *
 * If any error happens, the existing value shouldn't be modified.
 */
static void
set_session_variable(SVariable svar, Datum value, bool isnull)
{
	Datum		newval;
	SVariableData locsvar,
			   *_svar;

	Assert(svar);
	Assert(!isnull || value == (Datum) 0);

	/*
	 * Use typbyval, typbylen from session variable only when they are
	 * trustable (the invalidation message was not accepted for this variable).
	 * When the variable is possibly invalid, force setup.
	 *
	 * Do not do it against passed svar, it should be unchanged, when an
	 * assignment is not successful (the datumCopy can fail).
	 */
	if (!svar->is_valid)
	{
		setup_session_variable(&locsvar, svar->varid);
		_svar = &locsvar;
	}
	else
		_svar = svar;

	if (!isnull)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(SVariableMemoryContext);

		newval = datumCopy(value, _svar->typbyval, _svar->typlen);

		MemoryContextSwitchTo(oldcxt);
	}
	else
		newval = value;

	free_session_variable_value(svar);

	/* We can overwrite old variable now. No error expected */
	if (svar != _svar)
		memcpy(svar, _svar, sizeof(SVariableData));

	svar->value = newval;
	svar->isnull = isnull;

	/*
	 * XXX While unlikely, an error here is possible. It wouldn't leak memory
	 * as the allocated chunk has already been correctly assigned to the
	 * session variable, but would contradict this function contract, which is
	 * that this function should either succeed or leave the current value
	 * untouched.
	 */
	elog(DEBUG1, "session variable \"%s.%s\" (oid:%u) has new value",
		 get_namespace_name(get_session_variable_namespace(svar->varid)),
		 get_session_variable_name(svar->varid),
		 svar->varid);
}

/*
 * Create the hash table for storing session variables.
 */
static void
create_sessionvars_hashtables(void)
{
	HASHCTL		vars_ctl;

	Assert(!sessionvars);

	if (!SVariableMemoryContext)
	{
		/* Read sinval messages */
		CacheRegisterSyscacheCallback(VARIABLEOID,
									  pg_variable_cache_callback,
									  (Datum) 0);

		/* We need our own long lived memory context */
		SVariableMemoryContext =
			AllocSetContextCreate(TopMemoryContext,
								  "session variables",
								  ALLOCSET_START_SMALL_SIZES);
	}

	memset(&vars_ctl, 0, sizeof(vars_ctl));
	vars_ctl.keysize = sizeof(Oid);
	vars_ctl.entrysize = sizeof(SVariableData);
	vars_ctl.hcxt = SVariableMemoryContext;

	sessionvars = hash_create("Session variables", 64, &vars_ctl,
							  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * Search a seesion variable in the hash table given its oid. If it
 * doesn't exist, then insert it there.
 *
 * Caller is responsible for doing permission checks.
 *
 * As side effect this function acquires AccessShareLock on
 * related session variable until the end of the transaction.
 */
static SVariable
get_session_variable(Oid varid)
{
	SVariable	svar;
	bool		found;

	/* Protect used session variable against drop until transaction end */
	LockDatabaseObject(VariableRelationId, varid, 0, AccessShareLock);

	if (!sessionvars)
		create_sessionvars_hashtables();

	svar = (SVariable) hash_search(sessionvars, &varid,
								   HASH_ENTER, &found);

	if (found)
	{
		if (!svar->is_valid)
		{
			/*
			 * The variable can be flagged as invalid by processing invalidation
			 * message, but can be validated by recheck against system catalog.
			 */
			if (is_session_variable_valid(svar))
				svar->is_valid = true;
			else
				/*
				 * When the value cannot be validated, we can safely throw it.
				 * There is oid in catalog, but it is related to different
				 * session variable (different create_lsn).
				 */
				free_session_variable_value(svar);
		}
	}
	else
		svar->is_valid = false;

	/*
	 * Force setup for not yet initialized variables or variables that cannot
	 * be validated.
	 */
	if (!svar->is_valid)
	{
		setup_session_variable(svar, varid);

		elog(DEBUG1, "session variable \"%s.%s\" (oid:%u) has assigned entry in memory (emitted by READ)",
			 get_namespace_name(get_session_variable_namespace(varid)),
			 get_session_variable_name(varid),
			 varid);
	}

	/* Ensure so returned data is still correct domain */
	if (svar->is_domain)
	{
		/*
		 * Store domain_check extra in TopTransactionContext. When we are in
		 * other transaction, the domain_check_extra cache is not valid
		 * anymore.
		 */
		if (svar->domain_check_extra_lxid != MyProc->vxid.lxid)
			svar->domain_check_extra = NULL;

		domain_check(svar->value, svar->isnull,
					 svar->typid, &svar->domain_check_extra,
					 TopTransactionContext);

		svar->domain_check_extra_lxid = MyProc->vxid.lxid;
	}

	return svar;
}

/*
 * Store the given value in an SVariable, and cache it if not already present.
 *
 * Caller is responsible for doing permission checks.
 *
 * As side effect this function acquires AccessShareLock on
 * related session variable until the end of the transaction.
 */
void
SetSessionVariable(Oid varid, Datum value, bool isNull)
{
	SVariable	svar;
	bool		found;

	/* Protect used session variable against drop until transaction end */
	LockDatabaseObject(VariableRelationId, varid, 0, AccessShareLock);

	if (!sessionvars)
		create_sessionvars_hashtables();

	svar = (SVariable) hash_search(sessionvars, &varid,
								   HASH_ENTER, &found);

	if (!found)
	{
		setup_session_variable(svar, varid);

		elog(DEBUG1, "session variable \"%s.%s\" (oid:%u) has assigned entry in memory (emitted by WRITE)",
			 get_namespace_name(get_session_variable_namespace(svar->varid)),
			 get_session_variable_name(svar->varid),
			 varid);
	}

	/*
	 * This should either succeed or fail without changing the currently
	 * stored value.
	 */
	set_session_variable(svar, value, isNull);
}

/*
 * Wrapper around SetSessionVariable after checking for correct permission.
 */
void
SetSessionVariableWithSecurityCheck(Oid varid, Datum value, bool isNull)
{
	AclResult	aclresult;

	/*
	 * Is caller allowed to update the session variable?
	 */
	aclresult = object_aclcheck(VariableRelationId, varid, GetUserId(), ACL_UPDATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_VARIABLE, get_session_variable_name(varid));

	SetSessionVariable(varid, value, isNull);
}

/*
 * Returns copy of value stored in variable.
 */
static inline Datum
copy_session_variable_value(SVariable svar, bool *isNull)
{
	Datum		value;

	/* force copy of non NULL value */
	if (!svar->isnull)
	{
		value = datumCopy(svar->value, svar->typbyval, svar->typlen);
		*isNull = false;
	}
	else
	{
		value = (Datum) 0;
		*isNull = true;
	}

	return value;
}

/*
 * Returns a copy of the value of the session variable (in current memory
 * context) specified by its oid. Caller is responsible for doing permission
 * checks.
 */
Datum
GetSessionVariable(Oid varid, bool *isNull, Oid *typid)
{
	SVariable	svar;

	svar = get_session_variable(varid);

	/*
	 * Although svar is freshly validated in this point, the svar->is_valid can
	 * be false, due possible accepting invalidation message inside domain
	 * check. Now, the validation is done after lock, that can also accept
	 * invalidation message, so validation should be trustful.
	 *
	 * For now, we don't need to repeat validation. Only svar should be valid
	 * pointer.
	 */
	Assert(svar);

	*typid = svar->typid;

	return copy_session_variable_value(svar, isNull);
}

/*
 * Returns a copy of the value of the session variable specified by its oid
 * with a check of the expected type. Like previous GetSessionVariable, the
 * caller is responsible for doing permission checks.
 */
Datum
GetSessionVariableWithTypeCheck(Oid varid, bool *isNull, Oid expected_typid)
{
	SVariable	svar;

	svar = get_session_variable(varid);

	Assert(svar && svar->is_valid);

	if (expected_typid != svar->typid)
		elog(ERROR, "type of variable \"%s.%s\" is different than expected",
			 get_namespace_name(get_session_variable_namespace(varid)),
			 get_session_variable_name(varid));

	return copy_session_variable_value(svar, isNull);
}

/*
 * Assign result of evaluated expression to session variable
 */
void
ExecuteLetStmt(ParseState *pstate,
			   LetStmt *stmt,
			   ParamListInfo params,
			   QueryEnvironment *queryEnv,
			   QueryCompletion *qc)
{
	Query	   *query = castNode(Query, stmt->query);
	List	   *rewritten;
	DestReceiver *dest;
	AclResult	aclresult;
	PlannedStmt *plan;
	QueryDesc  *queryDesc;
	Oid			varid = query->resultVariable;

	Assert(OidIsValid(varid));

	/*
	 * Is it allowed to write to session variable?
	 */
	aclresult = object_aclcheck(VariableRelationId, varid, GetUserId(), ACL_UPDATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_VARIABLE, get_session_variable_name(varid));

	/* Create dest receiver for LET */
	dest = CreateVariableDestReceiver(varid);

	/* run rewriter */
	query = copyObject(query);

	rewritten = QueryRewrite(query);

	Assert(list_length(rewritten) == 1);

	query = linitial_node(Query, rewritten);
	Assert(query->commandType == CMD_SELECT);

	/* plan the query */
	plan = pg_plan_query(query, pstate->p_sourcetext,
						 CURSOR_OPT_PARALLEL_OK, params);

	/*
	 * Use a snapshot with an updated command ID to ensure this query sees
	 * results of any previously executed queries.  (This could only matter if
	 * the planner executed an allegedly-stable function that changed the
	 * database contents, but let's do it anyway to be parallel to the EXPLAIN
	 * code path.)
	 */
	PushCopiedSnapshot(GetActiveSnapshot());
	UpdateActiveSnapshotCommandId();

	/* Create a QueryDesc, redirecting output to our tuple receiver */
	queryDesc = CreateQueryDesc(plan, pstate->p_sourcetext,
								GetActiveSnapshot(), InvalidSnapshot,
								dest, params, queryEnv, 0);

	/* call ExecutorStart to prepare the plan for execution */
	ExecutorStart(queryDesc, 0);

	/*
	 * Run the plan to completion. The result should be only one row. For an
	 * check too_many_rows we need to read two rows.
	 */
	ExecutorRun(queryDesc, ForwardScanDirection, 2L, true);

	/* save the rowcount if we're given a qc to fill */
	if (qc)
		SetQueryCompletion(qc, CMDTAG_LET, queryDesc->estate->es_processed);

	/* and clean up */
	ExecutorFinish(queryDesc);
	ExecutorEnd(queryDesc);

	FreeQueryDesc(queryDesc);

	PopActiveSnapshot();
}

/*
 * pg_session_variables - designed for testing
 *
 * This is a function designed for testing and debugging.  It returns the
 * content of sessionvars as-is, and can therefore display entries about
 * session variables that were dropped but for which this backend didn't
 * process the shared invalidations yet.
 */
Datum
pg_session_variables(PG_FUNCTION_ARGS)
{
#define NUM_PG_SESSION_VARIABLES_ATTS 8

	elog(DEBUG1, "pg_session_variables start");

	InitMaterializedSRF(fcinfo, 0);

	if (sessionvars)
	{
		ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
		HASH_SEQ_STATUS status;
		SVariable	svar;

		hash_seq_init(&status, sessionvars);

		while ((svar = (SVariable) hash_seq_search(&status)) != NULL)
		{
			Datum		values[NUM_PG_SESSION_VARIABLES_ATTS];
			bool		nulls[NUM_PG_SESSION_VARIABLES_ATTS];
			HeapTuple	tp;
			bool		var_is_valid = false;

			memset(values, 0, sizeof(values));
			memset(nulls, 0, sizeof(nulls));

			values[0] = ObjectIdGetDatum(svar->varid);
			values[3] = ObjectIdGetDatum(svar->typid);

			/* check if session variable is visible in system catalog */
			tp = SearchSysCache1(VARIABLEOID, ObjectIdGetDatum(svar->varid));

			/*
			 * Sessionvars can hold data of variables removed from catalog,
			 * (and not purged) and then namespacename and name cannot be read
			 * from catalog.
			 */
			if (HeapTupleIsValid(tp))
			{
				Form_pg_variable varform = (Form_pg_variable) GETSTRUCT(tp);

				/* When we see data in catalog */
				if (svar->create_lsn == varform->varcreate_lsn)
				{
					/* and when when these data are not out of date */
					values[1] = CStringGetTextDatum(
													get_namespace_name(varform->varnamespace));

					values[2] = CStringGetTextDatum(NameStr(varform->varname));
					values[4] = CStringGetTextDatum(format_type_be(svar->typid));
					values[5] = BoolGetDatum(false);

					values[6] = BoolGetDatum(
											 object_aclcheck(VariableRelationId, svar->varid,
															 GetUserId(), ACL_SELECT) == ACLCHECK_OK);

					values[7] = BoolGetDatum(
											 object_aclcheck(VariableRelationId, svar->varid,
															 GetUserId(), ACL_UPDATE) == ACLCHECK_OK);

					var_is_valid = true;
				}

				ReleaseSysCache(tp);
			}

			if (!var_is_valid)
			{
				/*
				 * When session variable was removed from catalog, but we
				 * haven't processed the invlidation yet. In this case, we can
				 * display only few oids. Other data are not available
				 * (without Form_pg_variable record), or can be lost (because
				 * there is not protection by dependency (more).
				 */
				nulls[1] = true;
				nulls[2] = true;
				nulls[4] = true;
				nulls[6] = true;
				nulls[7] = true;

				values[5] = BoolGetDatum(true);
			}

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}
	}

	elog(DEBUG1, "pg_session_variables end");

	return (Datum) 0;
}
