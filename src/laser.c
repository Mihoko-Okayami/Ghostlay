#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "cfg.h"
#include "laser.h"
#include "main.h"
#include "vr.h"

#define MAX_TARGETS 8
#define CUR_PX 64
#define RAY_SIN 0.64278761f
#define RAY_COS 0.76604444f
#define DOWN_AT 0.4f
#define UP_AT 0.2f
#define GRIP 0.3f
#define CUR_MIN 0.0075f
#define CUR_GRAB 0.03f
#define PUSH 0.03f
#define DAMP 15.0f
#define HAP_CLICK 0.002f
#define HAP_HOVER 0.0005f
#define ACT "/actions/ghostlay"
#define SCR "/actions/scroll"
#define DEF(t) "{\"controller_type\":\"" t "\",\"binding_url\":\"bindings_" t ".json\"}"
#define SRC(h, p, m, i, o) "{\"path\":\"/user/hand/" h "/input/" p "\",\"mode\":\"" m "\",\"inputs\":{\"" i "\":{\"output\":\"" o "\"}}}"
#define HANDS(p, m, i, o) SRC("left", p, m, i, o) "," SRC("right", p, m, i, o)
#define STICK(p, m) HANDS(p, m, "position", SCR "/in/stick")
#define BIND(t, s, k) "{\"controller_type\":\"" t "\",\"name\":\"Ghostlay defaults\",\"bindings\":{\"" ACT "\":{\"sources\":[" HANDS("trigger", "trigger", "pull", ACT "/in/click") "," s "]},\"" SCR "\":{\"sources\":[" k "]}}}\n"

enum { CT_CURSOR, CT_LOCKED, CT_DROP, CT_COUNT };

enum { EZ_LINEAR, EZ_IN_QUAD, EZ_OUT_BACK };

struct tween {
	float a, b, dur;
	double t0;
	int ease;
};

struct hand {
	TrackedDeviceIndex_t dev;
	uint64_t src;
	struct ovl cur;
	struct laser_target *hov, *cap;
	struct tween size;
	struct v3 o, d, goff;
	float x, y, dist, gdist, trig, grip;
	bool down, grab, tgrab, gripped;
	int tex;
};

static const uint8_t prof[CT_COUNT][32][2] = {
	[CT_CURSOR] = { { 4, 5 }, { 2, 182 }, { 0, 255 }, { 87, 212 }, { 252, 139 }, { 255, 138 }, { 255, 138 }, { 255, 137 }, { 255, 151 }, { 255, 240 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 } },
	[CT_LOCKED] = { { 0, 5 }, { 0, 180 }, { 29, 255 }, { 223, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 152 }, { 255, 61 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 } },
	[CT_DROP] = { { 0, 5 }, { 0, 180 }, { 29, 255 }, { 223, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 204 }, { 255, 159 }, { 255, 160 }, { 255, 160 }, { 255, 160 }, { 255, 160 }, { 255, 160 }, { 255, 161 }, { 255, 118 }, { 255, 63 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 }, { 255, 64 } },
};

static struct laser_target *tgt[MAX_TARGETS];
static int ntgt;
static struct hand hands[2];
static uint64_t aset, sset, a_click, a_grab, a_push, a_stick;
static struct laser_target *stick_want, *stick_on;
static float push, stick_x, stick_y;
static double in_wait;
static bool in_said, stick_set;
static uint8_t pix[CUR_PX * CUR_PX * 4];

static float tw_at(const struct tween *t, double now) {
	float k = t->dur > 0 ? clampf((float)(now - t->t0) / t->dur, 0, 1) : 1;
	if (t->ease == EZ_IN_QUAD) {
		k *= k;
	} else if (t->ease == EZ_OUT_BACK) {
		k -= 1;
		k = 1 + 2.70158f * k * k * k + 1.70158f * k * k;
	}
	return t->a + (t->b - t->a) * k;
}

static void tw_set(struct tween *t, float a, float b, double now, float dur, int ease) {
	*t = (struct tween){ a, b, dur, now, ease };
}

