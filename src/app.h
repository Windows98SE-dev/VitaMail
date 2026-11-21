#ifndef VITAMAIL_APP_H
#define VITAMAIL_APP_H

#include <SDL2/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

int app_init(SDL_Renderer *ren);
void app_shutdown(void);
void app_handle_event(const SDL_Event *ev);
void app_update(void);
void app_render(void);
int app_should_quit(void);

#ifdef __cplusplus
}
#endif

#endif /* VITAMAIL_APP_H */
