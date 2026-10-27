#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "audio.h"
#include "battery.h"
#include "cfg.h"
#include "gfx.h"
#include "lang.h"
#include "main.h"
#include "notif.h"
#include "vr.h"

#ifdef GL_TESTS
#include "samples.h"
#endif

#define POP_S 0.5f
#define OUT_S 0.3f
#define DISCORD_CAP 8
#define NOTIF_CAP 32
#define TW 1024
#define TH 256

static struct ovl ov;
static struct gx_tex *tex;
static struct gx_bmp *bmp;
static SRWLOCK lk = SRWLOCK_INIT;
static struct notif q[NOTIF_CAP], cur;
static struct notif_hist hist[HIST_CAP];
static int qh, qn, step, lines, hh, hn;
static long drops;
static bool active, placed, ready, on = true, dnd, viewing;
static double start, drop_t;
static uint32_t lay_gen, gfx_seen, hver, ids, sess;
static float cw, ch, dur;
static struct v3 pos;
static struct quat rot;

#ifdef GL_TESTS
bool (*notif_tap)(const struct notif *n);
#endif

static void copy(struct notif *d, const struct notif *s) {
	memcpy(d, s, offsetof(struct notif, img));
	if (s->icon == NI_IMAGE) {
		memcpy(d->img, s->img, sizeof(d->img));
	}
}

static struct notif *slot(int i) {
	return &q[(qh + i) % NOTIF_CAP];
}

static bool merge(const struct notif *n) {
	struct notif *o;
	int i;
	for (i = 0; i < qn; i++) {
		o = slot(i);
		if (o->origin != NO_DISCORD || wcscmp(o->title, n->title) || wcscmp(o->source, n->source)) continue;
		put(o->body, ARRAYSIZE(o->body), n->body);
		o->timeout = n->timeout;
		o->count = (o->count > 1 ? o->count : 1) + 1;
		if (n->icon == NI_IMAGE) {
			o->icon = NI_IMAGE;
			memcpy(o->img, n->img, sizeof(o->img));
		}
		return true;
	}
	return false;
}

static bool evict(void) {
	int k = -1, c = 0, i;
	for (i = 0; i < qn; i++) {
		if (slot(i)->origin != NO_DISCORD) continue;
		k = k < 0 ? i : k;
		c++;
	}
	if (c < DISCORD_CAP) return false;
	for (i = k; i < qn - 1; i++) {
		copy(slot(i), slot(i + 1));
	}
	qn--;
	return true;
}

static bool loud(const struct notif *n) {
	return n->origin == NO_TEST || (n->icon == NI_BATTERY && n->kind == NK_CRITICAL);
}

static struct notif_hist *entry(int i) {
	return &hist[(hh + i) % HIST_CAP];
}

void notif_shrink(uint8_t *d, const uint8_t *s) {
	int x, y, c;
	const uint8_t *p;
	for (y = 0; y < HIST_PX; y++) {
		for (x = 0; x < HIST_PX; x++) {
			p = s + (2 * y * ICON_PX + 2 * x) * 4;
			for (c = 0; c < 4; c++) {
				*d++ = (uint8_t)((p[c] + p[c + 4] + p[c + ICON_PX * 4] + p[c + ICON_PX * 4 + 4] + 2) / 4);
			}
		}
	}
}

static void fill(struct notif_hist *h, const struct notif *n, const SYSTEMTIME *at) {
	WCHAR *p;
	h->at = *at;
	put(h->body, ARRAYSIZE(h->body), n->body);
	for (p = h->body; *p; p++) {
		if (*p < L' ') {
			*p = L' ';
		}
	}
	if (n->icon == NI_IMAGE) {
		h->icon = NI_IMAGE;
		notif_shrink(h->img, n->img);
	}
	hver++;
}

static void record(const struct notif *n, const SYSTEMTIME *at, bool missed) {
	struct notif_hist *h;
	hh = (hh + HIST_CAP - 1) % HIST_CAP;
	hn += hn < HIST_CAP;
	h = &hist[hh];
	*h = (struct notif_hist){ .id = ++ids, .missed = missed, .kind = n->kind, .icon = n->icon, .origin = n->origin, .count = n->count, .bat_dev = n->bat_dev, .bat_role = n->bat_role, .bat_pct = n->bat_pct };
	put(h->title, ARRAYSIZE(h->title), n->title);
	put(h->source, ARRAYSIZE(h->source), n->source);
	fill(h, n, at);
}

static void miss(const struct notif *n, const SYSTEMTIME *at) {
	static struct notif_hist t;
	struct notif_hist *h;
	int i, k;
	for (i = 0; n->origin == NO_DISCORD && !viewing && i < hn; i++) {
		h = entry(i);
		if (!h->missed || h->seen || h->id < sess || h->origin != NO_DISCORD || wcscmp(h->title, n->title) || wcscmp(h->source, n->source)) continue;
		t = *h;
		for (k = i; k > 0; k--) {
			*entry(k) = *entry(k - 1);
		}
		t.count = (t.count > 1 ? t.count : 1) + (n->count > 1 ? n->count : 1);
		fill(&t, n, at);
		*entry(0) = t;
		return;
	}
	record(n, at, true);
}

