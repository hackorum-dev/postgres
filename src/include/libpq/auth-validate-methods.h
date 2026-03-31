/*-------------------------------------------------------------------------
 *
 * auth-validate-methods.h
 *      Interface for authentication credential validation methods
 *
 * This file provides declarations for various credential validation methods
 * used with the credential validation system.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/auth-validate-methods.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AUTH_VALIDATE_METHODS_H
#define AUTH_VALIDATE_METHODS_H

#include "libpq/libpq-be.h"
#include "utils/timestamp.h"

/* Initialize all validation methods */
extern void InitializeValidationMethods(void);

#endif                          /* AUTH_VALIDATE_METHODS_H */