static float pv(int k, int n, int c) {
	if (n < 0) return c ? 0 : prof[k][0][0];
	return prof[k][n > 31 ? 31 : n][c];
}

static void bake(struct ovl *o, int k) {
	uint8_t *p = pix;
	float f, t, g, a;
	int i, j, n;
	for (j = 0; j < CUR_PX; j++) {
		for (i = 0; i < CUR_PX; i++, p += 4) {
			f = 31.5f - hypotf(i + 0.5f - CUR_PX / 2, j + 0.5f - CUR_PX / 2);
			n = (int)floorf(f);
			t = f - n;
			g = pv(k, n, 0) + (pv(k, n + 1, 0) - pv(k, n, 0)) * t;
			a = pv(k, n, 1) + (pv(k, n + 1, 1) - pv(k, n, 1)) * t;
			p[0] = p[1] = p[2] = (uint8_t)(g * a / 255 + 0.5f);
			p[3] = (uint8_t)(a + 0.5f);
		}
	}
	ovl_raw(o, pix, CUR_PX, CUR_PX);
}

static void set_tex(struct hand *h, int k) {
	if (h->tex == k) return;
	h->tex = k;
	bake(&h->cur, k);
}

static bool manifest(void) {
	static const struct { const char *type, *body; } b[] = {
		{ "knuckles", BIND("knuckles", HANDS("grip", "force_sensor", "force", ACT "/in/grab") "," SRC("right", "thumbstick", "joystick", "position", ACT "/in/pushpull"), STICK("thumbstick", "joystick")) },
		{ "oculus_touch", BIND("oculus_touch", HANDS("grip", "trigger", "pull", ACT "/in/grab") "," SRC("right", "joystick", "joystick", "position", ACT "/in/pushpull"), STICK("joystick", "joystick")) },
		{ "vive_controller", BIND("vive_controller", HANDS("grip", "button", "click", ACT "/in/grab") "," HANDS("trackpad", "trackpad", "position", ACT "/in/pushpull"), STICK("trackpad", "trackpad")) },
		{ "holographic_controller", BIND("holographic_controller", HANDS("grip", "button", "click", ACT "/in/grab") "," SRC("right", "joystick", "joystick", "position", ACT "/in/pushpull"), STICK("joystick", "joystick")) },
	};
	static const char actions[] = "{\"default_bindings\":[" DEF("knuckles") "," DEF("oculus_touch") "," DEF("vive_controller") "," DEF("holographic_controller") "],\"actions\":[{\"name\":\"" ACT "/in/click\",\"type\":\"vector1\",\"requirement\":\"suggested\"},{\"name\":\"" ACT "/in/grab\",\"type\":\"vector1\",\"requirement\":\"suggested\"},{\"name\":\"" ACT "/in/pushpull\",\"type\":\"vector2\",\"requirement\":\"optional\"},{\"name\":\"" SCR "/in/stick\",\"type\":\"vector2\",\"requirement\":\"optional\"}],\"action_sets\":[{\"name\":\"" ACT "\",\"usage\":\"leftright\"},{\"name\":\"" SCR "\",\"usage\":\"leftright\"}],\"localization\":[{\"language_tag\":\"en_US\",\"" ACT "\":\"Ghostlay\",\"" ACT "/in/click\":\"Click and drag\",\"" ACT "/in/grab\":\"Grab the configuration panel\",\"" ACT "/in/pushpull\":\"Push or pull a grabbed panel\",\"" SCR "\":\"Ghostlay scrolling\",\"" SCR "/in/stick\":\"Scroll the history or the tracker list\"}]}\n";
	WCHAR dir[MAX_PATH], p[MAX_PATH];
	char u[MAX_PATH * 3];
	int i;
	swprintf(dir, MAX_PATH, L"%ls\\input", g_app.data);
	CreateDirectoryW(dir, NULL);
	for (i = 0; i < (int)ARRAYSIZE(b); i++) {
		swprintf(p, MAX_PATH, L"%ls\\bindings_%hs.json", dir, b[i].type);
		write_file(p, b[i].body, (int)strlen(b[i].body));
	}
	swprintf(p, MAX_PATH, L"%ls\\actions.json", dir);
	write_file(p, actions, (int)strlen(actions));
	to_utf8(p, u, sizeof(u));
	return vr_input_manifest(u);
}