bool notif_push(const struct notif *n) {
	bool bat = n->icon == NI_BATTERY, drop, ok, cut;
	int lead = 0, i;
	long d = 0;
	double t = now_s();
	SYSTEMTIME st;
#ifdef GL_TESTS
	if (notif_tap) return notif_tap(n);
#endif
	GetLocalTime(&st);
	AcquireSRWLockExclusive(&lk);
	if (!on) {
		ReleaseSRWLockExclusive(&lk);
		return false;
	}
	if (dnd && !loud(n)) {
		miss(n, &st);
		ReleaseSRWLockExclusive(&lk);
		app_wake();
		return true;
	}
	if (n->origin == NO_DISCORD && merge(n)) {
		ReleaseSRWLockExclusive(&lk);
		return true;
	}
	cut = n->origin == NO_DISCORD && evict();
	while (bat && lead < qn && q[(qh + lead) % NOTIF_CAP].icon == NI_BATTERY) {
		lead++;
	}
	drop = bat && qn == NOTIF_CAP && lead < qn;
	ok = qn < NOTIF_CAP || drop;
	qn -= drop;
	if (ok && bat) {
		qh = (qh + NOTIF_CAP - 1) % NOTIF_CAP;
		for (i = 0; i < lead; i++) {
			copy(&q[(qh + i) % NOTIF_CAP], &q[(qh + i + 1) % NOTIF_CAP]);
		}
		copy(&q[(qh + lead) % NOTIF_CAP], n);
		qn++;
	} else if (ok) {
		copy(&q[(qh + qn++) % NOTIF_CAP], n);
	}
	if (!ok || drop || cut) {
		drops++;
		if (t - drop_t >= 5) {
			d = drops;
			drops = 0;
			drop_t = t;
		}
	}
	ReleaseSRWLockExclusive(&lk);
	if (ok) {
		app_wake();
	}
	if (d) {
		glog("notification queue full, dropped %ld (Discord keeps its newest %d)", d, DISCORD_CAP);
	}
	return ok;
}

bool notif_has_room(void) {
	bool r;
	AcquireSRWLockShared(&lk);
	r = qn < NOTIF_CAP;
	ReleaseSRWLockShared(&lk);
	return r;
}

static bool pop(void) {
	bool ok;
	AcquireSRWLockExclusive(&lk);
	ok = qn > 0;
	if (ok) {
		copy(&cur, &q[qh]);
		qh = --qn ? (qh + 1) % NOTIF_CAP : 0;
	}
	ReleaseSRWLockExclusive(&lk);
	return ok;
}

static void remember(const struct notif *n, const SYSTEMTIME *at) {
	AcquireSRWLockExclusive(&lk);
	record(n, at, false);
	ReleaseSRWLockExclusive(&lk);
}

static void hist_clear(void) {
	AcquireSRWLockExclusive(&lk);
	memset(hist, 0, sizeof(hist));
	hh = hn = 0;
	hver++;
	ReleaseSRWLockExclusive(&lk);
}

int notif_hist_read(int n, struct notif_hist *out) {
	int c, i;
	AcquireSRWLockShared(&lk);
	c = hn;
	for (i = 0; i < n && i < c; i++) {
		out[i] = *entry(i);
	}
	ReleaseSRWLockShared(&lk);
	return c;
}

uint32_t notif_hist_version(void) {
	uint32_t v;
	AcquireSRWLockShared(&lk);
	v = hver;
	ReleaseSRWLockShared(&lk);
	return v;
}

void notif_hist_open(bool open) {
	AcquireSRWLockExclusive(&lk);
	if (open != viewing) {
		viewing = open;
		sess = ids + 1;
	}
	ReleaseSRWLockExclusive(&lk);
}

bool notif_hist_seen(uint32_t id) {
	struct notif_hist *h;
	bool changed = false;
	int i;
	AcquireSRWLockExclusive(&lk);
	for (i = 0; i < hn; i++) {
		h = entry(i);
		if (h->id != id || !h->missed || h->seen) continue;
		h->seen = changed = true;
		hver++;
	}
	ReleaseSRWLockExclusive(&lk);
	return changed;
}

static int notif_missed(void) {
	int n = 0, i;
	AcquireSRWLockShared(&lk);
	for (i = 0; i < hn; i++) {
		n += entry(i)->missed && !entry(i)->seen;
	}
	ReleaseSRWLockShared(&lk);
	return n;
}

int notif_badge(WCHAR *b, int cap) {
	int n = notif_missed();
	b[0] = 0;
	if (n > 0) {
		swprintf(b, cap, L"%d%ls", n > 9 ? 9 : n, n > 9 ? L"+" : L"");
	}
	return n;
}

bool notif_dnd(void) {
	return dnd;
}

void notif_dnd_set(bool v) {
	SYSTEMTIME st;
	int k = 0, i;
	v = v && on;
	if (v == dnd) return;
	GetLocalTime(&st);
	AcquireSRWLockExclusive(&lk);
	dnd = v;
	sess = ids + 1;
	for (i = 0; v && i < qn; i++) {
		if (!loud(slot(i))) {
			miss(slot(i), &st);
			continue;
		}
		if (k != i) {
			copy(slot(k), slot(i));
		}
		k++;
	}
	qn = v ? k : qn;
	hver++;
	ReleaseSRWLockExclusive(&lk);
	glog("do not disturb %s", v ? "on" : "off");
}

void notif_config(bool v) {
	if (v == on) return;
	notif_dnd_set(false);
	AcquireSRWLockExclusive(&lk);
	on = v;
	if (!v) {
		qh = qn = 0;
	}
	hver++;
	ReleaseSRWLockExclusive(&lk);
	glog("notifications %s", v ? "on" : "off");
}

void notif_bat_text(int dev, int role, int pct, WCHAR *name, WCHAR *value) {
	if (dev >= BD_TRACKER && bat_role_str(role) > 0) {
		put(name, 128, tr(bat_role_str(role)));
	} else if (dev >= BD_TRACKER) {
		swprintf(name, 128, tr(S_N_DEV_TRACKER), dev - BD_TRACKER + 1);
	} else {
		put(name, 128, tr(dev == BD_HMD ? S_W_HEADSET : dev == BD_LEFT ? S_N_DEV_LEFT : S_N_DEV_RIGHT));
	}
	value[0] = 0;
	if (pct >= 0) {
		wcscat(name, L" \u2013");
		value[0] = L' ';
		fmt_num(value + 1, 15, pct, 0, L"%");
	}
}

static const WCHAR *suffix(const struct notif *n, WCHAR *b) {
	WCHAR c[32];
	b[0] = 0;
	if (n->count > 1) {
		swprintf(c, ARRAYSIZE(c), tr(S_N_MESSAGES), n->count);
		swprintf(b, 40, L" \u00b7 %ls", c);
	}
	return b;
}

