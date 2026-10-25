#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "battery.h"
#include "cfg.h"
#include "gfx.h"
#include "lang.h"
#include "laser.h"
#include "main.h"
#include "notif.h"
#include "panel.h"
#include "udp.h"
#include "vr.h"
#include "winnotif.h"

#define PW 1280
#define PH 800
#define PANEL_M 0.9f
#define MPX (PANEL_M / PW)
#define GS 1.6f
#define GW 1128
#define GH 90
#define BAR_X 464.0f
#define GX_OFF ((GW / 2.0f - (BAR_X + 120) * GS) * MPX)
#define GY_OFF (-(PH / 2.0f + 16 * GS + GH / 2.0f) * MPX)
#define WHEEL 320
#define MAXHIT 96
#define OK_I 0x6FD39Au
#define OK_T 0x8FE0B0u

enum { PG_GENERAL, PG_WATCH, PG_NOTIF, PG_BATTERY, PG_COLOR };

enum { KP_NONE, KP_HTML, KP_PORT, KP_TIER };

enum { KEY_BS = 16, KEY_CLR };

enum { KS_OK, KS_EMPTY, KS_BAD, KS_TAKEN };

enum { F_T22B, F_T22, F_T20, F_T20B, F_D28, F_D32, F_D22, F_D24, F_D52, F_D20, F_D30, F_D40, F_COUNT };

enum { K_FILL, K_ACCENT, K_GHOST, K_DANGER };

enum {
	W_NONE, W_NAV, W_CLOSE = W_NAV + 4, W_BACK, W_SCRIM, W_LANG, W_LANG_OPT, W_AUTOLAUNCH = W_LANG_OPT + LANG_COUNT, W_QUIT,
	W_WRIST, W_WRIST_R, W_SIZE, W_ANGLE, W_OPACITY, W_CLOCK, W_CLOCK24, W_CHOOSE, W_MEDIA, W_WHEEL, W_BRIGHT, W_HTML, W_COLOR_RESET, W_PRESET, W_TEST_NOTIF = W_PRESET + 6,
	W_POS_RESET, W_WATCH_RESET, W_DIST, W_HEIGHT, W_HOFF, W_NSIZE, W_MIN_DEC, W_MIN_INC, W_MAX_DEC, W_MAX_INC, W_NSOUND, W_VOLUME, W_UDP, W_PORT, W_PORT_DEFAULT, W_DISCORD, W_NOTIFS,
	W_TEST_ALERT, W_ALERTS, W_BSOUND, W_TIER_ADD, W_TIER, W_TIER_DEL = W_TIER + MAX_TIERS, W_DEV = W_TIER_DEL + MAX_TIERS,
	W_KEY = W_DEV + 3, W_KEY_BS = W_KEY + KEY_BS, W_KEY_CLR, W_KP_DEF, W_KP_OK, W_KP_CLOSE
};

struct hit {
	int id;
	float x, y, w, h;
};

struct btn {
	float h, r, pl, pr, gap, isz, minw;
	int font, kind;
	bool center;
};

static const struct btn b_head = { 64, 16, 20, 20, 10, 26, 160, F_T22B, K_FILL, true };
static const struct btn b_hacc = { 64, 16, 24, 24, 12, 28, 0, F_T22B, K_ACCENT, true };
static const struct btn b_back = { 64, 16, 12, 24, 8, 28, 144, F_T22B, K_FILL, false };
static const struct btn b_ghost = { 56, 12, 16, 16, 10, 22, 0, F_T20B, K_GHOST, true };
static const struct btn b_small = { 56, 12, 16, 20, 10, 22, 0, F_T22B, K_FILL, false };
static const struct btn b_quit = { 56, 12, 20, 24, 12, 24, 0, F_T22B, K_DANGER, true };
static const uint32_t presets[6] = { 0xF5B544, 0x7CC8FF, 0xB9A6FF, 0xECE6DA, 0x8EE3C8, 0xFF9FCB };
static const WCHAR hexd[] = L"0123456789ABCDEF";
static struct ovl ovl, grab;
static struct laser_target lt_panel, lt_grab;
static struct gx_tex *tex, *gtex;
static struct gx_bmp *wheel;
static bool is_open, dd, dirty, gdirty, ghover, drag, wheel_done, wheel_ok, fresh;
static int page, hover, active, kp, nhit, wn, minute = -1, glang = -1, fnt[F_COUNT];
static WCHAR kv[8];
static float mx, my, ch, cs, cv, sec_x, sec_w, fade = 1;
static uint32_t seen_gen, gfx_gen, gacc;
static HmdMatrix34_t xf;
static const HmdMatrix34_t grab_local = { { { 1, 0, 0, GX_OFF }, { 0, 1, 0, GY_OFF }, { 0, 0, 1, 0 } } };
static struct udp_status udp;
static struct hit hits[MAXHIT];

#ifdef GL_TESTS
static const SYSTEMTIME sample_time = { 2026, 10, 5, 23, 21, 47, 7, 0 };
static bool sample;
#endif

static void fonts(void) {
	static const struct { enum gx_family f; float s; int w; } d[F_COUNT] = { [F_T22B] = { GF_TEXT, 22, 600 }, [F_T22] = { GF_TEXT, 22, 500 }, [F_T20] = { GF_TEXT, 20, 500 }, [F_T20B] = { GF_TEXT, 20, 600 }, [F_D28] = { GF_DISPLAY, 28, 600 }, [F_D32] = { GF_DISPLAY, 32, 600 }, [F_D22] = { GF_DISPLAY, 22, 600 }, [F_D24] = { GF_DISPLAY, 24, 600 }, [F_D52] = { GF_DISPLAY, 52, 600 }, [F_D20] = { GF_DISPLAY, 20, 600 }, [F_D30] = { GF_DISPLAY, 30, 600 }, [F_D40] = { GF_DISPLAY, 40, 600 } };
	int i;
	for (i = 0; i < F_COUNT; i++) {
		fnt[i] = gx_font(d[i].f, d[i].s, d[i].w);
	}
}

static struct gx_color wa(float a) {
	return (struct gx_color){ 1, 1, 1, a };
}

static struct gx_color tint(struct gx_color c, int k) {
	return k ? col_mix(c, col_rgb(k == 2 ? 0 : 0xFFFFFF), 0.12f) : c;
}

static float tw(int f, const WCHAR *s, int fl) {
	return gx_measure(fnt[f], s, 0, fl, NULL);
}

static float text(int f, const WCHAR *s, float x, float y, float w, float h, struct gx_color c, int fl) {
	return gx_text(fnt[f], s, x, y, w, h, c, fl);
}

static void hit(int id, float x, float y, float w, float h) {
	if (nhit < MAXHIT && fade == 1) {
		hits[nhit++] = (struct hit){ id, x, y, w, h };
	}
}

static const struct hit *hit_find(int id) {
	int i;
	for (i = 0; i < nhit; i++) {
		if (hits[i].id == id) return &hits[i];
	}
	return NULL;
}

static int hit_at(float x, float y) {
	const struct hit *h;
	int i;
	for (i = nhit - 1; i >= 0; i--) {
		h = &hits[i];
		if (x >= h->x && y >= h->y && x < h->x + h->w && y < h->y + h->h) return h->id;
	}
	return W_NONE;
}

static int st(int id) {
	return !id ? 0 : active == id ? 2 : !active && hover == id;
}

static int range_of(int id) {
	static const struct { int id, r; } sliders[] = { { W_SIZE, CR_WATCH_SIZE }, { W_ANGLE, CR_WATCH_ANGLE }, { W_OPACITY, CR_WATCH_OPACITY }, { W_DIST, CR_NOTIF_DIST }, { W_HEIGHT, CR_NOTIF_ANGLE }, { W_HOFF, CR_NOTIF_HOFF }, { W_NSIZE, CR_NOTIF_SIZE }, { W_VOLUME, CR_VOLUME } };
	int i;
	for (i = 0; i < (int)ARRAYSIZE(sliders); i++) {
		if (sliders[i].id == id) return sliders[i].r;
	}
	return -1;
}

static bool is_slider(int id) {
	return id == W_BRIGHT || id == W_WHEEL || range_of(id) >= 0;
}

static void now_time(SYSTEMTIME *t) {
	GetLocalTime(t);
#ifdef GL_TESTS
	if (sample) {
		*t = sample_time;
	}
#endif
	minute = t->wMinute;
}

static void read_status(void) {
#ifdef GL_TESTS
	if (sample) return;
#endif
	udp_status(&udp);
	wn = winnotif_status();
}

static void dim(bool on) {
	if (on == (fade < 1)) return;
	fade = on ? 0.4f : 1;
	if (on) {
		gx_layer_push(fade);
	} else {
		gx_layer_pop();
	}
}

static void changed(void) {
	cfg_changed();
	dirty = true;
}

static void set_i(int *p, int v) {
	if (*p == v) return;
	*p = v;
	changed();
}

static void set_f(float *p, float v) {
	if (*p == v) return;
	*p = v;
	changed();
}

static void set_r(int r, float v) {
	if (cfg_set(r, v)) {
		changed();
	}
}

static void flip(bool *b) {
	*b = !*b;
	changed();
}

static void set_accent(uint32_t c) {
	if (g_cfg.accent == c) return;
	g_cfg.accent = c;
	changed();
}

