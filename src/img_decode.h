#ifndef VITAMAIL_IMG_DECODE_H
#define VITAMAIL_IMG_DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode PNG, JPEG, or uncompressed BMP from memory into a newly-allocated
 * RGBA8888 buffer (4 bytes per pixel). Caller must free() the returned pointer.
 * Returns NULL if format is unsupported or decoding fails. */
uint8_t *img_decode_memory(const unsigned char *data, size_t len, int *out_w, int *out_h);

/* Returns 1 if filename or mime_type looks like a supported image (PNG/JPEG/BMP). */
int img_is_supported(const char *filename, const char *mime_type);

#ifdef __cplusplus
}
#endif

#endif /* VITAMAIL_IMG_DECODE_H */