static uint32_t initial(const WCHAR *title, WCHAR *letter) {
	uint32_t h = 2166136261u;
	const WCHAR *p, *f = NULL;
	for (p = title; *p; p++) {
		h = (h ^ *p) * 16777619u;
		if (!f && IsCharAlphaNumericW(*p)) {
			f = p;
		}
	}
	for (p = title; !f && *p; p++) {
		if (*p != L' ') {
			f = p;
		}
	}
	letter[0] = f ? *f : L'?';
	letter[1] = f && IS_HIGH_SURROGATE(f[0]) && IS_LOW_SURROGATE(f[1]) ? f[1] : 0;
	letter[2] = 0;
	CharUpperBuffW(letter, 1);
	return col_hsv((float)(h % 360), 0.5f, 0.56f);
}

static void layout(const struct notif *n) {
	WCHAR dev[136], pct[16], sfx[40];
	float a, t, s, b;
	int ft, fb;
	if (n->icon == NI_BATTERY) {
		notif_bat_text(n->bat_dev, n->bat_role, n->bat_pct, dev, pct);
		a = gx_measure(gx_font(GF_DISPLAY, 26, 600), tr(S_N_LOW_BATTERY), 0, 0, NULL);
		b = gx_measure(gx_font(GF_TEXT, 22, 500), dev, 0, GT_TNUM, NULL) + gx_measure(gx_font(GF_TEXT, 22, 600), pct, 0, GT_TNUM, NULL);
		cw = ceilf(fminf(fmaxf(fmaxf(a, b) + 52, 560), 800));
		ch = 124;
		return;
	}
	ft = gx_font(GF_DISPLAY, 22, 600);
	fb = gx_font(GF_TEXT, 20, 500);
	t = gx_measure(ft, n->title, 0, 0, NULL);
	s = n->source[0] ? gx_measure(fb, n->source, 0, 0, NULL) + gx_measure(fb, suffix(n, sfx), 0, 0, NULL) + 12 : 0;
	b = gx_measure(fb, n->body, 0, 0, NULL);
	cw = ceilf(fminf(fmaxf(fmaxf(t + s, b) + 108, 560), 800));
	lines = 0;
	if (n->body[0]) {
		gx_measure(fb, n->body, cw - 108, GT_WRAP | GT_LINES(3), &lines);
		lines = clampi(lines, 1, 3);
	}
	ch = 24 + fmaxf(60, (n->title[0] || n->source[0] ? 28 : 0) + 26.0f * lines);
}

static void ring(float cx, float cy, float r, float sw, float f, struct gx_color c) {
	if (f > 0.999f) {
		gx_stroke_circle(cx, cy, r, sw, c);
	} else if (f > 0.002f) {
		gx_arc(cx, cy, r, sw, 0, 360 * f, c);
	}
}

static void draw_alert(const struct notif *n, float x, float y, float f, bool crit) {
	WCHAR dev[136], pct[16];
	float cx = x + cw / 2, cy = y + 1, w2, w1, bx, d;
	int f1, f2;
	gx_fill_circle(cx, cy, 39, col_rgb(crit ? 0x241417 : 0x121418));
	if (crit) {
		gx_stroke_circle(cx, cy, 39, 2, col_rgba(C_CRIT, 0.55f));
	}
	gx_stroke_circle(cx, cy, 32, 5, col_rgb(crit ? C_RING_CRIT : C_RING));
	ring(cx, cy, 32, 5, f, col_rgb(crit ? C_CRIT : C_WARN));
	notif_medallion(NI_BATTERY, crit ? NK_CRITICAL : NK_NORMAL, n->title, NULL, cx, cy, 25);
	notif_bat_text(n->bat_dev, n->bat_role, n->bat_pct, dev, pct);
	gx_text(gx_font(GF_DISPLAY, 26, 600), tr(S_N_LOW_BATTERY), x + 26, y + 48, cw - 52, 32, col_rgb(C_TEXT), GT_CENTER | GT_ELLIPSIS);
	f1 = gx_font(GF_TEXT, 22, 500);
	f2 = gx_font(GF_TEXT, 22, 600);
	w2 = gx_measure(f2, pct, 0, GT_TNUM, NULL);
	w1 = fminf(gx_measure(f1, dev, 0, GT_TNUM, NULL), cw - 52 - w2);
	bx = floorf(cx - (w1 + w2) / 2);
	d = gx_text(f1, dev, bx, y + 80, w1 + 1, 28, col_rgb(C_DEVICE), GT_TNUM | GT_ELLIPSIS);
	gx_text(f2, pct, bx + d, y + 80, 0, 28, col_rgb(crit ? C_CRIT_T : C_WARN_T), GT_TNUM);
}

void notif_medallion(int icon, int kind, const WCHAR *title, struct gx_bmp *b, float cx, float cy, float r) {
	static const uint32_t bg[3] = { 0x4B5262, C_WARN, C_CRIT }, fg[3] = { 0xFFFFFF, 0x2A160B, 0x2A0B0F };
	static const enum gx_icon ic[3] = { IC_GHOST, IC_WARNING, IC_ALERT };
	WCHAR ini[3];
	float s, k, ix, iy;
	int i;
	if (icon == NI_IMAGE) {
		gx_bmp_draw(b, cx - r, cy - r, 2 * r, 2 * r, r);
	} else if (icon == NI_INITIAL) {
		gx_fill_circle(cx, cy, r, col_rgb(initial(title, ini)));
		gx_text(gx_font(GF_DISPLAY, r + 1, 600), ini, cx - r, cy - r, 2 * r, 2 * r, col_rgb(0xFFFFFF), GT_CENTER);
	} else if (icon == NI_BATTERY) {
		i = kind == NK_CRITICAL ? 2 : 1;
		s = r + 5;
		k = s / 24;
		ix = cx - s / 2;
		iy = cy - s / 2;
		gx_fill_circle(cx, cy, r, col_rgb(bg[i]));
		gx_icon(IC_BATTERY, ix, iy, s, 2.2f, col_rgb(fg[i]));
		gx_line(ix + 6.2f * k, iy + 10.6f * k, ix + 6.2f * k, iy + 13.4f * k, 2.8f * k, col_rgb(fg[i]));
	} else {
		i = icon > NI_DEFAULT && icon <= NI_ERROR ? icon : NI_DEFAULT;
		s = r + 3;
		gx_fill_circle(cx, cy, r, col_rgb(bg[i]));
		gx_icon(ic[i], cx - s / 2, cy - s / 2, s, 2, col_rgb(fg[i]));
	}
}

