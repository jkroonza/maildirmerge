#define _GNU_SOURCE

#include <getopt.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <ctype.h>

#include "servertypes.h"

#define lerror(fmt, ...) fprintf(stderr, fmt ": %s.\n", ##__VA_ARGS__, strerror(errno))

#define DEFAULT_MAXAGE		"1 year ago"

static const char* progname = NULL;
static const char * subsources[] = { "new", "cur", NULL };

static time_t maxage;
static char* sourcefolder = NULL;
static size_t sflen = -1;
static bool recursive = false;
static bool dry_run = false;

static
time_t maxage2time(const char* maxage)
{
	int p[2]; // 0 read, 1 write.
	FILE* fp;
	pid_t pid;
	time_t res = 0;
	if (pipe(p) < 0) {
		perror("pipe");
		exit(1);
	}

	pid = fork();
	if (pid < -1) {
		perror("fork");
		exit(1);
	}

	if (pid == 0) {
		char* args[] = {
			"date",
			"+%s",
			"-d",
			strdupa(maxage),
			NULL,
		};
		/* stdin */
		int t = open("/dev/null", O_RDONLY);
		dup2(t, 0);
		close(t);
		/* stdout */
		dup2(p[1], 1);
		/* read end */
		close(p[0]);

		execvp(*args, args);
		perror("execvp(date)");
		exit(1);
	}

	close(p[1]);
	fp =  fdopen(p[0], "r");
	if (fscanf(fp, "%lu", &res) != 1) {
		fprintf(stderr, "Error reading from date sub-process.\n");
		exit(1);
	}
	fclose(fp);

	printf("Archiving email older than: %s", ctime(&res));

	return res;
}

static
void __attribute__((noreturn)) usage(int x)
{
	FILE *o = x ? stderr : stdout;

	fprintf(o, "USAGE: %s [options] root_folder [...]\n", progname);
	fprintf(o, "IMPORTANT:  sourcefolders will be migrated (merged) into destfolder.\n");
	fprintf(o, "  The emails will be REMOVED from the sourcefolders.\n");
	fprintf(o, "OPTIONS:\n");
	fprintf(o, "  -n|--dry-run\n");
	fprintf(o, "    Dry-run only, output what would be done without doing it.\n");
	fprintf(o, "  -s|--sourcefolder sourcefolder\n");
	fprintf(o, "    If purging should be performed on a source subfolder rather than INBOX\n");
	fprintf(o, "  -m|--maxage string\n");
	fprintf(o, "    Maximum age of emails to retain in source folder, this is passed to the\n");
	fprintf(o, "    date CLI tool using date -d 'string' - so please verify this usage.\n");
	fprintf(o, "    defaults to '%s'.\n", DEFAULT_MAXAGE);
	fprintf(o, "  -r|--recursive\n");
	fprintf(o, "    Perform this recursively on all subfolders.\n");
	fprintf(o, "  -h|--help\n");
	fprintf(o, "    Enable force mode, permits overriding certain safeties.\n");
	exit(x);
}

static struct option options[] = {
	{ "dry-run",		no_argument,		NULL,	'n' },
	{ "sourcefolder",	required_argument,	NULL,	's' },
	{ "maxage",			required_argument,	NULL,	'm' },
	{ "recursive",		no_argument,		NULL,	'r' },
	{ NULL, 0, NULL, 0 },
};

static
int purge_sub(const char* name, int fd)
{
	int ret = 0;
	time_t filetime;
	char * endptr;

	for (const char ** nn = subsources; *nn; nn++) {
		int dfd = openat(fd, *nn, O_RDONLY);
		if (dfd < 0) {
			fprintf(stderr, "%s/%s: %s\n", name, *nn, strerror(errno));
			ret = 1;
			continue;
		}
		DIR *dir = fdopendir(dfd);
		if (!dir) {
			fprintf(stderr, "%s/%s: %s\n", name, *nn, strerror(errno));
			ret = 1;
			continue;
		}
		struct dirent *de;
		while ((de = readdir(dir))) {
			if (de->d_type != DT_REG)
				continue;

			filetime = strtoul(de->d_name, &endptr, 10);
			if (endptr == de->d_name || !endptr || *endptr != '.') {
				fprintf(stderr, "Failed to extra timestamp from %s/%s/%s\n", name, *nn, de->d_name);
				continue;
			}

			if (filetime >= maxage)
				continue;

			if (dry_run)
				printf("Would remove %s/%s/%s\n", name, *nn, de->d_name);
			else
				unlinkat(dfd, de->d_name, 0);
		}
		closedir(dir); /* closes dfd */
	}
	return ret;
}

static
int purge(const char* base)
{
	int ret = 0;

	int basefd = open(base, O_RDONLY);
	if (basefd < 0) {
		perror(base);
		goto errout;
	}

	if (!sourcefolder)
		purge_sub(base, basefd);

	if (sourcefolder || recursive) {
		DIR* dir = fdopendir(dup(basefd));
		struct dirent *de;
		while ((de = readdir(dir))) {
			char * sfname;
			if (*de->d_name != '.' || strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
				continue;
			if (sourcefolder) {
				if (strncmp(de->d_name, sourcefolder, sflen))
					continue;
				if (de->d_name[sflen] && (!recursive || de->d_name[sflen] != '.'))
					continue;
			}

			if (de->d_type == DT_UNKNOWN) {
				static int warned = 0;
				if (!warned) {
					fprintf(stderr, "readdir() doesn't provide d_type, assuming everything is folders to avoid costly stat() calls.\n");
					warned = 1;
				}
			} else if (de->d_type != DT_DIR)
				continue;

			int sfd = openat(basefd, de->d_name, O_RDONLY);
			asprintf(&sfname, "%s/%s", base, de->d_name);
			ret |= purge_sub(sfname, sfd);
			close(sfd);
		}
		closedir(dir);
	}

cleanup:
	if (basefd >= 0)
		close(basefd);

	return ret;
errout:
	ret = 1;
	goto cleanup;
}

int main(int argc, char** argv)
{
	int c;
	const char* _maxage = DEFAULT_MAXAGE;

	progname = *argv;

	while ((c = getopt_long(argc, argv, "ns:m:r", options, NULL)) != -1) {
		switch (c) {
		case 0:
			break;
		case 'n':
			dry_run = true;
			break;
		case 's':
			sourcefolder = optarg;
			sflen = strlen(sourcefolder);
			break;
		case 'm':
			_maxage = optarg;
			break;
		case 'r':
			recursive = true;
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

	maxage = maxage2time(_maxage);
	if (!maxage) {
		fprintf(stderr, "Error converting '%s' to a date and time structure.\n",
				_maxage);
		usage(1);
	}

	printf("maxage=%lu\n", maxage);

	if (!argv[optind]) {
		fprintf(stderr, "At least one maildir should be specified.\n");
		usage(1);
	}

	while (argv[optind]) {
		int r = purge(argv[optind++]);
		if (r)
			return r;
	}

	return 0;
}
