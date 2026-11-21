#include "app.h"
#include "ime.h"
#include "net_tls.h"
#include "ui.h"

#include <SDL2/SDL.h>
#include <stdio.h>

#ifdef __vita__
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/processmgr.h>
extern void vita_runtime_init(void);
#define FONT_PATH "app0:assets/font.ttf"
#else
#define FONT_PATH "assets/font.ttf"
#endif

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

#ifdef __vita__
	vita_runtime_init();
	SceAppUtilInitParam app_init_param;
	SceAppUtilBootParam app_boot_param;
	memset(&app_init_param, 0, sizeof(app_init_param));
	memset(&app_boot_param, 0, sizeof(app_boot_param));
	sceAppUtilInit(&app_init_param, &app_boot_param);
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0)
		return 1;

	SDL_JoystickOpen(0);

	SDL_Window *win = SDL_CreateWindow("VitaMail",
	                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                                   VITA_SCREEN_W, VITA_SCREEN_H, 0);
	if (!win) {
		SDL_Quit();
		return 1;
	}

	SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren) {
		ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
	}
	if (!ren) {
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}

	net_tls_global_init(NULL);
	ime_global_init();
	ui_init(ren, FONT_PATH);
	app_init(ren);

	while (!app_should_quit()) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			app_handle_event(&ev);
		}

		app_update();
		app_render();
		SDL_RenderPresent(ren);

#ifdef __vita__
		if (ime_is_active()) {
			SceCommonDialogUpdateParam up;
			memset(&up, 0, sizeof(up));
			sceCommonDialogUpdate(&up);
		}
#endif
	}

	app_shutdown();
	ui_shutdown();
	ime_global_shutdown();
	net_tls_global_shutdown();

	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();

#ifdef __vita__
	sceKernelExitProcess(0);
#endif
	return 0;
}
