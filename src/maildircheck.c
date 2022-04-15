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
static const char * valid_flags = "PRSTDF";

/* tmp/ needs not be scanned.
 * ordering critical as stuff gets rename()d from new/ to cur/,
 * and thus we need to get filenames from cur/ first else we
 * can see a filename first in new/ and then in cur/ triggering an error
 * - indicates we don't need to scan.
 * + indicates that we must have :2, and optional flags.
 **/

struct msg_list {
	char* basename; /* unique */
	char **fullnames; /* NULL terminated */
	struct msg_list *next;
};

static
void msg_list_free(struct msg_list *m)
{
	struct msg_list *t;
	char **ff;
	while (m) {
		t = m->next;

		free(m->basename);
		for (ff = m->fullnames; *ff; ++ff)
			free(*ff);
		free(m->fullnames);
		free(m);

		m = t;
	}
}

static
void msg_list_add(struct msg_list **mlist, const char* sub, const char* fn)
{
	const char* base = fn;
	const char* colon = strchr(fn, ':');
	if (colon)
		base = strndupa(fn, colon - fn);

	/* insert in ASCII order, this is probably the worst choice */
	while (*mlist && strcmp((*mlist)->basename, base) < 0)
		mlist = &(*mlist)->next;

	if (*mlist && strcmp((*mlist)->basename, base) == 0) {
		/* error condition, base is not unique, just add to the fullnames. */
		int i = 0;
		while ((*mlist)->fullnames[i++])
			;
		(*mlist)->fullnames = realloc((*mlist)->fullnames, sizeof(*(*mlist)->fullnames) * (i+1));
		asprintf(&(*mlist)->fullnames[i-1], "%s/%s", sub, fn);
		(*mlist)->fullnames[i] = NULL;
	} else {
		struct msg_list *n = malloc(sizeof(struct msg_list));
		n->basename = strdup(base);
		n->fullnames = malloc(sizeof(*n->fullnames) * 2);
		asprintf(&n->fullnames[0], "%s/%s", sub, fn);
		n->fullnames[1] = NULL;
		n->next = *mlist;
		*mlist = n;
	}
}

static
void __attribute__((unused)) msg_list_dump(const struct msg_list *m)
{
	while (m) {
		printf("base: %s\n", m->basename);
		char** fn = m->fullnames;
		while (*fn)
			printf(" - %s\n", *fn++);
		m = m->next;
	}
}

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
	int forceflags;
	DIR *dir;
	struct dirent *de;
	struct msg_list *mlist = NULL, *slist;
	struct stat st;

	if (fstatat(fd, "maildirfolder", &st, 0) < 0) {
		if (errno != ENOENT) {
			add_error(ec, "maildirfolder: %s", strerror(errno));
		} else if (*rpath) {
			add_error(ec, "Expected to find a file called maildirfolder");
		}
	} else if (!*rpath) {
		add_error(ec, "Did not expect to find a file called maildirfolder");
	} else {
		if (st.st_size)
			add_error(ec, "maildirfolder file should be empty.");
		if (st.st_uid != uid)
			add_error(ec, "maildirfolder: Wrong ownership, uid=%lu is not %lu.", (unsigned long)st.st_uid, (unsigned long)uid);
		if (st.st_gid != gid)
			add_error(ec, "maildirfolder: Wrong group, gid=%lu is not %lu.", (unsigned long)st.st_gid, (unsigned long)gid);
	}

	check_ownership(fd, "", ec, ".");
	for (sp = maildir_subs; *sp; ++sp) {
		subname = *sp;
		noscan = *subname == '-';
		if (noscan)
			++subname;
		forceflags = *subname == '+';
		if (forceflags)
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
				if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
					continue;
				check_ownership(sfd, de->d_name, ec, "%s/%s", subname, de->d_name);
				msg_list_add(&mlist, subname, de->d_name);

				const char* colon = strchr(de->d_name, ':');
				if (!colon) {
					if (forceflags)
						add_error(ec, "%s/%s: in folder that requires flags (:2, in filename).\n",
								subname, de->d_name);
				} else if (strncmp(":2,", colon, 3) == 0) {
					int alphabetic = 1;
					char last_flag = 0;
					for (const char* flag = colon + 3; *flag; ++flag) {
						if (*flag == ',') {
							/* dovecot extended for this, warn about it but don't error on it */
							printf("\n%s/%s: warning: , found in flags, indicative of Dovecot extensions.", subname, de->d_name);
							break;
						}

						alphabetic &= *flag > last_flag;
						if (!strchr(valid_flags, *flag))
							add_error(ec, "%s/%s: invalid flag %c found.", subname, de->d_name, last_flag);

						last_flag = *flag;
					}
					if (!alphabetic)
						add_error(ec, "%s/%s: flags are not in alphabetic order.", subname, de->d_name);
				} else {
					add_error(ec, "%s/%s: flags marker is not recognized, expected :2, - probably an unsupported version ...\n", subname, de->d_name);
				}
			}
			closedir(dir);
		}
	}

	for (slist = mlist; slist; slist = slist->next) {
		/* Whilst base name has a structure, it really doesn't matter ...  the
		 * structure is merely intended to produce a unique - we NEED it to be
		 * unique, else the POP3 and IMAP servers tend to die. */

		/* We checked the filename structure above, so don't bother again,
		 * just check for duplicates here */
		if (slist->fullnames[1]) {
			// we know there is more than one, count them.
			int i;
			for (i = 1; slist->fullnames[i]; ++i)
				;
			add_error(ec, "%s: %d occurences, which means stuff is not unique.",
					slist->basename, i);
			for (i = 0; slist->fullnames[i]; ++i)
				printf("\n - %s", slist->fullnames[i]);
			fflush(stdout);
		}
	}

	if (ec) {
		printf("\n *** %d errors identified ***\n", ec);
	} else {
		printf(" All Good.\n");
	}

	//msg_list_dump(mlist);
	msg_list_free(mlist);

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