static void sync_hsv(void) {
	col_to_hsv(g_cfg.accent, &ch, &cs, &cv);
}

static bool bake(void) {
	const float c = WHEEL / 2.0f;
	uint8_t *p, *q;
	float dx, dy, r, a, h;
	uint32_t k;
	int x, y;
	bool ok;
	if (!wheel) {
		wheel = gx_bmp_new(WHEEL, WHEEL);
	}
	p = wheel ? malloc(WHEEL * WHEEL * 4) : NULL;
	if (!p) return false;
	for (y = 0; y < WHEEL; y++) {
		for (x = 0; x < WHEEL; x++) {
			dx = x + 0.5f - c;
			dy = y + 0.5f - c;
			r = sqrtf(dx * dx + dy * dy);
			a = fmaxf(0, fminf(1, c + 0.5f - r));
			h = atan2f(dx, -dy) * (180 / PI_F);
			k = col_hsv(h < 0 ? h + 360 : h, fminf(r / c, 1), 1);
			q = p + (y * WHEEL + x) * 4;
			q[0] = (uint8_t)((k & 255) * a + 0.5f);
			q[1] = (uint8_t)((k >> 8 & 255) * a + 0.5f);
			q[2] = (uint8_t)((k >> 16 & 255) * a + 0.5f);
			q[3] = (uint8_t)(255 * a + 0.5f);
		}
	}
	ok = gx_bmp_set(wheel, p, WHEEL * 4);
	free(p);
	return ok;
}

static float btn(int id, const struct btn *b, float x, float y, enum gx_icon ic, float isw, const WCHAR *s, bool right) {
	float t = tw(b->font, s, 0), cw = b->isz + b->gap + t, w = fmaxf(b->minw, b->pl + cw + b->pr), cx;
	struct gx_color fg;
	int k;
	x = right ? x - w : x;
	k = st(id);
	fg = col_rgb(C_TEXT);
	if (b->kind == K_ACCENT) {
		gx_fill_rrect(x, y, w, b->h, b->r, tint(col_accent(), k));
		fg = col_on(g_cfg.accent);
	} else if (b->kind == K_DANGER) {
		gx_fill_rrect(x, y, w, b->h, b->r, col_rgba(C_CRIT, 0.12f + 0.08f * k));
		gx_stroke_rrect(x, y, w, b->h, b->r, 2, col_rgb(C_CRIT));
		fg = col_rgb(C_CRIT_T);
	} else if (b->kind == K_GHOST) {
		gx_fill_rrect(x, y, w, b->h, b->r, wa(0.07f * k));
		fg = col_rgb(C_SOFT);
	} else {
		gx_fill_rrect(x, y, w, b->h, b->r, wa(0.1f + 0.06f * k));
	}
	cx = b->center ? x + (w - cw) / 2 : x + b->pl;
	gx_icon(ic, cx, y + (b->h - b->isz) / 2, b->isz, isw, fg);
	text(b->font, s, cx + b->isz + b->gap, y, 0, b->h, fg, 0);
	hit(id, x, y, w, b->h);
	return w;
}

static void sw(int id, float x, float y, bool on) {
	int k = st(id);
	if (on) {
		gx_fill_rrect(x + 6, y + 6, 76, 44, 22, tint(col_accent(), k));
		gx_fill_circle(x + 60, y + 28, 18, col_on(g_cfg.accent));
		gx_icon(IC_CHECK, x + 50, y + 18, 20, 3, col_accent());
	} else {
		gx_fill_rrect(x + 6, y + 6, 76, 44, 22, wa(0.06f + 0.05f * k));
		gx_stroke_rrect(x + 6, y + 6, 76, 44, 22, 2, wa(0.4f));
		gx_fill_circle(x + 28, y + 28, 14, col_rgb(C_DIM));
	}
	hit(id, x, y, 88, 56);
}

static void seg(int id, float y, const WCHAR *a, const WCHAR *b, int sel) {
	struct gx_color on;
	const WCHAR *s;
	float x = 928, bx, cx;
	int i, k;
	gx_fill_rrect(x, y, 288, 56, 14, wa(0.08f));
	for (i = 0; i < 2; i++) {
		bx = x + i * 144;
		s = i ? b : a;
		k = st(id + i);
		if (i == sel) {
			on = col_on(g_cfg.accent);
			cx = bx + (144 - 28 - tw(F_T22B, s, 0)) / 2;
			gx_fill_rrect(bx, y, 144, 56, 14, tint(col_accent(), k));
			gx_icon(IC_CHECK, cx, y + 18, 20, 3, on);
			text(F_T22B, s, cx + 28, y, 0, 56, on, 0);
		} else {
			gx_fill_rrect(bx, y, 144, 56, 14, wa(0.06f * k));
			text(F_T22, s, bx, y, 144, 56, col_rgb(C_SOFT), GT_CENTER);
		}
		hit(id + i, bx, y, 144, 56);
	}
}

static void thumb(int id, float x, float y) {
	gx_fill_circle(x, y, 26, wa(0.08f * st(id)));
	gx_fill_circle(x, y, 18, col_rgb(C_INK));
	gx_fill_circle(x, y, 15, col_rgb(C_TEXT));
}

static void slider(int id, float x, float y, float w, bool mid) {
	float t = fmaxf(0, fminf(1, cfg_norm(range_of(id)))), p, a, b;
	p = x + t * w;
	a = mid ? fminf(p, x + w / 2) : x;
	b = mid ? fmaxf(p, x + w / 2) : p;
	gx_fill_rrect(x, y + 24, w, 8, 4, wa(0.12f));
	if (b - a >= 1) {
		gx_fill_rrect(a, y + 24, b - a, 8, 4, col_accent());
	}
	thumb(id, p, y + 28);
	hit(id, x - 28, y, w + 56, 56);
}

static void stepper(int id, float y, const WCHAR *val) {
	float x = 1016;
	int i;
	gx_fill_rrect(x, y, 200, 56, 14, wa(0.06f));
	for (i = 0; i < 2; i++) {
		gx_fill_rrect(x + i * 144, y, 56, 56, 14, wa(0.1f + 0.06f * st(id + i)));
		gx_icon(i ? IC_PLUS : IC_MINUS, x + i * 144 + 16, y + 16, 24, 2.4f, col_rgb(C_TEXT));
		hit(id + i, x + i * 144, y, 56, 56);
	}
	text(F_D24, val, x + 56, y, 88, 56, col_rgb(C_TEXT), GT_CENTER | GT_TNUM);
}

static void field(int id, float x, float y, float w, bool focus) {
	gx_fill_rrect(x, y, w, 56, 12, (struct gx_color){ 0, 0, 0, 0.3f });
	gx_stroke_rrect(x, y, w, 56, 12, 2, focus ? col_accent() : wa(0.2f + 0.1f * st(id)));
	hit(id, x, y, w, 56);
}

static void swatch(float x, float y, float w, float h, float r, uint32_t c) {
	gx_fill_rrect(x - 2, y - 2, w + 4, h + 4, r + 2, wa(0.24f));
	gx_fill_rrect(x, y, w, h, r, col_rgb(c));
}

static void status(float r, float y, enum gx_icon ic, uint32_t icc, int f, const WCHAR *s, uint32_t sc, const WCHAR *extra) {
	float t = tw(f, s, 0), x = r - 28 - t - (extra ? tw(F_D20, extra, GT_TNUM) + 8 : 0);
	gx_icon(ic, x, y + 18, 20, f == F_T20B ? 2.4f : 2.2f, col_rgb(icc));
	text(f, s, x + 28, y, 0, 56, col_rgb(sc), 0);
	if (extra) {
		text(F_D20, extra, x + 36 + t, y, 0, 56, col_rgb(C_TEXT), GT_TNUM);
	}
}

static float sec(float x, float y, float w, float rows, enum str_id t) {
	bool f = fade < 1;
	sec_x = x;
	sec_w = w;
	dim(false);
	gx_fill_rrect(x, y, w, rows + 64, 16, wa(0.06f));
	dim(f);
	text(F_T20B, tr(t), x + 24, y, w - 40, 56, col_rgb(C_DIM), GT_ELLIPSIS);
	return y + 56;
}

static void line(float y) {
	gx_fill_rect(sec_x, y, sec_w, 2, wa(0.06f));
}

static void label(float y, float h, enum str_id t, const WCHAR *sub, float right) {
	float x = sec_x + 24, w = right - 16 - x, ty;
	line(y);
	if (sub) {
		ty = y + 2 + (h - 54) / 2;
		text(F_T22B, tr(t), x, ty, w, 28, col_rgb(C_TEXT), GT_ELLIPSIS);
		text(F_T20, sub, x, ty + 28, w, 24, col_rgb(C_DIM), GT_ELLIPSIS);
	} else {
		text(F_T22B, tr(t), x, y + 2, w, h - 2, col_rgb(C_TEXT), GT_ELLIPSIS);
	}
}

static void sblock(int id, float y, enum str_id t, const WCHAR *val, bool mid) {
	float vw = tw(F_D22, val, GT_TNUM), r = sec_x + sec_w - 24;
	line(y);
	text(F_T22B, tr(t), sec_x + 24, y + 2, sec_w - 64 - vw, 28, col_rgb(C_TEXT), GT_ELLIPSIS);
	text(F_D22, val, r - vw, y + 2, 0, 28, col_rgb(C_TEXT), GT_TNUM);
	slider(id, sec_x + 24, y + 24, sec_w - 48, mid);
}

