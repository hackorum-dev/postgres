/*--------------------------------------------------------------------
 * conffiles.c
 *
 * Utilities related to the handling of configuration files.
 *
 * This file contains some generic tools to work on configuration files
 * used by PostgreSQL, be they related to GUCs or authentication.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/conffiles.c
 *
 *--------------------------------------------------------------------
 */

#include "postgres.h"

#include <dirent.h>

#include "common/file_utils.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/conffiles.h"

/*
 * AbsoluteConfigLocation
 *
 * Given a configuration file or directory location that may be a relative
 * path, return an absolute one.  We consider the location to be relative to
 * the directory holding the calling file, or to DataDir if no calling file.
 */
char *
AbsoluteConfigLocation(const char *location, const char *calling_file)
{
	if (is_absolute_path(location))
		return pstrdup(location);
	else
	{
		char		abs_path[MAXPGPATH];

		if (calling_file != NULL)
		{
			strlcpy(abs_path, calling_file, sizeof(abs_path));
			get_parent_directory(abs_path);
			join_path_components(abs_path, abs_path, location);
			canonicalize_path(abs_path);
		}
		else
		{
			Assert(DataDir);
			join_path_components(abs_path, DataDir, location);
			canonicalize_path(abs_path);
		}
		return pstrdup(abs_path);
	}
}


/*
 * Match a file name against a pattern with "*" (any run of characters,
 * including none) and "?" (any one character).  There is no portable
 * fnmatch(), and character classes are more than configuration includes
 * need.  Recursion is one level per "*" in the pattern, which is fine for
 * directory entry names.
 */
static bool
wildcard_match(const char *pattern, const char *name)
{
	for (;;)
	{
		switch (*pattern)
		{
			case '\0':
				return *name == '\0';
			case '?':
				if (*name == '\0')
					return false;
				pattern++;
				name++;
				break;
			case '*':
				while (*pattern == '*')
					pattern++;
				if (*pattern == '\0')
					return true;
				for (; *name != '\0'; name++)
				{
					if (wildcard_match(pattern, name))
						return true;
				}
				return false;
			default:
				if (*pattern != *name)
					return false;
				pattern++;
				name++;
				break;
		}
	}
}

/*
 * GetConfFilesInDir
 *
 * Returns the list of config files located in a directory, in alphabetical
 * order.  On error, returns NULL with details about the error stored in
 * "err_msg".
 */
char	  **
GetConfFilesInDir(const char *includedir, const char *calling_file,
				  int elevel, int *num_filenames, char **err_msg)
{
	return GetConfFilesInDirPattern(includedir, calling_file, "*.conf",
									elevel, num_filenames, err_msg);
}

/*
 * GetConfFilesInDirPattern
 *
 * Like GetConfFilesInDir, but the caller chooses which file names count,
 * as a wildcard pattern matched against the bare file name.
 */
char	  **
GetConfFilesInDirPattern(const char *includedir, const char *calling_file,
						 const char *pattern, int elevel,
						 int *num_filenames, char **err_msg)
{
	char	   *directory;
	DIR		   *d;
	struct dirent *de;
	char	  **filenames = NULL;
	int			size_filenames;

	/*
	 * Reject directory name that is all-blank (including empty), as that
	 * leads to confusion --- we'd read the containing directory, typically
	 * resulting in recursive inclusion of the same file(s).
	 */
	if (strspn(includedir, " \t\r\n") == strlen(includedir))
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("empty configuration directory name: \"%s\"",
						includedir)));
		*err_msg = "empty configuration directory name";
		return NULL;
	}

	directory = AbsoluteConfigLocation(includedir, calling_file);
	d = AllocateDir(directory);
	if (d == NULL)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open configuration directory \"%s\": %m",
						directory)));
		*err_msg = psprintf("could not open directory \"%s\"", directory);
		goto cleanup;
	}

	/*
	 * Read the directory and put the filenames in an array, so we can sort
	 * them prior to caller processing the contents.
	 */
	size_filenames = 32;
	filenames = palloc_array(char *, size_filenames);
	*num_filenames = 0;

	while ((de = ReadDir(d, directory)) != NULL)
	{
		PGFileType	de_type;
		char		filename[MAXPGPATH];

		/*
		 * Only parse files whose names match the pattern.  Explicitly reject
		 * files starting with ".".  This excludes things like "." and "..",
		 * as well as typical hidden files, backup files, and editor debris.
		 */
		if (de->d_name[0] == '.')
			continue;
		if (!wildcard_match(pattern, de->d_name))
			continue;

		join_path_components(filename, directory, de->d_name);
		canonicalize_path(filename);
		de_type = get_dirent_type(filename, de, true, elevel);
		if (de_type == PGFILETYPE_ERROR)
		{
			*err_msg = psprintf("could not stat file \"%s\"", filename);
			pfree(filenames);
			filenames = NULL;
			goto cleanup;
		}
		else if (de_type != PGFILETYPE_DIR)
		{
			/* Add file to array, increasing its size in blocks of 32 */
			if (*num_filenames >= size_filenames)
			{
				size_filenames += 32;
				filenames = repalloc_array(filenames, char *, size_filenames);
			}
			filenames[*num_filenames] = pstrdup(filename);
			(*num_filenames)++;
		}
	}

	/* Sort the files by name before leaving */
	if (*num_filenames > 0)
		qsort(filenames, *num_filenames, sizeof(char *), pg_qsort_strcmp);

cleanup:
	if (d)
		FreeDir(d);
	pfree(directory);
	return filenames;
}
