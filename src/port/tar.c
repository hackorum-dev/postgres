#include "c.h"

#include <sys/stat.h>

#include "pgtar.h"

static bool tarSplitPathname(const char *pathname, char *name, char *prefix);
static bool tarCanEncodePath(const char *pathname);
static enum tarError tarCreateHeaderInternal(char *h,
											 const char *filename,
											 const char *linktarget,
											 pgoff_t size,
											 mode_t mode,
											 uid_t uid,
											 gid_t gid,
											 time_t mtime,
											 bool allow_prefix,
											 bool truncate);
static size_t tarPaxRecordSize(const char *key, const char *value);
static size_t tarPaxWriteRecord(char *p, const char *key, const char *value);
static void tarCreatePaxHeaderBlock(char *h, size_t payload_size, time_t mtime);

/*
 * Print a numeric field in a tar header.  The field starts at *s and is of
 * length len; val is the value to be written.
 *
 * Per POSIX, the way to write a number is in octal with leading zeroes and
 * one trailing space (or NUL, but we use space) at the end of the specified
 * field width.
 *
 * However, the given value may not fit in the available space in octal form.
 * If that's true, we use the GNU extension of writing \200 followed by the
 * number in base-256 form (ie, stored in binary MSB-first).  (Note: here we
 * support only non-negative numbers, so we don't worry about the GNU rules
 * for handling negative numbers.)
 */
void
print_tar_number(char *s, int len, uint64 val)
{
	if (val < (((uint64) 1) << ((len - 1) * 3)))
	{
		/* Use octal with trailing space */
		s[--len] = ' ';
		while (len)
		{
			s[--len] = (val & 7) + '0';
			val >>= 3;
		}
	}
	else
	{
		/* Use base-256 with leading \200 */
		s[0] = '\200';
		while (len > 1)
		{
			s[--len] = (val & 255);
			val >>= 8;
		}
	}
}


/*
 * Read a numeric field in a tar header.  The field starts at *s and is of
 * length len.
 *
 * The POSIX-approved format for a number is octal, ending with a space or
 * NUL.  However, for values that don't fit, we recognize the GNU extension
 * of \200 followed by the number in base-256 form (ie, stored in binary
 * MSB-first).  (Note: here we support only non-negative numbers, so we don't
 * worry about the GNU rules for handling negative numbers.)
 */
uint64
read_tar_number(const char *s, int len)
{
	uint64		result = 0;

	if (*s == '\200')
	{
		/* base-256 */
		while (--len)
		{
			result <<= 8;
			result |= (unsigned char) (*++s);
		}
	}
	else
	{
		/* octal */
		while (len-- && *s >= '0' && *s <= '7')
		{
			result <<= 3;
			result |= (*s - '0');
			s++;
		}
	}
	return result;
}


/*
 * Calculate the tar checksum for a header. The header is assumed to always
 * be 512 bytes, per the tar standard.
 */
int
tarChecksum(const char *header)
{
	int			i,
				sum;

	/*
	 * Per POSIX, the checksum is the simple sum of all bytes in the header,
	 * treating the bytes as unsigned, and treating the checksum field (at
	 * offset TAR_OFFSET_CHECKSUM) as though it contained 8 spaces.
	 */
	sum = 8 * ' ';				/* presumed value for checksum field */
	for (i = 0; i < TAR_BLOCK_SIZE; i++)
		if (i < TAR_OFFSET_CHECKSUM || i >= TAR_OFFSET_CHECKSUM + 8)
			sum += 0xFF & header[i];
	return sum;
}

/*
 * Check validity of a tar header (assumed to be 512 bytes long).
 * We verify the checksum and the magic number / version.
 */
bool
isValidTarHeader(const char *header)
{
	int			sum;
	int			chk = tarChecksum(header);

	sum = read_tar_number(&header[TAR_OFFSET_CHECKSUM], 8);

	if (sum != chk)
		return false;

	/* POSIX tar format */
	if (memcmp(&header[TAR_OFFSET_MAGIC], "ustar\0", 6) == 0 &&
		memcmp(&header[TAR_OFFSET_VERSION], "00", 2) == 0)
		return true;
	/* GNU tar format */
	if (memcmp(&header[TAR_OFFSET_MAGIC], "ustar  \0", 8) == 0)
		return true;
	/* not-quite-POSIX format written by pre-9.3 pg_dump */
	if (memcmp(&header[TAR_OFFSET_MAGIC], "ustar00\0", 8) == 0)
		return true;

	return false;
}