static void draw_card(const struct notif *n, struct gx_bmp *b, float x, float y, float f) {
	float ay = y + (ch - 60) / 2, cx = x + 42, cy = ay + 30, tx = x + 86, tw = cw - 108, ty, sw, xw;
	bool row = n->title[0] || n->source[0];
	WCHAR sfx[40];
	int fb, ft;
	gx_stroke_circle(cx, cy, 28, 4, col_rgb(C_RING));
	ring(cx, cy, 28, 4, f, col_accent());
	notif_medallion(n->icon, n->kind, n->title, b, cx, cy, 23);
	ty = y + (ch - (row ? 28 : 0) - 26.0f * lines) / 2;
	fb = gx_font(GF_TEXT, 20, 500);
	ft = gx_font(GF_DISPLAY, 22, 600);
	if (row) {
		xw = gx_measure(fb, suffix(n, sfx), 0, 0, NULL);
		sw = n->source[0] ? fminf(gx_measure(fb, n->source, 0, 0, NULL) + xw, fmaxf(tw * 0.4f, tw - gx_measure(ft, n->title, 0, 0, NULL) - 12)) : 0;
		if (sw > xw) {
			gx_text(fb, n->source, tx + tw - sw - 1, ty, sw - xw + 1, 28, col_rgb(C_DIM), GT_RIGHT | GT_ELLIPSIS | GT_LINES(1));
		}
		if (sw > 0 && xw > 0) {
			gx_text(fb, sfx, tx + tw - xw, ty, 0, 28, col_rgb(C_DIM), 0);
		}
		gx_text(ft, n->title, tx, ty, tw - (sw > 0 ? sw + 12 : 0), 28, col_rgb(C_TEXT), GT_ELLIPSIS | GT_LINES(1));
		ty += 28;
	}
	if (lines) {
		gx_text(fb, n->body, tx, ty, tw, 26, col_rgb(C_ICON), GT_WRAP | GT_LINES(3) | GT_ELLIPSIS);
	}
}

static bool draw(struct gx_tex *t, const struct notif *n, struct gx_bmp *b, float f) {
	static const struct gx_stop shade[4] = { { 0, { 0, 0, 0, 0.12f } }, { 0.16f, { 0, 0, 0, 0 } }, { 0.84f, { 0, 0, 0, 0 } }, { 1, { 0, 0, 0, 0.12f } } };
	bool al = n->icon == NI_BATTERY, crit = al && n->kind == NK_CRITICAL;
	float x = floorf((TW - cw) / 2), y = floorf((TH - ch) / 2) + (al ? 20 : 0), bw = crit ? 2 : 0, p = 0, r, m, a;
	int i;
	if (!t || !gx_begin(t)) return false;
	for (i = 0; i < 12; i++) {
		r = 1 - i / 12.0f;
		m = r - 1 / 24.0f;
		a = m > 0.55f ? 0.2f * (1 - m) / 0.45f : 0.5f - 0.3f * m / 0.55f;
		gx_fill_ellipse(x + cw / 2, y + ch + 2, (cw - 80) / 2 * r, 18 * r, col_rgba(0, 1 - (1 - a) / (1 - p)));
		p = a;
	}
	gx_shadow_rrect(x, y, cw, ch, 24, 18, 6, col_rgba(0, 0.35f));
	gx_fill_rrect(x, y, cw, ch, 24, crit ? col_rgba(0x241417, 0.96f) : col_rgba(0x121418, 0.95f));
	gx_grad_rrect(x, y, cw, ch, 24, x, y, x + cw, y, shade, 4);
	gx_clip_push(x, y + bw, cw, 12);
	gx_stroke_rrect(x + bw, y + bw, cw - 2 * bw, ch - 2 * bw, 24 - bw, 2, col_rgba(0xFFFFFF, crit ? 0.07f : 0.08f));
	gx_clip_pop();
	if (crit) {
		gx_stroke_rrect(x, y, cw, ch, 24, 2, col_rgba(C_CRIT, 0.55f));
	}
	if (al) {
		draw_alert(n, x, y, f, crit);
	} else {
		draw_card(n, b, x, y, f);
	}
	return gx_end();
}

static float pop_ease(float x) {
	float lo = 0, hi = 1, u = x;
	int i;
	for (i = 0; i < 16; i++) {
		u = (lo + hi) / 2;
		if (3 * u * (1 - u) * (1 - u) * 0.34f + 3 * u * u * (1 - u) * 0.64f + u * u * u < x) {
			lo = u;
		} else {
			hi = u;
		}
	}
	return 3 * u * (1 - u) * (1 - u) * 1.4f + 3 * u * u * (1 - u) + u * u * u;
}

static void begin(double now) {
	float lo = (float)g_cfg.dur_min, hi = (float)g_cfg.dur_max;
	start = now;
	dur = fminf(fmaxf(cur.timeout, lo), hi);
	cur.opacity = cur.opacity > 0 ? fminf(cur.opacity, 1) : 1;
	lay_gen = g_cfg_gen - 1;
	gfx_seen = g_gfx_gen - 1;
	ready = false;
	if (cur.sfx != SFX_NONE && (g_cfg.notif_sound || cur.icon == NI_BATTERY)) {
		audio_play(cur.sfx, cur.volume * g_cfg.notif_volume / 100.0f);
	}
}

