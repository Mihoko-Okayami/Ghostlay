#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "battery.h"
#include "cfg.h"
#include "gfx.h"
#include "lang.h"
#include "laser.h"
#include "main.h"
#include "media.h"
#include "notif.h"
#include "panel.h"
#include "vr.h"
#include "watch.h"

#ifdef GL_TESTS
#include "samples.h"
#endif

#define TW 640
#define TH 640
#define XSO_H 440
#define C_OFF 0x8C929Cu
#define C_CHARGE 0x5BD68Au
#define TITLE_W 204
#define PRESS_S 0.15
#define PROWS 3
#define BAR_MAX 6
#define TIME_X 39
#define DATE_BASE 198
#define REF_PX 20
#define TIME_FL (GT_PNUM | GT_TIGHT)
#define GRID_X 267
#define GRID_W 337
#define TRAY_Y 214
#define TRAY_H 56
#define BTN_X 480
#define BTN_GAP 52
#define BTN_Y 242
#define LOW_Y 282
#define HROWS 5
#define ROW_H 58
#define LIST_Y 322
#define OPEN_S 0.15
#define SNAP_S 0.12
#define DWELL_S 0.28f
#define SEEN_FAST 3.0f
#define DZ 0.25f
#define DZ_ON 0.30f
#define DZ_OFF 0.20f
#define RATE_MAX 11.0f
#define RATE_MIN 1.5f

#ifdef GL_TESTS
#define SAMPLES 14
#endif

enum { T_NONE, T_DND, T_HIST, T_GEAR, T_PREV, T_TOGGLE, T_NEXT, T_TRACKERS, T_POPUP };

struct view {
	SYSTEMTIME st;
	int up, hover, press;
	const struct bat_snap *bat;
	bool card, playing, popup, hist, dnd;
	struct gx_bmp *cover;
	double pos, dur;
	const WCHAR *title, *artist, *badge;
	float title_w, mq_shift, op, scroll, fade;
	int hn, missed, hrow;
	const struct notif_hist *rows;
	struct gx_bmp **av;
};

static struct {
	struct ovl ov;
	struct laser_target lt;
	struct gx_tex *tex;
	struct gx_bmp *cover, *av[HIST_CAP];
	SYSTEMTIME st;
	bool dirty, has_cover, card, popup, hist, dnd, stick;
	int hover, press, up, sec, hn, missed, hrow;
	float alpha, title_w, mq_shift, op, clip, lx, ly, scroll, vel, from, to, dwell;
	double t_title, press_end, open_t, snap_t;
	uint32_t cfg_gen, gfx_gen, bat_ver, med_ver, nver, hgen, dwell_id;
	WCHAR badge[4];
	struct media_snap m;
	struct notif_hist rows[HIST_CAP];
} w;

static struct {
	WCHAR key[48], tm[16], date[80];
	float tx, dx, dy, k, tl, tr, l, r;
} lay;

static const HmdMatrix34_t xso_left = { { { -0.083190f, 0.075290f, -0.993685f, -0.052032f }, { -0.776694f, -0.629640f, 0.017317f, 0.015226f }, { -0.624360f, 0.773230f, 0.110858f, 0.138261f } } };
static const uint32_t lvl_fill[3] = { 0xD9DCE1, C_WARN, C_CRIT }, lvl_text[3] = { C_TEXT, C_WARN, C_INK };
static const enum str_id btn_name[3] = { S_W_DND, S_W_HISTORY, S_CONFIGURATION };

static int pct_of(float p) {
	return (int)lroundf(p * 100);
}

static int charge(const struct bat_dev *d) {
	if (!d->present || !d->known || !d->charging) return 0;
	return pct_of(d->pct) >= 99 ? 2 : 1;
}

static int pop_rows(int n) {
	return (n + 1) / 2;
}

static bool bars(int n) {
	return n <= BAR_MAX;
}

static int time_font(void) {
	return gx_font(GF_DISPLAY, g_cfg.clock24 ? 82 : 88, 600);
}

static void fit(const SYSTEMTIME *st) {
	WCHAR key[48], d[64];
	int ft, fr, i;
	float s = 32;
	swprintf(key, 48, L"%d %d %d:%d %d %d %d", g_cfg.lang, g_cfg.clock24, st->wHour, st->wMinute, st->wDayOfWeek, st->wDay, st->wMonth);
	if (!wcscmp(key, lay.key)) return;
	wcscpy(lay.key, key);
	fmt_time(lay.tm, 16, st, g_cfg.clock24);
	ft = time_font();
	fr = gx_font(GF_TEXT, REF_PX, 600);
	gx_ink(ft, lay.tm, TIME_FL, &lay.tl, &lay.tr);
	for (i = 0; i < 3; i++) {
		fmt_date_wd(d, 64, st, i);
		if (g_cfg.clock24) {
			put(lay.date, 80, d);
		} else {
			swprintf(lay.date, 80, L"%ls · %ls", fmt_ampm(st), d);
		}
		gx_ink(fr, lay.date, 0, &lay.l, &lay.r);
		s = lay.r > lay.l ? fminf(32, floorf(20 * REF_PX * (lay.tr - lay.tl) / (lay.r - lay.l)) / 20) : 32;
		if (s >= (i ? 17 : 19)) break;
	}
	lay.k = s / REF_PX;
	lay.tx = TIME_X - lay.tl;
	lay.dx = TIME_X - lay.k * lay.l + (s < 32 ? (lay.tr - lay.tl - lay.k * (lay.r - lay.l)) / 2 : 0);
	lay.dy = DATE_BASE - lay.k * gx_baseline(fr);
}

static void card_bg(float y, float ch) {
	gx_shadow_rrect(12, y, 616, ch, 24, 12, 4, col_rgba(0, 0.32f));
	gx_fill_rrect(12, y, 616, ch, 24, col_rgba(C_CARD, 0.95f));
	gx_stroke_rrect(12, y, 616, ch, 24, 2, col_rgba(0xFFFFFF, 0.07f));
}

static void top_zone(const struct view *v) {
	WCHAR hh[16], *mm;
	const WCHAR *ap;
	int ft = time_font(), fr = gx_font(GF_TEXT, REF_PX, 600);
	float y = g_cfg.clock24 ? 85 : 84, h = g_cfg.clock24 ? 82 : 88, x, dw;
	size_t n;
	fit(&v->st);
	put(hh, 16, lay.tm);
	mm = wcschr(hh, L':');
	if (!mm) return;
	*mm++ = 0;
	x = lay.tx + gx_text(ft, hh, lay.tx, y, 0, h, col_rgb(C_TEXT), TIME_FL);
	x += gx_text(ft, L":", x, y, 0, h, col_accent(), TIME_FL);
	gx_text(ft, mm, x, y, 0, h, col_rgb(C_TEXT), TIME_FL);
	gx_transform(lay.k, lay.dx, lay.dy);
	if (g_cfg.clock24) {
		gx_text(fr, lay.date, 0, 0, 0, 24, col_rgb(C_SOFT), 0);
	} else {
		ap = fmt_ampm(&v->st);
		n = wcslen(ap);
		dw = gx_measure(fr, lay.date, 0, 0, NULL);
		gx_text(fr, ap, 0, 0, 0, 24, col_rgb(C_TEXT), 0);
		gx_text(fr, L" · ", dw - gx_measure(fr, lay.date + n, 0, 0, NULL), 0, 0, 24, col_rgb(0x7D8490), 0);
		gx_text(fr, lay.date + n + 3, dw - gx_measure(fr, lay.date + n + 3, 0, 0, NULL), 0, 0, 24, col_rgb(C_SOFT), 0);
	}
	gx_transform(1, 0, 0);
}

static void seg(float x, float y, float sw, float f, float r, int lv) {
	struct gx_color fill = col_rgb(lvl_fill[lv]);
	if (f < 0) {
		gx_fill_rrect(x, y, sw, 8, r, col_rgba(0xFFFFFF, 0.06f));
	} else if (f >= 1) {
		gx_fill_rrect(x, y, sw, 8, r, fill);
	} else {
		gx_fill_rrect(x, y, sw, 8, r, lv ? col_rgba(lvl_fill[lv], 0.3f) : col_rgba(0xFFFFFF, 0.2f));
		if (f > 0) {
			gx_clip_push(x, y, sw * f, 8);
			gx_fill_rrect(x, y, sw, 8, r, fill);
			gx_clip_pop();
		}
	}
}

static void gauge(float x, float y, float cw, int p, bool ok, int lv) {
	float sw = (cw - 16) / 5;
	int i;
	for (i = 0; i < 5; i++) {
		seg(x + i * (sw + 4), y, sw, ok ? fmaxf(0, p / 20.0f - i) : -1, 2, lv);
	}
}

static void label_fit(const WCHAR *s, float x, float y, float lw, uint32_t rgb) {
	int f;
	float nw;
	f = gx_font(GF_TEXT, 20, 500);
	nw = gx_measure(f, s, 0, 0, NULL);
	if (nw > lw && lw > 0) {
		f = gx_font(GF_TEXT, fmaxf(16, floorf(40 * lw / nw) / 2), 500);
	}
	gx_text(f, s, x, y, lw, 28, col_rgb(rgb), GT_ELLIPSIS);
}

static float head(float x, float y, float cw, bool ok, bool unlinked, int pct, int lv, int chg) {
	int fv;
	float tw, l;
	WCHAR b[16];
	fv = gx_font(GF_DISPLAY, 22, 600);
	if (!ok) {
		tw = gx_text(fv, L"\x2014", x, y, cw - 9, 28, col_rgb(C_OFF), GT_RIGHT);
		l = x + cw - 9 - tw;
		if (unlinked) {
			l -= 28;
			gx_icon(IC_UNLINK, l, y + 3, 22, 2.25f, col_rgb(C_OFF));
		}
		return l - x - 8;
	}
	fmt_num(b, 16, pct, 0, L"%");
	tw = gx_measure(fv, b, 0, GT_TNUM, NULL);
	l = x + cw - tw - 18;
	if (lv == BL_CRITICAL) {
		gx_fill_rrect(l, y, tw + 18, 28, 8, col_rgb(C_CRIT));
	} else if (lv == BL_WARNING) {
		gx_stroke_rrect(l, y, tw + 18, 28, 8, 2, col_rgb(C_WARN));
	} else {
		l += 9;
	}
	gx_text(fv, b, x, y, cw - 9, 28, col_rgb(lvl_text[lv]), GT_RIGHT | GT_TNUM);
	if (chg) {
		l -= lv ? 23 : 20;
		gx_icon(chg > 1 ? IC_CHECK : IC_BOLT, l, y + 5, 18, chg > 1 ? 3 : 2.2f, col_rgb(C_CHARGE));
	}
	return l - x - (chg ? 6 : 8);
}

