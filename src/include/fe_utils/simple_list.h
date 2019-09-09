/*-------------------------------------------------------------------------
 *
 * Simple list facilities for frontend code
 *
 * Data structures for simple lists of OIDs and strings.  The support for
 * these is very primitive compared to the backend's List facilities, but
 * it's all we need in, eg, pg_dump.
 *
 * Also, has data structures for simple lists of tablespace mappings used in
 * backups.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/simple_list.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SIMPLE_LIST_H
#define SIMPLE_LIST_H

typedef struct SimpleOidListCell
{
	struct SimpleOidListCell *next;
	Oid			val;
} SimpleOidListCell;

typedef struct SimpleOidList
{
	SimpleOidListCell *head;
	SimpleOidListCell *tail;
} SimpleOidList;

typedef struct SimpleStringListCell
{
	struct SimpleStringListCell *next;
	bool		touched;		/* true, when this string was searched and
								 * touched */
	char		val[FLEXIBLE_ARRAY_MEMBER]; /* null-terminated string here */
} SimpleStringListCell;

typedef struct SimpleStringList
{
	SimpleStringListCell *head;
	SimpleStringListCell *tail;
} SimpleStringList;

typedef struct TablespaceListCell
{
	struct TablespaceListCell *next;
	char		old_dir[MAXPGPATH];
	char		new_dir[MAXPGPATH];
} TablespaceListCell;

typedef struct TablespaceList
{
	TablespaceListCell *head;
	TablespaceListCell *tail;
} TablespaceList;


extern void simple_oid_list_append(SimpleOidList *list, Oid val);
extern bool simple_oid_list_member(SimpleOidList *list, Oid val);
extern void simple_oid_list_destroy(SimpleOidList *list);

extern void simple_string_list_append(SimpleStringList *list, const char *val);
extern bool simple_string_list_member(SimpleStringList *list, const char *val);
extern void simple_string_list_destroy(SimpleStringList *list);

extern const char *simple_string_list_not_touched(SimpleStringList *list);

extern void tablespace_list_append(TablespaceList *tablespace_dirs,
								   const char *arg);
extern const char *get_tablespace_mapping(TablespaceList *tablespace_dirs,
										  const char *dir);

#endif							/* SIMPLE_LIST_H */
