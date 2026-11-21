#include "html_view.h"
#include "http_img.h"
#include "img_decode.h"
#include "mime.h"
#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif

#define HTML_MAX_CSS_RULES 64
#define HTML_IMG_CACHE_CAP 24
#define MAX_LINE_TOKENS 64

typedef enum HtmlAlign {
	ALIGN_LEFT = 0,
	ALIGN_CENTER,
	ALIGN_RIGHT
} HtmlAlign;

typedef struct HtmlStyle {
	int font_px;
	int bold;
	int underline;
	int strike;
	int in_link;
	int in_pre;
	int in_quote;
	int list_depth;
	int has_bg;
	int has_fg;
	int hidden;
	HtmlAlign align;
	UiColor color;
	UiColor bg_color;
} HtmlStyle;

typedef struct CssRule {
	char selector[64];
	char decls[256];
} CssRule;

typedef enum ImgCacheState {
	IMG_EMPTY = 0,
	IMG_LOADING,
	IMG_READY,
	IMG_FAILED
} ImgCacheState;

typedef struct HtmlImgEntry {
	char src[1024];
	ImgCacheState state;
	uint8_t *rgba;
	SDL_Texture *tex;
	int w, h;
} HtmlImgEntry;

static struct {
	int init;
	int shutdown;
	SDL_mutex *lock;
	HtmlImgEntry imgs[HTML_IMG_CACHE_CAP];
	int worker_started;
} g_hcache;

static int img_worker_loop(void *arg)
{
	(void)arg;
	while (!g_hcache.shutdown) {
		int slot = -1;
		char url[1024] = {0};

		SDL_LockMutex(g_hcache.lock);
		for (int i = 0; i < HTML_IMG_CACHE_CAP; i++) {
			if (g_hcache.imgs[i].state == IMG_LOADING && !g_hcache.imgs[i].rgba && !g_hcache.imgs[i].tex) {
				slot = i;
				snprintf(url, sizeof(url), "%s", g_hcache.imgs[i].src);
				break;
			}
		}
		SDL_UnlockMutex(g_hcache.lock);

		if (slot < 0) {
			SDL_Delay(40);
			continue;
		}

		unsigned char *raw = NULL;
		size_t raw_len = 0;
		int w = 0, h = 0;
		uint8_t *rgba = NULL;

		if (http_fetch_url(url, &raw, &raw_len) == 0) {
			rgba = img_decode_memory(raw, raw_len, &w, &h);
			free(raw);
		}

		SDL_LockMutex(g_hcache.lock);
		if (strcmp(g_hcache.imgs[slot].src, url) == 0 &&
		    g_hcache.imgs[slot].state == IMG_LOADING) {
			if (rgba && w > 0 && h > 0) {
				g_hcache.imgs[slot].rgba = rgba;
				g_hcache.imgs[slot].w = w;
				g_hcache.imgs[slot].h = h;
				g_hcache.imgs[slot].state = IMG_READY;
				rgba = NULL;
			} else {
				g_hcache.imgs[slot].state = IMG_FAILED;
			}
		}
		SDL_UnlockMutex(g_hcache.lock);
		free(rgba);
	}
	return 0;
}

#ifdef __vita__
static int vita_img_thread_entry(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	img_worker_loop(NULL);
	return sceKernelExitDeleteThread(0);
}
#endif

static void ensure_cache_init(void)
{
	if (!g_hcache.init) {
		g_hcache.lock = SDL_CreateMutex();
		g_hcache.init = 1;
	}
	if (!g_hcache.worker_started) {
		g_hcache.worker_started = 1;
#ifdef __vita__
		/* 256 KB stack so mbedTLS + libpng/libturbojpeg never overflow */
		SceUID tid = sceKernelCreateThread("VitaMailImg", vita_img_thread_entry,
		                                   0x10000100, 256 * 1024, 0, 0, NULL);
		if (tid >= 0)
			sceKernelStartThread(tid, 0, NULL);
#else
		SDL_Thread *th = SDL_CreateThread(img_worker_loop, "HtmlImg", NULL);
		if (th)
			SDL_DetachThread(th);
#endif
	}
}

void html_view_clear_cache(void)
{
	if (!g_hcache.init)
		return;
	SDL_LockMutex(g_hcache.lock);
	for (int i = 0; i < HTML_IMG_CACHE_CAP; i++) {
		if (g_hcache.imgs[i].tex) {
			SDL_DestroyTexture(g_hcache.imgs[i].tex);
			g_hcache.imgs[i].tex = NULL;
		}
		free(g_hcache.imgs[i].rgba);
		g_hcache.imgs[i].rgba = NULL;
		g_hcache.imgs[i].state = IMG_EMPTY;
		g_hcache.imgs[i].src[0] = '\0';
	}
	SDL_UnlockMutex(g_hcache.lock);
}

