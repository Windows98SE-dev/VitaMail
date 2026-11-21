#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_init_default(MailAccountConfig *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	config_apply_preset(cfg, PRESET_GMAIL);
}

const char *config_preset_name(MailPreset preset)
{
	switch (preset) {
	case PRESET_GMAIL:    return "Gmail (App Password)";
	case PRESET_ICLOUD:   return "iCloud Mail";
	case PRESET_OUTLOOK:  return "Outlook / Hotmail";
	case PRESET_YAHOO:    return "Yahoo Mail";
	case PRESET_FASTMAIL: return "Fastmail";
	case PRESET_CUSTOM:
	default:              return "Custom IMAP/SMTP";
	}
}

void config_apply_preset(MailAccountConfig *cfg, MailPreset preset)
{
	cfg->preset = preset;
	switch (preset) {
	case PRESET_GMAIL:
		snprintf(cfg->imap_host, sizeof(cfg->imap_host), "imap.gmail.com");
		cfg->imap_port = 993;
		cfg->imap_starttls = 0;
		snprintf(cfg->smtp_host, sizeof(cfg->smtp_host), "smtp.gmail.com");
		cfg->smtp_port = 465;
		cfg->smtp_starttls = 0;
		break;
	case PRESET_ICLOUD:
		snprintf(cfg->imap_host, sizeof(cfg->imap_host), "imap.mail.me.com");
		cfg->imap_port = 993;
		cfg->imap_starttls = 0;
		snprintf(cfg->smtp_host, sizeof(cfg->smtp_host), "smtp.mail.me.com");
		cfg->smtp_port = 587;
		cfg->smtp_starttls = 1;
		break;
	case PRESET_OUTLOOK:
		snprintf(cfg->imap_host, sizeof(cfg->imap_host), "outlook.office365.com");
		cfg->imap_port = 993;
		cfg->imap_starttls = 0;
		snprintf(cfg->smtp_host, sizeof(cfg->smtp_host), "smtp.office365.com");
		cfg->smtp_port = 587;
		cfg->smtp_starttls = 1;
		break;
	case PRESET_YAHOO:
		snprintf(cfg->imap_host, sizeof(cfg->imap_host), "imap.mail.yahoo.com");
		cfg->imap_port = 993;
		cfg->imap_starttls = 0;
		snprintf(cfg->smtp_host, sizeof(cfg->smtp_host), "smtp.mail.yahoo.com");
		cfg->smtp_port = 465;
		cfg->smtp_starttls = 0;
		break;
	case PRESET_FASTMAIL:
		snprintf(cfg->imap_host, sizeof(cfg->imap_host), "imap.fastmail.com");
		cfg->imap_port = 993;
		cfg->imap_starttls = 0;
		snprintf(cfg->smtp_host, sizeof(cfg->smtp_host), "smtp.fastmail.com");
		cfg->smtp_port = 465;
		cfg->smtp_starttls = 0;
		break;
	case PRESET_CUSTOM:
	default:
		if (cfg->imap_port == 0) cfg->imap_port = 993;
		if (cfg->smtp_port == 0) cfg->smtp_port = 465;
		break;
	}
}

int config_load(MailAccountConfig *cfg, const char *path)
{
	config_init_default(cfg);
	FILE *f = fopen(path ? path : VITAMAIL_CONFIG_PATH, "r");
	if (!f)
		return -1;

	char line[512];
	while (fgets(line, sizeof(line), f)) {
		size_t n = strlen(line);
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = '\0';
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		const char *k = line;
		const char *v = eq + 1;
		if (strcmp(k, "preset") == 0) {
			int p = atoi(v);
			if (p >= 0 && p < PRESET_COUNT)
				cfg->preset = (MailPreset)p;
		} else if (strcmp(k, "email") == 0) {
			snprintf(cfg->email, sizeof(cfg->email), "%s", v);
		} else if (strcmp(k, "password") == 0) {
			snprintf(cfg->password, sizeof(cfg->password), "%s", v);
		} else if (strcmp(k, "imap_host") == 0) {
			snprintf(cfg->imap_host, sizeof(cfg->imap_host), "%s", v);
		} else if (strcmp(k, "imap_port") == 0) {
			cfg->imap_port = (uint16_t)atoi(v);
		} else if (strcmp(k, "imap_starttls") == 0) {
			cfg->imap_starttls = atoi(v);
		} else if (strcmp(k, "smtp_host") == 0) {
			snprintf(cfg->smtp_host, sizeof(cfg->smtp_host), "%s", v);
		} else if (strcmp(k, "smtp_port") == 0) {
			cfg->smtp_port = (uint16_t)atoi(v);
		} else if (strcmp(k, "smtp_starttls") == 0) {
			cfg->smtp_starttls = atoi(v);
		}
	}
	fclose(f);
	return 0;
}

int config_save(const MailAccountConfig *cfg, const char *path)
{
	FILE *f = fopen(path ? path : VITAMAIL_CONFIG_PATH, "w");
	if (!f)
		return -1;
	fprintf(f, "preset=%d\n", (int)cfg->preset);
	fprintf(f, "email=%s\n", cfg->email);
	fprintf(f, "password=%s\n", cfg->password);
	fprintf(f, "imap_host=%s\n", cfg->imap_host);
	fprintf(f, "imap_port=%u\n", (unsigned)cfg->imap_port);
	fprintf(f, "imap_starttls=%d\n", cfg->imap_starttls);
	fprintf(f, "smtp_host=%s\n", cfg->smtp_host);
	fprintf(f, "smtp_port=%u\n", (unsigned)cfg->smtp_port);
	fprintf(f, "smtp_starttls=%d\n", cfg->smtp_starttls);
	fclose(f);
	return 0;
}