static void device(float x, float y, float cw, const WCHAR *label, enum gx_icon ic, const struct bat_dev *d) {
	bool ok = d->present && d->known;
	int p = pct_of(d->pct), lv = ok && !d->charging ? bat_level(p / 100.0f) : BL_NORMAL;
	float lw = head(x, y, cw, ok, !d->present, p, lv, charge(d)), lx = x;
	if (ic < IC_COUNT) {
		gx_icon(ic, x, y + 4, 20, 2.2f, col_rgb(ok ? C_DIM : C_OFF));
		lx += 26;
		lw -= 26;
	}
	label_fit(label, lx, y, lw, ok ? C_DIM : C_OFF);
	gauge(x, y + 36, cw, p, ok, lv);
}

static bool trk_cell(const struct bat_snap *b, int *p, int *lv, int *chg) {
	int n = b->ntrk, nc = 0, nf = 0, i, c;
	bool ok = b->trk_known > 0 && b->trk_avg >= 0;
	for (i = 0; i < n; i++) {
		c = charge(&b->trk[i]);
		nc += c > 0;
		nf += c == 2;
	}
	*p = pct_of(b->trk_avg);
	*lv = ok && nc < b->trk_known ? bat_level(*p / 100.0f) : BL_NORMAL;
	*chg = nc ? (nf == nc ? 2 : 1) : 0;
	return ok;
}

static void trackers(float x, float y, float cw, const struct view *v) {
	const struct bat_snap *b = v->bat;
	const struct bat_dev *d;
	WCHAR s[48];
	int n = b->ntrk, i, p, lv, chg;
	uint32_t rgb;
	float lw, sw;
	bool ok;
	if (v->popup) {
		gx_fill_rrect(x - 12, y - 8, cw + 24, 60, 12, col_accent_soft());
		gx_stroke_rrect(x - 12, y - 8, cw + 24, 60, 12, 2, col_accent_mid());
	} else if (v->hover == T_TRACKERS || v->press == T_TRACKERS) {
		gx_fill_rrect(x - 12, y - 8, cw + 24, 60, 12, col_rgba(0xFFFFFF, v->press == T_TRACKERS ? 0.1f : 0.06f));
	}
	ok = trk_cell(b, &p, &lv, &chg);
	lw = head(x, y, cw, ok, false, p, lv, chg);
	rgb = ok ? C_DIM : C_OFF;
	if (!bars(n)) {
		swprintf(s, 48, L"%ls %d", tr(S_W_TRACKERS), n);
		label_fit(gx_measure(gx_font(GF_TEXT, 20, 500), s, 0, 0, NULL) <= lw ? s : tr(S_W_TRACKERS), x, y, lw, rgb);
	} else {
		label_fit(tr(S_W_TRACKERS), x, y, lw, rgb);
	}
	if (!bars(n)) {
		gauge(x, y + 36, cw, p, ok, lv);
		return;
	}
	sw = (cw - 6 * (n - 1)) / n;
	for (i = 0; i < n; i++) {
		d = &b->trk[i];
		ok = d->present && d->known;
		p = pct_of(d->pct);
		seg(x + i * (sw + 6), y + 36, sw, ok ? p / 100.0f : -1, 4, ok && !d->charging ? bat_level(p / 100.0f) : BL_NORMAL);
	}
}

static void grid(const struct view *v) {
	float cw = (GRID_W - 24) / 2.0f;
	if (v->bat->ntrk > 0) {
		device(GRID_X, 94, cw, tr(S_W_HEADSET), IC_COUNT, &v->bat->hmd);
		trackers(GRID_X + cw + 24, 94, cw, v);
	} else {
		device(GRID_X, 94, GRID_W, tr(S_W_HEADSET), IC_COUNT, &v->bat->hmd);
	}
	device(GRID_X, 154, cw, tr(S_W_LEFT), IC_CTRL_L, &v->bat->left);
	device(GRID_X + cw + 24, 154, cw, tr(S_W_RIGHT), IC_CTRL_R, &v->bat->right);
}

static bool shown_btn(int k) {
	return k == T_GEAR || g_cfg.notif_on;
}

static float btn_cx(int k) {
	return BTN_X + BTN_GAP * (k - T_DND);
}

static float chip_right(void) {
	return btn_cx(g_cfg.notif_on ? T_DND : T_GEAR) - 32;
}

static int btn_at(float px, float py) {
	int k;
	if (py < TRAY_Y || py >= TRAY_Y + TRAY_H) return T_NONE;
	for (k = T_DND; k <= T_GEAR; k++) {
		if (shown_btn(k) && px >= btn_cx(k) - BTN_GAP / 2 && px < btn_cx(k) + BTN_GAP / 2) return k;
	}
	return T_NONE;
}

static void tray_button(const struct view *v, int k, enum gx_icon ic) {
	float cx = btn_cx(k), r = v->press == k ? 18.5f : 20, s = v->press == k ? 20 : 22, sw = 2.1f;
	struct gx_color ink = col_rgb(C_ICON);
	if (k == T_HIST && v->hist) {
		gx_fill_circle(cx, BTN_Y, r, col_accent());
		ink = col_on(g_cfg.accent);
	} else if (k == T_DND && v->dnd) {
		gx_fill_circle(cx, BTN_Y, r, col_accent_soft());
		gx_stroke_circle(cx, BTN_Y, r - 1, 2, col_accent());
		ink = col_accent();
		ic = IC_MOON_FILL;
		sw = 1;
	} else if (v->hover == k) {
		gx_fill_circle(cx, BTN_Y, r, col_accent_soft());
		gx_stroke_circle(cx, BTN_Y, r - 1, 2, col_accent_mid());
		ink = col_accent();
	} else {
		gx_fill_circle(cx, BTN_Y, r, col_rgba(0xFFFFFF, 0.07f));
		gx_stroke_circle(cx, BTN_Y, r - 0.75f, 1.5f, col_rgba(0xFFFFFF, 0.06f));
	}
	gx_icon(ic, cx - s / 2, BTN_Y - s / 2, s, sw, ink);
}

static void badge(const WCHAR *s) {
	int f = gx_font(GF_TEXT, 14, 700);
	float bw = fmaxf(22, gx_measure(f, s, 0, GT_TNUM, NULL) + 12), x = btn_cx(T_HIST) + 14 - bw / 2;
	gx_fill_rrect(x - 2, TRAY_Y + 1, bw + 4, 26, 13, col_rgb(0x1B1D22));
	gx_fill_rrect(x, TRAY_Y + 3, bw, 22, 11, col_accent());
	gx_text(f, s, x, TRAY_Y + 3, bw, 22, col_on(g_cfg.accent), GT_CENTER | GT_TNUM);
}

static void tray(const struct view *v) {
	WCHAR up[24];
	const WCHAR *s = NULL;
	int f = gx_font(GF_TEXT, 20, 600);
	struct gx_color c = col_accent();
	gx_clip_push(14, TRAY_Y, 612, TRAY_H - 2);
	gx_fill_rrect(14, TRAY_Y - 30, 612, TRAY_H + 28, 22, col_rgba(0xFFFFFF, 0.03f));
	gx_clip_pop();
	gx_fill_rect(14, TRAY_Y, 612, 1, col_rgba(0xFFFFFF, 0.07f));
	fmt_hms(up, 24, v->up);
	gx_icon(IC_STOPWATCH, 36, BTN_Y - 10, 20, 2.3f, col_rgb(C_DIM));
	gx_text(f, up, 64, BTN_Y - 12, 0, 24, col_rgb(C_SOFT), GT_TNUM);
	if (v->hover >= T_DND && v->hover <= T_GEAR && shown_btn(v->hover)) {
		s = tr(btn_name[v->hover - T_DND]);
	} else if (v->dnd && g_cfg.notif_on) {
		s = tr(S_W_DND);
		c = col_rgb(C_DIM);
	}
	if (s) {
		gx_text(f, s, chip_right(), BTN_Y - 12, 0, 24, c, GT_RIGHT);
	}
	if (g_cfg.notif_on) {
		tray_button(v, T_DND, IC_MOON);
		tray_button(v, T_HIST, IC_HISTORY);
	}
	tray_button(v, T_GEAR, IC_GEAR);
	if (g_cfg.notif_on && v->missed > 0 && v->badge && v->badge[0]) {
		badge(v->badge);
	}
}

static void scroll_marks(float top, float bottom, float pos, int n, int vis) {
	struct gx_stop fade[2] = { { 0, col_rgba(C_CARD, 0) }, { 1, col_rgba(C_CARD, 0.85f) } };
	float h = bottom - 14 - top;
	if (n <= vis) return;
	if (pos < n - vis - 0.01f) {
		gx_clip_push(14, bottom - 32, 612, 30);
		gx_grad_rrect(14, bottom - 62, 612, 60, 22, 0, bottom - 32, 0, bottom - 2, fade, 2);
		gx_clip_pop();
	}
	gx_fill_rrect(619, top, 4, h, 2, col_rgba(0xFFFFFF, 0.06f));
	gx_fill_rrect(619, top + h * pos / n, 4, h * vis / n, 2, col_rgba(0xFFFFFF, 0.36f));
}

static float pop_h(int n) {
	return 60 * (pop_rows(n) < PROWS ? pop_rows(n) : PROWS) + 20;
}

static void popup(const struct view *v) {
	const struct bat_dev *d;
	const WCHAR *label;
	WCHAR nm[48];
	int n = v->bat->ntrk, first = 2 * (int)floorf(v->scroll), i;
	float h = pop_h(n);
	card_bg(LOW_Y, h);
	gx_clip_push(14, LOW_Y + 2, 612, h - 4);
	for (i = first; i < n && i < first + 2 * (PROWS + 1); i++) {
		d = &v->bat->trk[i];
		if (bat_role_str(d->role) > 0) {
			label = tr(bat_role_str(d->role));
		} else {
			swprintf(nm, 48, tr(S_N_DEV_TRACKER), i + 1);
			label = nm;
		}
		device(36 + i % 2 * 296, LOW_Y + 18 + 60 * (i / 2 - v->scroll), 272, label, IC_COUNT, d);
	}
	gx_clip_pop();
	scroll_marks(LOW_Y + 14, LOW_Y + h, v->scroll, pop_rows(n), PROWS);
}