static void follow(float dt) {
	const HmdMatrix34_t *hmd = vr_pose(0);
	struct v3 eye, tgt;
	HmdMatrix34_t look;
	struct quat r;
	if (!hmd) return;
	eye = m34_pos(hmd);
	tgt = m34_point(hmd, v3(g_cfg.notif_hoff, -g_cfg.notif_dist * tanf(g_cfg.notif_angle * DEG), -g_cfg.notif_dist));
	pos = placed ? v3_lerp(pos, tgt, fminf(1, 5 * dt)) : tgt;
	look = m34_look(pos, eye, m34_col(hmd, 1));
	r = q_from_m34(&look);
	rot = placed ? q_slerp(rot, r, fminf(1, 10 * dt)) : r;
	placed = true;
}

bool notif_update(double dt) {
	double now = now_s();
	float t, s, a, f, e, steps;
	int k;
	HmdMatrix34_t m;
	SYSTEMTIME st;
	if (active && !on && !ready) {
		active = false;
	} else if (active && !on && now - start < dur) {
		start = now - dur;
	}
	if (active && now - start >= dur + OUT_S) {
		if (!ready) {
			GetLocalTime(&st);
			AcquireSRWLockExclusive(&lk);
			record(&cur, &st, true);
			ReleaseSRWLockExclusive(&lk);
		}
		active = false;
	}
	if (!active) {
		active = pop();
		if (!active) {
			ovl_show(&ov, false);
			placed = false;
			return false;
		}
		begin(now);
	}
	t = (float)(now - start);
	if (t < dur) {
		s = 0.5f + 0.5f * pop_ease(fminf(1, t / POP_S));
		a = fminf(1, t / 0.2f);
		f = 1 - t / dur;
	} else {
		e = fminf(1, (t - dur) / OUT_S);
		s = 1 - 0.15f * e;
		a = 1 - e;
		f = 0;
	}
	if (!ov.h) return false;
	follow((float)dt);
	if (!placed) return true;
	if (lay_gen != g_cfg_gen || gfx_seen != g_gfx_gen) {
		if (gfx_seen != g_gfx_gen && cur.icon == NI_IMAGE) {
			gx_bmp_set(bmp, cur.img, ICON_PX * 4);
		}
		lay_gen = g_cfg_gen;
		gfx_seen = g_gfx_gen;
		layout(&cur);
		step = -1;
	}
	steps = fminf(90, dur * 45);
	k = (int)ceilf(f * steps);
	if (k != step && draw(tex, &cur, bmp, k / steps)) {
		ovl_texture(&ov, tex);
		step = k;
		if (!ready) {
			GetLocalTime(&st);
			remember(&cur, &st);
		}
		ready = true;
	}
	m = m34_quat(rot, pos);
	m = m34_scale(&m, s);
	ovl_abs(&ov, &m);
	ovl_width(&ov, 0.45f * g_cfg.notif_size / 100);
	ovl_alpha(&ov, a * cur.opacity);
	ovl_show(&ov, ready);
	return true;
}

void notif_init(void) {
	if (ovl_create(&ov, GL_APP_KEY ".notif", "Ghostlay notifications")) {
		ovl_sort(&ov, 100);
	}
	tex = gx_tex_new(TW, TH);
	bmp = gx_bmp_new(ICON_PX, ICON_PX);
	active = placed = false;
}

void notif_shutdown(void) {
	ovl_destroy(&ov);
	gx_tex_free(tex);
	gx_bmp_free(bmp);
	tex = NULL;
	bmp = NULL;
	active = placed = false;
	AcquireSRWLockExclusive(&lk);
	qh = qn = 0;
	dnd = viewing = false;
	ReleaseSRWLockExclusive(&lk);
	hist_clear();
}

void notif_blank(struct notif *n, int origin) {
	memset(n, 0, offsetof(struct notif, img));
	n->kind = NK_NORMAL;
	n->icon = NI_DEFAULT;
	n->sfx = SFX_NOTIFY;
	n->volume = 1;
	n->opacity = 1;
	n->bat_dev = n->bat_pct = -1;
	n->origin = origin;
}

static void test_card(struct notif *n) {
	notif_blank(n, NO_UDP);
	n->timeout = 5;
	put(n->title, ARRAYSIZE(n->title), tr(S_N_TEST_TITLE));
	put(n->body, ARRAYSIZE(n->body), tr(S_N_TEST_BODY));
	put(n->source, ARRAYSIZE(n->source), tr(S_APP_NAME));
}

void notif_test(void) {
	static struct notif n;
	test_card(&n);
	n.origin = NO_TEST;
	notif_push(&n);
}

#ifdef GL_TESTS
static void dc_card(struct notif *n, int origin, int icon, const WCHAR *title, const WCHAR *body) {
	test_card(n);
	n->origin = origin;
	n->count = 1;
	n->icon = icon;
	put(n->title, ARRAYSIZE(n->title), title);
	put(n->body, ARRAYSIZE(n->body), body);
	put(n->source, ARRAYSIZE(n->source), L"Discord");
}

