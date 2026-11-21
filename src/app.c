#include "app.h"
#include "config.h"
#include "html_view.h"
#include "imap.h"
#include "ime.h"
#include "img_decode.h"
#include "mime.h"
#include "net_tls.h"
#include "smtp.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __vita__
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <dirent.h>
#endif

typedef enum ScreenMode {
	SCREEN_INBOX = 0,
	SCREEN_READER,
	SCREEN_COMPOSE,
	SCREEN_SETTINGS
} ScreenMode;

typedef enum SortMode {
	SORT_NEWEST = 0,
	SORT_OLDEST,
	SORT_SENDER,
	SORT_SUBJECT,
	SORT_UNREAD,
	SORT_COUNT
} SortMode;

#define FP_MAX_ENTRIES 128

typedef struct FileEntry {
	char name[128];
	int is_dir;
	uint32_t size;
} FileEntry;

typedef enum WorkerJobType {
	JOB_NONE = 0,
	JOB_SYNC_FOLDER,
	JOB_FETCH_MESSAGE,
	JOB_SEND_EMAIL,
	JOB_TOGGLE_SEEN,
	JOB_DELETE_MESSAGE
} WorkerJobType;

typedef enum ImeTarget {
	IME_TGT_NONE = 0,
	IME_TGT_SET_EMAIL,
	IME_TGT_SET_PASS,
	IME_TGT_SET_IMAP_HOST,
	IME_TGT_SET_IMAP_PORT,
	IME_TGT_SET_SMTP_HOST,
	IME_TGT_SET_SMTP_PORT,
	IME_TGT_CMP_TO,
	IME_TGT_CMP_SUBJECT,
	IME_TGT_CMP_BODY
} ImeTarget;

/* Palette */
static const UiColor COL_BG        = { 15, 20, 30, 255 };
static const UiColor COL_PANEL     = { 24, 31, 46, 255 };
static const UiColor COL_PANEL_ALT = { 30, 39, 58, 255 };
static const UiColor COL_SEL       = { 34, 82, 148, 255 };
static const UiColor COL_BORDER    = { 52, 65, 92, 255 };
static const UiColor COL_ACCENT    = { 66, 153, 225, 255 };
static const UiColor COL_GREEN     = { 46, 184, 114, 255 };
static const UiColor COL_RED       = { 224, 79, 95, 255 };
static const UiColor COL_TEXT      = { 236, 240, 248, 255 };
static const UiColor COL_MUTED     = { 148, 163, 184, 255 };

static struct {
	int quit;
	ScreenMode screen;
	int show_tls_modal;

	MailAccountConfig cfg;
	MailAccountConfig edit_cfg;

	/* Folders & messages */
	ImapFolder folders[IMAP_MAX_FOLDERS];
	int folder_count;
	int selected_folder;

	ImapHeader headers[IMAP_MAX_HEADERS];
	int header_count;
	int selected_msg;
	int list_scroll;
	SortMode sort_mode;

	MimeMessage open_msg;
	uint32_t open_uid;
	int reader_scroll;
	int reader_total_h;
	int reader_html_mode;
	UiRect att_chip_rects[MIME_MAX_ATTACHMENTS];

	/* Attachment preview modal */
	SDL_Renderer *ren;
	int show_att_preview;
	int att_preview_idx; /* >= 0: open_msg.attachments[idx], -1: Compose cmp_att_path */
	SDL_Texture *att_tex;
	int att_img_w;
	int att_img_h;
	int att_text_scroll;
	unsigned char *att_disk_data;
	size_t att_disk_size;

	/* Compose state */
	char cmp_to[256];
	char cmp_subject[256];
	char cmp_body[4096];
	char cmp_reply_id[256];
	char cmp_att_path[512];
	char cmp_att_name[128];
	uint32_t cmp_att_size;
	int cmp_focus; /* 0=To, 1=Subject, 2=Body, 3=Attach, 4=Send, 5=Cancel */

	/* File picker modal */
	int show_file_picker;
	char fp_dir[512];
	FileEntry fp_entries[FP_MAX_ENTRIES];
	int fp_count;
	int fp_sel;
	int fp_scroll;

	/* Settings state */
	int set_focus; /* 0=Preset, 1=Email, 2=Pass, 3=ImapHost, 4=ImapPort, 5=ImapTLS,
	                  6=SmtpHost, 7=SmtpPort, 8=SmtpTLS, 9=Save&Connect, 10=Back */

	/* IME */
	ImeTarget ime_target;
	char ime_buf[4096];

	/* Background worker */
	SDL_mutex *lock;
	SDL_Thread *worker;
	WorkerJobType job_type;
	uint32_t job_uid;
	int job_flag;
	char job_folder[128];
	int busy;
	char status_msg[256];
	int status_is_err;

	/* Stick repeat timer */
	uint32_t last_nav_tick;
} g_app;

static void set_status(const char *msg, int is_err)
{
	SDL_LockMutex(g_app.lock);
	snprintf(g_app.status_msg, sizeof(g_app.status_msg), "%s", msg);
	g_app.status_is_err = is_err;
	SDL_UnlockMutex(g_app.lock);
}

static const char *sort_mode_label(SortMode m)
{
	switch (m) {
	case SORT_NEWEST:  return "Sort: Newest";
	case SORT_OLDEST:  return "Sort: Oldest";
	case SORT_SENDER:  return "Sort: Sender";
	case SORT_SUBJECT: return "Sort: Subject";
	case SORT_UNREAD:  return "Sort: Unread";
	default:           return "Sort: Newest";
	}
}

static int ci_cmp(const char *a, const char *b)
{
	for (;; a++, b++) {
		int ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : (unsigned char)*a;
		int cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : (unsigned char)*b;
		if (ca != cb)
			return ca - cb;
		if (ca == 0)
			return 0;
	}
}

static int cmp_header(const void *pa, const void *pb)
{
	const ImapHeader *a = (const ImapHeader *)pa;
	const ImapHeader *b = (const ImapHeader *)pb;
	switch (g_app.sort_mode) {
	case SORT_OLDEST:
		return (a->uid > b->uid) - (a->uid < b->uid);
	case SORT_SENDER: {
		int c = ci_cmp(a->from, b->from);
		return c != 0 ? c : ((b->uid > a->uid) - (b->uid < a->uid));
	}
	case SORT_SUBJECT: {
		int c = ci_cmp(a->subject, b->subject);
		return c != 0 ? c : ((b->uid > a->uid) - (b->uid < a->uid));
	}
	case SORT_UNREAD:
		if (a->seen != b->seen)
			return a->seen - b->seen; /* unseen (0) first */
		return (b->uid > a->uid) - (b->uid < a->uid);
	case SORT_NEWEST:
	default:
		return (b->uid > a->uid) - (b->uid < a->uid);
	}
}

static void sort_headers_locked(void)
{
	if (g_app.header_count > 1)
		qsort(g_app.headers, (size_t)g_app.header_count, sizeof(ImapHeader), cmp_header);
	g_app.selected_msg = 0;
	g_app.list_scroll = 0;
}

static int read_attachment_file(const char *path, unsigned char **out_data, size_t *out_len)
{
#ifdef __vita__
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return -1;
	SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
	if (sz <= 0 || sz > 8 * 1024 * 1024) {
		sceIoClose(fd);
		return -1;
	}
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	unsigned char *buf = (unsigned char *)malloc((size_t)sz);
	if (!buf) {
		sceIoClose(fd);
		return -1;
	}
	size_t total = 0;
	while (total < (size_t)sz) {
		int r = sceIoRead(fd, buf + total, (SceSize)((size_t)sz - total));
		if (r <= 0)
			break;
		total += (size_t)r;
	}
	sceIoClose(fd);
	*out_data = buf;
	*out_len = total;
	return 0;
#else
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	if (sz <= 0 || sz > 8 * 1024 * 1024) {
		fclose(f);
		return -1;
	}
	fseek(f, 0, SEEK_SET);
	unsigned char *buf = (unsigned char *)malloc((size_t)sz);
	if (!buf) {
		fclose(f);
		return -1;
	}
	size_t n = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	*out_data = buf;
	*out_len = n;
	return 0;
#endif
}