/*
 * Fill in the buffer pointed to by h with a tar format header. This buffer
 * must always have space for 512 characters, which is a requirement of
 * the tar format.
 *
 * This function retains the traditional PostgreSQL behavior of rejecting names
 * longer than the name field.  Use tarCreateHeaderExt() if support for the
 * ustar prefix field and PAX extended headers is desired.
 */
enum tarError
tarCreateHeader(char *h, const char *filename, const char *linktarget,
				pgoff_t size, mode_t mode, uid_t uid, gid_t gid, time_t mtime)
{
	return tarCreateHeaderInternal(h, filename, linktarget, size, mode,
								   uid, gid, mtime, false, false);
}

/*
 * Build the on-disk representation of a tar member at h, which must have room
 * for tarCreateHeaderExtSize(filename, linktarget) bytes.  If a PAX extended
 * header is required, it is written before the traditional header, and the
 * traditional header uses a truncated placeholder for the long field(s);
 * consumers must give precedence to the PAX attributes.
 *
 * On success, *bytes_written is set to the total number of bytes emitted.
 */
enum tarError
tarCreateHeaderExt(char *h, size_t hsize, const char *filename,
				   const char *linktarget, pgoff_t size, mode_t mode,
				   uid_t uid, gid_t gid, time_t mtime, size_t *bytes_written)
{
	bool	need_path;
	bool	need_link;
	size_t	paxsize = 0;
	size_t	padlen;
	size_t	total;
	char   *p;
	enum tarError	rc;

	*bytes_written = 0;

	if (strlen(filename) >= MAXPGPATH)
		return TAR_NAME_TOO_LONG;
	if (linktarget != NULL && strlen(linktarget) >= MAXPGPATH)
		return TAR_SYMLINK_TOO_LONG;

	need_path = !tarCanEncodePath(filename);
	need_link = (linktarget != NULL &&
				 strlen(linktarget) > TAR_LINKNAME_FIELD_SIZE - 1);

	if (!need_path && !need_link)
	{
		rc = tarCreateHeaderInternal(h, filename, linktarget, size, mode,
									 uid, gid, mtime, true, false);
		if (rc == TAR_OK)
			*bytes_written = TAR_BLOCK_SIZE;
		return rc;
	}

	if (need_path)
		paxsize += tarPaxRecordSize("path", filename);
	if (need_link)
		paxsize += tarPaxRecordSize("linkpath", linktarget);

	/*
	 * See tarCreateHeaderExtSize() for the layout; 'total' must match the
	 * value returned by that function.
	 */
	padlen = tarPaddingBytesRequired(paxsize);
	total = TAR_BLOCK_SIZE + paxsize + padlen + TAR_BLOCK_SIZE;

	if (hsize < total)
		return TAR_BUFFER_TOO_SMALL;

	p = h;

	/* PAX extended header member. */
	tarCreatePaxHeaderBlock(p, paxsize, mtime);
	p += TAR_BLOCK_SIZE;

	/* PAX payload */
	if (need_path)
		p += tarPaxWriteRecord(p, "path", filename);
	if (need_link)
		p += tarPaxWriteRecord(p, "linkpath", linktarget);

	/* Padding to 512-byte boundary. */
	memset(p, 0, padlen);
	p += padlen;

	Assert(p == h + TAR_BLOCK_SIZE + paxsize + padlen);

	/* The actual member header, with placeholders for the PAX fields. */
	rc = tarCreateHeaderInternal(p, filename, linktarget, size, mode,
								 uid, gid, mtime, true, true);
	if (rc == TAR_OK)
		*bytes_written = total;

	return rc;
}

