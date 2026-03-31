/*-------------------------------------------------------------------------
*
* auth-validate.c
*      Implementation of authentication credential validation
*
* This module provides a mechanism for validating credentials during
* an active PostgreSQL session.
*
* Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
* Portions Copyright (c) 1994, Regents of the University of California
*
* IDENTIFICATION
*      src/backend/libpq/auth-validate.c
*
*-------------------------------------------------------------------------
*/
#include "postgres.h"

#include "access/xact.h"
#include "access/xlog.h"
#include "libpq/auth.h"
#include "libpq/libpq-be.h"
#include "libpq/auth-validate.h"
#include "libpq/auth-validate-methods.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "tcop/tcopprot.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/timestamp.h"
#include "utils/timeout.h"

/* GUC variables */
bool		credential_validation_enabled;
int			credential_validation_interval;


/* Registered credential validators */
static CredentialValidationCallback validators[CVT_COUNT];


/*
 * Convert UserAuth enum to CredentialValidationType for validator selection
 */
static CredentialValidationType
UserAuthToValidationType(UserAuth auth_method)
{
	switch (auth_method)
	{
		case uaPassword:
		case uaMD5:
		case uaSCRAM:
		/* All password-based methods use the password validator */
			return CVT_PASSWORD;
		case uaOAuth:
			return CVT_OAUTH;
		default:
			/* No specific validator for other auth methods */
			return CVT_COUNT;  /* Invalid value */
	}
}

/*
 * Process credential validation
 */
void
ProcessCredentialValidation(void)
{
	/* Skip validation during initialization, bootstrap, authentication or connection setup */
	if (ClientAuthInProgress || IsInitProcessingMode() || IsBootstrapProcessingMode())
		return;

	/* Check credentials if validation is enabled */
	if (credential_validation_enabled && MyClientConnectionInfo.authn_id != NULL)
	{
		CredentialValidationStatus status;
		UserAuth	auth_method = MyClientConnectionInfo.auth_method;

		status = CheckCredentialValidity();

		switch (status)
		{
			case CVS_VALID:
				/* Credentials are valid, continue */
				break;

			case CVS_EXPIRED:
				elog(LOG, "credential validation: credentials expired for auth_method=%d",
					 (int) auth_method);
				ereport(FATAL,
						(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
						 errmsg("session credentials have expired"),
						 errhint("Please reconnect to establish a new authenticated session")));
				break;

			case CVS_ERROR:
				elog(LOG, "credential validation: error checking credentials for auth_method=%d",
					 (int) auth_method);
				ereport(WARNING,
						(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
						 errmsg("error checking credential validity"),
						 errhint("Credential validation will be retried at the next interval")));
				break;
			}
	}
}

/*
 * Initialize credential validation system Called from InitPostgres after
 * authentication completes
 */
void
InitializeCredentialValidation(void)
{
	int			i;

	/* Define GUC variables */
	DefineCustomBoolVariable("credential_validation.enabled",
							 "Enable periodic credential validation.",
							 NULL,
							 &credential_validation_enabled,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("credential_validation.interval",
							"Credential validation interval in seconds.",
							NULL,
							&credential_validation_interval,
							60,	/* default: 60 seconds */
							5,	/* min: 5 seconds */
							3600,	/* max: 3600 seconds (1 hour) */
							PGC_SUSET,
							GUC_UNIT_S,
							NULL,
							NULL,
							NULL);

	/* Initialize validator callbacks to NULL */
	for (i = 0; i < CVT_COUNT; i++)
		validators[i] = NULL;

	/* Initialize and register all validation methods */
	InitializeValidationMethods();
}

/*
 * Enable or re-enable the credential validation timeout timer.
 * Called at session startup and after each validation or error recovery.
 */
void
EnableCredentialValidationTimeout(void)
{
	int			interval_ms;

	/* Only enable if credential validation is configured */
	if (!credential_validation_enabled)
		return;

	/* Skip for non-client backends */
	if (!IsExternalConnectionBackend(MyBackendType))
		return;

	/* Convert interval from seconds to milliseconds */
	interval_ms = credential_validation_interval * 1000;

	enable_timeout_after(CREDENTIAL_VALIDATION_TIMEOUT, interval_ms);

	elog(DEBUG1, "credential validation timeout enabled, interval=%d s", credential_validation_interval);
}

/*
 * Register a validator callback for a specific authentication method
 */
void
RegisterCredentialValidator(CredentialValidationType method_type, CredentialValidationCallback validator)
{
	if (method_type < 0 || method_type >= CVT_COUNT)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid validation method type: %d", method_type)));

	validators[method_type] = validator;
}

/*
 * Check credential validity using the appropriate validator
 */
CredentialValidationStatus
CheckCredentialValidity(void)
{
	CredentialValidationCallback validator = NULL;
	CredentialValidationStatus status;

	/*
	 * Skip validation for:
	 * - During shutdown or recovery
	 * - Non-client backends (any process not serving a client connection)
	 * - AutoVacuum processes (launcher and workers)
	 * - Background worker processes
	 * - Authentication is in progress
	 */
	if (proc_exit_inprogress ||
		RecoveryInProgress() ||
		!IsExternalConnectionBackend(MyBackendType) ||
		AmAutoVacuumLauncherProcess() ||
		AmAutoVacuumWorkerProcess() ||
		AmBackgroundWorkerProcess() ||
		ClientAuthInProgress)
		return CVS_VALID;
	/*
	 * Use the session's authentication method from MyClientConnectionInfo
	 * to select the appropriate validator.
	 */
	if (MyClientConnectionInfo.authn_id != NULL)
	{
		CredentialValidationType validation_type;

		validation_type = UserAuthToValidationType(MyClientConnectionInfo.auth_method);

		/*
		 * If we have a valid validation type, get the corresponding
		 * validator
		 */
		if (validation_type < CVT_COUNT)
			validator = validators[validation_type];

	}

	/*
	 * If no validator found for the current auth method or no
	 * authenticated session, skip validation and consider credentials
	 * valid
	 */
	if (validator == NULL || !MyClientConnectionInfo.authn_id)
			return CVS_VALID;

	/* Call the validator and interpret result */
	elog(DEBUG1, "credential validation: validating auth_method=%d", (int) MyClientConnectionInfo.auth_method);

	PG_TRY();
	{
		bool		result = validator();

		status = result ? CVS_VALID : CVS_EXPIRED;
	}
	PG_CATCH();
	{
		/* Error during validation */
		FlushErrorState();
		status = CVS_ERROR;
	}
	PG_END_TRY();

	return status;
}