static int worker_thread_fn(void *arg)
{
	(void)arg;
	SDL_LockMutex(g_app.lock);
	WorkerJobType job = g_app.job_type;
	uint32_t uid = g_app.job_uid;
	int flag = g_app.job_flag;
	char folder[128];
	snprintf(folder, sizeof(folder), "%s", g_app.job_folder);
	MailAccountConfig cfg = g_app.cfg;
	SDL_UnlockMutex(g_app.lock);

	char err[256] = {0};

	if (job == JOB_SYNC_FOLDER) {
		if (!net_tls_is_online()) {
			set_status("Wi-Fi is offline. Connect your PS Vita to Wi-Fi in Settings.", 1);
			goto done;
		}
		set_status("Connecting via TLS...", 0);
		ImapSession sess;
		if (imap_connect(&sess, cfg.imap_host, cfg.imap_port, cfg.imap_starttls,
		                 cfg.email, cfg.password, err, sizeof(err)) != 0) {
			if (strstr(err, "AUTHENTICATIONFAILED") != NULL || strstr(err, "Invalid credentials") != NULL) {
				set_status("Login rejected! Gmail/iCloud/Yahoo require a 16-char App Password (see docs/PROVIDERS.md).", 1);
			} else {
				set_status(err, 1);
			}
			goto done;
		}

		if (g_app.folder_count == 0) {
			ImapFolder ftmp[IMAP_MAX_FOLDERS];
			int fc = imap_list_folders(&sess, ftmp, IMAP_MAX_FOLDERS, err, sizeof(err));
			if (fc > 0) {
				SDL_LockMutex(g_app.lock);
				memcpy(g_app.folders, ftmp, sizeof(ImapFolder) * (size_t)fc);
				g_app.folder_count = fc;
				SDL_UnlockMutex(g_app.lock);
			}
		}

		set_status("Syncing mailbox...", 0);
		if (imap_select_folder(&sess, folder, err, sizeof(err)) != 0) {
			imap_disconnect(&sess);
			set_status(err, 1);
			goto done;
		}

		ImapHeader htmp[IMAP_MAX_HEADERS];
		int hc = imap_fetch_headers(&sess, htmp, IMAP_MAX_HEADERS, err, sizeof(err));
		imap_disconnect(&sess);

		if (hc < 0) {
			set_status(err, 1);
			goto done;
		}

		SDL_LockMutex(g_app.lock);
		memcpy(g_app.headers, htmp, sizeof(ImapHeader) * (size_t)hc);
		g_app.header_count = hc;
		sort_headers_locked();
		snprintf(g_app.status_msg, sizeof(g_app.status_msg),
		         "Synced %d messages in %s", hc, folder);
		g_app.status_is_err = 0;
		SDL_UnlockMutex(g_app.lock);
	} else if (job == JOB_FETCH_MESSAGE) {
		set_status("Fetching message...", 0);
		ImapSession sess;
		if (imap_connect(&sess, cfg.imap_host, cfg.imap_port, cfg.imap_starttls,
		                 cfg.email, cfg.password, err, sizeof(err)) != 0) {
			set_status(err, 1);
			goto done;
		}
		if (imap_select_folder(&sess, folder, err, sizeof(err)) != 0) {
			imap_disconnect(&sess);
			set_status(err, 1);
			goto done;
		}

		MimeMessage msg;
		if (imap_fetch_message(&sess, uid, &msg, err, sizeof(err)) != 0) {
			imap_disconnect(&sess);
			set_status(err, 1);
			goto done;
		}
		imap_set_seen(&sess, uid, 1, err, sizeof(err));
		imap_disconnect(&sess);

		SDL_LockMutex(g_app.lock);
		mime_message_free(&g_app.open_msg);
		g_app.open_msg = msg;
		g_app.open_uid = uid;
		g_app.reader_scroll = 0;
		g_app.screen = SCREEN_READER;
		for (int i = 0; i < g_app.header_count; i++) {
			if (g_app.headers[i].uid == uid)
				g_app.headers[i].seen = 1;
		}
		g_app.status_msg[0] = '\0';
		g_app.status_is_err = 0;
		SDL_UnlockMutex(g_app.lock);
	} else if (job == JOB_SEND_EMAIL) {
		set_status("Sending via SMTP TLS...", 0);
		SDL_LockMutex(g_app.lock);
		char to[256], subj[256], body[4096], rep[256], att_path[512], att_name[128];
		snprintf(to, sizeof(to), "%s", g_app.cmp_to);
		snprintf(subj, sizeof(subj), "%s", g_app.cmp_subject);
		snprintf(body, sizeof(body), "%s", g_app.cmp_body);
		snprintf(rep, sizeof(rep), "%s", g_app.cmp_reply_id);
		snprintf(att_path, sizeof(att_path), "%s", g_app.cmp_att_path);
		snprintf(att_name, sizeof(att_name), "%s", g_app.cmp_att_name);
		SDL_UnlockMutex(g_app.lock);

		unsigned char *att_data = NULL;
		size_t att_len = 0;
		if (att_path[0] != '\0') {
			if (read_attachment_file(att_path, &att_data, &att_len) != 0) {
				set_status("Failed to read attachment (max 8 MiB).", 1);
				goto done;
			}
		}

		SmtpSendRequest req = {
			.host = cfg.smtp_host,
			.port = cfg.smtp_port,
			.use_starttls = cfg.smtp_starttls,
			.user = cfg.email,
			.pass = cfg.password,
			.from = cfg.email,
			.to = to,
			.subject = subj,
			.body = body,
			.in_reply_to = rep,
			.attachment_name = att_name[0] ? att_name : NULL,
			.attachment_data = att_data,
			.attachment_len = att_len
		};
		int send_rc = smtp_send_email(&req, err, sizeof(err));
		free(att_data);
		if (send_rc != 0) {
			set_status(err, 1);
			goto done;
		}
		SDL_LockMutex(g_app.lock);
		g_app.screen = SCREEN_INBOX;
		snprintf(g_app.status_msg, sizeof(g_app.status_msg), "Email sent to %s", to);
		g_app.status_is_err = 0;
		SDL_UnlockMutex(g_app.lock);
	} else if (job == JOB_TOGGLE_SEEN || job == JOB_DELETE_MESSAGE) {
		set_status(job == JOB_DELETE_MESSAGE ? "Deleting message..." : "Updating flags...", 0);
		ImapSession sess;
		if (imap_connect(&sess, cfg.imap_host, cfg.imap_port, cfg.imap_starttls,
		                 cfg.email, cfg.password, err, sizeof(err)) != 0 ||
		    imap_select_folder(&sess, folder, err, sizeof(err)) != 0) {
			imap_disconnect(&sess);
			set_status(err, 1);
			goto done;
		}

		if (job == JOB_TOGGLE_SEEN) {
			if (imap_set_seen(&sess, uid, flag, err, sizeof(err)) == 0) {
				SDL_LockMutex(g_app.lock);
				for (int i = 0; i < g_app.header_count; i++) {
					if (g_app.headers[i].uid == uid)
						g_app.headers[i].seen = flag;
				}
				snprintf(g_app.status_msg, sizeof(g_app.status_msg),
				         flag ? "Marked as read" : "Marked as unread");
				g_app.status_is_err = 0;
				SDL_UnlockMutex(g_app.lock);
			} else {
				set_status(err, 1);
			}
		} else {
			if (imap_delete_message(&sess, uid, err, sizeof(err)) == 0) {
				SDL_LockMutex(g_app.lock);
				for (int i = 0; i < g_app.header_count; i++) {
					if (g_app.headers[i].uid == uid) {
						memmove(&g_app.headers[i], &g_app.headers[i + 1],
						        sizeof(ImapHeader) * (size_t)(g_app.header_count - 1 - i));
						g_app.header_count--;
						if (g_app.selected_msg >= g_app.header_count && g_app.selected_msg > 0)
							g_app.selected_msg--;
						break;
					}
				}
				g_app.screen = SCREEN_INBOX;
				snprintf(g_app.status_msg, sizeof(g_app.status_msg), "Message deleted");
				g_app.status_is_err = 0;
				SDL_UnlockMutex(g_app.lock);
			} else {
				set_status(err, 1);
			}
		}
		imap_disconnect(&sess);
	}

done:
	SDL_LockMutex(g_app.lock);
	g_app.busy = 0;
	SDL_UnlockMutex(g_app.lock);
	return 0;
}

static void start_job(WorkerJobType job, uint32_t uid, int flag)
{
	if (g_app.busy)
		return;
	if (g_app.cfg.email[0] == '\0' || g_app.cfg.password[0] == '\0') {
		set_status("Configure your email & password in Settings (Start)", 1);
		return;
	}
	if (g_app.worker) {
		SDL_WaitThread(g_app.worker, NULL);
		g_app.worker = NULL;
	}

	SDL_LockMutex(g_app.lock);
	g_app.job_type = job;
	g_app.job_uid = uid;
	g_app.job_flag = flag;
	if (g_app.folder_count > 0 && g_app.selected_folder < g_app.folder_count)
		snprintf(g_app.job_folder, sizeof(g_app.job_folder), "%s",
		         g_app.folders[g_app.selected_folder].name);
	else
		snprintf(g_app.job_folder, sizeof(g_app.job_folder), "INBOX");
	g_app.busy = 1;
	SDL_UnlockMutex(g_app.lock);

	g_app.worker = SDL_CreateThread(worker_thread_fn, "MailWorker", NULL);
}

static void begin_ime(ImeTarget tgt, const char *title, const char *init_val,
                      ImeFieldKind kind, size_t max_chars)
{
	if (ime_is_active())
		return;
	g_app.ime_target = tgt;
	memset(g_app.ime_buf, 0, sizeof(g_app.ime_buf));
	if (ime_start(title, init_val, kind, max_chars) != 0) {
		g_app.ime_target = IME_TGT_NONE;
		set_status("On-screen keyboard only available on PS Vita", 1);
	}
}