static int burst_test(void) {
	static struct notif n;
	WCHAR t[8];
	int fail = 0, i;
	qh = qn = 0;
	dc_card(&n, NO_DISCORD, NI_INITIAL, L"Mihoko-Okayami", L"1");
	fail += !notif_push(&n);
	n.origin = NO_UDP;
	fail += !notif_push(&n);
	dc_card(&n, NO_DISCORD, NI_INITIAL, L"Mihoko-Okayami", L"2");
	fail += !notif_push(&n);
	dc_card(&n, NO_DISCORD, NI_IMAGE, L"Mihoko-Okayami", L"3");
	n.img[3] = 77;
	fail += !notif_push(&n);
	dc_card(&n, NO_DISCORD, NI_INITIAL, L"Mihoko-Okayami", L"x");
	put(n.source, ARRAYSIZE(n.source), L"Discord \u00b7 #general, Test Server");
	fail += !notif_push(&n);
	dc_card(&n, NO_UDP, NI_BATTERY, L"", L"");
	fail += !notif_push(&n) || qn != 4;
	fail += !pop() || cur.icon != NI_BATTERY;
	fail += !pop() || cur.origin != NO_DISCORD || cur.count != 3 || wcscmp(cur.body, L"3") || cur.icon != NI_IMAGE || cur.img[3] != 77;
	fail += !pop() || cur.origin != NO_UDP || cur.count > 1 || wcscmp(cur.body, L"1");
	fail += !pop() || cur.count > 1 || wcscmp(cur.body, L"x");
	fail += pop();
	dc_card(&n, NO_UDP, NI_DEFAULT, L"udp", L"");
	fail += !notif_push(&n);
	for (i = 0; i < 10; i++) {
		swprintf(t, ARRAYSIZE(t), L"d%d", i);
		dc_card(&n, NO_DISCORD, NI_INITIAL, t, L"");
		fail += !notif_push(&n);
	}
	dc_card(&n, NO_UDP, NI_BATTERY, L"", L"");
	fail += !notif_push(&n) || qn != 10;
	fail += !pop() || cur.icon != NI_BATTERY;
	fail += !pop() || wcscmp(cur.title, L"udp");
	for (i = 2; i < 10; i++) {
		swprintf(t, ARRAYSIZE(t), L"d%d", i);
		fail += !pop() || wcscmp(cur.title, t);
	}
	fail += pop();
	if (fail) {
		glog("notif: Discord burst merge / cap selftest failed (%d)", fail);
	}
	return fail;
}

static int initial_test(void) {
	static const struct { const WCHAR *title, *want; } t[] = { { L"mihoko", L"M" }, { L"@élodie", L"É" }, { L"  \U0001F600 bob", L"B" }, { L"\U0001F600", L"\U0001F600" }, { L"  ", L"?" }, { L"", L"?" }, { L"7even", L"7" } };
	WCHAR a[3], b[3];
	uint32_t c, d;
	int fail = 0, i, h, same = 0;
	for (i = 0; i < (int)ARRAYSIZE(t); i++) {
		initial(t[i].title, a);
		fail += wcscmp(a, t[i].want) != 0;
	}
	c = initial(L"Mihoko-Okayami", a);
	d = initial(L"Mihoko-Okayami", b);
	fail += c != d;
	for (h = 0; h < 26; h++) {
		b[0] = (WCHAR)(L'A' + h);
		b[1] = 0;
		d = initial(b, a);
		same += d == c;
		fail += 1.05f / (col_luma(d) + 0.05f) < 3;
	}
	fail += same > 4;
	if (fail) {
		glog("notif: initial letter / colour selftest failed (%d)", fail);
	}
	return fail;
}

static int hist_test(void) {
	static struct notif n;
	static struct notif_hist h[HIST_CAP + 1];
	SYSTEMTIME st = { 2026, 10, 5, 23, 21, 0, 0, 0 };
	const uint8_t *z = (const uint8_t *)hist;
	int fail = 0, i, k, x, y, c;
	uint32_t v;
	hist_clear();
	v = notif_hist_version();
	test_card(&n);
	for (i = 0; i < HIST_CAP + 5; i++) {
		swprintf(n.title, ARRAYSIZE(n.title), L"%d", i);
		st.wMinute = (WORD)i;
		remember(&n, &st);
	}
	fail += notif_hist_version() != v + HIST_CAP + 5;
	fail += notif_hist_read(HIST_CAP + 1, h) != HIST_CAP;
	for (i = 0; i < HIST_CAP; i++) {
		fail += _wtoi(h[i].title) != HIST_CAP + 4 - i || h[i].at.wMinute != HIST_CAP + 4 - i || wcscmp(h[i].source, tr(S_APP_NAME));
	}
	qh = qn = 0;
	for (i = 0; i < ICON_PX * ICON_PX * 4; i++) {
		n.img[i] = (uint8_t)(i % (ICON_PX * 4) / 4 * 3 + i / (ICON_PX * 4) * 5 + i % 4 * 40);
	}
	for (i = 0; i < 3; i++) {
		dc_card(&n, NO_DISCORD, i ? NI_IMAGE : NI_INITIAL, L"Mihoko-Okayami", i < 2 ? L"a" : L"b\nc\td");
		fail += !notif_push(&n);
	}
	fail += !pop();
	remember(&cur, &st);
	fail += notif_hist_read(2, h) != HIST_CAP || h[0].count != 3 || h[0].origin != NO_DISCORD || h[0].icon != NI_IMAGE || wcscmp(h[0].title, L"Mihoko-Okayami") || wcscmp(h[0].body, L"b c d") || _wtoi(h[1].title) != HIST_CAP + 4;
	for (k = 0; k < HIST_PX * HIST_PX * 4; k++) {
		x = k / 4 % HIST_PX * 2;
		y = k / 4 / HIST_PX * 2;
		c = k % 4;
		fail += h[0].img[k] != (n.img[(y * ICON_PX + x) * 4 + c] + n.img[(y * ICON_PX + x + 1) * 4 + c] + n.img[((y + 1) * ICON_PX + x) * 4 + c] + n.img[((y + 1) * ICON_PX + x + 1) * 4 + c] + 2) / 4;
	}
	test_card(&n);
	remember(&n, &st);
	fail += notif_hist_read(1, h) != HIST_CAP || h[0].img[5] != 0 || h[0].count != 0;
	hist_clear();
	fail += notif_hist_read(1, h) != 0;
	for (i = 0; i < (int)sizeof(hist); i++) {
		fail += z[i] != 0;
	}
	if (fail) {
		glog("notif: history selftest failed (%d)", fail);
	}
	return fail;
}

static void bat_card(struct notif *n, int kind, int pct) {
	dc_card(n, NO_UDP, NI_BATTERY, L"", L"");
	n->source[0] = 0;
	n->kind = kind;
	n->bat_dev = BD_RIGHT;
	n->bat_pct = pct;
}

