#define _GNU_SOURCE

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "servertypes.h"
#include "filetools.h"

#define lerror(fmt, ...) fprintf(stderr, fmt ": %s.\n", ##__VA_ARGS__, strerror(errno))

static const char* progname = NULL;
static const char * maildir_subs[] = { "cur", "new", NULL }; /* no tmp */
static unsigned long long mintime = 86400 * 7;

static
unsigned long long convert_date(/* const */char* datestring)
{
	int pfds[2];
	if (pipe(pfds) < 0) {
		perror("pipe");
		exit(1);
	}

	pid_t pid = fork();
	if (pid == -1) {
		perror("fork");
		exit(1);
	} else if (pid == 0) {
		/* child */
		if (dup2(pfds[1], 1) < 0) {
			perror("dup2");
			exit(1);
		}
		close(pfds[0]);
		close(pfds[1]);
		char *const argv[] = {
			"date",
			"-d",
			datestring,
			"+%s",
			NULL
		};
		execvp("date", argv);
		perror("date");
		exit(1);
	} else {
		/* parent */
		char bfr[64], *e;
		int status;
		close(pfds[1]);
		int r = read(pfds[0], bfr, sizeof(bfr) - 1);
		if (r < 0) {
			perror("read");
			exit(1);
		}
		bfr[r] = 0; /* null terminate the string */
		close(pfds[0]);
		if (waitpid(pid, &status, 0) < 0) {
			perror("waitpid");
			exit(1);
		}

		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			fprintf(stderr, "date command exited abnormally (status=%d).\n", status);
			exit(1); /* return 0 to indicate failure? */
		}

		unsigned long long t = strtoull(bfr, &e, 10);
		if (*bfr == 0 || (*e != '\n' && *e != 0)) {
			fprintf(stderr, "Invalid output from date for %s (got %s).\n", datestring, bfr);
			exit(1);
		}
		return t;
	}
}

static
void __attribute__((noreturn)) usage(int x)
{
	FILE *o = x ? stderr : stdout;

	fprintf(o, "USAGE: %s [options] folder [...]\n", progname);
	fprintf(o, "IMPORTANT: each folder will have files renamed which for IMAP will result in re-downloads\n");
	fprintf(o, "  and for POP3 in duplicate email downloads.  We thus block this application if POP3 is detected.\n");
	fprintf(o, "OPTIONS:\n");
	fprintf(o, "  -n|--dry-run\n");
	fprintf(o, "    Do not actually take action, just output what would be done (implies --verbose).\n");
	fprintf(o, "  -m|--mintime seconds\n");
	fprintf(o, "    If file timestamp and Date: header differs by less than this, do not update.\n");
	fprintf(o, "    DEFAULT: 604800 (1 week)\n");
	fprintf(o, "  -R|--replace\n");
	fprintf(o, "    Do NOT use REPLACE_NOREPLACE.  This option can potentially destroy email,\n");
	fprintf(o, "    as an extra safety a stat() call will be made prior to rename, and if the\n");
	fprintf(o, "    target file exists will be skipped.  This is racey, not to mention bad for performance.\n");
	fprintf(o, "  -v|--verbose\n");
	fprintf(o, "    Be verbose in that renames are output to stdout.\n");
	fprintf(o, "  -h|--help\n");
	fprintf(o, "    This help text.\n");
	exit(x);
}

static struct option options[] = {
	{ "dry-run",		no_argument,		NULL,	'n' },
	{ "mintime",		no_argument,		NULL,	'm' },
	{ "replace",		no_argument,		NULL,	'R' },
	{ "verbose",		no_argument,		NULL,	'v' },
	{ "help",			no_argument,		NULL,	'h' },
	{ NULL, 0, NULL, 0 },
};

