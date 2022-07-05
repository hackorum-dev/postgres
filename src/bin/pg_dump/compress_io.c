/*-------------------------------------------------------------------------
 *
 * compress_io.c
 *	 Routines for archivers to write an uncompressed or compressed data
 *	 stream.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * This file includes two APIs for dealing with compressed data. The first
 * provides more flexibility, using callbacks to read/write data from the
 * underlying stream. The second API is a wrapper around fopen/gzopen and
 * friends, providing an interface similar to those, but abstracts away
 * the possible compression. Both APIs use libz for the compression, but
 * the second API uses gzip headers, so the resulting files can be easily
 * manipulated with the gzip utility.
 *
 * Compressor API
 * --------------
 *
 *	The interface for writing to an archive consists of three functions:
 *	AllocateCompressor, WriteDataToArchive and EndCompressor. First you call
 *	AllocateCompressor, then write all the data by calling WriteDataToArchive
 *	as many times as needed, and finally EndCompressor. WriteDataToArchive
 *	and EndCompressor will call the WriteFunc that was provided to
 *	AllocateCompressor for each chunk of compressed data.
 *
 *	The interface for reading an archive consists of just one function:
 *	ReadDataFromArchive. ReadDataFromArchive reads the whole compressed input
 *	stream, by repeatedly calling the given ReadFunc. ReadFunc returns the
 *	compressed data chunk at a time, and ReadDataFromArchive decompresses it
 *	and passes the decompressed data to ahwrite(), until ReadFunc returns 0
 *	to signal EOF.
 *
 *	The interface is the same for compressed and uncompressed streams.
 *
 * Compressed stream API
 * ----------------------
 *
 *	The compressed stream API is a wrapper around the C standard fopen() and
 *	libz's gzopen() APIs. It allows you to use the same functions for
 *	compressed and uncompressed streams. cfopen_read() first tries to open
 *	the file with given name, and if it fails, it tries to open the same
 *	file with the .gz suffix. cfopen_write() opens a file for writing, an
 *	extra argument specifies if the file should be compressed, and adds the
 *	.gz suffix to the filename if so. This allows you to easily handle both
 *	compressed and uncompressed files.
 *
 * IDENTIFICATION
 *	   src/bin/pg_dump/compress_io.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "compress_io.h"
#include "pg_backup_utils.h"

/*----------------------
 * Compressor API
 *----------------------
 */

/* typedef appears in compress_io.h */
struct CompressorState
{
	pg_compress_algorithm compress_algorithm;
	WriteFunc	writeF;

#ifdef HAVE_LIBZ
	z_streamp	zp;
	char	   *zlibOut;
	size_t		zlibOutSize;
#endif
};

/* Routines that support zlib compressed data I/O */
#ifdef HAVE_LIBZ
static void InitCompressorZlib(CompressorState *cs, int level);
static void DeflateCompressorZlib(ArchiveHandle *AH, CompressorState *cs,
								  bool flush);
static void ReadDataFromArchiveZlib(ArchiveHandle *AH, ReadFunc readF);
static void WriteDataToArchiveZlib(ArchiveHandle *AH, CompressorState *cs,
								   const char *data, size_t dLen);
static void EndCompressorZlib(ArchiveHandle *AH, CompressorState *cs);
#endif

/* Routines that support uncompressed data I/O */
static void ReadDataFromArchiveNone(ArchiveHandle *AH, ReadFunc readF);
static void WriteDataToArchiveNone(ArchiveHandle *AH, CompressorState *cs,
								   const char *data, size_t dLen);

/* Public interface routines */

/* Allocate a new compressor */
CompressorState *
AllocateCompressor(const pg_compress_specification compress_spec,
				   WriteFunc writeF)
{
	CompressorState *cs;

#ifndef HAVE_LIBZ
	if (compress_spec.algorithm == PG_COMPRESSION_GZIP)
		pg_fatal("not built with zlib support");
#endif

	cs = (CompressorState *) pg_malloc0(sizeof(CompressorState));
	cs->writeF = writeF;
	cs->compress_algorithm = compress_spec.algorithm;

	/*
	 * Perform compression algorithm specific initialization.
	 */
#ifdef HAVE_LIBZ
	if (cs->compress_algorithm == PG_COMPRESSION_GZIP)
		InitCompressorZlib(cs, compress_spec.level);
#endif

	return cs;
}

/*
 * Read all compressed data from the input stream (via readF) and print it
 * out with ahwrite().
 */
