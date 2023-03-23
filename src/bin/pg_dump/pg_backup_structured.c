/*-------------------------------------------------------------------------
 *
 * pg_backup_structured.c
 *
 *  The structured dump is a plaintext dump split up into multiple files. Each
 *  of these files contains only one database object. The resulting small files
 *  stored in a directory path based on the dumped object. To avoid mixing data
 *  and schema, large objects and data is stored under the data directory
 *  structure.
 *
 *  To restore this format, feed the plaintext ToC file restore-dump.sql to psql.
 *  This file contains all dumped filenames in restore order.
 *
 *  This dump format can only output uncompressed files, compression during
 *  dumping is not supported.
 *
 *  One use case for this format is to import the small schema files into a VCS
 *  to track the actual changes in the schema in detail, for example after
 *  database migrations.
 *
 *	Based on pg_backup_null.c, please keep these formats in sync.
 *
 *
 *	See the headers to pg_restore for more details.
 *
 * Portions Copyright (c) 2023, Attila Soki, contact@attilasoki.com
 * Portions Copyright (c) 2000, Philip Warner (pg_backup_null.c)
 *		Rights are granted to use this software in any way so long
 *		as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	result from its use.
 *
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/pg_backup_structured.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/file_utils.h"
#include "common/hashfn.h"
#include "compress_io.h"
#include "fe_utils/string_utils.h"
#include "libpq/libpq-fs.h"
#include "pg_backup_archiver.h"
#include "pg_backup_utils.h"

static void _WriteData(ArchiveHandle *AH, const void *data, size_t dLen);
static void _WriteLOData(ArchiveHandle *AH, const void *data, size_t dLen);
static void _EndData(ArchiveHandle *AH, TocEntry *te);
static int	_WriteByte(ArchiveHandle *AH, const int i);
static void _WriteBuf(ArchiveHandle *AH, const void *buf, size_t len);
static void _CloseArchive(ArchiveHandle *AH);
static void _PrintTocData(ArchiveHandle *AH, TocEntry *te);
static void _StartLOs(ArchiveHandle *AH, TocEntry *te);
static void _StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLOs(ArchiveHandle *AH, TocEntry *te);

static size_t _CustomOutPtr(ArchiveHandle *AH, const void *buf, size_t len);
static FILE *_openFileForWrite(ArchiveHandle *AH);
static void _SetNameFromTag(char *buf, const char *tag);
static int	_CleanPathComponent(char *buf, const char *pathComponent);

typedef struct
{
	/*
	 * The backup directory as specified on the command line. All files and
	 * directories will be created inside this directory.
	 */
	char	   *directory;

	char		catalogFilename[MAXPGPATH];
	CompressFileHandle *catalogFH;

	char		currentFilename[MAXPGPATH];
	char		precedingFilename[MAXPGPATH];

	/*
	 * We merge consecutive outputs not associated with a TocEntry into the
	 * same file, while maintaning restore order. This flag indicates that the
	 * preceding file is suitable for appending output not associated with
	 * TocEntry (TELess).
	 */
	bool		precedingFileWasTELess;

	/*
	 * The last path component of the preceding TELess path or empty if the
	 * preceding file is associated with a TocEntry
	 */
	char		precedingTELessFilename[MAXPGPATH];

	/*
	 * counter for unique filenames in global folder for for files not
	 * assotiated with a TocEntry.
	 */
	unsigned int fileSerialIdTELess;
} lclContext;


/*
 *	Initializer
 */
