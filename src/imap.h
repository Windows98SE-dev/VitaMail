#ifndef VITAMAIL_IMAP_H
#define VITAMAIL_IMAP_H

#include <stddef.h>
#include <stdint.h>

#include "mime.h"
#include "net_tls.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMAP_MAX_FOLDERS 32
#define IMAP_MAX_HEADERS 50

typedef struct ImapFolder {
	char name[128];
} ImapFolder;

typedef struct ImapHeader {
	uint32_t uid;
	int seen;
	char subject[256];
	char from[160];
	char date[64];
} ImapHeader;

typedef struct ImapSession {
	NetTlsConn conn;
	int connected;
	int tag_seq;
	char selected_folder[128];
	uint32_t total_exists;
} ImapSession;

/* Connect and authenticate to IMAP server.
 * use_starttls: 0 = implicit TLS (port 993), 1 = STARTTLS (port 143). */
int imap_connect(ImapSession *sess, const char *host, uint16_t port, int use_starttls,
                 const char *user, const char *pass, char *errbuf, size_t errlen);

void imap_disconnect(ImapSession *sess);

/* List mailboxes (folders). Returns folder count >= 0, or -1 on error. */
int imap_list_folders(ImapSession *sess, ImapFolder *out, int max_folders,
                      char *errbuf, size_t errlen);

/* Select a folder (e.g. "INBOX"). Returns 0 on success, -1 on error. */
int imap_select_folder(ImapSession *sess, const char *folder,
                       char *errbuf, size_t errlen);

/* Fetch up to max_headers newest message headers from the currently selected folder.
 * Returns count >= 0, or -1 on error. */
int imap_fetch_headers(ImapSession *sess, ImapHeader *out, int max_headers,
                       char *errbuf, size_t errlen);

/* Fetch and parse full message by UID. Returns 0 on success, -1 on error. */
int imap_fetch_message(ImapSession *sess, uint32_t uid, MimeMessage *out,
                       char *errbuf, size_t errlen);

/* Mark message as Seen/Unseen. */
int imap_set_seen(ImapSession *sess, uint32_t uid, int seen,
                  char *errbuf, size_t errlen);

/* Delete message (sets \Deleted and EXPUNGE). */
int imap_delete_message(ImapSession *sess, uint32_t uid,
                        char *errbuf, size_t errlen);

/* Parsing helpers (exposed for unit testing) */
int imap_parse_literal_size(const char *line, size_t *out_size);
int imap_parse_search_uids(const char *line, uint32_t *uids, int max_uids);

#ifdef __cplusplus
}
#endif

#endif /* VITAMAIL_IMAP_H */