static int cmp_file_entry(const void *pa, const void *pb)
{
	const FileEntry *a = (const FileEntry *)pa;
	const FileEntry *b = (const FileEntry *)pb;
	if (strcmp(a->name, "..") == 0) return -1;
	if (strcmp(b->name, "..") == 0) return 1;
	if (a->is_dir != b->is_dir)
		return b->is_dir - a->is_dir;
	return ci_cmp(a->name, b->name);
}

static void fp_load_dir(const char *dir)
{
	snprintf(g_app.fp_dir, sizeof(g_app.fp_dir), "%s", dir);
	g_app.fp_count = 0;
	g_app.fp_sel = 0;
	g_app.fp_scroll = 0;

	/* Add parent entry if not at drive root */
	size_t dlen = strlen(dir);
	if (dlen > 4 && strcmp(dir, "ux0:/") != 0 && strcmp(dir, "ux0:") != 0) {
		snprintf(g_app.fp_entries[0].name, 128, "..");
		g_app.fp_entries[0].is_dir = 1;
		g_app.fp_entries[0].size = 0;
		g_app.fp_count = 1;
	}

#ifdef __vita__
	SceUID dfd = sceIoDopen(dir);
	if (dfd >= 0) {
		SceIoDirent ent;
		memset(&ent, 0, sizeof(ent));
		while (g_app.fp_count < FP_MAX_ENTRIES && sceIoDread(dfd, &ent) > 0) {
			if (ent.d_name[0] == '.' && (ent.d_name[1] == '\0' ||
			    (ent.d_name[1] == '.' && ent.d_name[2] == '\0'))) {
				memset(&ent, 0, sizeof(ent));
				continue;
			}
			FileEntry *fe = &g_app.fp_entries[g_app.fp_count++];
			snprintf(fe->name, sizeof(fe->name), "%.127s", ent.d_name);
			fe->is_dir = SCE_S_ISDIR(ent.d_stat.st_mode) ? 1 : 0;
			fe->size = (uint32_t)ent.d_stat.st_size;
			memset(&ent, 0, sizeof(ent));
		}
		sceIoDclose(dfd);
	}
#else
	DIR *d = opendir(dir);
	if (d) {
		struct dirent *ent;
		while (g_app.fp_count < FP_MAX_ENTRIES && (ent = readdir(d)) != NULL) {
			if (ent->d_name[0] == '.')
				continue;
			FileEntry *fe = &g_app.fp_entries[g_app.fp_count++];
			snprintf(fe->name, sizeof(fe->name), "%.127s", ent->d_name);
			fe->is_dir = (ent->d_type == DT_DIR) ? 1 : 0;
			fe->size = 0;
		}
		closedir(d);
	}
#endif
	if (g_app.fp_count > 1)
		qsort(g_app.fp_entries, (size_t)g_app.fp_count, sizeof(FileEntry), cmp_file_entry);
}

static void fp_activate_selected(void)
{
	if (g_app.fp_count == 0 || g_app.fp_sel < 0 || g_app.fp_sel >= g_app.fp_count)
		return;
	FileEntry *fe = &g_app.fp_entries[g_app.fp_sel];
	if (strcmp(fe->name, "..") == 0) {
		char tmp[512];
		snprintf(tmp, sizeof(tmp), "%s", g_app.fp_dir);
		size_t n = strlen(tmp);
		if (n > 0 && tmp[n - 1] == '/')
			tmp[--n] = '\0';
		char *sl = strrchr(tmp, '/');
		if (sl) {
			sl[1] = '\0';
			fp_load_dir(tmp);
		} else {
			fp_load_dir("ux0:/");
		}
		return;
	}
	char full[512];
	size_t dl = strlen(g_app.fp_dir);
	if (dl > 0 && g_app.fp_dir[dl - 1] == '/')
		snprintf(full, sizeof(full), "%.380s%.127s", g_app.fp_dir, fe->name);
	else
		snprintf(full, sizeof(full), "%.380s/%.127s", g_app.fp_dir, fe->name);

	if (fe->is_dir) {
		fp_load_dir(full);
	} else {
		if (fe->size > 8 * 1024 * 1024) {
			set_status("File too large (max 8 MiB).", 1);
			return;
		}
		snprintf(g_app.cmp_att_path, sizeof(g_app.cmp_att_path), "%s", full);
		snprintf(g_app.cmp_att_name, sizeof(g_app.cmp_att_name), "%s", fe->name);
		g_app.cmp_att_size = fe->size;
		g_app.show_file_picker = 0;
	}
}

static void open_compose_new(void)
{
	memset(g_app.cmp_to, 0, sizeof(g_app.cmp_to));
	memset(g_app.cmp_subject, 0, sizeof(g_app.cmp_subject));
	memset(g_app.cmp_body, 0, sizeof(g_app.cmp_body));
	memset(g_app.cmp_reply_id, 0, sizeof(g_app.cmp_reply_id));
	memset(g_app.cmp_att_path, 0, sizeof(g_app.cmp_att_path));
	memset(g_app.cmp_att_name, 0, sizeof(g_app.cmp_att_name));
	g_app.cmp_att_size = 0;
	g_app.cmp_focus = 0;
	g_app.screen = SCREEN_COMPOSE;
}

static void open_compose_reply(void)
{
	memset(g_app.cmp_to, 0, sizeof(g_app.cmp_to));
	memset(g_app.cmp_subject, 0, sizeof(g_app.cmp_subject));
	memset(g_app.cmp_body, 0, sizeof(g_app.cmp_body));
	/* Extract email inside <...> if present */
	const char *lt = strchr(g_app.open_msg.from, '<');
	const char *gt = lt ? strchr(lt + 1, '>') : NULL;
	if (lt && gt && gt > lt + 1) {
		size_t n = (size_t)(gt - (lt + 1));
		if (n >= sizeof(g_app.cmp_to)) n = sizeof(g_app.cmp_to) - 1;
		memcpy(g_app.cmp_to, lt + 1, n);
		g_app.cmp_to[n] = '\0';
	} else {
		snprintf(g_app.cmp_to, sizeof(g_app.cmp_to), "%s", g_app.open_msg.from);
	}

	if (strncmp(g_app.open_msg.subject, "Re:", 3) == 0 ||
	    strncmp(g_app.open_msg.subject, "RE:", 3) == 0) {
		snprintf(g_app.cmp_subject, sizeof(g_app.cmp_subject), "%s", g_app.open_msg.subject);
	} else {
		snprintf(g_app.cmp_subject, sizeof(g_app.cmp_subject), "Re: %s", g_app.open_msg.subject);
	}
	memset(g_app.cmp_att_path, 0, sizeof(g_app.cmp_att_path));
	memset(g_app.cmp_att_name, 0, sizeof(g_app.cmp_att_name));
	g_app.cmp_att_size = 0;
	snprintf(g_app.cmp_reply_id, sizeof(g_app.cmp_reply_id), "%s", g_app.open_msg.message_id);
	g_app.cmp_focus = 2; /* Focus body */
	g_app.screen = SCREEN_COMPOSE;
}

static void activate_settings_field(int idx)
{
	char port_buf[16];
	switch (idx) {
	case 0: {
		MailPreset next = (MailPreset)((g_app.edit_cfg.preset + 1) % PRESET_COUNT);
		config_apply_preset(&g_app.edit_cfg, next);
		break;
	}
	case 1:
		begin_ime(IME_TGT_SET_EMAIL, "Email Address", g_app.edit_cfg.email, IME_KIND_EMAIL, 127);
		break;
	case 2:
		begin_ime(IME_TGT_SET_PASS, "Password / App Password", g_app.edit_cfg.password, IME_KIND_PASSWORD, 127);
		break;
	case 3:
		begin_ime(IME_TGT_SET_IMAP_HOST, "IMAP Server Host", g_app.edit_cfg.imap_host, IME_KIND_TEXT, 127);
		break;
	case 4:
		snprintf(port_buf, sizeof(port_buf), "%u", (unsigned)g_app.edit_cfg.imap_port);
		begin_ime(IME_TGT_SET_IMAP_PORT, "IMAP Port", port_buf, IME_KIND_NUMBER, 5);
		break;
	case 5:
		g_app.edit_cfg.imap_starttls = !g_app.edit_cfg.imap_starttls;
		break;
	case 6:
		begin_ime(IME_TGT_SET_SMTP_HOST, "SMTP Server Host", g_app.edit_cfg.smtp_host, IME_KIND_TEXT, 127);
		break;
	case 7:
		snprintf(port_buf, sizeof(port_buf), "%u", (unsigned)g_app.edit_cfg.smtp_port);
		begin_ime(IME_TGT_SET_SMTP_PORT, "SMTP Port", port_buf, IME_KIND_NUMBER, 5);
		break;
	case 8:
		g_app.edit_cfg.smtp_starttls = !g_app.edit_cfg.smtp_starttls;
		break;
	case 9:
		g_app.cfg = g_app.edit_cfg;
		config_save(&g_app.cfg, VITAMAIL_CONFIG_PATH);
		g_app.folder_count = 0;
		g_app.selected_folder = 0;
		g_app.screen = SCREEN_INBOX;
		start_job(JOB_SYNC_FOLDER, 0, 0);
		break;
	case 10:
		g_app.screen = SCREEN_INBOX;
		break;
	}
}