static float hist_h(int n) {
	if (!n) return 104;
	return 50 + ROW_H * (n < HROWS ? n : HROWS);
}

static bool crit_row(const struct notif_hist *h) {
	return h->icon == NI_BATTERY && h->kind == NK_CRITICAL;
}

static void meta(int f, const WCHAR *m, float x1, float avail, float y) {
	WCHAR b[176];
	int lo = 1, hi = (int)wcslen(m), mid;
	if (hi > 0 && gx_measure(f, m, 0, 0, NULL) > avail) {
		while (lo < hi) {
			mid = (lo + hi) / 2;
			swprintf(b, 176, L"…%ls", m + mid);
			if (gx_measure(f, b, 0, 0, NULL) <= avail) {
				hi = mid;
			} else {
				lo = mid + 1;
			}
		}
		lo += IS_LOW_SURROGATE(m[lo]);
		swprintf(b, 176, L"…%ls", m + lo);
		m = b;
	}
	gx_text(f, m, x1, y, 0, 28, col_rgb(C_DIM), GT_RIGHT);
}

static void hist_row(const struct notif_hist *h, struct gx_bmp *b, float y, bool line, bool hover) {
	WCHAR t[24], m[160], c[32], dev[136], pct[16];
	const WCHAR *title = h->icon == NI_BATTERY ? tr(S_N_LOW_BATTERY) : h->title;
	bool bat = h->icon == NI_BATTERY, crit = crit_row(h);
	int ft = gx_font(GF_DISPLAY, 22, 600), fb = gx_font(GF_TEXT, 20, 500), fs = gx_font(GF_TEXT, 20, 600);
	float ty = bat || h->body[0] ? y + 1 : y + 15, tw, d;
	if (crit) {
		gx_fill_rrect(24, y - 1, 592, 60, 16, col_rgba(C_CRIT, 0.09f));
		gx_stroke_rrect(24, y - 1, 592, 60, 16, 2, col_rgba(C_CRIT, 0.32f));
	}
	if (hover) {
		gx_fill_rrect(24, y - 1, 592, 60, 16, col_rgba(g_cfg.accent, 0x1F / 255.0f));
		gx_stroke_rrect(24, y - 1, 592, 60, 16, 2, col_accent_mid());
	}
	if (line && y + 57 < LIST_Y + ROW_H * HROWS - 1) {
		gx_fill_rect(96, y + 57, 508, 1, col_rgba(0xFFFFFF, 0.07f));
	}
	gx_stroke_circle(60, y + 29, 24, 4, col_rgb(crit ? C_RING_CRIT : C_RING));
	notif_medallion(h->icon, h->kind, h->title, b, 60, y + 29, 18);
	if (h->missed && !h->seen) {
		gx_fill_circle(78, y + 47, 12, col_rgb(0x15171C));
		gx_fill_circle(78, y + 47, 10, col_accent());
		gx_icon(IC_MOON_FILL, 71.5f, y + 40.5f, 13, 1.4f, col_on(g_cfg.accent));
	}
	fmt_time(t, 24, &h->at, g_cfg.clock24);
	if (!g_cfg.clock24) {
		wcscat(t, L" ");
		wcscat(t, fmt_ampm(&h->at));
	}
	put(m, 160, h->source);
	if (h->count > 1) {
		swprintf(c, 32, tr(S_N_MESSAGES), h->count);
		swprintf(m + wcslen(m), 160 - wcslen(m), m[0] ? L" · %ls" : L"%ls", c);
	}
	swprintf(m + wcslen(m), 160 - wcslen(m), m[0] ? L" · %ls" : L"%ls", t);
	tw = fminf(gx_measure(ft, title, 0, 0, NULL), 300);
	gx_text(ft, title, 96, ty, 300, 28, col_rgb(C_TEXT), GT_ELLIPSIS | GT_LINES(1));
	meta(fb, m, 604, 508 - tw - 12, ty);
	if (bat) {
		notif_bat_text(h->bat_dev, h->bat_role, h->bat_pct, dev, pct);
		d = gx_text(fb, dev, 96, y + 29, 508 - gx_measure(fs, pct, 0, GT_TNUM, NULL), 26, col_rgb(C_DEVICE), GT_TNUM | GT_ELLIPSIS);
		gx_text(fs, pct, 96 + d, y + 29, 0, 26, col_rgb(crit ? C_CRIT_T : C_WARN_T), GT_TNUM);
	} else if (h->body[0]) {
		gx_text(fb, h->body, 96, y + 29, 508, 26, col_rgb(C_ICON), GT_ELLIPSIS | GT_LINES(1));
	}
}

static void hist_head(int missed) {
	WCHAR s[32];
	int f = gx_font(GF_TEXT, 18, 600);
	float x = 36 + gx_text(f, tr(S_W_RECENT), 36, LOW_Y + 12, 0, 24, col_rgb(C_DIM), 0);
	if (missed <= 0) return;
	x += 12 + gx_text(f, L"·", x + 6, LOW_Y + 12, 0, 24, col_rgb(0x6F7580), 0);
	gx_icon(IC_MOON_FILL, x, LOW_Y + 16, 16, 1.2f, col_accent());
	swprintf(s, 32, tr(missed == 1 ? S_W_MISSED_ONE : S_W_MISSED), missed);
	gx_text(f, s, x + 22, LOW_Y + 12, 0, 24, col_accent(), 0);
}

static void hist_card(const struct view *v) {
	int n = v->hn, first = (int)floorf(v->scroll), i;
	float y;
	if (!n) {
		card_bg(LOW_Y, 104);
		gx_stroke_circle(64, LOW_Y + 52, 28, 4, col_rgb(C_RING));
		gx_fill_circle(64, LOW_Y + 52, 23, col_rgba(0xFFFFFF, 0.06f));
		gx_icon(IC_HISTORY, 51, LOW_Y + 39, 26, 2, col_rgb(C_OFF));
		gx_text(gx_font(GF_DISPLAY, 22, 600), tr(S_W_HIST_EMPTY), 110, LOW_Y + 25, 494, 28, col_rgb(C_SOFT), GT_ELLIPSIS);
		gx_text(gx_font(GF_TEXT, 20, 500), tr(S_W_HIST_KEPT), 110, LOW_Y + 53, 494, 26, col_rgb(C_DIM), GT_ELLIPSIS);
		return;
	}
	card_bg(LOW_Y, hist_h(n));
	hist_head(v->missed);
	gx_clip_push(14, LIST_Y - 1, 612, ROW_H * HROWS + 2);
	for (i = first; i < n && i <= first + HROWS; i++) {
		y = LIST_Y + ROW_H * (i - v->scroll);
		hist_row(&v->rows[i], v->av[i], y, i + 1 < n && !crit_row(&v->rows[i]) && !crit_row(&v->rows[i + 1]) && i != v->hrow && i + 1 != v->hrow, i == v->hrow);
	}
	gx_clip_pop();
	scroll_marks(LOW_Y + 44, LOW_Y + hist_h(n), v->scroll, n, HROWS);
}

static void button(float cx, enum gx_icon ic, float sw, bool accent, bool hot, bool down) {
	struct gx_color bg = accent ? col_accent() : col_rgba(0xFFFFFF, hot ? 0.22f : 0.1f);
	float r = down ? 37 : 40, s = down ? 29 : 32, cy = LOW_Y + 52;
	if (down) {
		bg = accent ? col_mix(bg, col_rgb(0), 0.2f) : col_rgba(0xFFFFFF, 0.14f);
	}
	gx_fill_circle(cx, cy, r, bg);
	if (hot) {
		gx_stroke_circle(cx, cy, r - 1.5f, 3, accent ? col_rgb(0xFFFFFF) : col_accent());
	}
	gx_icon(ic, cx - s / 2, cy - s / 2, s, sw, accent ? col_on(g_cfg.accent) : col_rgb(0xE8EAEE));
}

static void media_card(const struct view *v) {
	struct gx_stop mask[4] = { { 0, col_rgba(0, v->mq_shift < 0 ? 0 : 1) }, { 16.0f / TITLE_W, col_rgb(0) }, { 1 - 20.0f / TITLE_W, col_rgb(0) }, { 1, col_rgba(0, 0) } };
	WCHAR a[24], d[24], b[32];
	float f, x, cy = LOW_Y + 52;
	int ft, f5;
	card_bg(LOW_Y, 104);
	gx_opacity(v->playing ? 1 : 0.5f);
	gx_stroke_circle(64, cy, 36, 6, col_rgba(0xFFFFFF, 0.12f));
	f = v->dur > 0 ? (float)(floor(v->pos) / v->dur) : 0;
	if (f > 0) {
		gx_arc(64, cy, 36, 6, 0, 360 * (f > 1 ? 1 : f), col_accent());
	}
	if (v->cover) {
		gx_bmp_draw(v->cover, 35, cy - 29, 58, 58, 29);
	} else {
		gx_fill_circle(64, cy, 29, col_rgba(0xFFFFFF, 0.08f));
		gx_icon(IC_GHOST, 50, cy - 14, 28, 2, col_rgb(C_DIM));
	}
	gx_opacity(1);
	ft = gx_font(GF_DISPLAY, 22, 600);
	f5 = gx_font(GF_TEXT, 20, 500);
	if (v->title_w > TITLE_W) {
		gx_mask_push(120, LOW_Y + 13, TITLE_W, 28, mask, 4);
		gx_opacity(v->op);
		gx_text(ft, v->title, 120 + v->mq_shift, LOW_Y + 13, 0, 28, col_rgb(C_TEXT), 0);
		gx_opacity(1);
		gx_layer_pop();
	} else {
		gx_text(ft, v->title, 120, LOW_Y + 13, TITLE_W, 28, col_rgb(C_TEXT), GT_ELLIPSIS);
	}
	gx_text(f5, v->artist, 120, LOW_Y + 41, TITLE_W, 26, col_rgb(C_SOFT), GT_ELLIPSIS);
	if (v->dur > 0) {
		fmt_clock(a, 24, v->pos);
		fmt_clock(d, 24, v->dur);
		swprintf(b, 32, L" / %ls", d);
		x = 120 + gx_text(gx_font(GF_TEXT, 20, 600), a, 120, LOW_Y + 67, 0, 24, col_rgb(v->playing ? C_TEXT : C_DIM), GT_TNUM);
		gx_text(f5, b, x, LOW_Y + 67, 0, 24, col_rgb(C_DIM), GT_TNUM);
	}
	button(380, IC_PREV, 2.25f, false, v->hover == T_PREV, v->press == T_PREV);
	button(472, v->playing ? IC_PAUSE : IC_PLAY, v->playing ? 3 : 2.25f, true, v->hover == T_TOGGLE, v->press == T_TOGGLE);
	button(564, IC_NEXT, 2.25f, false, v->hover == T_NEXT, v->press == T_NEXT);
}