static void unescape_url(const char *in, char *out, size_t cap)
{
	size_t o = 0;
	for (const char *p = in; *p && o + 1 < cap;) {
		if (strncmp(p, "&amp;", 5) == 0) {
			out[o++] = '&';
			p += 5;
		} else {
			out[o++] = *p++;
		}
	}
	out[o] = '\0';
}

static HtmlImgEntry *resolve_image(SDL_Renderer *ren, const char *raw_src, const MimeMessage *msg)
{
	if (!raw_src || !*raw_src)
		return NULL;

	char src[1024];
	unescape_url(raw_src, src, sizeof(src));
	ensure_cache_init();

	SDL_LockMutex(g_hcache.lock);
	int free_slot = -1;
	for (int i = 0; i < HTML_IMG_CACHE_CAP; i++) {
		if (g_hcache.imgs[i].state != IMG_EMPTY &&
		    strncmp(g_hcache.imgs[i].src, src, sizeof(g_hcache.imgs[i].src) - 1) == 0) {
			HtmlImgEntry *e = &g_hcache.imgs[i];
			if (e->state == IMG_READY && !e->tex && e->rgba && ren) {
				e->tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
				                           SDL_TEXTUREACCESS_STATIC, e->w, e->h);
				if (e->tex) {
					SDL_UpdateTexture(e->tex, NULL, e->rgba, e->w * 4);
					SDL_SetTextureBlendMode(e->tex, SDL_BLENDMODE_BLEND);
				}
				free(e->rgba);
				e->rgba = NULL;
			}
			SDL_UnlockMutex(g_hcache.lock);
			return e;
		}
		if (g_hcache.imgs[i].state == IMG_EMPTY && free_slot < 0)
			free_slot = i;
	}

	if (free_slot < 0) {
		SDL_UnlockMutex(g_hcache.lock);
		return NULL;
	}

	HtmlImgEntry *e = &g_hcache.imgs[free_slot];
	memset(e, 0, sizeof(*e));
	snprintf(e->src, sizeof(e->src), "%.1023s", src);

	if (strncmp(src, "cid:", 4) == 0 && msg) {
		const char *cid = src + 4;
		for (int i = 0; i < msg->attachment_count; i++) {
			const MimeAttachment *att = &msg->attachments[i];
			if (att->content_id[0] && strstr(att->content_id, cid) != NULL &&
			    att->data && att->size > 0) {
				int w = 0, h = 0;
				uint8_t *rgba = img_decode_memory(att->data, att->size, &w, &h);
				if (rgba) {
					e->tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
					                           SDL_TEXTUREACCESS_STATIC, w, h);
					if (e->tex) {
						SDL_UpdateTexture(e->tex, NULL, rgba, w * 4);
						SDL_SetTextureBlendMode(e->tex, SDL_BLENDMODE_BLEND);
						e->w = w;
						e->h = h;
						e->state = IMG_READY;
					} else {
						e->state = IMG_FAILED;
					}
					free(rgba);
					SDL_UnlockMutex(g_hcache.lock);
					return e;
				}
			}
		}
		e->state = IMG_FAILED;
		SDL_UnlockMutex(g_hcache.lock);
		return e;
	}

	if (strncmp(src, "data:image/", 11) == 0) {
		const char *b64 = strstr(src, ";base64,");
		if (b64) {
			b64 += 8;
			size_t blen = strlen(b64);
			unsigned char *raw = (unsigned char *)malloc(blen + 4);
			if (raw) {
				size_t rlen = mime_base64_decode(b64, blen, raw, blen + 1);
				int w = 0, h = 0;
				uint8_t *rgba = img_decode_memory(raw, rlen, &w, &h);
				free(raw);
				if (rgba) {
					e->tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
					                           SDL_TEXTUREACCESS_STATIC, w, h);
					if (e->tex) {
						SDL_UpdateTexture(e->tex, NULL, rgba, w * 4);
						SDL_SetTextureBlendMode(e->tex, SDL_BLENDMODE_BLEND);
						e->w = w;
						e->h = h;
						e->state = IMG_READY;
					} else {
						e->state = IMG_FAILED;
					}
					free(rgba);
					SDL_UnlockMutex(g_hcache.lock);
					return e;
				}
			}
		}
		e->state = IMG_FAILED;
		SDL_UnlockMutex(g_hcache.lock);
		return e;
	}

	if (strncmp(src, "https://", 8) == 0 || strncmp(src, "http://", 7) == 0) {
		e->state = IMG_LOADING;
		SDL_UnlockMutex(g_hcache.lock);
		return e;
	}

	e->state = IMG_FAILED;
	SDL_UnlockMutex(g_hcache.lock);
	return e;
}

