#include "http_img.h"
#include "net_tls.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_MAX_BYTES (2 * 1024 * 1024)

static int parse_url(const char *url, int *is_https, char *host, size_t hcap,
                     uint16_t *port, char *path, size_t pcap)
{
	if (strncmp(url, "https://", 8) == 0) {
		*is_https = 1;
		*port = 443;
		url += 8;
	} else if (strncmp(url, "http://", 7) == 0) {
		*is_https = 0;
		*port = 80;
		url += 7;
	} else {
		return -1;
	}

	const char *slash = strchr(url, '/');
	size_t hlen = slash ? (size_t)(slash - url) : strlen(url);
	if (hlen == 0 || hlen >= hcap)
		return -1;
	memcpy(host, url, hlen);
	host[hlen] = '\0';

	char *colon = strchr(host, ':');
	if (colon) {
		*colon = '\0';
		int p = atoi(colon + 1);
		if (p > 0 && p <= 65535)
			*port = (uint16_t)p;
	}

	if (slash)
		snprintf(path, pcap, "%s", slash);
	else
		snprintf(path, pcap, "/");
	return 0;
}

static int ci_starts_with(const char *s, const char *pfx)
{
	while (*pfx) {
		if (tolower((unsigned char)*s) != tolower((unsigned char)*pfx))
			return 0;
		s++;
		pfx++;
	}
	return 1;
}

int http_fetch_url(const char *url, unsigned char **out_data, size_t *out_len)
{
	char cur_url[1536];
	snprintf(cur_url, sizeof(cur_url), "%s", url);

	for (int hop = 0; hop < 4; hop++) {
		int is_https = 0;
		char host[256], path[1280], err[128];
		uint16_t port = 443;

		if (parse_url(cur_url, &is_https, host, sizeof(host), &port, path, sizeof(path)) != 0)
			return -1;

		NetTlsConn conn;
		if (net_tls_connect(&conn, host, port, is_https, err, sizeof(err)) != 0)
			return -1;

		if (net_tls_sendf(&conn, err, sizeof(err),
		                  "GET %s HTTP/1.1\r\n"
		                  "Host: %s\r\n"
		                  "User-Agent: Mozilla/5.0 (compatible; VitaMail/1.0)\r\n"
		                  "Accept: image/png,image/jpeg,image/bmp\r\n"
		                  "Connection: close\r\n\r\n",
		                  path, host) != 0) {
			net_tls_close(&conn);
			return -1;
		}

		char line[1536];
		if (net_tls_recv_line(&conn, line, sizeof(line), err, sizeof(err)) < 0) {
			net_tls_close(&conn);
			return -1;
		}

		const char *sp = strchr(line, ' ');
		int status = sp ? atoi(sp + 1) : 0;

		long content_len = -1;
		int chunked = 0;
		char location[1536] = {0};

		while (1) {
			int r = net_tls_recv_line(&conn, line, sizeof(line), err, sizeof(err));
			if (r <= 0)
				break;
			if (ci_starts_with(line, "Content-Length:")) {
				content_len = atol(line + 15);
			} else if (ci_starts_with(line, "Transfer-Encoding:") &&
			           (strstr(line, "chunked") || strstr(line, "CHUNKED"))) {
				chunked = 1;
			} else if (ci_starts_with(line, "Location:")) {
				const char *v = line + 9;
				while (*v == ' ' || *v == '\t') v++;
				snprintf(location, sizeof(location), "%s", v);
			}
		}

		if ((status == 301 || status == 302 || status == 307 || status == 308) && location[0]) {
			net_tls_close(&conn);
			if (strncmp(location, "http://", 7) == 0 || strncmp(location, "https://", 8) == 0) {
				snprintf(cur_url, sizeof(cur_url), "%s", location);
			} else {
				snprintf(cur_url, sizeof(cur_url), "%s://%s%s",
				         is_https ? "https" : "http", host, location);
			}
			continue;
		}

		if (status < 200 || status >= 300) {
			net_tls_close(&conn);
			return -1;
		}

		unsigned char *buf = NULL;
		size_t total = 0;

		if (chunked) {
			size_t cap = 65536;
			buf = (unsigned char *)malloc(cap);
			if (!buf) {
				net_tls_close(&conn);
				return -1;
			}
			while (1) {
				if (net_tls_recv_line(&conn, line, sizeof(line), err, sizeof(err)) < 0)
					break;
				size_t csz = (size_t)strtoul(line, NULL, 16);
				if (csz == 0)
					break;
				if (total + csz > HTTP_MAX_BYTES) {
					free(buf);
					net_tls_close(&conn);
					return -1;
				}
				if (total + csz > cap) {
					while (cap < total + csz) cap *= 2;
					unsigned char *nb = (unsigned char *)realloc(buf, cap);
					if (!nb) {
						free(buf);
						net_tls_close(&conn);
						return -1;
					}
					buf = nb;
				}
				if (net_tls_recv_exact(&conn, buf + total, csz, err, sizeof(err)) != 0) {
					free(buf);
					net_tls_close(&conn);
					return -1;
				}
				total += csz;
				net_tls_recv_line(&conn, line, sizeof(line), err, sizeof(err));
			}
		} else if (content_len > 0 && content_len <= HTTP_MAX_BYTES) {
			buf = (unsigned char *)malloc((size_t)content_len);
			if (!buf) {
				net_tls_close(&conn);
				return -1;
			}
			if (net_tls_recv_exact(&conn, buf, (size_t)content_len, err, sizeof(err)) != 0) {
				free(buf);
				net_tls_close(&conn);
				return -1;
			}
			total = (size_t)content_len;
		} else {
			size_t cap = 65536;
			buf = (unsigned char *)malloc(cap);
			if (!buf) {
				net_tls_close(&conn);
				return -1;
			}
			while (total < HTTP_MAX_BYTES) {
				uint8_t b;
				if (net_tls_recv_exact(&conn, &b, 1, err, sizeof(err)) != 0)
					break;
				if (total + 1 > cap) {
					cap *= 2;
					unsigned char *nb = (unsigned char *)realloc(buf, cap);
					if (!nb) {
						free(buf);
						net_tls_close(&conn);
						return -1;
					}
					buf = nb;
				}
				buf[total++] = b;
			}
		}

		net_tls_close(&conn);
		if (total == 0) {
			free(buf);
			return -1;
		}
		*out_data = buf;
		*out_len = total;
		return 0;
	}
	return -1;
}
