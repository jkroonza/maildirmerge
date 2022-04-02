#include <stdio.h>
#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>

static const char * progname;
static const char * maildir_subs[] = { "new", "cur", NULL }; /* ignore tmp here */

#define OUTPUT_ALL			0
#define OUTPUT_TOTALS		1
#define OUTPUT_TOTALSIZE	2
#define OUTPUT_MESSAGECOUNT	3

static int human = 0;
static int parse = 0;
static int output = OUTPUT_ALL;

static
char* pretty_size(size_t input, char * buffer /* should be at least 12 bytes "XXXX.XX XiB" */)
{
	size_t rem = 0;
	const char *units = "kMGTPEZY";
	const char *unit = NULL;

	while (*units && input >= 1024) {
		rem = input & 0x3ff;
		input >>= 10;
		unit = units++;
	}

	if (unit)
		sprintf(buffer, "%.2f %ciB", input + rem / 1024.0, *unit);
	else
		sprintf(buffer, "%zu B", input);

	return buffer;
}

static
void __attribute__((noreturn)) usage(int x)
{
	FILE *o = x ? stderr : stdout;

	fprintf(o, "USAGE: %s [options] folder [...]\n", progname);
	fprintf(o, "OPTIONS:\n");
	fprintf(o, "  --human,-h\n");
	fprintf(o, "    Output sizes in human readable format (doesn't affect counts at this stage).\n");
	fprintf(o, "  --parse,-p\n");
	fprintf(o, "    Output in parseable format.  Takes precedence over --human.\n");
	fprintf(o, "  --totalonly|--sizeonly|--countonly\n");
	fprintf(o, "    Without these options all individual folders are listed as well.\n");
	fprintf(o, "    Last one specified takes precedence.\n");
	exit(x);
}

static
void calc_size(int dir_fd, size_t *total_size, size_t *total_count, const char* rpath)
{
	size_t size = 0, count = 0;
	const char ** sub;

	for (sub = maildir_subs; *sub; ++sub) {
		int sub_fd = openat(dir_fd, *sub, O_RDONLY);
		if (sub_fd < 0) {
			fprintf(stderr, "INBOX%s/%s: %s\n", rpath, *sub, strerror(errno));
			continue;
		}
		DIR *d = fdopendir(sub_fd);
		struct dirent *de;
		if (!d) {
			fprintf(stderr, "INBOX%s/%s: %s\n", rpath, *sub, strerror(errno));
			close(sub_fd);
			continue;
		}
		while ((de = readdir(d))) {
			/* We ignore anything starting with a ., this covers . and .., which should
			 * be the only folders in this place, everything else should be valid
			 * maildir names ... */
			if (de->d_name[0] == '.')
				continue;
			const char* S = strstr(de->d_name, "S=");
			if (!S) {
				fprintf(stderr, "INBOX%s/%s/%s: Invalid filename.\n", rpath,
						*sub, de->d_name);
				continue;
			}
			unsigned long long msgsize = strtoull(S+2, NULL, 10);

			//printf("%s => %s => %llu\n", de->d_name, S, msgsize);

			size += msgsize;
			++count;
		}
		closedir(d);
		close(sub_fd);
	}

	*total_size += size;
	*total_count += count;

	if (output == OUTPUT_ALL) {
		if (parse) {
			printf("INBOX%s %zu %zu\n", rpath, size, count);
		} else if (human) {
			char bfr[15];
			printf("INBOX%-20s: %11s / %9zu messages\n", rpath, pretty_size(size, bfr),
					count);
		} else {
			printf("INBOX%-20s: %12zu B / %9zu messages\n", rpath, size, count);
		}
	}
}

static
void proc_path(const char* path)
{
	size_t msgsize = 0, msgcount = 0;
	char bfr[15];
	struct stat st;
	DIR *d;
	struct dirent *de;

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror(path);
		return;
	}

	if (fstat(fd, &st) < 0) {
		close(fd);
		perror(path);
		return;
	}

	if (!S_ISDIR(st.st_mode)) {
		close(fd);
		fprintf(stderr, "%s is not a directory.\n", path);
	}

	if (output == OUTPUT_ALL) {
		if (parse)
			printf("PATH: %s\n", path);
		else
			printf("Folder details for %s:\n", path);
	}
	calc_size(fd, &msgsize, &msgcount, "");

	d = fdopendir(fd);
	if (!d) {
		perror(path);
		fprintf(stderr, "Not scanning for sub-folders.\n");
	} else while ((de = readdir(d))) {
		/* sub-folders starts with a ., and is obviously not . or .. */
		if (de->d_name[0] != '.' || !strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;

		if (de->d_type != DT_DIR && de->d_type != DT_UNKNOWN)
			continue;

		int sfd = openat(fd, de->d_name, O_RDONLY);
		if (sfd < 0) {
			fprintf(stderr, "%s/%s: %s\n", path, de->d_name, strerror(errno));
			continue;
		}

		calc_size(sfd, &msgsize, &msgcount, de->d_name);
	}
	closedir(d);
	close(fd);

	switch (output) {
	case OUTPUT_ALL:
		if (parse)
			printf("TOTAL %zu %zu\n", msgsize, msgcount);
		else if (human)
			printf("Total: %s over %zu messages.\n", pretty_size(msgsize, bfr), msgcount);
		else
			printf("Total: %zu B over %zu messages.\n", msgsize, msgcount);
		break;
	case OUTPUT_TOTALS:
		if (parse)
			printf("%s %zu %zu\n", path, msgsize, msgcount);
		else if (human)
			printf("%s has %s over %zu messages.\n", path, pretty_size(msgsize, bfr), msgcount);
		else
			printf("%s: has %zu B over %zu messages.\n", path, msgsize, msgcount);
		break;
	case OUTPUT_TOTALSIZE:
		printf("%zu\n", msgsize);
		break;
	case OUTPUT_MESSAGECOUNT:
		printf("%zu\n", msgcount);
		break;
	default:
		fprintf(stderr, "BUG: output format not understood for totals.\n");
	}
}

static struct option options[] = {
	{ "human",			no_argument, NULL, 'h' },
	{ "parse",			no_argument, NULL, 'p' },
	{ "totalonly",		no_argument, &output, OUTPUT_TOTALS },
	{ "sizeonly",		no_argument, &output, OUTPUT_TOTALSIZE },
	{ "countonly",		no_argument, &output, OUTPUT_MESSAGECOUNT },
	{ NULL, 0, NULL, 0 }
};

int main(int argc, char** argv)
{
	progname = *argv;
	int c;

	while ((c = getopt_long(argc, argv, "hp", options, NULL)) != -1) {
		switch (c) {
		case 0:
			break;
		case 'h':
			human = 1;
			break;
		case 'p':
			parse = 1;
			break;
		case '?':
			usage(1);
		default:
			fprintf(stderr, "Option not implemented: %c.\n", c);
			usage(1);
		}
	}

	if (!*argv) {
		fprintf(stderr, "At least one path is required.\n");
		usage(1);
	}

	while (argv[optind]) {
		proc_path(argv[optind++]);
		if (output == OUTPUT_ALL && argv[optind])
			printf("\n");
	}

	return 0;
}