void
ReadDataFromArchive(ArchiveHandle *AH, pg_compress_specification compress_spec,
					ReadFunc readF)
{
	switch (compress_spec.algorithm)
	{
		case PG_COMPRESSION_NONE:
			ReadDataFromArchiveNone(AH, readF);
			break;
		case PG_COMPRESSION_GZIP:
#ifdef HAVE_LIBZ
			ReadDataFromArchiveZlib(AH, readF);
#else
			pg_fatal("not built with zlib support");
#endif
			break;
		default:
			pg_fatal("invalid compression method");
			break;
	}
}

/*
 * Compress and write data to the output stream (via writeF).
 */
void
WriteDataToArchive(ArchiveHandle *AH, CompressorState *cs,
				   const void *data, size_t dLen)
{
	switch (cs->compress_algorithm)
	{
		case PG_COMPRESSION_GZIP:
#ifdef HAVE_LIBZ
			WriteDataToArchiveZlib(AH, cs, data, dLen);
#else
			pg_fatal("not built with zlib support");
#endif
			break;
		case PG_COMPRESSION_NONE:
			WriteDataToArchiveNone(AH, cs, data, dLen);
			break;
		default:
			pg_fatal("invalid compression method");
			break;
	}
}

/*
 * Terminate compression library context and flush its buffers.
 */
void
EndCompressor(ArchiveHandle *AH, CompressorState *cs)
{
	switch (cs->compress_algorithm)
	{
		case PG_COMPRESSION_GZIP:
#ifdef HAVE_LIBZ
			EndCompressorZlib(AH, cs);
#else
			pg_fatal("not built with zlib support");
#endif
			break;
		case PG_COMPRESSION_NONE:
			free(cs);
			break;

		default:
			pg_fatal("invalid compression method");
			break;
	}
}

/* Private routines, specific to each compression method. */

#ifdef HAVE_LIBZ
/*
 * Functions for zlib compressed output.
 */

static void
InitCompressorZlib(CompressorState *cs, int level)
{
	z_streamp	zp;

	zp = cs->zp = (z_streamp) pg_malloc(sizeof(z_stream));
	zp->zalloc = Z_NULL;
	zp->zfree = Z_NULL;
	zp->opaque = Z_NULL;

	/*
	 * zlibOutSize is the buffer size we tell zlib it can output to.  We
	 * actually allocate one extra byte because some routines want to append a
	 * trailing zero byte to the zlib output.
	 */
	cs->zlibOut = (char *) pg_malloc(ZLIB_OUT_SIZE + 1);
	cs->zlibOutSize = ZLIB_OUT_SIZE;

	if (deflateInit(zp, level) != Z_OK)
		pg_fatal("could not initialize compression library: %s",
				 zp->msg);

	/* Just be paranoid - maybe End is called after Start, with no Write */
	zp->next_out = (void *) cs->zlibOut;
	zp->avail_out = cs->zlibOutSize;
}

static void
EndCompressorZlib(ArchiveHandle *AH, CompressorState *cs)
{
	z_streamp	zp = cs->zp;

	zp->next_in = NULL;
	zp->avail_in = 0;

	/* Flush any remaining data from zlib buffer */
	DeflateCompressorZlib(AH, cs, true);

	if (deflateEnd(zp) != Z_OK)
		pg_fatal("could not close compression stream: %s", zp->msg);

	free(cs->zlibOut);
	free(cs->zp);
}

static void
DeflateCompressorZlib(ArchiveHandle *AH, CompressorState *cs, bool flush)
{
	z_streamp	zp = cs->zp;
	char	   *out = cs->zlibOut;
	int			res = Z_OK;

	while (cs->zp->avail_in != 0 || flush)
	{
		res = deflate(zp, flush ? Z_FINISH : Z_NO_FLUSH);
		if (res == Z_STREAM_ERROR)
			pg_fatal("could not compress data: %s", zp->msg);
		if ((flush && (zp->avail_out < cs->zlibOutSize))
			|| (zp->avail_out == 0)
			|| (zp->avail_in != 0)
			)
		{
			/*
			 * Extra paranoia: avoid zero-length chunks, since a zero length
			 * chunk is the EOF marker in the custom format. This should never
			 * happen but...
			 */
			if (zp->avail_out < cs->zlibOutSize)
			{
				/*
				 * Any write function should do its own error checking but to
				 * make sure we do a check here as well...
				 */
				size_t		len = cs->zlibOutSize - zp->avail_out;

				cs->writeF(AH, out, len);
			}
			zp->next_out = (void *) out;
			zp->avail_out = cs->zlibOutSize;
		}

		if (res == Z_STREAM_END)
			break;
	}
}

