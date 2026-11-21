#ifndef VITAMAIL_NET_TLS_H
#define VITAMAIL_NET_TLS_H

#include <stddef.h>
#include <stdint.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NET_TLS_CA_PATH_VITA "vs0:/data/external/cert/CA_LIST.cer"

typedef struct NetTlsStatus {
	int net_initialized;
	int ca_found;
	int ca_loaded_count;
	int ca_skipped_count;
	char ca_path[256];
	char status_msg[256];
} NetTlsStatus;

typedef struct NetTlsConn {
	int sock;
	int tls_active;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	/* Small read-ahead buffer for fast line & literal reading */
	uint8_t rbuf[4096];
	size_t rpos;
	size_t rlen;
} NetTlsConn;

/* Global init: initializes SceNet (on Vita) and loads the iTLS CA certificate store.
 * If ca_path_override is NULL, uses NET_TLS_CA_PATH_VITA on Vita. */
int net_tls_global_init(const char *ca_path_override);
void net_tls_global_shutdown(void);

/* Reload CA store (e.g., if user just installed iTLS or wants to re-check). */
int net_tls_reload_ca(const char *ca_path_override);

/* Get current network & CA store status for UI diagnostics. */
void net_tls_get_status(NetTlsStatus *out);

/* Check if Vita Wi-Fi / network is currently connected (returns 1 if up, 0 if down). */
int net_tls_is_online(void);

/* Connect TCP to host:port. If use_tls != 0, immediately performs TLS handshake. */
int net_tls_connect(NetTlsConn *conn, const char *host, uint16_t port, int use_tls,
                    char *errbuf, size_t errlen);

/* Upgrade an existing plain TCP connection to TLS (for STARTTLS). */
int net_tls_upgrade(NetTlsConn *conn, const char *host, char *errbuf, size_t errlen);

/* Close connection and free TLS context. */
void net_tls_close(NetTlsConn *conn);

/* Send all bytes. Returns 0 on success, -1 on error. */
int net_tls_send_all(NetTlsConn *conn, const void *data, size_t len,
                     char *errbuf, size_t errlen);

/* Formatted send (e.g., IMAP/SMTP command). Returns 0 on success, -1 on error. */
int net_tls_sendf(NetTlsConn *conn, char *errbuf, size_t errlen, const char *fmt, ...);

/* Read a CRLF-terminated line (strips trailing \r\n). Returns line length >= 0, or -1 on error/EOF. */
int net_tls_recv_line(NetTlsConn *conn, char *out, size_t out_cap,
                      char *errbuf, size_t errlen);

/* Read exact number of bytes into buf. Returns 0 on success, -1 on error/EOF. */
int net_tls_recv_exact(NetTlsConn *conn, void *buf, size_t len,
                       char *errbuf, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* VITAMAIL_NET_TLS_H */
