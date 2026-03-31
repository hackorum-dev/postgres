/*-------------------------------------------------------------------------
 *
 * auth-validate-methods.c
 *      Implementation of authentication credential validation methods
 *
 * This module provides credential validation methods for various authentication
 * types during active PostgreSQL sessions. It includes validation for password
 * expiry, OAuth token expiry, and can be extended to other authentication
 * mechanisms.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *      src/backend/libpq/auth-validate-methods.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "catalog/catalog.h"
#include "libpq/auth-validate.h"
#include "libpq/libpq-be.h"
#include "libpq/oauth.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

/* Function declarations for internal use */
static bool validate_password_credentials(void);
static bool validate_oauth_credentials(void);

/* Function prototypes */
void InitializeValidationMethods(void);

/*
 * Initialize validation methods
 */
void
InitializeValidationMethods(void)
{
	/* Register all the validation methods */
	RegisterCredentialValidator(CVT_PASSWORD, validate_password_credentials);
	RegisterCredentialValidator(CVT_OAUTH, validate_oauth_credentials);
}

/*
 * Validate password credentials by checking rolvaliduntil
 * Returns true if credentials are still valid, false if they have expired.
 */
static bool
validate_password_credentials(void)
{
	HeapTuple   tuple = NULL;
	Datum       rolvaliduntil_datum;
	bool        validuntil_null;
	TimestampTz valid_until = 0;
	TimestampTz current_time;
	Oid         userid;
	bool        result = false;

	userid = GetSessionUserId();

	/*
	 * Try to take AccessShareLock on pg_authid to prevent concurrent modifications
	 * from interfering with our validation. Use conditional acquisition to avoid
	 * indefinite waiting during credential validation.
	 */
	if (!ConditionalLockRelationOid(AuthIdRelationId, AccessShareLock))
	{
		/*
		 * Could not acquire lock immediately, which likely means another session
		 * is modifying user data. For credential validation, it's better to
		 * consider credentials valid and retry later than to block indefinitely.
		 */
		elog(LOG, "credential validation: could not acquire lock on pg_authid immediately, will retry later");
		return true; /* Consider valid */
	}

	PG_TRY();
	{
		tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(userid));

		if (HeapTupleIsValid(tuple))
		{
			/* Get the expiration time column */
			rolvaliduntil_datum = SysCacheGetAttr(AUTHOID, tuple,
												  Anum_pg_authid_rolvaliduntil,
												  &validuntil_null);
			if (!validuntil_null)
			{
				valid_until = DatumGetTimestampTz(rolvaliduntil_datum);
				current_time = GetCurrentTimestamp();

				result = !(valid_until < current_time);
			}
			else
				result = true;

			ReleaseSysCache(tuple);
			tuple = NULL;
		}
	}
	PG_FINALLY();
	{
		if (tuple != NULL)
			ReleaseSysCache(tuple);

		UnlockRelationOid(AuthIdRelationId, AccessShareLock);
	}
	PG_END_TRY();

	return result;
}

/*
 * Check if an OAuth token has expired.
 *
 * Returns true if the token is still valid, false if it has expired.
 *
 * Calls wrapper CheckOAuthValidatorExpiration() function
 * to verify that the token hasn't expired.
 */
static bool
validate_oauth_credentials(void)
{
	/* Call the validator's expire_cb to check token expiration */
	if (!CheckOAuthValidatorExpiration())
		return false;

	return true;
}