static void
WriteDataToArchiveZlib(ArchiveHandle *AH, CompressorState *cs,
					   const char *data, size_t dLen)
{
	cs->zp->next_in = (void *) unconstify(char *, data);
	cs->zp->avail_in = dLen;
	DeflateCompressorZlib(AH, cs, false);
}

static void
ReadDataFromArchiveZlib(ArchiveHandle *AH, ReadFunc readF)
{
	z_streamp	zp;
	char	   *out;
	int			res = Z_OK;
	size_t		cnt;
	char	   *buf;
	size_t		buflen;

	zp = (z_streamp) pg_malloc(sizeof(z_stream));
	zp->zalloc = Z_NULL;
	zp->zfree = Z_NULL;
	zp->opaque = Z_NULL;

	buf = pg_malloc(ZLIB_IN_SIZE);
	buflen = ZLIB_IN_SIZE;

	out = pg_malloc(ZLIB_OUT_SIZE + 1);

	if (inflateInit(zp) != Z_OK)
		pg_fatal("could not initialize compression library: %s",
				 zp->msg);

	/* no minimal chunk size for zlib */
	while ((cnt = readF(AH, &buf, &buflen)))
	{
		zp->next_in = (void *) buf;
		zp->avail_in = cnt;

		while (zp->avail_in > 0)
		{
			zp->next_out = (void *) out;
			zp->avail_out = ZLIB_OUT_SIZE;

			res = inflate(zp, 0);
			if (res != Z_OK && res != Z_STREAM_END)
				pg_fatal("could not uncompress data: %s", zp->msg);

			out[ZLIB_OUT_SIZE - zp->avail_out] = '\0';
			ahwrite(out, 1, ZLIB_OUT_SIZE - zp->avail_out, AH);
		}
	}

	zp->next_in = NULL;
	zp->avail_in = 0;
	while (res != Z_STREAM_END)
	{
		zp->next_out = (void *) out;
		zp->avail_out = ZLIB_OUT_SIZE;
		res = inflate(zp, 0);
		if (res != Z_OK && res != Z_STREAM_END)
			pg_fatal("could not uncompress data: %s", zp->msg);

		out[ZLIB_OUT_SIZE - zp->avail_out] = '\0';
		ahwrite(out, 1, ZLIB_OUT_SIZE - zp->avail_out, AH);
	}

	if (inflateEnd(zp) != Z_OK)
		pg_fatal("could not close compression library: %s", zp->msg);

	free(buf);
	free(out);
	free(zp);
}
#endif							/* HAVE_LIBZ */


/*
 * Functions for uncompressed output.
 */

static void
ReadDataFromArchiveNone(ArchiveHandle *AH, ReadFunc readF)
{
	size_t		cnt;
	char	   *buf;
	size_t		buflen;

	buf = pg_malloc(ZLIB_OUT_SIZE);
	buflen = ZLIB_OUT_SIZE;

	while ((cnt = readF(AH, &buf, &buflen)))
	{
		ahwrite(buf, 1, cnt, AH);
	}

	free(buf);
}

static void
WriteDataToArchiveNone(ArchiveHandle *AH, CompressorState *cs,
					   const char *data, size_t dLen)
{
	cs->writeF(AH, data, dLen);
}


/*----------------------
 * Compressed stream API
 *----------------------
 */

/*
 * cfp represents an open stream, wrapping the underlying FILE or gzFile
 * pointer. This is opaque to the callers.
 */
struct cfp
{
	pg_compress_algorithm compress_algorithm;
	void	   *fp;
};

#ifdef HAVE_LIBZ
static int	hasSuffix(const char *filename, const char *suffix);
#endif

/* free() without changing errno; useful in several places below */
static void
free_keep_errno(void *p)
{
	int			save_errno = errno;

	free(p);
	errno = save_errno;
}

/*
 * Open a file for reading. 'path' is the file to open, and 'mode' should
 * be either "r" or "rb".
 *
 * If the file at 'path' does not exist, we append the ".gz" suffix (if 'path'
 * doesn't already have it) and try again. So if you pass "foo" as 'path',
 * this will open either "foo" or "foo.gz".
 *
 * On failure, return NULL with an error code in errno.
 */
