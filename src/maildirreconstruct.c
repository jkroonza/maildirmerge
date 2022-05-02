#include <filetools.h>

#define _GNU_SOURCE

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <string.h>
#include <dirent.h>
#include <sys/time.h>

#include "servertypes.h"

static const char* progname;

/* if there is a built-in for this I can't find it right now and don't have
 * too much time to waste TODO */
static
int timespec_cmp(const struct timespec * a, const struct timespec * b)
{
	if (a->tv_sec < b->tv_sec)
		return -1;
	if (a->tv_sec > b->tv_sec)
		return 1;
	if (a->tv_nsec < b->tv_nsec)
		return -1;
	if (a->tv_nsec > b->tv_nsec)
		return 1;
	return 0;
}

static
void __attribute__((noreturn)) usage(int x)
{
	FILE *o = x ? stderr : stdout;

	fprintf(o, "USAGE: %s [options] destfolder sourcefolder [...]\n", progname);
	fprintf(o, "This will recreate a maildir from fragments (say a set of glusterfs bricks).\n");
	fprintf(o, "You should *copy* the fragments onto a single filesystem (this code uses hard links).\n");
	fprintf(o, " destfolder must be empty.\n");
	fprintf(o, " sourcefolders will be left in tact, no permission or ownership fixups will be made - those you need to do yourself as directed by maildircheck.\n");
	exit(x);
}

static struct option options[] = {
	{ NULL, 0, NULL, 0 },
};

#define mdir_error(t, fmt, ...) do { fprintf(stderr, "%s%s%s: " fmt "\n", t, rel ? "/" : "", rel ?: "", ## __VA_ARGS__); ++ec; } while(0)
#define mdir_fmt_error(t, fmt, ...) mdir_error(t, fmt ": %s", ## __VA_ARGS__, strerror(errno))
#define mdir_perror(t, s) mdir_error(t, "%s: %s", s, strerror(errno))

