#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <objbase.h>
#include <psapi.h>

#include "audio.h"
#include "gfx.h"
#include "main.h"
#include "notif.h"
#include "winnotif.h"

#ifdef GL_TESTS
#include "samples.h"
#endif

#define DEBOUNCE_S 0.010
#define SETTLE_S 0.100
#define STALE_S 120.0
#define AVATAR_RETRY_MS 150
#define TEXTS 4
#define PAYLOAD 16384

enum { SQ_BUSY = 5, SQ_LOCKED = 6, SQ_ROW = 100, SQ_DONE = 101 };

enum { SQ_READONLY = 1 };

enum { SRC_NONE, SRC_PATH, SRC_FILE, SRC_HTTP, SRC_OTHER };

struct sqlite3;
struct sqlite3_stmt;

struct toast {
	int ntext, len[TEXTS];
	bool logo;
	char tmpl[24];
	WCHAR text[TEXTS][512], src[1024];
};

struct ctx {
	HANDLE stop, ev, reg_ev, dir;
	HKEY key;
	OVERLAPPED ov;
	struct sqlite3 *db;
	struct sqlite3_stmt *st_max, *st_rows;
	long long last;
	bool open, blocked, pending;
	double due, settle, last_q, retry, backoff;
	long wakes, db_wakes, queries, toasts, discord;
	int plen;
	DWORD buf[1024];
	char pay[PAYLOAD], aumid[128];
	WCHAR path[MAX_PATH];
	struct toast t;
	struct notif n;
};

static const char *const sq_names[] = { "sqlite3_open_v2", "sqlite3_close", "sqlite3_prepare_v2", "sqlite3_finalize", "sqlite3_reset", "sqlite3_step", "sqlite3_bind_int64", "sqlite3_column_int64", "sqlite3_column_blob", "sqlite3_column_bytes", "sqlite3_column_text", "sqlite3_busy_timeout", "sqlite3_exec" };
static const char *const discord_ids[] = { "com.squirrel.Discord.Discord", "com.squirrel.DiscordPTB.DiscordPTB", "com.squirrel.DiscordCanary.DiscordCanary" };
static const char *const src_names[] = { "none", "path", "file-uri", "http", "other" };
static const char *const state_names[] = { "off", "starting", "active", "blocked by the Windows privacy setting", "unavailable" };
static const char q_max[] = "SELECT COALESCE(MAX(Id), 0) FROM Notification";
static const char q_rows[] = "SELECT n.Id, n.ArrivalTime, n.Payload, h.PrimaryId FROM Notification n JOIN NotificationHandler h ON h.RecordId = n.HandlerId WHERE n.Id > ?1 AND n.Type = 'toast' AND (?2 OR h.PrimaryId IN ('com.squirrel.Discord.Discord', 'com.squirrel.DiscordPTB.DiscordPTB', 'com.squirrel.DiscordCanary.DiscordCanary')) ORDER BY n.Id LIMIT 1";
static const WCHAR consent_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\userNotificationListener";
static struct worker wk;
static HMODULE sq_dll;
static struct ctx *cx;
static volatile LONG state;
static bool want, diag;

static union {
	FARPROC p[ARRAYSIZE(sq_names)];
	struct {
		int (*open)(const char *name, struct sqlite3 **db, int flags, const char *vfs);
		int (*close)(struct sqlite3 *db);
		int (*prepare)(struct sqlite3 *db, const char *sql, int n, struct sqlite3_stmt **st, const char **tail);
		int (*finalize)(struct sqlite3_stmt *st);
		int (*reset)(struct sqlite3_stmt *st);
		int (*step)(struct sqlite3_stmt *st);
		int (*bind_int64)(struct sqlite3_stmt *st, int i, long long v);
		long long (*column_int64)(struct sqlite3_stmt *st, int i);
		const void *(*column_blob)(struct sqlite3_stmt *st, int i);
		int (*column_bytes)(struct sqlite3_stmt *st, int i);
		const unsigned char *(*column_text)(struct sqlite3_stmt *st, int i);
		int (*busy_timeout)(struct sqlite3 *db, int ms);
		int (*exec)(struct sqlite3 *db, const char *sql, int (*cb)(void *arg, int n, char **v, char **c), void *arg, char **err);
	} f;
} sq;

static void sq_free(void) {
	if (sq_dll) {
		FreeLibrary(sq_dll);
	}
	sq_dll = NULL;
}

