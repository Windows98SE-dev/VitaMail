								#include "smtp.h"
#include "mime.h"
#include "net_tls.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *guess_mime_type(const char *filename)
{
	if (!filename)
		return "application/octet-stream";
	const char *dot = strrchr(filename, '.');
	if (!dot)
		return "application/octet-stream";
	char ext[16];
	size_t i = 0;
	for (const char *p = dot + 1; *p && i + 1 < sizeof(ext); p++)
		ext[i++] = (char)tolower((unsigned char)*p);
	ext[i] = '\0';

	if (strcmp(ext, "png") == 0) return "image/png";
	if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0) return "image/jpeg";
	if (strcmp(ext, "gif") == 0) return "image/gif";
	if (strcmp(ext, "bmp") == 0) return "image/bmp";
	if (strcmp(ext, "webp") == 0) return "image/webp";
	if (strcmp(ext, "txt") == 0 || strcmp(ext, "log") == 0) return "text/plain";
	if (strcmp(ext, "pdf") == 0) return "application/pdf";
	if (strcmp(ext, "zip") == 0) return "application/zip";
	return "application/octet-stream";
}

static int smtp_read_reply(NetTlsConn *conn, int expected_code, char *errbuf, size_t errlen)
{
	char line[1024];
	while (1) {
		if (net_tls_recv_line(conn, line, sizeof(line), errbuf, errlen) < 0)
			return -1;
		if (strlen(line) < 3) {
			snprintf(errbuf, errlen, "Malformed SMTP reply: %s", line);
			return -1;
		}
		int code = atoi(line);
		/* Multiline SMTP reply has '-' at index 3, final line has ' ' or '\0' */
		if (line[3] == '-')
			continue;
		if (code != expected_code) {
			snprintf(errbuf, errlen, "SMTP error %d: %s", code, line);
			return -1;
		}
		return 0;
	}
}

size_t smtp_format_message(const SmtpSendRequest *req, char *out, size_t out_cap)
{
	time_t now = time(NULL);
	struct tm tm_utc;
#ifdef _WIN32
	gmtime_s(&tm_utc, &now);
#else
	gmtime_r(&now, &tm_utc);
#endif
	char date_buf[64];
	strftime(date_buf, sizeof(date_buf), "%a, %d %b %Y %H:%M:%S +0000", &tm_utc);

	int has_att = (req->attachment_data != NULL && req->attachment_len > 0 &&
	               req->attachment_name != NULL && req->attachment_name[0] != '\0');
	const char *bnd = "----=_VitaMail_Part_7F3A9C21";

	size_t o = 0;
	o += (size_t)snprintf(out + o, (o < out_cap) ? out_cap - o : 0,
	                      "From: %s\r\n"
	                      "To: %s\r\n"
	                      "Subject: %s\r\n"
	                      "Date: %s\r\n"
	                      "Message-ID: <%lu.vitamail@psvita>\r\n"
	                      "MIME-Version: 1.0\r\n",
	                      req->from, req->to, req->subject ? req->subject : "(No Subject)",
	                      date_buf, (unsigned long)now);

	if (req->in_reply_to && req->in_reply_to[0] != '\0' && o < out_cap) {
		o += (size_t)snprintf(out + o, out_cap - o,
		                      "In-Reply-To: %s\r\nReferences: %s\r\n",
		                      req->in_reply_to, req->in_reply_to);
	}

	if (has_att) {
		o += (size_t)snprintf(out + o, (o < out_cap) ? out_cap - o : 0,
		                      "Content-Type: multipart/mixed; boundary=\"%s\"\r\n\r\n"
		                      "--%s\r\n"
		                      "Content-Type: text/plain; charset=UTF-8\r\n"
		                      "Content-Transfer-Encoding: 8bit\r\n\r\n",
		                      bnd, bnd);
	} else {
		o += (size_t)snprintf(out + o, (o < out_cap) ? out_cap - o : 0,
		                      "Content-Type: text/plain; charset=UTF-8\r\n"
		                      "Content-Transfer-Encoding: 8bit\r\n\r\n");
	}

	/* Body with CRLF normalization and SMTP dot-stuffing */
	const char *b = req->body ? req->body : "";
	int at_line_start = 1;
	for (const char *p = b; *p && o + 8 < out_cap; p++) {
		if (at_line_start && *p == '.') {
			out[o++] = '.';
		}
		if (*p == '\r') {
			continue;
		} else if (*p == '\n') {
			out[o++] = '\r';
			out[o++] = '\n';
			at_line_start = 1;
		} else {
			out[o++] = *p;
			at_line_start = 0;
		}
	}

	if (!at_line_start && o + 2 < out_cap) {
		out[o++] = '\r';
		out[o++] = '\n';
	}

	if (has_att && o < out_cap) {
		const char *mtype = guess_mime_type(req->attachment_name);
		o += (size_t)snprintf(out + o, out_cap - o,
		                      "\r\n--%s\r\n"
		                      "Content-Type: %s; name=\"%s\"\r\n"
		                      "Content-Disposition: attachment; filename=\"%s\"\r\n"
		                      "Content-Transfer-Encoding: base64\r\n\r\n",
		                      bnd, mtype, req->attachment_name, req->attachment_name);

		/* Encode attachment in 57-byte chunks -> 76 Base64 chars per CRLF line */
		for (size_t off = 0; off < req->attachment_len && o + 82 < out_cap; off += 57) {
			size_t chunk = req->attachment_len - off;
			if (chunk > 57)
				chunk = 57;
			char line_b64[80];
			size_t blen = mime_base64_encode(req->attachment_data + off, chunk,
			                                 line_b64, sizeof(line_b64));
			if (o + blen + 2 >= out_cap)
				break;
			memcpy(out + o, line_b64, blen);
			o += blen;
			out[o++] = '\r';
			out[o++] = '\n';
		}

		o += (size_t)snprintf(out + o, (o < out_cap) ? out_cap - o : 0,
		                      "--%s--\r\n", bnd);
	}

	if (o + 3 < out_cap) {
		out[o++] = '.';
		out[o++] = '\r';
		out[o++] = '\n';
	}
	if (out_cap > 0)
		out[o < out_cap ? o : out_cap - 1] = '\0';
	return o;
}

