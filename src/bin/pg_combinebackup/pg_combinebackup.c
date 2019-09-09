/*-------------------------------------------------------------------------
 *
 * pg_combinebackup.c
 *	  Combines one or more incremental backups with the full base-backup to
 *	  generate new full base-backup.
 *
 * Copyright (c) 2010-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/bin/pg_combinebackup/pg_combinebackup.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "access/xlog_internal.h"
#include "common/controldata_utils.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "fe_utils/simple_list.h"
#include "getopt_long.h"
#include "pg_getopt.h"
#include "replication/basebackup.h"


/* Max number of incremental backups to be combined. */
#define MAX_INCR_BK_COUNT	10

/*
 * BACKUP_LABEL_FILE is defined in xlog.h which needs postgres.h to be included
 * too.  Thus to avoid that define it here again.
 */
#define BACKUP_LABEL_FILE		"backup_label"


typedef struct
{
	FILE	   *fp;
	char		filename[MAXPGPATH];
	bool		isPartial;
} FileMap;

typedef struct
{
	FILE	   *fp;
	int			offset;
} FileOffset;

static const char *progname;
static ControlFileData *ControlFile;
static bool verbose = false;
static bool success = false;
static bool noclean = false;
static bool made_new_outputdata = false;
static bool found_existing_outputdata = false;
static bool made_tablespace_dirs = false;
static bool found_tablespace_dirs = false;
static bool checksum_failure = false;
static char *OutputDir = NULL;
static TablespaceList tablespace_dirs = {NULL, NULL};

/* Function headers */
static void usage(void);
static void scan_file(const char *fn, char **IncrDirs, int nIncrDir,
					  const char *subdirpath);
static void scan_directory(char **IncrDirs, int nIncrDir,
						   const char *subdirpath);
static void check_compatibility(char *datadir);
static void verify_dir_is_empty_or_create(char *dirname, bool *created,
										  bool *found);
static void cleanup_directories_atexit(void);
static void combine_partial_files(const char *fn, char **IncrDirs,
								  int nIncrDir, const char *subdirpath,
								  const char*outfn);
static void copy_whole_file(const char *fromfn, const char *tofn);
static void cleanup_filemaps(FileMap *filemaps, int nfilemaps);
static void verify_backup_chain(char **IncrDirs, int nIncrDir);
static int create_filemap(const char *fn, char **IncrDirs, int nIncrDir,
						  const char *subdirpath, FileMap *filemaps);
static void write_backup_label_file(char *InputDir, char *label);


