#include "mime.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ci_strncasecmp(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char ca = (unsigned char)tolower((unsigned char)a[i]);
		unsigned char cb = (unsigned char)tolower((unsigned char)b[i]);
		if (ca != cb)
			return (int)ca - (int)cb;
		if (ca == '\0')
			return 0;
	}
	return 0;
}

static const char *ci_strstr(const char *hay, const char *needle)
{
	size_t nlen = strlen(needle);
	if (nlen == 0)
		return hay;
	for (const char *p = hay; *p; p++) {
		if (ci_strncasecmp(p, needle, nlen) == 0)
			return p;
	}
	return NULL;
}

void mime_message_free(MimeMessage *msg)
{
	if (!msg)
		return;
	free(msg->body);
	msg->body = NULL;
}

size_t mime_base64_encode(const unsigned char *in, size_t in_len, char *out, size_t out_cap)
{
	static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t o = 0;
	for (size_t i = 0; i < in_len; i += 3) {
		unsigned int v = ((unsigned int)in[i]) << 16;
		if (i + 1 < in_len)
			v |= ((unsigned int)in[i + 1]) << 8;
		if (i + 2 < in_len)
			v |= ((unsigned int)in[i + 2]);

		if (o + 4 >= out_cap)
			break;
		out[o++] = tbl[(v >> 18) & 0x3F];
		out[o++] = tbl[(v >> 12) & 0x3F];
		out[o++] = (i + 1 < in_len) ? tbl[(v >> 6) & 0x3F] : '=';
		out[o++] = (i + 2 < in_len) ? tbl[v & 0x3F] : '=';
	}
	if (out_cap > 0)
		out[o < out_cap ? o : out_cap - 1] = '\0';
	return o;
}

static int b64_val(unsigned char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

size_t mime_base64_decode(const char *in, size_t in_len, unsigned char *out, size_t out_cap)
{
	size_t o = 0;
	unsigned int accum = 0;
	int bits = 0;
	for (size_t i = 0; i < in_len; i++) {
		unsigned char c = (unsigned char)in[i];
		if (c == '=')
			break;
		int v = b64_val(c);
		if (v < 0)
			continue;
		accum = (accum << 6) | (unsigned int)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (o < out_cap)
				out[o++] = (unsigned char)((accum >> bits) & 0xFF);
		}
	}
	if (out_cap > 0)
		out[o < out_cap ? o : out_cap - 1] = '\0';
	return o;
}