static void srow(int id, float y, enum str_id t, const WCHAR *sub, const WCHAR *val) {
	text(F_D22, val, 1216 - tw(F_D22, val, GT_TNUM), y + 2, 0, 70, col_rgb(C_TEXT), GT_TNUM);
	slider(id, 808, y + 9, 320, false);
	label(y, 72, t, sub, 808);
}

static float hdr_close(void) {
	return 1232 - btn(W_CLOSE, &b_head, 1232, 24, IC_CLOSE, 2.4f, tr(S_P_CLOSE), true) - 16;
}

static void hdr_title(enum str_id t, float r) {
	float l = 384;
	if (page == PG_COLOR) {
		l += btn(W_BACK, &b_back, l, 24, IC_CHEVRON_LEFT, 2.4f, tr(S_P_WATCH), false) + 16;
	}
	text(F_D32, tr(t), l, 24, r - l, 64, col_rgb(C_TEXT), GT_ELLIPSIS);
}

static void chrome(void) {
	static const enum gx_icon ic[4] = { IC_SLIDERS, IC_WATCH, IC_BELL, IC_BATTERY };
	static const enum str_id nm[4] = { S_P_GENERAL, S_P_WATCH, S_P_NOTIFICATIONS, S_P_BATTERY };
	int cur = page == PG_COLOR ? PG_WATCH : page, i;
	float y;
	bool on;
	gx_fill_rrect(0, 0, PW, PH, 24, col_rgba(C_CARD, 0.95f));
	gx_stroke_rrect(0, 0, PW, PH, 24, 2, wa(0.07f));
	gx_clip_push(0, 0, 336, PH);
	gx_fill_rrect(0, 0, 360, PH, 24, wa(0.04f));
	gx_clip_pop();
	gx_fill_rect(334, 0, 2, PH, wa(0.06f));
	gx_fill_rrect(24, 32, 56, 56, 16, col_accent_soft());
	gx_icon(IC_GHOST, 36, 44, 32, 2, col_accent());
	text(F_D28, tr(S_APP_NAME), 92, 32, 220, 32, col_rgb(C_TEXT), GT_ELLIPSIS);
	text(F_T20, tr(S_CONFIGURATION), 92, 64, 220, 24, col_rgb(C_DIM), GT_ELLIPSIS);
	for (i = 0; i < 4; i++) {
		y = 120 + i * 80;
		on = i == cur;
		gx_fill_rrect(24, y, 288, 72, 16, wa(on ? 0.12f : 0.06f * st(W_NAV + i)));
		if (on) {
			gx_fill_rrect(24, y + 20, 6, 32, 3, col_accent());
		}
		gx_icon(ic[i], 48, y + 22, 28, 2.2f, col_rgb(on ? C_TEXT : C_DIM));
		text(on ? F_T22B : F_T22, tr(nm[i]), 92, y, 204, 72, col_rgb(on ? C_TEXT : C_SOFT), GT_ELLIPSIS);
		hit(W_NAV + i, 24, y, 288, 72);
	}
}

static void lang_btn(float x, float y) {
	field(W_LANG, x, y, 384, dd);
	gx_icon(IC_GLOBE, x + 20, y + 16, 24, 2.2f, col_rgb(C_SOFT));
	text(F_T22B, lang_native_name(g_cfg.lang), x + 56, y, 276, 56, col_rgb(C_TEXT), GT_ELLIPSIS);
	gx_icon(dd ? IC_CHEVRON_UP : IC_CHEVRON_DOWN, x + 344, y + 16, 24, 2.6f, dd ? col_accent() : col_rgb(C_SOFT));
}

static void dropdown(float x, float y) {
	float h = LANG_COUNT * 68 + 12, oy;
	bool on;
	int i;
	hit(W_SCRIM, 0, 0, PW, PH);
	gx_shadow_rrect(x, y, 384, h, 16, 48, 24, (struct gx_color){ 0, 0, 0, 0.55f });
	gx_fill_rrect(x - 2, y - 2, 388, h + 4, 18, wa(0.12f));
	gx_fill_rrect(x, y, 384, h, 16, col_rgb(0x24272E));
	for (i = 0; i < LANG_COUNT; i++) {
		oy = y + 8 + i * 68;
		on = i == g_cfg.lang;
		gx_fill_rrect(x + 8, oy, 368, 64, 12, wa(on ? 0.1f : 0.06f * st(W_LANG_OPT + i)));
		if (on) {
			gx_icon(IC_CHECK, x + 24, oy + 20, 24, 3, col_accent());
		}
		text(on ? F_T22B : F_T22, lang_native_name(i), x + 64, oy, 296, 64, col_rgb(on ? C_TEXT : C_SOFT), GT_ELLIPSIS);
		hit(W_LANG_OPT + i, x + 8, oy, 368, 64);
	}
}

static void pg_general(void) {
	float y, vw, qx;
	WCHAR version[48];
	swprintf(version, ARRAYSIZE(version), L"Ghostlay %hs%hs", VERSION_PREFIX, VERSION);
	hdr_title(S_P_GENERAL, hdr_close());
	y = sec(384, 112, 848, 144, S_G_PREFERENCES);
	vw = tw(F_D22, version, GT_TNUM);
	lang_btn(832, y + 9);
	label(y, 72, S_G_LANGUAGE, tr(S_G_LANGUAGE_SUB), 832);
	sw(W_AUTOLAUNCH, 1128, y + 81, g_cfg.autolaunch);
	label(y + 72, 72, S_G_AUTOLAUNCH, tr(S_G_AUTOLAUNCH_SUB), 1128);
	y = sec(384, 344, 848, 144, S_G_ABOUT);
	text(F_D22, version, 1216 - vw, y + 2, 0, 70, col_rgb(C_SOFT), GT_TNUM);
	label(y, 72, S_G_VERSION, NULL, 1216 - vw);
	qx = 1216 - btn(W_QUIT, &b_quit, 1216, y + 81, IC_POWER, 2.4f, tr(S_G_QUIT), true);
	label(y + 72, 72, S_G_QUIT_SECTION, tr(S_G_QUIT_SUB), qx);
	if (dd) {
		dropdown(832, 240);
	}
}

static float choose(float r, float y) {
	const WCHAR *s = tr(S_WP_CHOOSE);
	float t = tw(F_T22B, s, 0), w = fmaxf(184, t + 100), x = r - w;
	gx_fill_rrect(x, y, w, 56, 12, wa(0.1f + 0.06f * st(W_CHOOSE)));
	gx_icon(IC_PALETTE, x + 20, y + 16, 24, 2.2f, col_rgb(C_TEXT));
	text(F_T22B, s, x + 54, y, 0, 56, col_rgb(C_TEXT), 0);
	gx_icon(IC_CHEVRON_RIGHT, x + w - 36, y + 18, 20, 2.6f, col_rgb(C_DIM));
	hit(W_CHOOSE, x, y, w, 56);
	return x;
}

static void pg_watch(void) {
	WCHAR b[64], tb[24], hx[8];
	SYSTEMTIME t;
	float y, hw, hx0;
	int k;
	hdr_title(S_P_WATCH, hdr_close());
	y = sec(384, 112, 848, 216, S_WP_PLACEMENT);
	btn(W_WATCH_RESET, &b_ghost, 1224, 112, IC_RESET, 2.2f, tr(S_NP_RESET_POS), true);
	seg(W_WRIST, y + 9, tr(S_W_LEFT), tr(S_W_RIGHT), g_cfg.wrist);
	label(y, 72, S_WP_WRIST, tr(S_WP_WRIST_SUB), 928);
	fmt_num(b, 64, g_cfg.watch_size, 0, L"%");
	srow(W_SIZE, y + 72, S_WP_SIZE, tr(S_WP_SIZE_SUB), b);
	fmt_num(b, 64, g_cfg.watch_angle, 0, L"°");
	srow(W_ANGLE, y + 144, S_WP_ANGLE, tr(S_WP_ANGLE_SUB), b);
	y = sec(384, 416, 848, 288, S_WP_DISPLAY);
	seg(W_CLOCK, y + 9, L"12 h", L"24 h", g_cfg.clock24);
	now_time(&t);
	k = fmt_time(tb, 24, &t, g_cfg.clock24);
	if (!g_cfg.clock24) {
		swprintf(tb + k, 24 - k, L" %ls", fmt_ampm(&t));
	}
	swprintf(b, 64, tr(S_WP_PREVIEW), tb);
	label(y, 72, S_WP_TIME_FORMAT, b, 928);
	col_format(hx);
	hw = tw(F_D22, hx, GT_TNUM);
	hx0 = choose(1216, y + 81) - 16 - hw;
	text(F_D22, hx, hx0, y + 74, 0, 70, col_rgb(C_TEXT), GT_TNUM);
	swatch(hx0 - 52, y + 89, 40, 40, 8, g_cfg.accent);
	label(y + 72, 72, S_WP_ACCENT, tr(S_WP_ACCENT_SUB), hx0 - 52);
	fmt_num(b, 64, g_cfg.watch_opacity, 0, L"%");
	srow(W_OPACITY, y + 144, S_WP_OPACITY, tr(S_WP_OPACITY_SUB), b);
	sw(W_MEDIA, 1128, y + 225, g_cfg.media);
	label(y + 216, 72, S_WP_MEDIA, tr(S_WP_MEDIA_SUB), 1128);
}