static void ray(const HmdMatrix34_t *m, struct v3 *o, struct v3 *d) {
	*o = m34_pos(m);
	*d = v3_norm(v3_add(v3_mul(m34_col(m, 1), -RAY_SIN), v3_mul(m34_col(m, 2), -RAY_COS)));
}

#ifdef GL_TESTS
static HmdMatrix34_t m34_inv(const HmdMatrix34_t *a) {
	struct v3 x = m34_col(a, 0), y = m34_col(a, 1), z = m34_col(a, 2), t = m34_pos(a);
	return (HmdMatrix34_t){ { { x.x, x.y, x.z, -v3_dot(x, t) }, { y.x, y.y, y.z, -v3_dot(y, t) }, { z.x, z.y, z.z, -v3_dot(z, t) } } };
}

static bool t_hit(const struct laser_target *lt, struct v3 src, struct v3 dir, float *dist, float *u, float *v) {
	HmdMatrix34_t m, inv;
	struct v3 z, l;
	float den, t;
	if (!ovl_world(lt->ov, &m)) return false;
	z = m34_col(&m, 2);
	den = v3_dot(dir, z);
	if (fabsf(den) < 1e-6f) return false;
	t = v3_dot(v3_sub(m34_pos(&m), src), z) / den;
	inv = m34_inv(&m);
	l = m34_point(&inv, v3_add(src, v3_mul(dir, t)));
	*dist = t;
	*u = l.x / lt->ov->width + 0.5f;
	*v = l.y / (lt->ov->width * (lt->h / lt->w)) + 0.5f;
	return t > 0 && *u >= 0 && *u <= 1 && *v >= 0 && *v <= 1;
}
#endif

static struct laser_target *cast(struct v3 o, struct v3 d, const HmdMatrix34_t *hd, float *x, float *y, float *dist) {
	struct laser_target *best = NULL, *t;
	HmdMatrix34_t m;
	float u, v, dd, px, py;
	bool hit;
	int i;
	*dist = 1e9f;
	for (i = 0; i < ntgt; i++) {
		t = tgt[i];
		if (!t->ov->shown || t->ov->alpha <= 0) continue;
		if (hd && t->ov->curve <= 0 && ovl_world(t->ov, &m) && v3_dot(m34_col(hd, 2), m34_col(&m, 2)) <= 0) continue;
		hit = ovl_hit(t->ov, o, d, &dd, &u, &v);
#ifdef GL_TESTS
		hit = t->ov->h ? hit : t_hit(t, o, d, &dd, &u, &v);
#endif
		if (!hit || dd >= *dist) continue;
		px = u * t->w;
		py = (1 - v) * t->h;
		if (t->fn(t, LE_PASS, px, py)) continue;
		best = t;
		*dist = dd;
		*x = px;
		*y = py;
	}
	return best;
}

static void point(int i, struct laser_target *t, float x, float y) {
	if (t->fn(t, LE_MOVE, x, y) & LR_HAPTIC) {
		vr_haptic(hands[i].dev, HAP_HOVER);
	}
}

static void own(int i, struct laser_target *t) {
	int o = t->hand;
	if (o == i || (o >= 0 && hands[o].cap == t)) return;
	if (o >= 0) {
		t->fn(t, LE_LEAVE, -1, -1);
	}
	t->hand = i;
	point(i, t, hands[i].x, hands[i].y);
}

static void leave(int i, struct laser_target *t) {
	if (!t || t->hand != i) return;
	t->fn(t, LE_LEAVE, -1, -1);
	if (hands[i].cap != t) {
		t->hand = -1;
	}
}