int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"full-backup", required_argument, NULL, 'f'},
		{"incr-backup", required_argument, NULL, 'i'},
		{"output-dir", required_argument, NULL, 'o'},
		{"tablespace-mapping", required_argument, NULL, 'T'},
		{"label", required_argument, NULL, 'l'},
		{"no-clean", no_argument, NULL, 'n'},
		{"verbose", no_argument, NULL, 'v'},
		{NULL, 0, NULL, 0}
	};

	char	   *IncrDirs[MAX_INCR_BK_COUNT + 1];	/* Full backup directory is
													 * stored at index 0 */
	int			nIncrDir;
	int			c;
	int			option_index;
	char	   *label = "pg_combinebackup combined full backup";

	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_combinebackup"));
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_combinebackup (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	atexit(cleanup_directories_atexit);

	/* Zero index is reserved for full backup directory. */
	IncrDirs[0] = NULL;
	nIncrDir = 1;
	while ((c = getopt_long(argc, argv, "f:i:l:no:T:v", long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'f':
				IncrDirs[0] = optarg;
				break;
			case 'i':
				if (nIncrDir > MAX_INCR_BK_COUNT)
				{
					pg_log_error("too many incremental backups to combine");
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}

				IncrDirs[nIncrDir] = optarg;
				nIncrDir++;
				break;
			case 'o':
				OutputDir = optarg;
				break;
			case 'l':
				label = pg_strdup(optarg);
				if (strlen(label) > MAXPGPATH)
				{
					pg_log_error("backup label too long (max %d bytes)",
								 MAXPGPATH);
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}
				break;
			case 'n':
				noclean = true;
				break;
			case 'T':
				tablespace_list_append(&tablespace_dirs, optarg);
				break;
			case 'v':
				verbose = true;
				break;
			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}

	/*
	 * Need to have directory paths for full backup, incremental backups, and
	 * the output directory.  Error out if we don't get that.
	 */
	if (IncrDirs[0] == NULL)
	{
		pg_log_error("no full backup directory specified");
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}
	if (nIncrDir == 1)
	{
		pg_log_error("no incremental backup directory specified");
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}
	if (OutputDir == NULL)
	{
		pg_log_error("no target directory specified");
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}
	else
		verify_dir_is_empty_or_create(OutputDir, &made_new_outputdata,
									  &found_existing_outputdata);

	/* Complain if any arguments remain */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/* Check that we have a valid backup chain */
	verify_backup_chain(IncrDirs, nIncrDir);

	/* Scan whole directory and process all .partial files */
	scan_directory(IncrDirs, nIncrDir, NULL);

	/* Now write a backup label file into the output directory */
	write_backup_label_file(IncrDirs[nIncrDir - 1], label);

	success = true;
	return 0;
}

static void
usage(void)
{
	printf(_("%s combines full backup with one or more incremental backups.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -f, --full-backup=DIRECTORY full backup directory\n"));
	printf(_("  -i, --incr-backup=DIRECTORY incremental backup directory (maximum %d), "
			 "                              must be in the order the backups were taken\n"), MAX_INCR_BK_COUNT);
	printf(_("  -o, --output-dir=DIRECTORY  combine backup directory\n"));
	printf(_("  -T, --tablespace-mapping=OLDDIR=NEWDIR\n"
			 "                              relocate tablespace in OLDDIR to NEWDIR\n"));
	printf(_("\nGeneral options:\n"));
	printf(_("  -l, --label=LABEL           set combine backup label\n"));
	printf(_("  -n, --no-clean              do not clean up after errors\n"));
	printf(_("  -v, --verbose               output verbose messages\n"));
	printf(_("  -V, --version               output version information, then exit\n"));
	printf(_("  -?, --help                  show this help, then exit\n"));
	printf(_("\nReport bugs to <pgsql-bugs@lists.postgresql.org>.\n"));
}

/*
 * scan_file
 *
 * Checks whether the given file is a partial file or not.  If partial, then
 * combines it into a full backup file, else copies as is to the output
 * directory.
 */
static void
scan_file(const char *fn, char **IncrDirs, int nIncrDir,
		  const char *subdirpath)
{
	char	   *extptr = strstr(fn, ".partial");

	/* If .partial file, combine them, else copy it as is */
	if (extptr != NULL)
	{
		char		outfn[MAXPGPATH];

		if (verbose)
			pg_log_info("combining partial file \"%s\"", fn);

		if (subdirpath)
			snprintf(outfn, MAXPGPATH, "%s/%s/%s", OutputDir, subdirpath, fn);
		else
			snprintf(outfn, MAXPGPATH, "%s/%s", OutputDir, fn);

		extptr = strstr(outfn, ".partial");
		Assert (extptr != NULL);
		extptr[0] = '\0';

		combine_partial_files(fn, IncrDirs, nIncrDir, subdirpath, outfn);
	}
	else
	{
		char		infn[MAXPGPATH];
		char		outfn[MAXPGPATH];

		if (verbose)
			pg_log_info("copying file \"%s\"", fn);

		if (subdirpath)
		{
			snprintf(infn, MAXPGPATH, "%s/%s/%s", IncrDirs[nIncrDir - 1],
					 subdirpath, fn);
			snprintf(outfn, MAXPGPATH, "%s/%s/%s", OutputDir, subdirpath, fn);
		}
		else
		{
			snprintf(infn, MAXPGPATH, "%s/%s", IncrDirs[nIncrDir - 1], fn);
			snprintf(outfn, MAXPGPATH, "%s/%s", OutputDir, fn);
		}

		copy_whole_file(infn, outfn);
	}
}

/*
 * copy_whole_file
 *
 * Copy file from source to its destination.
 */
static void
copy_whole_file(const char *fromfn, const char *tofn)
{
	FILE	   *ifp;
	FILE	   *ofp;
	char	   *buf;
	struct stat statbuf;
	off_t		cnt;
	pgoff_t		len = 0;

	ifp = fopen(fromfn, "rb");
	if (ifp == NULL)
	{
		pg_log_error("could not open file \"%s\": %m", fromfn);
		exit(1);
	}

	if (fstat(fileno(ifp), &statbuf) != 0)
	{
		pg_log_error("could not stat file \"%s\": %m", fromfn);
		fclose(ifp);
		exit(1);
	}

	if (verbose && statbuf.st_size > (RELSEG_SIZE * BLCKSZ))
		pg_log_info("found big file \"%s\" (size: %.2lfGB): %m", fromfn,
					(double) statbuf.st_size / (RELSEG_SIZE * BLCKSZ));

	ofp = fopen(tofn, "wb");
	if (ofp == NULL)
	{
		pg_log_error("could not create file \"%s\": %m", tofn);
		fclose(ifp);
		exit(1);
	}

	/* 1GB slice */
	buf = (char *) pg_malloc(RELSEG_SIZE * BLCKSZ);

	/*
	 * We do read entire 1GB file in memory while taking incremental backup; so
	 * I don't see any reason why can't we do that here.  Also, copying data in
	 * chunks is expensive.  However, for bigger files, we still slice at 1GB
	 * border.
	 */
	while ((cnt = fread(buf, 1, Min(RELSEG_SIZE * BLCKSZ, statbuf.st_size - len), ifp)) > 0)
	{
		/* Write the buf to the output file. */
		if (fwrite(buf, 1, cnt, ofp) != cnt)
		{
			pg_log_error("could not write to file \"%s\": %m", tofn);
			fclose(ifp);
			fclose(ofp);
			pg_free(buf);
			exit(1);
		}

		len += cnt;
	}

	if (len < statbuf.st_size)
		pg_log_error("could not read file \"%s\": %m", fromfn);

	fclose(ifp);
	fclose(ofp);
	pg_free(buf);
}

/*
 * scan_directory
 *
 * Scan the input incremental directory and operates on each file.  Creates
 * corresponding directories in the output directory too.
 */
static void
scan_directory(char **IncrDirs, int nIncrDir, const char *subdirpath)
{
	char		path[MAXPGPATH];
	DIR		   *dir;
	struct dirent *de;

	if (subdirpath)
	{
		char		outputpath[MAXPGPATH];

		snprintf(path, sizeof(path), "%s/%s", IncrDirs[nIncrDir - 1],
				 subdirpath);
		snprintf(outputpath, sizeof(outputpath), "%s/%s", OutputDir,
				 subdirpath);

		/* Create this sub-directory in output directory */
		if (pg_mkdir_p(outputpath, pg_dir_create_mode) == -1)
		{
			pg_log_error("could not create directory \"%s\": %m", outputpath);
			exit(1);
		}
	}
	else
		snprintf(path, sizeof(path), "%s", IncrDirs[nIncrDir - 1]);

	dir = opendir(path);
	if (!dir)
	{
		pg_log_error("could not open directory \"%s\": %m", path);
		exit(1);
	}

	while ((de = readdir(dir)) != NULL)
	{
		char		fn[MAXPGPATH];
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		snprintf(fn, sizeof(fn), "%s/%s", path, de->d_name);
		if (lstat(fn, &st) < 0)
		{
			pg_log_error("could not stat file \"%s\": %m", fn);
			exit(1);
		}
		if (S_ISREG(st.st_mode))
		{
			/* Skip backup label file. */
			if (strcmp(de->d_name, BACKUP_LABEL_FILE) == 0)
				continue;

			scan_file(de->d_name, IncrDirs, nIncrDir, subdirpath);
		}
		else if (subdirpath && strcmp(subdirpath, "pg_tblspc") == 0 &&
#ifndef WIN32
			S_ISLNK(st.st_mode)
#else
			pgwin32_is_junction(fn)
#endif
			)
		{
			char		newsubdirpath[MAXPGPATH];
			char		linkpath[MAXPGPATH];
			char		outfn[MAXPGPATH];
			int			rllen;
			char	   *mapped_tblspc_path;

			rllen = readlink(fn, linkpath, sizeof(linkpath));
			if (rllen < 0)
			{
				pg_log_error("could not read symbolic link \"%s\": %m",	fn);
				exit(1);
			}
			if (rllen >= sizeof(linkpath))
			{
				pg_log_error("symbolic link \"%s\" target is too long", fn);
				exit(1);
			}
			linkpath[rllen] = '\0';

			snprintf(newsubdirpath, MAXPGPATH, "%s/%s", subdirpath,
					 de->d_name);
			snprintf(outfn, MAXPGPATH, "%s/%s", OutputDir, newsubdirpath);

			mapped_tblspc_path = (char *) get_tablespace_mapping(&tablespace_dirs,
																 (const char *) linkpath);

			verify_dir_is_empty_or_create(mapped_tblspc_path,
										  &made_tablespace_dirs,
										  &found_tablespace_dirs);

			/* Create a symlink in the output directory. */
			if (symlink(mapped_tblspc_path, outfn) != 0)
			{
				pg_log_error("could not create symbolic link from \"%s\" to \"%s\": %m",
							 outfn, mapped_tblspc_path);
				exit(1);
			}

			if (verbose)
				pg_log_info("mapped tablespace from \"%s\" to \"%s\"",
							linkpath, mapped_tblspc_path);

			scan_directory(IncrDirs, nIncrDir, newsubdirpath);
		}
		else if (S_ISDIR(st.st_mode))
		{
			char		newsubdirpath[MAXPGPATH];

			if (subdirpath)
				snprintf(newsubdirpath, MAXPGPATH, "%s/%s", subdirpath,
						 de->d_name);
			else
				snprintf(newsubdirpath, MAXPGPATH, "%s", de->d_name);

			scan_directory(IncrDirs, nIncrDir, newsubdirpath);
		}
	}
	closedir(dir);
	return;
}

/*
 * check_compatibility
 *
 * Read the control file and check compatibility
 */
static void
check_compatibility(char *datadir)
{
	bool		crc_ok;

	ControlFile = get_controlfile(datadir, &crc_ok);
	if (!crc_ok)
	{
		pg_log_error("pg_control CRC value is incorrect");
		exit(1);
	}

	if (ControlFile->pg_control_version != PG_CONTROL_VERSION)
	{
		pg_log_error("cluster is not compatible with this version of pg_combinebackup");
		exit(1);
	}

	if (ControlFile->blcksz != BLCKSZ)
	{
		pg_log_error("database cluster is not compatible");
		fprintf(stderr, _("The database cluster was initialized with block size %u, but pg_combinebackup was compiled with block size %u.\n"),
				ControlFile->blcksz, BLCKSZ);
		exit(1);
	}

	/* When backup was taken, the database should have been in clean state. */
	if (ControlFile->state != DB_IN_PRODUCTION)
	{
		pg_log_error("cluster must be in production");
		exit(1);
	}
}

/*
 * verify_dir_is_empty_or_create
 *
 * Verify that the given directory exists and is empty.  If it does not exists,
 * it is created.  If it exists but is not empty, an error will be given and
 * the process ended.
 */
static void
verify_dir_is_empty_or_create(char *dirname, bool *created, bool *found)
{
	switch (pg_check_dir(dirname))
	{
		case 0:
			/*
			 * Does not exist, so create
			 */
			if (pg_mkdir_p(dirname, pg_dir_create_mode) == -1)
			{
				pg_log_error("could not create directory \"%s\": %m", dirname);
				exit(1);
			}
			if (created)
				*created = true;
			return;

		case 1:
			/*
			 * Exists, empty
			 */
			if (found)
				*found = true;
			return;

		case 2:
		case 3:
		case 4:
			/*
			 * Exists, not empty
			 */
			pg_log_error("directory \"%s\" exists but is not empty", dirname);
			exit(1);

		case -1:
			/*
			 * Access problem
			 */
			pg_log_error("could not access directory \"%s\": %m", dirname);
			exit(1);
	}
}

static void
cleanup_directories_atexit(void)
{
	if (success)
		return;

	if (!noclean && !checksum_failure)
	{
		if (made_new_outputdata)
		{
			pg_log_info("removing target data directory \"%s\"", OutputDir);
			if (!rmtree(OutputDir, true))
				pg_log_error("failed to remove data directory");
		}
		else if (found_existing_outputdata)
		{
			pg_log_info("removing contents of target data directory \"%s\"",
						OutputDir);
			if (!rmtree(OutputDir, false))
				pg_log_error("failed to remove contents of data directory");
		}
	}
	else
	{
		if ((made_new_outputdata || found_existing_outputdata) &&
			!checksum_failure)
			pg_log_info("target data directory \"%s\" not removed at user's request",
						OutputDir);
	}

	if ((made_tablespace_dirs || found_tablespace_dirs) && !checksum_failure)
		pg_log_info("changes to tablespace directories will not be undone");
}

/*
 * combine_partial_files
 *
 * Combines one or more incremental backups with full backup.  The algorithm in
 * this function works this way:
 * 	1.	Work backward through the backup chain until we find a complete version
 * 		of the file. We create a filemap in this process.
 * 	2.	Loop over all the files within filemap, read the header and check the
 * 		blocks modified, verify the CRC and create a blockmap.
 * 	3.	Create a new file in output directory by writing all the blocks.
 */
static void
combine_partial_files(const char *fn, char **IncrDirs, int nIncrDir,
					  const char *subdirpath, const char *outfn)
{
	FILE	   *outfp;
	FileOffset	outblocks[RELSEG_SIZE] = {{0}};
	int			i;
	FileMap	   *filemaps;
	int			nfilemaps;
	bool		modifiedblockfound;
	uint32		lastblkno;
	FileMap    *fm;
	struct stat statbuf;
	uint32		nblocks;

	filemaps = (FileMap *) pg_malloc(sizeof(FileMap) * nIncrDir);

	/* Create file map from the input directories. */
	nfilemaps = create_filemap(fn, IncrDirs, nIncrDir, subdirpath, filemaps);

	/* Process all opened files. */
	lastblkno = 0;
	modifiedblockfound = false;
	for (i = 0; i < nfilemaps - 1; i++)
	{
		char	   *buf;
		int			hsize;
		int			k;
		int			blkstartoffset;
		int			blknumberssize;
		uint32	   *blknumbers;
		partial_file_header *pfh;
		pg_crc32c	savedchecksum;

		fm = &filemaps[i];
		Assert(fm->isPartial);

		hsize = offsetof(partial_file_header, blocknumbers);
		buf = (char *) pg_malloc(hsize);

		/* Read partial file header. */
		if (fread(buf, 1, hsize, fm->fp) != hsize)
		{
			pg_log_error("corrupted partial file \"%s\": %m", fm->filename);
			checksum_failure = true;
			pg_free(filemaps);
			pg_free(buf);
			exit(1);
		}

		pfh = (partial_file_header *) buf;

		/* Check magic */
		if (pfh->magic != INCREMENTAL_BACKUP_MAGIC)
		{
			pg_log_error("corrupted partial file \"%s\", magic mismatch: %m", fm->filename);
			pg_free(filemaps);
			pg_free(buf);
			exit(1);
		}

		blknumberssize = sizeof(uint32) * pfh->nblocks;
		blknumbers = (uint32 *) pg_malloc(blknumberssize);

		/* Read all block numbers. */
		if (fread((char *) blknumbers, 1, blknumberssize, fm->fp) != blknumberssize)
		{
			pg_log_error("corrupted partial file \"%s\": %m", fm->filename);
			pg_free(blknumbers);
			pg_free(buf);
			pg_free(filemaps);
			exit(1);
		}

		/* Check CRC */
		savedchecksum = pfh->checksum;
		INIT_CRC32C(pfh->checksum);
		COMP_CRC32C(pfh->checksum, pfh, hsize);
		COMP_CRC32C(pfh->checksum, blknumbers, blknumberssize);
		if (pfh->checksum != savedchecksum)
		{
			pg_log_error("corrupted partial file \"%s\", checksum mismatch: %m", fm->filename);
			pg_free(blknumbers);
			pg_free(filemaps);
			pg_free(buf);
			exit(1);
		}
		else if (verbose)
			pg_log_info("checksums verified in file \"%s\"", fm->filename);

		blkstartoffset = hsize + blknumberssize;
		for (k = 0; k < pfh->nblocks; k++)
		{
			uint32		blknum = blknumbers[k];

			/*
			 * Set this block pointer in outblock array.  We skip setting
			 * it if already set as we are processing from latest file to
			 * oldest file.  If same block is modified across multiple
			 * incremental backup, then we use the latest one; skipping all
			 * other.
			 */
			if (outblocks[blknum].fp == NULL)
			{
				outblocks[blknum].fp = fm->fp;
				outblocks[blknum].offset = blkstartoffset + BLCKSZ * k;
			}

			modifiedblockfound = true;
		}

		/* Update last block number */
		if (k != 0 && blknumbers[k - 1] > lastblkno)
			lastblkno = (int) blknumbers[k - 1];
	}

	/* Read base file */
	Assert(i == (nfilemaps - 1));

	fm = &filemaps[nfilemaps - 1];
	Assert(fm->isPartial == false);

	/*
	 * If after processing all .partial files, we end up with no blocks
	 * modified, then simply copy the base file to the output directory and
	 * we are done.
	 */
	if (!modifiedblockfound)
	{
		copy_whole_file(fm->filename, outfn);
		cleanup_filemaps(filemaps, nfilemaps);
		return;
	}

	/* Write all blocks to the output file */

	if (fstat(fileno(fm->fp), &statbuf) != 0)
	{
		pg_log_error("could not stat file \"%s\": %m", fm->filename);
		cleanup_filemaps(filemaps, nfilemaps);
		exit(1);
	}

	Assert((statbuf.st_size % BLCKSZ) == 0);

	nblocks = statbuf.st_size / BLCKSZ;
	if ((nblocks - 1) > lastblkno)
		lastblkno = nblocks - 1;

	outfp = fopen(outfn, "wb");
	if (!outfp)
	{
		pg_log_error("could not create file \"%s\": %m", outfn);
		cleanup_filemaps(filemaps, nfilemaps);
		exit(1);
	}

	for (i = 0; i <= lastblkno; i++)
	{
		char		blkdata[BLCKSZ];
		FILE	   *infp;
		int			offset;

		/*
		 * Read block by block from respective file.  If outblock has NULL
		 * file pointer, then fetch that block from the base file.
		 */
		if (outblocks[i].fp != NULL)
		{
			infp = outblocks[i].fp;
			offset = outblocks[i].offset;
		}
		else
		{
			infp = fm->fp;
			offset = i * BLCKSZ;
		}

		if (fseek(infp, offset, SEEK_SET) == -1)
		{
			pg_log_error("could not fseek in file: %m");
			fclose(outfp);
			cleanup_filemaps(filemaps, nfilemaps);
			exit(1);
		}

		if (fread(blkdata, 1, BLCKSZ, infp) != BLCKSZ)
		{
			pg_log_error("could not read from file \"%s\": %m", outfn);
			fclose(outfp);
			cleanup_filemaps(filemaps, nfilemaps);
			exit(1);
		}

		/* Finally write one block to the output file */
		if (fwrite(blkdata, 1, BLCKSZ, outfp) != BLCKSZ)
		{
			pg_log_error("could not write to file \"%s\": %m", outfn);
			fclose(outfp);
			cleanup_filemaps(filemaps, nfilemaps);
			exit(1);
		}
	}

	fclose(outfp);
	cleanup_filemaps(filemaps, nfilemaps);

	return;
}

static void
cleanup_filemaps(FileMap *filemaps, int nfilemaps)
{
	int			i;

	for (i = 0; i < nfilemaps; i++)
		fclose(filemaps[i].fp);

	pg_free(filemaps);
}

/*
 * verify_backup_chain
 *
 * Verifies that the INCREMENTAL BACKUP REFERENCE WAL LOCATION of the
 * incremental backup matches with the START WAL LOCATION of the previous
 * backup, until we reach a full backup in which there is no INCREMENTAL
 * BACKUP REFERENCE WAL LOCATION present.
 */
static void
verify_backup_chain(char **IncrDirs, int nIncrDir)
{
	int			i;
	XLogRecPtr	startlsn = InvalidXLogRecPtr;
	XLogRecPtr	prevlsn = InvalidXLogRecPtr;
	TimeLineID	tli = 0;

	for (i = (nIncrDir - 1); i >= 0; i--)
	{
		struct stat statbuf;
		char		filename[MAXPGPATH];
		FILE	   *fp;
		char	   *labelfile;
		char		startxlogfilename[MAXFNAMELEN];
		uint32		hi;
		uint32		lo;
		char		ch;
		char	   *ptr;
		TimeLineID	tli_from_file;

		check_compatibility(IncrDirs[i]);

		snprintf(filename, MAXPGPATH, "%s/%s", IncrDirs[i], BACKUP_LABEL_FILE);
		fp = fopen(filename, "r");
		if (fp == NULL)
		{
			pg_log_error("could not read file \"%s\": %m", filename);
			exit(1);
		}
		if (fstat(fileno(fp), &statbuf))
		{
			pg_log_error("could not stat file \"%s\": %m", filename);
			fclose(fp);
			exit(1);
		}

		labelfile = pg_malloc(statbuf.st_size + 1);
		if (fread(labelfile, 1, statbuf.st_size, fp) != statbuf.st_size)
		{
			pg_log_error("corrupted file \"%s\": %m", filename);
			pg_free(labelfile);
			fclose(fp);
			exit(1);
		}

		fclose(fp);
		labelfile[statbuf.st_size] = '\0';

		/*
		 * Read the START WAL LOCATION from the directory, we skip this for top
		 * most directory corresponding to the last incremental backup as it is
		 * not needed to check.
		 */
		if (i != (nIncrDir - 1))
		{
			if (sscanf(labelfile, "START WAL LOCATION: %X/%X (file %24s)%c",
					   &hi, &lo, startxlogfilename,
					   &ch) != 4 || ch != '\n')
			{
				pg_log_error("invalid data in file \"%s\": %m", filename);
				pg_free(labelfile);
				exit(1);
			}
			startlsn = ((uint64) hi) << 32 | lo;

			/*
			 * We end up here from second loop counter, thus prevlsn must have
			 * been already set.  Check that with startlsn fetched above, they
			 * must match.  Otherwise we have a broken chain, bail out.
			 */
			Assert(!XLogRecPtrIsInvalid(prevlsn));
			if (prevlsn != startlsn)
			{
				pg_log_error("invalid backup chain");
				pg_free(labelfile);
				exit(1);
			}
		}

		/*
		 * Read forward until we get START TIMELINE and read it.  We must
		 * ensure that all backups should have same timeline id.
		 */
		ptr = strstr(labelfile, "START TIMELINE:");

		if (!ptr || sscanf(ptr, "START TIMELINE: %u\n", &tli_from_file) != 1)
		{
			pg_log_error("invalid data in file \"%s\": %m", filename);
			pg_free(labelfile);
			exit(1);
		}
		if (i != (nIncrDir - 1) && tli_from_file != tli)
		{
			pg_log_error("invalid timeline");
			pg_free(labelfile);
			exit(1);
		}
		tli = tli_from_file;

		/*
		 * Fetch the INCREMENTAL BACKUP REFERENCE WAL LOCATION from the
		 * incremental backup directory.  Index 0 is of full backup directory
		 * where we won't have that, so we skip it.
		 */
		if (i != 0)
		{
			ptr = strstr(ptr, "INCREMENTAL BACKUP REFERENCE WAL LOCATION:");

			if (!ptr || sscanf(ptr, "INCREMENTAL BACKUP REFERENCE WAL LOCATION: %X/%X\n", &hi, &lo) != 2)
			{
				pg_log_error("invalid data in file \"%s\": %m", filename);
				pg_free(labelfile);
				exit(1);
			}
			prevlsn = ((uint64) hi) << 32 | lo;
		}

		pg_free(labelfile);
	}
}

/*
 * create_filemap
 *
 * Open all files from all incremental backup directories and create a file
 * map.  Returns number of files added into the filemaps.
 */
static int
create_filemap(const char *fn, char **IncrDirs, int nIncrDir,
			   const char *subdirpath, FileMap *filemaps)
{
	int			i;
	bool		basefilefound = false;
	FileMap    *fm;
	int			fmindex;

	for (i = (nIncrDir - 1), fmindex = 0; i >= 0; i--, fmindex++)
	{
		fm = &filemaps[fmindex];

		if (subdirpath)
			snprintf(fm->filename, MAXPGPATH, "%s/%s/%s", IncrDirs[i],
					 subdirpath, fn);
		else
			snprintf(fm->filename, MAXPGPATH, "%s/%s", IncrDirs[i], fn);

		fm->fp = fopen(fm->filename, "rb");
		if (fm->fp != NULL)
		{
			fm->isPartial = true;
			continue;
		}

		if (errno == ENOENT)
		{
			char *extptr = strstr(fm->filename, ".partial");

			Assert (extptr != NULL);
			extptr[0] = '\0';

			/* Check without .partial */
			fm->fp = fopen(fm->filename, "rb");
			if (fm->fp != NULL)
			{
				fm->isPartial = false;
				basefilefound = true;
				/* We got a non-partial file, so no need to scan further */
				break;
			}
		}

		pg_log_error("could not open file \"%s\": %m", fm->filename);
		cleanup_filemaps(filemaps, fmindex);
		exit(1);
	}

	/* We must have found the base file. */
	if (!basefilefound)
	{
		pg_log_error("could not find base file \"%s\": %m", fn);
		cleanup_filemaps(filemaps, fmindex);
		exit(1);
	}

	/* Number of files = last index + 1 */
	return fmindex + 1;
}

/*
 * write_backup_label_file
 *
 * From backup label given in the incremental backup directory, write a backup
 * label file into the output directory.  Note here that, LABEL field is
 * modified per user given string and incremental backup reference LSN is not
 * added in the output file.
 */
static void
write_backup_label_file(char *InputDir, char *label)
{
	char		fromfn[MAXPGPATH];
	char		tofn[MAXPGPATH];
	char		outputlabel[MAXPGPATH + 7];		/* Room for "LABEL: " */
	FILE	   *fp;
	char	   *labelfile;
	char	   *ptr;
	char	   *fromptr;
	struct stat statbuf;
	int			len = 0;

	/*
	 * Read entire backup label file from input directory into in-memory
	 * buffer.
	 */

	snprintf(fromfn, MAXPGPATH, "%s/%s", InputDir, BACKUP_LABEL_FILE);
	fp = fopen(fromfn, "rb");
	if (fp == NULL)
	{
		pg_log_error("could not open file \"%s\": %m", fromfn);
		exit(1);
	}
	if (fstat(fileno(fp), &statbuf) != 0)
	{
		pg_log_error("could not stat file \"%s\": %m", fromfn);
		fclose(fp);
		exit(1);
	}

	labelfile = pg_malloc(statbuf.st_size + 1);
	if (fread(labelfile, 1, statbuf.st_size, fp) != statbuf.st_size)
	{
		pg_log_error("corrupted file \"%s\": %m", fromfn);
		pg_free(labelfile);
		fclose(fp);
		exit(1);
	}

	fclose(fp);
	labelfile[statbuf.st_size] = '\0';
	fromptr = labelfile;

	/*
	 * We need to copy all details up-to LABEL as is into the output backup
	 * label file.  Then write a user given label followed by rest of the
	 * details except incremental backup reference LSN.
	 */

	snprintf(tofn, MAXPGPATH, "%s/%s", OutputDir, BACKUP_LABEL_FILE);
	fp = fopen(tofn, "wb");
	if (fp == NULL)
	{
		pg_log_error("could not create file \"%s\": %m", tofn);
		fclose(fp);
		exit(1);
	}

	/* Find start of the label and write up-to that. */
	ptr = strstr(fromptr, "LABEL:");
	if (!ptr)
	{
		pg_log_error("corrupted file \"%s\": %m", fromfn);
		fclose(fp);
		pg_free(labelfile);
		exit(1);
	}

	len = ptr - fromptr;
	if (fwrite(fromptr, 1, len, fp) != len)
	{
		pg_log_error("could not write to file \"%s\": %m", tofn);
		fclose(fp);
		pg_free(labelfile);
		exit(1);
	}

	/* Write label */
	snprintf(outputlabel, MAXPGPATH + 7, "LABEL: %s", label);

	len = strlen(outputlabel);
	if (fwrite(outputlabel, 1, len, fp) != len)
	{
		pg_log_error("could not write to file \"%s\": %m", tofn);
		fclose(fp);
		pg_free(labelfile);
		exit(1);
	}

	/* Skip label from the input */
	if (sscanf(ptr, "LABEL: %1023[^\n]\n", outputlabel) != 1)
	{
		pg_log_error("corrupted file \"%s\": %m", fromfn);
		fclose(fp);
		pg_free(labelfile);
		exit(1);
	}

	/* Move exactly after label. */
	fromptr = ptr + strlen(outputlabel) + strlen("LABEL: ");

	/* Find incremental backup reference LSN, and write up-to that as-is. */
	ptr = strstr(fromptr, "INCREMENTAL BACKUP REFERENCE WAL LOCATION:");
	/* We must find that, else its an error. */
	if (!ptr)
	{
		pg_log_error("corrupted file \"%s\": %m", fromfn);
		fclose(fp);
		pg_free(labelfile);
		exit(1);
	}

	len = ptr - fromptr;
	if (fwrite(fromptr, 1, len, fp) != len)
	{
		pg_log_error("could not write to file \"%s\": %m", tofn);
		fclose(fp);
		pg_free(labelfile);
		exit(1);
	}

	ptr = strstr(ptr, "\n");
	/* We must find that, else its an error. */
	if (!ptr)
	{
		pg_log_error("corrupted file \"%s\": %m", fromfn);
		fclose(fp);
		pg_free(labelfile);
		exit(1);
	}

	/* Move past '\n' */
	ptr++;

	/* Move until ptr, skipping incremental backup reference LSN line. */
	fromptr = ptr;

	/* Write rest of the text. */
	len = statbuf.st_size - (ptr - labelfile);
	if (len && fwrite(fromptr, 1, len, fp) != len)
	{
		pg_log_error("invalid backup file \"%s\": %m", tofn);
		fclose(fp);
		pg_free(labelfile);
		exit(1);
	}

	fclose(fp);
	pg_free(labelfile);
}