static void preview(float x, float y) {
	SYSTEMTIME t;
	WCHAR b[16], d[48], *c;
	float w;
	now_time(&t);
	fmt_time(b, 16, &t, g_cfg.clock24);
	c = wcschr(b, L':');
	if (c) {
		*c = 0;
	}
	w = text(F_D52, b, x, y, 0, 56, col_rgb(C_TEXT), GT_TNUM);
	w += text(F_D52, L":", x + w, y, 0, 56, col_accent(), GT_TNUM);
	w += text(F_D52, c ? c + 1 : L"", x + w, y, 0, 56, col_rgb(C_TEXT), GT_TNUM);
	if (!g_cfg.clock24) {
		text(F_D24, fmt_ampm(&t), x + w + 8, y + 23.5f, 0, 32, col_rgb(C_SOFT), 0);
	}
	fmt_date_short(d, 48, &t);
	text(F_T20, d, x, y + 60, 240, 24, col_rgb(C_DIM), GT_ELLIPSIS);
	gx_fill_circle(x + 22, y + 122, 22, col_accent());
	gx_icon(IC_PLAY, x + 14, y + 112, 20, 2, col_on(g_cfg.accent));
	gx_fill_rrect(x + 56, y + 118, 184, 8, 4, wa(0.12f));
	gx_clip_push(x + 56, y + 118, 184 * 0.4f, 8);
	gx_fill_rrect(x + 56, y + 118, 184, 8, 4, col_accent());
	gx_clip_pop();
}

static void pg_color(void) {
	WCHAR b[16], hx[8];
	struct gx_stop gs[2];
	float y, a, tx, ty, vw, contrast, cx, cy;
	int i, k;
	hdr_title(S_WP_ACCENT, hdr_close());
	y = sec(384, 112, 392, 452, S_C_WHEEL);
	a = ch * DEG;
	tx = 580 + sinf(a) * cs * 160;
	ty = 352 - cosf(a) * cs * 160;
	line(y);
	gx_fill_circle(580, 352, 162, wa(0.12f));
	if (wheel_ok) {
		gx_bmp_draw(wheel, 420, 192, WHEEL, WHEEL, 0);
	}
	gx_fill_circle(tx, ty, 32, wa(0.1f * st(W_WHEEL)));
	gx_fill_circle(tx, ty, 24, col_rgb(C_INK));
	gx_fill_circle(tx, ty, 22, col_rgb(C_TEXT));
	gx_fill_circle(tx, ty, 18, col_rgb(C_INK));
	gx_fill_circle(tx, ty, 16, col_accent());
	hit(W_WHEEL, 420, 192, WHEEL, WHEEL);
	y += 366;
	line(y);
	fmt_num(b, 16, roundf(cv * 100), 0, L"%");
	vw = tw(F_D22, b, GT_TNUM);
	text(F_T22B, tr(S_C_BRIGHTNESS), 408, y + 2, 328 - vw, 28, col_rgb(C_TEXT), GT_ELLIPSIS);
	text(F_D22, b, 752 - vw, y + 2, 0, 28, col_rgb(C_TEXT), GT_TNUM);
	gs[0] = (struct gx_stop){ 0, col_rgb(0) };
	gs[1] = (struct gx_stop){ 1, col_rgb(col_hsv(ch, cs, 1)) };
	gx_grad_rrect(408, y + 48, 344, 20, 10, 408, 0, 752, 0, gs, 2);
	gx_stroke_rrect(408, y + 48, 344, 20, 10, 2, wa(0.12f));
	thumb(W_BRIGHT, 408 + cv * 344, y + 58);
	hit(W_BRIGHT, 380, y + 30, 400, 56);
	contrast = (col_luma(g_cfg.accent) + 0.05f) / (col_luma(C_CARD) + 0.05f);
	y = sec(800, 112, 432, 274, S_C_SELECTED);
	if (contrast >= 3) {
		status(1216, 112, IC_CHECK_CIRCLE, OK_I, F_T20B, tr(S_C_READABLE), OK_T, NULL);
	} else {
		status(1216, 112, IC_WARNING, C_WARN, F_T20B, tr(S_C_LOW_CONTRAST), C_WARN, NULL);
	}
	line(y);
	swatch(824, y + 18, 96, 168, 16, g_cfg.accent);
	gx_fill_rrect(936, y + 18, 272, 168, 16, col_rgb(C_CARD));
	gx_stroke_rrect(936, y + 18, 272, 168, 16, 2, wa(0.07f));
	preview(952, y + 30);
	y += 202;
	col_format(hx);
	field(W_HTML, 1000, y + 9, 216, kp == KP_HTML);
	gx_fill_rrect(1012, y + 21, 32, 32, 8, col_accent());
	text(F_D24, hx, 1056, y + 9, 0, 56, col_rgb(C_TEXT), GT_TNUM);
	gx_icon(IC_PENCIL, 1176, y + 25, 24, 2.2f, col_rgb(C_SOFT));
	label(y, 72, S_C_HTML, NULL, 1000);
	y = sec(800, 474, 432, 90, S_C_PRESETS);
	btn(W_COLOR_RESET, &b_ghost, 1224, 474, IC_RESET, 2.2f, tr(S_C_RESET), true);
	line(y);
	for (i = 0; i < 6; i++) {
		cx = 852 + i * 64;
		cy = y + 46;
		k = st(W_PRESET + i);
		if (presets[i] == g_cfg.accent) {
			gx_fill_circle(cx, cy, 28, col_rgb(C_TEXT));
			gx_fill_circle(cx, cy, 25, col_rgb(C_INK));
			gx_fill_circle(cx, cy, 22, col_rgb(presets[i]));
			gx_icon(IC_CHECK, cx - 12, cy - 12, 24, 3, col_on(presets[i]));
		} else {
			gx_fill_circle(cx, cy, 28 + 2.0f * k, wa(0.3f * k));
			gx_fill_circle(cx, cy, 28, col_rgb(presets[i]));
		}
		hit(W_PRESET + i, cx - 28, cy - 28, 56, 56);
	}
}

static void udp_line(float r, float y) {
	WCHAR a[128], ex[32];
	switch (g_cfg.notif_on ? udp.state : UDP_OFF) {
	case UDP_LISTENING:
		swprintf(ex, 32, L"127.0.0.1:%d", udp.port);
		status(r, y, IC_CHECK_CIRCLE, OK_I, F_T20B, tr(S_NP_LISTENING), OK_T, ex);
		break;
	case UDP_IN_USE:
		swprintf(a, ARRAYSIZE(a), tr(S_NP_PORT_IN_USE), udp.owner);
		status(r, y, IC_WARNING, C_WARN, F_T20B, udp.owner[0] ? a : tr(S_NP_UDP_ERROR), C_WARN, NULL);
		break;
	case UDP_ERROR:
		status(r, y, IC_ALERT, C_CRIT, F_T20B, tr(S_NP_UDP_ERROR), C_CRIT_T, NULL);
		break;
	case UDP_STARTING:
		status(r, y, IC_INFO, C_DIM, F_T20B, tr(S_NP_STARTING), C_DIM, NULL);
		break;
	default:
		status(r, y, IC_INFO, C_DIM, F_T20B, tr(S_NP_UDP_OFF), C_DIM, NULL);
		break;
	}
}

static void discord_line(float y) {
	float x = sec_x + 24, ty = y + 7;
	const WCHAR *s = tr(S_NP_DISCORD_SUB);
	enum gx_icon ic = IC_COUNT;
	uint32_t icc = C_DIM, tc = C_DIM;
	line(y);
	text(F_T22B, tr(S_NP_DISCORD), x, ty, 1112 - x, 28, col_rgb(C_TEXT), GT_ELLIPSIS);
	switch (g_cfg.notif_on ? wn : WN_OFF) {
	case WN_STARTING:
		ic = IC_INFO;
		s = tr(S_NP_STARTING);
		break;
	case WN_ACTIVE:
		ic = IC_CHECK_CIRCLE;
		icc = OK_I;
		tc = OK_T;
		s = tr(S_NP_DISCORD_ACTIVE);
		break;
	case WN_BLOCKED:
		ic = IC_WARNING;
		icc = tc = C_WARN;
		s = tr(S_NP_DISCORD_BLOCKED);
		break;
	case WN_UNAVAILABLE:
		ic = IC_ALERT;
		icc = C_CRIT;
		tc = C_CRIT_T;
		s = tr(S_NP_DISCORD_UNAVAILABLE);
		break;
	}
	if (ic < IC_COUNT) {
		gx_icon(ic, x, ty + 30, 20, 2.4f, col_rgb(icc));
		x += 28;
	}
	text(ic < IC_COUNT ? F_T20B : F_T20, s, x, ty + 28, 1112 - x, 24, col_rgb(tc), GT_ELLIPSIS);
}