static int dnd_test(void) {
	static struct notif n;
	static struct notif_hist h[HIST_CAP + 1];
	SYSTEMTIME st = { 2026, 10, 5, 23, 21, 47, 0, 0 };
	WCHAR b[8];
	uint32_t v, id;
	int fail = 0, i, c;
	hist_clear();
	qh = qn = 0;
	notif_config(true);
	notif_dnd_set(false);
	notif_hist_open(false);
	dc_card(&n, NO_UDP, NI_DEFAULT, L"vrcx", L"joined");
	fail += !notif_push(&n) || qn != 1 || notif_hist_read(0, NULL) || notif_missed();
	v = notif_hist_version();
	notif_dnd_set(true);
	fail += !notif_dnd() || notif_hist_version() == v || qn || notif_hist_read(1, h) != 1 || !h[0].missed || h[0].seen || !h[0].id || wcscmp(h[0].title, L"vrcx") || notif_missed() != 1;
	dc_card(&n, NO_DISCORD, NI_INITIAL, L"Mihoko-Okayami", L"a");
	fail += !notif_push(&n);
	id = notif_hist_read(1, h) ? h[0].id : 0;
	dc_card(&n, NO_DISCORD, NI_INITIAL, L"Okayami", L"x");
	fail += !notif_push(&n);
	dc_card(&n, NO_DISCORD, NI_IMAGE, L"Mihoko-Okayami", L"b\nc");
	n.img[3] = 77;
	fail += !notif_push(&n);
	fail += qn || notif_hist_read(4, h) != 3 || h[0].id != id || !h[0].missed || h[0].count != 2 || wcscmp(h[0].body, L"b c") || h[0].icon != NI_IMAGE || h[0].img[3] != 19 || wcscmp(h[1].title, L"Okayami") || wcscmp(h[2].title, L"vrcx") || notif_missed() != 3;
	dc_card(&n, NO_DISCORD, NI_INITIAL, L"Mihoko-Okayami", L"d");
	put(n.source, ARRAYSIZE(n.source), L"Discord · #general, Test Server");
	fail += !notif_push(&n) || notif_hist_read(1, h) != 4 || h[0].id == id || h[0].count != 1;
	bat_card(&n, NK_NORMAL, 25);
	fail += !notif_push(&n) || qn || notif_hist_read(1, h) != 5 || !h[0].missed || h[0].icon != NI_BATTERY || h[0].bat_pct != 25;
	bat_card(&n, NK_CRITICAL, 12);
	fail += !notif_push(&n) || qn != 1 || notif_hist_read(0, NULL) != 5;
	notif_test();
	fail += qn != 2 || !pop() || cur.kind != NK_CRITICAL || !pop() || cur.origin != NO_TEST || notif_hist_read(0, NULL) != 5;
	remember(&cur, &st);
	fail += notif_hist_read(1, h) != 6 || h[0].missed || h[0].origin != NO_TEST || notif_missed() != 5;
	v = notif_hist_version();
	fail += !notif_hist_seen(id) || notif_missed() != 4 || notif_hist_version() == v;
	v = notif_hist_version();
	fail += notif_hist_seen(id) || notif_hist_seen(h[0].id) || notif_hist_seen(0) || notif_hist_seen(ids + 7) || notif_hist_version() != v;
	dc_card(&n, NO_DISCORD, NI_INITIAL, L"Mihoko-Okayami", L"e");
	fail += !notif_push(&n) || notif_hist_read(1, h) != 7 || h[0].id == id || h[0].count != 1 || notif_missed() != 5;
	id = h[0].id;
	notif_hist_open(true);
	fail += !notif_push(&n) || notif_hist_read(1, h) != 8 || h[0].id == id;
	fail += !notif_push(&n) || notif_hist_read(1, h) != 9;
	notif_hist_open(false);
	fail += !notif_push(&n) || notif_hist_read(1, h) != 10 || h[0].count != 1;
	id = h[0].id;
	fail += !notif_push(&n) || notif_hist_read(2, h) != 10 || h[0].id != id || h[0].count != 2 || h[1].count != 1;
	notif_dnd_set(false);
	notif_dnd_set(true);
	fail += !notif_push(&n) || notif_hist_read(1, h) != 11 || h[0].id == id;
	bat_card(&n, NK_CRITICAL, 12);
	fail += !notif_push(&n) || qn != 1;
	c = notif_missed();
	v = notif_hist_version();
	notif_dnd_set(false);
	fail += notif_dnd() || qn != 1 || !pop() || cur.icon != NI_BATTERY || notif_hist_read(0, NULL) != 11 || notif_missed() != c || notif_hist_version() == v;
	dc_card(&n, NO_UDP, NI_DEFAULT, L"udp", L"");
	fail += !notif_push(&n) || qn != 1 || notif_hist_read(0, NULL) != 11;
	notif_test();
	bat_card(&n, NK_CRITICAL, 12);
	fail += !notif_push(&n);
	dc_card(&n, NO_DISCORD, NI_INITIAL, L"Mihoko", L"gg");
	fail += !notif_push(&n) || qn != 4;
	notif_dnd_set(true);
	fail += qn != 2 || notif_hist_read(2, h) != 13 || wcscmp(h[0].title, L"Mihoko") || wcscmp(h[1].title, L"udp") || !h[0].missed || !h[1].missed;
	fail += !pop() || cur.icon != NI_BATTERY || !pop() || cur.origin != NO_TEST || pop();
	hist_clear();
	fail += notif_badge(b, ARRAYSIZE(b)) != 0 || b[0];
	dc_card(&n, NO_UDP, NI_DEFAULT, L"", L"");
	for (i = 0; i < HIST_CAP + 5; i++) {
		swprintf(n.title, ARRAYSIZE(n.title), L"%d", i);
		fail += !notif_push(&n);
		c = notif_badge(b, ARRAYSIZE(b));
		fail += c != (i < HIST_CAP ? i + 1 : HIST_CAP) || (c == 1 && wcscmp(b, L"1")) || (c == 9 && wcscmp(b, L"9")) || (c > 9 && wcscmp(b, L"9+"));
	}
	fail += qn || notif_hist_read(HIST_CAP + 1, h) != HIST_CAP || _wtoi(h[0].title) != HIST_CAP + 4 || _wtoi(h[HIST_CAP - 1].title) != 5;
	for (i = 0; i < HIST_CAP; i++) {
		fail += !notif_hist_seen(h[i].id);
	}
	fail += notif_badge(b, ARRAYSIZE(b)) != 0 || b[0];
	fail += !notif_push(&n) || notif_hist_read(1, h) != HIST_CAP || notif_missed() != 1;
	id = h[0].id;
	bat_card(&n, NK_CRITICAL, 12);
	fail += !notif_push(&n) || qn != 1;
	v = notif_hist_version();
	notif_config(false);
	fail += notif_dnd() || qn || notif_hist_version() == v;
	fail += notif_push(&n) || qn;
	notif_test();
	notif_dnd_set(true);
	fail += qn || notif_dnd() || notif_hist_read(1, h) != HIST_CAP || h[0].id != id || notif_missed() != 1;
	notif_config(true);
	fail += notif_dnd() || notif_hist_read(1, h) != HIST_CAP || h[0].id != id || notif_missed() != 1;
	fail += !notif_push(&n) || qn != 1 || !pop() || notif_hist_read(0, NULL) != HIST_CAP;
	hist_clear();
	qh = qn = 0;
	if (fail) {
		glog("notif: do not disturb / missed / master switch selftest failed (%d)", fail);
	}
	return fail;
}

