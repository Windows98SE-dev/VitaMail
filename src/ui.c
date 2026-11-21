#include "ui.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLYPH_CACHE_CAP 512

typedef struct CachedGlyph {
	uint32_t codepoint;
	int size_px;
	SDL_Texture *tex;
	int w, h;
	int bearing_x, bearing_y;
	int advance;
	uint64_t last_used;
} CachedGlyph;

static struct {
	SDL_Renderer *ren;
	FT_Library ft;
	FT_Face face;
	int ft_ready;
	int cur_size_px;
	CachedGlyph cache[GLYPH_CACHE_CAP];
	uint64_t tick;
} g_ui;

int ui_init(SDL_Renderer *renderer, const char *font_path)
{
	memset(&g_ui, 0, sizeof(g_ui));
	g_ui.ren = renderer;

	if (FT_Init_FreeType(&g_ui.ft) != 0)
		return -1;

	if (FT_New_Face(g_ui.ft, font_path, 0, &g_ui.face) != 0) {
		FT_Done_FreeType(g_ui.ft);
		return -1;
	}
	g_ui.ft_ready = 1;
	return 0;
}

void ui_shutdown(void)
{
	for (int i = 0; i < GLYPH_CACHE_CAP; i++) {
		if (g_ui.cache[i].tex) {
			SDL_DestroyTexture(g_ui.cache[i].tex);
			g_ui.cache[i].tex = NULL;
		}
	}
	if (g_ui.ft_ready) {
		FT_Done_Face(g_ui.face);
		FT_Done_FreeType(g_ui.ft);
		g_ui.ft_ready = 0;
	}
}

void ui_clear(UiColor c)
{
	SDL_SetRenderDrawColor(g_ui.ren, c.r, c.g, c.b, c.a);
	SDL_RenderClear(g_ui.ren);
}

