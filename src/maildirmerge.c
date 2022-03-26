#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>

#include "servertypes.h"

struct maildir_type_list {
	const struct maildir_type *type;
	void *pvt;
	struct maildir_type_list* next;
};

#define maildir_type_list_free(x) do { while (x) { struct maildir_type_list *_t = (x)->next; free(x); x = _t; }} while (0)

static
void maildir_type_list_prepend(struct maildir_type_list **list, const struct maildir_type *type)
{
	struct maildir_type_list *t = malloc(sizeof(struct maildir_type_list));
	if (!t) {
		perror("malloc");
		exit(1);
	}

	t->type = type;
	t->pvt = NULL;
	t->next = *list;
	*list = t;
}

static const char* progname = NULL;
static struct maildir_type_list *type_list = NULL;
static int force = 0, dry_run = 0, pop3_merge_seen = 0;
static int pop3_uidl = 0;
static const char* pop3_redirect = NULL;

void register_maildir_type(const struct maildir_type* type)
{
	maildir_type_list_prepend(&type_list, type);
}

static
void __attribute__((noreturn)) usage(int x)
{
	FILE *o = x ? stderr : stdout;

	fprintf(o, "USAGE: %s [options] destfolder sourcefolder [...]\n", progname);
	fprintf(o, "IMPORTANT:  sourcefolders will be migrated (merged) into destfolder.\n");
	fprintf(o, "  If all goes well the source will no longer exist.\n");
	fprintf(o, "OPTIONS:\n");
	fprintf(o, "  -f|--force\n");
	fprintf(o, "    Enable force mode, permits overriding certain safeties.\n");
	fprintf(o, "  -n|--dry-run\n");
	fprintf(o, "    Dry-run only, output what would be done without doing it.\n");
	fprintf(o, "  --pop3-uidl\n");
	fprintf(o, "    Do attempt to sync POP3 UIDL values.\n");
	fprintf(o, "  --pop3-redirect foldername\n");
	fprintf(o, "    Redirect previously seen messages for POP3 to an alternative IMAP folder.\n");
	fprintf(o, "  --pop3-merge-seen\n");
	fprintf(o, "    Ignore seen status when POP3 detected proceed to merge all mail into the destination.\n");
	fprintf(o, "    This is mutually exclusive with --pop3-redirect.\n");
	fprintf(o, "    By default any previously seen messages are left behind if the destination\n");
	fprintf(o, "    is detected to have POP3 active.  It doesn't care when last POP3 has been used currently.\n");
	fprintf(o, "  -h|--help\n");
	fprintf(o, "    Enable force mode, permits overriding certain safeties.\n");
	exit(x);
}

static
struct maildir_type_list* maildir_find_type(const char* folder)
{
	const struct maildir_type_list* test = type_list;
	struct maildir_type_list *result = NULL;

	while(test) {
		if (test->type->detect(folder))
			maildir_type_list_prepend(&result, test->type);
		test = test->next;
	}

	return result;
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

/**
 * This retrieves a file descriptor that for the folder, or -1 on error
 */
static
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

static
int get_maildir_fd(const char* folder)
{
	return get_maildir_fd_at(AT_FDCWD, folder);
}

static
int maildir_create_sub(int bfd, const char* target, const char* foldername)
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

static
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
		printf("p:%c\n", *c);
		if (*c == 'S')
			return 1;
		if (*c == ',') /* Dovecot extension, we can terminate here */
			return 0;
	}
	return 0;
}

static
void maildir_move(int sfd, const char* source, int tfd, const char* target, const char* sub, const char* fname)
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

