#include "net_tls.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/error.h"
#include "mbedtls/x509.h"

#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/rng.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static struct {
	int initialized;
	int ca_init;
	NetTlsStatus status;
	mbedtls_x509_crt cacert;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;
#ifdef __vita__
	void *net_mem;
#endif
} g_tls;

/* Hardware entropy source required by MBEDTLS_ENTROPY_HARDWARE_ALT */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
	(void)data;
#ifdef __vita__
	size_t off = 0;
	while (off < len) {
		size_t chunk = len - off;
		if (chunk > 64)
			chunk = 64;
		if (sceKernelGetRandomNumber(output + off, chunk) < 0)
			return -1;
		off += chunk;
	}
	*olen = len;
	return 0;
#else
	FILE *f = fopen("/dev/urandom", "rb");
	if (!f)
		return -1;
	size_t n = fread(output, 1, len, f);
	fclose(f);
	if (n != len)
		return -1;
	*olen = n;
	return 0;
#endif
}

static int read_entire_file(const char *path, char **out_buf, size_t *out_len, int *out_err)
{
	if (out_err) *out_err = 0;
#ifdef __vita__
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) {
		if (out_err) *out_err = fd;
		return -1;
	}
	SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
	if (sz <= 0 || sz > 4 * 1024 * 1024) {
		if (out_err) *out_err = (int)sz;
		sceIoClose(fd);
		return -1;
	}
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	char *buf = (char *)malloc((size_t)sz + 1);
	if (!buf) {
		sceIoClose(fd);
		return -1;
	}
	size_t total = 0;
	while (total < (size_t)sz) {
		int r = sceIoRead(fd, buf + total, (SceSize)((size_t)sz - total));
		if (r <= 0)
			break;
		total += (size_t)r;
	}
	sceIoClose(fd);
	buf[total] = '\0';
	*out_buf = buf;
	*out_len = total;
	return 0;
#else
	FILE *f = fopen(path, "rb");
	if (!f) {
		if (out_err) *out_err = errno;
		return -1;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return -1;
	}
	long sz = ftell(f);
	if (sz <= 0 || sz > 4 * 1024 * 1024) {
		fclose(f);
		return -1;
	}
	fseek(f, 0, SEEK_SET);
	char *buf = (char *)malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return -1;
	}
	size_t n = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[n] = '\0';
	*out_buf = buf;
	*out_len = n;
	return 0;
#endif
}

/* Parse PEM bundle cert-by-cert so comment headers (#SCEI...) and individual
 * unsupported certs in CA_LIST.cer don't abort loading the entire bundle. */
static int parse_pem_bundle(const char *pem, size_t len, int *loaded, int *skipped)
{
	const char *begin_tag = "-----BEGIN CERTIFICATE-----";
	const char *end_tag = "-----END CERTIFICATE-----";
	size_t end_tag_len = strlen(end_tag);

	*loaded = 0;
	*skipped = 0;

	const char *p = pem;
	const char *end = pem + len;
	while (p < end) {
		const char *b = strstr(p, begin_tag);
		if (!b)
			break;
		const char *e = strstr(b, end_tag);
		if (!e)
			break;
		e += end_tag_len;
		/* Include trailing newline if present */
		while (e < end && (*e == '\r' || *e == '\n'))
			e++;

		size_t cert_len = (size_t)(e - b);
		char *tmp = (char *)malloc(cert_len + 1);
		if (!tmp)
			return -1;
		memcpy(tmp, b, cert_len);
		tmp[cert_len] = '\0';

		int rc = mbedtls_x509_crt_parse(&g_tls.cacert, (const unsigned char *)tmp, cert_len + 1);
		free(tmp);

		if (rc == 0)
			(*loaded)++;
		else
			(*skipped)++;

		p = e;
	}
	return (*loaded > 0) ? 0 : -1;
}