static void open_compose_attachment_preview(void);

static void activate_compose_field(int idx)
{
	switch (idx) {
	case 0:
		begin_ime(IME_TGT_CMP_TO, "To (Recipient Email)", g_app.cmp_to, IME_KIND_EMAIL, 255);
		break;
	case 1:
		begin_ime(IME_TGT_CMP_SUBJECT, "Subject", g_app.cmp_subject, IME_KIND_TEXT, 255);
		break;
	case 2:
		begin_ime(IME_TGT_CMP_BODY, "Message Body", g_app.cmp_body, IME_KIND_MULTILINE, 2000);
		break;
	case 3:
		if (g_app.cmp_att_path[0] != '\0') {
			open_compose_attachment_preview();
		} else {
#ifdef __vita__
			fp_load_dir("ux0:picture/");
#else
			fp_load_dir(".");
#endif
			g_app.show_file_picker = 1;
		}
		break;
	case 4:
		if (g_app.cmp_to[0] == '\0') {
			set_status("Recipient (To) is required", 1);
		} else {
			start_job(JOB_SEND_EMAIL, 0, 0);
		}
		break;
	case 5:
		g_app.screen = SCREEN_INBOX;
		break;
	}
}

static void close_attachment_preview(void)
{
	if (g_app.att_tex) {
		SDL_DestroyTexture(g_app.att_tex);
		g_app.att_tex = NULL;
	}
	free(g_app.att_disk_data);
	g_app.att_disk_data = NULL;
	g_app.att_disk_size = 0;
	g_app.att_img_w = 0;
	g_app.att_img_h = 0;
	g_app.att_text_scroll = 0;
	g_app.show_att_preview = 0;
}

static void decode_preview_texture(const char *filename, const char *mime_type,
                                   const unsigned char *data, size_t size)
{
	if (data && size > 0 && img_is_supported(filename, mime_type) && g_app.ren) {
		int w = 0, h = 0;
		uint8_t *rgba = img_decode_memory(data, size, &w, &h);
		if (rgba) {
			SDL_Texture *tex = SDL_CreateTexture(g_app.ren, SDL_PIXELFORMAT_RGBA32,
			                                     SDL_TEXTUREACCESS_STATIC, w, h);
			if (tex) {
				SDL_UpdateTexture(tex, NULL, rgba, w * 4);
				SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
				g_app.att_tex = tex;
				g_app.att_img_w = w;
				g_app.att_img_h = h;
			}
			free(rgba);
		}
	}
}

static void open_attachment_preview(int idx)
{
	if (idx < 0 || idx >= g_app.open_msg.attachment_count)
		return;
	close_attachment_preview();

	const MimeAttachment *att = &g_app.open_msg.attachments[idx];
	g_app.att_preview_idx = idx;
	g_app.show_att_preview = 1;
	decode_preview_texture(att->filename, att->mime_type, att->data, att->size);
}

static void open_compose_attachment_preview(void)
{
	if (g_app.cmp_att_path[0] == '\0')
		return;
	close_attachment_preview();

	unsigned char *data = NULL;
	size_t len = 0;
	if (read_attachment_file(g_app.cmp_att_path, &data, &len) != 0) {
		set_status("Failed to read attachment for preview.", 1);
		return;
	}
	g_app.att_preview_idx = -1;
	g_app.att_disk_data = data;
	g_app.att_disk_size = len;
	g_app.show_att_preview = 1;
	decode_preview_texture(g_app.cmp_att_name, NULL, data, len);
}

static void save_attachment_to_disk(int idx)
{
	if (idx < 0 || idx >= g_app.open_msg.attachment_count)
		return;
	const MimeAttachment *att = &g_app.open_msg.attachments[idx];
	if (!att->data || att->size == 0) {
		set_status("Attachment is empty.", 1);
		return;
	}
	char path[256];
#ifdef __vita__
	snprintf(path, sizeof(path), "ux0:data/VitaMail/%.120s", att->filename);
#else
	snprintf(path, sizeof(path), "%.120s", att->filename);
#endif
	FILE *f = fopen(path, "wb");
	if (!f) {
		set_status("Failed to save attachment.", 1);
		return;
	}
	fwrite(att->data, 1, att->size, f);
	fclose(f);
	char msg[300];
	snprintf(msg, sizeof(msg), "Saved to %s", path);
	set_status(msg, 0);
}

int app_init(SDL_Renderer *ren)
{
	memset(&g_app, 0, sizeof(g_app));
	g_app.ren = ren;
	g_app.reader_html_mode = 1;
	g_app.lock = SDL_CreateMutex();
	snprintf(g_app.folders[0].name, sizeof(g_app.folders[0].name), "INBOX");
	g_app.folder_count = 1;

	if (config_load(&g_app.cfg, VITAMAIL_CONFIG_PATH) != 0) {
		g_app.screen = SCREEN_SETTINGS;
		g_app.edit_cfg = g_app.cfg;
		set_status("Welcome! Enter your email & app password to begin.", 0);
	} else {
		g_app.screen = SCREEN_INBOX;
		start_job(JOB_SYNC_FOLDER, 0, 0);
	}

	NetTlsStatus st;
	net_tls_get_status(&st);
	if (!st.ca_found || st.ca_loaded_count == 0) {
		g_app.show_tls_modal = 1;
	}
	return 0;
}

void app_shutdown(void)
{
	close_attachment_preview();
	html_view_clear_cache();
	if (g_app.worker) {
		SDL_WaitThread(g_app.worker, NULL);
		g_app.worker = NULL;
	}
	mime_message_free(&g_app.open_msg);
	if (g_app.lock) {
		SDL_DestroyMutex(g_app.lock);
		g_app.lock = NULL;
	}
}

int app_should_quit(void)
{
	return g_app.quit;
}

static void handle_button_confirm(void)
{
	if (g_app.show_tls_modal) {
		net_tls_reload_ca(NULL);
		return;
	}
	if (g_app.show_att_preview) {
		save_attachment_to_disk(g_app.att_preview_idx);
		return;
	}
	if (g_app.show_file_picker) {
		fp_activate_selected();
		return;
	}
	if (g_app.screen == SCREEN_INBOX) {
		if (!g_app.busy && g_app.header_count > 0) {
			uint32_t uid = g_app.headers[g_app.selected_msg].uid;
			start_job(JOB_FETCH_MESSAGE, uid, 0);
		}
	} else if (g_app.screen == SCREEN_READER) {
		if (!g_app.busy)
			start_job(JOB_DELETE_MESSAGE, g_app.open_uid, 0);
	} else if (g_app.screen == SCREEN_COMPOSE) {
		activate_compose_field(g_app.cmp_focus);
	} else if (g_app.screen == SCREEN_SETTINGS) {
		activate_settings_field(g_app.set_focus);
	}
}

static void handle_button_back(void)
{
	if (g_app.show_tls_modal) {
		g_app.show_tls_modal = 0;
		return;
	}
	if (g_app.show_att_preview) {
		close_attachment_preview();
		return;
	}
	if (g_app.show_file_picker) {
		g_app.show_file_picker = 0;
		return;
	}
	if (g_app.screen == SCREEN_READER || g_app.screen == SCREEN_COMPOSE || g_app.screen == SCREEN_SETTINGS) {
		if (g_app.screen == SCREEN_READER)
			html_view_clear_cache();
		g_app.screen = SCREEN_INBOX;
	}
}

static void handle_nav(int dy, int dx)
{
	if (g_app.show_tls_modal)
		return;

	if (g_app.show_att_preview) {
		if (dy != 0) {
			g_app.att_text_scroll += dy * 30;
			if (g_app.att_text_scroll < 0)
				g_app.att_text_scroll = 0;
		}
		return;
	}
	if (g_app.show_file_picker) {
		if (dy != 0 && g_app.fp_count > 0) {
			g_app.fp_sel += dy;
			if (g_app.fp_sel < 0) g_app.fp_sel = 0;
			if (g_app.fp_sel >= g_app.fp_count) g_app.fp_sel = g_app.fp_count - 1;
			if (g_app.fp_sel < g_app.fp_scroll) g_app.fp_scroll = g_app.fp_sel;
			if (g_app.fp_sel >= g_app.fp_scroll + 8) g_app.fp_scroll = g_app.fp_sel - 7;
		}
		return;
	}

	if (g_app.screen == SCREEN_INBOX) {
		if (dy != 0 && g_app.header_count > 0) {
			g_app.selected_msg += dy;
			if (g_app.selected_msg < 0) g_app.selected_msg = 0;
			if (g_app.selected_msg >= g_app.header_count)
				g_app.selected_msg = g_app.header_count - 1;
			if (g_app.selected_msg < g_app.list_scroll)
				g_app.list_scroll = g_app.selected_msg;
			if (g_app.selected_msg >= g_app.list_scroll + 8)
				g_app.list_scroll = g_app.selected_msg - 7;
		}
		if (dx != 0 && g_app.folder_count > 1 && !g_app.busy) {
			g_app.selected_folder = (g_app.selected_folder + dx + g_app.folder_count) % g_app.folder_count;
			start_job(JOB_SYNC_FOLDER, 0, 0);
		}
	} else if (g_app.screen == SCREEN_READER) {
		g_app.reader_scroll += dy * 36;
		if (g_app.reader_scroll < 0)
			g_app.reader_scroll = 0;
	} else if (g_app.screen == SCREEN_COMPOSE) {
		g_app.cmp_focus = (g_app.cmp_focus + dy + 6) % 6;
	} else if (g_app.screen == SCREEN_SETTINGS) {
		g_app.set_focus = (g_app.set_focus + dy + 11) % 11;
	}
}

