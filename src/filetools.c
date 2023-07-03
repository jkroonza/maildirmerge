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
#include <stdbool.h>
#include <stdlib.h>
#include <ctype.h>

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

int is_maildir(int fd, const char* folder)
{
	const char* subs[] = { "new", "cur", "tmp", NULL };
	struct stat st;
	int i;

	for (i = 0; subs[i]; ++i) {
		if (fstatat(fd, subs[i], &st, 0) < 0) {
			fprintf(stderr, "%s/%s: %s\n", folder, subs[i], strerror(errno));
			return 0;
		}
		if ((st.st_mode & S_IFMT) != S_IFDIR) {
			fprintf(stderr, "%s/%s: is not a folder\n", folder, subs[i]);
			return 0;
		}
	}

	return 1;
}

int get_maildir_fd_at(int bfd, const char* folder)
{
	struct stat st;
	int fd;

	if (fstatat(bfd, folder, &st, 0) < 0) {
		perror(folder);
		return -1;
	}
	if ((st.st_mode & S_IFMT) != S_IFDIR) {
		fprintf(stderr, "%s: is not a folder\n", folder);
		return -1;
	}

	fd = openat(bfd, folder, O_RDONLY);
	if (fd < 0) {
		perror(folder);
		return -1;
	}

	if (!is_maildir(fd, folder)) {
		close(fd);
		fd = -1;
	}

	return fd;
}

int get_maildir_fd(const char* folder)
{
	return get_maildir_fd_at(AT_FDCWD, folder);
}

int maildir_create_sub(int bfd, const char* target, const char* foldername, bool dry_run)
{
	struct stat st;
	int fd = -1;

	if (fstat(bfd, &st) < 0) {
		perror(target);
		return -1;
	}

	if (dry_run) {
		/* this is outright nasty */
		printf("Would create maildir %s/%s (assuming it doesn't exist).\n",
				target, foldername);
		fd = get_maildir_fd_at(bfd, foldername);
		if (fd < 0) {
			/* this is outright wrong, but it works */
			fd = dup(bfd);
		}
		return fd;
	}

	if (mkdirat(bfd, foldername, st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) == 0) {
		fd = openat(bfd, foldername, O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "%s/%s: %s\n", target, foldername, strerror(errno));
			return -1;
		}

		mkdirat(fd, "new", st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO));
		mkdirat(fd, "cur", st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO));
		mkdirat(fd, "tmp", st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO));

		/* create an empty file to indicate sub-folder */
		close(openat(fd, "maildirfolder", O_CREAT | O_WRONLY, 0600));

		if (geteuid() == 0) {
			/* we are root */
			fchown(fd, st.st_uid, st.st_gid);
			fchownat(fd, "new", st.st_uid, st.st_gid, 0);
			fchownat(fd, "cur", st.st_uid, st.st_gid, 0);
			fchownat(fd, "tmp", st.st_uid, st.st_gid, 0);
			fchownat(fd, "maildirfolder", st.st_uid, st.st_gid, 0);
		}
		return fd;
	}

	if (errno == EEXIST)
		return get_maildir_fd_at(bfd, foldername);

	fprintf(stderr, "mkdir(%s/%s): %s\n", target, foldername, strerror(errno));
	return -1;
}

int message_seen(const char* filename)
{
	const char* p = strchr(filename, ':');
	const char* c;
	if (!p) {/* definitely no flags, so can't be seen */
		fprintf(stderr, "WARNING: No colon (info delimeter) found in %s.\n", filename);
		return 0;
	}
	c = strchr(++p, ',');
	if (!c) {
		fprintf(stderr, "WARNING: No comma found in info portion of %s, separating version from flags.\n", filename);
		return 0;
	}

	if ((c-p) != 1 || *p != '2') {
		fprintf(stderr, "WARNING: Unrecognized info version (%.*s) in %s, assuming not seen.\n",
				(int)(c-p), p, filename);
	}

	while (*++c) {
		if (*c == 'S')
			return 1;
		if (*c == ',') /* Dovecot extension, we can terminate here */
			return 0;
	}
	return 0;
}