static void pg_notif(void) {
	WCHAR b[32];
	float r = hdr_close(), y, px;
	bool off = !g_cfg.notif_on;
	dim(off);
	r -= btn(W_TEST_NOTIF, &b_hacc, r, 24, IC_BELL, 2.4f, tr(S_NP_SEND_TEST), true) + 16;
	dim(false);
	hdr_title(S_P_NOTIFICATIONS, r - 104);
	sw(W_NOTIFS, 400 + fminf(tw(F_D32, tr(S_P_NOTIFICATIONS), 0), r - 488), 28, g_cfg.notif_on);
	dim(off);
	y = sec(384, 112, 412, 320, S_NP_POSITION);
	btn(W_POS_RESET, &b_ghost, 788, 112, IC_RESET, 2.2f, tr(S_NP_RESET_POS), true);
	fmt_num(b, 32, g_cfg.notif_dist, 2, L"m");
	sblock(W_DIST, y, S_NP_DISTANCE, b, false);
	fmt_num(b, 32, g_cfg.notif_angle, 0, L"°");
	sblock(W_HEIGHT, y + 80, S_NP_HEIGHT, b, false);
	fmt_num(b, 32, roundf(g_cfg.notif_hoff * 100), 0, L"cm");
	sblock(W_HOFF, y + 160, S_NP_HOFF, b, true);
	fmt_num(b, 32, g_cfg.notif_size, 0, L"%");
	sblock(W_NSIZE, y + 240, S_NP_SIZE, b, false);
	y = sec(820, 112, 412, 320, S_NP_DURATION_SOUND);
	fmt_num(b, 32, g_cfg.dur_min, 0, L"s");
	stepper(W_MIN_DEC, y + 13, b);
	label(y, 80, S_NP_MIN, NULL, 1016);
	fmt_num(b, 32, g_cfg.dur_max, 0, L"s");
	stepper(W_MAX_DEC, y + 93, b);
	label(y + 80, 80, S_NP_MAX, NULL, 1016);
	sw(W_NSOUND, 1128, y + 173, g_cfg.notif_sound);
	label(y + 160, 80, S_NP_NOTIF_SOUND, NULL, 1128);
	fmt_num(b, 32, g_cfg.notif_volume, 0, L"%");
	text(F_D22, b, 1216 - tw(F_D22, b, GT_TNUM), y + 242, 0, 78, col_rgb(C_TEXT), GT_TNUM);
	slider(W_VOLUME, 960, y + 253, 168, false);
	label(y + 240, 80, S_NP_VOLUME, NULL, 960);
	y = sec(384, 520, 848, 192, S_NP_EXTERNAL);
	udp_line(1216, 520);
	sw(W_UDP, 1128, y + 5, g_cfg.udp_on);
	label(y, 64, S_NP_RECEIVE, tr(S_NP_RECEIVE_SUB), 1128);
	px = 1216 - btn(W_PORT_DEFAULT, &b_small, 1216, y + 69, IC_RESET, 2.2f, tr(S_G_DEFAULT), true) - 176;
	swprintf(b, 32, L"%d", g_cfg.udp_port);
	field(W_PORT, px, y + 69, 160, kp == KP_PORT);
	text(F_D24, b, px + 20, y + 69, 0, 56, col_rgb(C_TEXT), GT_TNUM);
	gx_icon(IC_PENCIL, px + 120, y + 85, 24, 2.2f, col_rgb(C_SOFT));
	label(y + 64, 64, S_NP_PORT, tr(S_NP_PORT_SUB), px);
	sw(W_DISCORD, 1128, y + 133, g_cfg.discord);
	discord_line(y + 128);
	dim(false);
}

static void dashes(float x, float y, float len, bool vert, struct gx_color c) {
	int n = (int)fmaxf(1, roundf(len / 12)), i;
	float s = len / n, p;
	for (i = 0; i < n; i++) {
		p = i * s + (s - 6) / 2;
		gx_fill_rect(vert ? x : x + p, vert ? y + p : y, vert ? 2 : 6, vert ? 6 : 2, c);
	}
}

static void dashed(float x, float y, float w, float h, float r, struct gx_color c) {
	gx_arc(x + r, y + r, r - 1, 2, 270, 90, c);
	gx_arc(x + w - r, y + r, r - 1, 2, 0, 90, c);
	gx_arc(x + w - r, y + h - r, r - 1, 2, 90, 90, c);
	gx_arc(x + r, y + h - r, r - 1, 2, 180, 90, c);
	dashes(x + r, y, w - 2 * r, false, c);
	dashes(x + r, y + h - 2, w - 2 * r, false, c);
	dashes(x, y + r, h - 2 * r, true, c);
	dashes(x + w - 2, y + r, h - 2 * r, true, c);
}

static float chip(int i, float x, float y) {
	WCHAR b[16];
	bool crit = i == g_cfg.bat_ntiers - 1, one = g_cfg.bat_ntiers < 2, ed = kp == KP_TIER + i;
	uint32_t c = crit ? C_CRIT : C_WARN;
	float t, ew, w;
	fmt_num(b, 16, g_cfg.bat_tiers[i], 0, L"%");
	t = tw(F_D22, b, GT_TNUM);
	ew = 48 + t;
	w = ew + 56;
	gx_fill_rrect(x, y, w, 56, 8, col_rgba(c, crit ? 0.12f : 0.1f));
	gx_fill_rrect(x, y, ew, 56, 8, wa(0.06f * st(W_TIER + i)));
	gx_fill_rrect(x + ew, y, 56, 56, 8, wa(one ? 0 : 0.06f * st(W_TIER_DEL + i)));
	gx_stroke_rrect(x, y, w, 56, 8, ed ? 3 : 2, ed ? col_rgb(C_TEXT) : col_rgb(c));
	gx_icon(crit ? IC_ALERT : IC_WARNING, x + 16, y + 18, 20, 2.4f, col_rgb(c));
	text(F_D22, b, x + 44, y, 0, 56, col_rgb(C_TEXT), GT_TNUM);
	gx_icon(IC_CLOSE, x + ew + 18, y + 18, 20, 2.4f, col_rgba(C_SOFT, one ? 0.3f : 1));
	hit(W_TIER + i, x, y, ew, 56);
	if (!one) {
		hit(W_TIER_DEL + i, x + ew, y, 56, 56);
	}
	return w;
}

static void add_btn(float x, float y) {
	const WCHAR *s = tr(S_BP_ADD);
	float t = tw(F_T22B, s, 0), w = t + 70;
	bool compact = x + w > 1216;
	w = compact ? 56 : w;
	gx_fill_rrect(x, y, w, 56, 8, wa(0.06f * st(W_TIER_ADD)));
	dashed(x, y, w, 56, 8, kp == KP_TIER + g_cfg.bat_ntiers ? col_accent() : wa(0.4f));
	gx_icon(IC_PLUS, compact ? x + 17 : x + 20, y + 17, 22, 2.4f, col_rgb(C_SOFT));
	if (!compact) {
		text(F_T22B, s, x + 50, y, 0, 56, col_rgb(C_SOFT), 0);
	}
	hit(W_TIER_ADD, x, y, w, 56);
}

static void legend(float x, float y, float w, enum gx_icon ic, uint32_t icc, enum str_id t, uint32_t tc, const WCHAR *sub) {
	gx_icon(ic, x, y + 25, 24, 2.4f, col_rgb(icc));
	text(F_T22B, tr(t), x + 36, y + 11, w - 36, 28, col_rgb(tc), GT_ELLIPSIS);
	text(F_T20, sub, x + 36, y + 39, w - 36, 24, col_rgb(C_DIM), GT_ELLIPSIS);
}

static void pg_battery(void) {
	static const enum gx_icon di[3] = { IC_HEADSET, IC_CONTROLLER, IC_TRACKER };
	static const enum str_id dn[3] = { S_W_HEADSET, S_BP_CONTROLLERS, S_W_TRACKERS };
	WCHAR pb[16], ab[64];
	const WCHAR *s = tr(S_BP_NOTIFS_OFF);
	float r = hdr_close(), y, x, cw, w[3], sum = 0;
	bool off = !g_cfg.notif_on, on;
	int i;
	dim(off);
	r -= btn(W_TEST_ALERT, &b_hacc, r, 24, IC_BATTERY, 2.2f, tr(S_BP_TEST), true) + 16;
	dim(false);
	hdr_title(S_P_BATTERY, r);
	dim(off);
	y = sec(384, 112, 848, 144, S_BP_ALERTS);
	sw(W_ALERTS, 1128, y + 9, g_cfg.bat_alerts);
	label(y, 72, S_P_BATTERY, tr(S_BP_ALERTS_SUB), 1128);
	sw(W_BSOUND, 1128, y + 81, g_cfg.bat_sound);
	label(y + 72, 72, S_BP_SOUND, tr(S_BP_SOUND_SUB), 1128);
	dim(false);
	if (off) {
		x = 1184 - tw(F_T20B, s, 0);
		gx_icon(IC_BELL_OFF, x, 129, 22, 2.2f, col_rgb(C_SOFT));
		text(F_T20B, s, x + 32, 112, 0, 56, col_rgb(C_SOFT), 0);
	}
	y = sec(384, 344, 848, 144, S_BP_THRESHOLDS);
	status(1216, 344, IC_PENCIL, C_DIM, F_T20, tr(S_BP_THRESHOLDS_SUB), C_DIM, NULL);
	line(y);
	x = 408;
	for (i = 0; i < g_cfg.bat_ntiers; i++) {
		x += chip(i, x, y + 9) + 12;
	}
	if (g_cfg.bat_ntiers < MAX_TIERS) {
		add_btn(x, y + 9);
	}
	y += 72;
	line(y);
	cw = (808 - 32) / 3.0f;
	fmt_num(pb, 16, g_cfg.bat_tiers[0], 0, L"%");
	swprintf(ab, 64, tr(S_BP_ABOVE), pb);
	legend(408, y, cw, IC_CHECK_CIRCLE, C_SOFT, S_BP_NORMAL, C_TEXT, ab);
	legend(424 + cw, y, cw, IC_WARNING, C_WARN, S_BP_WARNING, C_WARN, tr(S_BP_BELOW));
	legend(440 + 2 * cw, y, cw, IC_ALERT, C_CRIT, S_BP_CRITICAL, C_CRIT_T, tr(S_BP_LOWEST));
	dim(off);
	y = sec(384, 576, 848, 72, S_BP_DEVICES);
	line(y);
	for (i = 0; i < 3; i++) {
		w[i] = tw(F_T22B, tr(dn[i]), 0) + 136;
		sum += w[i];
	}
	x = 408;
	for (i = 0; i < 3; i++) {
		on = g_cfg.bat_devices & 1u << i;
		gx_icon(di[i], x, y + 25, 24, 2.2f, col_rgb(on ? C_SOFT : C_DIM));
		text(F_T22B, tr(dn[i]), x + 36, y + 2, 0, 70, col_rgb(on ? C_TEXT : C_SOFT), 0);
		sw(W_DEV + i, x + w[i] - 88, y + 9, on);
		x += w[i] + (808 - sum) / 2;
	}
	dim(false);
}