static void handle_touch_tap(int x, int y)
{
	/* Top-bar iTLS pill */
	UiRect tls_pill = { 690, 8, 256, 28 };
	if (ui_point_in_rect(x, y, tls_pill)) {
		g_app.show_tls_modal = !g_app.show_tls_modal;
		return;
	}

	if (g_app.show_tls_modal) {
		UiRect btn_recheck = { 290, 360, 180, 38 };
		UiRect btn_close   = { 490, 360, 180, 38 };
		if (ui_point_in_rect(x, y, btn_recheck)) {
			net_tls_reload_ca(NULL);
		} else if (ui_point_in_rect(x, y, btn_close)) {
			g_app.show_tls_modal = 0;
		}
		return;
	}

	if (g_app.show_att_preview) {
		if (g_app.att_preview_idx >= 0) {
			if (ui_point_in_rect(x, y, (UiRect){ 260, 444, 220, 36 })) {
				save_attachment_to_disk(g_app.att_preview_idx);
			} else if (ui_point_in_rect(x, y, (UiRect){ 500, 444, 160, 36 })) {
				close_attachment_preview();
			}
		} else {
			if (ui_point_in_rect(x, y, (UiRect){ 400, 444, 160, 36 })) {
				close_attachment_preview();
			}
		}
		return;
	}
	if (g_app.show_file_picker) {
		/* Quick-jump tabs */
		if (ui_point_in_rect(x, y, (UiRect){ 120, 102, 130, 30 })) { fp_load_dir("ux0:picture/"); return; }
		if (ui_point_in_rect(x, y, (UiRect){ 260, 102, 160, 30 })) { fp_load_dir("ux0:data/VitaMail/"); return; }
		if (ui_point_in_rect(x, y, (UiRect){ 430, 102, 110, 30 })) { fp_load_dir("ux0:data/"); return; }
		if (ui_point_in_rect(x, y, (UiRect){ 550, 102, 90, 30 }))  { fp_load_dir("ux0:/"); return; }
		if (ui_point_in_rect(x, y, (UiRect){ 720, 444, 120, 34 })) { g_app.show_file_picker = 0; return; }
		for (int row = 0; row < 8; row++) {
			int idx = g_app.fp_scroll + row;
			if (idx >= g_app.fp_count) break;
			if (ui_point_in_rect(x, y, (UiRect){ 120, 144 + row * 36, 720, 32 })) {
				g_app.fp_sel = idx;
				fp_activate_selected();
				return;
			}
		}
		return;
	}

	if (g_app.screen == SCREEN_INBOX) {
		/* Sidebar Compose, Settings, and Sort buttons */
		if (ui_point_in_rect(x, y, (UiRect){ 14, 56, 182, 34 })) {
			open_compose_new();
			return;
		}
		if (ui_point_in_rect(x, y, (UiRect){ 14, 96, 182, 34 })) {
			g_app.edit_cfg = g_app.cfg;
			g_app.set_focus = 0;
			g_app.screen = SCREEN_SETTINGS;
			return;
		}
		if (ui_point_in_rect(x, y, (UiRect){ 14, 136, 182, 34 })) {
			SDL_LockMutex(g_app.lock);
			g_app.sort_mode = (SortMode)((g_app.sort_mode + 1) % SORT_COUNT);
			sort_headers_locked();
			SDL_UnlockMutex(g_app.lock);
			return;
		}
		/* Folder items */
		for (int i = 0; i < g_app.folder_count && i < 8; i++) {
			UiRect fr = { 14, 196 + i * 36, 182, 32 };
			if (ui_point_in_rect(x, y, fr) && !g_app.busy) {
				g_app.selected_folder = i;
				start_job(JOB_SYNC_FOLDER, 0, 0);
				return;
			}
		}
		/* Email rows */
		for (int row = 0; row < 8; row++) {
			int idx = g_app.list_scroll + row;
			if (idx >= g_app.header_count)
				break;
			UiRect mr = { 216, 54 + row * 56, 730, 52 };
			if (ui_point_in_rect(x, y, mr) && !g_app.busy) {
				g_app.selected_msg = idx;
				start_job(JOB_FETCH_MESSAGE, g_app.headers[idx].uid, 0);
				return;
			}
		}
	} else if (g_app.screen == SCREEN_READER) {
		for (int i = 0; i < g_app.open_msg.attachment_count && i < MIME_MAX_ATTACHMENTS; i++) {
			if (ui_point_in_rect(x, y, g_app.att_chip_rects[i])) {
				open_attachment_preview(i);
				return;
			}
		}
		if (ui_point_in_rect(x, y, (UiRect){ 20, 488, 130, 38 })) {
			open_compose_reply();
		} else if (ui_point_in_rect(x, y, (UiRect){ 165, 488, 170, 38 }) && !g_app.busy) {
			int cur_seen = 1;
			for (int i = 0; i < g_app.header_count; i++) {
				if (g_app.headers[i].uid == g_app.open_uid) {
					cur_seen = g_app.headers[i].seen;
					break;
				}
			}
			start_job(JOB_TOGGLE_SEEN, g_app.open_uid, !cur_seen);
		} else if (ui_point_in_rect(x, y, (UiRect){ 350, 488, 130, 38 }) && !g_app.busy) {
			start_job(JOB_DELETE_MESSAGE, g_app.open_uid, 0);
		} else if (g_app.open_msg.html_body && ui_point_in_rect(x, y, (UiRect){ 495, 488, 150, 38 })) {
			g_app.reader_html_mode = !g_app.reader_html_mode;
			g_app.reader_scroll = 0;
		} else if (ui_point_in_rect(x, y, (UiRect){ 810, 488, 130, 38 })) {
			g_app.screen = SCREEN_INBOX;
		}
	} else if (g_app.screen == SCREEN_COMPOSE) {
		if (ui_point_in_rect(x, y, (UiRect){ 40, 74, 880, 40 })) { g_app.cmp_focus = 0; activate_compose_field(0); }
		else if (ui_point_in_rect(x, y, (UiRect){ 40, 126, 880, 40 })) { g_app.cmp_focus = 1; activate_compose_field(1); }
		else if (ui_point_in_rect(x, y, (UiRect){ 40, 178, 880, 270 })) { g_app.cmp_focus = 2; activate_compose_field(2); }
		else if (g_app.cmp_att_name[0] != '\0' && ui_point_in_rect(x, y, (UiRect){ 340, 468, 42, 42 })) {
			g_app.cmp_att_path[0] = '\0';
			g_app.cmp_att_name[0] = '\0';
			g_app.cmp_att_size = 0;
		}
		else if (ui_point_in_rect(x, y, (UiRect){ 40, 468, g_app.cmp_att_name[0] ? 294 : 340, 42 })) { g_app.cmp_focus = 3; activate_compose_field(3); }
		else if (ui_point_in_rect(x, y, (UiRect){ 400, 468, 180, 42 })) { g_app.cmp_focus = 4; activate_compose_field(4); }
		else if (ui_point_in_rect(x, y, (UiRect){ 600, 468, 160, 42 })) { g_app.cmp_focus = 5; activate_compose_field(5); }
	} else if (g_app.screen == SCREEN_SETTINGS) {
		for (int i = 0; i < 9; i++) {
			UiRect r = { 40, 62 + i * 42, 880, 36 };
			if (ui_point_in_rect(x, y, r)) {
				g_app.set_focus = i;
				activate_settings_field(i);
				return;
			}
		}
		if (ui_point_in_rect(x, y, (UiRect){ 40, 456, 220, 42 })) { g_app.set_focus = 9; activate_settings_field(9); }
		else if (ui_point_in_rect(x, y, (UiRect){ 280, 456, 160, 42 })) { g_app.set_focus = 10; activate_settings_field(10); }
	}
}

