#include "servertypes.h"

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/sendfile.h>

struct dovecot_data {
	const char* folder;
	int dirfd;

	struct uidl_list *uidl;
	char *uidl_headline;
};

struct dovecot_uidl_extra {
	char *flags;
	int utf8;
	int size;
};

static
int dovecot_detect(const char* folder)
{
	int detected;
	struct stat dummy;

	int dirfd = open(folder, O_RDONLY);
	if (dirfd < 0) {
		perror(folder);
		exit(1);
	}

	detected = fstatat(dirfd, "courierimapuiddb", &dummy, 0) == 0;

	close(dirfd);
	return detected;
}

static
const char * const * dovecot_metafiles()
{
	static const char *flist[] = {
		/* files */
		"dovecot.index",
		"dovecot.index.cache",
		"dovecot.index.log",
		"dovecot.index.log.2",
		"dovecot-keywords",
		"dovecot.list.index",
		"dovecot.list.index.log",
		"dovecot-uidlist",
		"dovecot-uidvalidity",
		NULL
	};
	return flist;
}

static
void* dovecot_open(const char* folder, int dirfd)
{
	struct dovecot_data *p = malloc(sizeof(struct dovecot_data));
	if (!p) {
		perror("malloc");
		exit(1);
	}

	p->folder = folder;
	p->dirfd = dirfd;
	p->uidl = NULL;

	return p;
}

static
int dovecot_imap_is_subscribed(void* _p, const char* fldrname)
{
	struct dovecot_data *p = _p;
	char linebuf[2048];

	// all subfolders in courier starts with INBOX.

	int fd = openat(p->dirfd, "subscriptions", O_RDONLY, 0);
	if (fd < 0) {
		fprintf(stderr, "%s/%s: %s\n", p->folder, "subscriptions", strerror(errno));
		return 0; /* best guess */
	}

	FILE* fp = fdopen(fd, "r");
	if (!fp) {
		fprintf(stderr, "%s/%s: %s\n", p->folder, "subscriptions", strerror(errno));
		return 0; /* best guess */
	}

	while (fgets(linebuf, sizeof(linebuf), fp)) {
		char *nl = strchr(linebuf, '\n');
		if (nl)
			*nl = 0;
		if (strcmp(fldrname, linebuf) == 0)
			return 1;
	}

	return 0;
}

static
void dovecot_imap_subscribe(void* _p, const char* fldrname)
{
	struct dovecot_data *p = _p;
	char tmpfname[1024];
	struct stat st;
	mode_t mode = 0644;
	int tfd, stvalid = 0, r;
	int sfd = openat(p->dirfd, "subscriptions", O_RDONLY, 0);
	if (sfd < 0 && errno != ENOENT) {
			/* ENOENT - no subscriptions, so no harm in creating */
			fprintf(stderr, "%s/%s: %s\n", p->folder, "subscriptions", strerror(errno));
			return;
	}

	if ((sfd >= 0 && fstat(sfd, &st) == 0) || fstat(p->dirfd, &st) == 0) {
		stvalid = 1;
		mode = st.st_mode & 0666;
	}

	do {
		sprintf(tmpfname, "tmp/dovecot-subscriptions-%d", rand());
	} while ((tfd = openat(p->dirfd, tmpfname, O_WRONLY | O_CREAT | O_EXCL, mode)) < 0 && errno == EEXIST);

	if (tfd < 0) {
		fprintf(stderr, "%s/%s: %s\n", p->folder, tmpfname, strerror(errno));
		if (sfd >= 0)
			close(sfd);
		return;
	}

	if (stvalid && geteuid() == 0)
		fchown(tfd, st.st_uid, st.st_gid);

	if (sfd >= 0) {
		while ((r = sendfile(tfd, sfd, NULL, 16384)) > 0);
		if (r < 0) {
			fprintf(stderr, "%s/%s: %s\n", p->folder, tmpfname, strerror(errno));
			close(sfd);
			close(tfd);
			unlinkat(p->dirfd, tmpfname, 0);
			return;
		}
		close(sfd);
	}

	write(tfd, fldrname, strlen(fldrname));
	write(tfd, "\n", 1);
	close(tfd);

	if (renameat(p->dirfd, tmpfname, p->dirfd, "subscriptions") < 0) {
		fprintf(stderr, "%s => %s/subscriptions: %s\n", tmpfname, p->folder,
				strerror(errno));
		unlinkat(p->dirfd, tmpfname, 0);
	}
}

static
void dovecot_close(void* _p)
{
	free(_p);
}

static
int dovecot_is_pop3(void*)
{
	return 0; /* if we deal with IMAP, POP3 will follow suit */
}

static struct maildir_type maildir_courier = {
	.label = "Dovecot",
	.buglist = root_maildirfolder,
	.detect = dovecot_detect,
	.metafiles = dovecot_metafiles,
	.open = dovecot_open,
	.is_pop3 = dovecot_is_pop3,
	.imap_is_subscribed = dovecot_imap_is_subscribed,
	.imap_subscribe = dovecot_imap_subscribe,
	.close = dovecot_close,
};

static void __attribute__((constructor)) _open_courier()
{
	register_maildir_type(&maildir_courier);
}