static int kp_tier(void) {
	return kp - KP_TIER;
}

static int kp_state(uint32_t *v) {
	int i;
	if (!kv[0]) return KS_EMPTY;
	if (kp == KP_HTML) return col_parse(kv, v) ? KS_OK : KS_BAD;
	*v = (uint32_t)wcstoul(kv, NULL, 10);
	if (kp == KP_PORT) return *v >= 1024 && *v <= 65535 ? KS_OK : KS_BAD;
	if (*v < 5 || *v > 95) return KS_BAD;
	for (i = 0; i < g_cfg.bat_ntiers; i++) {
		if (i != kp_tier() && g_cfg.bat_tiers[i] == (int)*v) return KS_TAKEN;
	}
	return KS_OK;
}

static void pad_btn(int id, float x, float y, float w, const WCHAR *s, enum gx_icon ic, bool acc) {
	int k = st(id), f = ic < IC_COUNT ? F_D24 : F_D30;
	float t = tw(f, s, 0), isz = ic < IC_COUNT ? 28 : 0, g = t > 0 && isz > 0 ? 12 : 0, cx = x + (w - isz - g - t) / 2;
	struct gx_color fg = acc ? col_on(g_cfg.accent) : col_rgb(C_TEXT);
	gx_fill_rrect(x, y, w, 72, 14, acc ? tint(col_accent(), k) : wa(0.08f + 0.06f * k));
	gx_icon(ic, cx, y + 22, 28, 2.2f, fg);
	text(f, s, cx + isz + g, y, 0, 72, fg, 0);
	hit(id, x, y, w, 72);
}

static void keypad(void) {
	static const char np[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, KEY_BS, 0, KEY_CLR };
	bool hex = kp == KP_HTML, wide;
	uint32_t v = 0;
	int cols = hex ? 4 : 3, rows = hex ? 5 : 4, s = kp_state(&v), i, k;
	float gw = cols * 140 - 12.0f, w = gw + 48, h = rows * 84 + 320.0f, x = 808 - w / 2, y = (PH - h) / 2, half = (gw - 12) / 2, tx = x + 48;
	struct gx_color hc = col_rgb(s >= KS_BAD ? C_WARN : C_DIM);
	WCHAR d[2];
	hit(W_SCRIM, 0, 0, PW, PH);
	gx_fill_rrect(0, 0, PW, PH, 24, col_rgba(0x0A0B0E, 0.6f));
	gx_shadow_rrect(x, y, w, h, 24, 48, 24, col_rgba(0, 0.55f));
	gx_fill_rrect(x, y, w, h, 24, col_rgb(0x1E2128));
	gx_stroke_rrect(x, y, w, h, 24, 2, wa(0.08f));
	hit(W_NONE, x, y, w, h);
	text(F_D28, tr(hex ? S_C_HTML : kp == KP_PORT ? S_NP_PORT : kp_tier() == g_cfg.bat_ntiers ? S_BP_ADD : S_KP_THRESHOLD), x + 24, y + 24, gw - 72, 56, col_rgb(C_TEXT), GT_ELLIPSIS);
	gx_fill_rrect(x + w - 80, y + 24, 56, 56, 16, wa(0.1f + 0.06f * st(W_KP_CLOSE)));
	gx_icon(IC_CLOSE, x + w - 64, y + 40, 24, 2.4f, col_rgb(C_TEXT));
	hit(W_KP_CLOSE, x + w - 80, y + 24, 56, 56);
	gx_fill_rrect(x + 24, y + 96, gw, 72, 14, col_rgba(0, 0.3f));
	gx_stroke_rrect(x + 24, y + 96, gw, 72, 14, 2, col_accent_mid());
	if (hex) {
		tx += text(F_D40, L"#", tx, y + 96, 0, 72, col_rgb(C_DIM), 0);
	}
	tx += text(F_D40, kv, tx, y + 96, 0, 72, col_rgb(fresh ? C_DIM : C_TEXT), GT_TNUM);
	gx_fill_rrect(tx + 4, y + 110, 3, 44, 1.5f, col_accent());
	if (kp >= KP_TIER) {
		text(F_D40, L"%", tx + 16, y + 96, 0, 72, col_rgb(C_DIM), 0);
	}
	if (hex && s == KS_OK) {
		swatch(x + gw - 32, y + 112, 40, 40, 10, v);
	}
	gx_icon(s >= KS_BAD ? IC_WARNING : IC_INFO, x + 24, y + 180, 20, 2.2f, hc);
	text(F_T20, tr(s == KS_TAKEN ? S_KP_TAKEN : hex ? S_KP_HEX_HINT : kp == KP_PORT ? S_KP_PORT_HINT : S_KP_TIER_HINT), x + 52, y + 176, gw - 28, 28, hc, GT_ELLIPSIS);
	for (i = 0; i < (hex ? 18 : 12); i++) {
		k = hex ? i : np[i];
		wide = i >= 16;
		d[0] = k < KEY_BS ? hexd[k] : 0;
		d[1] = 0;
		pad_btn(W_KEY + k, x + 24 + (wide ? (i - 16) * (half + 12) : i % cols * 140), y + 220 + i / cols * 84, wide ? half : 128, k == KEY_CLR ? hex ? tr(S_KP_CLEAR) : L"C" : d, k == KEY_BS ? IC_BACKSPACE : IC_COUNT, false);
	}
	pad_btn(W_KP_DEF, x + 24, y + 224 + rows * 84, half, tr(S_G_DEFAULT), IC_RESET, false);
	gx_opacity(s == KS_OK ? 1 : 0.4f);
	pad_btn(s == KS_OK ? W_KP_OK : W_NONE, x + 36 + half, y + 224 + rows * 84, half, tr(S_KP_OK), IC_CHECK, true);
	gx_opacity(1);
}

static void draw(void) {
	dirty = false;
	seen_gen = g_cfg_gen;
	if (!tex) return;
	if (page == PG_COLOR && !wheel_done) {
		wheel_done = true;
		wheel_ok = bake();
	}
	if (page == PG_NOTIF) {
		read_status();
	}
	nhit = 0;
	fade = 1;
	if (!gx_begin(tex)) return;
	chrome();
	switch (page) {
	case PG_GENERAL:
		pg_general();
		break;
	case PG_WATCH:
		pg_watch();
		break;
	case PG_NOTIF:
		pg_notif();
		break;
	case PG_BATTERY:
		pg_battery();
		break;
	default:
		pg_color();
		break;
	}
	if (kp) {
		keypad();
	}
	if (gx_end()) {
		ovl_texture(&ovl, tex);
	}
}

static void draw_grab(void) {
	const WCHAR *s;
	float w, x;
	int i;
	gdirty = false;
	glang = g_cfg.lang;
	gacc = g_cfg.accent;
	if (!gtex || !gx_begin(gtex)) return;
	gx_transform(GS, 0, 0);
	if (drag || ghover) {
		s = tr(S_P_GRAB_HINT);
		w = fminf(448, tw(F_T20, s, 0) + 56);
		x = 448 - w;
		gx_fill_rrect(x, 4, w, 48, 8, col_rgba(C_CARD, 0.95f));
		gx_stroke_rrect(x, 4, w, 48, 8, 2, wa(0.07f));
		gx_icon(IC_MOVE, x + 12, 18, 20, 2.2f, col_rgb(C_TEXT));
		text(F_T20, s, x + 40, 4, w - 56, 48, col_rgb(C_SOFT), GT_ELLIPSIS);
	}
	if (drag) {
		gx_fill_rrect(BAR_X, 0, 240, 56, 16, col_accent_soft());
		gx_shadow_rrect(BAR_X + 16, 16, 208, 24, 12, 16, 0, col_accent_mid());
		gx_fill_rrect(BAR_X + 17, 17, 206, 22, 11, col_rgb(C_TEXT));
		gx_fill_rrect(BAR_X + 20, 20, 200, 16, 8, col_accent());
	} else {
		gx_fill_rrect(BAR_X, 0, 240, 56, 16, col_rgba(C_CARD, ghover ? 0.95f : 0.8f));
		gx_stroke_rrect(BAR_X, 0, 240, 56, 16, 2, wa(0.07f));
		gx_fill_rrect(BAR_X + 20, 20, 200, 16, 8, ghover ? col_accent() : col_rgb(C_DIM));
	}
	for (i = 0; i < 5; i++) {
		gx_fill_circle(BAR_X + 104 + 8 * i, 28, 2.5f, col_rgb(C_INK));
	}
	if (gx_end()) {
		ovl_texture(&grab, gtex);
	}
}

