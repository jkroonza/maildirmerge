#include "servertypes.h"
#include "uidl.h"

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/sendfile.h>

struct courier_data {
	const char* folder;
	int dirfd;

	struct uidl_list *uidl;
	char *uidl_headline;
};

struct courier_uidl_extra {
	char *flags;
	int utf8;
	int size;
};

static
int courier_detect(const char* folder)
{
	int detected;
	struct stat dummy;

	int dirfd = open(folder, O_RDONLY);
	if (dirfd < 0) {
		perror(folder);
		exit(1);
	}

	detected = fstatat(dirfd, "courierimapuiddb", &dummy, 0) == 0
		|| fstatat(dirfd, "courierpop3dsizelist", &dummy, 0) == 0;

	close(dirfd);
	return detected;
}

static
void* courier_open(const char* folder, int dirfd)
{
	struct courier_data *p = malloc(sizeof(struct courier_data));
	if (!p) {
		perror("malloc");
		exit(1);
	}

	p->folder = folder;
	p->dirfd = dirfd;
	p->uidl = NULL;

	return p;
}

//static
//char* courier_pop3_get_uidl(void *_p)
//{
//	struct courier_data *p = _p;
//	int fd = openat(p->dirfd, "courierpop3dsizelist");
//
//	return NULL;
//}

static
int courier_imap_is_subscribed(void* _p, const char* fldrname)
{
	struct courier_data *p = _p;
	char linebuf[2048];

	// all subfolders in courier starts with INBOX.

	int fd = openat(p->dirfd, "courierimapsubscribed", O_RDONLY, 0);
	if (fd < 0) {
		fprintf(stderr, "%s/%s: %s\n", p->folder, "courierimapsubscribed", strerror(errno));
		return 0; /* best guess */
	}

	FILE* fp = fdopen(fd, "r");
	if (!fp) {
		fprintf(stderr, "%s/%s: %s\n", p->folder, "courierimapsubscribed", strerror(errno));
		return 0; /* best guess */
	}

	while (fgets(linebuf, sizeof(linebuf), fp)) {
		char *nl = strchr(linebuf, '\n');
		if (nl)
			*nl = 0;
		if (strncmp("INBOX", linebuf, strlen("INBOX")))
			continue;
		if (strcmp(fldrname, linebuf + strlen("INBOX")))
			continue;
		return 1;
	}

	return 0;
}

static
void courier_imap_subscribe(void* _p, const char* fldrname)
{
	struct courier_data *p = _p;
	char tmpfname[1024];
	struct stat st;
	mode_t mode = 0644;
	int tfd, stvalid = 0, r;
	int sfd = openat(p->dirfd, "courierimapsubscribed", O_RDONLY, 0);
	if (sfd < 0 && errno != ENOENT) {
			/* ENOENT - no subscriptions, so no harm in creating */
			fprintf(stderr, "%s/%s: %s\n", p->folder, "courierimapsubscribed", strerror(errno));
			return;
	}

	if ((sfd >= 0 && fstat(sfd, &st) == 0) || fstat(p->dirfd, &st)) {
		stvalid = 1;
		mode = st.st_mode & 0666;
	}

	do {
		sprintf(tmpfname, "tmp/maildirmerge-courier-%d", rand());
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

	write(tfd, "INBOX", strlen("INBOX"));
	write(tfd, fldrname, strlen(fldrname));
	write(tfd, "\n", 1);
	close(tfd);

	if (renameat(p->dirfd, tmpfname, p->dirfd, "courierimapsubscribed") < 0) {
		fprintf(stderr, "%s => %s/courierimapsubscribed: %s\n", tmpfname, p->folder,
				strerror(errno));
		unlinkat(p->dirfd, tmpfname, 0);
	}
}

static
void courier_close(void* _p)
{
	free(_p);
}

static
int courier_is_pop3(void* _p)
{
	struct stat st;
	struct courier_data *p = _p;

	return fstatat(p->dirfd, "courierpop3dsizelist", &st, 0) == 0;
}

static struct maildir_type maildir_courier = {
	.label = "Courier-IMAP",
	.detect = courier_detect,
	.open = courier_open,
	.is_pop3 = courier_is_pop3,
//	.pop3_get_uidl = courier_pop3_get_uidl,
//	.pop3_set_uidl = courier_pop3_set_uidl,
	.imap_is_subscribed = courier_imap_is_subscribed,
	.imap_subscribe = courier_imap_subscribe,
	.close = courier_close,
};

static void __attribute__((constructor)) _open_courier()
{
	register_maildir_type(&maildir_courier);
}