static bool draw(struct gx_tex *t, const struct view *v) {
	if (!gx_begin(t)) return false;
	card_bg(78, 192);
	tray(v);
	top_zone(v);
	grid(v);
	if (v->hist && v->fade < 1) {
		gx_layer_push(v->fade);
		hist_card(v);
		gx_layer_pop();
	} else if (v->hist) {
		hist_card(v);
	} else if (v->popup && v->bat->ntrk > 0) {
		popup(v);
	} else if (v->card) {
		media_card(v);
	}
	return gx_end();
}

static void marquee(float tw, double t, float *shift, float *op) {
	float over = tw - TITLE_W, travel = over + 24, run = travel / 40;
	*shift = 0;
	*op = 1;
	if (over <= 0) return;
	t = fmod(t, 3.5 + run);
	if (t < 1.5) return;
	if (t < 1.5 + run) {
		*shift = -(float)(t - 1.5) * 40;
	} else if (t < 3 + run) {
		*shift = -travel;
	} else if (t < 3.25 + run) {
		*shift = -travel;
		*op = 1 - (float)(t - 3 - run) / 0.25f;
	} else {
		*op = (float)(t - 3.25 - run) / 0.25f;
	}
	*shift = roundf(*shift);
	*op = roundf(*op * 20) / 20;
}

static void sync_media(double now) {
	uint32_t v = media_version();
	const struct media_snap *s;
	bool nt, card;
	if (v != w.med_ver || w.gfx_gen != g_gfx_gen) {
		s = media_lock();
		nt = wcscmp(w.m.title, s->title) != 0;
		if (!s->has_cover) {
			w.has_cover = false;
		} else if (!w.m.has_cover || memcmp(w.m.cover, s->cover, sizeof(s->cover)) || w.gfx_gen != g_gfx_gen) {
			w.has_cover = gx_bmp_set(w.cover, s->cover, ICON_PX * 4);
		}
		w.med_ver = v;
		w.gfx_gen = g_gfx_gen;
		w.m = *s;
		media_unlock();
		if (nt) {
			w.title_w = gx_measure(gx_font(GF_DISPLAY, 22, 600), w.m.title, 0, 0, NULL);
			w.t_title = now;
		}
		w.dirty = true;
	}
	card = g_cfg.media && (w.m.active || w.has_cover);
	if (card != w.card) {
		w.card = card;
		w.dirty = true;
	}
}

static void refresh(double now) {
	SYSTEMTIME st;
	int up, sec;
	float sh = 0, op = 1;
	bool m = w.card && !w.popup && !w.hist;
	GetLocalTime(&st);
	up = app_uptime(now);
	sec = m ? (int)media_pos(&w.m, now) : -1;
	if (m) {
		marquee(w.title_w, now - w.t_title, &sh, &op);
	}
	if (st.wMinute != w.st.wMinute || st.wHour != w.st.wHour || st.wDay != w.st.wDay || up != w.up || sec != w.sec || sh != w.mq_shift || op != w.op || g_bat.version != w.bat_ver) {
		w.dirty = true;
	}
	w.st = st;
	w.up = up;
	w.sec = sec;
	w.mq_shift = sh;
	w.op = op;
	w.bat_ver = g_bat.version;
}

static float lower_h(void) {
	if (w.hist) return hist_h(w.hn);
	if (w.popup && g_bat.ntrk > 0) return pop_h(g_bat.ntrk);
	return w.card ? 104 : 0;
}

static int target(float px, float py) {
	float cw = (GRID_W - 24) / 2.0f;
	int k = btn_at(px, py), i;
	if (k) return k;
	if (g_bat.ntrk > 0 && py >= 86 && py < 146 && px >= GRID_X + cw + 12 && px < 616) return T_TRACKERS;
	if (w.hist || (w.popup && g_bat.ntrk > 0)) return px >= 12 && px < 628 && py >= LOW_Y && py < LOW_Y + lower_h() ? T_POPUP : T_NONE;
	for (i = 0; w.card && i < 3; i++) {
		if (hypotf(px - (380 + 92 * i), py - (LOW_Y + 52)) <= 40) return T_PREV + i;
	}
	return T_NONE;
}

static int row_at(float x, float y) {
	int r;
	if (!w.hist || !w.hn || x < 12 || x >= 628 || y < LIST_Y || y >= LIST_Y + ROW_H * (w.hn < HROWS ? w.hn : HROWS)) return -1;
	r = (int)floorf((y - LIST_Y) / ROW_H + w.scroll);
	return r < w.hn ? r : -1;
}

static void press(int k, double end) {
	if (k == w.press && end == w.press_end) return;
	w.press = k;
	w.press_end = end;
	w.dirty = true;
}

static void scroll_reset(void) {
	w.scroll = w.vel = 0;
	w.snap_t = -1;
	w.stick = false;
	laser_stick(&w.lt, false);
	w.dirty = true;
}

static float scroll_max(void) {
	if (w.hist) return fmaxf(0, (float)(w.hn - HROWS));
	return w.popup ? fmaxf(0, (float)(pop_rows(g_bat.ntrk) - PROWS)) : 0;
}

static void scroll_shift(int d) {
	float max = scroll_max(), p = clampf(w.scroll + d, 0, max);
	if (p != w.scroll) {
		w.dirty = true;
	}
	w.scroll = p;
	w.from = clampf(w.from + d, 0, max);
	w.to = clampf(w.to + d, 0, max);
}

static void hist_show(bool open) {
	if (open == w.hist) return;
	w.hist = open;
	notif_hist_open(open);
	w.hn = 0;
	w.hrow = -1;
	w.dwell = 0;
	scroll_reset();
	w.dwell_id = 0;
	w.open_t = now_s();
	w.nver = notif_hist_version() - 1;
}

static void pop_show(bool open) {
	if (open == w.popup) return;
	w.popup = open;
	scroll_reset();
}

static void activate(int k) {
	if (w.popup && k != T_TRACKERS && k != T_POPUP) {
		pop_show(false);
	}
	if (w.hist && k != T_HIST && k != T_POPUP && k != T_DND) {
		hist_show(false);
	}
	if (!k || k == T_POPUP) return;
	press(k, now_s() + PRESS_S);
	if (k == T_GEAR) {
		panel_toggle();
	} else if (k == T_DND) {
		notif_dnd_set(!notif_dnd());
	} else if (k == T_HIST) {
		hist_show(!w.hist);
	} else if (k == T_TRACKERS) {
		pop_show(!w.popup);
	} else {
		media_cmd(k - T_PREV);
	}
}

static void sync_popup(void) {
	if (w.popup && g_bat.ntrk <= 0) {
		pop_show(false);
	}
	if (!w.popup) return;
	scroll_shift(0);
}

static void read_rows(void) {
	uint32_t top = 0;
	int k = (int)floorf(w.scroll), n, i, d = 0;
	if (k < w.hn) {
		top = w.rows[k].id;
	}
	n = notif_hist_read(HIST_CAP, w.rows);
	for (i = 0; i < n; i++) {
		if (w.rows[i].icon == NI_IMAGE) {
			gx_bmp_set(w.av[i], w.rows[i].img, HIST_PX * 4);
		}
		if (top && w.rows[i].id == top) {
			d = i - k;
		}
	}
	w.hn = n;
	scroll_shift(d);
}

static void sync_notif(void) {
	uint32_t v = notif_hist_version();
	if (!g_cfg.notif_on) {
		hist_show(false);
	}
	if (v == w.nver && w.hgen == g_gfx_gen) return;
	w.nver = v;
	w.hgen = g_gfx_gen;
	w.dnd = notif_dnd();
	w.missed = notif_badge(w.badge, ARRAYSIZE(w.badge));
	if (w.hist) {
		read_rows();
	}
	w.dirty = true;
}

static float stick_rate(float x, float y, bool *on) {
	float m = hypotf(x, y), s;
	*on = *on ? m >= DZ_OFF : m > DZ_ON;
	if (!*on || fabsf(x) > fabsf(y)) return 0;
	s = fmaxf(0, (fabsf(y) - DZ) / (1 - DZ));
	s = fmaxf(s * s * RATE_MAX, RATE_MIN);
	return y > 0 ? -s : s;
}

static void scroll_step(double now, float dt, float sx, float sy) {
	float max = scroll_max(), v, p = clampf(w.scroll, 0, max), k;
	if (!w.hist && !w.popup) return;
	v = stick_rate(sx, sy, &w.stick);
	if (v != 0) {
		p = clampf(p + v * dt, 0, max);
		w.snap_t = -1;
	} else if (w.snap_t < 0 && p != roundf(p)) {
		w.from = p;
		w.to = clampf(roundf(p), 0, max);
		w.snap_t = now - dt;
	}
	if (v == 0 && w.snap_t >= 0) {
		k = clampf((float)((now - w.snap_t) / SNAP_S), 0, 1);
		k = 1 - (1 - k) * (1 - k) * (1 - k);
		p = w.from + (w.to - w.from) * k;
		if (k >= 1) {
			p = w.to;
			w.snap_t = -1;
		}
	}
	w.vel = dt > 0 ? fabsf(p - w.scroll) / dt : 0;
	if (p != w.scroll) {
		w.scroll = p;
		w.dirty = true;
	}
}

static uint32_t dwell_step(double now, float dt) {
	int r = row_at(w.lx, w.ly);
	struct notif_hist *h = r >= 0 ? &w.rows[r] : NULL;
	bool want = h && h->missed && !h->seen;
	if (r != w.hrow) {
		w.hrow = r;
		w.dirty = true;
	}
	if (!want || h->id != w.dwell_id) {
		w.dwell_id = want ? h->id : 0;
		w.dwell = 0;
		return 0;
	}
	if (w.vel > SEEN_FAST || now - w.open_t < OPEN_S) return 0;
	w.dwell += dt;
	if (w.dwell < DWELL_S) return 0;
	h->seen = true;
	w.dwell_id = 0;
	return h->id;
}