/*
 * Compute the number of bytes needed to represent a tar member, including a
 * PAX extended header if the file name or symbolic link target cannot be
 * represented using the traditional ustar fields.  The result covers the PAX
 * header block, the PAX payload and its padding, and the traditional member
 * header block, but not the member contents or their padding.
 *
 * Returns 0 if the name or link target is longer than MAXPGPATH, in which case
 * no archive representation is possible.
 */
size_t
tarCreateHeaderExtSize(const char *filename, const char *linktarget)
{
	bool	need_path;
	bool	need_link;
	size_t	paxsize = 0;
	size_t	padlen;

	/*
	 * The PAX attributes are stored in MAXPGPATH-sized buffers by the
	 * consumers, so refuse anything that could not be represented there.
	 */
	if (strlen(filename) >= MAXPGPATH)
		return 0;
	if (linktarget != NULL && strlen(linktarget) >= MAXPGPATH)
		return 0;

	need_path = !tarCanEncodePath(filename);
	need_link = (linktarget != NULL &&
				 strlen(linktarget) > TAR_LINKNAME_FIELD_SIZE - 1);

	if (!need_path && !need_link)
		return TAR_BLOCK_SIZE;

	if (need_path)
		paxsize += tarPaxRecordSize("path", filename);
	if (need_link)
		paxsize += tarPaxRecordSize("linkpath", linktarget);

	/*
	 * A member that needs PAX is laid out as:
	 *
	 * 	[ PAX extended header block ]	TAR_BLOCK_SIZE, typeflag 'x'
	 * 	[ PAX payload               ]	paxsize bytes
	 * 	[ padding to 512 boundary   ]	tarPaddingBytesRequired(paxsize)
	 * 	[ traditional header block  ]	TAR_BLOCK_SIZE, typeflag '2' etc
	 *
	 * The second block is not redundant: POSIX requires a conventional ustar
	 * header to follow a PAX extended header, and the parser applies the PAX
	 * attributes to that header's member.  The long path or link target lives
	 * in the PAX payload; the traditional header carries only a placeholder.
	 */
	padlen = tarPaddingBytesRequired(paxsize);
	return TAR_BLOCK_SIZE + paxsize + padlen + TAR_BLOCK_SIZE;
}

/*
 * Try to split a pathname into the ustar prefix and name fields.
 *
 * Return true if the pathname can be represented that way, false otherwise.
 * On success, name and prefix are NUL-terminated.
 */
static bool
tarSplitPathname(const char *pathname, char *name, char *prefix)
{
	size_t	len = strlen(pathname);
	size_t	lower;
	size_t	upper;

	name[0] = prefix[0] = '\0';

	/* A short pathname fits in the name field by itself. */
	if (len < TAR_NAME_FIELD_SIZE)
	{
		memcpy(name, pathname, len + 1);
		return true;
	}

	/*
	 * Otherwise, we need a slash that separates a prefix of at most 154
	 * characters from a name of at most 99 characters.
	 */
	if (len > (TAR_PREFIX_FIELD_SIZE - 1) + 1 + (TAR_NAME_FIELD_SIZE - 1))
		return false;

	/*
	 * Search backwards for a slash that splits the pathname into a prefix that
	 * fits in the prefix field and a name that fits in the name field.
	 */
	lower = Max((size_t) 1, len - TAR_NAME_FIELD_SIZE);
	upper = Min(len - 1, (size_t) TAR_PREFIX_FIELD_SIZE - 1);
	for (size_t i = upper + 1; i > lower; /* void */)
	{
		size_t	prefixlen;
		size_t	namelen;

		i--;
		if (pathname[i] != '/')
			continue;

		prefixlen = i;
		namelen = len - i - 1;
		if (prefixlen == 0 || namelen == 0)
			continue;

		memcpy(prefix, pathname, prefixlen);
		prefix[prefixlen] = '\0';
		memcpy(name, pathname + i + 1, namelen);
		name[namelen] = '\0';
		return true;
	}

	return false;
}

/*
 * Can this pathname be represented using the traditional ustar name and
 * prefix fields?
 */
static bool
tarCanEncodePath(const char *pathname)
{
	char	name[TAR_NAME_FIELD_SIZE + 1];
	char	prefix[TAR_PREFIX_FIELD_SIZE + 1];

	return tarSplitPathname(pathname, name, prefix);
}

