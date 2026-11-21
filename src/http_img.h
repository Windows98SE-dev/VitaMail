#ifndef VITAMAIL_HTTP_IMG_H
#define VITAMAIL_HTTP_IMG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fetch a remote image (http:// or https://) into a newly allocated buffer.
 * Handles up to 3 redirects and caps size at 2 MiB.
 * Returns 0 on success (caller frees *out_data), -1 on failure. */
int http_fetch_url(const char *url, unsigned char **out_data, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* VITAMAIL_HTTP_IMG_H */