static void place(void) {
	HmdMatrix34_t g = m34_mul(&xf, &grab_local);
	ovl_abs(&ovl, &xf);
	ovl_abs(&grab, &g);
}

static void place_front(void) {
	const HmdMatrix34_t *h = vr_pose(0);
	struct v3 eye = h ? m34_pos(h) : v3(0, 1.6f, 0), f = v3(0, 0, -1), fw, p;
	if (h) {
		fw = v3_mul(m34_col(h, 2), -1);
		f = v3_add(fw, v3_mul(m34_col(h, 1), fw.y > 0 ? -1.0f : 1.0f));
	}
	f.y = 0;
	f = v3_len(f) > 0.05f ? v3_norm(f) : v3(0, 0, -1);
	p = v3_add(eye, v3_mul(f, 1.0f));
	p.y += 0.05f;
	xf = m34_look(p, eye, v3(0, 1, 0));
	place();
}

static void release(void) {
	ovl_clear(&ovl);
	ovl_clear(&grab);
	gx_bmp_free(wheel);
	gx_tex_free(tex);
	gx_tex_free(gtex);
	wheel = NULL;
	tex = gtex = NULL;
	wheel_done = wheel_ok = false;
	nhit = 0;
}

static void panel_show(bool on) {
	if (on == is_open) return;
	if (!on) {
		is_open = dd = drag = ghover = false;
		ovl_show(&ovl, false);
		ovl_show(&grab, false);
		kp = KP_NONE;
		release();
		return;
	}
	if (!ovl.h || !grab.h) return;
	tex = gx_tex_new(PW, PH);
	gtex = gx_tex_new(GW, GH);
	if (!tex || !gtex) {
		glog("panel: cannot create its textures");
		release();
		return;
	}
	is_open = true;
	hover = active = W_NONE;
	gfx_gen = g_gfx_gen;
	sync_hsv();
	place_front();
	draw();
	draw_grab();
	ovl_show(&ovl, true);
	ovl_show(&grab, true);
}

static void kp_open(int f, uint32_t v) {
	kp = f;
	kv[0] = 0;
	fresh = true;
	if (f == KP_HTML || v) {
		swprintf(kv, ARRAYSIZE(kv), f == KP_HTML ? L"%06X" : L"%u", v);
	}
}

static void kp_key(int id) {
	size_t n;
	if (fresh && id < W_KEY_BS) {
		kv[0] = 0;
	}
	fresh = false;
	n = wcslen(kv);
	if (id == W_KEY_BS) {
		kv[n ? n - 1 : 0] = 0;
	} else if (id == W_KEY_CLR) {
		kv[0] = 0;
	} else if (n < (size_t)(kp == KP_HTML ? 6 : kp == KP_PORT ? 5 : 2) && (n || id > W_KEY || kp == KP_HTML)) {
		kv[n] = hexd[id - W_KEY];
		kv[n + 1] = 0;
	}
}

static void kp_ok(void) {
	uint32_t v = 0;
	int i = kp_tier();
	if (kp_state(&v) != KS_OK) return;
	if (kp == KP_HTML) {
		set_accent(v);
		sync_hsv();
	} else if (kp == KP_PORT) {
		set_i(&g_cfg.udp_port, (int)v);
	} else {
		g_cfg.bat_tiers[i] = (int)v;
		g_cfg.bat_ntiers += i == g_cfg.bat_ntiers;
		cfg_clamp(&g_cfg);
		changed();
	}
	kp = KP_NONE;
}

static void click(int id) {
	struct cfg d;
	int i;
	cfg_defaults(&d);
	if (id >= W_NAV && id < W_NAV + 4) {
		page = id - W_NAV;
		return;
	}
	if (id >= W_LANG_OPT && id < W_LANG_OPT + LANG_COUNT) {
		dd = false;
		set_i(&g_cfg.lang, id - W_LANG_OPT);
		return;
	}
	if (id >= W_PRESET && id < W_PRESET + 6) {
		set_accent(presets[id - W_PRESET]);
		sync_hsv();
		return;
	}
	if (id >= W_TIER && id < W_TIER + MAX_TIERS) {
		kp_open(KP_TIER + id - W_TIER, (uint32_t)g_cfg.bat_tiers[id - W_TIER]);
		return;
	}
	if (id >= W_KEY && id <= W_KEY_CLR) {
		kp_key(id);
		return;
	}
	if (id >= W_TIER_DEL && id < W_TIER_DEL + MAX_TIERS) {
		i = id - W_TIER_DEL;
		if (g_cfg.bat_ntiers < 2 || i >= g_cfg.bat_ntiers) return;
		memmove(&g_cfg.bat_tiers[i], &g_cfg.bat_tiers[i + 1], (MAX_TIERS - 1 - i) * sizeof(*g_cfg.bat_tiers));
		g_cfg.bat_tiers[--g_cfg.bat_ntiers] = 0;
		changed();
		return;
	}
	if (id >= W_DEV && id < W_DEV + 3) {
		g_cfg.bat_devices ^= 1u << (id - W_DEV);
		changed();
		return;
	}
	switch (id) {
	case W_CLOSE:
		panel_show(false);
		break;
	case W_BACK:
		page = PG_WATCH;
		break;
	case W_SCRIM:
	case W_KP_CLOSE:
		dd = false;
		kp = KP_NONE;
		break;
	case W_KP_DEF:
		kp_open(kp, kp == KP_HTML ? d.accent : kp == KP_PORT ? (uint32_t)d.udp_port : (uint32_t)d.bat_tiers[kp_tier()]);
		break;
	case W_KP_OK:
		kp_ok();
		break;
	case W_LANG:
		dd = !dd;
		break;
	case W_AUTOLAUNCH:
		flip(&g_cfg.autolaunch);
		break;
	case W_QUIT:
		app_quit();
		break;
	case W_WRIST:
	case W_WRIST_R:
		set_i(&g_cfg.wrist, id - W_WRIST);
		break;
	case W_CLOCK:
	case W_CLOCK24:
		if (g_cfg.clock24 != (id == W_CLOCK24)) {
			flip(&g_cfg.clock24);
		}
		break;
	case W_CHOOSE:
		page = PG_COLOR;
		sync_hsv();
		break;
	case W_MEDIA:
		flip(&g_cfg.media);
		break;
	case W_HTML:
		kp_open(KP_HTML, g_cfg.accent);
		break;
	case W_COLOR_RESET:
		set_accent(d.accent);
		sync_hsv();
		break;
	case W_TEST_NOTIF:
		notif_test();
		break;
	case W_WATCH_RESET:
		set_i(&g_cfg.watch_size, d.watch_size);
		set_i(&g_cfg.watch_angle, d.watch_angle);
		break;
	case W_POS_RESET:
		set_f(&g_cfg.notif_dist, d.notif_dist);
		set_i(&g_cfg.notif_angle, d.notif_angle);
		set_f(&g_cfg.notif_hoff, d.notif_hoff);
		set_i(&g_cfg.notif_size, d.notif_size);
		break;
	case W_MIN_DEC:
	case W_MIN_INC:
		set_r(CR_DUR_MIN, g_cfg.dur_min + (id == W_MIN_INC ? 1 : -1));
		set_i(&g_cfg.dur_max, g_cfg.dur_max < g_cfg.dur_min ? g_cfg.dur_min : g_cfg.dur_max);
		break;
	case W_MAX_DEC:
	case W_MAX_INC:
		set_r(CR_DUR_MAX, g_cfg.dur_max + (id == W_MAX_INC ? 1 : -1));
		set_i(&g_cfg.dur_min, g_cfg.dur_min > g_cfg.dur_max ? g_cfg.dur_max : g_cfg.dur_min);
		break;
	case W_NSOUND:
		flip(&g_cfg.notif_sound);
		break;
	case W_UDP:
		flip(&g_cfg.udp_on);
		break;
	case W_PORT:
		kp_open(KP_PORT, (uint32_t)g_cfg.udp_port);
		break;
	case W_DISCORD:
		flip(&g_cfg.discord);
		break;
	case W_NOTIFS:
		flip(&g_cfg.notif_on);
		break;
	case W_PORT_DEFAULT:
		set_i(&g_cfg.udp_port, d.udp_port);
		break;
	case W_TEST_ALERT:
		battery_test_alert();
		break;
	case W_ALERTS:
		flip(&g_cfg.bat_alerts);
		break;
	case W_BSOUND:
		flip(&g_cfg.bat_sound);
		break;
	case W_TIER_ADD:
		kp_open(KP_TIER + g_cfg.bat_ntiers, 0);
		break;
	}
}