void
InitArchiveFmt_Structured(ArchiveHandle *AH)
{
	lclContext *ctx;

	/* Assuming static functions, this can be copied for each format. */
	AH->WriteDataPtr = _WriteData;
	AH->EndDataPtr = _EndData;
	AH->WriteBytePtr = _WriteByte;
	AH->WriteBufPtr = _WriteBuf;
	AH->ClosePtr = _CloseArchive;
	AH->ReopenPtr = NULL;
	AH->PrintTocDataPtr = _PrintTocData;

	AH->StartLOsPtr = _StartLOs;
	AH->StartLOPtr = _StartLO;
	AH->EndLOPtr = _EndLO;
	AH->EndLOsPtr = _EndLOs;
	AH->ClonePtr = NULL;
	AH->DeClonePtr = NULL;

	/* Initialize LO buffering */
	AH->lo_buf_size = LOBBUFSIZE;
	AH->lo_buf = (void *) pg_malloc(LOBBUFSIZE);

	AH->CustomOutPtr = _CustomOutPtr;

	/* Set up a private area. */
	ctx = (lclContext *) pg_malloc0(sizeof(lclContext));
	AH->formatData = (void *) ctx;


	/*
	 * Make sure the output is not compressed
	 */
	if (AH->compression_spec.algorithm != PG_COMPRESSION_NONE)
		pg_fatal("compression is not supported by structured format");


	if (AH->mode == archModeWrite)
	{
		struct stat st;
		CompressFileHandle *CFH;

		if (!AH->fSpec || strcmp(AH->fSpec, "") == 0)
			pg_fatal("no output directory specified");

		ctx->catalogFH = NULL;
		ctx->catalogFilename[0] = '\0';
		ctx->currentFilename[0] = '\0';
		ctx->directory = AH->fSpec;
		ctx->fileSerialIdTELess = 0;
		ctx->precedingFilename[0] = '\0';
		ctx->precedingFileWasTELess = false;

		if (stat(ctx->directory, &st) < 0)
		{
			/* not exists, try to create the directory */
			if (mkdir(ctx->directory, 0700) < 0)
				pg_fatal("could not create output directory \"%s\": %m",
						 ctx->directory);
		}
		else
		{
			if (S_ISDIR(st.st_mode))
			{
				/* we accept an empty existing directory */
				bool		is_empty = true;
				DIR		   *dir = opendir(ctx->directory);

				if (dir)
				{
					struct dirent *d;

					while (errno = 0, (d = readdir(dir)))
					{
						if (strcmp(d->d_name, ".") == 0 ||
							strcmp(d->d_name, "..") == 0)
							continue;
						is_empty = false;
						break;
					}

					if (errno)
						pg_fatal("could not read directory \"%s\": %m",
								 ctx->directory);

					if (closedir(dir))
						pg_fatal("could not close directory \"%s\": %m",
								 ctx->directory);

					if (!is_empty)
						pg_fatal("existing directory should be empty \"%s\": %m",
								 ctx->directory);
				}
				else
				{
					pg_fatal("could not open directory \"%s\": %m",
							 ctx->directory);
				}
			}
			else
				pg_fatal("could not read directory \"%s\": %m",
						 ctx->directory);
		}

		/* now create and open the catalog file in this directory */
		if (strlen(ctx->directory) + 1 + 16 + 1 > MAXPGPATH)
			pg_fatal("file name too long: \"%s\"", ctx->directory);

		strcpy(ctx->catalogFilename, ctx->directory);
		strcat(ctx->catalogFilename, "/restore-dump.sql");

		CFH = InitCompressFileHandle(AH->compression_spec);
		if (CFH->open_func(ctx->catalogFilename, -1, PG_BINARY_W, CFH))
			pg_fatal("could not open catalog file \"%s\": %m",
					 ctx->catalogFilename);

		ctx->catalogFH = CFH;

		if (!ctx->catalogFH)
			pg_fatal("could not open catalog file \"%s\": %m",
					 ctx->catalogFilename);
	}
	else
	{
		pg_fatal("This format can be restored by feeding restore-dump.sql to psql");
	}
}

/*
 * - Start a new TOC entry
 */

/*
 * Called by dumper via archiver from within a data dump routine
 */
static void
_WriteData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	/* Just send it to output, ahwrite() already errors on failure */
	ahwrite(data, 1, dLen, AH);
}

/*
 * Called by dumper via archiver from within a data dump routine
 * We substitute this for _WriteData while emitting a LO
 */
static void
_WriteLOData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	if (dLen > 0)
	{
		PQExpBuffer buf = createPQExpBuffer();

		appendByteaLiteralAHX(buf,
							  (const unsigned char *) data,
							  dLen,
							  AH);

		ahprintf(AH, "SELECT pg_catalog.lowrite(0, %s);\n", buf->data);

		destroyPQExpBuffer(buf);
	}
}

static void
_EndData(ArchiveHandle *AH, TocEntry *te)
{
	ahprintf(AH, "\n\n");
}

