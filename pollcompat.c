/*

pollcompat -- poll API compatibility for darwin
Copyright (C) 2017,2018,2019 Kyle J. McKay.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

  * Neither the name of the copyright holder nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#undef _DARWIN_UNLIMITED_SELECT
#define _DARWIN_UNLIMITED_SELECT 1
#undef pollcompat

#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/select.h>
#include <unistd.h>

#include <poll.h>

#ifndef __DARWIN_ALIAS_C
#define __DARWIN_ALIAS_C(x)
#endif

int
pollcompat(struct pollfd fds[], nfds_t nfds, int timeout) __DARWIN_ALIAS_C(pollcompat);

static int reterr(int e)
{
	errno = e;
	return -1;
}

#ifndef POLLCOMPAT_ALWAYS
static int isapathfd(int fd)
{
#if defined(F_GETPATH) && defined(MAXPATHLEN)
	char scratchpath[MAXPATHLEN+1];
	return fcntl(fd, F_GETPATH, scratchpath) != -1;
#else
	return 0;
#endif
}

/* The isatty function call only returns true for tty devices, not for all
 * devices; roll our own that uses fstat to match any non fifo/socket/file fds.
 */
static int isadevice(int fd)
{
	struct stat info;
	mode_t kind;

	if (fstat(fd, &info))
		/* This includes an EOVERFLOW error which can only happen on
		 * a file and therefore such an fd is NOT a device! */
		return 0;

	kind = info.st_mode;
	/* Darwin's poll works on unnamed pipes (and gives us a helpful
	** POLLNVAL on EPIPE in that case), but does NOT work on a mkfifo
	** style pipe at all.  If we have the F_GETPATH fcntl then we can
	** tell the difference as F_GETPATH will succeed for a mkfifo but
	** fail for a regular pipe.
	*/
	return !(S_ISREG(kind) || S_ISSOCK(kind) ||
		 (S_ISFIFO(kind) && !isapathfd(fd)));
}
#endif

static int can_sys_poll(const struct pollfd fds[], nfds_t nfds)
{
#	ifdef POLLCOMPAT_ALWAYS
	(void)fds; (void)nfds;
	return 0;
#	else
	int maxfds = getdtablesize();
	nfds_t i;
	nfds_t valid = 0;
	nfds_t invalid = 0;

	for (i = 0; i < nfds; ++i) {
		if (fds[i].fd < 0)
			continue; /* ignored fd */
		if (fds[i].fd >= maxfds || fcntl(fds[i].fd, F_GETFD) == -1) {
			++invalid;
			continue; /* invalid fd */
		}
		if (isadevice(fds[i].fd))
			return 0; /* the system poll never works on devices */
		++valid;
	}
	/* The cURL project has a nice description of poll's various failings:
	** <https://daniel.haxx.se/blog/2016/10/11/poll-on-mac-10-12-is-broken/>
	** Basically passing in no fds rarely ever works and based on the man
	** page description, invalid fds are probably handled incorrectly as
	** well.  Therefore the system poll can only be used if one or more
	** valid fds are present but no invalid ones are present. */
	return valid > 0 && !invalid;
#	endif
}

static int get_max_fd(const struct pollfd fds[], nfds_t nfds)
{
	int maxfds = getdtablesize();
	nfds_t i;
	int max = -1;

	for (i=0; i < nfds; ++i) {
		if (fds[i].fd < 0 || fds[i].fd >= maxfds)
			continue; /* invalid fd */
		if (fds[i].fd > max)
			max = fds[i].fd;
	}
	return max;
}

static int get_fd_set(fd_set *aset, fd_set *oset[3], size_t *osize,
		      const struct pollfd fds[], nfds_t nfds)
{
	int maxfd = get_max_fd(fds, nfds);

	if (maxfd < FD_SETSIZE /* 1024 */) {
		FD_ZERO(aset);
		FD_ZERO(aset+1);
		FD_ZERO(aset+2);
		oset[0] = aset;
		oset[1] = aset+1;
		oset[2] = aset+2;
		*osize = sizeof(*aset);
		return 1; /* standard set up to 1024 fds */
	}
#	ifdef POLLCOMPAT_UNLIMITED
	else {
		/* calloc some memory and use it */
		size_t quads = ((unsigned)maxfd + 64) >> 6; /* how many 8-bytes */
		size_t setsize = quads << 3;
		char *sets = (char *)calloc(3 * quads, 8);

		if (sets == NULL) return 0; /* out of memory */
		*osize = setsize;
		oset[0] = (fd_set *)sets;
		oset[1] = (fd_set *)(sets + setsize);
		oset[2] = (fd_set *)(sets + (setsize + setsize));
		return 1; /* using unlimited mode */
	}
#	else
	return 0; /* not using unlimited mode */
#	endif
}

