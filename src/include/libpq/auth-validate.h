/*-------------------------------------------------------------------------
 *
 * auth-validate.h
 *	  Interface for authentication credential validation
 *
 * This file provides a common interface for validating credentials
 * during an active PostgreSQL session.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/auth-validate.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AUTH_VALIDATE_H
#define AUTH_VALIDATE_H

#include "libpq/libpq-be.h"
#include "libpq/protocol.h"
#include "postmaster/postmaster.h"
#include "utils/guc.h"
#include "utils/timeout.h"

/* Define credential validation method types as an enum */
typedef enum CredentialValidationType
{
	CVT_PASSWORD = 0,          /* All password-based methods (md5, scram, etc) */
	CVT_OAUTH,                 /* OAuth bearer token authentication */
	CVT_COUNT                  /* Total number of credential validation types */
} CredentialValidationType;

/* Process credential validation */
extern void ProcessCredentialValidation(void);

/* GUC variables */
extern PGDLLIMPORT bool credential_validation_enabled;
extern PGDLLIMPORT int credential_validation_interval;

/* Common credential validation callback prototype */
typedef bool (*CredentialValidationCallback) (void);

/* Credential validation status */
typedef enum CredentialValidationStatus
{
	CVS_VALID,					/* Credentials are valid */
	CVS_EXPIRED,				/* Credentials have expired */
	CVS_ERROR					/* Error during validation */
} CredentialValidationStatus;

/* Initialize credential validation system */
extern void InitializeCredentialValidation(void);

/* Register a validation callback for a specific authentication method */
extern void RegisterCredentialValidator(CredentialValidationType method_type,
										CredentialValidationCallback validator);

/* Check credential validity */
extern CredentialValidationStatus CheckCredentialValidity(void);

/* Enable credential validation timeout timer */
extern void EnableCredentialValidationTimeout(void);

#endif							/* AUTH_VALIDATE_H */