/*
 * Called by the archiver when starting to save all BLOB DATA (not schema).
 * This routine should save whatever format-specific information is needed
 * to read the LOs back into memory.
 *
 * It is called just prior to the dumper's DataDumper routine.
 *
 * Optional, but strongly recommended.
 */
static void
_StartLOs(ArchiveHandle *AH, TocEntry *te)
{
	ahprintf(AH, "BEGIN;\n\n");
}

/*
 * Called by the archiver when the dumper calls StartLO.
 *
 * Mandatory.
 *
 * Must save the passed OID for retrieval at restore-time.
 */
static void
_StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	bool		old_lo_style = (AH->version < K_VERS_1_12);

	if (oid == 0)
		pg_fatal("invalid OID for large object");

	/* With an old archive we must do drop and create logic here */
	if (old_lo_style && AH->public.ropt->dropSchema)
		DropLOIfExists(AH, oid);

	if (old_lo_style)
		ahprintf(AH, "SELECT pg_catalog.lo_open(pg_catalog.lo_create('%u'), %d);\n",
				 oid, INV_WRITE);
	else
		ahprintf(AH, "SELECT pg_catalog.lo_open('%u', %d);\n",
				 oid, INV_WRITE);

	AH->WriteDataPtr = _WriteLOData;
}

/*
 * Called by the archiver when the dumper calls EndLO.
 *
 * Optional.
 */
static void
_EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	AH->WriteDataPtr = _WriteData;

	ahprintf(AH, "SELECT pg_catalog.lo_close(0);\n\n");
}

/*
 * Called by the archiver when finishing saving all BLOB DATA.
 *
 * Optional.
 */
static void
_EndLOs(ArchiveHandle *AH, TocEntry *te)
{
	ahprintf(AH, "COMMIT;\n\n");
}

/*------
 * Called as part of a RestoreArchive call; for the NULL archive, this
 * just sends the data for a given TOC entry to the output.
 *------
 */
static void
_PrintTocData(ArchiveHandle *AH, TocEntry *te)
{
	if (te->dataDumper)
	{
		AH->currToc = te;

		if (strcmp(te->desc, "BLOBS") == 0)
			_StartLOs(AH, te);

		te->dataDumper((Archive *) AH, te->dataDumperArg);

		if (strcmp(te->desc, "BLOBS") == 0)
			_EndLOs(AH, te);

		AH->currToc = NULL;
	}
}

static int
_WriteByte(ArchiveHandle *AH, const int i)
{
	/* Don't do anything */
	return 0;
}

static void
_WriteBuf(ArchiveHandle *AH, const void *buf, size_t len)
{
	/* Don't do anything */
}

/*
 * We split the output of the plaintext dump into multiple files.
 * The file path depends on currentTE and is choosen in _openFileForWrite
 */
static size_t
_CustomOutPtr(ArchiveHandle *AH, const void *buf, size_t len)
{
	int			bytesWritten = 0;
	FILE	   *cf;
	lclContext *ctx = (lclContext *) AH->formatData;

	if (len == 0)
		return 0;

	/* construct the target path and open it for write */
	cf = _openFileForWrite(AH);

	/* this is cathed before, pure paranoia */
	if (!cf)
		pg_fatal("no filehandle get to write.");

	bytesWritten = fwrite(buf, 1, len, cf);

	/* we close the file right here */
	fclose(cf);

	/* sync the resulting file, errors are not fatal */
	if (AH->dosync)
		(void) fsync_fname(ctx->currentFilename, false);

	strcpy(ctx->precedingFilename, ctx->currentFilename);

	return bytesWritten;
}


/*
 * Determine the filepath using the properties of currentTE.
 * This function creates the necessary directories and opens the file for write.
 * On success returns the filehandle, on error terminates the dump.
 */