static int lost_test(void) {
	static struct notif n;
	static struct notif_hist h[2];
	int fail = 0;
	hist_clear();
	qh = qn = 0;
	active = false;
	notif_config(true);
	notif_dnd_set(false);
	dc_card(&n, NO_UDP, NI_DEFAULT, L"x", L"y");
	n.sfx = SFX_NONE;
	fail += !notif_push(&n);
	notif_update(0);
	fail += !active || ready || notif_hist_read(0, NULL);
	start = now_s() - dur - OUT_S - 1;
	notif_update(0);
	fail += active || notif_hist_read(2, h) != 1 || !h[0].missed || h[0].seen || wcscmp(h[0].body, L"y");
	hist_clear();
	qh = qn = 0;
	if (fail) {
		glog("notif: unshown card selftest failed (%d)", fail);
	}
	return fail;
}

int notif_selftest(void) {
	static struct notif n;
	WCHAR nm[128], val[16], trk[64];
	int fail = 0, want[NOTIF_CAP] = { 3, NOTIF_CAP + 1, 0, 1, 2 }, i;
	for (i = 5; i < NOTIF_CAP; i++) {
		want[i] = i - 1;
	}
	test_card(&n);
	for (i = 0; i < NOTIF_CAP + 2; i++) {
		n.icon = i == 3 || i == NOTIF_CAP + 1 ? NI_BATTERY : NI_DEFAULT;
		swprintf(n.title, ARRAYSIZE(n.title), L"%d", i);
		fail += notif_push(&n) != (i != NOTIF_CAP);
	}
	for (i = 0; i < NOTIF_CAP; i++) {
		fail += !pop() || _wtoi(cur.title) != want[i];
	}
	fail += pop();
	if (fail) {
		glog("notif: battery priority queue selftest failed");
	}
	notif_bat_text(BD_TRACKER + 1, BR_LEFT_FOOT, 14, nm, val);
	fail += wcsncmp(nm, tr(S_W_LEFT_FOOT), wcslen(tr(S_W_LEFT_FOOT))) != 0;
	swprintf(trk, ARRAYSIZE(trk), tr(S_N_DEV_TRACKER), 2);
	notif_bat_text(BD_TRACKER + 1, BR_NONE, 14, nm, val);
	fail += wcsncmp(nm, trk, wcslen(trk)) != 0;
	return fail + burst_test() + initial_test() + hist_test() + dnd_test() + lost_test();
}

bool notif_render_card(const struct notif *n, float f, const WCHAR *path) {
	struct gx_tex *t = gx_tex_new(TW, TH);
	struct gx_bmp *b = gx_bmp_new(ICON_PX, ICON_PX);
	bool ok;
	if (b && n->icon == NI_IMAGE) {
		gx_bmp_set(b, n->img, ICON_PX * 4);
	}
	layout(n);
	ok = t && draw(t, n, b, f) && gx_save_png(t, path);
	gx_tex_free(t);
	gx_bmp_free(b);
	return ok;
}

int notif_render_samples(const WCHAR *dir) {
	const struct sample_card *s;
	struct gx_tex *t = gx_tex_new(TW, TH);
	struct gx_bmp *b = gx_bmp_new(ICON_PX, ICON_PX);
	int count = 0, i;
	WCHAR path[MAX_PATH];
	sample_path(L"avatar.png", path, MAX_PATH);
	if (b && img_decode_file(path, cur.img)) {
		gx_bmp_set(b, cur.img, ICON_PX * 4);
	}
	for (i = 0; i < SAMPLE_CARDS; i++) {
		s = &sample_cards[i];
		test_card(&cur);
		if (s->title) {
			put(cur.title, ARRAYSIZE(cur.title), s->title);
			put(cur.body, ARRAYSIZE(cur.body), s->body);
			put(cur.source, ARRAYSIZE(cur.source), s->source);
		}
		cur.kind = s->kind;
		cur.icon = s->icon;
		cur.bat_dev = s->dev;
		cur.bat_pct = s->pct;
		layout(&cur);
		swprintf(path, MAX_PATH, L"%ls\\notif-%ls.png", dir, s->name);
		if (draw(t, &cur, b, s->f) && gx_save_png(t, path)) {
			count++;
		}
	}
	gx_tex_free(t);
	gx_bmp_free(b);
	return count;
}
#endif