#define out_error_if(x, f, ...) do { if (x) { fprintf(stderr, f ": %s\n", ## __VA_ARGS__, strerror(errno)); goto out; } } while(0)
static
void maildir_merge(const char* target, int targetfd, struct maildir_type_list *target_types,
		const char* source)
{
	struct maildir_type_list *source_types, *ti;
	const struct maildir_type *stype = NULL;
	int sourcefd = get_maildir_fd(source);
	int sfd = -1, tfd = -1, rfd = -1;
	char *redirectname = NULL;
	struct stat st;
	int is_pop3 = 0;
	void *stype_pvt = NULL;

	DIR* dir;
	struct dirent *de;

	if (sourcefd < 0)
		return;

	source_types = maildir_find_type(source);
	if (source_types) {
		if (source_types->next) {
			fprintf(stderr, "%s: multiple types triggered, not proceeding for safety.\n",
					source);
			goto out;
		}

		stype = source_types->type;
		maildir_type_list_free(source_types);

		if (stype->open)
			stype_pvt = stype->open(source, sourcefd);
	}

	printf("Merging %s (%s) into %s.\n", source, stype ? stype->label : "no type detected", target);

	for (ti = target_types; ti && !is_pop3; ti = ti->next)
		if (ti->type->is_pop3)
			is_pop3 = ti->type->is_pop3(ti->pvt);

	if (is_pop3)
		printf("Target folder is used for POP3.\n");

	/* For the new folder we couldn't care less, just rename from source into dest */
	sfd = openat(sourcefd, "new", O_RDONLY);
	out_error_if(sfd < 0, "%s/new", source);

	tfd = openat(targetfd, "new", O_RDONLY);
	out_error_if(tfd < 0, "%s/new", target);

	dir = fdopendir(sfd);
	out_error_if(!dir, "%s/new", source);

	while ((de = readdir(dir))) {
		switch (de->d_type) {
			case DT_REG:
				break;
			case DT_UNKNOWN:
				if (fstatat(sfd, de->d_name, &st, 0) < 0) {
					fprintf(stderr, "%s/cur/%s: %s\n", source, de->d_name, strerror(errno));
					continue;
				}

				if ((st.st_mode & S_IFMT) == S_IFREG)\
					break;
				/* FALLTHROUGH */
			default:
				continue;
		}

		maildir_move(sfd, source, tfd, target, "new", de->d_name);
	}
	close(sfd); sfd = -1;
	close(tfd); tfd = -1;

	/* the cur folder is somewhat more involved, there are IMAP UID values, as well
	 * as POP3 related work here.
	 *
	 * Firstly, we may or may not bring over the POP3 UIDL values (usually pointless).
	 *
	 * Secondly, we may leave behind, redirect or merge anyway messages that have already
	 * been seen (in short:
	 * leave behind - just ignore these.
	 * redirect - create a new maildir folder and dump these messages in it's new/
	 *
	 * These two 'functions' really aught to be merged.
	 **/
	sfd = openat(sourcefd, "cur", O_RDONLY);
	out_error_if(sfd < 0, "%s/cur", source);

	tfd = openat(targetfd, "cur", O_RDONLY);
	out_error_if(tfd < 0, "%s/cur", target);

	dir = fdopendir(sfd);
	out_error_if(!dir, "%s/cur", source);

	while ((de = readdir(dir))) {
		switch (de->d_type) {
			case DT_REG:
				break;
			case DT_UNKNOWN:
				if (fstatat(sfd, de->d_name, &st, 0) < 0) {
					fprintf(stderr, "%s/cur/%s: %s\n", source, de->d_name, strerror(errno));
					continue;
				}

				if ((st.st_mode & S_IFMT) == S_IFREG)\
					break;
				/* FALLTHROUGH */
			default:
				continue;
		}

		if (!is_pop3 || pop3_merge_seen || !message_seen(de->d_name)) {
			maildir_move(sfd, source, tfd, target, "cur", de->d_name);
			if (pop3_uidl) {
				if (!stype->pop3_get_uidl) {
					fprintf(stderr, "UIDL transfer requested but source doesn't support UIDL retrieval.\n");
				} else {
					char *basename = strdupa(de->d_name);
					char *t = strchr(basename, ':');
					if (t)
						*t = 0; /* truncate the fields out of there. */
					char *uidl = stype->pop3_get_uidl(stype_pvt, basename);

					if (uidl) {
						if (dry_run) {
							printf("Setting UIDL to %s\n", uidl);
						} else {
							for (ti = target_types; ti; ti = ti->next) {
								if (ti->type->pop3_set_uidl)
									ti->type->pop3_set_uidl(ti->pvt, basename, uidl);
							}
						}
						free(uidl);
					}
				}
			}
		} else if (pop3_redirect) {
			if (rfd < 0) {
				rfd = maildir_create_sub(tfd, target, pop3_redirect);
				if (rfd < 0)
					exit(1);
				asprintf(&redirectname, "%s/%s", target, pop3_redirect);
			}

			maildir_move(sfd, source, rfd, redirectname, "cur", de->d_name);
		} else if (dry_run) {
			printf("%s/cur/%s: left behind (seen, target is POP3, no redirect).\n",
					source, de->d_name);
		}
	}

out:
	if (stype && stype->close)
		stype->close(stype_pvt);

	close(sourcefd);
	if (sfd >= 0)
		close(sfd);
	if (tfd >= 0)
		close(tfd);
	if (rfd >= 0)
		close(rfd);
}

static struct option options[] = {
	{ "dry-run",		no_argument,		NULL,	'd' },
	{ "force",			no_argument,		NULL,	'f' },
	{ "help",			no_argument,		NULL,	'h' },
	{ "pop3-redirect",	required_argument,	NULL,	'r' },
	{ "pop3-merge-seen",no_argument,		&pop3_merge_seen, 1 },
	{ "pop3-uidl",		no_argument,		&pop3_uidl, 1 },
	{ NULL, 0, NULL, 0 },
};

int main(int argc, char** argv)
{
	int c, targetfd;
	const char* target;
	struct maildir_type_list *target_types, *ti;

	progname = *argv;

	while ((c = getopt_long(argc, argv, "fhn", options, NULL)) != -1) {
		switch (c) {
			case 0:
				break;
			case 'f':
				force = 1;
				break;
			case 'n':
				dry_run = 1;
				break;
			case 'r':
				pop3_redirect = optarg;
				break;
			case 'h':
				usage(0);
			case '?':
				fprintf(stderr, "Unrecognised option encountered.\n");
				usage(1);
			default:
				fprintf(stderr, "Option not implemented: %c.\n", c);
				exit(1);
		}
	}

	if (pop3_merge_seen && pop3_redirect) {
		fprintf(stderr, "You can't specify both --pop3-redirect and --pop3-ignore\n");
		usage(1);
	}

	if (!argv[optind]) {
		fprintf(stderr, "No target folder specified!\n");
		usage(1);
	}

	target = argv[optind++];
	targetfd = get_maildir_fd(target);
	if (targetfd < 0)
		return 1;

	target_types = maildir_find_type(target);
	if (!target_types) {
		fprintf(stderr, "Error detecting destination folder type(s).\n");
		if (!force) {
			fprintf(stderr, "Use --force to proceed as bare maildir.\n");
			return 1;
		}
	}

	for (ti = target_types; ti; ti = ti->next) {
		printf("%s: Detected type: %s\n", target, ti->type->label);
		if (ti->type->open)
			ti->pvt = ti->type->open(target, targetfd);
	}

	while (argv[optind])
		maildir_merge(target, targetfd, target_types, argv[optind++]);

	for (ti = target_types; ti; ti = ti->next) {
		if (ti->type->close)
			ti->type->close(ti->pvt);
	}

	maildir_type_list_free(target_types);
	maildir_type_list_free(type_list);

	return 0;
}