static FILE *
_openFileForWrite(ArchiveHandle *AH)
{
	FILE	   *fh = NULL;
	char	   *dname;

	/* The constructed filename relative (inside) to the directory dname */
	char		relfname[MAXPGPATH];
	int			relfnameLen;
	char		fsfname[MAXPGPATH];
	int			fsfnameLen;


	/* The constructed directoryname relative (inside) to the directory dname */
	char		reldname[MAXPGPATH];
	int			reldnameLen;

	char		fullpath[MAXPGPATH];

	/* /i filepath\n0 */
	char		catalogLine[MAXPGPATH + 5];
	int			catalogLineLen;
	int			catalogLineBytesWritten;

	/*
	 * The tag or the string "_empty_tag" when the tag property is empty,
	 * suffixed with an underscore and a 4 byte hex hash.
	 */
	char		tagPart[MAXPGPATH];

	/*
	 * The namespace component of the path with a slash suffix, or an epty
	 * string when the namespace is not defined this, to make the path
	 * construction simpler. A nonempty nsPart is suffixed with a hash to avoid
	 * name collisions on case insensitive filesystems.
	 */
	char		nsPart[MAXPGPATH];
	int			i;
	bool		folderIsTELess = false;

	struct stat st;
	lclContext *ctx = (lclContext *) AH->formatData;

	dname = ctx->directory;

	if (!AH->currentTE)
	{
		/*
		 * create a new file in global folder directory/globals/global_n.sql
		 */
		reldnameLen = snprintf(reldname, MAXPGPATH, "globals");
		folderIsTELess = true;

		if (ctx->precedingFileWasTELess)
		{
			strcpy(relfname, ctx->precedingTELessFilename);
			relfnameLen = strlen(relfname);
		}
		else
			relfnameLen = snprintf(relfname, MAXPGPATH, "global_%u.sql",
								   ++ctx->fileSerialIdTELess);
	}
	else
	{
		TocEntry   *te = AH->currentTE;

		if (!te->namespace || strlen(te->namespace) == 0)
			nsPart[0] = '\0';
		else
		{
			char	cleanNS[MAXPGPATH];

			_CleanPathComponent(cleanNS, te->namespace);

			snprintf(nsPart, MAXPGPATH, "schemas/%s_%x/", cleanNS,
					 string_hash(cleanNS, strlen(cleanNS)));
		}

		if (!te->tag || strlen(te->tag) == 0)
			strcpy(tagPart, "_empty_tag");
		else
		{
			/*
			 * We suffix the tag with a hex hash to avoid filename collisions on
			 * case insensitive filesystems.
			 */
			char unsafeTagPart[MAXPGPATH];

			snprintf(unsafeTagPart, NAMEDATALEN + 9, "%s_%x", te->tag,
					string_hash(te->tag, strlen(te->tag)));

			/* Make sure the string is FS safe */
			_CleanPathComponent(tagPart, unsafeTagPart);
		}

		relfnameLen = snprintf(relfname, MAXPGPATH, "%s.sql", tagPart);

		if (strcmp(te->desc, "<Init>") == 0)
		{
			/* should we ignore this? */
			reldnameLen = snprintf(reldname, MAXPGPATH, "globals/init");
		}
		else if (strcmp(te->desc, "ACL LANGUAGE") == 0 ||
				 strcmp(te->desc, "ACL") == 0 ||
				 strcmp(te->desc, "DATABASE PROPERTIES") == 0 ||
				 strcmp(te->desc, "DATABASE") == 0 ||
				 strcmp(te->desc, "ENCODING") == 0 ||
				 strcmp(te->desc, "SERVER") == 0 ||
				 strcmp(te->desc, "STDSTRINGS") == 0)
		{
			/* for example directory/globals/DATABASE */
			reldnameLen = snprintf(reldname, MAXPGPATH, "globals/%s", te->desc);
		}
		else if (strcmp(te->desc, "BLOB") == 0)
		{
			/* BLOB is under the directory/data/LO */
			reldnameLen = snprintf(reldname, MAXPGPATH, "data/%sLO", nsPart);
		}
		else if (strcmp(te->desc, "BLOBS") == 0)
		{
			/* BLOBS is under the directory/data/LOs */
			reldnameLen = snprintf(reldname, MAXPGPATH, "data/%sLOs", nsPart);
			if (te->tag && strcmp(te->tag, "BLOBS") == 0)
			{
				/* override default filename to avoid BLOB terminology */
				strcpy(relfname, "LOs.sql");
				relfnameLen = 7;
			}
		}
		else if (strcmp(te->desc, "BLOB COMMENTS") == 0)
		{
			/* BLOB COMMENTS is under the directory/data/LO COMMENTS */
			reldnameLen = snprintf(reldname, MAXPGPATH, "data/%sLO COMMENTS",
								   nsPart);
		}
		else if (strcmp(te->desc, "MATERIALIZED VIEW DATA") == 0 ||
				 strcmp(te->desc, "TABLE DATA") == 0)
		{
			/* data is under directory/data */
			reldnameLen = snprintf(reldname, MAXPGPATH, "data/%s%s", nsPart,
								   te->desc);
		}
		else if (strcmp(te->desc, "CHECK CONSTRAINT") == 0 ||
				 strcmp(te->desc, "CONSTRAINT") == 0 ||
				 strcmp(te->desc, "FK CONSTRAINT") == 0 ||
				 strcmp(te->desc, "RULE") == 0 ||
				 strcmp(te->desc, "TRIGGER") == 0 ||
				 strcmp(te->desc, "POLICY") == 0 ||
				 strcmp(te->desc, "DEFAULT") == 0)
		{
			char		unsafeTblName[NAMEDATALEN + 1];
			char		tblName[MAXPGPATH];

			_SetNameFromTag(unsafeTblName, te->tag);

			if (strlen(unsafeTblName))
			{
				/*
				 * store it next to the table
				 * directory/schemas/schemaname/TABLE/tblname/CONSTRAINT/
				 */
				
				/* make the tablename safe to use it as a directory name */
				_CleanPathComponent(tblName, unsafeTblName);

				reldnameLen = snprintf(reldname, MAXPGPATH, "%sTABLE/%s_%x/%s",
									   nsPart, tblName,
									   string_hash(tblName, strlen(tblName)),
									   te->desc);
			}
			else
			{
				/*
				 * no tablename found, store it in the schema root
				 * directory/schemas/schemaname/CONSTRAINT/
				 */
				reldnameLen = snprintf(reldname, MAXPGPATH, "%s%s", nsPart,
									   te->desc);
			}
		}
		else if (strcmp(te->desc, "COMMENT") == 0)
		{
			if (te->tag && strncmp(te->tag, "LARGE OBJECT", 12) == 0)
			{
				/*
				 * COMMENT ON LARGE OBJECT is stored under the
				 * directory/data/LO COMMENTS directory
				 */
				reldnameLen = snprintf(reldname, MAXPGPATH,
									   "data/%sLO COMMENTS", nsPart);
			}
			else
			{
				/*
				 * te->tag containts some hints on the commented object eg:
				 * "COLUMN tbl.col", "CONSTRAINT name ON tbl", "CONSTRAINT
				 * name ON DOMAIN dom", "TABLE tbl" "FUNCTION fname(parname
				 * partype)" we do NOT use this info to place the comment next
				 * to the object. All comments will go to directory/COMMENTS
				 * or to directory/schemas/schemaname/COMMENT
				 */
				reldnameLen = snprintf(reldname, MAXPGPATH, "%s%s", nsPart,
									   te->desc);
			}
		}
		else if (strcmp(te->desc, "TABLE") == 0)
		{
			/* directory/schemas/schemaname/TABLE/tablename */
			reldnameLen = snprintf(reldname, MAXPGPATH, "%s%s/%s", nsPart,
								   te->desc, tagPart);
		}
		else if (strcmp(te->desc, "SCHEMA") == 0)
		{
			/* directory/schemas/foobar/SCHEMA/foobar.sql */
			reldnameLen = snprintf(reldname, MAXPGPATH, "schemas/%s/%s",
								   tagPart, te->desc);
		}
		else if (strlen(te->desc) == 0)
		{
			/*
			 * when no desc is given the file is stored under
			 * directory/_empty_desc_
			 */
			reldnameLen = snprintf(reldname, MAXPGPATH, "%s_empty_desc_",
								   nsPart);
		}
		else
		{
			/* handle all other te->desc cases */
			char		cleanDesc[MAXPGPATH];

			_CleanPathComponent(cleanDesc, te->desc);

			reldnameLen = snprintf(reldname, MAXPGPATH, "%s%s", nsPart,
								   cleanDesc);
		}
	}

	if (strlen(dname) + 1 + reldnameLen + 1 + relfnameLen + 1 > MAXPGPATH)
		pg_fatal("file name too long: \"%s\"", dname);

	/* get an FS safe filename */
	fsfnameLen = _CleanPathComponent(fsfname, relfname);

	if (strlen(dname) + 1 + reldnameLen + 1 + fsfnameLen + 1 > MAXPGPATH)
		pg_fatal("file name too long: \"%s\"", dname);

	/* replace whitespaces with underscore in reldname */
	i = 0;
	while (reldname[i])
	{
		if (isspace((unsigned char) reldname[i]))
			reldname[i] = '_';
		i++;
	}

	/* construct the directory name */
	strcpy(fullpath, dname);
	strcat(fullpath, "/");
	strcat(fullpath, reldname);

	/* check if the directory exists, if not, create it recursively */
	if (stat(fullpath, &st) < 0)
	{
		if (pg_mkdir_p(fullpath, 0700) < 0)
			pg_fatal("could not create directory \"%s\": %m", fullpath);
	}

	/* append filename */
	strcat(fullpath, "/");
	strcat(fullpath, fsfname);

	if (strcmp(ctx->precedingFilename, fullpath) != 0)
	{
		if (stat(fullpath, &st) == 0)
		{
			/*
			 * This is not the same filename as in the preceding call, but a
			 * file with this name already exists. When the current output
			 * appended to a previously saved file, the restore order is not
			 * maintained anymore. For now, we log it.
			 *
			 * TODO: make sure the output goes into a new file, while
			 * maintaning the dump logic. preceeding calls to this file should
			 * go into the last file. Avoid creating unnecessary new files.
			 * An alternative to this would be to declare this format as a
			 * write-only format.
			 */
			pg_log_warning("can not maintain restore order, appending to previous file: \"%s\"",
						   fullpath);
		}

		/*
		 * Output filename changed, maintain our catalog file append a new
		 * line with "\i reldname/relfname\n" to our "catalog"
		 * restore-dump.sql opened to ctx->catalogFH
		 */
		catalogLineLen = snprintf(catalogLine, MAXPGPATH + 5, "\\i %s/%s\n",
								  reldname, relfname);
		if (catalogLineLen > MAXPGPATH + 5)
			pg_fatal("file entry too long: \"%s\"", catalogLine);

		if (ctx->catalogFH)
		{
			CompressFileHandle *CFH = (CompressFileHandle *) ctx->catalogFH;

			catalogLineBytesWritten = CFH->write_func(catalogLine,
													  catalogLineLen, CFH);

			if (catalogLineBytesWritten != catalogLineLen)
				pg_fatal("could not write \"%s/\": %m", ctx->catalogFilename);
		}
		else
			pg_fatal("catalog file is not ready for write \"%s\": %m",
					 ctx->catalogFilename);
	}

	if (ctx->precedingFileWasTELess != folderIsTELess)
	{
		ctx->precedingFileWasTELess = folderIsTELess;
		if (folderIsTELess)
			strcpy(ctx->precedingTELessFilename, relfname);
		else
			ctx->precedingTELessFilename[0] = '\0';
	}

	/* open it for append, we write multiple times into the same file */
	fh = fopen(fullpath, PG_BINARY_A);
	if (!fh)
		pg_fatal("could not open output file \"%s\": %m", fullpath);

	strcpy(ctx->currentFilename, fullpath);

	return fh;
}


