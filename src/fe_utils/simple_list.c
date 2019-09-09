/*-------------------------------------------------------------------------
 *
 * Simple list facilities for frontend code
 *
 * Data structures for simple lists of OIDs and strings.  The support for
 * these is very primitive compared to the backend's List facilities, but
 * it's all we need in, eg, pg_dump.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/fe_utils/simple_list.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "common/logging.h"
#include "fe_utils/simple_list.h"


/*
 * Append an OID to the list.
 */
void
simple_oid_list_append(SimpleOidList *list, Oid val)
{
	SimpleOidListCell *cell;

	cell = (SimpleOidListCell *) pg_malloc(sizeof(SimpleOidListCell));
	cell->next = NULL;
	cell->val = val;

	if (list->tail)
		list->tail->next = cell;
	else
		list->head = cell;
	list->tail = cell;
}

/*
 * Is OID present in the list?
 */
bool
simple_oid_list_member(SimpleOidList *list, Oid val)
{
	SimpleOidListCell *cell;

	for (cell = list->head; cell; cell = cell->next)
	{
		if (cell->val == val)
			return true;
	}
	return false;
}

/*
 * Append a string to the list.
 *
 * The given string is copied, so it need not survive past the call.
 */
void
simple_string_list_append(SimpleStringList *list, const char *val)
{
	SimpleStringListCell *cell;

	cell = (SimpleStringListCell *)
		pg_malloc(offsetof(SimpleStringListCell, val) + strlen(val) + 1);

	cell->next = NULL;
	cell->touched = false;
	strcpy(cell->val, val);

	if (list->tail)
		list->tail->next = cell;
	else
		list->head = cell;
	list->tail = cell;
}

/*
 * Is string present in the list?
 *
 * If found, the "touched" field of the first match is set true.
 */
bool
simple_string_list_member(SimpleStringList *list, const char *val)
{
	SimpleStringListCell *cell;

	for (cell = list->head; cell; cell = cell->next)
	{
		if (strcmp(cell->val, val) == 0)
		{
			cell->touched = true;
			return true;
		}
	}
	return false;
}

/*
 * Destroy an OID list
 */
void
simple_oid_list_destroy(SimpleOidList *list)
{
	SimpleOidListCell *cell;

	cell = list->head;
	while (cell != NULL)
	{
		SimpleOidListCell *next;

		next = cell->next;
		pg_free(cell);
		cell = next;
	}
}

/*
 * Destroy a string list
 */
void
simple_string_list_destroy(SimpleStringList *list)
{
	SimpleStringListCell *cell;

	cell = list->head;
	while (cell != NULL)
	{
		SimpleStringListCell *next;

		next = cell->next;
		pg_free(cell);
		cell = next;
	}
}

/*
 * Find first not-touched list entry, if there is one.
 */
const char *
simple_string_list_not_touched(SimpleStringList *list)
{
	SimpleStringListCell *cell;

	for (cell = list->head; cell; cell = cell->next)
	{
		if (!cell->touched)
			return cell->val;
	}
	return NULL;
}

/*
 * Split argument into old_dir and new_dir and append to tablespace mapping
 * list.
 */
void
tablespace_list_append(TablespaceList *tablespace_dirs, const char *arg)
{
	TablespaceListCell *cell = (TablespaceListCell *) pg_malloc0(sizeof(TablespaceListCell));
	char	   *dst;
	char	   *dst_ptr;
	const char *arg_ptr;

	Assert(tablespace_dirs);

	dst_ptr = dst = cell->old_dir;
	for (arg_ptr = arg; *arg_ptr; arg_ptr++)
	{
		if (dst_ptr - dst >= MAXPGPATH)
		{
			pg_log_error("directory name too long");
			exit(1);
		}

		if (*arg_ptr == '\\' && *(arg_ptr + 1) == '=')
			;					/* skip backslash escaping = */
		else if (*arg_ptr == '=' && (arg_ptr == arg || *(arg_ptr - 1) != '\\'))
		{
			if (*cell->new_dir)
			{
				pg_log_error("multiple \"=\" signs in tablespace mapping");
				exit(1);
			}
			else
				dst = dst_ptr = cell->new_dir;
		}
		else
			*dst_ptr++ = *arg_ptr;
	}

	if (!*cell->old_dir || !*cell->new_dir)
	{
		pg_log_error("invalid tablespace mapping format \"%s\", must be \"OLDDIR=NEWDIR\"", arg);
		exit(1);
	}

	/*
	 * This check isn't absolutely necessary.  But all tablespaces are created
	 * with absolute directories, so specifying a non-absolute path here would
	 * just never match, possibly confusing users.  It's also good to be
	 * consistent with the new_dir check.
	 */
	if (!is_absolute_path(cell->old_dir))
	{
		pg_log_error("old directory is not an absolute path in tablespace mapping: %s",
					 cell->old_dir);
		exit(1);
	}

	if (!is_absolute_path(cell->new_dir))
	{
		pg_log_error("new directory is not an absolute path in tablespace mapping: %s",
					 cell->new_dir);
		exit(1);
	}

	/*
	 * Comparisons done with these values should involve similarly
	 * canonicalized path values.  This is particularly sensitive on Windows
	 * where path values may not necessarily use Unix slashes.
	 */
	canonicalize_path(cell->old_dir);
	canonicalize_path(cell->new_dir);

	if (tablespace_dirs->tail)
		tablespace_dirs->tail->next = cell;
	else
		tablespace_dirs->head = cell;
	tablespace_dirs->tail = cell;
}

/*
 * Retrieve tablespace path, either relocated or original depending on whether
 * -T was passed or not.
 */
const char *
get_tablespace_mapping(TablespaceList *tablespace_dirs, const char *dir)
{
	TablespaceListCell *cell;
	char		canon_dir[MAXPGPATH];

	Assert(tablespace_dirs);

	/* Canonicalize path for comparison consistency */
	strlcpy(canon_dir, dir, sizeof(canon_dir));
	canonicalize_path(canon_dir);

	for (cell = tablespace_dirs->head; cell; cell = cell->next)
		if (strcmp(canon_dir, cell->old_dir) == 0)
			return cell->new_dir;

	return dir;
}