static void free_fd_set(fd_set *aset, fd_set *oset[3])
{
#ifdef POLLCOMPAT_UNLIMITED
	if (aset && oset[0] && oset[0] != aset)
		free(oset[0]);
#else
	/* nothing to do, non-unlimited mode never allocs set memory */
	(void)aset; (void)oset;
#endif
}

static int dopoll(struct pollfd fds[], nfds_t nfds, fd_set *set[3], int timeout);

int
pollcompat(struct pollfd fds[], nfds_t nfds, int timeout)
{
	fd_set sets[3], *setp[3];
	size_t setsize;
	int result;

#ifdef OPEN_MAX
	if (nfds > OPEN_MAX) return reterr(EINVAL);
#endif
	if (timeout < -1) return reterr(EINVAL);
	if (nfds > 0 && fds == NULL) return reterr(EINVAL);
	if (can_sys_poll(fds, nfds))
		return poll(fds, nfds, timeout);
	if (!get_fd_set(sets, setp, &setsize, fds, nfds)) return reterr(E2BIG);
	result = dopoll(fds, nfds, setp, timeout);
	free_fd_set(sets, setp);
	return result;
}

static int dopoll(struct pollfd fds[], nfds_t nfds, fd_set *set[3], int timeout)
{
	int maxfds = getdtablesize();
	nfds_t i;
	int extra = 0, fdcnt = 0;
	struct timeval tv, *tvp = NULL;
	int result;

	if (timeout >= 0) {
		if (timeout) {
			ldiv_t ld = ldiv(timeout, 1000);

			tv.tv_sec = ld.quot;
			tv.tv_usec = ld.rem * 1000;
		} else {
			tv.tv_sec = 0;
			tv.tv_usec = 0;
		}
		tvp = &tv;
	}
	for (i = 0; i < nfds; ++i) {
		fds[i].revents = 0;
		if (fds[i].fd < 0)
			continue; /* ignore < 0 fds */
		if (fds[i].fd >= maxfds || fcntl(fds[i].fd, F_GETFD) == -1) {
			fds[i].revents = POLLNVAL;
			++extra;
			continue; /* invalid fd */
		}
		/* we should always request error state as POLLERR must always
		** be returned, BUT the errorfds bits are completely unreliable
		** and may and up being set for no "errorful" reason, so never
		** ever set the bit in set[2] except when at least one of the
		** other bits is also being set. */
		if (fds[i].fd >= fdcnt)
			fdcnt = fds[i].fd + 1;
		if (fds[i].events & (POLLRDNORM|POLLIN)) {
			FD_SET(fds[i].fd, set[0]);
			FD_SET(fds[i].fd, set[2]);
		}
		if (fds[i].events & (POLLWRNORM|POLLOUT)) {
			FD_SET(fds[i].fd, set[1]);
			FD_SET(fds[i].fd, set[2]);
		}
	}
	result = select(fdcnt, set[0], set[1], set[2], tvp);
	if (result < 0)
		return result;
	if (result > 0) for (i = 0; i < nfds; ++i) {
		int fd = fds[i].fd;
		if (fd < 0 || fds[i].revents)
			continue; /* ignore < 0 or already processed */
		if (fcntl(fd, F_GETFD) == -1)
			fds[i].revents |= POLLHUP;  /* kludge */
		else {
			if (FD_ISSET(fd, set[2])) {
				/* kludge alert
				** if a descriptor is ready for reading AND has
				** an error then we do NOT set POLLERR nor do
				** we set POLLHUP!  Both /dev/null and /dev/zero
				** devices can cause this, but only one of them
				** is in a "POLLHUP" state, but in either case
				** the "read" call will return without blocking
				** and it will be discovered which at that time.
				** Similarly if a descriptor is ready for
				** writing and has an error we do NOT set the
				** POLLERR bit either as a subsequent "write"
				** call will complete without blocking and
				** return the error if there really is one.
				*/
				if (!(FD_ISSET(fd, set[0]) || FD_ISSET(fd, set[1])))
					fds[i].revents |= POLLERR;
			}
			if (FD_ISSET(fd, set[1]))
				fds[i].revents |= POLLWRNORM|POLLOUT;
			if (FD_ISSET(fd, set[0])) {
				int avail = 1;

				fds[i].revents |= POLLRDNORM|POLLIN;
				/* kludge alert */
				if (ioctl(fd, FIONREAD, &avail) != -1 && !avail)
					fds[i].revents |= POLLHUP;
			}
		}
		if (fds[i].revents)
			++extra;
	}
	return extra;
}