static void hidden(void) {
	w.hover = w.press = T_NONE;
	w.lx = w.ly = -1;
	laser_stick(&w.lt, false);
	pop_show(false);
	hist_show(false);
}

static int input(struct laser_target *t, int ev, float x, float y) {
	int k = target(x, y), was = w.press;
	bool button = k && k != T_POPUP;
	(void)t;
	switch (ev) {
	case LE_PASS:
		return x < 12 || x >= 628 || !((y >= 78 && y < 270) || (y >= LOW_Y && y < LOW_Y + lower_h()));
	case LE_MOVE:
	case LE_LEAVE:
		w.lx = x;
		w.ly = y;
		laser_stick(&w.lt, ev == LE_MOVE && k == T_POPUP);
		if (k == w.hover) return 0;
		w.hover = k;
		w.dirty = true;
		return ev == LE_MOVE && button ? LR_HAPTIC : 0;
	case LE_DOWN:
		press(k, 1e30);
		return button ? LR_HAPTIC : 0;
	case LE_UP:
		press(T_NONE, 0);
		if (k == was) {
			activate(k);
		}
		break;
	}
	return 0;
}

void watch_init(void) {
	int i;
	memset(&w, 0, sizeof(w));
	w.cfg_gen = g_cfg_gen - 1;
	w.gfx_gen = g_gfx_gen - 1;
	w.op = 1;
	w.lx = w.ly = -1;
	w.hrow = -1;
	w.snap_t = -1;
	w.nver = notif_hist_version() - 1;
	w.tex = gx_tex_new(TW, TH);
	w.cover = gx_bmp_new(ICON_PX, ICON_PX);
	for (i = 0; i < HIST_CAP; i++) {
		w.av[i] = gx_bmp_new(HIST_PX, HIST_PX);
	}
	if (!w.tex || !w.cover) {
		glog("watch: texture creation failed");
		return;
	}
	if (!ovl_create(&w.ov, GL_APP_KEY ".watch", "Ghostlay Watch")) return;
	w.lt = (struct laser_target){ &w.ov, TW, TH, NULL, input, -1 };
	laser_add(&w.lt);
}

void watch_shutdown(void) {
	int i;
	if (w.hist) {
		notif_hist_open(false);
	}
	ovl_destroy(&w.ov);
	gx_tex_free(w.tex);
	gx_bmp_free(w.cover);
	for (i = 0; i < HIST_CAP; i++) {
		gx_bmp_free(w.av[i]);
	}
	memset(&w, 0, sizeof(w));
}

bool watch_update(double dt) {
	double now;
	bool right, face = false, drew = false;
	TrackedDeviceIndex_t dev;
	const HmdMatrix34_t *pw, *ph;
	HmdMatrix34_t rel, wm, xf;
	float a, fade, sx, sy;
	struct view v;
	uint32_t id;
	int i, k;
	if (!w.ov.h) return false;
	now = now_s();
	if (w.cfg_gen != g_cfg_gen) {
		w.cfg_gen = g_cfg_gen;
		w.dirty = true;
		ovl_width(&w.ov, 0.132f * g_cfg.watch_size / 100);
		w.clip = cosf(g_cfg.watch_angle * DEG);
	}
	sync_media(now);
	right = g_cfg.wrist == WRIST_RIGHT;
	dev = right ? g_dev.right : g_dev.left;
	pw = vr_pose(dev);
	ph = vr_pose(0);
	rel = xso_left;
	if (right) {
		rel.m[0][3] = -rel.m[0][3];
		rel.m[0][1] = -rel.m[0][1];
		rel.m[0][2] = -rel.m[0][2];
		rel.m[1][0] = -rel.m[1][0];
		rel.m[2][0] = -rel.m[2][0];
	}
	xf = rel;
	for (i = 0; i < 3; i++) {
		xf.m[i][3] -= rel.m[i][1] * (TH - XSO_H) / 2 * w.ov.width / TW;
	}
	if (pw) {
		ovl_rel(&w.ov, dev, &xf);
		wm = m34_mul(pw, &rel);
		face = ph && g_app.worn && v3_dot(v3_norm(m34_col(&wm, 2)), v3_norm(v3_sub(m34_pos(ph), m34_pos(&wm)))) >= w.clip;
	}
	a = pw ? w.alpha + (face ? 1 : -1) * (float)dt / 0.2f : 0;
	a = clampf(a, 0, 1);
	if (a > 0 && w.alpha <= 0) {
		w.dirty = true;
		w.t_title = now;
	}
	w.alpha = a;
	if (a <= 0) {
		ovl_show(&w.ov, false);
		hidden();
		return false;
	}
	if (w.press && now >= w.press_end) {
		press(T_NONE, 0);
	}
	sync_popup();
	sync_notif();
	k = w.ly >= 0 ? target(w.lx, w.ly) : T_NONE;
	if (k != w.hover) {
		w.hover = k;
		w.dirty = true;
	}
	laser_stick(&w.lt, k == T_POPUP);
	laser_stick_pos(&w.lt, &sx, &sy);
	scroll_step(now, (float)dt, sx, sy);
	id = dwell_step(now, (float)dt);
	if (id && notif_hist_seen(id)) {
		sync_notif();
	}
	refresh(now);
	fade = w.hist ? clampf((float)((now - w.open_t) / OPEN_S), 0, 1) : 1;
	if (fade < 1) {
		w.dirty = true;
	}
	if (w.dirty) {
		v = (struct view){ .st = w.st, .up = w.up, .hover = w.hover, .press = w.press, .bat = &g_bat, .card = w.card, .playing = w.m.playing, .popup = w.popup, .hist = w.hist, .dnd = w.dnd, .cover = w.has_cover ? w.cover : NULL, .pos = media_pos(&w.m, now), .dur = w.m.dur, .title = w.m.title, .artist = w.m.artist, .badge = w.badge, .title_w = w.title_w, .mq_shift = w.mq_shift, .op = w.op, .scroll = w.scroll, .fade = fade, .hn = w.hn, .missed = w.missed, .hrow = w.hrow, .rows = w.rows, .av = w.av };
		if (draw(w.tex, &v)) {
			ovl_texture(&w.ov, w.tex);
			w.dirty = false;
			drew = true;
		}
	}
	ovl_alpha(&w.ov, a * g_cfg.watch_opacity / 100);
	ovl_show(&w.ov, true);
	return a < 1 || drew;
}

#ifdef GL_TESTS
static void sample_hist(struct notif_hist *h, struct gx_bmp **av) {
	static uint8_t img[ICON_PX * ICON_PX * 4];
	const struct sample_note *s;
	WCHAR p[MAX_PATH];
	int i, m;
	sample_path(L"avatar.png", p, MAX_PATH);
	img_decode_file(p, img);
	for (i = 0; i < SAMPLES; i++) {
		s = &sample_history[i % SAMPLE_HISTORY];
		m = s->hour * 60 + s->minute - i / SAMPLE_HISTORY * 47;
		h[i] = (struct notif_hist){ .id = (uint32_t)(i + 1), .kind = s->kind, .icon = s->icon, .count = 1, .bat_dev = s->dev, .bat_pct = s->pct, .at = { 2026, 10, 5, 23, (WORD)(m / 60), (WORD)(m % 60), 0, 0 } };
		put(h[i].title, ARRAYSIZE(h[i].title), s->title);
		put(h[i].body, ARRAYSIZE(h[i].body), s->body);
		put(h[i].source, ARRAYSIZE(h[i].source), s->source);
		if (s->icon == NI_IMAGE) {
			notif_shrink(h[i].img, img);
			gx_bmp_set(av[i], h[i].img, HIST_PX * 4);
		}
	}
}

static void sample_bat(struct bat_snap *b) {
	int i;
	*b = (struct bat_snap){ .version = 1, .hmd = { .present = true, .known = true, .pct = sample_levels[0], .idx = 0 }, .left = { .present = true, .known = true, .pct = sample_levels[1], .idx = 1 }, .right = { .present = true, .known = true, .pct = sample_levels[2], .idx = 2 }, .ntrk = SAMPLE_TRACKERS };
	for (i = 0; i < SAMPLE_TRACKERS; i++) {
		b->trk[i] = (struct bat_dev){ .present = true, .known = true, .pct = sample_trackers[i].pct, .charging = sample_trackers[i].charging, .idx = (TrackedDeviceIndex_t)(3 + i), .role = sample_trackers[i].role };
	}
}

static int shot(struct gx_tex *t, struct view *v, struct bat_snap *b, const WCHAR *dir, const WCHAR *name) {
	WCHAR p[MAX_PATH];
	bat_summary(b);
	v->title_w = gx_measure(gx_font(GF_DISPLAY, 22, 600), v->title, 0, 0, NULL);
	swprintf(p, MAX_PATH, L"%ls\\watch-%ls.png", dir, name);
	if (draw(t, v) && gx_save_png(t, p)) return 1;
	glog("watch: sample %ls failed", name);
	return 0;
}

static void sample_missed(struct notif_hist *rows, int n) {
	int i;
	for (i = 0; i < SAMPLES; i++) {
		rows[i].missed = i < n;
		rows[i].seen = false;
		rows[i].count = 1;
	}
	rows[0].count = n ? 3 : 1;
}

static int dnd_shots(struct gx_tex *t, struct view *v, struct bat_snap *b, struct notif_hist *rows, const WCHAR *dir) {
	int n = 0;
	g_cfg.clock24 = true;
	v->card = false;
	n += shot(t, v, b, dir, L"dnd-off");
	v->hover = T_DND;
	n += shot(t, v, b, dir, L"dnd-hover");
	v->hover = T_NONE;
	v->dnd = true;
	n += shot(t, v, b, dir, L"dnd-on");
	v->missed = 3;
	v->badge = L"3";
	n += shot(t, v, b, dir, L"dnd-missed");
	b->right.pct = 0.12f;
	n += shot(t, v, b, dir, L"dnd-critical");
	b->right.pct = 0.31f;
	v->dnd = false;
	v->hist = true;
	sample_missed(rows, 3);
	n += shot(t, v, b, dir, L"dnd-history");
	rows[0].seen = true;
	v->missed = 2;
	v->badge = L"2";
	v->hrow = 0;
	n += shot(t, v, b, dir, L"dnd-history-hover");
	rows[1].seen = rows[2].seen = true;
	v->missed = 0;
	v->badge = L"";
	v->hrow = -1;
	v->scroll = 4;
	n += shot(t, v, b, dir, L"dnd-history-scrolled");
	v->scroll = 2.5f;
	n += shot(t, v, b, dir, L"history-mid-scroll");
	v->scroll = 9;
	n += shot(t, v, b, dir, L"history-bottom");
	v->scroll = 0;
	v->hist = false;
	v->dnd = true;
	g_cfg.lang = LANG_FR;
	n += shot(t, v, b, dir, L"fr-dnd-on");
	v->dnd = false;
	g_cfg.notif_on = false;
	g_cfg.lang = LANG_EN;
	n += shot(t, v, b, dir, L"notifs-off");
	v->hover = T_GEAR;
	n += shot(t, v, b, dir, L"notifs-off-hover");
	v->hover = T_NONE;
	g_cfg.notif_on = true;
	sample_missed(rows, 0);
	return n;
}