void app_handle_event(const SDL_Event *ev)
{
	if (ev->type == SDL_QUIT) {
		g_app.quit = 1;
		return;
	}

	if (ime_is_active())
		return;

	if (ev->type == SDL_FINGERDOWN) {
		/* Only respond to direct front touchscreen; ignore indirect rear touchpad */
		if (SDL_GetTouchDeviceType(ev->tfinger.touchId) != SDL_TOUCH_DEVICE_DIRECT)
			return;
		int tx = (int)(ev->tfinger.x * VITA_SCREEN_W);
		int ty = (int)(ev->tfinger.y * VITA_SCREEN_H);
		handle_touch_tap(tx, ty);
		return;
	}

	if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT &&
	    ev->button.which != SDL_TOUCH_MOUSEID) {
		handle_touch_tap(ev->button.x, ev->button.y);
		return;
	}

	/* SDL2 Vita controller mapping:
	 * 0=Triangle, 1=Circle, 2=Cross, 3=Square, 4=L, 5=R,
	 * 6=Down, 7=Left, 8=Up, 9=Right, 10=Select, 11=Start */
	if (ev->type == SDL_JOYBUTTONDOWN) {
		switch (ev->jbutton.button) {
		case 2: /* Cross */
			handle_button_confirm();
			break;
		case 1: /* Circle */
			handle_button_back();
			break;
		case 3: /* Square */
			if (g_app.screen == SCREEN_INBOX)
				open_compose_new();
			else if (g_app.screen == SCREEN_READER)
				open_compose_reply();
			break;
		case 0: /* Triangle */
			if (g_app.screen == SCREEN_INBOX && !g_app.busy)
				start_job(JOB_SYNC_FOLDER, 0, 0);
			else if (g_app.screen == SCREEN_READER && !g_app.busy)
				start_job(JOB_TOGGLE_SEEN, g_app.open_uid, 0);
			break;
		case 4: /* L */
			if (g_app.screen == SCREEN_READER && g_app.open_msg.attachment_count > 0)
				open_attachment_preview(0);
			else
				handle_nav(0, -1);
			break;
		case 5: /* R */
			if (g_app.screen == SCREEN_READER && g_app.open_msg.html_body) {
				g_app.reader_html_mode = !g_app.reader_html_mode;
				g_app.reader_scroll = 0;
			} else {
				handle_nav(0, 1);
			}
			break;
		case 6: /* Down */
			handle_nav(1, 0);
			break;
		case 8: /* Up */
			handle_nav(-1, 0);
			break;
		case 10: /* Select */
			g_app.show_tls_modal = !g_app.show_tls_modal;
			break;
		case 11: /* Start */
			if (g_app.screen != SCREEN_SETTINGS) {
				g_app.edit_cfg = g_app.cfg;
				g_app.set_focus = 0;
				g_app.screen = SCREEN_SETTINGS;
			} else {
				g_app.screen = SCREEN_INBOX;
			}
			break;
		}
	}
}

void app_update(void)
{
	if (ime_is_active()) {
		ImePollResult pr = ime_poll(g_app.ime_buf, sizeof(g_app.ime_buf));
		if (pr == IME_POLL_CONFIRMED) {
			switch (g_app.ime_target) {
			case IME_TGT_SET_EMAIL:
				snprintf(g_app.edit_cfg.email, sizeof(g_app.edit_cfg.email), "%.127s", g_app.ime_buf);
				break;
			case IME_TGT_SET_PASS:
				snprintf(g_app.edit_cfg.password, sizeof(g_app.edit_cfg.password), "%.127s", g_app.ime_buf);
				break;
			case IME_TGT_SET_IMAP_HOST:
				snprintf(g_app.edit_cfg.imap_host, sizeof(g_app.edit_cfg.imap_host), "%.127s", g_app.ime_buf);
				break;
			case IME_TGT_SET_IMAP_PORT:
				g_app.edit_cfg.imap_port = (uint16_t)atoi(g_app.ime_buf);
				break;
			case IME_TGT_SET_SMTP_HOST:
				snprintf(g_app.edit_cfg.smtp_host, sizeof(g_app.edit_cfg.smtp_host), "%.127s", g_app.ime_buf);
				break;
			case IME_TGT_SET_SMTP_PORT:
				g_app.edit_cfg.smtp_port = (uint16_t)atoi(g_app.ime_buf);
				break;
			case IME_TGT_CMP_TO:
				snprintf(g_app.cmp_to, sizeof(g_app.cmp_to), "%.255s", g_app.ime_buf);
				break;
			case IME_TGT_CMP_SUBJECT:
				snprintf(g_app.cmp_subject, sizeof(g_app.cmp_subject), "%.255s", g_app.ime_buf);
				break;
			case IME_TGT_CMP_BODY:
				snprintf(g_app.cmp_body, sizeof(g_app.cmp_body), "%s", g_app.ime_buf);
				break;
			default:
				break;
			}
			g_app.ime_target = IME_TGT_NONE;
		} else if (pr == IME_POLL_CANCELLED) {
			g_app.ime_target = IME_TGT_NONE;
		}
		return;
	}

#ifdef __vita__
	/* Smooth stick scrolling in Reader view */
	if (g_app.screen == SCREEN_READER && !g_app.show_tls_modal) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		int ry = (int)pad.ry - 128;
		if (ry > 24 || ry < -24) {
			g_app.reader_scroll += ry / 16;
			if (g_app.reader_scroll < 0)
				g_app.reader_scroll = 0;
		}
	}
#endif
}

static void draw_button(int x, int y, int w, int h, const char *label, int focused, UiColor bg)
{
	ui_fill_rect(x, y, w, h, bg);
	ui_draw_rect(x, y, w, h, focused ? COL_ACCENT : COL_BORDER);
	ui_draw_text(x + 12, y + (h - 16) / 2, 16, COL_TEXT, label, w - 24);
}

static void render_top_bar(void)
{
	ui_fill_rect(0, 0, VITA_SCREEN_W, 44, COL_PANEL);
	ui_fill_rect(0, 43, VITA_SCREEN_W, 1, COL_BORDER);

	ui_draw_text(14, 11, 20, COL_ACCENT, "VitaMail", 0);

	const char *sub = "Inbox";
	if (g_app.screen == SCREEN_READER) sub = "Message";
	else if (g_app.screen == SCREEN_COMPOSE) sub = "Compose";
	else if (g_app.screen == SCREEN_SETTINGS) sub = "Account Settings";
	else if (g_app.folder_count > 0) sub = g_app.folders[g_app.selected_folder].name;
	ui_draw_text(118, 13, 16, COL_MUTED, sub, 240);

	NetTlsStatus st;
	net_tls_get_status(&st);
	int tls_ok = (st.ca_found && st.ca_loaded_count > 0);
	UiColor pill_col = tls_ok ? (UiColor){ 20, 75, 52, 255 } : (UiColor){ 105, 28, 38, 255 };
	UiColor pill_bdr = tls_ok ? COL_GREEN : COL_RED;

	ui_fill_rect(690, 8, 256, 28, pill_col);
	ui_draw_rect(690, 8, 256, 28, pill_bdr);

	char pill_txt[64];
	if (tls_ok)
		snprintf(pill_txt, sizeof(pill_txt), "iTLS OK (%d CAs) [Sel]", st.ca_loaded_count);
	else
		snprintf(pill_txt, sizeof(pill_txt), "NO iTLS CA STORE [Sel]");
	ui_draw_text(700, 14, 14, COL_TEXT, pill_txt, 236);
}

static void render_inbox(void)
{
	/* Left sidebar */
	ui_fill_rect(0, 44, 210, VITA_SCREEN_H - 44, COL_PANEL_ALT);
	ui_fill_rect(209, 44, 1, VITA_SCREEN_H - 44, COL_BORDER);

	draw_button(14, 56, 182, 34, "[Square] Compose", 0, (UiColor){ 36, 98, 176, 255 });
	draw_button(14, 96, 182, 34, "[Start] Settings", 0, COL_PANEL);
	draw_button(14, 136, 182, 34, sort_mode_label(g_app.sort_mode), 0, COL_PANEL);

	ui_draw_text(14, 178, 13, COL_MUTED, "FOLDERS (L/R)", 182);
	for (int i = 0; i < g_app.folder_count && i < 8; i++) {
		int y = 196 + i * 36;
		int sel = (i == g_app.selected_folder);
		ui_fill_rect(14, y, 182, 32, sel ? COL_SEL : COL_PANEL);
		ui_draw_rect(14, y, 182, 32, sel ? COL_ACCENT : COL_BORDER);
		ui_draw_text(24, y + 8, 15, COL_TEXT, g_app.folders[i].name, 162);
	}

	/* Email list */
	if (g_app.header_count == 0) {
		if (g_app.busy) {
			ui_fill_rect(240, 170, 680, 120, COL_PANEL);
			ui_draw_rect(240, 170, 680, 120, COL_ACCENT);
			ui_draw_text(260, 195, 18, COL_TEXT, "Connecting & Syncing...", 640);
			ui_draw_wrapped_text(260, 230, 640, 50, 15, COL_MUTED,
			                     g_app.status_msg[0] ? g_app.status_msg : "Please wait...", 0);
		} else if (g_app.status_is_err && g_app.status_msg[0] != '\0') {
			ui_fill_rect(240, 150, 680, 180, (UiColor){ 52, 20, 28, 255 });
			ui_draw_rect(240, 150, 680, 180, COL_RED);
			ui_draw_text(260, 168, 18, COL_RED, "Connection Failed", 640);
			ui_draw_wrapped_text(260, 202, 640, 84, 15, COL_TEXT, g_app.status_msg, 0);
			ui_draw_text(260, 296, 14, COL_MUTED,
			             "Press [Start] for Account Settings or [Triangle] to retry.", 640);
		} else {
			ui_draw_text(250, 220, 18, COL_MUTED,
			             "No messages. Press [Triangle] to refresh or [Start] for Settings.",
			             650);
		}
	} else {
		for (int row = 0; row < 8; row++) {
			int idx = g_app.list_scroll + row;
			if (idx >= g_app.header_count)
				break;
			const ImapHeader *h = &g_app.headers[idx];
			int y = 54 + row * 56;
			int sel = (idx == g_app.selected_msg);

			ui_fill_rect(218, y, 728, 52, sel ? COL_SEL : COL_PANEL);
			ui_draw_rect(218, y, 728, 52, sel ? COL_ACCENT : COL_BORDER);

			if (!h->seen)
				ui_fill_circle_dot(232, y + 26, 5, COL_ACCENT);

			ui_draw_text(246, y + 7, 15, h->seen ? COL_MUTED : COL_TEXT, h->from, 430);
			ui_draw_text(690, y + 7, 13, COL_MUTED, h->date, 244);
			ui_draw_text(246, y + 28, 16, COL_TEXT, h->subject, 688);
		}
	}
}

