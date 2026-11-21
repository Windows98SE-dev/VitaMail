#include "imap.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void imap_next_tag(ImapSession *sess, char *tag, size_t cap)
{
	sess->tag_seq++;
	snprintf(tag, cap, "A%04d", sess->tag_seq);
}

static void imap_quote_str(const char *in, char *out, size_t cap)
{
	size_t o = 0;
	if (o + 1 < cap)
		out[o++] = '"';
	for (const char *p = in; *p && o + 3 < cap; p++) {
		if (*p == '"' || *p == '\\')
			out[o++] = '\\';
		out[o++] = *p;
	}
	if (o + 1 < cap)
		out[o++] = '"';
	out[o] = '\0';
}

int imap_parse_literal_size(const char *line, size_t *out_size)
{
	if (!line)
		return 0;
	size_t len = strlen(line);
	if (len < 3 || line[len - 1] != '}')
		return 0;
	const char *lb = strrchr(line, '{');
	if (!lb)
		return 0;
	char *endp = NULL;
	unsigned long sz = strtoul(lb + 1, &endp, 10);
	if (endp == lb + 1 || *endp != '}')
		return 0;
	*out_size = (size_t)sz;
	return 1;
}

int imap_parse_search_uids(const char *line, uint32_t *uids, int max_uids)
{
	const char *p = strstr(line, "SEARCH");
	if (!p)
		return 0;
	p += 6;
	int count = 0;
	while (*p && count < max_uids) {
		while (*p && !isdigit((unsigned char)*p))
			p++;
		if (!*p)
			break;
		char *endp = NULL;
		unsigned long v = strtoul(p, &endp, 10);
		if (endp == p)
			break;
		uids[count++] = (uint32_t)v;
		p = endp;
	}
	return count;
}

/* Read tagged completion line; ignores untagged lines (skipping any literals). */
static int imap_wait_tagged(ImapSession *sess, const char *tag, char *errbuf, size_t errlen)
{
	char line[2048];
	size_t taglen = strlen(tag);

	while (1) {
		if (net_tls_recv_line(&sess->conn, line, sizeof(line), errbuf, errlen) < 0)
			return -1;

		/* Skip any literal payload on untagged lines */
		size_t lit_sz = 0;
		if (imap_parse_literal_size(line, &lit_sz) && lit_sz > 0) {
			if (lit_sz <= 8 * 1024 * 1024) {
				char *discard = (char *)malloc(lit_sz);
				if (discard) {
					int rc = net_tls_recv_exact(&sess->conn, discard, lit_sz, errbuf, errlen);
					free(discard);
					if (rc != 0)
						return -1;
				}
			}
			continue;
		}

		if (strncmp(line, tag, taglen) == 0 && line[taglen] == ' ') {
			if (strncmp(line + taglen + 1, "OK", 2) == 0)
				return 0;
			snprintf(errbuf, errlen, "IMAP error: %s", line + taglen + 1);
			return -1;
		}
	}
}

int imap_connect(ImapSession *sess, const char *host, uint16_t port, int use_starttls,
                 const char *user, const char *pass, char *errbuf, size_t errlen)
{
	memset(sess, 0, sizeof(*sess));
	sess->conn.sock = -1;

	if (net_tls_connect(&sess->conn, host, port, use_starttls ? 0 : 1, errbuf, errlen) != 0)
		return -1;

	char line[1024];
	if (net_tls_recv_line(&sess->conn, line, sizeof(line), errbuf, errlen) < 0) {
		net_tls_close(&sess->conn);
		return -1;
	}
	if (strncmp(line, "* OK", 4) != 0 && strncmp(line, "* PREAUTH", 9) != 0) {
		snprintf(errbuf, errlen, "Unexpected IMAP greeting: %s", line);
		net_tls_close(&sess->conn);
		return -1;
	}

	char tag[16];
	if (use_starttls) {
		imap_next_tag(sess, tag, sizeof(tag));
		if (net_tls_sendf(&sess->conn, errbuf, errlen, "%s STARTTLS\r\n", tag) != 0 ||
		    imap_wait_tagged(sess, tag, errbuf, errlen) != 0) {
			net_tls_close(&sess->conn);
			return -1;
		}
		if (net_tls_upgrade(&sess->conn, host, errbuf, errlen) != 0) {
			net_tls_close(&sess->conn);
			return -1;
		}
	}

	char q_user[512], q_pass[512];
	imap_quote_str(user, q_user, sizeof(q_user));
	imap_quote_str(pass, q_pass, sizeof(q_pass));

	imap_next_tag(sess, tag, sizeof(tag));
	if (net_tls_sendf(&sess->conn, errbuf, errlen, "%s LOGIN %s %s\r\n", tag, q_user, q_pass) != 0 ||
	    imap_wait_tagged(sess, tag, errbuf, errlen) != 0) {
		net_tls_close(&sess->conn);
		return -1;
	}

	sess->connected = 1;
	return 0;
}

