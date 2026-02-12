/*-------------------------------------------------------------------------
 * wait_classes.h
 *	  Definitions related to wait event classes
 *
 * Copyright (c) 2001-2026, PostgreSQL Global Development Group
 *
 * src/include/utils/wait_classes.h
 * ----------
 */
#ifndef WAIT_CLASSES_H
#define WAIT_CLASSES_H


/* ----------
 * Wait Classes
 * ----------
 */
#define PG_WAIT_LWLOCK				0x0100000000000000ULL
#define PG_WAIT_LOCK				0x0300000000000000ULL
#define PG_WAIT_BUFFER				0x0400000000000000ULL
#define PG_WAIT_ACTIVITY			0x0500000000000000ULL
#define PG_WAIT_CLIENT				0x0600000000000000ULL
#define PG_WAIT_EXTENSION			0x0700000000000000ULL
#define PG_WAIT_IPC					0x0800000000000000ULL
#define PG_WAIT_TIMEOUT				0x0900000000000000ULL
#define PG_WAIT_IO					0x0A00000000000000ULL
#define PG_WAIT_INJECTIONPOINT		0x0B00000000000000ULL

#endif							/* WAIT_CLASSES_H */