int net_tls_reload_ca(const char *ca_path_override)
{
	if (g_tls.ca_init)
		mbedtls_x509_crt_free(&g_tls.cacert);
	mbedtls_x509_crt_init(&g_tls.cacert);
	g_tls.ca_init = 1;

	const char *candidates[] = {
		ca_path_override ? ca_path_override : NET_TLS_CA_PATH_VITA,
		"vs0:data/external/cert/CA_LIST.cer",
		"ux0:data/VitaMail/CA_LIST.cer",
		"ux0:data/VitaMail/ca_cert.pem"
	};
	int num_candidates = ca_path_override ? 1 : 4;

	g_tls.status.ca_found = 0;
	g_tls.status.ca_loaded_count = 0;
	g_tls.status.ca_skipped_count = 0;

	char *buf = NULL;
	size_t len = 0;
	int last_err = 0;
	const char *used_path = candidates[0];

	for (int i = 0; i < num_candidates; i++) {
		int err = 0;
		if (read_entire_file(candidates[i], &buf, &len, &err) == 0) {
			used_path = candidates[i];
			break;
		}
		last_err = err;
	}

	snprintf(g_tls.status.ca_path, sizeof(g_tls.status.ca_path), "%s", used_path);

	if (!buf) {
		snprintf(g_tls.status.status_msg, sizeof(g_tls.status.status_msg),
		         "Cannot open %s (err 0x%08X). Install iTLS-Enso CA certs.",
		         candidates[0], (unsigned int)last_err);
		return -1;
	}

	g_tls.status.ca_found = 1;
	int loaded = 0, skipped = 0;
	int rc = parse_pem_bundle(buf, len, &loaded, &skipped);
	free(buf);

	g_tls.status.ca_loaded_count = loaded;
	g_tls.status.ca_skipped_count = skipped;

	if (rc != 0 || loaded == 0) {
		snprintf(g_tls.status.status_msg, sizeof(g_tls.status.status_msg),
		         "CA bundle at %s contained 0 valid certificates.", used_path);
		return -1;
	}

	snprintf(g_tls.status.status_msg, sizeof(g_tls.status.status_msg),
	         "Loaded %d CA certificates from %s (%d skipped).", loaded, used_path, skipped);
	return 0;
}

int net_tls_global_init(const char *ca_path_override)
{
	if (g_tls.initialized)
		return 0;

	memset(&g_tls, 0, sizeof(g_tls));

#ifdef __vita__
	sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	SceNetInitParam initparam;
	g_tls.net_mem = malloc(1 * 1024 * 1024);
	if (g_tls.net_mem) {
		initparam.memory = g_tls.net_mem;
		initparam.size = 1 * 1024 * 1024;
		initparam.flags = 0;
		sceNetInit(&initparam);
	}
	sceNetCtlInit();
#endif
	g_tls.status.net_initialized = 1;

	mbedtls_entropy_init(&g_tls.entropy);
	mbedtls_ctr_drbg_init(&g_tls.ctr_drbg);

	const char *pers = "VitaMail_TLS";
	if (mbedtls_ctr_drbg_seed(&g_tls.ctr_drbg, mbedtls_entropy_func, &g_tls.entropy,
	                          (const unsigned char *)pers, strlen(pers)) != 0) {
		snprintf(g_tls.status.status_msg, sizeof(g_tls.status.status_msg),
		         "Failed to seed TLS RNG.");
		return -1;
	}

	g_tls.initialized = 1;
	net_tls_reload_ca(ca_path_override);
	return 0;
}

void net_tls_global_shutdown(void)
{
	if (!g_tls.initialized)
		return;
	if (g_tls.ca_init)
		mbedtls_x509_crt_free(&g_tls.cacert);
	mbedtls_ctr_drbg_free(&g_tls.ctr_drbg);
	mbedtls_entropy_free(&g_tls.entropy);
#ifdef __vita__
	sceNetCtlTerm();
	sceNetTerm();
	free(g_tls.net_mem);
	g_tls.net_mem = NULL;
	sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
#endif
	memset(&g_tls, 0, sizeof(g_tls));
}

void net_tls_get_status(NetTlsStatus *out)
{
	if (out)
		*out = g_tls.status;
}