static enum tarError
tarCreateHeaderInternal(char *h, const char *filename, const char *linktarget,
						pgoff_t size, mode_t mode, uid_t uid, gid_t gid,
						time_t mtime, bool allow_prefix, bool truncate)
{
	char	name[TAR_NAME_FIELD_SIZE + 1];
	char	prefix[TAR_PREFIX_FIELD_SIZE + 1];

	name[0] = prefix[0] = '\0';

	if (allow_prefix)
	{
		if (!tarSplitPathname(filename, name, prefix))
		{
			const char *base;

			if (!truncate)
				return TAR_NAME_TOO_LONG;

			/*
			 * The traditional header must still contain something reasonable,
			 * since consumers that do not understand PAX may look at it.  Use
			 * the basename when it fits, otherwise a truncated form of the
			 * path.
			 */
			base = last_dir_separator(filename);
			if (base != NULL && base[1] != '\0')
				base++;
			else
				base = filename;

			strlcpy(name, base, TAR_NAME_FIELD_SIZE);
		}
	}
	else
	{
		if (strlen(filename) > TAR_NAME_FIELD_SIZE - 1)
			return TAR_NAME_TOO_LONG;
		strlcpy(name, filename, TAR_NAME_FIELD_SIZE);
	}

	if (name[0] == '\0')
		return TAR_NAME_TOO_LONG;

	if (linktarget != NULL &&
		strlen(linktarget) > TAR_LINKNAME_FIELD_SIZE - 1 &&
		!truncate)
		return TAR_SYMLINK_TOO_LONG;

	memset(h, 0, TAR_BLOCK_SIZE);

	/* Name 100 */
	strlcpy(&h[TAR_OFFSET_NAME], name, TAR_NAME_FIELD_SIZE);
	if (linktarget != NULL || S_ISDIR(mode))
	{
		/*
		 * We only support symbolic links to directories, and this is indicated
		 * in the tar format by adding a slash at the end of the name, the same
		 * as for regular directories.
		 */
		int	flen = strlen(&h[TAR_OFFSET_NAME]);

		if (flen < TAR_NAME_FIELD_SIZE - 1)
		{
			h[TAR_OFFSET_NAME + flen] = '/';
			h[TAR_OFFSET_NAME + flen + 1] = '\0';
		}
		else
			h[TAR_OFFSET_NAME + flen - 1] = '/';
	}

	/* Mode 8 - this doesn't include the file type bits (S_IFMT) */
	print_tar_number(&h[TAR_OFFSET_MODE], 8, (mode & 07777));

	/* User ID 8 */
	print_tar_number(&h[TAR_OFFSET_UID], 8, uid);

	/* Group ID 8 */
	print_tar_number(&h[TAR_OFFSET_GID], 8, gid);

	/* File size 12 */
	if (linktarget != NULL || S_ISDIR(mode))
		/* Symbolic link or directory has size zero. */
		print_tar_number(&h[TAR_OFFSET_SIZE], 12, 0);
	else
		print_tar_number(&h[TAR_OFFSET_SIZE], 12, size);

	/* Mode Time 12 */
	print_tar_number(&h[TAR_OFFSET_MTIME], 12, mtime);

	/* Checksum 8 cannot be calculated until we've filled all other fields. */

	if (linktarget != NULL)
	{
		/* Type - Symbolic link */
		h[TAR_OFFSET_TYPEFLAG] = TAR_FILETYPE_SYMLINK;
		/* Link Name 100 */
		strlcpy(&h[TAR_OFFSET_LINKNAME], linktarget, TAR_LINKNAME_FIELD_SIZE);
	}
	else if (S_ISDIR(mode))
	{
		/* Type - directory */
		h[TAR_OFFSET_TYPEFLAG] = TAR_FILETYPE_DIRECTORY;
	}
	else
	{
		/* Type - regular file */
		h[TAR_OFFSET_TYPEFLAG] = TAR_FILETYPE_PLAIN;
	}

	/* Magic 6 */
	strcpy(&h[TAR_OFFSET_MAGIC], "ustar");

	/* Version 2 */
	memcpy(&h[TAR_OFFSET_VERSION], "00", 2);

	/* User 32 */
	/* XXX: Do we need to care about setting correct username? */
	strlcpy(&h[TAR_OFFSET_UNAME], "postgres", 32);

	/* Group 32 */
	/* XXX: Do we need to care about setting correct group name? */
	strlcpy(&h[TAR_OFFSET_GNAME], "postgres", 32);

	/* Major Dev 8 */
	print_tar_number(&h[TAR_OFFSET_DEVMAJOR], 8, 0);

	/* Minor Dev 8 */
	print_tar_number(&h[TAR_OFFSET_DEVMINOR], 8, 0);

	/* Prefix 155 */
	if (prefix[0] != '\0')
		strlcpy(&h[TAR_OFFSET_PREFIX], prefix, TAR_PREFIX_FIELD_SIZE);

	/* Finally, compute and insert the checksum */
	print_tar_number(&h[TAR_OFFSET_CHECKSUM], 8, tarChecksum(h));

	return TAR_OK;
}