/*
 * Extracts the tablename from the te->tag property
 * this, by copying the first word from tag string into buf
 * see: pg_dump.c, the content of the tag variable consists:
 * tablename space constraint_name
 * tag = psprintf("%s %s"
 * this valid for:
 * POLICY -> tablename
 * PUBLICATION TABLES IN SCHEMA -> publication_name
 * PUBLICATION TABLE -> publication_name
 * DEFAULT -> tablename
 * CONSTRAINT -> tablename
 * FK CONSTRAINT -> tablename
 * CHECK CONSTRAINT -> tablename OR! domainname
 * TRIGGER -> tablename
 * RULE -> tablename
 *
 * The result is always only the first word from the tag contents
 * but that is OK for us.
 */
static void
_SetNameFromTag(char *buf, const char *tag)
{
	int			i = 0;

	while (tag[i] && tag[i] != ' ' && i < NAMEDATALEN)
	{
		buf[i] = tag[i];
		i++;
	}
	buf[i] = '\0';
}

/*
 * Replaces invalid or problematic characters with a safe representation and
 * stores the resulting new string into buf. The string taken as a path
 * component, path separators will be replaced. The size of buf should be at
 * least MAXPGPATH. When the resulting string would be longer than MAXPGPATH the
 * dump will be terminated with error.
 * 
 * The same replacements used on all platforms to get consistent filenames.
 * Returns the length of the new string
 */