static int hex_val(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

size_t mime_qp_decode(const char *in, size_t in_len, char *out, size_t out_cap)
{
	size_t o = 0;
	for (size_t i = 0; i < in_len && o + 1 < out_cap; i++) {
		if (in[i] == '=') {
			if (i + 1 < in_len && in[i + 1] == '\r') {
				i++;
				if (i + 1 < in_len && in[i + 1] == '\n')
					i++;
				continue;
			}
			if (i + 1 < in_len && in[i + 1] == '\n') {
				i++;
				continue;
			}
			if (i + 2 < in_len) {
				int hi = hex_val(in[i + 1]);
				int lo = hex_val(in[i + 2]);
				if (hi >= 0 && lo >= 0) {
					out[o++] = (char)((hi << 4) | lo);
					i += 2;
					continue;
				}
			}
		}
		out[o++] = in[i];
	}
	if (out_cap > 0)
		out[o] = '\0';
	return o;
}

/* Convert ISO-8859-1 / Windows-1252 single-byte text to UTF-8 if needed. */
static void latin1_to_utf8(const unsigned char *in, size_t len, char *out, size_t out_cap)
{
	size_t o = 0;
	for (size_t i = 0; i < len && o + 3 < out_cap; i++) {
		unsigned char c = in[i];
		if (c < 0x80) {
			out[o++] = (char)c;
		} else {
			out[o++] = (char)(0xC0 | (c >> 6));
			out[o++] = (char)(0x80 | (c & 0x3F));
		}
	}
	if (out_cap > 0)
		out[o] = '\0';
}

void mime_decode_header(const char *in, char *out, size_t out_cap)
{
	if (!in || out_cap == 0) {
		if (out_cap > 0)
			out[0] = '\0';
		return;
	}

	size_t o = 0;
	const char *p = in;
	int prev_was_encoded = 0;

	while (*p && o + 1 < out_cap) {
		if (p[0] == '=' && p[1] == '?') {
			const char *q1 = strchr(p + 2, '?');
			const char *q2 = q1 ? strchr(q1 + 1, '?') : NULL;
			const char *end = q2 ? strstr(q2 + 1, "?=") : NULL;
			if (q1 && q2 && end && (q2 - q1 == 2)) {
				char enc = (char)toupper((unsigned char)q1[1]);
				int is_latin1 = (ci_strncasecmp(p + 2, "iso-8859-1", 10) == 0 ||
				                 ci_strncasecmp(p + 2, "windows-1252", 12) == 0);
				size_t payload_len = (size_t)(end - (q2 + 1));

				char *decoded = (char *)malloc(payload_len * 2 + 4);
				if (decoded) {
					size_t dlen = 0;
					if (enc == 'B') {
						dlen = mime_base64_decode(q2 + 1, payload_len,
						                          (unsigned char *)decoded,
						                          payload_len + 1);
					} else if (enc == 'Q') {
						/* In Q-encoding, '_' represents space (0x20) */
						char *tmp = (char *)malloc(payload_len + 1);
						if (tmp) {
							for (size_t k = 0; k < payload_len; k++)
								tmp[k] = (q2[1 + k] == '_') ? ' ' : q2[1 + k];
							tmp[payload_len] = '\0';
							dlen = mime_qp_decode(tmp, payload_len, decoded, payload_len + 1);
							free(tmp);
						}
					}

					if (is_latin1 && dlen > 0) {
						char *u8 = (char *)malloc(dlen * 2 + 1);
						if (u8) {
							latin1_to_utf8((const unsigned char *)decoded, dlen, u8, dlen * 2 + 1);
							size_t ul = strlen(u8);
							size_t copy = (ul < out_cap - 1 - o) ? ul : (out_cap - 1 - o);
							memcpy(out + o, u8, copy);
							o += copy;
							free(u8);
						}
					} else {
						size_t copy = (dlen < out_cap - 1 - o) ? dlen : (out_cap - 1 - o);
						memcpy(out + o, decoded, copy);
						o += copy;
					}
					free(decoded);
					p = end + 2;
					prev_was_encoded = 1;
					continue;
				}
			}
		}

		/* Collapse whitespace between adjacent encoded-words per RFC 2047 */
		if (prev_was_encoded && (*p == ' ' || *p == '\t')) {
			const char *ws = p;
			while (*ws == ' ' || *ws == '\t')
				ws++;
			if (ws[0] == '=' && ws[1] == '?') {
				p = ws;
				continue;
			}
		}
		prev_was_encoded = 0;
		out[o++] = *p++;
	}
	out[o] = '\0';
}

void mime_html_to_text(const char *html, size_t html_len, char *out, size_t out_cap)
{
	if (!html || out_cap == 0)
		return;
	size_t o = 0;
	int in_tag = 0;
	int skip_block = 0; /* inside <style> or <script> */
	int last_was_space = 0;

	for (size_t i = 0; i < html_len && o + 2 < out_cap; i++) {
		char c = html[i];
		if (c == '<') {
			if (ci_strncasecmp(html + i, "<style", 6) == 0 ||
			    ci_strncasecmp(html + i, "<script", 7) == 0) {
				skip_block = 1;
			} else if (ci_strncasecmp(html + i, "</style", 7) == 0 ||
			           ci_strncasecmp(html + i, "</script", 8) == 0) {
				skip_block = 0;
			} else if (!skip_block &&
			           (ci_strncasecmp(html + i, "<br", 3) == 0 ||
			            ci_strncasecmp(html + i, "<p", 2) == 0 ||
			            ci_strncasecmp(html + i, "</p", 3) == 0 ||
			            ci_strncasecmp(html + i, "<div", 4) == 0 ||
			            ci_strncasecmp(html + i, "<li", 3) == 0 ||
			            ci_strncasecmp(html + i, "<tr", 3) == 0)) {
				if (o > 0 && out[o - 1] != '\n')
					out[o++] = '\n';
				last_was_space = 1;
			}
			in_tag = 1;
			continue;
		}
		if (c == '>') {
			in_tag = 0;
			continue;
		}
		if (in_tag || skip_block)
			continue;

		if (c == '&') {
			if (ci_strncasecmp(html + i, "&amp;", 5) == 0) { out[o++] = '&'; i += 4; last_was_space = 0; continue; }
			if (ci_strncasecmp(html + i, "&lt;", 4) == 0) { out[o++] = '<'; i += 3; last_was_space = 0; continue; }
			if (ci_strncasecmp(html + i, "&gt;", 4) == 0) { out[o++] = '>'; i += 3; last_was_space = 0; continue; }
			if (ci_strncasecmp(html + i, "&quot;", 6) == 0) { out[o++] = '"'; i += 5; last_was_space = 0; continue; }
			if (ci_strncasecmp(html + i, "&#39;", 5) == 0) { out[o++] = '\''; i += 4; last_was_space = 0; continue; }
			if (ci_strncasecmp(html + i, "&nbsp;", 6) == 0) { out[o++] = ' '; i += 5; last_was_space = 1; continue; }
		}

		if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
			if (!last_was_space && o > 0) {
				out[o++] = ' ';
				last_was_space = 1;
			}
			continue;
		}
		out[o++] = c;
		last_was_space = 0;
	}
	out[o] = '\0';
}

