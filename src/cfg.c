#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "cfg.h"
#include "gfx.h"
#include "lang.h"
#include "main.h"

#define DEFAULT_ACCENT 0xF5B544u
#define F(s, k, t, m) { s, L##k, t, offsetof(struct cfg, m), NULL, 0 }
#define E(s, k, t, m, n) { s, L##k, t, offsetof(struct cfg, m), n, (int)ARRAYSIZE(n) }
#define R(m, real, lo, hi, step) { offsetof(struct cfg, m), real, lo, hi, step }
#define NFIELDS (int)ARRAYSIZE(fields)

struct cfg g_cfg;
uint32_t g_cfg_gen = 1;
static WCHAR ini[MAX_PATH];
static double save_at;
static bool save_failed;

enum { T_BOOL, T_INT, T_FLOAT, T_COLOR, T_ENUM, T_TIERS, T_BITS };

struct field {
	int sec;
	const WCHAR *key;
	int type;
	size_t off;
	const WCHAR *const *names;
	int count;
};

struct range {
	size_t off;
	bool real;
	float lo, hi, step;
};

static const WCHAR *const sections[] = { L"general", L"watch", L"notifications", L"battery" };
static const WCHAR *const langs[] = { L"en", L"fr" };
static const WCHAR *const wrists[] = { L"left", L"right" };
static const WCHAR *const devices[] = { L"headset", L"controllers", L"trackers" };

static const struct field fields[] = {
	E(0, "language", T_ENUM, lang, langs),
	F(0, "autolaunch", T_BOOL, autolaunch),
	E(1, "wrist", T_ENUM, wrist, wrists),
	F(1, "size", T_INT, watch_size),
	F(1, "opacity", T_INT, watch_opacity),
	F(1, "angle", T_INT, watch_angle),
	F(1, "clock24", T_BOOL, clock24),
	F(1, "accent", T_COLOR, accent),
	F(1, "media", T_BOOL, media),
	F(2, "enabled", T_BOOL, notif_on),
	F(2, "distance", T_FLOAT, notif_dist),
	F(2, "angle", T_INT, notif_angle),
	F(2, "horizontal", T_FLOAT, notif_hoff),
	F(2, "size", T_INT, notif_size),
	F(2, "min_duration", T_INT, dur_min),
	F(2, "max_duration", T_INT, dur_max),
	F(2, "sound", T_BOOL, notif_sound),
	F(2, "volume", T_INT, notif_volume),
	F(2, "udp", T_BOOL, udp_on),
	F(2, "udp_port", T_INT, udp_port),
	F(2, "discord", T_BOOL, discord),
	F(3, "alerts", T_BOOL, bat_alerts),
	F(3, "tiers", T_TIERS, bat_ntiers),
	E(3, "devices", T_BITS, bat_devices, devices),
	F(3, "sound", T_BOOL, bat_sound),
};

static const struct range ranges[CR_COUNT] = {
	[CR_WATCH_SIZE] = R(watch_size, false, 70, 130, 1),
	[CR_WATCH_ANGLE] = R(watch_angle, false, 10, 180, 5),
	[CR_WATCH_OPACITY] = R(watch_opacity, false, 10, 100, 5),
	[CR_NOTIF_DIST] = R(notif_dist, true, 0.3f, 1.2f, 0.05f),
	[CR_NOTIF_ANGLE] = R(notif_angle, false, 0, 40, 1),
	[CR_NOTIF_HOFF] = R(notif_hoff, true, -0.3f, 0.3f, 0.01f),
	[CR_NOTIF_SIZE] = R(notif_size, false, 50, 200, 5),
	[CR_VOLUME] = R(notif_volume, false, 0, 100, 5),
	[CR_DUR_MIN] = R(dur_min, false, 1, 30, 1),
	[CR_DUR_MAX] = R(dur_max, false, 1, 60, 1),
};

