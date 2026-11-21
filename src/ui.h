#ifndef VITAMAIL_UI_H
#define VITAMAIL_UI_H

#include <SDL2/SDL.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VITA_SCREEN_W 960
#define VITA_SCREEN_H 544

typedef struct UiColor {
	uint8_t r, g, b, a;
} UiColor;

typedef struct UiRect {
	int x, y, w, h;
} UiRect;

int ui_init(SDL_Renderer *renderer, const char *font_path);
void ui_shutdown(void);

void ui_clear(UiColor c);
void ui_fill_rect(int x, int y, int w, int h, UiColor c);
void ui_draw_rect(int x, int y, int w, int h, UiColor c);
void ui_fill_circle_dot(int cx, int cy, int r, UiColor c);

/* Draw single-line UTF-8 text truncated with '...' if wider than max_w (0 = no clip). */
void ui_draw_text(int x, int y, int size_px, UiColor c, const char *text, int max_w);

/* Measure single-line UTF-8 text width in pixels. */
int ui_measure_text(int size_px, const char *text);

/* Draw single-line UTF-8 text with optional bold, underline, strikethrough. */
void ui_draw_text_styled(int x, int y, int size_px, UiColor c, const char *text, int max_w,
                         int bold, int underline, int strike);

/* Draw word-wrapped UTF-8 text inside rect starting at y - scroll_y.
 * Returns total content height in pixels. */
int ui_draw_wrapped_text(int x, int y, int max_w, int max_h, int size_px,
                         UiColor c, const char *text, int scroll_y);

int ui_point_in_rect(int px, int py, UiRect r);

#ifdef __cplusplus
}
#endif

#endif /* VITAMAIL_UI_H */