cfp *
cfopen_read(const char *path, const char *mode)
{
	cfp		   *fp;
	pg_compress_specification compress_spec = {0};

	compress_spec.algorithm = PG_COMPRESSION_GZIP;
#ifdef HAVE_LIBZ
	if (hasSuffix(path, ".gz"))
		fp = cfopen(path, mode, compress_spec);
	else
#endif
	{
		compress_spec.algorithm = PG_COMPRESSION_NONE;
		fp = cfopen(path, mode, compress_spec);
#ifdef HAVE_LIBZ
		if (fp == NULL)
		{
			char	   *fname;

			compress_spec.algorithm = PG_COMPRESSION_GZIP;
			fname = psprintf("%s.gz", path);
			fp = cfopen(fname, mode, compress_spec);
			free_keep_errno(fname);
		}
#endif
	}
	return fp;
}

/*
 * Open a file for writing. 'path' indicates the path name, and 'mode' must
 * be a filemode as accepted by fopen() and gzopen() that indicates writing
 * ("w", "wb", "a", or "ab").
 *
 * If 'compress_spec.algorithm' is GZIP, a gzip compressed stream is opened,
 * and 'compress_spec.level' used. The ".gz" suffix is automatically added to
 * 'path' in that case.
 *
 * On failure, return NULL with an error code in errno.
 */
cfp *
cfopen_write(const char *path, const char *mode,
			 const pg_compress_specification compress_spec)
{
	cfp		   *fp;

	if (compress_spec.algorithm == PG_COMPRESSION_NONE)
		fp = cfopen(path, mode, compress_spec);
	else
	{
#ifdef HAVE_LIBZ
		char	   *fname;

		fname = psprintf("%s.gz", path);
		fp = cfopen(fname, mode, compress_spec);
		free_keep_errno(fname);
#else
		pg_fatal("not built with zlib support");
		fp = NULL;				/* keep compiler quiet */
#endif
	}
	return fp;
}

/*
 * This is the workhorse for cfopen() or cfdopen(). It opens file 'path' or
 * associates a stream 'fd', if 'fd' is a valid descriptor, in 'mode'. The
 * descriptor is not dup'ed and it is the caller's responsibility to do so.
 * The caller must verify that the 'compress_algorithm' is supported by the
 * current build.
 *
 * On failure, return NULL with an error code in errno.
 */
static cfp *
cfopen_internal(const char *path, int fd, const char *mode,
				pg_compress_algorithm compress_algorithm, int compressionLevel)
{
	cfp		   *fp = pg_malloc(sizeof(cfp));

	fp->compress_algorithm = compress_algorithm;

	switch (compress_algorithm)
	{
		case PG_COMPRESSION_NONE:
			if (fd >= 0)
				fp->fp = fdopen(fd, mode);
			else
				fp->fp = fopen(path, mode);
			if (fp->fp == NULL)
			{
				free_keep_errno(fp);
				fp = NULL;
			}

			break;
		case PG_COMPRESSION_GZIP:
#ifdef HAVE_LIBZ
			if (compressionLevel != Z_DEFAULT_COMPRESSION)
			{
				/*
				 * user has specified a compression level, so tell zlib to use
				 * it
				 */
				char		mode_compression[32];

				snprintf(mode_compression, sizeof(mode_compression), "%s%d",
						 mode, compressionLevel);
				if (fd >= 0)
					fp->fp = gzdopen(fd, mode_compression);
				else
					fp->fp = gzopen(path, mode_compression);
			}
			else
			{
				/* don't specify a level, just use the zlib default */
				if (fd >= 0)
					fp->fp = gzdopen(fd, mode);
				else
					fp->fp = gzopen(path, mode);
			}

			if (fp->fp == NULL)
			{
				free_keep_errno(fp);
				fp = NULL;
			}
#else
			pg_fatal("not built with zlib support");
#endif
			break;
		default:
			pg_fatal("invalid compression method");
			break;
	}

	return fp;
}

cfp *
cfopen(const char *path, const char *mode,
	   const pg_compress_specification compress_spec)
{
	return cfopen_internal(path, -1, mode,
						   compress_spec.algorithm,
						   compress_spec.level);
}

cfp *
cfdopen(int fd, const char *mode,
		const pg_compress_specification compress_spec)
{
	return cfopen_internal(NULL, fd, mode,
						   compress_spec.algorithm,
						   compress_spec.level);
}