static void hover(int i, struct laser_target *t, float x, float y, float d) {
	struct hand *h = &hands[i];
	bool moved = x != h->x || y != h->y;
	if (h->hov != t) {
		leave(i, h->hov);
		moved = true;
	}
	h->hov = t;
	h->x = x;
	h->y = y;
	h->dist = d;
	if (t && t->hand < 0) {
		own(i, t);
	} else if (t && t->hand == i && moved) {
		point(i, t, x, y);
	}
}

static bool held(const HmdMatrix34_t *xf) {
	int i;
	for (i = 0; i < 2; i++) {
		if (hands[i].grab && hands[i].cap->xf == xf) return true;
	}
	return false;
}

static bool upright(const HmdMatrix34_t *xf) {
	int i;
	for (i = 0; i < ntgt; i++) {
		if (tgt[i]->xf == xf && tgt[i]->ov->curve > 0) return true;
	}
	return false;
}

static void grab_start(int i, double now, bool trig) {
	struct hand *h = &hands[i];
	struct laser_target *t = h->hov;
	h->cap = t;
	h->grab = true;
	h->tgrab = trig;
	h->gdist = h->dist;
	h->goff = v3_sub(m34_pos(t->xf), v3_add(h->o, v3_mul(h->d, h->dist)));
	tw_set(&h->size, tw_at(&h->size, now), CUR_GRAB, now, 0.3f, EZ_OUT_BACK);
	set_tex(h, CT_LOCKED);
	t->fn(t, LE_GRAB, h->x, h->y);
}

static void release(int i, double now) {
	struct hand *h = &hands[i];
	struct laser_target *c = h->cap;
	bool on = h->hov == c;
	h->cap = NULL;
	if (h->grab) {
		h->grab = h->tgrab = false;
		tw_set(&h->size, tw_at(&h->size, now), CUR_MIN, now, 0.15f, EZ_IN_QUAD);
		set_tex(h, CT_DROP);
		c->fn(c, LE_DROP, on ? h->x : -1, on ? h->y : -1);
	} else {
		c->fn(c, LE_UP, on ? h->x : -1, on ? h->y : -1);
	}
	if (!on && c->hand == i) {
		c->hand = -1;
	}
}

static void trigger(int i, double now) {
	struct hand *h = &hands[i];
	struct laser_target *t = h->hov;
	bool down = h->trig > (h->down ? UP_AT : DOWN_AT);
	int r;
	if (down && !h->down && !h->cap && t) {
		own(i, t);
		if (t->hand == i) {
			h->cap = t;
			r = t->fn(t, LE_DOWN, h->x, h->y);
			if (r & LR_HAPTIC) {
				vr_haptic(h->dev, HAP_CLICK);
			}
			if ((r & LR_GRAB) && t->xf && !held(t->xf)) {
				grab_start(i, now, true);
			}
		}
	} else if (!down && h->down && h->cap && (!h->grab || h->tgrab)) {
		release(i, now);
	}
	h->down = down;
}

static void grip(int i, double now) {
	struct hand *h = &hands[i];
	bool on = h->grip > GRIP;
	if (on && !h->gripped && !h->cap && h->hov && h->hov->xf && !held(h->hov->xf)) {
		own(i, h->hov);
		if (h->hov->hand == i) {
			grab_start(i, now, false);
		}
	} else if (!on && h->grab && !h->tgrab) {
		release(i, now);
	}
	h->gripped = on;
}

static void move(struct hand *h, const HmdMatrix34_t *hd, float dt) {
	struct laser_target *t = h->cap;
	struct v3 p, aim;
	float d, k;
	h->gdist = clampf(h->gdist + push * PUSH * dt * 90, 0.01f, 5);
	p = v3_lerp(m34_pos(t->xf), v3_add(v3_add(h->o, v3_mul(h->d, h->gdist)), h->goff), fminf(1, dt * DAMP));
	d = v3_len(v3_sub(p, h->o));
	t->xf->m[0][3] = p.x;
	t->xf->m[1][3] = p.y;
	t->xf->m[2][3] = p.z;
	if (hd && d > 0.15f) {
		k = clampf(d / 4, 0, 1);
		aim = v3_lerp(h->o, m34_pos(hd), k * k * (3 - 2 * k));
		if (upright(t->xf)) {
			aim.y = p.y;
		}
		if (v3_len(v3_sub(aim, p)) > 0.05f) {
			*t->xf = m34_look(p, aim, v3(0, 1, 0));
		}
	}
	t->fn(t, LE_PLACE, 0, 0);
}