int main(int argc, char**argv)
{
	int c;
	bool dryrun = false;
	bool verbose = false;
	unsigned rename_flags = RENAME_NOREPLACE;

	progname = *argv;

	while ((c = getopt_long(argc, argv, "hnm:Rv", options, NULL)) != -1) {
		switch (c) {
		case 0:
			break;
		case 'n':
			dryrun = true;
			break;
		case 'm':
			{
				char *endp;
				unsigned long long t = strtoull(optarg, &endp, 0);

				if (*endp) {
					fprintf(stderr, "Error converting %s to a number.\n", optarg);
					exit(1);
				}

				mintime = t;
			}
			break;
		case 'R':
			rename_flags &= ~RENAME_NOREPLACE;
			break;
		case 'v':
			verbose = true;
			break;
		case 'h':
			usage(0);
		case '?':
			fprintf(stderr, "Unrecognised option encountered.\n");
			usage(1);
		default:
			fprintf(stderr, "option not implemented: %c.\n", c);
			exit(1);
		}
	}

	if (dryrun)
		verbose = true;

	if (!argv[optind]) {
		fprintf(stderr, "At least one folder to operate on must be specified.\n");
		usage(1);
	}

	for ( ; argv[optind]; ++optind) {
		if (verbose)
			printf("Processing %s\n", argv[optind]);
		int dir_fd = get_maildir_fd(argv[optind]);
		if (dir_fd < 0)
			continue;

		for (const char ** sub = maildir_subs; *sub; ++sub) {
			int sub_fd = openat(dir_fd, *sub, O_RDONLY);
			if (sub_fd < 0) {
				fprintf(stderr, "%s/%s: %s\n", argv[optind], *sub, strerror(errno));
				continue;
			}

			DIR* dir= fdopendir(sub_fd);
			if (!dir) {
				fprintf(stderr, "%s/%s: %s\n", argv[optind], *sub, strerror(errno));
				close(sub_fd);
				continue;
			}

			struct dirent * de;
			while ((de = readdir(dir))) {
				if (de->d_type == DT_UNKNOWN) {
					struct stat st;
					if (fstatat(sub_fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
						fprintf(stderr, "fstatat(%s/%s/%s): %s", argv[optind], *sub, de->d_name, strerror(errno));
						continue;
					}
					if ((st.st_mode & S_IFMT) == S_IFREG)
						de->d_type = DT_REG;
				}
				if (de->d_type != DT_REG)
					continue;

				errno = 0;
				struct mail_header *hd = get_mail_header(sub_fd, de->d_name);
				if (errno) {
					fprintf(stderr, "%s/%s/%s: %s\n", argv[optind], *sub, de->d_name, strerror(errno));
					free_mail_header(hd);
					continue;
				}

				//for (struct mail_header *c = hd; c; c = c->next) {
				//	printf("  %s:\n", c->header);
				//	for (char ** v = c->value; *v; ++v) {
				//		printf("    - %s\n", *v);
				//	}
				//}

				const struct mail_header *date = find_mail_header(hd, "date");

				if (!date) {
					fprintf(stderr, "%s/%s/%s: No Date: header found.\n", argv[optind], *sub, de->d_name);
					free_mail_header(hd);
					continue;
				}

				if (date->value[1])
					fprintf(stderr, "%s/%s/%s: Multiple Date: headers found, using first one.\n", argv[optind], *sub, de->d_name);

				char *endp;
				unsigned long long header_ts = convert_date(*date->value);
				unsigned long long filename_ts = strtoull(de->d_name, &endp, 10);

				if (*endp != '.') {
					fprintf(stderr, "%s/%s/%s: Filename isn't of the format TS.stuff\n",
							argv[optind], *sub, de->d_name);
					free_mail_header(hd);
					continue;
				}

				//fprintf(stderr, "%s/%s/%s: header ts: %s = %lld\n", argv[optind], *sub, de->d_name, *date->value, header_ts);
				//fprintf(stderr, "%s/%s/%s: filename ts: %lld\n", argv[optind], *sub, de->d_name, filename_ts);

				if (filename_ts < header_ts + mintime) {
					free_mail_header(hd);
					continue;
				}

				char *tfname;
				asprintf(&tfname, "%llu%s", header_ts, endp);
				if (verbose)
					printf("%s/%s/%s to %s (Date: %s)\n", argv[optind], *sub, de->d_name, tfname, *date->value);
				free_mail_header(hd);

				if (!dryrun) {
					struct stat st;

					if ((rename_flags & RENAME_NOREPLACE) == 0) {
						if (fstatat(sub_fd, tfname, &st, 0) == 0)
							errno = EEXIST;

						if (errno != ENOENT) {
							lerror("%s/%s/%s => %s", argv[optind], *sub, de->d_name, tfname);
							free(tfname);
							continue;
						}
					}

					if (renameat2(sub_fd, de->d_name, sub_fd, tfname, rename_flags) < 0) {
						lerror("%s/%s/%s => %s", argv[optind], *sub, de->d_name, tfname);
						if ((rename_flags & RENAME_NOREPLACE) != 0 && errno == EINVAL &&
								fstatat(sub_fd, tfname, &st, 0) == -1 && errno == ENOENT) {
							fprintf(stderr, "We received EINVAL on rename using RENAME_NOREPLACE.  Possibly the filesystem doesn't like this, so please retry using (potentially dangerous) -R.\n");
							exit(1);
						}
					}
				}

				free(tfname);
			}

			closedir(dir);
		}

		close(dir_fd);
	}

	return 0;
}
