#define _GNU_SOURCE

#include <stdio.h>
#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#define MAX_STAT_ENOENT_RETRY		10

static const char * progname;
static const char * maildir_subs[] = { "cur", "new", "-tmp", NULL };
/* tmp/ needs not be scanned.
 * ordering critical as stuff gets moved from new/ to cur/,
 * and thus we need to get filenames from cur/ first else we
 * can see a filename first in new/ and then in cur/ triggering an error
 **/

struct msg_list {
	const char* basename; /* unique */
	const char **fullnames; /* NULL terminated */
	struct msg_list *next;
};

#define free_msg_list(x) do { while ((x)) { msg_list*t = (x)->next; free((x)->basename); const char** t2; for (t2 = (x)->fullnames; t2; ++t2) { free(t2); } free(x); }} while(0)

/* This is designed to "accomodate" a glusterfs bug w.r.t. linkto files that
 * doesn't properly heal. Since we are VERY certain that we got a readdir()
 * entry prior to this, we know that we should NEVER get ENOENT, so keep retrying
 * a few times until we don't.  Output a warning to STDERR indicating number of rounds.
 **/
static
int myfstatat(int dirfd, const char *restrict pathname, struct stat *restrict statbuf, int flags)
{
	int r;
	int retries = MAX_STAT_ENOENT_RETRY;
	do {
		r = fstatat(dirfd, pathname, statbuf, AT_SYMLINK_NOFOLLOW | flags);
	} while (r < 0 && errno == ENOENT && --retries);
	if (retries < MAX_STAT_ENOENT_RETRY) {
		fprintf(stderr, "\nWe had %d ENOENT failures for stat(%s).\n",
				MAX_STAT_ENOENT_RETRY - retries, pathname);
	}
	return r;
}

#define add_error(ec, fmt, ...) do { printf("\n" fmt, ## __VA_ARGS__); fflush(stdout); ++(ec); } while(0)

#define check_ownership(fd, path, ec, fmt, ...) do { \
	struct stat st; \
	if (myfstatat(fd, path, &st, AT_EMPTY_PATH) < 0) { \
		add_error(ec, "fstatat(" fmt "): %s - cannot check ownership", ## __VA_ARGS__, strerror(errno)); \
	} else { \
		if (st.st_uid != uid) \
			add_error(ec, fmt ": Wrong ownership, uid=%lu is not %lu.", ## __VA_ARGS__, (unsigned long)st.st_uid, (unsigned long)uid); \
		if (st.st_gid != gid) \
			add_error(ec, fmt ": Wrong group, gid=%lu is not %lu.", ## __VA_ARGS__, (unsigned long)st.st_gid, (unsigned long)gid); \
	} \
} while(0)

static
int check_fdpath(int fd, const char* rpath, uid_t uid, gid_t gid)
{
	const char ** sp = maildir_subs;
	const char * subname;
	int ec = 0, sfd;
	printf("INBOX%s:", rpath); fflush(stdout);
	int noscan;
	DIR *dir;
	struct dirent *de;

	check_ownership(fd, "", ec, ".");
	for (sp = maildir_subs; *sp; ++sp) {
		subname = *sp;
		noscan = *subname == '-';
		if (noscan)
			++subname;
		sfd = openat(fd, subname, O_RDONLY);
		if (sfd < 0) {
			add_error(ec, "%s: %s.", subname, strerror(errno));
			continue;
		}

		check_ownership(sfd, "", ec, "%s", subname);

		if (noscan) {
			close(sfd);
			continue;
		}

		dir = fdopendir(sfd);
		if (!dir) {
			add_error(ec, "%s: %s", subname, strerror(errno));
			close(sfd);
		} else {
			while ((de = readdir(dir))) {
				check_ownership(sfd, de->d_name, ec, "%s/%s", subname, de->d_name);
			}
			closedir(dir);
		}
	}

	if (ec) {
		printf("\n *** %d errors identified ***\n", ec);
	} else {
		printf(" All Good.\n");
	}
	return ec;
}

static
int check_path(const char* path)
{
	int ec, sfd;
	int fd = open(path, O_RDONLY);
	DIR* dir;
	struct dirent* de;
	struct stat st;
	uid_t uid;
	gid_t gid;

	if (fd < 0) {
		perror(path);
		return 1;
	}
	printf("PATH: %s\n", path);

	ec = myfstatat(fd, "", &st, AT_EMPTY_PATH);
	if (ec < 0) {
		printf("Error stat'ing base folder: %s.\n", strerror(errno));
		return 1;
	}

	uid = st.st_uid;
	gid = st.st_gid;

	ec = check_fdpath(fd, "", uid, gid);

	dir = fdopendir(fd);
	if (!dir) {
		printf("%s: %s\n", path, strerror(errno));
		close(fd);
		++ec;
	} else {
		while ((de = readdir(dir))) {
			if (de->d_name[0] != '.' || strcmp(de->d_name, "..") == 0 || strcmp(de->d_name, ".") == 0)
				continue;

			if (de->d_type == DT_UNKNOWN) {
				if (myfstatat(fd, de->d_name, &st, 0) < 0) {
					printf("%s: %s.\n", de->d_name, strerror(errno));
					++ec;
					continue;
				}
				if (S_ISDIR(st.st_mode))
					de->d_type = DT_DIR;
			}

			if (de->d_type != DT_DIR) {
				printf("%s: Not a folder (.Name entries must be folders).\n", de->d_name);
				++ec;
				continue;
			}

			sfd = openat(fd, de->d_name, O_RDONLY);
			if (sfd < 0) {
				printf("%s: %s.\n", de->d_name, strerror(errno));
				continue;
			}
			ec += check_fdpath(sfd, de->d_name, uid, gid);
			close(sfd);
		}
		closedir(dir);
	}

	return ec;
}

static
void __attribute__((noreturn)) usage(int x)
{
	FILE *o = x ? stderr : stdout;

	fprintf(o, "USAGE: %s [options] folder [...]\n", progname);
	fprintf(o, "  -h|--help\n");
	fprintf(o, "    Display this text and terminate.\n");
	fprintf(o, "Progam will exit with 0 exit code if, and only if none of the folders exhibit any errors:\n");
	fprintf(o, "  0 - no errors.\n");
	fprintf(o, "  1 - usage error (ie, we terminated due to a usage problem).\n");
	fprintf(o, "  2 - errors were encountered, see output from program for details.\n");
	fprintf(o, "WARNING:  This doesn't currently check anything server (courier/dovecot etc ...) specific.\n");
	fprintf(o, "SERIOUS WARNING: This uses stat ... a lot ... VERY slow on certain filesystems.\n");
	exit(x);
};

static struct option options[] = {
	{ "help",		no_argument, NULL, 'h' },
	{ NULL, 0, NULL, 0 }
};

int main(int argc, char **argv)
{
	progname = *argv;
	int c;

	while (( c = getopt_long(argc, argv, "h", options, NULL)) != -1) {
		switch (c) {
		case 0:
			break;
		case 'h':
			usage(0);
		default:
			fprintf(stderr, "Option not implemented: %c.\n", c);
			usage(1);
		}
	}

	if (!argv[optind])
		usage(1);

	c = 0;
	while (argv[optind])
		c += check_path(argv[optind++]);
	return c ? 2 : 0;
}