int net_tls_is_online(void)
{
#ifdef __vita__
	int state = 0;
	if (sceNetCtlInetGetState(&state) < 0)
		return 0;
	return (state == SCE_NETCTL_STATE_CONNECTED) ? 1 : 0;
#else
	return 1;
#endif
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	NetTlsConn *c = (NetTlsConn *)ctx;
#ifdef __vita__
	int ret = sceNetSend(c->sock, buf, (unsigned int)len, 0);
	if (ret < 0) {
		if (ret == (int)SCE_NET_ERROR_EAGAIN || ret == (int)SCE_NET_ERROR_EINTR)
			return MBEDTLS_ERR_SSL_WANT_WRITE;
		return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	}
	return ret;
#else
	ssize_t ret = send(c->sock, buf, len, 0);
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return MBEDTLS_ERR_SSL_WANT_WRITE;
		return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	}
	return (int)ret;
#endif
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	NetTlsConn *c = (NetTlsConn *)ctx;
#ifdef __vita__
	int ret = sceNetRecv(c->sock, buf, (unsigned int)len, 0);
	if (ret < 0) {
		if (ret == (int)SCE_NET_ERROR_EAGAIN || ret == (int)SCE_NET_ERROR_EINTR)
			return MBEDTLS_ERR_SSL_WANT_READ;
		return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	}
	return ret;
#else
	ssize_t ret = recv(c->sock, buf, len, 0);
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return MBEDTLS_ERR_SSL_WANT_READ;
		return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	}
	return (int)ret;
#endif
}

static void set_err(char *errbuf, size_t errlen, const char *fmt, ...)
{
	if (!errbuf || errlen == 0)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(errbuf, errlen, fmt, ap);
	va_end(ap);
}

static int tcp_connect_raw(const char *host, uint16_t port, char *errbuf, size_t errlen)
{
#ifdef __vita__
	SceNetInAddr addr;
	int rid = sceNetResolverCreate("VitaMailDNS", NULL, 0);
	if (rid < 0) {
		set_err(errbuf, errlen, "DNS resolver init failed (0x%08X)", rid);
		return -1;
	}
	int rc = sceNetResolverStartNtoa(rid, host, &addr, 10 * 1000 * 1000, 3, 0);
	sceNetResolverDestroy(rid);
	if (rc < 0) {
		set_err(errbuf, errlen, "DNS lookup failed for %s (0x%08X)", host, rc);
		return -1;
	}

	int s = sceNetSocket("VitaMailSock", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
	if (s < 0) {
		set_err(errbuf, errlen, "Socket creation failed (0x%08X)", s);
		return -1;
	}

	unsigned int timeout_us = 15 * 1000 * 1000;
	sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout_us, sizeof(timeout_us));
	sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &timeout_us, sizeof(timeout_us));

	SceNetSockaddrIn sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = SCE_NET_AF_INET;
	sin.sin_port = sceNetHtons(port);
	sin.sin_addr = addr;

	rc = sceNetConnect(s, (SceNetSockaddr *)&sin, sizeof(sin));
	if (rc < 0) {
		sceNetSocketClose(s);
		set_err(errbuf, errlen, "TCP connect to %s:%u failed (0x%08X)", host, port, rc);
		return -1;
	}
	return s;
#else
	struct addrinfo hints, *res = NULL, *rp = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	char portstr[16];
	snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
	int rc = getaddrinfo(host, portstr, &hints, &res);
	if (rc != 0) {
		set_err(errbuf, errlen, "DNS lookup failed for %s: %s", host, gai_strerror(rc));
		return -1;
	}

	int s = -1;
	for (rp = res; rp != NULL; rp = rp->ai_next) {
		s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (s < 0)
			continue;
		struct timeval tv = { 15, 0 };
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0)
			break;
		close(s);
		s = -1;
	}
	freeaddrinfo(res);

	if (s < 0) {
		set_err(errbuf, errlen, "TCP connect to %s:%u failed: %s", host, port, strerror(errno));
		return -1;
	}
	return s;
#endif
}

