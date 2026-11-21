#ifndef VITAMAIL_HTML_VIEW_H
#define VITAMAIL_HTML_VIEW_H

#include <SDL2/SDL.h>
#include "mime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Render modern HTML email inside clip rectangle (x, y, max_w, max_h) scrolled by scroll_y.
 * Supports headings, paragraphs, lists, blockquotes, code blocks, links, inline styles,
 * horizontal rules, entities, and image placeholders.
 * Returns total content height in pixels. */
int html_view_render(SDL_Renderer *ren, int x, int y, int max_w, int max_h,
                     const char *html, const MimeMessage *msg, int scroll_y);

#ifdef __cplusplus
}
#endif

#endif /* VITAMAIL_HTML_VIEW_H */