int watch_render_samples(const WCHAR *dir) {
	static uint8_t px[ICON_PX * ICON_PX * 4];
	static struct notif_hist rows[SAMPLES];
	struct gx_tex *t = gx_tex_new(TW, TH);
	struct gx_bmp *cv = gx_bmp_new(ICON_PX, ICON_PX), *av[SAMPLES];
	int n = 0, i;
	struct bat_snap b;
	struct cfg keep;
	struct view v;
	WCHAR p[MAX_PATH];
	for (i = 0; i < SAMPLES; i++) {
		av[i] = gx_bmp_new(HIST_PX, HIST_PX);
	}
	sample_bat(&b);
	sample_path(L"cover.png", p, MAX_PATH);
	img_decode_file(p, px);
	if (t && cv && gx_bmp_set(cv, px, ICON_PX * 4)) {
		keep = g_cfg;
		g_cfg.notif_on = true;
		sample_hist(rows, av);
		v = (struct view){ .st = { 2026, 10, 5, 23, 21, 47, 0, 0 }, .up = 4987, .hover = T_NONE, .press = T_NONE, .bat = &b, .playing = true, .cover = cv, .pos = sample_media.pos, .dur = sample_media.dur, .title = sample_media.title, .artist = sample_media.artist, .badge = L"", .op = 1, .fade = 1, .hn = SAMPLES, .hrow = -1, .rows = rows, .av = av };
		n += shot(t, &v, &b, dir, L"a2-12h");
		v.st.wHour = 12;
		v.st.wMinute = 59;
		n += shot(t, &v, &b, dir, L"a2-1259");
		v.st.wHour = 21;
		v.st.wMinute = 47;
		g_cfg.clock24 = true;
		n += shot(t, &v, &b, dir, L"a2-24h");
		g_cfg.clock24 = false;
		v.card = true;
		n += shot(t, &v, &b, dir, L"a2-media");
		v.hover = T_HIST;
		n += shot(t, &v, &b, dir, L"hover-history");
		v.hover = T_GEAR;
		n += shot(t, &v, &b, dir, L"hover-config");
		v.hover = T_PREV;
		n += shot(t, &v, &b, dir, L"media-hover");
		v.hover = v.press = T_TOGGLE;
		n += shot(t, &v, &b, dir, L"media-press");
		v.hover = v.press = T_NONE;
		v.hist = true;
		v.hn = SAMPLE_HISTORY;
		n += shot(t, &v, &b, dir, L"history");
		v.hn = 3;
		n += shot(t, &v, &b, dir, L"history-3");
		v.hn = 0;
		n += shot(t, &v, &b, dir, L"history-empty");
		v.hn = SAMPLES;
		v.hist = false;
		n += dnd_shots(t, &v, &b, rows, dir);
		g_cfg.clock24 = false;
		v.card = true;
		v.title = sample_media_long.title;
		v.artist = sample_media_long.artist;
		v.dur = sample_media_long.dur;
		n += shot(t, &v, &b, dir, L"title-long");
		marquee(v.title_w, 3, &v.mq_shift, &v.op);
		n += shot(t, &v, &b, dir, L"title-scroll");
		v.mq_shift = 0;
		v.op = 1;
		v.card = false;
		b.ntrk = 0;
		n += shot(t, &v, &b, dir, L"no-trackers");
		b.ntrk = 6;
		b.right.present = false;
		n += shot(t, &v, &b, dir, L"right-missing");
		b.ntrk = 0;
		b.left.present = false;
		n += shot(t, &v, &b, dir, L"controllers-missing");
		b.ntrk = 6;
		b.left.present = b.right.present = true;
		b.right.pct = 0.12f;
		g_cfg.lang = LANG_FR;
		g_cfg.clock24 = true;
		v.card = true;
		v.playing = false;
		v.title = sample_media.title;
		v.artist = sample_media.artist;
		v.dur = sample_media.dur;
		n += shot(t, &v, &b, dir, L"fr-24h-critical");
		g_cfg.clock24 = false;
		v.st.wDayOfWeek = 3;
		v.st.wDay = 30;
		v.st.wMonth = 9;
		n += shot(t, &v, &b, dir, L"fr-12h-mercredi");
		v.st = (SYSTEMTIME){ 2026, 10, 5, 23, 21, 47, 0, 0 };
		g_cfg = keep;
		g_cfg.notif_on = true;
		g_cfg.clock24 = false;
		b.right.pct = 0.31f;
		n += shot(t, &v, &b, dir, L"paused");
		v.card = false;
		b.hmd.charging = true;
		b.hmd.pct = 0.3f;
		b.left.charging = true;
		b.left.pct = 1;
		b.right.pct = 0.64f;
		b.trk[5].charging = true;
		n += shot(t, &v, &b, dir, L"charging");
		b.hmd.charging = b.left.charging = b.trk[5].charging = false;
		b.hmd.pct = 0.78f;
		b.left.pct = 0.31f;
		b.right.pct = 0.12f;
		b.trk[5].pct = 0.12f;
		n += shot(t, &v, &b, dir, L"critical");
		b.left.pct = 0.64f;
		b.right.pct = 0.31f;
		b.trk[4].pct = 0.31f;
		v.card = v.playing = v.popup = true;
		n += shot(t, &v, &b, dir, L"trackers-popup-6");
		v.popup = false;
		n += shot(t, &v, &b, dir, L"trackers-6");
		b.ntrk = SAMPLE_MANY;
		for (i = 0; i < SAMPLE_MANY; i++) {
			b.trk[i] = (struct bat_dev){ .present = true, .known = sample_many[i].pct >= 0, .pct = fmaxf(sample_many[i].pct, 0), .idx = (TrackedDeviceIndex_t)(3 + i), .role = sample_many[i].role };
		}
		n += shot(t, &v, &b, dir, L"trackers-11");
		v.popup = true;
		n += shot(t, &v, &b, dir, L"trackers-popup");
		v.scroll = 3;
		n += shot(t, &v, &b, dir, L"trackers-scrolled");
		sample_bat(&b);
		v.popup = false;
		v.scroll = 0;
		v.card = false;
		v.st.wHour = 13;
		v.st.wMinute = 5;
		v.up = 37425;
		n += shot(t, &v, &b, dir, L"1305-uptime-10h");
		v.st.wHour = 10;
		v.st.wMinute = 0;
		v.st.wDayOfWeek = 3;
		v.st.wDay = 30;
		v.st.wMonth = 9;
		n += shot(t, &v, &b, dir, L"1000-wednesday");
		g_cfg = keep;
	} else {
		glog("watch: sample texture creation failed");
	}
	for (i = 0; i < SAMPLES; i++) {
		gx_bmp_free(av[i]);
	}
	gx_bmp_free(cv);
	gx_tex_free(t);
	return n;
}

static int fit_case(bool h24, int lang, int hour, int minute, int wday, int day, int mon) {
	SYSTEMTIME st = { 2026, (WORD)mon, (WORD)wday, (WORD)day, (WORD)hour, (WORD)minute, 0, 0 };
	WCHAR d[80], f[64];
	float tl, tr, dl, dr, sz[3], l, r, size;
	int fr, i, want, wd = -1;
	bool bad;
	g_cfg.clock24 = h24;
	g_cfg.lang = lang;
	lay.key[0] = 0;
	fit(&st);
	fr = gx_font(GF_TEXT, REF_PX, 600);
	for (i = 0; i < 3; i++) {
		fmt_date_wd(f, 64, &st, i);
		if (h24) {
			put(d, 80, f);
		} else {
			swprintf(d, 80, L"%ls · %ls", fmt_ampm(&st), f);
		}
		gx_ink(fr, d, 0, &l, &r);
		sz[i] = fminf(32, floorf(20 * REF_PX * (lay.tr - lay.tl) / (r - l)) / 20);
		wd = wd < 0 && !wcscmp(d, lay.date) ? i : wd;
	}
	want = sz[0] >= 19 ? 0 : sz[1] >= 17 ? 1 : 2;
	size = lay.k * REF_PX;
	tl = lay.tx + lay.tl;
	tr = lay.tx + lay.tr;
	dl = lay.dx + lay.k * lay.l;
	dr = lay.dx + lay.k * lay.r;
	bad = fabsf(tl - TIME_X) > 1e-3f || wd != want || fabsf(size - sz[want]) > 1e-4f || fabsf(size * 20 - roundf(size * 20)) > 1e-3f || size > 32;
	bad |= size < 32 ? fabsf(dl - tl) > 0.25f || fabsf(dr - tr) > 0.25f : fabsf(dl - tl) > 1e-3f || dr > tr + 0.25f;
	bad |= fabsf(lay.dy + lay.k * gx_baseline(fr) - DATE_BASE) > 1e-3f;
	if (bad) {
		glog("watch: fit %02d:%02d %s lang %d weekday %d: time ink %.2f-%.2f, date ink %.2f-%.2f at %.2f px, form %d (want %d)", hour, minute, h24 ? "24h" : "12h", lang, wday, tl, tr, dl, dr, size, wd, want);
	}
	return bad;
}