static bool sq_load(void) {
	static bool warned;
	bool ok;
	int i;
	if (sq_dll) return true;
	sq_dll = LoadLibraryExW(L"winsqlite3.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	ok = sq_dll != NULL;
	for (i = 0; ok && i < (int)ARRAYSIZE(sq_names); i++) {
		sq.p[i] = GetProcAddress(sq_dll, sq_names[i]);
		ok = sq.p[i] != NULL;
	}
	if (!ok && !warned) {
		glog("winnotif: System32 winsqlite3.dll %s", sq_dll ? "lacks a needed function" : "cannot be loaded");
	}
	warned |= !ok;
	if (!ok) {
		sq_free();
	}
	return ok;
}

static void set_state(int s, const char *why) {
	if (InterlockedExchange(&state, s) == s) return;
	glog("winnotif: %s%s%s%s", state_names[s], why[0] ? " (" : "", why, why[0] ? ")" : "");
	app_wake();
}

static const char *entity(const char *p, const char *e, uint32_t *c) {
	static const char *const names[] = { "amp;", "lt;", "gt;", "quot;", "apos;" };
	static const char vals[] = "&<>\"'";
	const char *q = p + 1;
	uint32_t v = 0;
	int base = 10, nd = 0, i, d;
	size_t k;
	for (i = 0; i < (int)ARRAYSIZE(names); i++) {
		k = strlen(names[i]);
		if ((size_t)(e - q) >= k && !memcmp(q, names[i], k)) {
			*c = (unsigned char)vals[i];
			return q + k;
		}
	}
	if (q < e && *q == '#') {
		q++;
		if (q < e && (*q | 32) == 'x') {
			base = 16;
			q++;
		}
		for (; q < e && nd < 8; q++, nd++) {
			d = hex_digit(*q);
			if (d < 0 || d >= base) break;
			v = v * base + d;
		}
		if (nd && q < e && *q == ';' && v && v <= 0x10FFFF && (v < 0xD800 || v >= 0xE000)) {
			*c = v;
			return q + 1;
		}
	}
	*c = '&';
	return p + 1;
}

static const char *chars(const char *p, const char *e, char stop, WCHAR *w, int *n, int cap) {
	uint32_t c;
	while (p < e && *p != stop) {
		if (*p == '&') {
			p = entity(p, e, &c);
		} else if ((unsigned char)*p >= 0x80) {
			c = utf8_next(&p, e);
		} else {
			c = (unsigned char)*p++;
		}
		if (w && c) {
			*n = utf16_put(w, *n, cap, c);
		}
	}
	return p;
}

static const char *name(const char *p, const char *e, char *out, int cap) {
	int n = 0;
	for (; p < e && (unsigned char)*p > ' ' && *p != '/' && *p != '>' && *p != '='; p++) {
		if (n < cap - 1) {
			out[n++] = *p;
		}
	}
	out[n] = 0;
	return p;
}

static const char *sp(const char *p, const char *e) {
	while (p < e && (unsigned char)*p <= ' ') {
		p++;
	}
	return p;
}

static const char *past(const char *p, const char *e, const char *s) {
	size_t n = strlen(s);
	for (; p + n <= e; p++) {
		if (!memcmp(p, s, n)) return p + n;
	}
	return e;
}

static void tidy(WCHAR *s, bool lines) {
	WCHAR *d = s, *p;
	size_t k;
	for (p = s; *p; p++) {
		if (*p == L'\r') continue;
		*d++ = *p == L'\n' || *p == 0x2028 || *p == 0x2029 ? (lines ? L'\n' : L' ') : *p < 32 || (*p >= 0x7F && *p <= 0x9F) ? L' ' : *p;
	}
	while (d > s && (d[-1] == L' ' || d[-1] == L'\n')) {
		d--;
	}
	*d = 0;
	k = wcsspn(s, L" \n");
	memmove(s, s + k, (wcslen(s + k) + 1) * sizeof(WCHAR));
}

static void xml_toast(const char *s, int len, struct toast *t) {
	const char *p = s, *e = s + (len > 0 ? len : 0);
	char tag[16], an[16], q;
	WCHAR tm[24], pl[24], sr[1024], *v;
	int bind = 0, cur = -1, n = 0, i;
	bool end, self;
	memset(t, 0, sizeof(*t));
	if (e - p >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) {
		p += 3;
	}
	while (p < e) {
		if (*p != '<') {
			p = chars(p, e, '<', cur >= 0 ? t->text[cur] : NULL, cur >= 0 ? &t->len[cur] : &n, ARRAYSIZE(t->text[0]));
			continue;
		}
		if (e - p >= 4 && !memcmp(p, "<!--", 4)) {
			p = past(p + 4, e, "-->");
			continue;
		}
		if (e - p >= 9 && !memcmp(p, "<![CDATA[", 9)) {
			p = past(p + 9, e, "]]>");
			continue;
		}
		if (e - p >= 2 && (p[1] == '?' || p[1] == '!')) {
			p = past(p, e, ">");
			continue;
		}
		end = e - p >= 2 && p[1] == '/';
		p = name(p + 1 + end, e, tag, sizeof(tag));
		self = false;
		tm[0] = pl[0] = sr[0] = 0;
		for (;;) {
			p = sp(p, e);
			if (p >= e) break;
			if (*p == '>') {
				p++;
				break;
			}
			if (*p == '/') {
				self = true;
				p++;
				continue;
			}
			p = sp(name(p, e, an, sizeof(an)), e);
			if (p >= e || *p != '=') continue;
			p = sp(p + 1, e);
			if (p >= e) break;
			q = *p;
			if (q != '"' && q != '\'') {
				p = name(p, e, an, sizeof(an));
				continue;
			}
			v = !_stricmp(tag, "binding") && !_stricmp(an, "template") ? tm : !_stricmp(tag, "image") && !_stricmp(an, "placement") ? pl : !_stricmp(tag, "image") && !_stricmp(an, "src") ? sr : NULL;
			n = 0;
			p = chars(p + 1, e, q, v, &n, v == sr ? ARRAYSIZE(sr) : ARRAYSIZE(tm));
			p += p < e;
		}
		if (end && !_stricmp(tag, "text")) {
			cur = -1;
		} else if (end && !_stricmp(tag, "binding") && bind == 1) {
			bind = 2;
		} else if (!end && !_stricmp(tag, "binding") && !bind) {
			bind = 1;
			for (i = 0; tm[i] && i < (int)sizeof(t->tmpl) - 1; i++) {
				t->tmpl[i] = (tm[i] >= L'0' && tm[i] <= L'9') || ((tm[i] | 32) >= L'a' && (tm[i] | 32) <= L'z') ? (char)tm[i] : '_';
			}
		} else if (!end && bind == 1 && !_stricmp(tag, "text")) {
			cur = !self && t->ntext < TEXTS ? t->ntext : -1;
			t->ntext += t->ntext < TEXTS;
		} else if (!end && bind == 1 && !_stricmp(tag, "image") && sr[0] && (!t->src[0] || (!t->logo && !_wcsicmp(pl, L"appLogoOverride")))) {
			wcscpy(t->src, sr);
			t->logo = !_wcsicmp(pl, L"appLogoOverride");
		}
	}
	for (i = 0; i < t->ntext; i++) {
		tidy(t->text[i], i > 0);
		t->len[i] = (int)wcslen(t->text[i]);
	}
}

static int src_path(const WCHAR *src, WCHAR *out, int cap) {
	char u[MAX_PATH * 3], *d;
	const char *s;
	const WCHAR *p = src;
	int kind = SRC_PATH, i;
	out[0] = 0;
	if (!src[0]) return SRC_NONE;
	if (!_wcsnicmp(src, L"http://", 7) || !_wcsnicmp(src, L"https://", 8)) return SRC_HTTP;
	if (!_wcsnicmp(src, L"file:", 5)) {
		kind = SRC_FILE;
		p = src + 5;
		while (*p == L'/') {
			p++;
		}
		if (!_wcsnicmp(p, L"localhost/", 10)) {
			p += 10;
		}
	}
	if (!((p[0] | 32) >= L'a' && (p[0] | 32) <= L'z' && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/'))) return kind == SRC_FILE ? SRC_FILE : SRC_OTHER;
	if (kind == SRC_FILE) {
		to_utf8(p, u, sizeof(u));
		for (s = u, d = u; *s; s++) {
			if (s[0] == '%' && hex_digit(s[1]) >= 0 && hex_digit(s[2]) >= 0) {
				*d++ = (char)(hex_digit(s[1]) * 16 + hex_digit(s[2]));
				s += 2;
			} else {
				*d++ = *s;
			}
		}
		*d = 0;
		if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u, -1, out, cap)) {
			out[0] = 0;
		}
	} else if (wcslen(p) < (size_t)cap) {
		put(out, cap, p);
	}
	for (i = 0; out[i]; i++) {
		out[i] = out[i] == L'/' ? L'\\' : out[i];
	}
	return kind;
}

static void cat(WCHAR *d, int cap, const WCHAR *s) {
	int n = (int)wcslen(d);
	while (*s && n < cap - 1) {
		if (IS_HIGH_SURROGATE(*s) && n >= cap - 2) break;
		d[n++] = *s++;
	}
	d[n] = 0;
}

static bool title_split(WCHAR *title, WCHAR *where, int cap) {
	WCHAR *p = NULL, *q;
	size_t n = wcslen(title);
	for (q = wcsstr(title, L" (#"); q; q = wcsstr(q + 1, L" (#")) {
		p = q;
	}
	if (!p || wcsspn(title, L" ") >= (size_t)(p - title) || title[n - 1] != L')' || p + 3 >= title + n - 1) return false;
	title[n - 1] = 0;
	where[0] = 0;
	cat(where, cap, p + 2);
	*p = 0;
	tidy(title, false);
	return true;
}

static bool on_disk(const WCHAR *path) {
	WCHAR root[4] = { path[0], L':', L'\\', 0 };
	UINT dt = path[0] ? GetDriveTypeW(root) : DRIVE_UNKNOWN;
	return dt == DRIVE_FIXED || dt == DRIVE_REMOVABLE;
}

static int avatar(struct ctx *c, const WCHAR *path) {
	if (!on_disk(path)) return NI_INITIAL;
	if (!img_local(path) && GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES && WaitForSingleObject(c->stop, AVATAR_RETRY_MS) != WAIT_TIMEOUT) return NI_INITIAL;
	return img_local(path) && img_decode_file(path, c->n.img) ? NI_IMAGE : NI_INITIAL;
}

static void clip(WCHAR *s, size_t len) {
	size_t k = wcslen(s);
	if (!k || k >= len) return;
	k -= 1 + (k > 1 && IS_LOW_SURROGATE(s[k - 1]));
	s[k] = 0x2026;
	s[k + 1] = 0;
}

static bool build(struct ctx *c, int len, int *kind, bool *split) {
	struct toast *t = &c->t;
	struct notif *n = &c->n;
	WCHAR where[256];
	int i;
	xml_toast(c->pay, len, t);
	*kind = src_path(t->src, c->path, ARRAYSIZE(c->path));
	notif_blank(n, NO_DISCORD);
	n->icon = NI_INITIAL;
	n->volume = 0.7f;
	n->count = 1;
	*split = title_split(t->text[0], where, ARRAYSIZE(where));
	cat(n->title, ARRAYSIZE(n->title), t->text[0]);
	for (i = 1; i < t->ntext; i++) {
		if (!t->text[i][0]) continue;
		if (n->body[0]) {
			cat(n->body, ARRAYSIZE(n->body), L"\n");
		}
		cat(n->body, ARRAYSIZE(n->body), t->text[i]);
	}
	cat(n->source, ARRAYSIZE(n->source), L"Discord");
	if (*split) {
		cat(n->source, ARRAYSIZE(n->source), L" \u00b7 ");
		cat(n->source, ARRAYSIZE(n->source), where);
		clip(n->source, wcslen(where) + 10);
	}
	if (!n->title[0] && !n->body[0]) return false;
	n->timeout = 3 + (wcslen(n->title) + wcslen(n->body)) / 15.0f;
	n->icon = avatar(c, c->path);
	return true;
}

static bool is_discord(const char *aumid) {
	int i;
	for (i = 0; i < (int)ARRAYSIZE(discord_ids); i++) {
		if (!_stricmp(aumid, discord_ids[i])) return true;
	}
	return false;
}

static bool png_size(const WCHAR *path, int *w, int *h) {
	unsigned char b[24];
	DWORD got = 0;
	HANDLE f;
	f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_NO_RECALL, NULL);
	if (f == INVALID_HANDLE_VALUE) return false;
	if (!ReadFile(f, b, sizeof(b), &got, NULL)) {
		got = 0;
	}
	CloseHandle(f);
	if (got < sizeof(b) || memcmp(b, "\x89PNG\r\n\x1a\n", 8) || memcmp(b + 12, "IHDR", 4)) return false;
	*w = b[16] << 24 | b[17] << 16 | b[18] << 8 | b[19];
	*h = b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23];
	return true;
}

