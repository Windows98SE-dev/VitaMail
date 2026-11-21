#ifndef VITAMAIL_IME_H
#define VITAMAIL_IME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ImeFieldKind {
	IME_KIND_TEXT = 0,
	IME_KIND_EMAIL,
	IME_KIND_PASSWORD,
	IME_KIND_NUMBER,
	IME_KIND_MULTILINE
} ImeFieldKind;

typedef enum ImePollResult {
	IME_POLL_NONE = 0,
	IME_POLL_RUNNING,
	IME_POLL_CONFIRMED,
	IME_POLL_CANCELLED
} ImePollResult;

int ime_global_init(void);
void ime_global_shutdown(void);

/* Start an on-screen keyboard dialog. Returns 0 on success, -1 on error. */
int ime_start(const char *title_utf8, const char *initial_utf8,
              ImeFieldKind kind, size_t max_chars);

/* Poll IME dialog state. When IME_POLL_CONFIRMED is returned, out_utf8 receives the text. */
ImePollResult ime_poll(char *out_utf8, size_t out_cap);

int ime_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* VITAMAIL_IME_H */