/*
 * Number of bytes in a PAX extended header record of the form
 *
 *		<length> <key>=<value>\n
 *
 * The length includes its own decimal digits, so we must iterate until the
 * length and its digit count are consistent.  This converges in at most two
 * iterations.
 */
static size_t
tarPaxRecordSize(const char *key, const char *value)
{
	size_t	body = strlen(key) + strlen(value) + 3;
	size_t	len = body + 1;

	for (;;)
	{
		size_t	digits = 1;
		size_t	tmp = len;

		while (tmp >= 10)
		{
			tmp /= 10;
			digits++;
		}
		if (len == body + digits)
			return len;
		len = body + digits;
	}
}

/*
 * Write on PAX record.  The buffer must have room for tarPaxRecordSize(), plus
 * one byte for the terminating NUL that snprintf() writes.
 */
static size_t
tarPaxWriteRecord(char *p, const char *key, const char *value)
{
	size_t	len = tarPaxRecordSize(key, value);
	int		n;

	n = snprintf(p, len + 1, "%zu %s=%s\n", len, key, value);
	Assert(n == (int) len);

	return len;
}

/*
 * Build the header for the PAX extended header member itself.  The payload
 * follows immediately after this block.
 */
static void
tarCreatePaxHeaderBlock(char *h, size_t payload_size, time_t mtime)
{
	memset(h, 0, TAR_BLOCK_SIZE);

	/* Name 100 */
	strlcpy(&h[TAR_OFFSET_NAME], TAR_PAX_HEADER_NAME, TAR_NAME_FIELD_SIZE);

	/* Mode 8 */
	print_tar_number(&h[TAR_OFFSET_MODE], 8, 0600);

	/* User ID 8 */
	print_tar_number(&h[TAR_OFFSET_UID], 8, 0);

	/* Group ID 8 */
	print_tar_number(&h[TAR_OFFSET_GID], 8, 0);

	/* File size 12: the unpadded PAX payload size. */
	print_tar_number(&h[TAR_OFFSET_SIZE], 12, payload_size);

	/* Mod Time 12 */
	print_tar_number(&h[TAR_OFFSET_MTIME], 12, mtime);

	/* Type - PAX extended header */
	h[TAR_OFFSET_TYPEFLAG] = TAR_FILETYPE_PAX_EXTENDED;

	/* Magic 6 */
	strcpy(&h[TAR_OFFSET_MAGIC], "ustar");

	/* Version 2 */
	memcpy(&h[TAR_OFFSET_VERSION], "00", 2);

	/* User 32 */
	strlcpy(&h[TAR_OFFSET_UNAME], "postgres", 32);

	/* Group 32 */
	strlcpy(&h[TAR_OFFSET_GNAME], "postgres", 32);

	/* Major Dev 8 */
	print_tar_number(&h[TAR_OFFSET_DEVMAJOR], 8, 0);

	/* Minor Dev 8 */
	print_tar_number(&h[TAR_OFFSET_DEVMINOR], 8, 0);

	/* Finally, compute and insert the checksum */
	print_tar_number(&h[TAR_OFFSET_CHECKSUM], 8, tarChecksum(h));
}