static int
_CleanPathComponent(char *buf, const char *pathComponent)
{
	int			rpos = 0;
	int			wpos = 0;
	bool		tooLong = false;

	while (pathComponent[rpos] && rpos < MAXPGPATH && wpos < MAXPGPATH)
	{
		if (pathComponent[rpos] == '<')
		{
			if (wpos + 4 >= MAXPGPATH)
			{
				tooLong = true;
				break;
			}

			buf[wpos++] = '_';
			buf[wpos++] = 'l';
			buf[wpos++] = 't';
			buf[wpos++] = '_';
		}
		else if (pathComponent[rpos] == '>')
		{
			if (wpos + 4 >= MAXPGPATH)
			{
				tooLong = true;
				break;
			}

			buf[wpos++] = '_';
			buf[wpos++] = 'g';
			buf[wpos++] = 't';
			buf[wpos++] = '_';
		}
		else if (pathComponent[rpos] == ':')
		{
			if (wpos + 7 >= MAXPGPATH)
			{
				tooLong = true;
				break;
			}

			buf[wpos++] = '_';
			buf[wpos++] = 'c';
			buf[wpos++] = 'o';
			buf[wpos++] = 'l';
			buf[wpos++] = 'o';
			buf[wpos++] = 'n';
			buf[wpos++] = '_';
		}
		else if (pathComponent[rpos] == '/')
		{
			if (wpos + 4 >= MAXPGPATH)
			{
				tooLong = true;
				break;
			}

			buf[wpos++] = '_';
			buf[wpos++] = 'f';
			buf[wpos++] = 's';
			buf[wpos++] = '_';
		}
		else if (pathComponent[rpos] == '\\')
		{
			if (wpos + 4 >= MAXPGPATH)
			{
				tooLong = true;
				break;
			}

			buf[wpos++] = '_';
			buf[wpos++] = 'b';
			buf[wpos++] = 's';
			buf[wpos++] = '_';
		}
		else if (pathComponent[rpos] == '|')
		{
			if (wpos + 6 >= MAXPGPATH)
			{
				tooLong = true;
				break;
			}

			buf[wpos++] = '_';
			buf[wpos++] = 'p';
			buf[wpos++] = 'i';
			buf[wpos++] = 'p';
			buf[wpos++] = 'e';
			buf[wpos++] = '_';
		}
		else if (pathComponent[rpos] == '?')
		{
			if (wpos + 4 >= MAXPGPATH)
			{
				tooLong = true;
				break;
			}

			buf[wpos++] = '_';
			buf[wpos++] = 'q';
			buf[wpos++] = 'm';
			buf[wpos++] = '_';
		}
		else if (pathComponent[rpos] == '*')
		{
			buf[wpos++] = 'x';
		}
		else if (isspace((unsigned char) pathComponent[rpos]))
		{
			buf[wpos++] = '_';
		}
		else
		{
			buf[wpos++] = pathComponent[rpos];
		}
		rpos++;
	}

	if (wpos >= MAXPGPATH || tooLong)
		pg_fatal("file name too long: \"%s\"", pathComponent);

	buf[wpos] = '\0';

	return wpos;
}

static void
_CloseArchive(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	int			res = 0;

	if (ctx->catalogFH)
	{
		/* Close the catalog file */
		errno = 0;
		res = EndCompressFileHandle(ctx->catalogFH);

		if (res != 0)
			pg_fatal("could not close catalog file \"%s\": %m",
					 ctx->catalogFilename);

		/* sync the resulting file, errors are not fatal */
		if (AH->dosync)
			(void) fsync_fname(ctx->catalogFilename, false);
	}
}
