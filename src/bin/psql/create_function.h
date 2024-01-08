/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2024, PostgreSQL Global Development Group
 *
 * src/bin/psql/create_function.h
 */
#ifndef CREATE_FUNCTION_H
#define CREATE_FUNCTION_H

#include "libpq-fe.h"

extern bool do_create_function(const char *args);

#endif