int net_tls_connect(NetTlsConn *conn, const char *host, uint16_t port, int use_tls,
                    char *errbuf, size_t errlen)
{
	memset(conn, 0, sizeof(*conn));
	conn->sock = -1;

	if (!g_tls.initialized) {
		set_err(errbuf, errlen, "TLS subsystem not initialized");
		return -1;
	}

	int s = tcp_connect_raw(host, port, errbuf, errlen);
	if (s < 0)
		return -1;

	conn->sock = s;
	if (use_tls) {
		if (net_tls_upgrade(conn, host, errbuf, errlen) != 0) {
			net_tls_close(conn);
			return -1;
		}
	}
	return 0;
}

int net_tls_upgrade(NetTlsConn *conn, const char *host, char *errbuf, size_t errlen)
{
	if (!g_tls.status.ca_found || g_tls.status.ca_loaded_count == 0) {
		set_err(errbuf, errlen,
		        "No valid CA certificates loaded (%s). Install iTLS-Enso CA certs first.",
		        g_tls.status.ca_path);
		return -1;
	}

	/* Clear any leftover plaintext buffer before starting TLS */
	conn->rpos = 0;
	conn->rlen = 0;

	mbedtls_ssl_init(&conn->ssl);
	mbedtls_ssl_config_init(&conn->conf);

	int rc = mbedtls_ssl_config_defaults(&conn->conf,
	                                     MBEDTLS_SSL_IS_CLIENT,
	                                     MBEDTLS_SSL_TRANSPORT_STREAM,
	                                     MBEDTLS_SSL_PRESET_DEFAULT);
	if (rc != 0) {
		set_err(errbuf, errlen, "TLS config defaults failed (-0x%04X)", -rc);
		return -1;
	}

	mbedtls_ssl_conf_authmode(&conn->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&conn->conf, &g_tls.cacert, NULL);
	mbedtls_ssl_conf_rng(&conn->conf, mbedtls_ctr_drbg_random, &g_tls.ctr_drbg);
	mbedtls_ssl_conf_min_version(&conn->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
	                             MBEDTLS_SSL_MINOR_VERSION_3); /* TLS 1.2 */

	rc = mbedtls_ssl_setup(&conn->ssl, &conn->conf);
	if (rc != 0) {
		set_err(errbuf, errlen, "TLS setup failed (-0x%04X)", -rc);
		return -1;
	}

	rc = mbedtls_ssl_set_hostname(&conn->ssl, host);
	if (rc != 0) {
		set_err(errbuf, errlen, "TLS SNI hostname failed (-0x%04X)", -rc);
		return -1;
	}

	mbedtls_ssl_set_bio(&conn->ssl, conn, bio_send, bio_recv, NULL);

	while ((rc = mbedtls_ssl_handshake(&conn->ssl)) != 0) {
		if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
			uint32_t flags = mbedtls_ssl_get_verify_result(&conn->ssl);
			if (flags != 0) {
				char vbuf[256];
				mbedtls_x509_crt_verify_info(vbuf, sizeof(vbuf), "", flags);
				/* Trim trailing newline */
				size_t vl = strlen(vbuf);
				while (vl > 0 && (vbuf[vl - 1] == '\n' || vbuf[vl - 1] == '\r'))
					vbuf[--vl] = '\0';
				set_err(errbuf, errlen,
				        "TLS cert verify failed (%s). Ensure iTLS-Enso CA certs & Vita clock are up to date.",
				        vbuf);
			} else {
				char mbed_err[128];
				mbedtls_strerror(rc, mbed_err, sizeof(mbed_err));
				set_err(errbuf, errlen, "TLS handshake failed: %s (-0x%04X)", mbed_err, -rc);
			}
			return -1;
		}
	}

	conn->tls_active = 1;
	return 0;
}

void net_tls_close(NetTlsConn *conn)
{
	if (!conn)
		return;
	if (conn->tls_active) {
		mbedtls_ssl_close_notify(&conn->ssl);
		mbedtls_ssl_free(&conn->ssl);
		mbedtls_ssl_config_free(&conn->conf);
		conn->tls_active = 0;
	}
	if (conn->sock >= 0) {
#ifdef __vita__
		sceNetSocketClose(conn->sock);
#else
		close(conn->sock);
#endif
		conn->sock = -1;
	}
	conn->rpos = 0;
	conn->rlen = 0;
}

