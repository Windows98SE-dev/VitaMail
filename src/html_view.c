#include "html_view.h"
#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct HtmlStyle {
	int font_px;
	int bold;
	int underline;
	int strike;
	int in_link;
	int in_pre;
	int in_quote;
	int list_depth;
	UiColor color;
} HtmlStyle;

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

static int parse_hex_color(const char *s, UiColor *out)
{
	if (*s == '#')
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
		/* Keep text readable on dark VitaMail background */
		int lum = (out->r * 299 + out->g * 587 + out->b * 114) / 1000;
		if (lum < 85) {
			out->r = (uint8_t)(out->r + 120 > 240 ? 240 : out->r + 120);
			out->g = (uint8_t)(out->g + 120 > 240 ? 240 : out->g + 120);
			out->b = (uint8_t)(out->b + 120 > 240 ? 240 : out->b + 120);
		}
		return 1;
	}
	return 0;
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
		while (*p && !isspace((unsigned char)*p))
			p++;
	}
}

static size_t decode_entity(const char *p, char *utf8_out)
{
	/* Returns consumed bytes starting at '&', writes UTF-8 to utf8_out */
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

typedef struct RenderCtx {
	SDL_Renderer *ren;
	int base_x, base_y, max_w, max_h, scroll_y;
	int pen_x, pen_y, line_h;
	int last_space;
	HtmlStyle stack[16];
	int sp;
} RenderCtx;

static void ctx_newline(RenderCtx *c, int extra_gap)
{
	int indent = c->stack[c->sp].in_quote * 16 + c->stack[c->sp].list_depth * 18;
	c->pen_x = c->base_x + indent;
	c->pen_y += c->line_h + extra_gap;
	c->line_h = c->stack[c->sp].font_px + 7;
	c->last_space = 1;
}

static void ctx_draw_quote_bar(RenderCtx *c, int y_start, int h)
{
	if (c->stack[c->sp].in_quote <= 0)
		return;
	int screen_y = c->base_y + y_start - c->scroll_y;
	if (screen_y + h >= c->base_y && screen_y <= c->base_y + c->max_h) {
		ui_fill_rect(c->base_x + 4, screen_y, 3, h, (UiColor){ 66, 153, 225, 255 });
	}
}

static void ctx_emit_word(RenderCtx *c, const char *word)
{
	if (!word || !*word)
		return;

	HtmlStyle *st = &c->stack[c->sp];
	int indent = st->in_quote * 16 + st->list_depth * 18;
	int right_edge = c->base_x + c->max_w;
	int w = ui_measure_text(st->font_px, word);

	if (c->pen_x > c->base_x + indent && c->pen_x + w > right_edge) {
		ctx_newline(c, 0);
	}

	int cur_line_h = st->font_px + 7;
	if (cur_line_h > c->line_h)
		c->line_h = cur_line_h;

	int draw_y = c->base_y + c->pen_y - c->scroll_y;
	if (draw_y + c->line_h >= c->base_y && draw_y <= c->base_y + c->max_h) {
		ctx_draw_quote_bar(c, c->pen_y, c->line_h);
		ui_draw_text_styled(c->pen_x, draw_y, st->font_px, st->color, word, 0,
		                    st->bold, st->underline, st->strike);
	}
	c->pen_x += w;
	c->last_space = 0;
}

static void ctx_emit_space(RenderCtx *c)
{
	if (c->last_space)
		return;
	HtmlStyle *st = &c->stack[c->sp];
	int sw = ui_measure_text(st->font_px, " ");
	c->pen_x += sw;
	c->last_space = 1;
}

int html_view_render(SDL_Renderer *ren, int x, int y, int max_w, int max_h,
                     const char *html, const MimeMessage *msg, int scroll_y)
{
	if (!html)
		return 0;

	SDL_Rect clip = { x, y, max_w, max_h };
	SDL_RenderSetClipRect(ren, &clip);

	RenderCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.ren = ren;
	ctx.base_x = x;
	ctx.base_y = y;
	ctx.max_w = max_w;
	ctx.max_h = max_h;
	ctx.scroll_y = scroll_y;
	ctx.pen_x = x;
	ctx.pen_y = 0;
	ctx.line_h = 23;
	ctx.last_space = 1;

	ctx.stack[0] = (HtmlStyle){
		.font_px = 16,
		.bold = 0,
		.underline = 0,
		.strike = 0,
		.in_link = 0,
		.in_pre = 0,
		.in_quote = 0,
		.list_depth = 0,
		.color = { 236, 240, 248, 255 }
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

			/* HTML comment */
			if (strncmp(p, "<!--", 4) == 0) {
				const char *endc = strstr(p + 4, "-->");
				p = endc ? (endc + 3) : (p + strlen(p));
				continue;
			}

			const char *gt = strchr(p + 1, '>');
			if (!gt)
				break;

			size_t tlen = (size_t)(gt - (p + 1));
			char tag_raw[512];
			if (tlen >= sizeof(tag_raw)) tlen = sizeof(tag_raw) - 1;
			memcpy(tag_raw, p + 1, tlen);
			tag_raw[tlen] = '\0';
			p = gt + 1;

			/* Extract tag name */
			char tname[32];
			size_t ni = 0;
			const char *tp = tag_raw;
			while (*tp && isspace((unsigned char)*tp)) tp++;
			int is_close = (*tp == '/');
			if (is_close) tp++;
			while (*tp && !isspace((unsigned char)*tp) && *tp != '/' && ni + 1 < sizeof(tname)) {
				tname[ni++] = (char)tolower((unsigned char)*tp++);
			}
			tname[ni] = '\0';

			if (ci_eq(tname, "head") || ci_eq(tname, "style") ||
			    ci_eq(tname, "script") || ci_eq(tname, "title")) {
				skip_block = is_close ? 0 : 1;
				continue;
			}
			if (skip_block)
				continue;

			if (ci_eq(tname, "br")) {
				ctx_newline(&ctx, 0);
			} else if (ci_eq(tname, "hr")) {
				if (!ctx.last_space || ctx.pen_x > ctx.base_x)
					ctx_newline(&ctx, 4);
				int sy = ctx.base_y + ctx.pen_y - ctx.scroll_y;
				if (sy >= ctx.base_y && sy <= ctx.base_y + ctx.max_h)
					ui_fill_rect(ctx.base_x, sy, ctx.max_w, 1, (UiColor){ 64, 78, 108, 255 });
				ctx.pen_y += 8;
			} else if (ci_eq(tname, "img")) {
				char alt[96], src[128];
				extract_attr(tag_raw, "alt", alt, sizeof(alt));
				extract_attr(tag_raw, "src", src, sizeof(src));
				char badge[140];
				if (alt[0])
					snprintf(badge, sizeof(badge), "[Image: %.100s]", alt);
				else if (strncmp(src, "cid:", 4) == 0 && msg)
					snprintf(badge, sizeof(badge), "[Inline Image: %.100s]", src + 4);
				else
					snprintf(badge, sizeof(badge), "[Image]");
				HtmlStyle saved = ctx.stack[ctx.sp];
				ctx.stack[ctx.sp].color = (UiColor){ 148, 163, 184, 255 };
				ctx_emit_word(&ctx, badge);
				ctx_emit_space(&ctx);
				ctx.stack[ctx.sp] = saved;
			} else if (is_close) {
				if (ci_eq(tname, "p") || ci_eq(tname, "div") || ci_eq(tname, "tr") ||
				    ci_eq(tname, "li") || ci_eq(tname, "blockquote") ||
				    ci_eq(tname, "pre") || (tname[0] == 'h' && tname[1] >= '1' && tname[1] <= '6')) {
					ctx_newline(&ctx, (tname[0] == 'h' || ci_eq(tname, "p")) ? 6 : 2);
				}
				if (ctx.sp > 0 && !ci_eq(tname, "li") && !ci_eq(tname, "tr") && !ci_eq(tname, "td"))
					ctx.sp--;
			} else {
				/* Opening tag */
				int is_block = (ci_eq(tname, "p") || ci_eq(tname, "div") || ci_eq(tname, "tr") ||
				                ci_eq(tname, "blockquote") || ci_eq(tname, "pre") ||
				                ci_eq(tname, "ul") || ci_eq(tname, "ol") ||
				                (tname[0] == 'h' && tname[1] >= '1' && tname[1] <= '6'));
				if (is_block && ctx.pen_x > ctx.base_x)
					ctx_newline(&ctx, ci_eq(tname, "p") ? 4 : 0);

				if (ci_eq(tname, "li")) {
					if (ctx.pen_x > ctx.base_x)
						ctx_newline(&ctx, 0);
					ctx_emit_word(&ctx, "•");
					ctx_emit_space(&ctx);
					continue;
				}
				if (ci_eq(tname, "td") || ci_eq(tname, "th")) {
					ctx_emit_space(&ctx);
					continue;
				}
				if (ci_eq(tname, "tr"))
					continue;

				if (ctx.sp + 1 < 16) {
					ctx.stack[ctx.sp + 1] = ctx.stack[ctx.sp];
					ctx.sp++;
				}
				HtmlStyle *st = &ctx.stack[ctx.sp];

				if (tname[0] == 'h' && tname[1] >= '1' && tname[1] <= '6' && tname[2] == '\0') {
					int lvl = tname[1] - '0';
					st->font_px = (lvl == 1) ? 24 : (lvl == 2) ? 21 : (lvl == 3) ? 18 : 16;
					st->bold = 1;
					st->color = (UiColor){ 255, 255, 255, 255 };
				} else if (ci_eq(tname, "b") || ci_eq(tname, "strong") || ci_eq(tname, "th")) {
					st->bold = 1;
				} else if (ci_eq(tname, "u")) {
					st->underline = 1;
				} else if (ci_eq(tname, "s") || ci_eq(tname, "del") || ci_eq(tname, "strike")) {
					st->strike = 1;
				} else if (ci_eq(tname, "a")) {
					st->in_link = 1;
					st->underline = 1;
					st->color = (UiColor){ 99, 179, 237, 255 };
				} else if (ci_eq(tname, "blockquote")) {
					st->in_quote++;
					st->color = (UiColor){ 176, 190, 212, 255 };
				} else if (ci_eq(tname, "ul") || ci_eq(tname, "ol")) {
					st->list_depth++;
				} else if (ci_eq(tname, "pre") || ci_eq(tname, "code")) {
					st->in_pre = 1;
					st->color = (UiColor){ 163, 230, 185, 255 };
				}

				/* Check inline style="color: #RRGGBB" */
				char style_attr[256];
				extract_attr(tag_raw, "style", style_attr, sizeof(style_attr));
				if (style_attr[0]) {
					const char *cp_col = strstr(style_attr, "color:");
					if (cp_col) {
						cp_col += 6;
						while (*cp_col == ' ') cp_col++;
						parse_hex_color(cp_col, &st->color);
					}
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
			ctx_newline(&ctx, 0);
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

	SDL_RenderSetClipRect(ren, NULL);
	return ctx.pen_y + ctx.line_h;
}