void imap_disconnect(ImapSession *sess)
{
	if (!sess)
		return;
	if (sess->connected) {
		char tag[16], dummy[64];
		imap_next_tag(sess, tag, sizeof(tag));
		net_tls_sendf(&sess->conn, dummy, sizeof(dummy), "%s LOGOUT\r\n", tag);
	}
	net_tls_close(&sess->conn);
	sess->connected = 0;
}

int imap_list_folders(ImapSession *sess, ImapFolder *out, int max_folders,
                      char *errbuf, size_t errlen)
{
	char tag[16], line[1024];
	imap_next_tag(sess, tag, sizeof(tag));
	if (net_tls_sendf(&sess->conn, errbuf, errlen, "%s LIST \"\" \"*\"\r\n", tag) != 0)
		return -1;

	int count = 0;
	size_t taglen = strlen(tag);
	while (1) {
		if (net_tls_recv_line(&sess->conn, line, sizeof(line), errbuf, errlen) < 0)
			return -1;
		if (strncmp(line, tag, taglen) == 0 && line[taglen] == ' ') {
			if (strncmp(line + taglen + 1, "OK", 2) == 0)
				return count;
			snprintf(errbuf, errlen, "IMAP LIST failed: %s", line + taglen + 1);
			return -1;
		}
		if (strncmp(line, "* LIST ", 7) == 0 && count < max_folders) {
			/* Format: * LIST (\Flags) "/" "FolderName" or * LIST (\Flags) "/" INBOX */
			char *last_q = strrchr(line, '"');
			if (last_q && last_q > line + 7 && *(last_q - 1) != '\\') {
				*last_q = '\0';
				char *first_q = strrchr(line, '"');
				if (first_q) {
					snprintf(out[count].name, sizeof(out[count].name), "%s", first_q + 1);
					count++;
					continue;
				}
			}
			/* Unquoted folder name at end */
			char *sp = strrchr(line, ' ');
			if (sp && sp[1] != '\0') {
				snprintf(out[count].name, sizeof(out[count].name), "%s", sp + 1);
				count++;
			}
		}
	}
}

int imap_select_folder(ImapSession *sess, const char *folder,
                       char *errbuf, size_t errlen)
{
	char tag[16], q_folder[256], line[1024];
	imap_quote_str(folder, q_folder, sizeof(q_folder));
	imap_next_tag(sess, tag, sizeof(tag));

	if (net_tls_sendf(&sess->conn, errbuf, errlen, "%s SELECT %s\r\n", tag, q_folder) != 0)
		return -1;

	sess->total_exists = 0;
	size_t taglen = strlen(tag);
	while (1) {
		if (net_tls_recv_line(&sess->conn, line, sizeof(line), errbuf, errlen) < 0)
			return -1;
		if (strncmp(line, tag, taglen) == 0 && line[taglen] == ' ') {
			if (strncmp(line + taglen + 1, "OK", 2) == 0) {
				snprintf(sess->selected_folder, sizeof(sess->selected_folder), "%s", folder);
				return 0;
			}
			snprintf(errbuf, errlen, "IMAP SELECT failed: %s", line + taglen + 1);
			return -1;
		}
		if (line[0] == '*' && strstr(line, " EXISTS") != NULL) {
			sess->total_exists = (uint32_t)strtoul(line + 2, NULL, 10);
		}
	}
}

