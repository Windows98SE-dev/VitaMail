#ifndef VITAMAIL_HTML_VIEW_H
#define VITAMAIL_HTML_VIEW_H

#include <SDL2/SDL.h>
#include "mime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Render modern HTML email inside clip rectangle (x, y, max_w, max_h) scrolled by scroll_y.
 * Supports <style> CSS rules, inline style="..." attributes (color, background-color,
 * font-size, font-weight, text-decoration), headings, lists, blockquotes, code blocks,
 * links, entities, and inline/remote images (cid:, data:, http://, https://).
 * Returns total content height in pixels. */
int html_view_render(SDL_Renderer *ren, int x, int y, int max_w, int max_h,
                     const char *html, const MimeMessage *msg, int scroll_y);

/* Free cached textures and stop background image fetches (call when closing/switching message). */
void html_view_clear_cache(void);

#ifdef __cplusplus
}
#endif

#endif /* VITAMAIL_HTML_VIEW_H */