void cfg_defaults(struct cfg *c) {
	*c = (struct cfg){ .lang = LANG_EN, .autolaunch = false, .wrist = WRIST_LEFT, .watch_size = 100, .watch_opacity = 100, .watch_angle = 60, .accent = DEFAULT_ACCENT, .media = true, .notif_on = true, .notif_dist = 0.55f, .notif_angle = 25, .notif_size = 100, .dur_min = 2, .dur_max = 15, .notif_sound = true, .notif_volume = 70, .udp_on = true, .udp_port = DEFAULT_PORT, .discord = true, .bat_alerts = true, .bat_ntiers = 3, .bat_tiers = { 35, 25, 15, 10, 5 }, .bat_devices = BDEV_HMD | BDEV_CTRL | BDEV_TRK, .bat_sound = true };
}

static float rget(const struct cfg *c, int r) {
	const void *p = (const char *)c + ranges[r].off;
	return ranges[r].real ? *(const float *)p : (float)*(const int *)p;
}

static bool rset(struct cfg *c, int r, float v) {
	void *p = (char *)c + ranges[r].off;
	float old = rget(c, r);
	v = clampf(v, ranges[r].lo, ranges[r].hi);
	if (ranges[r].real) {
		*(float *)p = v;
	} else {
		*(int *)p = (int)lroundf(v);
	}
	return rget(c, r) != old;
}

float cfg_norm(int r) {
	return (rget(&g_cfg, r) - ranges[r].lo) / (ranges[r].hi - ranges[r].lo);
}

float cfg_snap(int r, float t) {
	const struct range *g = &ranges[r];
	return (roundf(g->lo / g->step) + roundf(t * (float)lroundf((g->hi - g->lo) / g->step))) * g->step;
}

bool cfg_set(int r, float v) {
	return rset(&g_cfg, r, v);
}

void cfg_clamp(struct cfg *c) {
	int n = 0, t[MAX_TIERS], i, v, j, k;
	struct cfg d;
	c->lang = clampi(c->lang, 0, LANG_COUNT - 1);
	c->wrist = clampi(c->wrist, WRIST_LEFT, WRIST_RIGHT);
	for (i = 0; i < CR_COUNT; i++) {
		rset(c, i, rget(c, i));
	}
	c->dur_max = c->dur_max > c->dur_min ? c->dur_max : c->dur_min;
	c->accent &= 0xFFFFFF;
	c->udp_port = clampi(c->udp_port, 1024, 65535);
	c->bat_devices &= BDEV_HMD | BDEV_CTRL | BDEV_TRK;
	for (i = 0; i < clampi(c->bat_ntiers, 0, MAX_TIERS); i++) {
		v = clampi(c->bat_tiers[i], 5, 95);
		j = n;
		for (k = 0; k < n; k++) {
			if (t[k] == v) {
				j = -1;
				break;
			}
		}
		if (j < 0) continue;
		for (; j > 0 && t[j - 1] < v; j--) {
			t[j] = t[j - 1];
		}
		t[j] = v;
		n++;
	}
	if (!n) {
		cfg_defaults(&d);
		n = d.bat_ntiers;
		memcpy(t, d.bat_tiers, sizeof(t));
	}
	c->bat_ntiers = n;
	memset(c->bat_tiers, 0, sizeof(c->bat_tiers));
	memcpy(c->bat_tiers, t, n * sizeof(*t));
}