void ui_fill_rect(int x, int y, int w, int h, UiColor c)
{
	SDL_SetRenderDrawBlendMode(g_ui.ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(g_ui.ren, c.r, c.g, c.b, c.a);
	SDL_Rect r = { x, y, w, h };
	SDL_RenderFillRect(g_ui.ren, &r);
}

void ui_draw_rect(int x, int y, int w, int h, UiColor c)
{
	SDL_SetRenderDrawBlendMode(g_ui.ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(g_ui.ren, c.r, c.g, c.b, c.a);
	SDL_Rect r = { x, y, w, h };
	SDL_RenderDrawRect(g_ui.ren, &r);
}

void ui_fill_circle_dot(int cx, int cy, int r, UiColor c)
{
	SDL_SetRenderDrawBlendMode(g_ui.ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(g_ui.ren, c.r, c.g, c.b, c.a);
	for (int dy = -r; dy <= r; dy++) {
		for (int dx = -r; dx <= r; dx++) {
			if (dx * dx + dy * dy <= r * r)
				SDL_RenderDrawPoint(g_ui.ren, cx + dx, cy + dy);
		}
	}
}

static uint32_t next_utf8(const char **p)
{
	const unsigned char *s = (const unsigned char *)*p;
	if (*s == '\0')
		return 0;
	uint32_t cp;
	if (*s < 0x80) {
		cp = s[0];
		*p += 1;
	} else if ((s[0] & 0xE0) == 0xC0 && s[1]) {
		cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
		*p += 2;
	} else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
		cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
		*p += 3;
	} else if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
		cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
		     ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
		*p += 4;
	} else {
		cp = '?';
		*p += 1;
	}
	return cp;
}

static int is_zero_width_cp(uint32_t cp)
{
	return (cp >= 0x200B && cp <= 0x200F) ||
	       (cp >= 0x202A && cp <= 0x202E) ||
	       (cp >= 0x2060 && cp <= 0x206F) ||
	       (cp >= 0xFE00 && cp <= 0xFE0F) ||
	       cp == 0x034F || cp == 0xFEFF || cp == 0x00AD;
}

static uint32_t normalize_space_cp(uint32_t cp)
{
	if (cp == 0x00A0 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000)
		return ' ';
	return cp;
}

static CachedGlyph *get_glyph(uint32_t cp, int size_px)
{
	if (is_zero_width_cp(cp))
		return NULL;
	cp = normalize_space_cp(cp);

	g_ui.tick++;
	int empty_idx = -1;
	int lru_idx = 0;

	for (int i = 0; i < GLYPH_CACHE_CAP; i++) {
		if (g_ui.cache[i].size_px == size_px && g_ui.cache[i].codepoint == cp &&
		    (g_ui.cache[i].tex != NULL || g_ui.cache[i].advance > 0)) {
			g_ui.cache[i].last_used = g_ui.tick;
			return &g_ui.cache[i];
		}
		if (g_ui.cache[i].size_px == 0 && empty_idx < 0)
			empty_idx = i;
		if (g_ui.cache[i].last_used < g_ui.cache[lru_idx].last_used)
			lru_idx = i;
	}

	int slot = (empty_idx >= 0) ? empty_idx : lru_idx;
	if (g_ui.cache[slot].tex) {
		SDL_DestroyTexture(g_ui.cache[slot].tex);
		g_ui.cache[slot].tex = NULL;
	}

	if (g_ui.cur_size_px != size_px) {
		FT_Set_Pixel_Sizes(g_ui.face, 0, (FT_UInt)size_px);
		g_ui.cur_size_px = size_px;
	}

	if (FT_Load_Char(g_ui.face, cp, FT_LOAD_RENDER) != 0)
		return NULL;

	FT_GlyphSlot gs = g_ui.face->glyph;
	CachedGlyph *cg = &g_ui.cache[slot];
	cg->codepoint = cp;
	cg->size_px = size_px;
	cg->w = (int)gs->bitmap.width;
	cg->h = (int)gs->bitmap.rows;
	cg->bearing_x = gs->bitmap_left;
	cg->bearing_y = gs->bitmap_top;
	cg->advance = (int)(gs->advance.x >> 6);
	cg->last_used = g_ui.tick;

	if (cg->w > 0 && cg->h > 0) {
		uint32_t *rgba = (uint32_t *)malloc((size_t)cg->w * (size_t)cg->h * 4);
		if (rgba) {
			for (int y = 0; y < cg->h; y++) {
				const uint8_t *row = gs->bitmap.buffer + y * gs->bitmap.pitch;
				for (int x = 0; x < cg->w; x++) {
					uint8_t a = row[x];
					rgba[y * cg->w + x] = 0xFFFFFF00u | (uint32_t)a;
				}
			}
			cg->tex = SDL_CreateTexture(g_ui.ren, SDL_PIXELFORMAT_RGBA8888,
			                            SDL_TEXTUREACCESS_STATIC, cg->w, cg->h);
			if (cg->tex) {
				SDL_UpdateTexture(cg->tex, NULL, rgba, cg->w * 4);
				SDL_SetTextureBlendMode(cg->tex, SDL_BLENDMODE_BLEND);
			}
			free(rgba);
		}
	}
	return cg;
}

int ui_measure_text(int size_px, const char *text)
{
	if (!g_ui.ft_ready || !text)
		return 0;
	int pen_x = 0;
	const char *p = text;
	while (*p) {
		uint32_t cp = next_utf8(&p);
		if (cp == '\r' || cp == '\n' || cp == '\t')
			cp = ' ';
		CachedGlyph *cg = get_glyph(cp, size_px);
		if (cg)
			pen_x += cg->advance;
	}
	return pen_x;
}

void ui_draw_text_styled(int x, int y, int size_px, UiColor c, const char *text, int max_w,
                         int bold, int underline, int strike)
{
	ui_draw_text(x, y, size_px, c, text, max_w);
	if (bold)
		ui_draw_text(x + 1, y, size_px, c, text, max_w);
	if (underline || strike) {
		int w = ui_measure_text(size_px, text);
		if (max_w > 0 && w > max_w) w = max_w;
		if (underline)
			ui_fill_rect(x, y + size_px + 2, w, 1, c);
		if (strike)
			ui_fill_rect(x, y + (size_px * 2) / 3, w, 1, c);
	}
}

void ui_draw_text(int x, int y, int size_px, UiColor c, const char *text, int max_w)
{
	if (!g_ui.ft_ready || !text)
		return;

	int pen_x = x;
	int baseline_y = y + size_px;
	const char *p = text;

	while (*p) {
		const char *prev = p;
		uint32_t cp = next_utf8(&p);
		if (cp == '\r' || cp == '\n' || cp == '\t')
			cp = ' ';

		CachedGlyph *cg = get_glyph(cp, size_px);
		if (!cg)
			continue;

		if (max_w > 0 && (pen_x - x + cg->advance) > max_w - 14 && *prev != '\0') {
			/* Draw ellipsis */
			for (int k = 0; k < 3; k++) {
				CachedGlyph *dot = get_glyph('.', size_px);
				if (dot && dot->tex) {
					SDL_SetTextureColorMod(dot->tex, c.r, c.g, c.b);
					SDL_SetTextureAlphaMod(dot->tex, c.a);
					SDL_Rect dst = { pen_x + dot->bearing_x, baseline_y - dot->bearing_y, dot->w, dot->h };
					SDL_RenderCopy(g_ui.ren, dot->tex, NULL, &dst);
				}
				if (dot)
					pen_x += dot->advance;
			}
			break;
		}

		if (cg->tex) {
			SDL_SetTextureColorMod(cg->tex, c.r, c.g, c.b);
			SDL_SetTextureAlphaMod(cg->tex, c.a);
			SDL_Rect dst = {
				pen_x + cg->bearing_x,
				baseline_y - cg->bearing_y,
				cg->w,
				cg->h
			};
			SDL_RenderCopy(g_ui.ren, cg->tex, NULL, &dst);
		}
		pen_x += cg->advance;
	}
}

int ui_draw_wrapped_text(int x, int y, int max_w, int max_h, int size_px,
                         UiColor c, const char *text, int scroll_y)
{
	if (!g_ui.ft_ready || !text)
		return 0;

	SDL_Rect clip = { x, y, max_w, max_h };
	SDL_RenderSetClipRect(g_ui.ren, &clip);

	int line_h = size_px + 6;
	int cur_y = y - scroll_y;
	int total_h = 0;

	const char *p = text;
	while (*p) {
		/* Measure one line up to '\n' or word wrap at max_w */
		const char *line_start = p;
		const char *last_space = NULL;
		int pen_x = 0;

		while (*p && *p != '\n') {
			const char *before = p;
			uint32_t cp = next_utf8(&p);
			if (cp == '\r')
				continue;
			if (cp == ' ' || cp == '\t')
				last_space = before;

			CachedGlyph *cg = get_glyph(cp, size_px);
			int adv = cg ? cg->advance : (size_px / 2);
			if (pen_x + adv > max_w && before > line_start) {
				if (last_space && last_space > line_start) {
					p = last_space;
				} else {
					p = before;
				}
				break;
			}
			pen_x += adv;
		}

		const char *line_end = p;
		if (*p == '\n' || *p == ' ')
			p++;

		/* Render line if visible inside clip rect */
		if (cur_y + line_h >= y && cur_y <= y + max_h) {
			int draw_x = x;
			int baseline_y = cur_y + size_px;
			const char *q = line_start;
			while (q < line_end) {
				uint32_t cp = next_utf8(&q);
				if (cp == '\r' || cp == '\t')
					cp = ' ';
				CachedGlyph *cg = get_glyph(cp, size_px);
				if (!cg)
					continue;
				if (cg->tex) {
					SDL_SetTextureColorMod(cg->tex, c.r, c.g, c.b);
					SDL_SetTextureAlphaMod(cg->tex, c.a);
					SDL_Rect dst = {
						draw_x + cg->bearing_x,
						baseline_y - cg->bearing_y,
						cg->w,
						cg->h
					};
					SDL_RenderCopy(g_ui.ren, cg->tex, NULL, &dst);
				}
				draw_x += cg->advance;
			}
		}
		cur_y += line_h;
		total_h += line_h;
	}

	SDL_RenderSetClipRect(g_ui.ren, NULL);
	return total_h;
}

int ui_point_in_rect(int px, int py, UiRect r)
{
	return (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h);
}
