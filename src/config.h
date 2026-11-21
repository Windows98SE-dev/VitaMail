#ifndef VITAMAIL_CONFIG_H
#define VITAMAIL_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __vita__
#define VITAMAIL_CONFIG_PATH "ux0:data/VitaMail/accounts.cfg"
#else
#define VITAMAIL_CONFIG_PATH "vitamail_accounts.cfg"
#endif

typedef enum MailPreset {
	PRESET_CUSTOM = 0,
	PRESET_GMAIL,
	PRESET_ICLOUD,
	PRESET_OUTLOOK,
	PRESET_YAHOO,
	PRESET_FASTMAIL,
	PRESET_COUNT
} MailPreset;

typedef struct MailAccountConfig {
	MailPreset preset;
	char email[128];
	char password[128];
	char imap_host[128];
	uint16_t imap_port;
	int imap_starttls; /* 0 = IMAPS (993), 1 = STARTTLS (143) */
	char smtp_host[128];
	uint16_t smtp_port;
	int smtp_starttls; /* 0 = SMTPS (465), 1 = STARTTLS (587) */
} MailAccountConfig;

void config_init_default(MailAccountConfig *cfg);
void config_apply_preset(MailAccountConfig *cfg, MailPreset preset);
const char *config_preset_name(MailPreset preset);

int config_load(MailAccountConfig *cfg, const char *path);
int config_save(const MailAccountConfig *cfg, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* VITAMAIL_CONFIG_H */