static int format(const struct field *f, const struct cfg *c, WCHAR *b, int n) {
	const void *p = (const char *)c + f->off;
	int k = 0, i;
	switch (f->type) {
	case T_BOOL:
		return swprintf(b, n, L"%d", *(const bool *)p ? 1 : 0);
	case T_INT:
		return swprintf(b, n, L"%d", *(const int *)p);
	case T_FLOAT:
		return swprintf(b, n, L"%.3f", *(const float *)p);
	case T_COLOR:
		return swprintf(b, n, L"#%06X", *(const uint32_t *)p & 0xFFFFFF);
	case T_ENUM:
		return swprintf(b, n, L"%ls", f->names[clampi(*(const int *)p, 0, f->count - 1)]);
	case T_TIERS:
		for (i = 0; i < c->bat_ntiers && k >= 0; i++) {
			k += swprintf(b + k, n - k, i ? L",%d" : L"%d", c->bat_tiers[i]);
		}
		break;
	case T_BITS:
		for (i = 0; i < f->count && k >= 0; i++) {
			if (*(const unsigned int *)p & 1u << i) {
				k += swprintf(b + k, n - k, k ? L",%ls" : L"%ls", f->names[i]);
			}
		}
		if (!k) {
			k = swprintf(b, n, L"none");
		}
		break;
	}
	b[k > 0 ? k : 0] = 0;
	return k > 0 ? k : 0;
}

static void parse(const struct field *f, struct cfg *c, WCHAR *s) {
	void *p = (char *)c + f->off;
	WCHAR *e, *ctx = NULL, *t;
	long v;
	double d;
	int i;
	switch (f->type) {
	case T_BOOL:
		*(bool *)p = !wcscmp(s, L"1") || !_wcsicmp(s, L"true") || !_wcsicmp(s, L"on") || !_wcsicmp(s, L"yes");
		break;
	case T_INT:
		v = wcstol(s, &e, 10);
		if (e != s) {
			*(int *)p = (int)v;
		}
		break;
	case T_FLOAT:
		d = wcstod(s, &e);
		if (e != s) {
			*(float *)p = (float)d;
		}
		break;
	case T_COLOR:
		col_parse(s, (uint32_t *)p);
		break;
	case T_ENUM:
		for (i = 0; i < f->count; i++) {
			if (!_wcsicmp(s, f->names[i])) {
				*(int *)p = i;
			}
		}
		break;
	case T_TIERS:
		c->bat_ntiers = 0;
		for (t = wcstok(s, L", ", &ctx); t && c->bat_ntiers < MAX_TIERS; t = wcstok(NULL, L", ", &ctx)) {
			v = wcstol(t, &e, 10);
			if (e != t) {
				c->bat_tiers[c->bat_ntiers++] = (int)v;
			}
		}
		break;
	case T_BITS:
		*(unsigned int *)p = 0;
		for (t = wcstok(s, L", ", &ctx); t; t = wcstok(NULL, L", ", &ctx)) {
			for (i = 0; i < f->count; i++) {
				if (!_wcsicmp(t, f->names[i])) {
					*(unsigned int *)p |= 1u << i;
				}
			}
		}
		break;
	}
}

static int clock_kind(const WCHAR *f) {
	bool q = false;
	for (; *f; f++) {
		if (*f == L'\'') {
			q = !q;
		} else if (!q && *f == L'H') {
			return 1;
		} else if (!q && *f == L'h') {
			return 0;
		}
	}
	return -1;
}

static int lang_pick(LANGID id, const WCHAR *loc) {
	if (id) return PRIMARYLANGID(id) == LANG_FRENCH ? LANG_FR : LANG_EN;
	return !_wcsnicmp(loc, L"fr", 2) && (!loc[2] || loc[2] == L'-') ? LANG_FR : LANG_EN;
}

static void first_launch(struct cfg *c) {
	WCHAR loc[LOCALE_NAME_MAX_LENGTH], f[96];
	LANGID id = GetUserDefaultUILanguage();
	int k = -1;
	loc[0] = 0;
	if (!id && !GetUserDefaultLocaleName(loc, LOCALE_NAME_MAX_LENGTH)) {
		loc[0] = 0;
	}
	c->lang = lang_pick(id, loc);
	if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SSHORTTIME, f, 96)) {
		k = clock_kind(f);
	}
	if (k < 0 && GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STIMEFORMAT, f, 96)) {
		k = clock_kind(f);
	}
	if (k >= 0) {
		c->clock24 = k;
	}
	glog("first launch: UI language %04X, language %s, %d h clock", id, c->lang == LANG_FR ? "fr" : "en", c->clock24 ? 24 : 12);
}

