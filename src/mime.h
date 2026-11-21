#ifndef VITAMAIL_MIME_H
#define VITAMAIL_MIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIME_MAX_ATTACHMENTS 16

typedef struct MimeAttachment {
	char filename[128];
	char mime_type[64];
	char content_id[128];
	unsigned char *data;
	size_t size;
} MimeAttachment;

typedef struct MimeMessage {
	char subject[512];
	char from[256];
	char to[256];
	char date[128];
	char message_id[256];
	char attachments_summary[512]; /* comma-separated list of attachment filenames */
	MimeAttachment attachments[MIME_MAX_ATTACHMENTS];
	int attachment_count;
	char *body;      /* heap-allocated UTF-8 plain text */
	char *html_body; /* heap-allocated UTF-8 HTML (NULL if none) */
} MimeMessage;

/* Free heap fields inside MimeMessage (does not free msg itself). */
void mime_message_free(MimeMessage *msg);

/* Decode RFC 2047 encoded-words (=?UTF-8?B?...?= / =?ISO-8859-1?Q?...?=) into UTF-8. */
void mime_decode_header(const char *in, char *out, size_t out_cap);

/* Base64 encode/decode helpers. */
size_t mime_base64_encode(const unsigned char *in, size_t in_len, char *out, size_t out_cap);
size_t mime_base64_decode(const char *in, size_t in_len, unsigned char *out, size_t out_cap);

/* Quoted-Printable decode. Returns decoded length. */
size_t mime_qp_decode(const char *in, size_t in_len, char *out, size_t out_cap);

/* Strip HTML tags & decode common HTML entities into readable UTF-8 plain text. */
void mime_html_to_text(const char *html, size_t html_len, char *out, size_t out_cap);

/* Parse a raw RFC 5322 / MIME email into MimeMessage. */
int mime_parse_message(const char *raw, size_t raw_len, MimeMessage *out);

#ifdef __cplusplus
}
#endif

#endif /* VITAMAIL_MIME_H */
