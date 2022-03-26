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

	/* must allocate the string, will be free'd with free,
	 * return NULL if no UIDL available */
	char* (*pop3_get_uidl)(void *pvt, const char* basename);

	/** set the POP3 UIDL value
	 */
	void (*pop3_set_uidl)(void* pvt, const char* basename, const char* uidl);

	/** fldrname is as per directory, so may need to prefix with eg INBOX. as per rquirement */
	int (*imap_is_subscribed)(void* pvt, const char* fldrname);
	void (*imap_subscribe)(void* pvt, const char* fldrname);

	/** close the structure, fee stuff */
	void (*close)(void* pvt);
};

void register_maildir_type(const struct maildir_type *mt);
#endif