static
int mdir(const char* target, int targetfd, const char* source, int sourcefd, const char* rel, const char* const * extra_folders)
{
	const char *bases[] = { "cur", "new", "-tmp", NULL };
	int ec = 0, extra = 0;
	/* rel is relative to both target and source, and *fd references these relative targets */
	/* rel is NULL for INBOX. */

	const char * const * bi = NULL;
	while (true) {
		if (!bi) {
			bi = bases;
		} else {
			++bi;
			if (!*bi) {
				if (extra_folders) {
					bi = extra_folders;
					extra_folders = NULL;
					extra = 1;
				} else {
					break;
				}
			}
		}

		const char* base = *bi;
		struct stat st;

		int linkto, linkfrom;
		DIR* dir;
		struct dirent* de;

		int nocopy = base[0] == '-';
		if (nocopy)
			++base;

		if (fstatat(targetfd, base, &st, AT_SYMLINK_NOFOLLOW) == 0) {
			if (!S_ISDIR(st.st_mode)) {
				mdir_error(target, "%s exist but is not a folder (we should have created it on an earlier round)", base);
				continue;
			}
		} else if (errno == ENOENT) {
			if (mkdirat(targetfd, base, 0700) < 0) {
				mdir_perror(target, base);
				continue;
			}
		} else {
			mdir_perror(target, base);
			continue;
		}

		if (nocopy)
			continue;

		linkto = openat(targetfd, base, O_RDONLY | O_DIRECTORY);
		if (linkto < 0) {
			mdir_perror(target, base);
			continue;
		}

		linkfrom = openat(sourcefd, base, O_RDONLY | O_DIRECTORY);
		if (linkfrom < 0) {
			mdir_perror(source, base);
			if (errno == ENOENT && !extra) {
				fprintf(stderr, "This is unexpected, but let's not count it as an error\n");
				--ec;
			}
			close(linkto);
			continue;
		}

		dir = fdopendir(linkfrom);
		if (!dir) {
			mdir_perror(source, base);
			close(linkto);
			close(linkfrom);
			continue;
		}

		while ((de = readdir(dir))) {
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
				continue;

			if (de->d_type != DT_REG && de->d_type != DT_UNKNOWN /* fstat below will reveal */) {
				mdir_error(source, "%s/%s is not a regular file!", base, de->d_name);
				continue;
			}

			if (fstatat(linkfrom, de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
				mdir_fmt_error(source, "%s/%s", base, de->d_name);
				continue;
			}

			if (!S_ISREG(st.st_mode)) {
				mdir_error(source, "%s/%s is not a regular file!", base, de->d_name);
				continue;
			}

			if (st.st_size == 0) {
				/* either an unhealed glusterfs file, or a dht linkfile
				 * either way, we can't use it */
				continue;
			}

retry_link:
			if (linkat(linkfrom, de->d_name, linkto, de->d_name, 0) == 0)
				continue; /* we're done */

			if (errno != EEXIST) {
				fprintf(stderr, "Error linking %s from %s%s%s/%s/ to %s%s%s/%s/: %s.\n",
						de->d_name,
						source, rel ? "/" : "", rel ?: "", base,
						target, rel ? "/" : "", rel ?: "", base,
						strerror(errno));
				ec++;
				continue;
			}

			/* target already exist, so we need to compare them and make
			 * sure that they are identical (in content), if not, use the
			 * later mtime one */

			int r = files_identical(linkfrom, de->d_name, &st, linkto, de->d_name, NULL);

			if (r == 0) {
				struct stat st2;
				/* only do this for meta files, a few duplicate downloads etc is probably OK
				 * but with email files we want to take no chances */
				if (extra && fstatat(linkto, de->d_name, &st2, AT_SYMLINK_NOFOLLOW) == 0) {
					if (timespec_cmp(&st2.st_mtim, &st.st_mtim) < 0) {
						if (unlinkat(linkto, de->d_name, 0) < 0) {
							mdir_perror(target, de->d_name);
						} else
							goto retry_link;
					}
				} else {
					mdir_error(target, "%s/%s: alternative file available at %s%s%s/%s/.\n",
							base, de->d_name, source, rel ? "/" : "", rel ?: "", base);
				}
			} else if (r < 0)
				ec++; /* files_identical will already have output an error */
		}

		closedir(dir); /* linkfrom */
		close(linkto);
	}
	return ec;
}

static
int overlay(const char* target, int targetfd, const char* source, const char* const * metafiles, int root){
	int sourcefd = open(source, O_RDONLY | O_DIRECTORY);
	int ec = 0;
	DIR *dir;
	const char **extra_folders = NULL;
	int efc = 0, efm = 0;
	struct dirent *de;
	struct stat st;
	const char * const * mfscan;

	if (sourcefd < 0) {
		perror(source);
		return 1;
	}

	dir = fdopendir(dup(sourcefd));
	if (!dir) {
		perror(source);
		fprintf(stderr, "meta files/folders, and sub-folders in the case of mail root, will not be able to be synced.");
		ec++;
	} else {
		while ((de = readdir(dir))) {
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0
					|| strcmp(de->d_name, "new") == 0
					|| strcmp(de->d_name, "cur") == 0
					|| strcmp(de->d_name, "tmp") == 0
					|| strcmp(de->d_name, "maildirfolder") == 0
					|| strcmp(de->d_name, "maildirsize") == 0)
				continue;

			mfscan = metafiles;
			while (*mfscan && strcmp(de->d_name, *mfscan) != 0)
				++mfscan;

			if (de->d_type == DT_UNKNOWN) {
				if (fstatat(sourcefd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
					fprintf(stderr, "%s/%s: %s.\n", source, de->d_name, strerror(errno));
					ec++;
					continue;
				}
			} else
				st.st_mode = 0; /* our marker, also stops S_IS* from working. */

			if (de->d_type == DT_DIR || S_ISDIR(st.st_mode)) {
				if (de->d_name[0] == '.') {
					if (root) {
						char *starget = NULL, *ssource = NULL;
						int sfd = -1, t;

						asprintf(&starget, "%s/%s", target, de->d_name);
						asprintf(&ssource, "%s/%s", source, de->d_name);
						if (!starget || !ssource) {
							fprintf(stderr, "Memory allocation error trying to traverse into %s/%s - skipping.", source, de->d_name);
							ec++;
							free(starget);
							free(ssource);
							continue;
						}

						if (mkdirat(targetfd, de->d_name, 0700) < 0 && errno != EEXIST) {
							fprintf(stderr, "mkdir(%s): %s.\n", starget, strerror(errno));
							ec++;
						} else if ((sfd = openat(targetfd, de->d_name, O_RDONLY | O_DIRECTORY)) < 0) {
							fprintf(stderr, "open(%s): %s.\n", starget, strerror(errno));
							ec++;
						} else {
							t = openat(sfd, "maildirfolder", O_WRONLY | O_CREAT, 0600);
							if (t < 0) {
								fprintf(stderr, "WARNING: Unable to create maildirfolder in %s: %s.\n",
										starget, strerror(errno));
							} else
								close(t);

							ec += overlay(starget, sfd, ssource, metafiles, false);
							close(sfd);
						}
						free(starget);
						free(ssource);
					} else {
						fprintf(stderr, "Sub-folder %s under sub-folder in %s?\n",
								de->d_name, source);
					}
				} else if (*mfscan) {
					if (efc >= efm) {
						efm += 1024;
						extra_folders = realloc(extra_folders, efm * sizeof(*extra_folders));
					}
					extra_folders[efc++] = *mfscan;
				} else {
					fprintf(stderr, "WARNING: %s/%s isn't a known maildir file, and is not a known metadata file, ignoring.\n", source, de->d_name);
				}
			} else if (de->d_type == DT_REG || S_ISREG(st.st_mode)) {
				if (!*mfscan) {
					fprintf(stderr, "WARNING: %s/%s isn't a known maildir file, and is not a known metadata file, ignoring.\n", source, de->d_name);
					continue;
				}

				if (st.st_mode == 0 && fstatat(sourcefd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
					fprintf(stderr, "%s/%s: %s.\n", source, de->d_name, strerror(errno));
					ec++;
					continue;
				}

				if (st.st_size == 0) {
					/* either an unhealed glusterfs file, or a dht linkfile
					 * either way, we can't use it */
					continue;
				}

retry_link:
				if (linkat(sourcefd, de->d_name, targetfd, de->d_name, 0) == 0)
					continue; /* we're done */

				if (errno != EEXIST) {
					fprintf(stderr, "Error linking %s from %s/ to %s/: %s.\n",
							de->d_name, source, target, strerror(errno));
					ec++;
					continue;
				}

				/* target already exist, so we need to compare them and make
				 * sure that they are identical (in content), if not, use the
				 * later mtime one */

				int r = files_identical(sourcefd, de->d_name, &st, targetfd, de->d_name, NULL);

				if (r == 0) {
					struct stat st2;
					/* only do this for meta files, a few duplicate downloads etc is probably OK
					 * but with email files we want to take no chances */
					if (fstatat(targetfd, de->d_name, &st2, AT_SYMLINK_NOFOLLOW) == 0) {
						if (timespec_cmp(&st2.st_mtim, &st.st_mtim) < 0) {
							if (unlinkat(targetfd, de->d_name, 0) < 0) {
								fprintf(stderr, "unlink(%s/%s): %s.\n",
										target, de->d_name, strerror(errno));
								ec++;
							} else
								goto retry_link;
						}
					} else {
						fprintf(stderr, "fstatat(%s/%s): %s.\n", target, de->d_name, strerror(errno));
						ec++;
					}
				} else if (r < 0)
					ec++; /* files_identical will already have output an error */
			} else {
				fprintf(stderr, "%s/%s is neither a file nor a folder ... ?\n",
						source, de->d_name);
				ec++;
			}
		}

		closedir(dir);

		if (extra_folders)
			extra_folders[efc] = 0;
	}

	ec += mdir(target, targetfd, source, sourcefd, NULL, extra_folders);

	close(sourcefd);
	free(extra_folders);

	return ec;
}

int main(int argc, char** argv)
{
	int c, targetfd;
	const char* target;
	const char* const * metafiles = maildir_get_all_metafiles();

	progname = *argv;

	while ((c = getopt_long(argc, argv, "fhn", options, NULL)) != -1) {
		switch (c) {
		case 0:
			break;
		default:
			fprintf(stderr, "Option not implemented: %c.\n", c);
			exit(1);
		}
	}

	if (!argv[optind]) {
		fprintf(stderr, "No target folder specified!\n");
		usage(1);
	}

	target = argv[optind++];
	targetfd = open(target, O_RDONLY | O_DIRECTORY);
	if (targetfd < 0) {
		if (errno == ENOENT) {
			if (mkdir(target, 0700) < 0) {
				perror(target);
				return 1;
			}
			targetfd = open(target, O_RDONLY | O_DIRECTORY);
			if (targetfd < 0) {
				perror(target);
				return 1;
			}
		} else {
			perror(target);
			return 1;
		}
	} else {
		/* make sure folder is empty, ie, only . and .. entries,
		 * we create a copy of the fd */
		int tfd = dup(targetfd);
		if (tfd < 0) {
			perror(target);
			return 1;
		}
		DIR *dir = fdopendir(tfd);
		if (!dir) {
			perror(target);
			return 1;
		}
		struct dirent *d;
		while ((d = readdir(dir))) {
			if (strcmp(d->d_name, ".") && strcmp(d->d_name, "..")) {
				fprintf(stderr, "Target folder %s is not an empty folder.\n",
						target);
				return 1;
			}
		}
		closedir(dir);
	}

	c = 0;
	while (argv[optind])
		c += overlay(target, targetfd, argv[optind++], metafiles, true);

	if (c)
		fprintf(stderr, "%d errors encountered, you should PROBABLY NOT use the resulting folder.\n", c);
	return c ? 2 : 0;
}