static bool cfg_read(const WCHAR *path, struct cfg *c) {
	WCHAR v[256];
	int i;
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
	for (i = 0; i < NFIELDS; i++) {
		if (GetPrivateProfileStringW(sections[fields[i].sec], fields[i].key, L"", v, 256, path)) {
			parse(&fields[i], c, v);
		}
	}
	cfg_clamp(c);
	return true;
}

static bool cfg_write(const WCHAR *path, const struct cfg *c) {
	bool ok = true;
	WCHAR b[2048];
	int s, n, i;
	for (s = 0; s < (int)ARRAYSIZE(sections); s++) {
		n = 0;
		for (i = 0; i < NFIELDS; i++) {
			if (fields[i].sec != s) continue;
			n += swprintf(b + n, 2000 - n, L"%ls=", fields[i].key);
			n += format(&fields[i], c, b + n, 2000 - n) + 1;
		}
		b[n] = 0;
		ok &= WritePrivateProfileSectionW(sections[s], b, path) != 0;
	}
	return ok;
}

void cfg_load(void) {
	char u[MAX_PATH * 3];
	swprintf(ini, MAX_PATH, L"%ls\\ghostlay.ini", g_app.data);
	to_utf8(ini, u, sizeof(u));
	cfg_defaults(&g_cfg);
	if (cfg_read(ini, &g_cfg)) {
		glog("config loaded from %s", u);
	} else {
		first_launch(&g_cfg);
		if (cfg_write(ini, &g_cfg)) {
			glog("config created at %s", u);
		} else {
			glog("config cannot be written to %s", u);
		}
	}
	g_cfg_gen++;
}

void cfg_changed(void) {
	g_cfg_gen++;
	save_at = now_s() + 1.0;
}

void cfg_update(double now) {
	if (save_at > 0 && now >= save_at) {
		cfg_flush();
	}
}

void cfg_flush(void) {
	if (save_at <= 0) return;
	save_at = 0;
	if (!cfg_write(ini, &g_cfg)) {
		if (!save_failed) {
			glog("config save failed, will retry");
		}
		save_failed = true;
		save_at = now_s() + 10;
	} else if (save_failed) {
		glog("config saved");
		save_failed = false;
	}
}