static bool cursor(struct hand *h, const HmdMatrix34_t *hd, double now) {
	HmdMatrix34_t b, m;
	struct v3 p;
	float w;
	if ((!h->hov && !h->grab) || !hd) {
		ovl_show(&h->cur, false);
		return h->grab;
	}
	if (h->tex == CT_DROP && now >= h->size.t0 + h->size.dur) {
		set_tex(h, CT_CURSOR);
	}
	p = v3_add(h->o, v3_mul(h->d, h->hov ? h->dist : h->gdist));
	b = m34_look(p, m34_pos(hd), v3(0, 1, 0));
	w = fmaxf(v3_len(v3_sub(m34_pos(hd), p)) * tw_at(&h->size, now), CUR_MIN);
	m = m34_scale(&b, w);
	ovl_color(&h->cur, g_cfg.accent);
	ovl_abs(&h->cur, &m);
	ovl_show(&h->cur, true);
	return true;
}

static bool step(int i, double now, float dt) {
	struct hand *h = &hands[i];
	const HmdMatrix34_t *m = vr_pose(h->dev), *hd = vr_pose(0);
	struct laser_target *t = NULL;
	float x = -1, y = -1, d = 0;
	if (m) {
		ray(m, &h->o, &h->d);
		t = cast(h->o, h->d, hd, &x, &y, &d);
	}
	hover(i, t, x, y, d);
	if (h->cap && !h->cap->ov->shown) {
		release(i, now);
	}
	trigger(i, now);
	grip(i, now);
	if (h->grab && m) {
		move(h, hd, dt);
	}
	return cursor(h, hd, now);
}

static bool run(double now, float dt) {
	bool fast = false;
	int i;
	for (i = 0; i < 2; i++) {
		fast |= step(i, now, dt);
	}
	return fast;
}

static void diag(bool live, double dt) {
	if (in_said || !aset) return;
	if (live) {
		glog("laser: SteamVR input active");
		in_said = true;
	} else if (vr_pose(g_dev.left) || vr_pose(g_dev.right)) {
		in_wait += dt;
		if (in_wait > 10) {
			glog("laser: SteamVR input still inactive after 10 s, the trigger will not click");
			in_said = true;
		}
	}
}

void laser_init(void) {
	static const char *const side[2] = { "left", "right" };
	char key[64];
	struct hand *h;
	int i;
	memset(hands, 0, sizeof(hands));
	in_wait = 0;
	in_said = false;
	for (i = 0; i < 2; i++) {
		h = &hands[i];
		snprintf(key, sizeof(key), GL_APP_KEY ".cursor.%s", side[i]);
		ovl_create(&h->cur, key, "Ghostlay cursor");
		ovl_sort(&h->cur, 100);
		h->tex = -1;
		set_tex(h, CT_CURSOR);
		tw_set(&h->size, CUR_MIN, CUR_MIN, 0, 0, EZ_LINEAR);
	}
	if (!manifest()) {
		glog("laser: SteamVR input unavailable, the pointer cannot click");
		return;
	}
	aset = vr_input_handle(VI_SET, ACT);
	sset = vr_input_handle(VI_SET, SCR);
	a_click = vr_input_handle(VI_ACTION, ACT "/in/click");
	a_grab = vr_input_handle(VI_ACTION, ACT "/in/grab");
	a_push = vr_input_handle(VI_ACTION, ACT "/in/pushpull");
	a_stick = vr_input_handle(VI_ACTION, SCR "/in/stick");
	for (i = 0; i < 2; i++) {
		snprintf(key, sizeof(key), "/user/hand/%s", side[i]);
		hands[i].src = vr_input_handle(VI_SOURCE, key);
	}
}