int
cfread(void *ptr, int size, cfp *fp)
{
	int			ret;

	if (size == 0)
		return 0;

	switch (fp->compress_algorithm)
	{
		case PG_COMPRESSION_NONE:
			ret = fread(ptr, 1, size, fp->fp);
			if (ret != size && !feof(fp->fp))
				READ_ERROR_EXIT(fp->fp);

			break;
		case PG_COMPRESSION_GZIP:
#ifdef HAVE_LIBZ
			ret = gzread(fp->fp, ptr, size);
			if (ret != size && !gzeof(fp->fp))
			{
				int			errnum;
				const char *errmsg = gzerror(fp->fp, &errnum);

				pg_fatal("could not read from input file: %s",
						 errnum == Z_ERRNO ? strerror(errno) : errmsg);
			}
#else
			pg_fatal("not built with zlib support");
#endif
			break;

		default:
			pg_fatal("invalid compression method");
			break;
	}

	return ret;
}

int
cfwrite(const void *ptr, int size, cfp *fp)
{
	int			ret = 0;

	switch (fp->compress_algorithm)
	{
		case PG_COMPRESSION_NONE:
			ret = fwrite(ptr, 1, size, fp->fp);
			break;
		case PG_COMPRESSION_GZIP:
#ifdef HAVE_LIBZ
			ret = gzwrite(fp->fp, ptr, size);
#else
			pg_fatal("not built with zlib support");
#endif
			break;
		default:
			pg_fatal("invalid compression method");
			break;
	}

	return ret;
}

int
cfgetc(cfp *fp)
{
	int			ret;

	switch (fp->compress_algorithm)
	{
		case PG_COMPRESSION_NONE:
			ret = fgetc(fp->fp);
			if (ret == EOF)
				READ_ERROR_EXIT(fp->fp);

			break;
		case PG_COMPRESSION_GZIP:
#ifdef HAVE_LIBZ
			ret = gzgetc((gzFile) fp->fp);
			if (ret == EOF)
			{
				if (!gzeof(fp->fp))
					pg_fatal("could not read from input file: %s", strerror(errno));
				else
					pg_fatal("could not read from input file: end of file");
			}
#else
			pg_fatal("not built with zlib support");
#endif
			break;
		default:
			pg_fatal("invalid compression method");
			break;
	}

	return ret;
}

char *
cfgets(cfp *fp, char *buf, int len)
{
	char	   *ret;

	switch (fp->compress_algorithm)
	{
		case PG_COMPRESSION_NONE:
			ret = fgets(buf, len, fp->fp);

			break;
		case PG_COMPRESSION_GZIP:
#ifdef HAVE_LIBZ
			ret = gzgets(fp->fp, buf, len);
#else
			pg_fatal("not built with zlib support");
#endif
			break;
		default:
			pg_fatal("invalid compression method");
			break;
	}

	return ret;
}

int
cfclose(cfp *fp)
{
	int			ret;

	if (fp == NULL)
	{
		errno = EBADF;
		return EOF;
	}

	switch (fp->compress_algorithm)
	{
		case PG_COMPRESSION_NONE:
			ret = fclose(fp->fp);
			fp->fp = NULL;

			break;
		case PG_COMPRESSION_GZIP:
#ifdef HAVE_LIBZ
			ret = gzclose(fp->fp);
			fp->fp = NULL;
#else
			pg_fatal("not built with zlib support");
#endif
			break;
		default:
			pg_fatal("invalid compression method");
			break;
	}

	free_keep_errno(fp);

	return ret;
}

int
cfeof(cfp *fp)
{
	int			ret;

	switch (fp->compress_algorithm)
	{
		case PG_COMPRESSION_NONE:
			ret = feof(fp->fp);

			break;
		case PG_COMPRESSION_GZIP:
#ifdef HAVE_LIBZ
			ret = gzeof(fp->fp);
#else
			pg_fatal("not built with zlib support");
#endif
			break;
		default:
			pg_fatal("invalid compression method");
			break;
	}

	return ret;
}

const char *
get_cfp_error(cfp *fp)
{
	if (fp->compress_algorithm == PG_COMPRESSION_GZIP)
	{
#ifdef HAVE_LIBZ
		int			errnum;
		const char *errmsg = gzerror(fp->fp, &errnum);

		if (errnum != Z_ERRNO)
			return errmsg;
#else
		pg_fatal("not built with zlib support");
#endif
	}

	return strerror(errno);
}

#ifdef HAVE_LIBZ
static int
hasSuffix(const char *filename, const char *suffix)
{
	int			filenamelen = strlen(filename);
	int			suffixlen = strlen(suffix);

	if (filenamelen < suffixlen)
		return 0;

	return memcmp(&filename[filenamelen - suffixlen],
				  suffix,
				  suffixlen) == 0;
}

#endif
