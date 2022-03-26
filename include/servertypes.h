#ifndef __SERVERTYPES_H__
#define __SERVERTYPES_H__

struct maildir_type {
	const char* label;

	/** returns non-zero if the given path is detected as being of this type */
	int (*detect)(const char* folder);

	/** returns a point to pvt data to be used further on,
	 * may return NULL, must exit() on failure, return will
	 * be passed as pvt in future calls. */
	void* (*open)(const char* folder, int dirfd);

	/** returns non-zero if the target folder is known to service
	 * a POP3 client */
	int (*is_pop3)(void* pvt);

	/** close the structure, fee stuff */
	void (*close)(void* pvt);
};

void register_maildir_type(const struct maildir_type *mt);
#endif
