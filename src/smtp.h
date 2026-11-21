#ifndef VITAMAIL_SMTP_H
#define VITAMAIL_SMTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SmtpSendRequest {
	const char *host;
	uint16_t port;
	int use_starttls; /* 0 = implicit TLS (465), 1 = STARTTLS (587) */
	const char *user;
	const char *pass;
	const char *from;
	const char *to;
	const char *subject;
	const char *body;
	const char *in_reply_to; /* optional Message-ID */
} SmtpSendRequest;

/* Connect, authenticate, and send an email via SMTP. Returns 0 on success, -1 on error. */
int smtp_send_email(const SmtpSendRequest *req, char *errbuf, size_t errlen);

/* Format an RFC 5322 email with dot-stuffing for SMTP DATA (exposed for unit tests). */
size_t smtp_format_message(const SmtpSendRequest *req, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* VITAMAIL_SMTP_H */