static void parse_header_fields_block(const char *hdr, size_t len, ImapHeader *h)
{
	MimeMessage tmp;
	/* Reuse mime_parse_message on header-only block to decode From/Subject/Date */
	char *buf = (char *)malloc(len + 5);
	if (!buf)
		return;
	memcpy(buf, hdr, len);
	memcpy(buf + len, "\r\n\r\n", 5);
	if (mime_parse_message(buf, len + 4, &tmp) == 0) {
		snprintf(h->subject, sizeof(h->subject), "%.255s", tmp.subject);
		snprintf(h->from, sizeof(h->from), "%.159s", tmp.from);
		snprintf(h->date, sizeof(h->date), "%.63s", tmp.date);
		mime_message_free(&tmp);
	}
	free(buf);
}

int imap_fetch_headers(ImapSession *sess, ImapHeader *out, int max_headers,
                       char *errbuf, size_t errlen)
{
	if (max_headers <= 0)
		return 0;

	/* 1. UID SEARCH ALL */
	char tag[16], line[4096];
	imap_next_tag(sess, tag, sizeof(tag));
	if (net_tls_sendf(&sess->conn, errbuf, errlen, "%s UID SEARCH ALL\r\n", tag) != 0)
		return -1;

	uint32_t *all_uids = (uint32_t *)malloc(sizeof(uint32_t) * 4096);
	if (!all_uids) {
		snprintf(errbuf, errlen, "Out of memory");
		return -1;
	}
	int total_uids = 0;
	size_t taglen = strlen(tag);

	while (1) {
		if (net_tls_recv_line(&sess->conn, line, sizeof(line), errbuf, errlen) < 0) {
			free(all_uids);
			return -1;
		}
		if (strncmp(line, tag, taglen) == 0 && line[taglen] == ' ') {
			if (strncmp(line + taglen + 1, "OK", 2) != 0) {
				snprintf(errbuf, errlen, "IMAP UID SEARCH failed: %s", line + taglen + 1);
				free(all_uids);
				return -1;
			}
			break;
		}
		if (strncmp(line, "* SEARCH", 8) == 0) {
			total_uids = imap_parse_search_uids(line, all_uids, 4096);
		}
	}

	if (total_uids == 0) {
		free(all_uids);
		return 0;
	}

	/* Take up to max_headers newest UIDs (at the end of the list) */
	int take = (total_uids < max_headers) ? total_uids : max_headers;
	int start_idx = total_uids - take;

	/* Build comma-separated UID set in reverse order? IMAP returns ascending, we reverse in out[] */
	char *uid_set = (char *)malloc((size_t)take * 12 + 16);
	if (!uid_set) {
		free(all_uids);
		return -1;
	}
	size_t uoff = 0;
	for (int i = start_idx; i < total_uids; i++) {
		uoff += (size_t)snprintf(uid_set + uoff, 16, "%s%u",
		                         (i > start_idx) ? "," : "", all_uids[i]);
	}
	free(all_uids);

	/* 2. UID FETCH <uid_set> (UID FLAGS BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)]) */
	imap_next_tag(sess, tag, sizeof(tag));
	if (net_tls_sendf(&sess->conn, errbuf, errlen,
	                  "%s UID FETCH %s (UID FLAGS BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)])\r\n",
	                  tag, uid_set) != 0) {
		free(uid_set);
		return -1;
	}
	free(uid_set);

	int count = 0;
	taglen = strlen(tag);
	while (1) {
		if (net_tls_recv_line(&sess->conn, line, sizeof(line), errbuf, errlen) < 0)
			return -1;

		if (strncmp(line, tag, taglen) == 0 && line[taglen] == ' ') {
			if (strncmp(line + taglen + 1, "OK", 2) == 0)
				break;
			snprintf(errbuf, errlen, "IMAP FETCH headers failed: %s", line + taglen + 1);
			return -1;
		}

		if (line[0] == '*' && strstr(line, " FETCH ") != NULL && count < max_headers) {
			ImapHeader h;
			memset(&h, 0, sizeof(h));

			const char *uidp = strstr(line, "UID ");
			if (uidp)
				h.uid = (uint32_t)strtoul(uidp + 4, NULL, 10);
			if (strstr(line, "\\Seen") != NULL)
				h.seen = 1;

			size_t lit_sz = 0;
			if (imap_parse_literal_size(line, &lit_sz) && lit_sz > 0 && lit_sz <= 65536) {
				char *hdr_buf = (char *)malloc(lit_sz + 1);
				if (hdr_buf) {
					if (net_tls_recv_exact(&sess->conn, hdr_buf, lit_sz, errbuf, errlen) != 0) {
						free(hdr_buf);
						return -1;
					}
					hdr_buf[lit_sz] = '\0';
					parse_header_fields_block(hdr_buf, lit_sz, &h);
					free(hdr_buf);
				}
				/* Consume trailing ')' line after literal */
				char tail[256];
				net_tls_recv_line(&sess->conn, tail, sizeof(tail), errbuf, errlen);
			}
			if (h.uid != 0)
				out[count++] = h;
		}
	}

	/* Reverse so newest email is at index 0 */
	for (int i = 0; i < count / 2; i++) {
		ImapHeader tmp = out[i];
		out[i] = out[count - 1 - i];
		out[count - 1 - i] = tmp;
	}
	return count;
}