int net_tls_send_all(NetTlsConn *conn, const void *data, size_t len,
                     char *errbuf, size_t errlen)
{
	const uint8_t *p = (const uint8_t *)data;
	size_t sent = 0;
	while (sent < len) {
		int rc;
		if (conn->tls_active) {
			rc = mbedtls_ssl_write(&conn->ssl, p + sent, len - sent);
			if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
				continue;
		} else {
			rc = bio_send(conn, p + sent, len - sent);
			if (rc == MBEDTLS_ERR_SSL_WANT_WRITE)
				continue;
		}
		if (rc <= 0) {
			set_err(errbuf, errlen, "Network send failed (-0x%04X)", rc < 0 ? -rc : 0);
			return -1;
		}
		sent += (size_t)rc;
	}
	return 0;
}

int net_tls_sendf(NetTlsConn *conn, char *errbuf, size_t errlen, const char *fmt, ...)
{
	char stack_buf[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
	va_end(ap);

	if (n < 0)
		return -1;
	if ((size_t)n < sizeof(stack_buf)) {
		return net_tls_send_all(conn, stack_buf, (size_t)n, errbuf, errlen);
	}

	char *dyn = (char *)malloc((size_t)n + 1);
	if (!dyn)
		return -1;
	va_start(ap, fmt);
	vsnprintf(dyn, (size_t)n + 1, fmt, ap);
	va_end(ap);
	int rc = net_tls_send_all(conn, dyn, (size_t)n, errbuf, errlen);
	free(dyn);
	return rc;
}

static int refill_rbuf(NetTlsConn *conn, char *errbuf, size_t errlen)
{
	conn->rpos = 0;
	conn->rlen = 0;
	while (1) {
		int rc;
		if (conn->tls_active) {
			rc = mbedtls_ssl_read(&conn->ssl, conn->rbuf, sizeof(conn->rbuf));
			if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
				continue;
			if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == 0) {
				set_err(errbuf, errlen, "Connection closed by server");
				return -1;
			}
		} else {
			rc = bio_recv(conn, conn->rbuf, sizeof(conn->rbuf));
			if (rc == MBEDTLS_ERR_SSL_WANT_READ)
				continue;
			if (rc == 0) {
				set_err(errbuf, errlen, "Connection closed by server");
				return -1;
			}
		}
		if (rc < 0) {
			set_err(errbuf, errlen, "Network receive failed (-0x%04X)", -rc);
			return -1;
		}
		conn->rlen = (size_t)rc;
		return 0;
	}
}

int net_tls_recv_line(NetTlsConn *conn, char *out, size_t out_cap,
                      char *errbuf, size_t errlen)
{
	if (out_cap == 0)
		return -1;

	size_t w = 0;
	while (1) {
		if (conn->rpos >= conn->rlen) {
			if (refill_rbuf(conn, errbuf, errlen) != 0)
				return -1;
		}
		uint8_t ch = conn->rbuf[conn->rpos++];
		if (ch == '\n') {
			if (w > 0 && out[w - 1] == '\r')
				w--;
			out[w] = '\0';
			return (int)w;
		}
		if (w + 1 < out_cap)
			out[w++] = (char)ch;
	}
}

int net_tls_recv_exact(NetTlsConn *conn, void *buf, size_t len,
                       char *errbuf, size_t errlen)
{
	uint8_t *dst = (uint8_t *)buf;
	size_t got = 0;
	while (got < len) {
		if (conn->rpos < conn->rlen) {
			size_t avail = conn->rlen - conn->rpos;
			size_t take = (len - got < avail) ? (len - got) : avail;
			memcpy(dst + got, conn->rbuf + conn->rpos, take);
			conn->rpos += take;
			got += take;
		} else {
			if (refill_rbuf(conn, errbuf, errlen) != 0)
				return -1;
		}
	}
	return 0;
}