static void report(struct ctx *c, long long id, double lat, double ms, bool ok, int kind, bool split) {
	char lens[64], px[32];
	int k = 0, w = 0, h = 0, i;
	DWORD at = on_disk(c->path) ? GetFileAttributesW(c->path) : INVALID_FILE_ATTRIBUTES;
	lens[0] = 0;
	for (i = 0; i < c->t.ntext && k >= 0 && k < (int)sizeof(lens); i++) {
		k += snprintf(lens + k, sizeof(lens) - k, i ? ",%d" : "%d", c->t.len[i]);
	}
	strcpy(px, "-");
	if (at != INVALID_FILE_ATTRIBUTES && png_size(c->path, &w, &h)) {
		snprintf(px, sizeof(px), "%dx%d", w, h);
	}
	glog("toast id=%lld aumid=%s discord=%s template=%s texts=%d lens=%s image=%s exists=%s png=%s avatar=%s split=%s payload=%d latency=%.0f ms build=%.1f ms", id, c->aumid, is_discord(c->aumid) ? "yes" : "no", c->t.tmpl[0] ? c->t.tmpl : "none", c->t.ntext, lens[0] ? lens : "-", src_names[kind], at == INVALID_FILE_ATTRIBUTES ? "no" : "yes", px, ok && c->n.icon == NI_IMAGE ? "decoded" : "initial", split ? "yes" : "no", c->plen, lat, ms);
}