int imap_fetch_message(ImapSession *sess, uint32_t uid, MimeMessage *out,
                       char *errbuf, size_t errlen)
{
	char tag[16], line[2048];
	imap_next_tag(sess, tag, sizeof(tag));
	if (net_tls_sendf(&sess->conn, errbuf, errlen,
	                  "%s UID FETCH %u BODY.PEEK[]\r\n", tag, uid) != 0)
		return -1;

	int parsed = 0;
	size_t taglen = strlen(tag);
	while (1) {
		if (net_tls_recv_line(&sess->conn, line, sizeof(line), errbuf, errlen) < 0)
			return -1;
		if (strncmp(line, tag, taglen) == 0 && line[taglen] == ' ') {
			if (strncmp(line + taglen + 1, "OK", 2) == 0)
				return parsed ? 0 : -1;
			snprintf(errbuf, errlen, "IMAP FETCH body failed: %s", line + taglen + 1);
			return -1;
		}
		size_t lit_sz = 0;
		if (line[0] == '*' && imap_parse_literal_size(line, &lit_sz)) {
			if (lit_sz > 8 * 1024 * 1024) {
				snprintf(errbuf, errlen, "Message too large (%zu bytes)", lit_sz);
				return -1;
			}
			char *raw = (char *)malloc(lit_sz + 1);
			if (!raw) {
				snprintf(errbuf, errlen, "Out of memory for message (%zu bytes)", lit_sz);
				return -1;
			}
			if (net_tls_recv_exact(&sess->conn, raw, lit_sz, errbuf, errlen) != 0) {
				free(raw);
				return -1;
			}
			raw[lit_sz] = '\0';
			mime_parse_message(raw, lit_sz, out);
			free(raw);
			parsed = 1;

			/* Consume closing ')' line */
			char tail[256];
			net_tls_recv_line(&sess->conn, tail, sizeof(tail), errbuf, errlen);
		}
	}
}

int imap_set_seen(ImapSession *sess, uint32_t uid, int seen,
                  char *errbuf, size_t errlen)
{
	char tag[16];
	imap_next_tag(sess, tag, sizeof(tag));
	if (net_tls_sendf(&sess->conn, errbuf, errlen,
	                  "%s UID STORE %u %sFLAGS.SILENT (\\Seen)\r\n",
	                  tag, uid, seen ? "+" : "-") != 0)
		return -1;
	return imap_wait_tagged(sess, tag, errbuf, errlen);
}

int imap_delete_message(ImapSession *sess, uint32_t uid,
                        char *errbuf, size_t errlen)
{
	char tag[16];
	imap_next_tag(sess, tag, sizeof(tag));
	if (net_tls_sendf(&sess->conn, errbuf, errlen,
	                  "%s UID STORE %u +FLAGS.SILENT (\\Deleted)\r\n", tag, uid) != 0)
		return -1;
	if (imap_wait_tagged(sess, tag, errbuf, errlen) != 0)
		return -1;

	imap_next_tag(sess, tag, sizeof(tag));
	if (net_tls_sendf(&sess->conn, errbuf, errlen, "%s EXPUNGE\r\n", tag) != 0)
		return -1;
	return imap_wait_tagged(sess, tag, errbuf, errlen);
}