static int fit_test(void) {
	static const struct { bool h24; int lang, hour, minute, wday, day, mon; } c[] = {
		{ false, LANG_EN, 13, 5, 5, 23, 10 }, { false, LANG_EN, 13, 11, 5, 23, 10 }, { false, LANG_EN, 21, 47, 5, 23, 10 }, { false, LANG_EN, 10, 0, 5, 23, 10 }, { false, LANG_EN, 12, 59, 5, 23, 10 }, { false, LANG_EN, 23, 11, 5, 23, 10 },
		{ true, LANG_EN, 21, 47, 5, 23, 10 }, { true, LANG_EN, 1, 5, 5, 23, 10 }, { true, LANG_EN, 11, 11, 5, 23, 10 }, { true, LANG_EN, 0, 0, 3, 30, 9 },
		{ false, LANG_EN, 1, 5, 3, 30, 9 }, { false, LANG_EN, 1, 11, 3, 30, 9 }, { false, LANG_EN, 12, 59, 3, 30, 9 }, { false, LANG_EN, 10, 0, 4, 24, 12 }, { false, LANG_EN, 21, 47, 6, 28, 2 },
		{ true, LANG_FR, 21, 47, 3, 30, 9 }, { true, LANG_FR, 1, 11, 3, 30, 9 }, { false, LANG_FR, 9, 47, 3, 30, 9 }, { false, LANG_FR, 12, 59, 0, 31, 12 }, { true, LANG_FR, 10, 0, 6, 1, 8 },
	};
	struct cfg saved = g_cfg;
	int fail = 0, i;
	if (!gfx_init(-1)) {
		glog("watch: the fit test needs graphics");
		return 1;
	}
	for (i = 0; i < (int)ARRAYSIZE(c); i++) {
		fail += fit_case(c[i].h24, c[i].lang, c[i].hour, c[i].minute, c[i].wday, c[i].day, c[i].mon);
	}
	g_cfg = saved;
	lay.key[0] = 0;
	gfx_shutdown();
	return fail;
}

static int ck(bool ok, const char *what) {
	if (!ok) {
		glog("watch: scroll %.3f, dwell %.3f", w.scroll, w.dwell);
	}
	return st_ck(ok, "watch", what);
}

static int run_stick(double *now, float sx, float sy, double secs) {
	int i, n = (int)(secs / 0.011 + 0.5);
	for (i = 0; i < n; i++) {
		*now += 0.011;
		scroll_step(*now, 0.011f, sx, sy);
	}
	return n;
}

static int scroll_test(void) {
	double now = 50;
	bool on = false;
	int fail = 0, i;
	float r, prev;
	r = stick_rate(0, -0.29f, &on);
	fail += ck(r == 0 && !on, "dead zone: 0.29 does not start");
	r = stick_rate(0, -0.31f, &on);
	fail += ck(on && fabsf(r - RATE_MIN) < 1e-5f, "0.31 starts at the 1.5 rows/s floor");
	r = stick_rate(0, -0.21f, &on);
	fail += ck(on && r > 0, "hysteresis keeps it running at 0.21");
	r = stick_rate(0.1f, -0.15f, &on);
	fail += ck(!on && r == 0, "stops below 0.20");
	r = stick_rate(0, -1, &on);
	fail += ck(fabsf(r - RATE_MAX) < 1e-4f, "full down is 11 rows/s");
	r = stick_rate(0, 1, &on);
	fail += ck(fabsf(r + RATE_MAX) < 1e-4f, "full up is -11 rows/s");
	r = stick_rate(0, -0.625f, &on);
	fail += ck(fabsf(r - 2.75f) < 1e-4f, "quadratic speed curve");
	r = stick_rate(0.8f, -0.5f, &on);
	fail += ck(r == 0 && on, "x larger than y is ignored");
	memset(&w, 0, sizeof(w));
	w.hist = true;
	w.hn = SAMPLES;
	w.snap_t = -1;
	run_stick(&now, 0, -1, 0.5);
	fail += ck(fabsf(w.scroll - 5.5f) < 0.1f, "half a second at full speed scrolls 5.5 rows");
	prev = w.scroll;
	run_stick(&now, 0, 0, 0.099);
	fail += ck(w.scroll != roundf(prev) && fabsf(w.scroll - roundf(prev)) < 0.2f, "release eases toward the nearest row");
	run_stick(&now, 0, 0, 0.033);
	fail += ck(w.scroll == roundf(prev) && w.snap_t < 0, "snapped to the row within 120 ms");
	run_stick(&now, 0, -1, 2);
	fail += ck(w.scroll == SAMPLES - HROWS, "clamped at the bottom");
	run_stick(&now, 0, -1, 0.2);
	fail += ck(w.scroll == SAMPLES - HROWS && w.vel == 0, "stays at the bottom");
	run_stick(&now, 0, 1, 2);
	fail += ck(w.scroll == 0, "clamped at the top");
	w.scroll = 2.4f;
	run_stick(&now, 0, 0, 0.143);
	fail += ck(w.scroll == 2, "2.4 snaps to 2");
	w.scroll = 2.6f;
	run_stick(&now, 0, 0, 0.143);
	fail += ck(w.scroll == 3, "2.6 snaps to 3");
	w.scroll = 0;
	w.stick = false;
	run_stick(&now, 0.1f, -0.28f, 0.5);
	fail += ck(w.scroll == 0, "inside the dead zone nothing moves");
	run_stick(&now, 0, -0.35f, 1);
	fail += ck(fabsf(w.scroll - RATE_MIN) < 0.06f, "slow push runs at the floor");
	w.hist = false;
	prev = w.scroll;
	for (i = 0; i < 3; i++) {
		run_stick(&now, 0, -1, 0.1);
	}
	fail += ck(w.scroll == prev, "no scrolling when the history is closed");
	return fail;
}

static int pop_test(void) {
	static const struct { int n, rows; float h, max; bool bars; } cnt[] = { { 1, 1, 80, 0, true }, { 6, 3, 200, 0, true }, { 7, 4, 200, 1, false }, { 12, 6, 200, 3, false }, { 13, 7, 200, 4, false }, { 32, 16, 200, 13, false } };
	static const struct { int n; float scroll; } drop[] = { { 12, 3 }, { 7, 1 }, { 6, 0 }, { 32, 0 } };
	double now = 120;
	int fail = 0, i;
	memset(&w, 0, sizeof(w));
	hidden();
	for (i = 0; i < (int)ARRAYSIZE(cnt); i++) {
		g_bat.ntrk = cnt[i].n;
		if (pop_rows(cnt[i].n) != cnt[i].rows || pop_h(cnt[i].n) != cnt[i].h || bars(cnt[i].n) != cnt[i].bars || LOW_Y + pop_h(cnt[i].n) + 16 > TH) {
			glog("watch: %d trackers give %d rows, height %.0f, bars %d", cnt[i].n, pop_rows(cnt[i].n), pop_h(cnt[i].n), bars(cnt[i].n));
			fail++;
		}
		activate(T_TRACKERS);
		fail += ck(w.popup && w.scroll == 0 && target(300, LOW_Y + cnt[i].h - 0.1f) == T_POPUP && target(300, LOW_Y + cnt[i].h) == T_NONE, "the tracker popup opens at the top");
		run_stick(&now, 0, -1, 2);
		fail += ck(w.scroll == cnt[i].max, "tracker popup clamped at the last row");
		w.scroll = 40;
		sync_popup();
		fail += ck(w.scroll == cnt[i].max, "sync keeps the tracker scroll clamped");
		fail += ck(!input(&w.lt, LE_DOWN, 300, LOW_Y + 50) && !input(&w.lt, LE_UP, 300, LOW_Y + 50) && w.popup && w.scroll == cnt[i].max, "a click in the tracker popup does nothing");
		run_stick(&now, 0, 1, 2);
		fail += ck(w.scroll == 0, "tracker popup clamped at the top");
		run_stick(&now, 0, -1, 2);
		fail += ck(input(&w.lt, LE_DOWN, 500, 110) == LR_HAPTIC && !input(&w.lt, LE_UP, 500, 110) && !w.popup && w.scroll == 0, "a click on the Trackers cell closes the popup and resets the scroll");
	}
	g_bat.ntrk = 13;
	activate(T_TRACKERS);
	run_stick(&now, 0, -1, 2);
	for (i = 0; i < (int)ARRAYSIZE(drop); i++) {
		g_bat.ntrk = drop[i].n;
		sync_popup();
		if (!w.popup || w.scroll != drop[i].scroll) {
			glog("watch: %d trackers while open give open %d scroll %.2f, expected %.0f", drop[i].n, w.popup, w.scroll, drop[i].scroll);
			fail++;
		}
	}
	w.scroll = 10.6f;
	run_stick(&now, 0, 0, 0.05);
	fail += ck(w.snap_t >= 0 && w.to == 11, "easing toward row 11");
	g_bat.ntrk = 13;
	sync_popup();
	for (i = 0; i < 20; i++) {
		run_stick(&now, 0, 0, 0.011);
		fail += ck(w.scroll == 4, "an easing past the new last row stays on it");
	}
	g_bat.ntrk = 0;
	sync_popup();
	fail += ck(!w.popup && w.scroll == 0, "the popup closes when no tracker is left");
	g_bat.ntrk = 13;
	activate(T_TRACKERS);
	run_stick(&now, 0, -1, 2);
	hidden();
	activate(T_TRACKERS);
	fail += ck(w.popup && w.scroll == 0, "hiding the watch resets the scroll");
	hidden();
	return fail;
}

static int dwell_test(void) {
	double now = 80;
	uint32_t id = 0;
	int fail = 0, n = 0, i;
	memset(&w, 0, sizeof(w));
	w.hist = true;
	w.hn = 7;
	w.hrow = -1;
	for (i = 0; i < 7; i++) {
		w.rows[i] = (struct notif_hist){ .id = (uint32_t)(40 + i), .missed = i == 1 || i == 6 };
	}
	w.open_t = now - 1;
	w.lx = 300;
	w.ly = LIST_Y + ROW_H + 20;
	while (!id && n < 100) {
		now += 0.011;
		id = dwell_step(now, 0.011f);
		n++;
	}
	fail += ck(id == 41 && n * 0.011 >= DWELL_S && n * 0.011 < DWELL_S + 0.025, "seen after 280 ms on the row");
	fail += ck(w.rows[1].seen && w.hrow == 1, "row marked seen and hovered");
	fail += ck(!dwell_step(now + 0.011, 0.011f), "seen only once");
	w.ly = LIST_Y + 20;
	for (i = 0; i < 40; i++) {
		fail += ck(!dwell_step(now += 0.011, 0.011f), "rows that were not missed are never marked");
	}
	w.scroll = 2;
	w.ly = LIST_Y + 4 * ROW_H + 20;
	w.vel = 4;
	for (i = 0; i < 40; i++) {
		fail += ck(!dwell_step(now += 0.011, 0.011f), "no dwell while the list scrolls fast");
	}
	w.vel = 2;
	for (i = 0, id = 0; !id && i < 40; i++) {
		id = dwell_step(now += 0.011, 0.011f);
	}
	fail += ck(id == 46 && i >= 25, "slow scrolling still counts");
	w.rows[6].seen = false;
	w.open_t = now;
	for (i = 0, id = 0; !id && i < 60; i++) {
		id = dwell_step(now += 0.011, 0.011f);
	}
	fail += ck(id == 46 && i * 0.011 >= OPEN_S + DWELL_S - 0.012, "not counted during the open animation");
	w.rows[6].seen = false;
	w.open_t = now - 1;
	for (i = 0; i < 20; i++) {
		dwell_step(now += 0.011, 0.011f);
	}
	w.ly = LIST_Y + 2 * ROW_H + 20;
	dwell_step(now += 0.011, 0.011f);
	w.ly = LIST_Y + 4 * ROW_H + 20;
	for (i = 0, id = 0; !id && i < 60; i++) {
		id = dwell_step(now += 0.011, 0.011f);
	}
	fail += ck(id == 46 && i >= 26, "leaving the row restarts the timer");
	w.ly = -1;
	dwell_step(now, 0.011f);
	fail += ck(w.hrow == -1, "no hovered row when the laser leaves");
	return fail;
}

