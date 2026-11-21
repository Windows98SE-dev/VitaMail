#include "img_decode.h"

#include <ctype.h>
#include <png.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>

#define IMG_MAX_DIM 4096

static int ci_ends_with(const char *s, const char *suf)
{
	size_t sl = strlen(s);
	size_t fl = strlen(suf);
	if (sl < fl)
		return 0;
	for (size_t i = 0; i < fl; i++) {
		if (tolower((unsigned char)s[sl - fl + i]) != tolower((unsigned char)suf[i]))
			return 0;
	}
	return 1;
}

int img_is_supported(const char *filename, const char *mime_type)
{
	if (mime_type) {
		if (strstr(mime_type, "image/png") ||
		    strstr(mime_type, "image/jpeg") ||
		    strstr(mime_type, "image/jpg") ||
		    strstr(mime_type, "image/bmp"))
			return 1;
	}
	if (filename) {
		if (ci_ends_with(filename, ".png") ||
		    ci_ends_with(filename, ".jpg") ||
		    ci_ends_with(filename, ".jpeg") ||
		    ci_ends_with(filename, ".bmp"))
			return 1;
	}
	return 0;
}

typedef struct PngMemReader {
	const unsigned char *data;
	size_t len;
	size_t pos;
} PngMemReader;

static void png_mem_read_cb(png_structp png_ptr, png_bytep out_bytes, png_size_t count)
{
	PngMemReader *r = (PngMemReader *)png_get_io_ptr(png_ptr);
	if (!r || r->pos + count > r->len) {
		png_error(png_ptr, "Read past end of memory buffer");
		return;
	}
	memcpy(out_bytes, r->data + r->pos, count);
	r->pos += count;
}

static uint8_t *decode_png_mem(const unsigned char *data, size_t len, int *out_w, int *out_h)
{
	if (len < 8 || png_sig_cmp(data, 0, 8) != 0)
		return NULL;

	png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png_ptr)
		return NULL;
	png_infop info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr) {
		png_destroy_read_struct(&png_ptr, NULL, NULL);
		return NULL;
	}

	uint8_t *rgba = NULL;
	png_bytep *row_ptrs = NULL;

	if (setjmp(png_jmpbuf(png_ptr))) {
		free(row_ptrs);
		free(rgba);
		png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
		return NULL;
	}

	PngMemReader reader = { data, len, 0 };
	png_set_read_fn(png_ptr, &reader, png_mem_read_cb);
	png_read_info(png_ptr, info_ptr);

	png_uint_32 w = png_get_image_width(png_ptr, info_ptr);
	png_uint_32 h = png_get_image_height(png_ptr, info_ptr);
	if (w == 0 || h == 0 || w > IMG_MAX_DIM || h > IMG_MAX_DIM) {
		png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
		return NULL;
	}

	png_byte color_type = png_get_color_type(png_ptr, info_ptr);
	png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);

	if (bit_depth == 16)
		png_set_strip_16(png_ptr);
	if (color_type == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(png_ptr);
	if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
		png_set_expand_gray_1_2_4_to_8(png_ptr);
	if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png_ptr);
	if (color_type == PNG_COLOR_TYPE_RGB ||
	    color_type == PNG_COLOR_TYPE_GRAY ||
	    color_type == PNG_COLOR_TYPE_PALETTE)
		png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
	if (color_type == PNG_COLOR_TYPE_GRAY ||
	    color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(png_ptr);

	png_read_update_info(png_ptr, info_ptr);

	size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);
	rgba = (uint8_t *)malloc(rowbytes * h);
	row_ptrs = (png_bytep *)malloc(sizeof(png_bytep) * h);
	if (!rgba || !row_ptrs) {
		free(row_ptrs);
		free(rgba);
		png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
		return NULL;
	}
	for (png_uint_32 y = 0; y < h; y++)
		row_ptrs[y] = rgba + y * rowbytes;

	png_read_image(png_ptr, row_ptrs);
	free(row_ptrs);
	png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

	*out_w = (int)w;
	*out_h = (int)h;
	return rgba;
}

static uint8_t *decode_jpeg_mem(const unsigned char *data, size_t len, int *out_w, int *out_h)
{
	if (len < 3 || data[0] != 0xFF || data[1] != 0xD8)
		return NULL;

	tjhandle tj = tjInitDecompress();
	if (!tj)
		return NULL;

	int w = 0, h = 0, subsamp = 0, colorspace = 0;
	if (tjDecompressHeader3(tj, data, (unsigned long)len, &w, &h, &subsamp, &colorspace) != 0 ||
	    w <= 0 || h <= 0 || w > IMG_MAX_DIM || h > IMG_MAX_DIM) {
		tjDestroy(tj);
		return NULL;
	}

	uint8_t *rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
	if (!rgba) {
		tjDestroy(tj);
		return NULL;
	}

	if (tjDecompress2(tj, data, (unsigned long)len, rgba, w, 0, h, TJPF_RGBA, TJFLAG_FASTDCT) != 0) {
		free(rgba);
		tjDestroy(tj);
		return NULL;
	}

	tjDestroy(tj);
	*out_w = w;
	*out_h = h;
	return rgba;
}

static uint32_t read_u32_le(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_u16_le(const unsigned char *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint8_t *decode_bmp_mem(const unsigned char *data, size_t len, int *out_w, int *out_h)
{
	if (len < 54 || data[0] != 'B' || data[1] != 'M')
		return NULL;

	uint32_t pix_off = read_u32_le(data + 10);
	int32_t w = (int32_t)read_u32_le(data + 18);
	int32_t h_raw = (int32_t)read_u32_le(data + 22);
	int top_down = (h_raw < 0);
	int32_t h = top_down ? -h_raw : h_raw;
	uint16_t bpp = read_u16_le(data + 28);
	uint32_t comp = read_u32_le(data + 30);

	if (w <= 0 || h <= 0 || w > IMG_MAX_DIM || h > IMG_MAX_DIM || comp != 0 || (bpp != 24 && bpp != 32))
		return NULL;

	size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
	if (pix_off + stride * (size_t)h > len)
		return NULL;

	uint8_t *rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
	if (!rgba)
		return NULL;

	for (int32_t y = 0; y < h; y++) {
		int32_t src_y = top_down ? y : (h - 1 - y);
		const unsigned char *src = data + pix_off + (size_t)src_y * stride;
		uint8_t *dst = rgba + (size_t)y * (size_t)w * 4;
		for (int32_t x = 0; x < w; x++) {
			dst[x * 4 + 0] = src[x * (bpp / 8) + 2];
			dst[x * 4 + 1] = src[x * (bpp / 8) + 1];
			dst[x * 4 + 2] = src[x * (bpp / 8) + 0];
			dst[x * 4 + 3] = (bpp == 32) ? src[x * 4 + 3] : 0xFF;
		}
	}

	*out_w = w;
	*out_h = h;
	return rgba;
}

uint8_t *img_decode_memory(const unsigned char *data, size_t len, int *out_w, int *out_h)
{
	if (!data || len < 4 || !out_w || !out_h)
		return NULL;
	if (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
		return decode_png_mem(data, len, out_w, out_h);
	if (data[0] == 0xFF && data[1] == 0xD8)
		return decode_jpeg_mem(data, len, out_w, out_h);
	if (data[0] == 'B' && data[1] == 'M')
		return decode_bmp_mem(data, len, out_w, out_h);
	return NULL;
}
