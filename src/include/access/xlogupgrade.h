/*
 * xlogupgrade.h
 *
 * Taking over a database cluster at an in-place major version upgrade
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogupgrade.h
 */
#ifndef XLOGUPGRADE_H
#define XLOGUPGRADE_H

#include "catalog/pg_control.h"

extern bool InplaceUpgradeTranslateControlFile(ControlFileData *cf);
extern void InplaceUpgradeTakeover(ControlFileData *cf);

#endif							/* XLOGUPGRADE_H */