static void extract_header_value(const char *hdrs, size_t hdr_len, const char *name,
                                 char *out, size_t out_cap)
{
	out[0] = '\0';
	size_t nlen = strlen(name);
	const char *p = hdrs;
	const char *end = hdrs + hdr_len;

	while (p < end) {
		const char *line_end = memchr(p, '\n', (size_t)(end - p));
		if (!line_end)
			line_end = end;

		if ((size_t)(line_end - p) > nlen + 1 &&
		    ci_strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
			const char *val = p + nlen + 1;
			while (val < line_end && (*val == ' ' || *val == '\t'))
				val++;

			char raw_val[1024];
			size_t w = 0;

			const char *cur = val;
			const char *cur_end = line_end;
			while (1) {
				size_t seg = (size_t)(cur_end - cur);
				while (seg > 0 && (cur[seg - 1] == '\r' || cur[seg - 1] == '\n'))
					seg--;
				if (w > 0 && w + 1 < sizeof(raw_val))
					raw_val[w++] = ' ';
				size_t cp = (seg < sizeof(raw_val) - 1 - w) ? seg : (sizeof(raw_val) - 1 - w);
				memcpy(raw_val + w, cur, cp);
				w += cp;

				/* Check folded continuation line */
				const char *next = (cur_end < end) ? cur_end + 1 : end;
				if (next < end && (*next == ' ' || *next == '\t')) {
					cur = next;
					while (cur < end && (*cur == ' ' || *cur == '\t'))
						cur++;
					cur_end = memchr(cur, '\n', (size_t)(end - cur));
					if (!cur_end)
						cur_end = end;
				} else {
					break;
				}
			}
			raw_val[w] = '\0';
			mime_decode_header(raw_val, out, out_cap);
			return;
		}

		p = (line_end < end) ? line_end + 1 : end;
	}
}

static void extract_param(const char *hdr_val, const char *param, char *out, size_t out_cap)
{
	out[0] = '\0';
	const char *p = ci_strstr(hdr_val, param);
	if (!p)
		return;
	p += strlen(param);
	while (*p == ' ' || *p == '=')
		p++;
	char quote = 0;
	if (*p == '"' || *p == '\'')
		quote = *p++;
	size_t o = 0;
	while (*p && o + 1 < out_cap) {
		if (quote && *p == quote)
			break;
		if (!quote && (*p == ';' || *p == ' ' || *p == '\r' || *p == '\n'))
			break;
		out[o++] = *p++;
	}
	out[o] = '\0';
}

static const char *find_header_body_split(const char *raw, size_t len, size_t *hdr_len_out)
{
	for (size_t i = 0; i + 1 < len; i++) {
		if (raw[i] == '\r' && i + 3 < len &&
		    raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') {
			*hdr_len_out = i;
			return raw + i + 4;
		}
		if (raw[i] == '\n' && raw[i + 1] == '\n') {
			*hdr_len_out = i;
			return raw + i + 2;
		}
	}
	*hdr_len_out = len;
	return raw + len;
}