static void slide(int id, float x, float y) {
	const struct hit *h = hit_find(id);
	float dx, dy, a, nh, ns, p, o, t;
	int r = range_of(id);
	if (!h) return;
	if (id == W_WHEEL) {
		dx = x - h->x - h->w / 2;
		dy = y - h->y - h->h / 2;
		a = atan2f(dx, -dy) * (180 / PI_F);
		nh = a < 0 ? a + 360 : a;
		ns = fminf(1, sqrtf(dx * dx + dy * dy) / (h->w / 2));
		p = nh * DEG;
		o = ch * DEG;
		if (lroundf(sinf(p) * ns * 160) != lroundf(sinf(o) * cs * 160) || lroundf(cosf(p) * ns * 160) != lroundf(cosf(o) * cs * 160) || col_hsv(nh, ns, 1) != col_hsv(ch, cs, 1)) {
			dirty = true;
		}
		ch = nh;
		cs = ns;
		set_accent(col_hsv(ch, cs, cv));
		return;
	}
	t = fmaxf(0, fminf(1, (x - h->x - 28) / (h->w - 56)));
	if (r >= 0) {
		set_r(r, cfg_snap(r, t));
		return;
	}
	if (lroundf(t * 344) != lroundf(cv * 344) || lroundf(t * 100) != lroundf(cv * 100)) {
		dirty = true;
	}
	cv = t;
	set_accent(col_hsv(ch, cs, cv));
}

static void mouse_move(float x, float y) {
	int h;
	mx = x;
	my = y;
	if (is_slider(active)) {
		slide(active, x, y);
		return;
	}
	h = hit_at(x, y);
	if (h != hover) {
		hover = h;
		dirty = true;
	}
}

static void mouse_down(float x, float y) {
	mx = x;
	my = y;
	hover = active = hit_at(x, y);
	dirty = true;
	if (is_slider(active)) {
		slide(active, x, y);
	}
}

static void mouse_up(void) {
	int a = active;
	active = W_NONE;
	dirty = true;
	if (!a || is_slider(a) || hit_at(mx, my) != a) return;
	click(a);
	if (!is_open) return;
	draw();
	a = hit_at(mx, my);
	dirty = a != hover;
	hover = a;
}

static int input(struct laser_target *t, int ev, float x, float y) {
	bool bar = t == &lt_grab;
	int h = hover;
	switch (ev) {
	case LE_PASS:
		return bar && (x < BAR_X * GS || x >= (BAR_X + 240) * GS);
	case LE_MOVE:
	case LE_LEAVE:
		if (bar && ghover != (ev == LE_MOVE)) {
			ghover = ev == LE_MOVE;
			gdirty = true;
		} else if (!bar && ev == LE_MOVE) {
			mouse_move(x, y);
			return hover && hover != W_SCRIM && hover != h ? LR_HAPTIC : 0;
		} else if (!bar && hover) {
			hover = W_NONE;
			dirty = true;
		}
		break;
	case LE_DOWN:
		if (bar) return LR_GRAB;
		mouse_down(x, y);
		return active && active != W_SCRIM ? LR_HAPTIC : 0;
	case LE_UP:
		if (bar) return 0;
		mx = x;
		my = y;
		mouse_up();
		break;
	case LE_GRAB:
	case LE_DROP:
		drag = ev == LE_GRAB;
		gdirty = true;
		break;
	case LE_PLACE:
		place();
		break;
	}
	return 0;
}

void panel_init(void) {
	fonts();
	if (!ovl_create(&ovl, GL_APP_KEY ".panel", "Ghostlay configuration")) return;
	ovl_width(&ovl, PANEL_M);
	ovl_curvature(&ovl, 0.08f);
	if (!ovl_create(&grab, GL_APP_KEY ".grab", "Ghostlay grab bar")) return;
	ovl_width(&grab, GW * MPX);
	lt_panel = (struct laser_target){ &ovl, PW, PH, &xf, input, -1 };
	lt_grab = (struct laser_target){ &grab, GW, GH, &xf, input, -1 };
	laser_add(&lt_panel);
	laser_add(&lt_grab);
}

void panel_shutdown(void) {
	panel_show(false);
	ovl_destroy(&grab);
	ovl_destroy(&ovl);
}

void panel_toggle(void) {
	panel_show(!is_open);
}

bool panel_update(void) {
	SYSTEMTIME t;
	struct udp_status u;
	if (!is_open) return false;
	if (gfx_gen != g_gfx_gen) {
		gfx_gen = g_gfx_gen;
		wheel_done = false;
		dirty = gdirty = true;
	}
	dirty |= seen_gen != g_cfg_gen;
	if (page == PG_WATCH || page == PG_COLOR) {
		GetLocalTime(&t);
		dirty |= t.wMinute != minute;
	}
	if (page == PG_NOTIF) {
		udp_status(&u);
		dirty |= u.state != udp.state || u.port != udp.port || wcscmp(u.owner, udp.owner) || winnotif_status() != wn;
	}
	gdirty |= glang != g_cfg.lang || gacc != g_cfg.accent;
	if (dirty) {
		draw();
	}
	if (gdirty) {
		draw_grab();
	}
	return true;
}

#ifdef GL_TESTS
int panel_render_samples(const WCHAR *dir) {
	static const struct { int page, lang, w, ds; const WCHAR *name; } s[] = {
		{ PG_GENERAL, LANG_EN, 0, WN_ACTIVE, L"general" }, { PG_GENERAL, LANG_EN, W_LANG, WN_ACTIVE, L"language" }, { PG_WATCH, LANG_EN, 0, WN_ACTIVE, L"watch" }, { PG_COLOR, LANG_EN, 0, WN_ACTIVE, L"colour" },
		{ PG_NOTIF, LANG_EN, 0, WN_ACTIVE, L"notifications" }, { PG_BATTERY, LANG_EN, 0, WN_ACTIVE, L"battery" }, { PG_GENERAL, LANG_FR, W_LANG, WN_ACTIVE, L"general-fr" }, { PG_WATCH, LANG_FR, 0, WN_ACTIVE, L"watch-fr" },
		{ PG_COLOR, LANG_FR, 0, WN_ACTIVE, L"colour-fr" }, { PG_NOTIF, LANG_FR, 0, WN_ACTIVE, L"notifications-fr" }, { PG_BATTERY, LANG_FR, 0, WN_ACTIVE, L"battery-fr" },
		{ PG_NOTIF, LANG_EN, W_PORT, WN_ACTIVE, L"keypad-port" }, { PG_COLOR, LANG_EN, W_HTML, WN_ACTIVE, L"keypad-hex" }, { PG_BATTERY, LANG_FR, W_TIER + 1, WN_ACTIVE, L"keypad-tier-fr" },
		{ PG_NOTIF, LANG_FR, 0, WN_BLOCKED, L"notifications-discord-blocked-fr" }, { PG_NOTIF, LANG_FR, 0, WN_UNAVAILABLE, L"notifications-discord-unavailable-fr" }, { PG_NOTIF, LANG_EN, 0, WN_OFF, L"notifications-discord-off" },
		{ PG_NOTIF, LANG_EN, W_NOTIFS, WN_ACTIVE, L"notifications-off" }, { PG_NOTIF, LANG_FR, W_NOTIFS, WN_ACTIVE, L"notifications-off-fr" }, { PG_BATTERY, LANG_EN, W_NOTIFS, WN_ACTIVE, L"battery-off" }, { PG_BATTERY, LANG_FR, W_NOTIFS, WN_ACTIVE, L"battery-off-fr" },
	};
	struct cfg saved = g_cfg;
	WCHAR p[MAX_PATH];
	int n = 0, i;
	fonts();
	tex = gx_tex_new(PW, PH);
	gtex = gx_tex_new(GW, GH);
	sample = true;
	g_cfg.notif_volume = 50;
	g_cfg.bat_devices = BDEV_HMD | BDEV_CTRL;
	udp = (struct udp_status){ UDP_LISTENING, DEFAULT_PORT, { 0 } };
	for (i = 0; tex && i < (int)ARRAYSIZE(s); i++) {
		page = s[i].page;
		g_cfg.lang = s[i].lang;
		wn = s[i].ds;
		g_cfg.discord = wn != WN_OFF;
		g_cfg.notif_on = s[i].w != W_NOTIFS;
		dd = false;
		kp = KP_NONE;
		sync_hsv();
		if (s[i].w && g_cfg.notif_on) {
			click(s[i].w);
		}
		draw();
		swprintf(p, MAX_PATH, L"%ls\\panel-%ls.png", dir, s[i].name);
		n += gx_save_png(tex, p);
	}
	g_cfg.lang = LANG_EN;
	for (i = 0; gtex && i < 2; i++) {
		ghover = !i;
		drag = i;
		draw_grab();
		swprintf(p, MAX_PATH, L"%ls\\panel-grab-%ls.png", dir, i ? L"held" : L"hover");
		n += gx_save_png(gtex, p);
	}
	release();
	g_cfg = saved;
	sample = dd = drag = ghover = false;
	page = PG_GENERAL;
	kp = KP_NONE;
	return n;
}
#endif