static void handle(struct ctx *c, long long id, long long arrival) {
	FILETIME ft;
	ULARGE_INTEGER u;
	double t0 = now_s(), lat;
	bool ok = false, split = false;
	int kind = SRC_NONE;
	GetSystemTimePreciseAsFileTime(&ft);
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	lat = (double)((long long)u.QuadPart - arrival) / 10000.0;
	c->toasts++;
	c->discord += is_discord(c->aumid);
	if (c->plen > PAYLOAD) {
		glog("toast id=%lld aumid=%s skipped: payload of %d bytes", id, c->aumid, c->plen);
		return;
	}
	ok = build(c, c->plen, &kind, &split);
	if (diag) {
		report(c, id, lat, (now_s() - t0) * 1000, ok, kind, split);
	} else {
		glog("discord: toast %lld, %d texts, image %s %s, title split %s, latency %.0f ms%s", id, c->t.ntext, src_names[kind], c->n.icon == NI_IMAGE ? "decoded" : "not used", split ? "yes" : "no", lat, lat > STALE_S * 1000 ? ", too old: skipped" : "");
	}
	if (ok && !diag && lat <= STALE_S * 1000 && WaitForSingleObject(c->stop, 0) == WAIT_TIMEOUT) {
		notif_push(&c->n);
	}
}

static int poll(struct ctx *c) {
	long long mx = 0, id, at;
	const unsigned char *a;
	const void *b;
	int rc, n = 0, i;
	rc = sq.f.step(c->st_max);
	if (rc == SQ_ROW) {
		mx = sq.f.column_int64(c->st_max, 0);
	}
	sq.f.reset(c->st_max);
	if (rc != SQ_ROW) return -rc;
	if (c->last < 0 || mx < c->last) {
		c->last = mx;
		return 0;
	}
	while (mx > c->last && WaitForSingleObject(c->stop, 0) == WAIT_TIMEOUT) {
		sq.f.bind_int64(c->st_rows, 1, c->last);
		sq.f.bind_int64(c->st_rows, 2, diag);
		rc = sq.f.step(c->st_rows);
		if (rc != SQ_ROW) {
			sq.f.reset(c->st_rows);
			if (rc != SQ_DONE) return -rc;
			c->last = mx;
			break;
		}
		id = sq.f.column_int64(c->st_rows, 0);
		at = sq.f.column_int64(c->st_rows, 1);
		b = sq.f.column_blob(c->st_rows, 2);
		c->plen = sq.f.column_bytes(c->st_rows, 2);
		if (b && c->plen > 0) {
			memcpy(c->pay, b, c->plen < PAYLOAD ? c->plen : PAYLOAD);
		}
		a = sq.f.column_text(c->st_rows, 3);
		for (i = 0; a && a[i] && i < (int)sizeof(c->aumid) - 1; i++) {
			c->aumid[i] = a[i] >= ' ' && a[i] < 127 ? (char)a[i] : '?';
		}
		c->aumid[i] = 0;
		sq.f.reset(c->st_rows);
		c->last = id;
		handle(c, id, at);
		n++;
	}
	return n;
}

static bool denied(void) {
	WCHAR v[16];
	DWORD n;
	int i;
	for (i = 0; i < 2; i++) {
		n = sizeof(v);
		if (!RegGetValueW(i ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, consent_key, L"Value", RRF_RT_REG_SZ, NULL, v, &n) && !_wcsicmp(v, L"Deny")) return true;
	}
	return false;
}

static void watch_consent(struct ctx *c) {
	if (!c->key && RegOpenKeyExW(HKEY_CURRENT_USER, consent_key, 0, KEY_NOTIFY, &c->key)) {
		c->key = NULL;
	}
	if (c->key && RegNotifyChangeKeyValue(c->key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, c->reg_ev, TRUE)) {
		RegCloseKey(c->key);
		c->key = NULL;
	}
}

static bool arm(struct ctx *c) {
	return ReadDirectoryChangesW(c->dir, c->buf, sizeof(c->buf), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE, NULL, &c->ov, NULL);
}

static bool db_file(const DWORD *buf, DWORD got) {
	const FILE_NOTIFY_INFORMATION *f = (const void *)buf;
	const char *end = (const char *)buf + got;
	int n;
	for (;;) {
		n = (int)(f->FileNameLength / sizeof(WCHAR));
		if ((n == 14 || n == 18) && !_wcsnicmp(f->FileName, L"wpndatabase.db-wal", n)) return true;
		if (!f->NextEntryOffset || (const char *)f + f->NextEntryOffset + sizeof(*f) > end) return false;
		f = (const void *)((const char *)f + f->NextEntryOffset);
	}
}

static bool db_prepare(struct ctx *c) {
	return !sq.f.prepare(c->db, q_max, -1, &c->st_max, NULL) && !sq.f.prepare(c->db, q_rows, -1, &c->st_rows, NULL);
}

static void db_close(struct ctx *c) {
	if (c->db) {
		sq.f.finalize(c->st_max);
		sq.f.finalize(c->st_rows);
		sq.f.close(c->db);
	}
	c->st_max = c->st_rows = NULL;
	c->db = NULL;
}

static void close_all(struct ctx *c) {
	DWORD n;
	db_close(c);
	if (c->pending) {
		CancelIoEx(c->dir, &c->ov);
		GetOverlappedResult(c->dir, &c->ov, &n, TRUE);
	}
	if (c->dir != INVALID_HANDLE_VALUE) {
		CloseHandle(c->dir);
	}
	ResetEvent(c->ev);
	c->dir = INVALID_HANDLE_VALUE;
	c->pending = c->open = false;
	c->due = c->settle = 0;
}