void laser_shutdown(void) {
	int i;
	for (i = 0; i < 2; i++) {
		ovl_destroy(&hands[i].cur);
	}
	memset(hands, 0, sizeof(hands));
	ntgt = 0;
	aset = sset = a_click = a_grab = a_push = a_stick = 0;
	stick_want = stick_on = NULL;
	stick_set = false;
	push = 0;
}

void laser_add(struct laser_target *t) {
	int i;
	for (i = 0; i < ntgt; i++) {
		if (tgt[i] == t) return;
	}
	if (ntgt < MAX_TARGETS) {
		t->hand = -1;
		tgt[ntgt++] = t;
		ovl_mouse_scale(t->ov, 1, t->h / t->w);
	}
}

void laser_stick(struct laser_target *t, bool on) {
	if (on) {
		stick_want = t;
	} else if (stick_want == t) {
		stick_want = NULL;
	}
}

void laser_stick_pos(struct laser_target *t, float *x, float *y) {
	*x = t == stick_on ? stick_x : 0;
	*y = t == stick_on ? stick_y : 0;
}

static int stick_hand(void) {
	int i;
	for (i = 0; stick_want && i < 2; i++) {
		if (hands[i].hov == stick_want && stick_want->hand == i) return i;
	}
	return -1;
}

bool laser_update(double dt) {
	bool any = stick_set, live = false;
	float f;
	int i, s;
	stick_on = NULL;
	stick_x = stick_y = 0;
	for (i = 0; i < ntgt; i++) {
		any |= tgt[i]->ov->shown;
	}
	for (i = 0; i < 2; i++) {
		any |= hands[i].hov || hands[i].cap || hands[i].cur.shown;
	}
	if (!any) return false;
	hands[0].dev = g_dev.left;
	hands[1].dev = g_dev.right;
	s = stick_hand();
	stick_set = s >= 0 && sset;
	vr_input_sync(aset, stick_set ? sset : 0, stick_set ? hands[s].src : 0);
	for (i = 0; i < 2; i++) {
		live |= vr_analog(a_click, hands[i].src, &hands[i].trig, &f);
		vr_analog(a_grab, hands[i].src, &hands[i].grip, &f);
	}
	vr_analog(a_push, 0, &f, &push);
	if (s >= 0 && vr_analog(a_stick, hands[s].src, &stick_x, &stick_y)) {
		stick_on = hands[s].hov;
	}
	diag(live, dt);
	return run(now_s(), (float)dt);
}

#ifdef GL_TESTS
static int t_ev[64], t_n, t_ret;
static float t_x, t_y;

static int t_fn(struct laser_target *t, int ev, float x, float y) {
	(void)t;
	if (ev == LE_PASS) return x < 10;
	if (t_n < (int)ARRAYSIZE(t_ev)) {
		t_ev[t_n++] = ev;
	}
	t_x = x;
	t_y = y;
	return ev == LE_DOWN ? t_ret : 0;
}

static void t_pose(TrackedDeviceIndex_t d, float x, float y, float z, bool aim) {
	float c = aim ? RAY_COS : 1, s = aim ? RAY_SIN : 0;
	g_pose[d].bPoseIsValid = g_pose[d].bDeviceIsConnected = true;
	g_pose[d].mDeviceToAbsoluteTracking = (HmdMatrix34_t){ { { 1, 0, 0, x }, { 0, c, -s, y }, { 0, s, c, z } } };
}

static void t_run(double *now, int frames, float trig) {
	int i;
	for (i = 0; i < frames; i++) {
		hands[1].trig = trig;
		*now += 0.011;
		run(*now, 0.011f);
	}
}

static int t_count(int ev) {
	int n = 0, i;
	for (i = 0; i < t_n; i++) {
		n += t_ev[i] == ev;
	}
	return n;
}

static int t_ck(bool ok, const char *what) {
	if (!ok) {
		glog("laser: %d events, last at %.1f, %.1f", t_n, t_x, t_y);
	}
	return st_ck(ok, "laser", what);
}