static void decode_body_part(const char *hdrs, size_t hdr_len,
                             const char *body, size_t body_len,
                             char **plain_out, char **html_out,
                             char *attachments, size_t att_cap, int depth)
{
	if (depth > 4)
		return;

	char ctype[256], cte[64], cdisp[256];
	extract_header_value(hdrs, hdr_len, "Content-Type", ctype, sizeof(ctype));
	extract_header_value(hdrs, hdr_len, "Content-Transfer-Encoding", cte, sizeof(cte));
	extract_header_value(hdrs, hdr_len, "Content-Disposition", cdisp, sizeof(cdisp));

	if (ctype[0] == '\0')
		snprintf(ctype, sizeof(ctype), "text/plain");

	if (ci_strstr(ctype, "multipart/") != NULL) {
		char boundary[128];
		extract_param(ctype, "boundary", boundary, sizeof(boundary));
		if (boundary[0] == '\0')
			return;

		char delim[136];
		snprintf(delim, sizeof(delim), "--%s", boundary);
		size_t dlen = strlen(delim);

		const char *p = body;
		const char *end = body + body_len;
		while (p < end) {
			const char *part = NULL;
			for (const char *s = p; s + dlen <= end; s++) {
				if (memcmp(s, delim, dlen) == 0) {
					part = s;
					break;
				}
			}
			if (!part)
				break;
			part += dlen;
			if (part + 2 <= end && part[0] == '-' && part[1] == '-')
				break; /* closing boundary */
			if (part < end && *part == '\r') part++;
			if (part < end && *part == '\n') part++;

			/* Find next boundary */
			const char *next = NULL;
			for (const char *s = part; s + dlen <= end; s++) {
				if (memcmp(s, delim, dlen) == 0) {
					next = s;
					break;
				}
			}
			if (!next)
				next = end;
			size_t plen = (size_t)(next - part);
			while (plen > 0 && (part[plen - 1] == '\n' || part[plen - 1] == '\r'))
				plen--;

			size_t phdr_len = 0;
			const char *pbody = find_header_body_split(part, plen, &phdr_len);
			size_t pbody_len = (size_t)((part + plen) - pbody);
			decode_body_part(part, phdr_len, pbody, pbody_len,
			                 plain_out, html_out, attachments, att_cap, depth + 1);
			p = next;
		}
		return;
	}

	/* Check attachment */
	if (ci_strstr(cdisp, "attachment") != NULL) {
		char fname[128];
		extract_param(cdisp, "filename", fname, sizeof(fname));
		if (fname[0] == '\0')
			extract_param(ctype, "name", fname, sizeof(fname));
		if (fname[0] != '\0' && attachments[0] == '\0') {
			snprintf(attachments, att_cap, "%s", fname);
		} else if (fname[0] != '\0') {
			size_t cur = strlen(attachments);
			if (cur + 3 + strlen(fname) < att_cap)
				snprintf(attachments + cur, att_cap - cur, ", %s", fname);
		}
		return;
	}

	int is_plain = (ci_strstr(ctype, "text/plain") != NULL);
	int is_html = (ci_strstr(ctype, "text/html") != NULL);
	if (!is_plain && !is_html)
		return;

	char *decoded = (char *)malloc(body_len + 4);
	if (!decoded)
		return;

	size_t dlen = body_len;
	if (ci_strstr(cte, "base64") != NULL) {
		dlen = mime_base64_decode(body, body_len, (unsigned char *)decoded, body_len + 1);
	} else if (ci_strstr(cte, "quoted-printable") != NULL) {
		dlen = mime_qp_decode(body, body_len, decoded, body_len + 1);
	} else {
		memcpy(decoded, body, body_len);
		decoded[body_len] = '\0';
	}

	if (is_plain && !*plain_out) {
		*plain_out = decoded;
	} else if (is_html && !*html_out) {
		char *text = (char *)malloc(dlen + 1);
		if (text) {
			mime_html_to_text(decoded, dlen, text, dlen + 1);
			*html_out = text;
		}
		free(decoded);
	} else {
		free(decoded);
	}
}

int mime_parse_message(const char *raw, size_t raw_len, MimeMessage *out)
{
	memset(out, 0, sizeof(*out));
	if (!raw || raw_len == 0)
		return -1;

	size_t hdr_len = 0;
	const char *body = find_header_body_split(raw, raw_len, &hdr_len);
	size_t body_len = (size_t)((raw + raw_len) - body);

	extract_header_value(raw, hdr_len, "Subject", out->subject, sizeof(out->subject));
	extract_header_value(raw, hdr_len, "From", out->from, sizeof(out->from));
	extract_header_value(raw, hdr_len, "To", out->to, sizeof(out->to));
	extract_header_value(raw, hdr_len, "Date", out->date, sizeof(out->date));
	extract_header_value(raw, hdr_len, "Message-ID", out->message_id, sizeof(out->message_id));

	if (out->subject[0] == '\0')
		snprintf(out->subject, sizeof(out->subject), "(No Subject)");

	char *plain = NULL;
	char *html = NULL;
	decode_body_part(raw, hdr_len, body, body_len,
	                 &plain, &html, out->attachments, sizeof(out->attachments), 0);

	if (plain) {
		out->body = plain;
		free(html);
	} else if (html) {
		out->body = html;
	} else {
		out->body = strdup("(Empty or unsupported message body)");
	}
	return 0;
}
