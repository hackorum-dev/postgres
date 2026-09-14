/*-------------------------------------------------------------------------
 *
 * bundle.h
 *		The files that an in-place major version upgrade keeps for a rollback
 *
 * Until an in-place upgrade goes live, it must be possible to roll it back
 * and leave the data directory as the old major version shut it down.
 * Everything a rollback needs is kept in INPLACE_UPGRADE_DIR, inside the
 * data directory; together these files are the rollback bundle.  This
 * header names them, for the server that writes them and for the frontend
 * programs that read them, so it must compile under postgres_fe.h as well.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/upgrade/bundle.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UPGRADE_BUNDLE_H
#define UPGRADE_BUNDLE_H

#define INPLACE_UPGRADE_DIR		"pg_inplace_upgrade_bundle"

/* Copies of the files that the takeover at startup replaces */
#define INPLACE_UPGRADE_CONTROL_STASH	INPLACE_UPGRADE_DIR "/pg_control"
#define INPLACE_UPGRADE_STATS_STASH		INPLACE_UPGRADE_DIR "/pgstat.stat"

/*
 * The list of files that the upgrade creates, one path per line relative to
 * the data directory.  A rollback removes these files.
 */
#define INPLACE_UPGRADE_CREATED_FILES	INPLACE_UPGRADE_DIR "/created_files"

#endif							/* UPGRADE_BUNDLE_H */