static int ci_eq(const char *a, const char *b)
{
	while (*a && *b) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

static int ci_starts(const char *s, const char *pfx)
{
	while (*pfx) {
		if (tolower((unsigned char)*s) != tolower((unsigned char)*pfx))
			return 0;
		s++;
		pfx++;
	}
	return 1;
}

static const char *ci_find(const char *hay, const char *needle)
{
	size_t nlen = strlen(needle);
	for (const char *p = hay; *p; p++) {
		if (ci_starts(p, needle) && (p == hay || !isalnum((unsigned char)p[-1]))) {
			if (needle[nlen - 1] == ':' || !isalnum((unsigned char)p[nlen]))
				return p;
		}
	}
	return NULL;
}

static int parse_css_color(const char *s, UiColor *out)
{
	while (*s == ' ') s++;
	if (*s == '#') {
		s++;
		size_t len = 0;
		while (isxdigit((unsigned char)s[len]))
			len++;
		if (len == 6) {
			unsigned int rgb = (unsigned int)strtoul(s, NULL, 16);
			out->r = (uint8_t)((rgb >> 16) & 0xFF);
			out->g = (uint8_t)((rgb >> 8) & 0xFF);
			out->b = (uint8_t)(rgb & 0xFF);
			out->a = 255;
			return 1;
		}
		if (len == 3) {
			char buf[7] = { s[0], s[0], s[1], s[1], s[2], s[2], '\0' };
			unsigned int rgb = (unsigned int)strtoul(buf, NULL, 16);
			out->r = (uint8_t)((rgb >> 16) & 0xFF);
			out->g = (uint8_t)((rgb >> 8) & 0xFF);
			out->b = (uint8_t)(rgb & 0xFF);
			out->a = 255;
			return 1;
		}
		return 0;
	}
	if (ci_starts(s, "rgb(")) {
		int r = 0, g = 0, b = 0;
		if (sscanf(s + 4, "%d , %d , %d", &r, &g, &b) == 3) {
			out->r = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
			out->g = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
			out->b = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
			out->a = 255;
			return 1;
		}
		return 0;
	}
	if (ci_starts(s, "white"))  { *out = (UiColor){ 255, 255, 255, 255 }; return 1; }
	if (ci_starts(s, "black"))  { *out = (UiColor){ 16, 16, 16, 255 }; return 1; }
	if (ci_starts(s, "red"))    { *out = (UiColor){ 220, 38, 38, 255 }; return 1; }
	if (ci_starts(s, "green"))  { *out = (UiColor){ 22, 163, 74, 255 }; return 1; }
	if (ci_starts(s, "blue"))   { *out = (UiColor){ 37, 99, 235, 255 }; return 1; }
	if (ci_starts(s, "gray") || ci_starts(s, "grey")) { *out = (UiColor){ 100, 116, 139, 255 }; return 1; }
	if (ci_starts(s, "orange")) { *out = (UiColor){ 234, 88, 12, 255 }; return 1; }
	if (ci_starts(s, "navy"))   { *out = (UiColor){ 30, 58, 138, 255 }; return 1; }
	if (ci_starts(s, "purple")) { *out = (UiColor){ 147, 51, 234, 255 }; return 1; }
	return 0;
}

static void apply_css_decls(const char *decls, HtmlStyle *st)
{
	if (!decls || !*decls)
		return;

	const char *bg = ci_find(decls, "background-color:");
	if (!bg) bg = ci_find(decls, "background:");
	if (bg) {
		const char *val = strchr(bg, ':') + 1;
		UiColor c;
		if (parse_css_color(val, &c)) {
			st->bg_color = c;
			st->has_bg = 1;
		}
	}

	const char *col = ci_find(decls, "color:");
	if (col && (col == decls || col[-1] != '-')) {
		const char *val = strchr(col, ':') + 1;
		UiColor c;
		if (parse_css_color(val, &c)) {
			st->color = c;
			st->has_fg = 1;
		}
	}

	const char *fs = ci_find(decls, "font-size:");
	if (fs) {
		const char *val = strchr(fs, ':') + 1;
		while (*val == ' ') val++;
		int px = atoi(val);
		if (px > 0) {
			if (strstr(val, "pt")) px = (px * 4) / 3;
			if (px < 12) px = 12;
			if (px > 26) px = 26;
			st->font_px = px;
		} else if (ci_starts(val, "x-large")) st->font_px = 24;
		else if (ci_starts(val, "large")) st->font_px = 20;
		else if (ci_starts(val, "small")) st->font_px = 13;
	}

	const char *fw = ci_find(decls, "font-weight:");
	if (fw) {
		const char *val = strchr(fw, ':') + 1;
		while (*val == ' ') val++;
		if (ci_starts(val, "bold") || atoi(val) >= 600)
			st->bold = 1;
	}

	const char *td = ci_find(decls, "text-decoration:");
	if (td) {
		const char *val = strchr(td, ':') + 1;
		if (strstr(val, "underline")) st->underline = 1;
		if (strstr(val, "none")) st->underline = 0;
		if (strstr(val, "line-through")) st->strike = 1;
	}

	const char *ta = ci_find(decls, "text-align:");
	if (ta) {
		const char *val = strchr(ta, ':') + 1;
		while (*val == ' ') val++;
		if (ci_starts(val, "center")) st->align = ALIGN_CENTER;
		else if (ci_starts(val, "right")) st->align = ALIGN_RIGHT;
		else if (ci_starts(val, "left")) st->align = ALIGN_LEFT;
	}

	/* Hide preheader / hidden elements */
	const char *disp = ci_find(decls, "display:");
	if (disp) {
		const char *val = strchr(disp, ':') + 1;
		while (*val == ' ') val++;
		if (ci_starts(val, "none")) st->hidden = 1;
	}
	const char *vis = ci_find(decls, "visibility:");
	if (vis) {
		const char *val = strchr(vis, ':') + 1;
		while (*val == ' ') val++;
		if (ci_starts(val, "hidden")) st->hidden = 1;
	}
	const char *mh = ci_find(decls, "max-height:");
	if (mh) {
		const char *val = strchr(mh, ':') + 1;
		while (*val == ' ') val++;
		if (val[0] == '0' && !isdigit((unsigned char)val[1])) st->hidden = 1;
	}
	const char *op = ci_find(decls, "opacity:");
	if (op) {
		const char *val = strchr(op, ':') + 1;
		while (*val == ' ') val++;
		if (val[0] == '0' && val[1] != '.') st->hidden = 1;
	}
}

static void parse_style_blocks(const char *html, CssRule *rules, int *rule_count)
{
	*rule_count = 0;
	const char *p = html;
	while (*p && *rule_count < HTML_MAX_CSS_RULES) {
		const char *st = strstr(p, "<style");
		if (!st) st = strstr(p, "<STYLE");
		if (!st) break;
		const char *gt = strchr(st, '>');
		if (!gt) break;
		const char *end = strstr(gt + 1, "</style>");
		if (!end) end = strstr(gt + 1, "</STYLE>");
		if (!end) break;

		const char *c = gt + 1;
		while (c < end && *rule_count < HTML_MAX_CSS_RULES) {
			while (c < end && (isspace((unsigned char)*c) || *c == '}')) c++;
			if (c >= end) break;
			if (c[0] == '/' && c + 1 < end && c[1] == '*') {
				const char *ec = strstr(c + 2, "*/");
				c = (ec && ec < end) ? (ec + 2) : end;
				continue;
			}
			const char *lb = memchr(c, '{', (size_t)(end - c));
			if (!lb) break;
			const char *rb = memchr(lb + 1, '}', (size_t)(end - (lb + 1)));
			if (!rb) break;

			size_t slen = (size_t)(lb - c);
			size_t dlen = (size_t)(rb - (lb + 1));
			if (slen > 0 && slen < 64 && dlen > 0 && dlen < 256 && c[0] != '@') {
				CssRule *r = &rules[(*rule_count)++];
				memcpy(r->selector, c, slen);
				r->selector[slen] = '\0';
				while (slen > 0 && isspace((unsigned char)r->selector[slen - 1]))
					r->selector[--slen] = '\0';
				memcpy(r->decls, lb + 1, dlen);
				r->decls[dlen] = '\0';
			}
			c = rb + 1;
		}
		p = end + 8;
	}
}

static void extract_attr(const char *tag, const char *attr, char *out, size_t cap)
{
	out[0] = '\0';
	size_t alen = strlen(attr);
	const char *p = tag;
	while (*p) {
		while (*p && isspace((unsigned char)*p))
			p++;
		if (ci_starts(p, attr) && (p[alen] == '=' || isspace((unsigned char)p[alen]))) {
			p += alen;
			while (*p && (isspace((unsigned char)*p) || *p == '='))
				p++;
			char q = 0;
			if (*p == '"' || *p == '\'')
				q = *p++;
			size_t o = 0;
			while (*p && o + 1 < cap) {
				if (q && *p == q) break;
				if (!q && isspace((unsigned char)*p)) break;
				out[o++] = *p++;
			}
			out[o] = '\0';
			return;
		}
		/* Advance past current token (respecting quotes) */
		while (*p && !isspace((unsigned char)*p)) {
			if (*p == '"' || *p == '\'') {
				char q = *p++;
				while (*p && *p != q) p++;
				if (*p == q) p++;
			} else {
				p++;
			}
		}
	}
}

static size_t decode_entity(const char *p, char *utf8_out)
{
	const char *semi = strchr(p, ';');
	if (!semi || (semi - p) > 12) {
		utf8_out[0] = '&';
		utf8_out[1] = '\0';
		return 1;
	}

	size_t nlen = (size_t)(semi - p - 1);
	char name[16];
	memcpy(name, p + 1, nlen);
	name[nlen] = '\0';

	uint32_t cp = 0;
	if (name[0] == '#') {
		if (name[1] == 'x' || name[1] == 'X')
			cp = (uint32_t)strtoul(name + 2, NULL, 16);
		else
			cp = (uint32_t)strtoul(name + 1, NULL, 10);
	} else if (strcmp(name, "amp") == 0) cp = '&';
	else if (strcmp(name, "lt") == 0) cp = '<';
	else if (strcmp(name, "gt") == 0) cp = '>';
	else if (strcmp(name, "quot") == 0) cp = '"';
	else if (strcmp(name, "apos") == 0 || strcmp(name, "#39") == 0) cp = '\'';
	else if (strcmp(name, "nbsp") == 0) cp = ' ';
	else if (strcmp(name, "mdash") == 0) cp = 0x2014;
	else if (strcmp(name, "ndash") == 0) cp = 0x2013;
	else if (strcmp(name, "hellip") == 0) cp = 0x2026;
	else if (strcmp(name, "bull") == 0) cp = 0x2022;
	else if (strcmp(name, "copy") == 0) cp = 0x00A9;
	else if (strcmp(name, "reg") == 0) cp = 0x00AE;
	else if (strcmp(name, "trade") == 0) cp = 0x2122;
	else if (strcmp(name, "lsquo") == 0) cp = 0x2018;
	else if (strcmp(name, "rsquo") == 0) cp = 0x2019;
	else if (strcmp(name, "ldquo") == 0) cp = 0x201C;
	else if (strcmp(name, "rdquo") == 0) cp = 0x201D;

	if (cp == 0) {
		utf8_out[0] = '&';
		utf8_out[1] = '\0';
		return 1;
	}
	if (cp < 0x80) {
		utf8_out[0] = (char)cp;
		utf8_out[1] = '\0';
	} else if (cp < 0x800) {
		utf8_out[0] = (char)(0xC0 | (cp >> 6));
		utf8_out[1] = (char)(0x80 | (cp & 0x3F));
		utf8_out[2] = '\0';
	} else if (cp < 0x10000) {
		utf8_out[0] = (char)(0xE0 | (cp >> 12));
		utf8_out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		utf8_out[2] = (char)(0x80 | (cp & 0x3F));
		utf8_out[3] = '\0';
	} else {
		utf8_out[0] = '?';
		utf8_out[1] = '\0';
	}
	return (size_t)(semi - p + 1);
}

typedef struct LineToken {
	char text[256];
	int x_off;
	int w;
	HtmlStyle style;
} LineToken;

typedef struct RenderCtx {
	SDL_Renderer *ren;
	int base_x, base_y, max_w, max_h, scroll_y;
	int pen_x, pen_y, line_h;
	int last_space;
	HtmlStyle stack[16];
	int sp;
	LineToken line[MAX_LINE_TOKENS];
	int ntok;
} RenderCtx;

static void flush_line(RenderCtx *c, int extra_gap)
{
	int indent = c->stack[c->sp].in_quote * 16 + c->stack[c->sp].list_depth * 18;
	if (c->ntok > 0) {
		int line_w = c->line[c->ntok - 1].x_off + c->line[c->ntok - 1].w;
		HtmlAlign al = c->line[0].style.align;
		int shift = 0;
		if (al == ALIGN_CENTER && line_w < c->max_w)
			shift = (c->max_w - line_w) / 2;
		else if (al == ALIGN_RIGHT && line_w < c->max_w)
			shift = c->max_w - line_w;

		int draw_y = c->base_y + c->pen_y - c->scroll_y;
		if (draw_y + c->line_h >= c->base_y && draw_y <= c->base_y + c->max_h) {
			if (c->stack[c->sp].in_quote > 0)
				ui_fill_rect(c->base_x + 4, draw_y, 3, c->line_h, (UiColor){ 66, 153, 225, 255 });

			/* Draw continuous background pills for spans with bg_color */
			for (int i = 0; i < c->ntok;) {
				if (!c->line[i].style.has_bg) {
					i++;
					continue;
				}
				UiColor bg = c->line[i].style.bg_color;
				int start_x = c->base_x + shift + c->line[i].x_off;
				int end_x = start_x + c->line[i].w;
				int j = i + 1;
				while (j < c->ntok && c->line[j].style.has_bg &&
				       memcmp(&c->line[j].style.bg_color, &bg, sizeof(UiColor)) == 0) {
					end_x = c->base_x + shift + c->line[j].x_off + c->line[j].w;
					j++;
				}
				ui_fill_rect(start_x - 8, draw_y - 2, (end_x - start_x) + 16, c->line_h + 4, bg);
				i = j;
			}

			for (int i = 0; i < c->ntok; i++) {
				const LineToken *t = &c->line[i];
				int tx = c->base_x + shift + t->x_off;
				ui_draw_text_styled(tx, draw_y, t->style.font_px, t->style.color, t->text, 0,
				                    t->style.bold, t->style.underline, t->style.strike);
			}
		}
		c->pen_y += c->line_h + extra_gap;
	} else if (extra_gap > 0) {
		c->pen_y += extra_gap;
	}

	c->ntok = 0;
	c->pen_x = indent;
	c->line_h = c->stack[c->sp].font_px + 7;
	c->last_space = 1;
}

static void ctx_emit_word(RenderCtx *c, const char *word)
{
	if (!word || !*word || c->stack[c->sp].hidden)
		return;

	HtmlStyle *st = &c->stack[c->sp];
	int indent = st->in_quote * 16 + st->list_depth * 18;
	int w = ui_measure_text(st->font_px, word);

	if ((c->ntok > 0 && c->pen_x + w > c->max_w) || c->ntok >= MAX_LINE_TOKENS)
		flush_line(c, 0);

	if (c->pen_x < indent)
		c->pen_x = indent;

	int cur_line_h = st->font_px + 7;
	if (cur_line_h > c->line_h)
		c->line_h = cur_line_h;

	LineToken *t = &c->line[c->ntok++];
	snprintf(t->text, sizeof(t->text), "%.255s", word);
	t->x_off = c->pen_x;
	t->w = w;
	t->style = *st;

	c->pen_x += w;
	c->last_space = 0;
}

static void ctx_emit_space(RenderCtx *c)
{
	if (c->last_space || c->ntok == 0 || c->stack[c->sp].hidden)
		return;
	HtmlStyle *st = &c->stack[c->sp];
	c->pen_x += ui_measure_text(st->font_px, " ");
	c->last_space = 1;
}

int html_view_render(SDL_Renderer *ren, int x, int y, int max_w, int max_h,
                     const char *html, const MimeMessage *msg, int scroll_y)
{
	if (!html)
		return 0;

	static CssRule rules[HTML_MAX_CSS_RULES];
	int rule_count = 0;
	parse_style_blocks(html, rules, &rule_count);

	SDL_Rect clip = { x, y, max_w, max_h };
	SDL_RenderSetClipRect(ren, &clip);

	static RenderCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.ren = ren;
	ctx.base_x = x;
	ctx.base_y = y;
	ctx.max_w = max_w;
	ctx.max_h = max_h;
	ctx.scroll_y = scroll_y;
	ctx.pen_x = 0;
	ctx.pen_y = 4;
	ctx.line_h = 23;
	ctx.last_space = 1;

	/* Default dark text on light paper background */
	ctx.stack[0] = (HtmlStyle){
		.font_px = 16,
		.color = { 31, 41, 55, 255 }
	};

	const char *p = html;
	int skip_block = 0;
	char word[512];
	size_t wlen = 0;

	while (*p) {
		if (*p == '<') {
			if (wlen > 0) {
				word[wlen] = '\0';
				ctx_emit_word(&ctx, word);
				wlen = 0;
			}

			if (strncmp(p, "<!--", 4) == 0) {
				const char *endc = strstr(p + 4, "-->");
				p = endc ? (endc + 3) : (p + strlen(p));
				continue;
			}

			const char *gt = strchr(p + 1, '>');
			if (!gt)
				break;

			size_t tlen = (size_t)(gt - (p + 1));
			char tag_raw[1536];
			if (tlen >= sizeof(tag_raw)) tlen = sizeof(tag_raw) - 1;
			memcpy(tag_raw, p + 1, tlen);
			tag_raw[tlen] = '\0';
			p = gt + 1;

			char tname[32];
			size_t ni = 0;
			const char *tp = tag_raw;
			while (*tp && isspace((unsigned char)*tp)) tp++;
			int is_close = (*tp == '/');
			if (is_close) tp++;
			while (*tp && !isspace((unsigned char)*tp) && *tp != '/' && ni + 1 < sizeof(tname))
				tname[ni++] = (char)tolower((unsigned char)*tp++);
			tname[ni] = '\0';

			if (ci_eq(tname, "head") || ci_eq(tname, "style") ||
			    ci_eq(tname, "script") || ci_eq(tname, "title")) {
				skip_block = is_close ? 0 : 1;
				continue;
			}
			if (skip_block)
				continue;

			if (ci_eq(tname, "br")) {
				flush_line(&ctx, ctx.ntok == 0 ? 12 : 0);
			} else if (ci_eq(tname, "hr")) {
				flush_line(&ctx, 6);
				int sy = ctx.base_y + ctx.pen_y - ctx.scroll_y;
				if (sy >= ctx.base_y && sy <= ctx.base_y + ctx.max_h)
					ui_fill_rect(ctx.base_x + 12, sy, ctx.max_w - 24, 1, (UiColor){ 203, 213, 225, 255 });
				ctx.pen_y += 10;
			} else if (ci_eq(tname, "img")) {
				if (ctx.stack[ctx.sp].hidden)
					continue;
				char alt[96], src[1024], w_attr[16], h_attr[16];
				extract_attr(tag_raw, "alt", alt, sizeof(alt));
				extract_attr(tag_raw, "src", src, sizeof(src));
				extract_attr(tag_raw, "width", w_attr, sizeof(w_attr));
				extract_attr(tag_raw, "height", h_attr, sizeof(h_attr));

				HtmlImgEntry *img = resolve_image(ren, src, msg);
				if (img && img->state == IMG_READY && img->tex && img->w > 0 && img->h > 0) {
					int dw = atoi(w_attr) > 0 ? atoi(w_attr) : img->w;
					int dh = atoi(h_attr) > 0 ? atoi(h_attr) : img->h;
					if (dw > ctx.max_w - 24) {
						float sc = (float)(ctx.max_w - 24) / (float)dw;
						dw = ctx.max_w - 24;
						dh = (int)((float)dh * sc);
					}
					if (dh > 260) {
						float sc = 260.0f / (float)dh;
						dh = 260;
						dw = (int)((float)dw * sc);
					}
					if (ctx.ntok > 0 && ctx.stack[ctx.sp].align == ALIGN_CENTER) {
						flush_line(&ctx, 4);
					} else if (ctx.ntok > 0) {
						/* Keep small inline icon on same line if it fits */
						if (dh <= 32 && dw <= 48 && ctx.pen_x + dw < ctx.max_w) {
							int sy = ctx.base_y + ctx.pen_y - ctx.scroll_y;
							if (sy + dh >= ctx.base_y && sy <= ctx.base_y + ctx.max_h) {
								SDL_Rect dst = { ctx.base_x + ctx.pen_x, sy, dw, dh };
								SDL_RenderCopy(ren, img->tex, NULL, &dst);
							}
							ctx.pen_x += dw + 6;
							continue;
						}
						flush_line(&ctx, 4);
					}
					int draw_x = ctx.base_x;
					if (ctx.stack[ctx.sp].align == ALIGN_CENTER)
						draw_x = ctx.base_x + (ctx.max_w - dw) / 2;
					int sy = ctx.base_y + ctx.pen_y - ctx.scroll_y;
					if (sy + dh >= ctx.base_y && sy <= ctx.base_y + ctx.max_h) {
						SDL_Rect dst = { draw_x, sy, dw, dh };
						SDL_RenderCopy(ren, img->tex, NULL, &dst);
					}
					ctx.pen_y += dh + 6;
					ctx.pen_x = 0;
					ctx.last_space = 1;
				} else if (img && img->state == IMG_LOADING) {
					HtmlStyle saved = ctx.stack[ctx.sp];
					ctx.stack[ctx.sp].color = (UiColor){ 100, 116, 139, 255 };
					ctx_emit_word(&ctx, "[Loading...]");
					ctx_emit_space(&ctx);
					ctx.stack[ctx.sp] = saved;
				} else if (alt[0]) {
					char badge[128];
					snprintf(badge, sizeof(badge), "[%.100s]", alt);
					HtmlStyle saved = ctx.stack[ctx.sp];
					ctx.stack[ctx.sp].color = (UiColor){ 100, 116, 139, 255 };
					ctx_emit_word(&ctx, badge);
					ctx_emit_space(&ctx);
					ctx.stack[ctx.sp] = saved;
				}
			} else if (is_close) {
				if (ci_eq(tname, "p") || ci_eq(tname, "div") || ci_eq(tname, "tr") ||
				    ci_eq(tname, "li") || ci_eq(tname, "blockquote") ||
				    ci_eq(tname, "pre") || ci_eq(tname, "center") ||
				    (tname[0] == 'h' && tname[1] >= '1' && tname[1] <= '6')) {
					flush_line(&ctx, (tname[0] == 'h' || ci_eq(tname, "p")) ? 6 : 2);
				} else if (ci_eq(tname, "td") || ci_eq(tname, "th")) {
					ctx_emit_space(&ctx);
				}
				if (ctx.sp > 0)
					ctx.sp--;
			} else {
				int is_block = (ci_eq(tname, "p") || ci_eq(tname, "div") || ci_eq(tname, "tr") ||
				                ci_eq(tname, "blockquote") || ci_eq(tname, "pre") ||
				                ci_eq(tname, "ul") || ci_eq(tname, "ol") || ci_eq(tname, "center") ||
				                (tname[0] == 'h' && tname[1] >= '1' && tname[1] <= '6'));
				if (is_block && ctx.ntok > 0)
					flush_line(&ctx, ci_eq(tname, "p") ? 4 : 0);

				if (ci_eq(tname, "li")) {
					if (ctx.ntok > 0)
						flush_line(&ctx, 0);
					ctx_emit_word(&ctx, "•");
					ctx_emit_space(&ctx);
				}

				if (ctx.sp + 1 < 16) {
					ctx.stack[ctx.sp + 1] = ctx.stack[ctx.sp];
					ctx.sp++;
				}
				HtmlStyle *st = &ctx.stack[ctx.sp];

				/* Inherit block bg shouldn't turn every nested text into a pill unless inline/button */
				if (ci_eq(tname, "table") || ci_eq(tname, "tr") || ci_eq(tname, "td") || ci_eq(tname, "div"))
					st->has_bg = 0;

				char cls_attr[128], id_attr[64];
				extract_attr(tag_raw, "class", cls_attr, sizeof(cls_attr));
				extract_attr(tag_raw, "id", id_attr, sizeof(id_attr));
				for (int r = 0; r < rule_count; r++) {
					const char *sel = rules[r].selector;
					if (ci_eq(sel, tname) ||
					    (sel[0] == '.' && cls_attr[0] && strstr(cls_attr, sel + 1)) ||
					    (sel[0] == '#' && id_attr[0] && ci_eq(id_attr, sel + 1))) {
						apply_css_decls(rules[r].decls, st);
					}
				}

				if (tname[0] == 'h' && tname[1] >= '1' && tname[1] <= '6' && tname[2] == '\0') {
					int lvl = tname[1] - '0';
					st->font_px = (lvl == 1) ? 24 : (lvl == 2) ? 21 : (lvl == 3) ? 18 : 16;
					st->bold = 1;
				} else if (ci_eq(tname, "b") || ci_eq(tname, "strong") || ci_eq(tname, "th")) {
					st->bold = 1;
				} else if (ci_eq(tname, "u")) {
					st->underline = 1;
				} else if (ci_eq(tname, "s") || ci_eq(tname, "del") || ci_eq(tname, "strike")) {
					st->strike = 1;
				} else if (ci_eq(tname, "center")) {
					st->align = ALIGN_CENTER;
				} else if (ci_eq(tname, "a")) {
					st->in_link = 1;
					st->underline = 1;
					if (!st->has_fg)
						st->color = (UiColor){ 26, 115, 232, 255 };
				} else if (ci_eq(tname, "blockquote")) {
					st->in_quote++;
					st->color = (UiColor){ 71, 85, 105, 255 };
				} else if (ci_eq(tname, "ul") || ci_eq(tname, "ol")) {
					st->list_depth++;
				} else if (ci_eq(tname, "pre") || ci_eq(tname, "code")) {
					st->in_pre = 1;
					st->color = (UiColor){ 15, 118, 110, 255 };
				}

				char align_attr[24], col_attr[32], style_attr[768];
				extract_attr(tag_raw, "align", align_attr, sizeof(align_attr));
				if (ci_eq(align_attr, "center")) st->align = ALIGN_CENTER;
				else if (ci_eq(align_attr, "right")) st->align = ALIGN_RIGHT;
				else if (ci_eq(align_attr, "left")) st->align = ALIGN_LEFT;

				extract_attr(tag_raw, "color", col_attr, sizeof(col_attr));
				if (col_attr[0]) {
					UiColor cc;
					if (parse_css_color(col_attr, &cc)) {
						st->color = cc;
						st->has_fg = 1;
					}
				}

				extract_attr(tag_raw, "style", style_attr, sizeof(style_attr));
				if (style_attr[0]) {
					apply_css_decls(style_attr, st);
					/* Don't draw pill boxes around whole table/div containers */
					if (ci_eq(tname, "table") || ci_eq(tname, "td") || ci_eq(tname, "div") || ci_eq(tname, "body"))
						st->has_bg = 0;
				}
			}
			continue;
		}

		if (skip_block) {
			p++;
			continue;
		}

		if (*p == '&') {
			char ent[8];
			size_t adv = decode_entity(p, ent);
			p += adv;
			if (ent[0] == ' ' && !ctx.stack[ctx.sp].in_pre) {
				if (wlen > 0) {
					word[wlen] = '\0';
					ctx_emit_word(&ctx, word);
					wlen = 0;
				}
				ctx_emit_space(&ctx);
			} else {
				size_t el = strlen(ent);
				if (wlen + el < sizeof(word)) {
					memcpy(word + wlen, ent, el);
					wlen += el;
				}
			}
			continue;
		}

		if (*p == '\n' && ctx.stack[ctx.sp].in_pre) {
			if (wlen > 0) {
				word[wlen] = '\0';
				ctx_emit_word(&ctx, word);
				wlen = 0;
			}
			flush_line(&ctx, 0);
			p++;
			continue;
		}

		if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
			if (wlen > 0) {
				word[wlen] = '\0';
				ctx_emit_word(&ctx, word);
				wlen = 0;
			}
			ctx_emit_space(&ctx);
			p++;
			continue;
		}

		if (wlen + 1 < sizeof(word))
			word[wlen++] = *p;
		p++;
	}

	if (wlen > 0) {
		word[wlen] = '\0';
		ctx_emit_word(&ctx, word);
	}
	flush_line(&ctx, 0);

	SDL_RenderSetClipRect(ren, NULL);
	return ctx.pen_y + 12;
}
