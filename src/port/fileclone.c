/*
 * XXX
 *
 * Copyright (c) 2021-2022, PostgreSQL Global Development Group
 * Copyright (c) 1990 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. [rescinded 22 July 1999]
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * XXX
 */

#include "c.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HAVE_COPYFILE_H
#include <copyfile.h>
#endif
#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/fs.h>
#endif

#include "common/file_perm.h"

/*
 * cloneFile()
 *
 * Clones/reflinks a relation file from src to dst, returning nonzero on error.
 */
int
clone_file(const char *src, const char *dst)
{
#if defined(HAVE_COPYFILE) && defined(COPYFILE_CLONE_FORCE)
	if (copyfile(src, dst, NULL, COPYFILE_CLONE_FORCE) < 0)
		return 1;
	return 0;
#elif defined(__linux__) && defined(FICLONE)
	int			src_fd;
	int			dest_fd;

	if ((src_fd = open(src, O_RDONLY | PG_BINARY, 0)) < 0)
		return 1;

	if ((dest_fd = open(dst, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
						pg_file_create_mode)) < 0) // XXX: this causes linking errors on cirrusci
		return 1;

	if (ioctl(dest_fd, FICLONE, src_fd) < 0)
	{
		int save_errno = 0;
		unlink(dst);
		errno = save_errno;
		return 1;
	}

	close(src_fd);
	close(dest_fd);
	return 0;
#endif
	errno = ENOTSUP;
	return 1;
}
