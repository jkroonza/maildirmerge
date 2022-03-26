#include "servertypes.h"

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

struct courier_data {
	const char* folder;
	int dirfd;
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

	return p;
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

static
void courier_lock(void* _p)
{
	struct courier_data *p = _p;


}

static struct maildir_type maildir_courier = {
	.label = "Courier-IMAP",
	.detect = courier_detect,
	.open = courier_open,
	.is_pop3 = courier_is_pop3,
	.close = courier_close,
};

static void __attribute__((constructor)) _open_courier()
{
	register_maildir_type(&maildir_courier);
}