void maildir_move(int sfd, const char* source, int tfd, const char* target, const char* sub, const char* fname, bool dry_run)
{
	if (dry_run) {
		printf("Rename: %s/%s/%s -> %s/%s/%s\n",
				source, sub, fname, target, sub, fname);
	} else {
		if (renameat(sfd, fname, tfd, fname) < 0)
			fprintf(stderr, "rename %s/%s/%s -> %s/%s/%s failed: %s\n",
				source, sub, fname, target, sub, fname, strerror(errno));
	}
}

static
void insert_mail_header(struct mail_header ** headp, char* header, char* value)
{
	struct mail_header *insp = (struct mail_header*)find_mail_header(*headp, header);  /* we can safely cast the const away here */
	if (!insp) {
		insp = malloc(sizeof(*insp));
		insp->header = header;
		insp->value = malloc(sizeof(*insp->value) * 2);
		insp->value[0] = value;
		insp->value[1] = NULL;
		insp->next = *headp;
		*headp = insp;
		return;
	}

	free(header);
	int i = 1;
	while (insp->value[i])
		++i;
	insp->value[i++] = value;
	if ((i & (i-1)) == 0)
		insp->value = realloc(insp->value, sizeof(*insp->value) * i * 2); /* if this fails we will segfault */
	insp->value[i] = NULL;
}

struct mail_header* get_mail_header(int sfd, const char* filename)
{
	size_t bfrsize = 8192, slen;
	char *bfr = NULL, *header = NULL, *value = NULL;
	int fd = openat(sfd, filename, O_RDONLY);
	struct mail_header *head = NULL;

	errno= 0;

	if (fd < 0)
		return NULL;
	FILE* fp = fdopen(fd, "r");
	if (!fp)
		goto errout;
	fd = -1; /* taken over by fp */

	bfr = malloc(bfrsize);
	if (!bfr)
		goto errout;

	while (fgets(bfr, bfrsize, fp)) {
		slen = strlen(bfr);
		while (bfr[slen-1] != '\n') {
			if (slen != bfrsize - 1) /* we have a NULL character in the input */
				goto errout;

			char *t = realloc(bfr, bfrsize *= 2);
			if (!t)
				goto errout;
			bfr = t;

			/* now we're looking to read from the file and append it to the existing slen string */
			if (!fgets(bfr + slen, bfrsize - slen, fp))
				goto errout;

			slen = strlen(bfr);
		}

		/* trim trailing \r\n characters */
		while (slen--) {
			if (bfr[slen] != '\n' && bfr[slen] != '\r')
				break;
			bfr[slen] = 0;
		}

		if (!*bfr)
			break; /* headers are done */

		if (!isspace(*bfr)) {

			char *v = strchr(bfr, ':');
			if (v) {
			/* new header */
				if (header)
					insert_mail_header(&head, header, value);

				*v++ = '\0';
				header = strdup(bfr);

				while (isspace(*v))
					++v;

				value = strdup(v);
			} else if (header) {
				// no : - this is outright wrong, assume incorrect/invalid mapping and append to existing buffer.
				char * t = realloc(value, strlen(value) + strlen(bfr) + 1);
				if (!t)
					break;
				value = t;
				strcat(value, bfr);
			}
		} else {
			/* appending to existing header */
			char * t = realloc(value, strlen(value) + strlen(bfr) + 1);
			if (!t)
				break;
			value = t;
			strcat(value, bfr);
		}
	}
errout:
	if (header)
		insert_mail_header(&head, header, value);

	if (fp)
		fclose(fp);
	if (fd >= 0)
		close(fd);
	if (bfr)
		free(bfr);

	return head;
}

const struct mail_header* find_mail_header(const struct mail_header* head, const char* header)
{
	while (head) {
		if (strcasecmp(head->header, header) == 0)
			break;
		head = head->next;
	}
	return head;
}

void free_mail_header(struct mail_header* head)
{
	while (head) {
		struct mail_header *next = head->next;
		free(head);
		head = next;
	}
}