static void render_reader(void)
{
	ui_fill_rect(16, 54, 928, 86, COL_PANEL);
	ui_draw_rect(16, 54, 928, 86, COL_BORDER);

	ui_draw_text(28, 62, 18, COL_TEXT, g_app.open_msg.subject, 900);
	char from_line[300];
	snprintf(from_line, sizeof(from_line), "From: %s", g_app.open_msg.from);
	ui_draw_text(28, 88, 14, COL_ACCENT, from_line, 620);
	ui_draw_text(660, 88, 13, COL_MUTED, g_app.open_msg.date, 270);

	memset(g_app.att_chip_rects, 0, sizeof(g_app.att_chip_rects));
	if (g_app.open_msg.attachment_count > 0) {
		int cx = 28;
		ui_draw_text(cx, 113, 13, COL_MUTED, "Attachments:", 90);
		cx += 92;
		for (int i = 0; i < g_app.open_msg.attachment_count && i < MIME_MAX_ATTACHMENTS; i++) {
			const MimeAttachment *att = &g_app.open_msg.attachments[i];
			char chip[160];
			snprintf(chip, sizeof(chip), "%.28s (%u KB)",
			         att->filename, (unsigned)((att->size + 1023) / 1024));
			int cw = ui_measure_text(13, chip) + 18;
			if (cx + cw > 932)
				break;
			UiRect r = { cx, 108, cw, 24 };
			g_app.att_chip_rects[i] = r;
			ui_fill_rect(r.x, r.y, r.w, r.h, (UiColor){ 32, 62, 104, 255 });
			ui_draw_rect(r.x, r.y, r.w, r.h, COL_ACCENT);
			ui_draw_text(r.x + 8, r.y + 5, 13, COL_TEXT, chip, cw - 12);
			cx += cw + 8;
		}
	}

	int use_html = (g_app.reader_html_mode && g_app.open_msg.html_body && g_app.ren);
	ui_fill_rect(16, 148, 928, 330, use_html ? (UiColor){ 255, 255, 255, 255 } : COL_PANEL_ALT);
	ui_draw_rect(16, 148, 928, 330, COL_BORDER);

	if (use_html) {
		g_app.reader_total_h = html_view_render(g_app.ren, 28, 158, 904, 310,
		                                        g_app.open_msg.html_body,
		                                        &g_app.open_msg,
		                                        g_app.reader_scroll);
	} else {
		g_app.reader_total_h = ui_draw_wrapped_text(28, 158, 904, 310, 16, COL_TEXT,
		                                            g_app.open_msg.body ? g_app.open_msg.body : "",
		                                            g_app.reader_scroll);
	}

	draw_button(20, 488, 130, 38, "[Sq] Reply", 0, (UiColor){ 36, 98, 176, 255 });
	draw_button(165, 488, 170, 38, "[Tri] Mark Unread", 0, COL_PANEL);
	draw_button(350, 488, 130, 38, "[X] Delete", 0, (UiColor){ 120, 34, 46, 255 });
	if (g_app.open_msg.html_body) {
		draw_button(495, 488, 150, 38,
		            g_app.reader_html_mode ? "[R] View Plain" : "[R] View HTML",
		            0, COL_PANEL);
	}
	draw_button(810, 488, 130, 38, "[O] Back", 0, COL_PANEL);
}

static void render_compose(void)
{
	ui_draw_text(40, 54, 14, COL_MUTED, "Tap any field or press [Cross] to type with Vita Keyboard", 880);

	char buf[300];
	snprintf(buf, sizeof(buf), "To: %s", g_app.cmp_to[0] ? g_app.cmp_to : "(tap to enter)");
	draw_button(40, 74, 880, 40, buf, g_app.cmp_focus == 0, COL_PANEL);

	snprintf(buf, sizeof(buf), "Subject: %s", g_app.cmp_subject[0] ? g_app.cmp_subject : "(tap to enter)");
	draw_button(40, 126, 880, 40, buf, g_app.cmp_focus == 1, COL_PANEL);

	ui_fill_rect(40, 178, 880, 270, COL_PANEL);
	ui_draw_rect(40, 178, 880, 270, g_app.cmp_focus == 2 ? COL_ACCENT : COL_BORDER);
	ui_draw_wrapped_text(52, 188, 856, 250, 16,
	                     g_app.cmp_body[0] ? COL_TEXT : COL_MUTED,
	                     g_app.cmp_body[0] ? g_app.cmp_body : "(Tap or press [Cross] to write message body...)",
	                     0);

	char att_label[192];
	if (g_app.cmp_att_name[0] != '\0') {
		snprintf(att_label, sizeof(att_label), "%.80s (%u KB)",
		         g_app.cmp_att_name, (unsigned)((g_app.cmp_att_size + 1023) / 1024));
		draw_button(40, 468, 294, 42, att_label, g_app.cmp_focus == 3, (UiColor){ 32, 62, 104, 255 });
		draw_button(340, 468, 42, 42, "X", 0, (UiColor){ 120, 34, 46, 255 });
	} else {
		snprintf(att_label, sizeof(att_label), "Attach Picture / File...");
		draw_button(40, 468, 340, 42, att_label, g_app.cmp_focus == 3, COL_PANEL);
	}
	draw_button(400, 468, 180, 42, "Send Email", g_app.cmp_focus == 4, (UiColor){ 36, 120, 82, 255 });
	draw_button(600, 468, 160, 42, "Cancel [O]", g_app.cmp_focus == 5, COL_PANEL);
}

static void render_file_picker(void)
{
	ui_fill_rect(0, 0, VITA_SCREEN_W, VITA_SCREEN_H, (UiColor){ 0, 0, 0, 180 });
	ui_fill_rect(100, 56, 760, 434, COL_PANEL);
	ui_draw_rect(100, 56, 760, 434, COL_ACCENT);

	char title[600];
	snprintf(title, sizeof(title), "Attach File: %.480s", g_app.fp_dir);
	ui_draw_text(120, 68, 17, COL_TEXT, title, 720);

	draw_button(120, 102, 130, 30, "ux0:picture", 0, COL_PANEL_ALT);
	draw_button(260, 102, 160, 30, "ux0:data/VitaMail", 0, COL_PANEL_ALT);
	draw_button(430, 102, 110, 30, "ux0:data", 0, COL_PANEL_ALT);
	draw_button(550, 102, 90, 30, "ux0:/", 0, COL_PANEL_ALT);

	if (g_app.fp_count == 0) {
		ui_draw_text(140, 260, 16, COL_MUTED, "(Empty directory)", 680);
	} else {
		for (int row = 0; row < 8; row++) {
			int idx = g_app.fp_scroll + row;
			if (idx >= g_app.fp_count) break;
			const FileEntry *fe = &g_app.fp_entries[idx];
			int y = 144 + row * 36;
			int sel = (idx == g_app.fp_sel);
			ui_fill_rect(120, y, 720, 32, sel ? COL_SEL : COL_PANEL_ALT);
			ui_draw_rect(120, y, 720, 32, sel ? COL_ACCENT : COL_BORDER);
			char row_txt[180];
			if (fe->is_dir)
				snprintf(row_txt, sizeof(row_txt), "[DIR] %s", fe->name);
			else
				snprintf(row_txt, sizeof(row_txt), "%s (%u KB)", fe->name, (unsigned)((fe->size + 1023) / 1024));
			ui_draw_text(132, y + 7, 15, fe->is_dir ? COL_ACCENT : COL_TEXT, row_txt, 696);
		}
	}

	ui_draw_text(120, 450, 14, COL_MUTED, "Tap or [Cross] to open/attach (max 8 MiB)", 580);
	draw_button(720, 444, 120, 34, "[O] Close", 0, COL_PANEL_ALT);
}