static const char *open_db(struct ctx *c) {
	WCHAR d[MAX_PATH], la[MAX_PATH];
	char u[MAX_PATH * 3];
	DWORD n;
	if (!sq_load()) return "System32 winsqlite3.dll is not usable";
	n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
	if (!n || n >= MAX_PATH - 64) return "LOCALAPPDATA is not set";
	swprintf(d, MAX_PATH, L"%ls\\Microsoft\\Windows\\Notifications", la);
	c->dir = CreateFileW(d, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
	c->pending = c->dir != INVALID_HANDLE_VALUE && arm(c);
	if (!c->pending) return "the notifications folder cannot be watched";
	wcscat(d, L"\\wpndatabase.db");
	to_utf8(d, u, sizeof(u));
	if (sq.f.open(u, &c->db, SQ_READONLY, NULL)) return "wpndatabase.db cannot be opened read-only";
	sq.f.busy_timeout(c->db, 200);
	sq.f.exec(c->db, "PRAGMA query_only = 1; PRAGMA cache_size = -64", NULL, NULL, NULL);
	if (!db_prepare(c)) return "wpndatabase.db has an unexpected schema";
	return NULL;
}

static void start(struct ctx *c) {
	const char *why;
	if (denied()) {
		c->blocked = true;
		c->last = -1;
		set_state(WN_BLOCKED, "");
		return;
	}
	c->blocked = false;
	why = open_db(c);
	if (!why && poll(c) < 0) {
		why = "wpndatabase.db cannot be read";
	}
	if (why) {
		close_all(c);
		c->retry = now_s() + c->backoff;
		c->backoff = c->backoff < 30 ? c->backoff * 2 : 60;
		set_state(WN_UNAVAILABLE, why);
		return;
	}
	c->backoff = 1;
	c->open = true;
	set_state(WN_ACTIVE, "");
}

static void query(struct ctx *c) {
	int r = poll(c);
	c->last_q = now_s();
	c->queries++;
	if (r >= 0) return;
	if (-r == SQ_BUSY || -r == SQ_LOCKED) {
		c->due = c->last_q + 0.05;
		return;
	}
	glog("winnotif: query failed (%d), reopening", -r);
	close_all(c);
	c->retry = c->last_q + c->backoff;
	set_state(WN_UNAVAILABLE, "wpndatabase.db read error");
}

static DWORD timeout(const struct ctx *c, double now) {
	double t = 0;
	if (c->open && c->due > 0) {
		t = c->due;
	}
	if (c->open && c->settle > 0 && (!t || c->settle < t)) {
		t = c->settle;
	}
	if (!c->open && !c->blocked) {
		t = c->retry;
	}
	if (t <= 0) return INFINITE;
	return t <= now ? 0 : (DWORD)((t - now) * 1000) + 1;
}

static void wake(struct ctx *c, double now) {
	DWORD got = 0;
	bool ok = GetOverlappedResult(c->dir, &c->ov, &got, FALSE), hit;
	c->wakes++;
	hit = !got || db_file(c->buf, got);
	c->pending = ok && arm(c);
	if (!c->pending) {
		glog("winnotif: the folder watch stopped (%lu)", GetLastError());
		close_all(c);
		c->retry = now + c->backoff;
		set_state(WN_UNAVAILABLE, "the notifications folder cannot be watched");
		return;
	}
	if (!hit) return;
	c->db_wakes++;
	c->settle = now + SETTLE_S;
	if (c->due <= 0) {
		c->due = c->last_q + DEBOUNCE_S > now ? c->last_q + DEBOUNCE_S : now;
	}
}

static DWORD WINAPI run(void *arg) {
	struct ctx *c = arg;
	HANDLE hs[3];
	double now;
	DWORD r;
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	hs[0] = c->stop;
	hs[1] = c->ev;
	hs[2] = c->reg_ev;
	watch_consent(c);
	for (;;) {
		now = now_s();
		if (!c->open && !c->blocked && now >= c->retry) {
			start(c);
			continue;
		}
		if (c->open && ((c->due > 0 && now >= c->due) || (c->settle > 0 && now >= c->settle))) {
			c->due = 0;
			if (now >= c->settle) {
				c->settle = 0;
			}
			query(c);
			continue;
		}
		r = WaitForMultipleObjects(c->key ? 3 : 2, hs, FALSE, timeout(c, now));
		if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
		if (r == WAIT_OBJECT_0 + 1 && c->open) {
			wake(c, now_s());
		} else if (r == WAIT_OBJECT_0 + 2) {
			watch_consent(c);
			if (denied() && !c->blocked) {
				close_all(c);
				c->blocked = true;
				c->last = -1;
				set_state(WN_BLOCKED, "");
			} else if (!denied() && c->blocked) {
				c->blocked = false;
				c->retry = 0;
			}
		}
	}
	close_all(c);
	if (c->key) {
		RegCloseKey(c->key);
	}
	glog("winnotif: stopped after %ld folder wakes, %ld database wakes, %ld queries, %ld toasts (%ld from Discord)", c->wakes, c->db_wakes, c->queries, c->toasts, c->discord);
	CoUninitialize();
	return 0;
}

static void ctx_free(struct ctx *c) {
	if (!c) return;
	if (c->stop) {
		CloseHandle(c->stop);
	}
	if (c->ev) {
		CloseHandle(c->ev);
	}
	if (c->reg_ev) {
		CloseHandle(c->reg_ev);
	}
	free(c);
}

static struct ctx *ctx_new(void) {
	struct ctx *c = calloc(1, sizeof(*c));
	if (!c) return NULL;
	c->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
	c->ev = CreateEventW(NULL, TRUE, FALSE, NULL);
	c->reg_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
	c->dir = INVALID_HANDLE_VALUE;
	c->ov.hEvent = c->ev;
	c->last = -1;
	c->backoff = 1;
	c->last_q = -1;
	if (c->stop && c->ev && c->reg_ev) return c;
	ctx_free(c);
	return NULL;
}

static void stop(DWORD ms) {
	if (!wk.th) return;
	if (worker_stop(&wk, ms, "winnotif")) {
		ctx_free(cx);
	}
	if (!wk.stuck) {
		sq_free();
	}
	cx = NULL;
}

void winnotif_init(void) {
	set_state(WN_OFF, "");
}

void winnotif_config(bool on) {
	if (on == want) return;
	stop(500);
	want = on;
	if (!on) {
		set_state(WN_OFF, "");
		return;
	}
	set_state(WN_STARTING, "");
	cx = ctx_new();
	if (worker_start(&wk, cx ? cx->stop : NULL, run, cx)) return;
	ctx_free(cx);
	cx = NULL;
	set_state(WN_UNAVAILABLE, "cannot start the worker thread");
}

int winnotif_status(void) {
	return (int)state;
}

void winnotif_shutdown(void) {
	stop(2000);
	want = false;
	set_state(WN_OFF, "");
}

static double cpu_ms(void) {
	FILETIME a, b, k, u;
	if (!GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u)) return 0;
	return ((double)(((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (double)(((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 10000.0;
}

int winnotif_toasts(int secs) {
	PROCESS_MEMORY_COUNTERS_EX m;
	double end, left, c0;
	bool seen = false;
	diag = true;
	glog("toasts: watching the Windows toasts of every app for %d s; only their structure is logged (no text, names or images)", secs);
	winnotif_init();
	c0 = cpu_ms();
	winnotif_config(true);
	end = now_s() + secs;
	for (;;) {
		seen |= winnotif_status() == WN_ACTIVE;
		left = end - now_s();
		if (left <= 0) break;
		WaitForSingleObject(g_app.wake, (DWORD)(left * 1000) + 1);
	}
	winnotif_shutdown();
	memset(&m, 0, sizeof(m));
	m.cb = sizeof(m);
	GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof(m));
	glog("toasts: done; CPU %.1f ms while watching for %d s, process private memory %.1f MB", cpu_ms() - c0, secs, m.PrivateUsage / 1048576.0);
	diag = false;
	return !seen;
}

#ifdef GL_TESTS
enum { SQ_READWRITE = 2, SQ_CREATE = 4 };

static WCHAR got_title[4][32];
static int got;

static bool capture(const struct notif *n) {
	if (got < (int)ARRAYSIZE(got_title)) {
		put(got_title[got], ARRAYSIZE(got_title[0]), n->title);
	}
	got++;
	return true;
}

static int ck(bool ok, const char *what) {
	return st_ck(ok, "winnotif", what);
}

static void parse(struct toast *t, const char *x) {
	xml_toast(x, (int)strlen(x), t);
}

static bool card(struct ctx *c, const char *x, int *kind, bool *split) {
	int n = (int)strlen(x);
	memcpy(c->pay, x, n);
	return build(c, n, kind, split);
}

static bool path_is(const WCHAR *src, int kind, const WCHAR *want_path) {
	WCHAR o[MAX_PATH];
	return src_path(src, o, MAX_PATH) == kind && !wcscmp(o, want_path);
}

static bool split_is(const WCHAR *title, const WCHAR *who, const WCHAR *where) {
	WCHAR t[128], w[64];
	put(t, ARRAYSIZE(t), title);
	if (!title_split(t, w, ARRAYSIZE(w))) return !where && !wcscmp(t, title);
	return where && !wcscmp(t, who) && !wcscmp(w, where);
}

static int xml_test(struct toast *t) {
	static const char a[] = "<toast launch=\"x\"><visual><binding template=\"ToastGeneric\"><text>Caf&#xE9; &amp; co</text><text>Hi &lt;b&gt; &quot;q&quot; &apos;a&apos; &#128512; \xF0\x9F\x98\x80 \xC3\xA9</text><image placement='appLogoOverride' src='C:/Temp/a b.png' hint-crop=\"none\"/></binding></visual><actions><action content=\"Reply\" arguments=\"r\"/><input id=\"t\"><text>no</text></input></actions><audio silent=\"true\"/></toast>";
	static const char b[] = "\xEF\xBB\xBF<?xml version=\"1.0\"?><!-- <text>no</text> --><toast><visual><binding template=\"ToastImageAndText02\"><image id=\"1\" src=\"file:///C:/My%20Pics/a%C3%A9.png\"/><text id=\"1\">Title</text><text id=\"2\">Line 1\r\n\tLine 2  </text></binding><binding template=\"ToastGeneric\"><text>dup</text></binding></visual></toast>";
	static const char c[] = "<toast><visual><binding template='ToastGeneric'><text>  Only one  </text></binding></visual><audio silent='true'/></toast>";
	static const char d[] = "<toast><visual><binding template=\"ToastGeneric\"><text>a &nbsp; &#xD800; &#99999999; &amp b";
	static const char e[] = "<toast><visual><binding template=\"ToastGeneric\"><image placement=\"hero\" src=\"C:\\h.png\"/><text/><text>x</text><image placement=\"appLogoOverride\" src=\"C:\\l.png\"/><image src=\"C:\\z.png\"/></binding></visual></toast>";
	char big[700];
	int f = 0;
	parse(t, a);
	f += ck(t->ntext == 2 && !wcscmp(t->text[0], L"Caf\u00e9 & co") && !wcscmp(t->text[1], L"Hi <b> \"q\" 'a' \U0001F600 \U0001F600 \u00e9") && !wcscmp(t->src, L"C:/Temp/a b.png") && t->logo && !strcmp(t->tmpl, "ToastGeneric") && t->len[0] == 9, "xml entities, emoji, quotes, actions ignored");
	parse(t, b);
	f += ck(t->ntext == 2 && !wcscmp(t->text[0], L"Title") && !wcscmp(t->text[1], L"Line 1\n Line 2") && !wcscmp(t->src, L"file:///C:/My%20Pics/a%C3%A9.png") && !t->logo && !strcmp(t->tmpl, "ToastImageAndText02"), "legacy template, first binding only, comment, bom");
	parse(t, c);
	f += ck(t->ntext == 1 && !wcscmp(t->text[0], L"Only one") && !t->src[0], "one text, no image");
	parse(t, "<toast><visual><binding><text><![CDATA[a <x> b]]>c</text></binding></visual></toast>");
	f += ck(t->ntext == 1 && !wcscmp(t->text[0], L"c"), "cdata skipped whole");
	parse(t, d);
	f += ck(t->ntext == 1 && !wcscmp(t->text[0], L"a &nbsp; &#xD800; &#99999999; &amp b"), "bad entities and truncated xml");
	parse(t, e);
	f += ck(t->ntext == 2 && !t->text[0][0] && !wcscmp(t->text[1], L"x") && !wcscmp(t->src, L"C:\\l.png") && t->logo, "appLogoOverride preferred, empty text");
	snprintf(big, sizeof(big), "<toast><visual><binding><text>%0510d\xF0\x9F\x98\x80 tail</text></binding></visual></toast>", 0);
	parse(t, big);
	f += ck(t->ntext == 1 && t->len[0] == 510 && !t->tmpl[0], "long text keeps surrogate pairs whole");
	parse(t, "");
	f += ck(!t->ntext && !t->src[0], "empty payload");
	return f;
}

static int path_test(void) {
	WCHAR l[300];
	int f = 0;
	wmemset(l, L'a', ARRAYSIZE(l) - 1);
	wcsncpy(l, L"C:\\", 3);
	l[ARRAYSIZE(l) - 1] = 0;
	f += ck(path_is(L"C:\\Users\\a\\x.png", SRC_PATH, L"C:\\Users\\a\\x.png") && path_is(l, SRC_PATH, L""), "plain path, overlong path rejected");
	f += ck(path_is(L"F:/Temp\\Windows/u.png", SRC_PATH, L"F:\\Temp\\Windows\\u.png"), "mixed separators");
	f += ck(path_is(L"file:///C:/My%20Pics/a%C3%A9.png", SRC_FILE, L"C:\\My Pics\\a\u00e9.png"), "file uri with escapes");
	f += ck(path_is(L"FILE://localhost/D:/x%2.png", SRC_FILE, L"D:\\x%2.png"), "file uri localhost, bad escape kept");
	f += ck(path_is(L"file://server/share/a.png", SRC_FILE, L""), "file uri unc rejected");
	f += ck(path_is(L"https://cdn.discordapp.com/a.png", SRC_HTTP, L""), "https never fetched");
	f += ck(path_is(L"ms-appdata:///local/a.png", SRC_OTHER, L"") && path_is(L"\\\\server\\share\\a.png", SRC_OTHER, L"") && path_is(L"", SRC_NONE, L""), "other sources");
	return f;
}

static int split_test(void) {
	int f = 0;
	f += ck(split_is(L"Mihoko-Okayami (#general, Test Server)", L"Mihoko-Okayami", L"#general, Test Server"), "server title");
	f += ck(split_is(L"Mihoko-Okayami (#general)", L"Mihoko-Okayami", L"#general"), "channel only title");
	f += ck(split_is(L"Bob (Admin) (#dev, X (EU))", L"Bob (Admin)", L"#dev, X (EU)"), "parentheses in names");
	f += ck(split_is(L"Group chat (3)", NULL, NULL) && split_is(L"(#x)", NULL, NULL) && split_is(L"Mihoko-Okayami (#)", NULL, NULL) && split_is(L"Mihoko-Okayami (#a, b) later", NULL, NULL) && split_is(L"Mihoko-Okayami", NULL, NULL) && split_is(L"   (#general)", NULL, NULL), "titles left whole");
	return f;
}

static int card_test(struct ctx *c) {
	static const char srv[] = "<toast><visual><binding template=\"ToastGeneric\"><text>Mihoko-Okayami (#general, Test Server)</text><text>one</text><text></text><text>two</text></binding></visual><audio silent=\"true\"/></toast>";
	static const char dm[] = "<toast><visual><binding template=\"ToastGeneric\"><text>Mihoko-Okayami</text></binding></visual></toast>";
	static const char png[] = "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52\x00\x00\x00\x02\x00\x00\x00\x02\x08\x06\x00\x00\x00\x72\xb6\x0d\x24\x00\x00\x00\x11\x49\x44\x41\x54\x78\x9c\x63\xf8\xcf\xc0\x00\x42\xff\x19\x60\x0c\x00\x43\xce\x07\xf9\x58\xed\x66\xa0\x00\x00\x00\x00\x49\x45\x4e\x44\xae\x42\x60\x82";
	WCHAR p[MAX_PATH];
	char u[MAX_PATH * 3], x[MAX_PATH * 4], *s, *d;
	DWORD k = GetTempPathW(MAX_PATH - 40, p), wr = 0;
	HANDLE h;
	bool split, ok;
	int kind, f = 0, w = 0, ht = 0;
	ok = card(c, srv, &kind, &split);
	f += ck(ok && split && kind == SRC_NONE && !wcscmp(c->n.title, L"Mihoko-Okayami") && !wcscmp(c->n.source, L"Discord \u00b7 #general, Test Server") && !wcscmp(c->n.body, L"one\ntwo") && c->n.icon == NI_INITIAL && c->n.sfx == SFX_NOTIFY && c->n.timeout == 3 + 21 / 15.0f && c->n.origin == NO_DISCORD && c->n.count == 1, "server card mapping");
	ok = card(c, dm, &kind, &split);
	f += ck(ok && !split && !wcscmp(c->n.title, L"Mihoko-Okayami") && !wcscmp(c->n.source, L"Discord") && !c->n.body[0], "single text card");
	f += ck(!card(c, "<toast><visual><binding><text> </text></binding></visual></toast>", &kind, &split), "blank toast rejected");
	snprintf(x, sizeof(x), "<toast><visual><binding><text>Mihoko-Okayami (#%0100d, %0100d)</text><text>hi</text><image src=\"\\\\server\\share\\a.png\"/></binding></visual></toast>", 0, 0);
	f += ck(card(c, x, &kind, &split) && split && kind == SRC_OTHER && c->n.icon == NI_INITIAL && !wcscmp(c->n.title, L"Mihoko-Okayami") && wcslen(c->n.source) == ARRAYSIZE(c->n.source) - 1 && c->n.source[ARRAYSIZE(c->n.source) - 2] == 0x2026, "long server title split, source ellipsized, unc avatar ignored");
	swprintf(p + k, MAX_PATH - k, L"ghostlay selftest %lu.png", GetCurrentProcessId());
	h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
	ok = h != INVALID_HANDLE_VALUE && WriteFile(h, png, sizeof(png) - 1, &wr, NULL) && wr == sizeof(png) - 1;
	if (h != INVALID_HANDLE_VALUE) {
		CloseHandle(h);
	}
	to_utf8(p, u, sizeof(u));
	d = x + snprintf(x, sizeof(x), "<toast><visual><binding template=\"ToastGeneric\"><text>Mihoko-Okayami</text><text>hi</text><image placement=\"appLogoOverride\" src=\"file:///");
	for (s = u; *s && d < x + sizeof(x) - 200; s++) {
		if (*s == ' ') {
			d += sprintf(d, "%%20");
		} else {
			*d++ = *s == '\\' ? '/' : *s;
		}
	}
	strcpy(d, "\"/></binding></visual></toast>");
	f += ck(ok && card(c, x, &kind, &split) && kind == SRC_FILE && c->n.icon == NI_IMAGE && c->n.img[3] == 255 && png_size(c->path, &w, &ht) && w == 2 && ht == 2, "file uri avatar decoded");
	DeleteFileW(p);
	f += ck(card(c, x, &kind, &split) && c->n.icon == NI_INITIAL, "missing avatar falls back to the initial");
	return f;
}

static bool run_sql(struct ctx *c, const char *fmt, const char *x, long long t) {
	static char sql[8192];
	snprintf(sql, sizeof(sql), fmt, x, t);
	return !sq.f.exec(c->db, sql, NULL, NULL, NULL);
}

static int sql_test(struct ctx *c) {
	static const char toast[] = "<toast><visual><binding template=\"ToastGeneric\"><text>%s</text><text>body</text></binding></visual></toast>";
	char a[256], b[256], s[256], q[256], o[256], sh[256];
	FILETIME ft;
	ULARGE_INTEGER u;
	long long now;
	int f = 0, n;
	if (!sq_load()) return ck(false, "winsqlite3.dll");
	GetSystemTimePreciseAsFileTime(&ft);
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	now = (long long)u.QuadPart;
	snprintf(o, sizeof(o), toast, "Old");
	snprintf(a, sizeof(a), toast, "Third");
	snprintf(b, sizeof(b), toast, "Second");
	snprintf(sh, sizeof(sh), toast, "Shell");
	snprintf(s, sizeof(s), toast, "Stale");
	snprintf(q, sizeof(q), toast, "Fourth (#general, Test Server)");
	f += ck(!sq.f.open(":memory:", &c->db, SQ_READWRITE | SQ_CREATE, NULL), "in-memory database");
	f += ck(!db_prepare(c), "schema check rejects a database without the tables");
	db_close(c);
	f += ck(!sq.f.open(":memory:", &c->db, SQ_READWRITE | SQ_CREATE, NULL), "in-memory database");
	f += ck(run_sql(c, "CREATE TABLE NotificationHandler(RecordId INTEGER PRIMARY KEY, PrimaryId TEXT NOT NULL UNIQUE COLLATE NOCASE); CREATE TABLE Notification([Order] INTEGER PRIMARY KEY, Id INTEGER NOT NULL UNIQUE, HandlerId INTEGER, Type TEXT, Payload BLOB, ArrivalTime INTEGER); INSERT INTO NotificationHandler VALUES (260, 'com.squirrel.Discord.Discord'), (7, 'COM.SQUIRREL.DISCORDPTB.DISCORDPTB'), (521, 'ps'); INSERT INTO Notification VALUES (1, 100, 260, 'toast', CAST('%s' AS BLOB), %lld);", o, now), "create tables");
	f += ck(db_prepare(c), "schema check");
	c->last = -1;
	got = 0;
	f += ck(poll(c) == 0 && c->last == 100 && !got, "no replay at start");
	f += ck(run_sql(c, "INSERT INTO Notification VALUES (2, 102, 7, 'toast', CAST('%s' AS BLOB), %lld);", b, now) && run_sql(c, "DELETE FROM Notification WHERE [Order] = 1; INSERT INTO Notification VALUES (1, 103, 260, 'toast', '%s', %lld);", a, now) && run_sql(c, "INSERT INTO Notification VALUES (3, 101, 521, 'toast', '%s', %lld);", sh, now), "insert rows");
	f += ck(run_sql(c, "INSERT INTO Notification VALUES (4, 104, 260, 'tile', '%s', %lld);", a, now) && run_sql(c, "INSERT INTO Notification VALUES (5, 105, 260, 'toast', '%s', %lld);", s, now - 6000000000LL) && run_sql(c, "INSERT INTO Notification VALUES (6, 106, 260, 'toast', '%s', %lld);", q, now), "insert more rows");
	n = poll(c);
	f += ck(n == 4 && got == 3 && !wcscmp(got_title[0], L"Second") && !wcscmp(got_title[1], L"Third") && !wcscmp(got_title[2], L"Fourth") && c->last == 106, "Id order (not the reused Order), Discord toasts only, stale skipped");
	f += ck(poll(c) == 0 && got == 3, "no duplicates");
	f += ck(run_sql(c, "INSERT INTO Notification VALUES (7, 107, 260, 'toast', '%s', %lld);", o, now) && poll(c) == 1 && got == 4 && c->last == 107, "next toast");
	diag = true;
	c->last = 100;
	f += ck(poll(c) == 6 && c->last == 107, "diagnostic mode sees every app");
	diag = false;
	db_close(c);
	return f;
}

int winnotif_selftest(void) {
	struct ctx *c = ctx_new();
	int f;
	if (!c) return 1;
	notif_tap = capture;
	f = xml_test(&c->t) + path_test() + split_test() + card_test(c) + sql_test(c);
	notif_tap = NULL;
	ctx_free(c);
	return f;
}

int winnotif_render_samples(const WCHAR *dir) {
	const struct sample_toast *s;
	struct ctx *c = ctx_new();
	WCHAR av[MAX_PATH], out[MAX_PATH];
	char u[MAX_PATH * 3], x[2048];
	bool split;
	int n = 0, kind, i;
	if (!c) return 0;
	sample_path(L"avatar.png", av, MAX_PATH);
	to_utf8(av, u, sizeof(u));
	for (i = 0; i < SAMPLE_TOASTS; i++) {
		s = &sample_toasts[i];
		snprintf(x, sizeof(x), "<toast><visual><binding template=\"ToastGeneric\"><text>%s</text><text>%s</text>%s%s%s</binding></visual><audio silent=\"true\"/></toast>", s->title, s->body, s->avatar ? "<image placement=\"appLogoOverride\" src=\"" : "", s->avatar ? u : "", s->avatar ? "\"/>" : "");
		if (!card(c, x, &kind, &split)) continue;
		c->n.count = s->count;
		swprintf(out, MAX_PATH, L"%ls\\notif-discord-%ls.png", dir, s->name);
		n += notif_render_card(&c->n, s->f, out);
	}
	ctx_free(c);
	return n;
}
#endif
