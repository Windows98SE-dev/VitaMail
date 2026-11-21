	#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "imap.h"
#include "mime.h"
#include "net_tls.h"
#include "smtp.h"

static void test_base64_and_qp(void)
{
	char b64[64];
	unsigned char dec[64];

	size_t elen = mime_base64_encode((const unsigned char *)"VitaMail!", 9, b64, sizeof(b64));
	assert(elen == 12);
	assert(strcmp(b64, "Vml0YU1haWwh") == 0);

	size_t dlen = mime_base64_decode(b64, elen, dec, sizeof(dec));
	assert(dlen == 9);
	assert(strcmp((char *)dec, "VitaMail!") == 0);

	const char *qp = "Hello=20Vita=\r\nWorld=3F";
	char qp_out[64];
	size_t qlen = mime_qp_decode(qp, strlen(qp), qp_out, sizeof(qp_out));
	assert(qlen == 16);
	assert(strcmp(qp_out, "Hello VitaWorld?") == 0);
	printf("[PASS] test_base64_and_qp\n");
}

static void test_rfc2047_and_mime_multipart(void)
{
	char decoded[256];
	mime_decode_header("=?UTF-8?B?SGVsbG8gVml0YQ==?= =?ISO-8859-1?Q?_Caf=E9?=",
	                   decoded, sizeof(decoded));
	assert(strcmp(decoded, "Hello Vita Caf\xC3\xA9") == 0);

	const char *raw_mail =
	    "From: Alice <alice@example.com>\r\n"
	    "To: Bob <bob@example.com>\r\n"
	    "Subject: =?UTF-8?B?VGVzdCBTdWJqZWN0?=\r\n"
	    "Date: Sat, 21 Nov 2026 12:00:00 +0000\r\n"
	    "Message-ID: <123@example.com>\r\n"
	    "Content-Type: multipart/mixed; boundary=\"bnd1\"\r\n"
	    "\r\n"
	    "--bnd1\r\n"
	    "Content-Type: text/html; charset=UTF-8\r\n"
	    "Content-Transfer-Encoding: quoted-printable\r\n"
	    "\r\n"
	    "<html><body><p>Hello=20<b>Vita</b>!</p><br>Line2</body></html>\r\n"
	    "--bnd1\r\n"
	    "Content-Type: application/octet-stream\r\n"
	    "Content-Disposition: attachment; filename=\"report.pdf\"\r\n"
	    "\r\n"
	    "DATA\r\n"
	    "--bnd1--\r\n";

	MimeMessage msg;
	assert(mime_parse_message(raw_mail, strlen(raw_mail), &msg) == 0);
	assert(strcmp(msg.subject, "Test Subject") == 0);
	assert(strcmp(msg.from, "Alice <alice@example.com>") == 0);
	assert(strcmp(msg.attachments, "report.pdf") == 0);
	assert(strstr(msg.body, "Hello Vita!") != NULL);
	assert(strstr(msg.body, "Line2") != NULL);
	mime_message_free(&msg);
	printf("[PASS] test_rfc2047_and_mime_multipart\n");
}

static void test_imap_parsers(void)
{
	size_t sz = 0;
	assert(imap_parse_literal_size("* 12 FETCH (UID 45 BODY[] {1024}", &sz) == 1);
	assert(sz == 1024);
	assert(imap_parse_literal_size("* OK Done", &sz) == 0);

	uint32_t uids[16];
	int n = imap_parse_search_uids("* SEARCH 10 25 42 108", uids, 16);
	assert(n == 4);
	assert(uids[0] == 10 && uids[1] == 25 && uids[2] == 42 && uids[3] == 108);
	printf("[PASS] test_imap_parsers\n");
}

static void test_smtp_format(void)
{
	SmtpSendRequest req = {
		.from = "vita@example.com",
		.to = "friend@example.com",
		.subject = "Hello from PS Vita",
		.body = "Line 1\n.Leading dot\nLast line",
		.in_reply_to = "<prev@example.com>"
	};
	char out[2048];
	size_t n = smtp_format_message(&req, out, sizeof(out));
	assert(n > 0);
	assert(strstr(out, "In-Reply-To: <prev@example.com>\r\n") != NULL);
	assert(strstr(out, "\r\n..Leading dot\r\n") != NULL);
	assert(strcmp(out + n - 5, "\r\n.\r\n") == 0);

	/* Attachment multipart/mixed test + round-trip parse */
	const unsigned char png_stub[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 1, 2, 3 };
	req.attachment_name = "photo.png";
	req.attachment_data = png_stub;
	req.attachment_len = sizeof(png_stub);
	size_t n2 = smtp_format_message(&req, out, sizeof(out));
	assert(n2 > 0);
	assert(strstr(out, "multipart/mixed") != NULL);
	assert(strstr(out, "Content-Type: image/png; name=\"photo.png\"") != NULL);
	assert(strstr(out, "Content-Disposition: attachment; filename=\"photo.png\"") != NULL);

	MimeMessage parsed;
	assert(mime_parse_message(out, n2 - 3, &parsed) == 0);
	assert(strcmp(parsed.attachments, "photo.png") == 0);
	assert(strstr(parsed.body, "Line 1") != NULL);
	mime_message_free(&parsed);
	printf("[PASS] test_smtp_format\n");
}

static void test_config_roundtrip(void)
{
	MailAccountConfig cfg;
	config_init_default(&cfg);
	assert(strcmp(cfg.imap_host, "imap.gmail.com") == 0);
	assert(cfg.imap_port == 993);

	snprintf(cfg.email, sizeof(cfg.email), "user@gmail.com");
	snprintf(cfg.password, sizeof(cfg.password), "abcd-efgh-ijkl-mnop");
	const char *tmp_path = "/tmp/vitamail_test_cfg.cfg";
	assert(config_save(&cfg, tmp_path) == 0);

	MailAccountConfig loaded;
	assert(config_load(&loaded, tmp_path) == 0);
	assert(strcmp(loaded.email, "user@gmail.com") == 0);
	assert(strcmp(loaded.password, "abcd-efgh-ijkl-mnop") == 0);
	assert(loaded.imap_port == 993);
	remove(tmp_path);
	printf("[PASS] test_config_roundtrip\n");
}

static void test_itls_ca_bundle(void)
{
	const char *candidates[] = {
		"iTLS-Enso/misc/certs.cer",
		"../iTLS-Enso/misc/certs.cer",
		"/etc/ssl/certs/ca-certificates.crt",
		"/etc/ssl/cert.pem"
	};
	const char *ca_path = NULL;
	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		FILE *f = fopen(candidates[i], "rb");
		if (f) {
			fclose(f);
			ca_path = candidates[i];
			break;
		}
	}
	assert(ca_path != NULL);
	assert(net_tls_global_init(ca_path) == 0);
	NetTlsStatus st;
	net_tls_get_status(&st);
	printf("[INFO] iTLS CA bundle: found=%d loaded=%d skipped=%d\n",
	       st.ca_found, st.ca_loaded_count, st.ca_skipped_count);
	assert(st.ca_found == 1);
	assert(st.ca_loaded_count >= 1);
	net_tls_global_shutdown();
	printf("[PASS] test_itls_ca_bundle\n");
}

int main(void)
{
	test_base64_and_qp();
	test_rfc2047_and_mime_multipart();
	test_imap_parsers();
	test_smtp_format();
	test_config_roundtrip();
	test_itls_ca_bundle();
	printf("All VitaMail protocol & iTLS CA tests passed!\n");
	return 0;
}