#ifdef GL_TESTS
int cfg_selftest(void) {
	static const struct { const WCHAR *f; int want; } tf[] = { { L"HH:mm", 1 }, { L"H:mm", 1 }, { L"h:mm tt", 0 }, { L"hh:mm tt", 0 }, { L"tt h:mm", 0 }, { L"'H'h:mm", 0 }, { L"HH' h 'mm", 1 }, { L"H' h 'mm", 1 }, { L"'h'H:mm", 1 }, { L"mm:ss", -1 }, { L"", -1 } };
	static const struct { LANGID id; const WCHAR *loc; int want; } tl[] = { { 0x040C, L"", LANG_FR }, { 0x0C0C, L"", LANG_FR }, { 0x100C, L"en-US", LANG_FR }, { 0x0409, L"fr-FR", LANG_EN }, { 0x0407, L"", LANG_EN }, { 0, L"fr-BE", LANG_FR }, { 0, L"fr", LANG_FR }, { 0, L"fil-PH", LANG_EN }, { 0, L"en-US", LANG_EN }, { 0, L"", LANG_EN } };
	WCHAR p[MAX_PATH], fa[256], fb[256];
	char ua[256], ub[256];
	DWORD n;
	struct cfg a = { .lang = LANG_FR, .wrist = WRIST_RIGHT, .watch_size = 115, .watch_opacity = 45, .watch_angle = 95, .clock24 = true, .accent = 0x7CC8FF, .notif_dist = 0.8f, .notif_angle = 10, .notif_hoff = 0.12f, .notif_size = 150, .dur_min = 3, .dur_max = 20, .notif_volume = 40, .udp_port = 42100, .bat_ntiers = 2, .bat_tiers = { 40, 10 }, .bat_devices = BDEV_HMD | BDEV_TRK }, b;
	int fail = 0, i;
	for (i = 0; i < (int)ARRAYSIZE(tf); i++) {
		if (clock_kind(tf[i].f) != tf[i].want) {
			to_utf8(tf[i].f, ua, sizeof(ua));
			glog("cfg: time format \"%s\" gives %d, expected %d", ua, clock_kind(tf[i].f), tf[i].want);
			fail++;
		}
	}
	for (i = 0; i < (int)ARRAYSIZE(tl); i++) {
		if (lang_pick(tl[i].id, tl[i].loc) != tl[i].want) {
			to_utf8(tl[i].loc, ua, sizeof(ua));
			glog("cfg: language for %04X \"%s\" is %d, expected %d", tl[i].id, ua, lang_pick(tl[i].id, tl[i].loc), tl[i].want);
			fail++;
		}
	}
	n = GetTempPathW(MAX_PATH - 40, p);
	swprintf(p + n, MAX_PATH - n, L"ghostlay-selftest-%lu.ini", GetCurrentProcessId());
	if (!cfg_write(p, &a)) {
		glog("cfg: write failed");
		fail++;
	}
	cfg_defaults(&b);
	if (!cfg_read(p, &b)) {
		glog("cfg: read failed");
		fail++;
	}
	for (i = 0; i < NFIELDS; i++) {
		format(&fields[i], &a, fa, 256);
		format(&fields[i], &b, fb, 256);
		if (wcscmp(fa, fb)) {
			to_utf8(fa, ua, sizeof(ua));
			to_utf8(fb, ub, sizeof(ub));
			glog("cfg: %ls wrote %s read %s", fields[i].key, ua, ub);
			fail++;
		}
	}
	a.bat_devices = 0;
	cfg_write(p, &a);
	WritePrivateProfileStringW(L"watch", L"size", L"999", p);
	WritePrivateProfileStringW(L"watch", L"accent", L"nonsense", p);
	WritePrivateProfileStringW(L"watch", L"opacity", L"3", p);
	WritePrivateProfileStringW(L"watch", L"angle", L"500", p);
	WritePrivateProfileStringW(L"battery", L"tiers", L"50, 3,50,abc,20,96", p);
	WritePrivateProfileStringW(L"notifications", L"distance", L"0.4", p);
	cfg_defaults(&b);
	cfg_read(p, &b);
	if (b.watch_size != 130 || b.watch_opacity != 10 || b.watch_angle != 180 || b.accent != DEFAULT_ACCENT || b.bat_ntiers != 4 || b.bat_tiers[0] != 95 || b.bat_tiers[1] != 50 || b.bat_tiers[2] != 20 || b.bat_tiers[3] != 5 || fabsf(b.notif_dist - 0.4f) > 1e-4f || b.bat_devices) {
		glog("cfg: clamp/parse check failed (size %d, opacity %d, angle %d, accent %06X, tiers %d: %d %d %d %d, distance %.3f, devices %u)", b.watch_size, b.watch_opacity, b.watch_angle, b.accent, b.bat_ntiers, b.bat_tiers[0], b.bat_tiers[1], b.bat_tiers[2], b.bat_tiers[3], b.notif_dist, b.bat_devices);
		fail++;
	}
	if (signbit(cfg_snap(CR_NOTIF_HOFF, 0.5f)) || cfg_snap(CR_NOTIF_HOFF, 0.5f) != 0 || cfg_snap(CR_NOTIF_DIST, 1) != 1.2f || cfg_snap(CR_WATCH_ANGLE, 1) != 180) {
		glog("cfg: slider snap gives %g at the middle of horizontal offset", cfg_snap(CR_NOTIF_HOFF, 0.5f));
		fail++;
	}
	DeleteFileW(p);
	return fail;
}
#endif