static int anchor_test(void) {
	static struct notif n;
	WCHAR b[8];
	uint32_t top;
	int fail = 0, i, k;
	bool on = g_cfg.notif_on;
	memset(&w, 0, sizeof(w));
	w.snap_t = -1;
	g_cfg.notif_on = true;
	n = (struct notif){ .kind = NK_NORMAL, .icon = NI_DEFAULT, .origin = NO_UDP, .count = 1, .bat_dev = -1, .bat_pct = -1 };
	notif_dnd_set(true);
	for (i = 0; i < 10; i++) {
		swprintf(n.title, ARRAYSIZE(n.title), L"a%d", i);
		notif_push(&n);
	}
	hist_show(true);
	sync_notif();
	k = w.hn;
	fail += ck(k >= 10 && w.missed >= 10 && w.dnd && !wcscmp(w.badge, L"9+"), "history read with the missed count and badge");
	w.scroll = 2;
	top = w.rows[2].id;
	for (i = 0; i < 2; i++) {
		swprintf(n.title, ARRAYSIZE(n.title), L"b%d", i);
		notif_push(&n);
	}
	sync_notif();
	fail += ck(w.hn == (k + 2 < HIST_CAP ? k + 2 : HIST_CAP) && w.scroll == 4 && w.rows[4].id == top, "new entries keep the visible rows in place");
	fail += ck(notif_hist_seen(w.rows[5].id), "a missed entry can be marked seen");
	sync_notif();
	fail += ck(w.scroll == 4 && w.rows[5].seen, "a seen entry keeps the scroll");
	fail += ck(w.missed == notif_badge(b, ARRAYSIZE(b)) && !wcscmp(w.badge, b), "badge follows the seen entry in the same frame");
	hist_show(false);
	notif_dnd_set(false);
	sync_notif();
	fail += ck(!w.hist && !w.dnd, "closed");
	memset(&w, 0, sizeof(w));
	g_cfg.notif_on = on;
	return fail;
}

static int tray_test(void) {
	static const struct { bool on; float x, y; int k; } hit[] = {
		{ true, 480, 242, T_DND }, { true, 532, 242, T_HIST }, { true, 584, 242, T_GEAR }, { true, 454, 214, T_DND }, { true, 453.9f, 242, T_NONE }, { true, 505.9f, 242, T_DND }, { true, 506, 242, T_HIST },
		{ true, 557.9f, 269.9f, T_HIST }, { true, 558, 269.9f, T_GEAR }, { true, 609.9f, 242, T_GEAR }, { true, 610, 242, T_NONE }, { true, 532, 213.9f, T_NONE }, { true, 532, 270, T_NONE },
		{ false, 480, 242, T_NONE }, { false, 532, 242, T_NONE }, { false, 584, 242, T_GEAR }, { false, 558, 214, T_GEAR },
	};
	struct cfg saved = g_cfg;
	int fail = 0, i;
	memset(&w, 0, sizeof(w));
	g_cfg.notif_on = true;
	fail += ck(chip_right() == btn_cx(T_DND) - 20 - 12 && btn_cx(T_GEAR) + 20 == 604, "chip slot ends 12 px before the moon, buttons end at 604");
	for (i = T_DND; i < T_GEAR; i++) {
		fail += ck(btn_cx(i + 1) - btn_cx(i) - 40 == 12, "buttons 12 px apart");
	}
	g_cfg.notif_on = false;
	fail += ck(chip_right() == btn_cx(T_GEAR) - 32, "chip slot ends 12 px before Configuration when notifications are off");
	for (i = 0; i < (int)ARRAYSIZE(hit); i++) {
		g_cfg.notif_on = hit[i].on;
		if (target(hit[i].x, hit[i].y) != hit[i].k) {
			glog("watch: hit test %d at %.1f, %.1f gives %d, expected %d", i, hit[i].x, hit[i].y, target(hit[i].x, hit[i].y), hit[i].k);
			fail++;
		}
	}
	g_cfg.notif_on = true;
	w.card = true;
	fail += ck(target(380, LOW_Y + 52) == T_PREV && target(472, LOW_Y + 52) == T_TOGGLE && target(564, LOW_Y + 52) == T_NEXT, "media buttons below the tray");
	w.hist = true;
	w.hn = SAMPLES;
	fail += ck(target(300, 400) == T_POPUP && target(300, LOW_Y + 341) == T_NONE && target(300, 275) == T_NONE, "history popup area");
	fail += ck(input(&w.lt, LE_PASS, 300, 275) && !input(&w.lt, LE_PASS, 300, 250) && !input(&w.lt, LE_PASS, 300, 600) && input(&w.lt, LE_PASS, 300, 630), "the laser passes between and below the cards");
	fail += ck(row_at(300, LIST_Y) == 0 && row_at(300, LIST_Y + 5 * ROW_H - 0.1f) == 4 && row_at(300, LIST_Y + 5 * ROW_H) < 0 && row_at(300, LIST_Y - 0.1f) < 0, "rows under the laser");
	w.scroll = 3.5f;
	fail += ck(row_at(300, LIST_Y) == 3 && row_at(300, LIST_Y + 30) == 4, "rows follow the scroll");
	w.hn = 3;
	fail += ck(row_at(300, LIST_Y + 3 * ROW_H + 1) < 0 && hist_h(3) == 50 + 3 * ROW_H && hist_h(SAMPLES) == 340 && hist_h(0) == 104, "short lists");
	for (i = 0; i <= HIST_CAP; i++) {
		fail += ck(LOW_Y + hist_h(i) + 16 <= TH, "history popup fits in the texture");
	}
	g_cfg = saved;
	memset(&w, 0, sizeof(w));
	return fail;
}

int watch_selftest(void) {
	static const int tiers[2][MAX_TIERS] = { { 35, 25, 15 }, { 50, 30, 20 } };
	static const struct { uint8_t set; int8_t pct[4]; uint8_t chg; int lv, bolt; } col[] = {
		{ 0, { 10, 70, 80, 80 }, 0, BL_NORMAL, 0 }, { 0, { 20, 20, 20, 20 }, 0, BL_WARNING, 0 }, { 1, { 20, 20, 20, 20 }, 0, BL_CRITICAL, 0 }, { 0, { 5, 10, 15, 18 }, 0, BL_CRITICAL, 0 },
		{ 1, { 10, 30, 20, 20 }, 1, BL_CRITICAL, 1 }, { 0, { 10, 10, 10, 10 }, 15, BL_NORMAL, 1 }, { 0, { 99, 100, -1, 60 }, 3, BL_NORMAL, 2 }, { 0, { -1, -1, -1, -1 }, 0, BL_NORMAL, 0 },
	};
	static const struct { int k; bool open, trk; } hseq[] = {
		{ T_HIST, true, false }, { T_POPUP, true, false }, { T_POPUP, true, false }, { T_HIST, false, false }, { T_HIST, true, false }, { T_DND, true, false }, { T_DND, true, false },
		{ T_TRACKERS, false, true }, { T_HIST, true, false }, { T_NONE, false, false }, { T_HIST, true, false }, { -1, false, false }, { T_HIST, true, false }, { -2, false, false },
	};
	static struct bat_snap keep, b;
	struct cfg saved = g_cfg;
	int fail = 0, i, j, p, lv, chg;
	bool dnd = notif_dnd();
	keep = g_bat;
	for (i = 0; i < (int)ARRAYSIZE(col); i++) {
		g_cfg.bat_ntiers = 3;
		memcpy(g_cfg.bat_tiers, tiers[col[i].set], sizeof(g_cfg.bat_tiers));
		b = (struct bat_snap){ .ntrk = 4 };
		for (j = 0; j < 4; j++) {
			b.trk[j] = (struct bat_dev){ true, col[i].pct[j] >= 0, (col[i].chg >> j & 1) != 0, fmaxf(col[i].pct[j], 0) / 100.0f, (TrackedDeviceIndex_t)(3 + j), BR_NONE };
		}
		bat_summary(&b);
		trk_cell(&b, &p, &lv, &chg);
		if (lv != col[i].lv || chg != col[i].bolt) {
			glog("watch: trackers cell %d at %d%% is level %d bolt %d, expected %d %d", i, p, lv, chg, col[i].lv, col[i].bolt);
			fail++;
		}
	}
	g_cfg = saved;
	fail += pop_test();
	g_bat.ntrk = 6;
	g_cfg.notif_on = true;
	for (i = 0; i < (int)ARRAYSIZE(hseq); i++) {
		if (hseq[i].k == -1) {
			g_cfg.notif_on = false;
			sync_notif();
			g_cfg.notif_on = true;
		} else if (hseq[i].k == -2) {
			hidden();
		} else {
			activate(hseq[i].k);
		}
		if (w.hist != hseq[i].open || w.popup != hseq[i].trk) {
			glog("watch: history step %d (target %d) is open %d trackers %d, expected %d %d", i, hseq[i].k, w.hist, w.popup, hseq[i].open, hseq[i].trk);
			fail++;
		}
	}
	fail += ck(notif_dnd() == dnd, "two clicks on the moon give the state back");
	hidden();
	g_bat = keep;
	g_cfg = saved;
	fail += scroll_test() + dwell_test() + tray_test() + anchor_test();
	memset(&w, 0, sizeof(w));
	return fail + fit_test();
}
#endif