int smtp_send_email(const SmtpSendRequest *req, char *errbuf, size_t errlen)
{
	NetTlsConn conn;
	if (net_tls_connect(&conn, req->host, req->port, req->use_starttls ? 0 : 1, errbuf, errlen) != 0)
		return -1;

	if (smtp_read_reply(&conn, 220, errbuf, errlen) != 0) {
		net_tls_close(&conn);
		return -1;
	}

	if (net_tls_sendf(&conn, errbuf, errlen, "EHLO vitamail.local\r\n") != 0 ||
	    smtp_read_reply(&conn, 250, errbuf, errlen) != 0) {
		net_tls_close(&conn);
		return -1;
	}

	if (req->use_starttls) {
		if (net_tls_sendf(&conn, errbuf, errlen, "STARTTLS\r\n") != 0 ||
		    smtp_read_reply(&conn, 220, errbuf, errlen) != 0 ||
		    net_tls_upgrade(&conn, req->host, errbuf, errlen) != 0) {
			net_tls_close(&conn);
			return -1;
		}
		if (net_tls_sendf(&conn, errbuf, errlen, "EHLO vitamail.local\r\n") != 0 ||
		    smtp_read_reply(&conn, 250, errbuf, errlen) != 0) {
			net_tls_close(&conn);
			return -1;
		}
	}

	/* AUTH LOGIN */
	char b64_user[512], b64_pass[512];
	mime_base64_encode((const unsigned char *)req->user, strlen(req->user), b64_user, sizeof(b64_user));
	mime_base64_encode((const unsigned char *)req->pass, strlen(req->pass), b64_pass, sizeof(b64_pass));

	if (net_tls_sendf(&conn, errbuf, errlen, "AUTH LOGIN\r\n") != 0 ||
	    smtp_read_reply(&conn, 334, errbuf, errlen) != 0 ||
	    net_tls_sendf(&conn, errbuf, errlen, "%s\r\n", b64_user) != 0 ||
	    smtp_read_reply(&conn, 334, errbuf, errlen) != 0 ||
	    net_tls_sendf(&conn, errbuf, errlen, "%s\r\n", b64_pass) != 0 ||
	    smtp_read_reply(&conn, 235, errbuf, errlen) != 0) {
		net_tls_close(&conn);
		return -1;
	}

	if (net_tls_sendf(&conn, errbuf, errlen, "MAIL FROM:<%s>\r\n", req->from) != 0 ||
	    smtp_read_reply(&conn, 250, errbuf, errlen) != 0 ||
	    net_tls_sendf(&conn, errbuf, errlen, "RCPT TO:<%s>\r\n", req->to) != 0 ||
	    smtp_read_reply(&conn, 250, errbuf, errlen) != 0 ||
	    net_tls_sendf(&conn, errbuf, errlen, "DATA\r\n") != 0 ||
	    smtp_read_reply(&conn, 354, errbuf, errlen) != 0) {
		net_tls_close(&conn);
		return -1;
	}

	size_t body_len = req->body ? strlen(req->body) : 0;
	size_t att_b64_len = (req->attachment_len > 0) ? ((req->attachment_len * 4) / 3 + (req->attachment_len / 57) * 2 + 1024) : 0;
	size_t msg_cap = body_len * 2 + att_b64_len + 2048;
	char *msg = (char *)malloc(msg_cap);
	if (!msg) {
		snprintf(errbuf, errlen, "Out of memory");
		net_tls_close(&conn);
		return -1;
	}
	size_t msg_len = smtp_format_message(req, msg, msg_cap);
	int rc = net_tls_send_all(&conn, msg, msg_len, errbuf, errlen);
	free(msg);

	if (rc != 0 || smtp_read_reply(&conn, 250, errbuf, errlen) != 0) {
		net_tls_close(&conn);
		return -1;
	}

	char dummy[64];
	net_tls_sendf(&conn, dummy, sizeof(dummy), "QUIT\r\n");
	net_tls_close(&conn);
	return 0;
}