static void render_settings(void)
{
	char rows[9][256];
	snprintf(rows[0], sizeof(rows[0]), "Preset: %s  (tap / [Cross] to cycle)", config_preset_name(g_app.edit_cfg.preset));
	snprintf(rows[1], sizeof(rows[1]), "Email: %s", g_app.edit_cfg.email[0] ? g_app.edit_cfg.email : "(not set)");
	snprintf(rows[2], sizeof(rows[2]), "Password: %s", g_app.edit_cfg.password[0] ? "************" : "(not set)");
	snprintf(rows[3], sizeof(rows[3]), "IMAP Host: %s", g_app.edit_cfg.imap_host);
	snprintf(rows[4], sizeof(rows[4]), "IMAP Port: %u", (unsigned)g_app.edit_cfg.imap_port);
	snprintf(rows[5], sizeof(rows[5]), "IMAP Security: %s", g_app.edit_cfg.imap_starttls ? "STARTTLS" : "Implicit TLS (SSL)");
	snprintf(rows[6], sizeof(rows[6]), "SMTP Host: %s", g_app.edit_cfg.smtp_host);
	snprintf(rows[7], sizeof(rows[7]), "SMTP Port: %u", (unsigned)g_app.edit_cfg.smtp_port);
	snprintf(rows[8], sizeof(rows[8]), "SMTP Security: %s", g_app.edit_cfg.smtp_starttls ? "STARTTLS" : "Implicit TLS (SSL)");

	for (int i = 0; i < 9; i++) {
		draw_button(40, 62 + i * 42, 880, 36, rows[i], g_app.set_focus == i, COL_PANEL);
	}

	draw_button(40, 456, 220, 42, "Save & Connect", g_app.set_focus == 9, (UiColor){ 36, 120, 82, 255 });
	draw_button(280, 456, 160, 42, "Back [O]", g_app.set_focus == 10, COL_PANEL);
}

static void render_tls_modal(void)
{
	ui_fill_rect(0, 0, VITA_SCREEN_W, VITA_SCREEN_H, (UiColor){ 0, 0, 0, 175 });
	ui_fill_rect(140, 100, 680, 330, COL_PANEL);
	ui_draw_rect(140, 100, 680, 330, COL_ACCENT);

	NetTlsStatus st;
	net_tls_get_status(&st);

	ui_draw_text(165, 118, 20, COL_TEXT, "TLS & iTLS-Enso Certificate Store", 630);

	char line[320];
	snprintf(line, sizeof(line), "CA Store Path: %s", st.ca_path);
	ui_draw_text(165, 158, 15, COL_MUTED, line, 630);

	snprintf(line, sizeof(line), "Status: %s",
	         (st.ca_found && st.ca_loaded_count > 0)
	             ? "VALID (Non-expired CA bundle loaded)"
	             : "MISSING OR EMPTY - iTLS-Enso REQUIRED!");
	ui_draw_text(165, 188, 16, (st.ca_found && st.ca_loaded_count > 0) ? COL_GREEN : COL_RED, line, 630);

	snprintf(line, sizeof(line), "Certificates Loaded: %d   |   Skipped: %d   |   Network: %s",
	         st.ca_loaded_count, st.ca_skipped_count,
	         net_tls_is_online() ? "Connected" : "Offline");
	ui_draw_text(165, 220, 15, COL_TEXT, line, 630);

	ui_draw_wrapped_text(165, 255, 630, 90, 14, COL_MUTED,
	                     st.status_msg[0] ? st.status_msg
	                                      : "VitaMail verifies TLS certificates against vs0:/data/external/cert/CA_LIST.cer (iTLS-Enso).",
	                     0);

	draw_button(290, 360, 180, 38, "[X] Re-check CA", 1, (UiColor){ 36, 98, 176, 255 });
	draw_button(490, 360, 180, 38, "[O] Close", 0, COL_PANEL_ALT);
}

static void render_att_preview(void)
{
	const char *fname = NULL;
	const char *mtype = "";
	const unsigned char *data = NULL;
	size_t size = 0;

	if (g_app.att_preview_idx == -1) {
		fname = g_app.cmp_att_name;
		data = g_app.att_disk_data;
		size = g_app.att_disk_size;
	} else if (g_app.att_preview_idx >= 0 && g_app.att_preview_idx < g_app.open_msg.attachment_count) {
		const MimeAttachment *att = &g_app.open_msg.attachments[g_app.att_preview_idx];
		fname = att->filename;
		mtype = att->mime_type;
		data = att->data;
		size = att->size;
	} else {
		return;
	}

	ui_fill_rect(0, 0, VITA_SCREEN_W, VITA_SCREEN_H, (UiColor){ 0, 0, 0, 190 });
	ui_fill_rect(90, 50, 780, 444, COL_PANEL);
	ui_draw_rect(90, 50, 780, 444, COL_ACCENT);

	char hdr[256];
	snprintf(hdr, sizeof(hdr), "%.120s  (%u KB%s%s)",
	         fname, (unsigned)((size + 1023) / 1024),
	         mtype[0] ? "  |  " : "", mtype[0] ? mtype : "");
	ui_draw_text(110, 64, 16, COL_TEXT, hdr, 740);

	int box_x = 110, box_y = 96, box_w = 740, box_h = 334;
	ui_fill_rect(box_x, box_y, box_w, box_h, COL_PANEL_ALT);
	ui_draw_rect(box_x, box_y, box_w, box_h, COL_BORDER);

	if (g_app.att_tex && g_app.att_img_w > 0 && g_app.att_img_h > 0 && g_app.ren) {
		float sx = (float)box_w / (float)g_app.att_img_w;
		float sy = (float)box_h / (float)g_app.att_img_h;
		float scale = (sx < sy) ? sx : sy;
		if (scale > 1.0f) scale = 1.0f;
		int dw = (int)((float)g_app.att_img_w * scale);
		int dh = (int)((float)g_app.att_img_h * scale);
		SDL_Rect dst = { box_x + (box_w - dw) / 2, box_y + (box_h - dh) / 2, dw, dh };
		SDL_RenderCopy(g_app.ren, g_app.att_tex, NULL, &dst);
	} else if ((strncmp(mtype, "text/", 5) == 0 || strstr(fname, ".txt") || strstr(fname, ".log")) &&
	           data && size > 0) {
		size_t cap = (size < 32768) ? size : 32768;
		char *txt = (char *)malloc(cap + 1);
		if (txt) {
			memcpy(txt, data, cap);
			txt[cap] = '\0';
			ui_draw_wrapped_text(box_x + 12, box_y + 10, box_w - 24, box_h - 20, 15,
			                     COL_TEXT, txt, g_app.att_text_scroll);
			free(txt);
		}
	} else {
		ui_draw_text(box_x + 24, box_y + 130, 16, COL_MUTED,
		             "Preview not available for this file type.", box_w - 48);
		if (g_app.att_preview_idx >= 0) {
			ui_draw_text(box_x + 24, box_y + 162, 14, COL_TEXT,
			             "Press [Cross] or tap Save to export it to ux0:data/VitaMail/.", box_w - 48);
		}
	}

	if (g_app.att_preview_idx >= 0) {
		draw_button(260, 444, 220, 36, "[X] Save to VitaMail/", 1, (UiColor){ 36, 120, 82, 255 });
		draw_button(500, 444, 160, 36, "[O] Close", 0, COL_PANEL_ALT);
	} else {
		draw_button(400, 444, 160, 36, "[O] Close", 1, COL_PANEL_ALT);
	}
}

void app_render(void)
{
	ui_clear(COL_BG);
	render_top_bar();

	SDL_LockMutex(g_app.lock);
	if (g_app.screen == SCREEN_INBOX) render_inbox();
	else if (g_app.screen == SCREEN_READER) render_reader();
	else if (g_app.screen == SCREEN_COMPOSE) render_compose();
	else if (g_app.screen == SCREEN_SETTINGS) render_settings();

	/* Bottom status bar */
	if (g_app.status_msg[0] != '\0') {
		ui_fill_rect(0, VITA_SCREEN_H - 28, VITA_SCREEN_W, 28,
		             g_app.status_is_err ? (UiColor){ 105, 28, 38, 240 } : (UiColor){ 24, 34, 52, 240 });
		ui_draw_text(14, VITA_SCREEN_H - 22, 14, COL_TEXT, g_app.status_msg, VITA_SCREEN_W - 28);
	}
	SDL_UnlockMutex(g_app.lock);

	if (g_app.show_tls_modal)
		render_tls_modal();
	if (g_app.show_file_picker)
		render_file_picker();
	if (g_app.show_att_preview)
		render_att_preview();
}