static bool t_at(float x, float y) {
	return fabsf(t_x - x) < 0.5f && fabsf(t_y - y) < 0.5f;
}

static bool t_seq(int a, int b, int c, int d) {
	const int e[4] = { a, b, c, d };
	int i;
	for (i = 0; i < 4 && e[i] >= 0; i++) {
		if (i >= t_n || t_ev[i] != e[i]) return false;
	}
	return t_n == i;
}

int laser_selftest(void) {
	struct vr_devs keep = g_dev;
	struct ovl o, o2;
	struct laser_target t, t2;
	struct v3 ro, rd;
	double now = 100;
	int fail = 0, i;
	memset(&o, 0, sizeof(o));
	o.shown = true;
	o.alpha = 1;
	o.width = 0.5f;
	o.xf_kind = XF_ABS;
	o.xf = (HmdMatrix34_t){ { { 1, 0, 0, 0 }, { 0, 1, 0, 1.5f }, { 0, 0, 1, -1 } } };
	o2 = o;
	o2.xf.m[0][3] = 1;
	t = (struct laser_target){ &o, 500, 250, NULL, t_fn, -1 };
	t2 = (struct laser_target){ &o2, 500, 250, &o.xf, t_fn, -1 };
	laser_shutdown();
	laser_add(&t);
	laser_add(&t2);
	for (i = 0; i < 2; i++) {
		hands[i].dev = (TrackedDeviceIndex_t)(i + 1);
		tw_set(&hands[i].size, CUR_MIN, CUR_MIN, 0, 0, EZ_LINEAR);
	}
	memset(g_pose, 0, sizeof(g_pose));
	t_pose(0, 0, 1.6f, 0, false);
	t_pose(2, 0, 1.5f, 0, false);
	ray(&g_pose[2].mDeviceToAbsoluteTracking, &ro, &rd);
	fail += t_ck(fabsf(rd.x) < 1e-5f && fabsf(rd.y + RAY_SIN) < 1e-5f && fabsf(rd.z + RAY_COS) < 1e-5f, "ray tilted 40 degrees down");
	t_pose(2, 0, 1.5f, 0, true);
	t_run(&now, 1, 0);
	fail += t_ck(t_seq(LE_MOVE, -1, -1, -1) && t_at(250, 125) && fabsf(hands[1].dist - 1) < 1e-4f, "hover at the centre");
	fail += t_ck(stick_hand() < 0, "no thumbstick capture unless the target asks");
	laser_stick(&t2, true);
	fail += t_ck(stick_hand() < 0, "no capture for a target nobody points at");
	laser_stick(&t, true);
	fail += t_ck(stick_hand() == 1, "thumbstick captured for the pointing hand");
	t.hand = 0;
	fail += t_ck(stick_hand() < 0, "thumbstick capture only for the hand that owns the target");
	t.hand = 1;
	laser_stick(&t2, false);
	fail += t_ck(stick_hand() == 1, "another target cannot release the capture");
	laser_stick(&t, false);
	fail += t_ck(stick_hand() < 0, "capture released");
	t_n = 0;
	t_pose(2, -0.2f, 1.55f, 0, true);
	t_run(&now, 1, 0);
	fail += t_ck(t_seq(LE_MOVE, -1, -1, -1) && t_at(50, 75), "pixel conversion, y from the top");
	t_pose(2, 0, 1.5f, 0, true);
	t_run(&now, 1, 0);
	t_n = 0;
	t_run(&now, 3, 0.35f);
	fail += t_ck(t_n == 0, "no press below 0.4");
	t_run(&now, 1, 0.5f);
	fail += t_ck(t_seq(LE_DOWN, -1, -1, -1) && t_at(250, 125), "press at once above 0.4");
	t_run(&now, 3, 0.25f);
	fail += t_ck(t_n == 1, "still pressed above 0.2");
	t_run(&now, 1, 0.1f);
	fail += t_ck(t_seq(LE_DOWN, LE_UP, -1, -1) && t_at(250, 125), "release below 0.2");
	t_n = 0;
	t_run(&now, 1, 1);
	t_pose(2, -0.2f, 1.55f, 0, true);
	t_run(&now, 1, 1);
	t_run(&now, 1, 0);
	fail += t_ck(t_seq(LE_DOWN, LE_MOVE, LE_UP, -1) && t_at(50, 75), "press, drag, release");
	t_n = 0;
	t_pose(2, -0.245f, 1.5f, 0, true);
	t_run(&now, 1, 0);
	fail += t_ck(t_seq(LE_LEAVE, -1, -1, -1) && !hands[1].hov && t.hand < 0, "transparent pixel lets the ray pass");
	t_n = 0;
	t_run(&now, 1, 1);
	t_pose(2, 0, 1.5f, 0, true);
	t_run(&now, 2, 1);
	t_run(&now, 1, 0);
	fail += t_ck(t_seq(LE_MOVE, -1, -1, -1), "a press that started off the target does nothing");
	t_n = 0;
	t_pose(1, 0.1f, 1.5f, 0, true);
	t_pose(2, -0.245f, 1.5f, 0, true);
	t_run(&now, 1, 0);
	t_pose(2, 0, 1.5f, 0, true);
	t_run(&now, 1, 0);
	fail += t_ck(t_seq(LE_LEAVE, LE_MOVE, -1, -1) && t.hand == 0 && t_at(350, 125), "pointing does not steal a target");
	t_n = 0;
	t_run(&now, 1, 1);
	t_run(&now, 1, 0);
	fail += t_ck(t_seq(LE_LEAVE, LE_MOVE, LE_DOWN, LE_UP) && t.hand == 1 && t_at(250, 125), "trigger takes the target over");
	g_pose[1].bPoseIsValid = false;
	t_n = 0;
	t.xf = &o.xf;
	t_ret = LR_GRAB;
	t_run(&now, 1, 1);
	fail += t_ck(t_count(LE_DOWN) == 1 && t_count(LE_GRAB) == 1 && hands[1].grab && hands[1].tgrab, "press on a grab handle grabs");
	t_pose(1, 1, 1.5f, 0, true);
	hands[0].grip = 1;
	t_run(&now, 1, 1);
	fail += t_ck(t_count(LE_GRAB) == 1 && !hands[0].grab, "one hand at a time moves a transform");
	hands[0].grip = 0;
	g_pose[1].bPoseIsValid = false;
	t_pose(2, 0.1f, 1.5f, 0, true);
	t_run(&now, 90, 1);
	fail += t_ck(fabsf(o.xf.m[0][3] - 0.1f) < 0.01f && fabsf(o.xf.m[2][3] + 1) < 0.01f && o.xf.m[2][2] > 0.99f && t_count(LE_PLACE) > 0, "grabbed target follows the ray");
	t_n = 0;
	t_run(&now, 1, 0);
	fail += t_ck(t_seq(LE_DROP, -1, -1, -1) && !hands[1].grab && !hands[1].cap, "release drops it");
	t_n = 0;
	t_ret = 0;
	hands[1].grip = 1;
	t_run(&now, 1, 0);
	t_run(&now, 2, 1);
	fail += t_ck(t_count(LE_GRAB) == 1 && t_count(LE_DOWN) == 0 && hands[1].grab && !hands[1].tgrab, "grip grabs and the trigger is ignored");
	hands[1].grip = 0;
	t_run(&now, 1, 1);
	t_run(&now, 1, 0);
	fail += t_ck(t_count(LE_DROP) == 1 && t_count(LE_DOWN) == 0 && !hands[1].grab && !hands[1].cap, "grip release drops it");
	laser_shutdown();
	memset(g_pose, 0, sizeof(g_pose));
	stick_set = true;
	laser_update(0.011);
	fail += t_ck(!stick_set, "a held thumbstick is released even when nothing is shown any more");
	laser_shutdown();
	g_dev = keep;
	t_ret = t_n = 0;
	return fail;
}
#endif
