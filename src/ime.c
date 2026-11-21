#include "ime.h"

#include <stdint.h>
#include <string.h>

#ifdef __vita__
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/clib.h>
#include <psp2/sysmodule.h>
#endif

static int g_ime_active = 0;
static int g_ime_multiline = 0;

#ifdef __vita__
static uint16_t g_title_u16[SCE_IME_DIALOG_MAX_TITLE_LENGTH + 1];
static uint16_t g_init_u16[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static uint16_t g_input_u16[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];

static size_t utf8_to_utf16(const char *src, uint16_t *dst, size_t dst_cap)
{
	if (!src || dst_cap == 0) {
		if (dst_cap > 0) dst[0] = 0;
		return 0;
	}
	size_t o = 0;
	const unsigned char *p = (const unsigned char *)src;
	while (*p && o + 1 < dst_cap) {
		uint32_t cp = 0;
		if (*p < 0x80) {
			cp = *p++;
		} else if ((*p & 0xE0) == 0xC0 && p[1]) {
			cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
			p += 2;
		} else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
			cp = ((uint32_t)(p[0] & 0x0F) << 12) |
			     ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
			p += 3;
		} else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
			cp = ((uint32_t)(p[0] & 0x07) << 18) |
			     ((uint32_t)(p[1] & 0x3F) << 12) |
			     ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
			p += 4;
		} else {
			cp = '?';
			p++;
		}
		if (cp <= 0xFFFF) {
			dst[o++] = (uint16_t)cp;
		} else if (o + 2 < dst_cap) {
			cp -= 0x10000;
			dst[o++] = (uint16_t)(0xD800 + (cp >> 10));
			dst[o++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
		}
	}
	dst[o] = 0;
	return o;
}

static size_t utf16_to_utf8(const uint16_t *src, char *dst, size_t dst_cap)
{
	if (!src || dst_cap == 0) {
		if (dst_cap > 0) dst[0] = '\0';
		return 0;
	}
	size_t o = 0;
	while (*src && o + 1 < dst_cap) {
		uint32_t cp = *src++;
		if (cp >= 0xD800 && cp <= 0xDBFF && *src >= 0xDC00 && *src <= 0xDFFF) {
			uint32_t lo = *src++;
			cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
		}
		if (cp < 0x80) {
			dst[o++] = (char)cp;
		} else if (cp < 0x800 && o + 2 < dst_cap) {
			dst[o++] = (char)(0xC0 | (cp >> 6));
			dst[o++] = (char)(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000 && o + 3 < dst_cap) {
			dst[o++] = (char)(0xE0 | (cp >> 12));
			dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			dst[o++] = (char)(0x80 | (cp & 0x3F));
		} else if (o + 4 < dst_cap) {
			dst[o++] = (char)(0xF0 | (cp >> 18));
			dst[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
			dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			dst[o++] = (char)(0x80 | (cp & 0x3F));
		} else {
			break;
		}
	}
	dst[o] = '\0';
	return o;
}
#endif

int ime_global_init(void)
{
#ifdef __vita__
	sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
#endif
	g_ime_active = 0;
	return 0;
}

void ime_global_shutdown(void)
{
#ifdef __vita__
	if (g_ime_active) {
		sceImeDialogAbort();
		sceImeDialogTerm();
		g_ime_active = 0;
	}
	sceSysmoduleUnloadModule(SCE_SYSMODULE_IME);
#endif
}

int ime_start(const char *title_utf8, const char *initial_utf8,
              ImeFieldKind kind, size_t max_chars)
{
#ifdef __vita__
	if (g_ime_active)
		return -1;

	SceImeDialogParam param;
	sceImeDialogParamInit(&param);

	utf8_to_utf16(title_utf8 ? title_utf8 : "Input", g_title_u16,
	              SCE_IME_DIALOG_MAX_TITLE_LENGTH);
	utf8_to_utf16(initial_utf8 ? initial_utf8 : "", g_init_u16,
	              SCE_IME_DIALOG_MAX_TEXT_LENGTH);
	memset(g_input_u16, 0, sizeof(g_input_u16));

	if (max_chars == 0 || max_chars > SCE_IME_DIALOG_MAX_TEXT_LENGTH)
		max_chars = SCE_IME_DIALOG_MAX_TEXT_LENGTH;

	param.supportedLanguages = 0;
	param.languagesForced = SCE_FALSE;
	g_ime_multiline = (kind == IME_KIND_MULTILINE) ? 1 : 0;
	param.dialogMode = g_ime_multiline
	                       ? SCE_IME_DIALOG_DIALOG_MODE_DEFAULT
	                       : SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
	param.textBoxMode = (kind == IME_KIND_PASSWORD)
	                        ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD
	                        : SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
	switch (kind) {
	case IME_KIND_EMAIL:
		param.type = SCE_IME_TYPE_MAIL;
		break;
	case IME_KIND_NUMBER:
		param.type = SCE_IME_TYPE_NUMBER;
		break;
	case IME_KIND_MULTILINE:
		param.type = SCE_IME_TYPE_DEFAULT;
		param.option = SCE_IME_OPTION_MULTILINE;
		break;
	default:
		param.type = SCE_IME_TYPE_DEFAULT;
		break;
	}

	param.title = g_title_u16;
	param.maxTextLength = (SceUInt32)max_chars;
	param.initialText = g_init_u16;
	param.inputTextBuffer = g_input_u16;

	if (sceImeDialogInit(&param) < 0)
		return -1;

	g_ime_active = 1;
	return 0;
#else
	(void)title_utf8;
	(void)initial_utf8;
	(void)kind;
	(void)max_chars;
	return -1;
#endif
}

ImePollResult ime_poll(char *out_utf8, size_t out_cap)
{
#ifdef __vita__
	if (!g_ime_active)
		return IME_POLL_NONE;

	SceCommonDialogStatus st = sceImeDialogGetStatus();
	if (st == SCE_COMMON_DIALOG_STATUS_RUNNING)
		return IME_POLL_RUNNING;

	if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) {
		SceImeDialogResult res;
		memset(&res, 0, sizeof(res));
		sceImeDialogGetResult(&res);
		sceImeDialogTerm();
		g_ime_active = 0;

		if (res.button == SCE_IME_DIALOG_BUTTON_ENTER ||
		    (g_ime_multiline && res.button == SCE_IME_DIALOG_BUTTON_CLOSE)) {
			utf16_to_utf8(g_input_u16, out_utf8, out_cap);
			return IME_POLL_CONFIRMED;
		}
		return IME_POLL_CANCELLED;
	}

	sceImeDialogTerm();
	g_ime_active = 0;
	return IME_POLL_CANCELLED;
#else
	(void)out_utf8;
	(void)out_cap;
	return IME_POLL_NONE;
#endif
}

int ime_is_active(void)
{
	return g_ime_active;
}
