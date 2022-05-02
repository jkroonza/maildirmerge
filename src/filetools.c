#include "filetools.h"

#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/stat.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

static
void fdperror(int fd, const char* path, int err, const char* operation)
{
	char procname[1024], fdpath[1024];

	sprintf(procname, "/proc/%d/fd/%d", getpid(), fd);
	int r = readlink(procname, fdpath, sizeof(fdpath));
	if (r < 0) {
		fprintf(stderr, "readlink(%s): %s.\n", procname, strerror(errno));
		fprintf(stderr, "%s(fd=%d/%s): %s.\n", operation, fd, path, strerror(err));
	} else {
		if (r >= (int)sizeof(fdpath))
			fdpath[sizeof(fdpath) - 1] = 0;
		fprintf(stderr, "%s(%s/%s): %s.\n", operation, fdpath, path, strerror(err));
	}
}

static
int relstat_error(int fd, const char* path, struct stat *st)
{
	if (fstatat(fd, path, st, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH) == 0)
		return 0;

	fdperror(fd, path, errno, "fstatat");
	return -1;
}

int files_identical(int fd1, const char* path1, const struct stat* st1, int fd2, const char* path2, const struct stat* st2)
{
	struct stat _st1, _st2;
	int f1, f2;
	void *m1, *m2;

	if (!st1) {
		if (relstat_error(fd1, path1, &_st1) < 0)
			return -1;
		st1 = &_st1;
	}
	if (!st2) {
		if (relstat_error(fd2, path2, &_st2) < 0)
			return -1;
		st2 = &_st2;
	}

	if (st1->st_size != st2->st_size)
		return 0;

	/* if they are the same file, short out */
	if (st1->st_dev == st2->st_dev && st1->st_ino == st2->st_ino)
		return 1;

	f1 = openat(fd1, path1, O_RDONLY | AT_EMPTY_PATH);
	if (f1 < 0) {
		fdperror(fd1, path1, errno, "openat");
		return -1;
	}
	f2 = openat(fd2, path2, O_RDONLY | AT_EMPTY_PATH);
	if (f2 < 0) {
		fdperror(fd2, path2, errno, "openat");
		close(f1);
		return -1;
	}

	m1 = mmap(NULL, st1->st_size, PROT_READ, MAP_SHARED, f1, 0);
	if (!m1) {
		fdperror(fd1, path1, errno, "mmap");
		close(f1);
		close(f2);
		return -1;
	}
	m2 = mmap(NULL, st1->st_size, PROT_READ, MAP_SHARED, f1, 0);
	if (!m2) {
		fdperror(fd2, path2, errno, "mmap");
		munmap(m1, st1->st_size);
		close(f1);
		close(f2);
		return -1;
	}
	close(f1);
	close(f2);

	int r = memcmp(m1, m2, st1->st_size);
	munmap(m1, st1->st_size);
	munmap(m2, st2->st_size);

	return r == 0;
}
