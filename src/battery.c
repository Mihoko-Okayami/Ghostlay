#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "audio.h"
#include "battery.h"
#include "cfg.h"
#include "lang.h"
#include "main.h"
#include "notif.h"
#include "vr.h"

#define BAT_PERIOD 5.0

struct track {
	double seen;
	int prev;
	uint64_t fired[2];
};

static const char *const roles[BR_COUNT] = { "none", "chest", "waist", "leftshoulder", "rightshoulder", "leftelbow", "rightelbow", "leftwrist", "rightwrist", "handed", "leftknee", "rightknee", "leftankle", "rightankle", "leftfoot", "rightfoot", "camera", "keyboard" };
struct bat_snap g_bat;
static struct track tk[VR_MAX_DEV];
static double last;
static uint32_t gen;

int bat_level(float pct) {
	int p = (int)lroundf(pct * 100);
	if (p <= g_cfg.bat_tiers[g_cfg.bat_ntiers - 1]) return BL_CRITICAL;
	return p <= g_cfg.bat_tiers[0] ? BL_WARNING : BL_NORMAL;
}

static void forget(struct track *t) {
	t->seen = -1;
	t->prev = -1;
}

static int check(struct track *t, const struct bat_dev *b, double now, bool ok) {
	int p = (int)lroundf(b->pct * 100), prev = t->prev, hit = -1, i, v;
	uint64_t m, *w;
	bool f;
	if (t->seen < 0) {
		t->seen = now;
	}
	t->prev = b->charging ? -1 : p;
	for (i = 0; i < g_cfg.bat_ntiers; i++) {
		v = g_cfg.bat_tiers[i];
		m = 1ull << (v & 63);
		w = &t->fired[v >> 6 & 1];
		f = *w & m;
		if (f && p >= v + 5) {
			*w &= ~m;
		} else if (!f && ok && !b->charging && prev >= 0 && prev <= v && p <= v && now - t->seen >= 10) {
			*w |= m;
			hit = i;
		}
	}
	return hit;
}

int bat_role_str(int role) {
	static const int s[BR_COUNT] = { [BR_NONE] = -1, [BR_CHEST] = S_W_CHEST, [BR_WAIST] = S_W_WAIST, [BR_LEFT_SHOULDER] = S_W_LEFT_SHOULDER, [BR_RIGHT_SHOULDER] = S_W_RIGHT_SHOULDER, [BR_LEFT_ELBOW] = S_W_LEFT_ELBOW, [BR_RIGHT_ELBOW] = S_W_RIGHT_ELBOW, [BR_LEFT_WRIST] = S_W_LEFT_WRIST, [BR_RIGHT_WRIST] = S_W_RIGHT_WRIST, [BR_HANDED] = S_W_HANDED, [BR_LEFT_KNEE] = S_W_LEFT_KNEE, [BR_RIGHT_KNEE] = S_W_RIGHT_KNEE, [BR_LEFT_ANKLE] = S_W_LEFT_ANKLE, [BR_RIGHT_ANKLE] = S_W_RIGHT_ANKLE, [BR_LEFT_FOOT] = S_W_LEFT_FOOT, [BR_RIGHT_FOOT] = S_W_RIGHT_FOOT, [BR_CAMERA] = S_W_CAMERA, [BR_KEYBOARD] = S_W_KEYBOARD };
	return s[role];
}

static void build(struct notif *n, int dev, int role, int pct, bool critical) {
	notif_blank(n, NO_UDP);
	n->kind = critical ? NK_CRITICAL : NK_NORMAL;
	n->icon = NI_BATTERY;
	n->sfx = g_cfg.bat_sound ? SFX_ALERT : SFX_NONE;
	n->timeout = 6;
	n->bat_dev = dev;
	n->bat_role = role;
	n->bat_pct = pct;
}

static bool alert(int dev, int role, int pct, bool critical) {
	static struct notif n;
	build(&n, dev, role, pct, critical);
	glog("battery %s: device %d at %d%%", critical ? "critical" : "low", dev, pct);
	return notif_push(&n);
}

static bool same_name(const char *a, const char *b) {
	for (;;) {
		while (*a == '_') {
			a++;
		}
		if ((*a >= 'A' && *a <= 'Z' ? *a + 32 : *a) != *b) return false;
		if (!*b) return true;
		a++;
		b++;
	}
}

static int role_of(const char *s) {
	int i;
	if (!_strnicmp(s, "TrackerRole_", 12)) {
		s += 12;
	} else if (!_strnicmp(s, "vive_tracker_", 13)) {
		s += 13;
	} else {
		return BR_NONE;
	}
	for (i = 1; i < BR_COUNT; i++) {
		if (same_name(s, roles[i])) return i;
	}
	return BR_NONE;
}

static int role_read(TrackedDeviceIndex_t i) {
	char t[128], k[144], v[64];
	int r = BR_NONE;
	if (vr_prop_str(i, ETrackedDeviceProperty_Prop_RegisteredDeviceType_String, t, sizeof(t))) {
		snprintf(k, sizeof(k), "/devices/%s", t);
		if (vr_setting_str("trackers", k, v, sizeof(v))) {
			r = role_of(v);
		}
	}
	if (r == BR_NONE && vr_prop_str(i, ETrackedDeviceProperty_Prop_ControllerType_String, v, sizeof(v))) {
		r = role_of(v);
	}
	return r;
}

static struct bat_dev rd(TrackedDeviceIndex_t i) {
	struct bat_dev b = { .idx = i };
	float p;
	if (i >= VR_MAX_DEV || !g_pose[i].bDeviceIsConnected) return b;
	p = vr_prop_bool(i, ETrackedDeviceProperty_Prop_DeviceProvidesBatteryStatus_Bool) ? vr_prop_float(i, ETrackedDeviceProperty_Prop_DeviceBatteryPercentage_Float, -1) : -1;
	b.present = true;
	b.known = p >= 0;
	b.pct = b.known ? roundf(fminf(p, 1) * 100) / 100 : 0;
	b.charging = b.known && vr_prop_bool(i, ETrackedDeviceProperty_Prop_DeviceIsCharging_Bool);
	return b;
}

static struct bat_dev rd_trk(TrackedDeviceIndex_t i) {
	struct bat_dev b = rd(i);
	if (b.present) {
		b.role = role_read(i);
	}
	return b;
}

static int rank(const struct bat_dev *b) {
	return b->role == BR_NONE ? BR_COUNT : b->role;
}

static bool before(const struct bat_dev *a, const struct bat_dev *b) {
	return rank(a) != rank(b) ? rank(a) < rank(b) : a->idx < b->idx;
}

void bat_summary(struct bat_snap *s) {
	float sum = 0;
	int i;
	s->trk_known = 0;
	for (i = 0; i < s->ntrk; i++) {
		if (!s->trk[i].known) continue;
		s->trk_known++;
		sum += s->trk[i].pct;
	}
	s->trk_avg = s->trk_known ? sum / s->trk_known : -1;
}

static void order(struct bat_snap *s) {
	struct bat_dev b;
	int i, j;
	for (i = 1; i < s->ntrk; i++) {
		b = s->trk[i];
		for (j = i; j > 0 && before(&b, &s->trk[j - 1]); j--) {
			s->trk[j] = s->trk[j - 1];
		}
		s->trk[j] = b;
	}
	bat_summary(s);
}

static bool armed(void) {
	return g_app.worn && g_cfg.bat_alerts && g_cfg.notif_on;
}

static bool due(double now, double prev, uint32_t g, uint32_t pg) {
	return g != pg || now - prev >= BAT_PERIOD;
}

static int feed(const struct bat_dev *b, unsigned int cls, double now, bool ok, uint64_t *vis) {
	if (!b->present || !b->known) return -1;
	*vis |= 1ull << b->idx;
	return check(&tk[b->idx], b, now, ok && (g_cfg.bat_devices & cls));
}

static const struct bat_dev *dev_of(const struct bat_snap *s, int d) {
	return d == BD_HMD ? &s->hmd : d == BD_LEFT ? &s->left : d == BD_RIGHT ? &s->right : &s->trk[d - BD_TRACKER];
}

static void scan(const struct bat_snap *s, double now, bool ok, int *hit) {
	uint64_t vis = 0;
	int i;
	for (i = 0; i < BD_TRACKER + s->ntrk; i++) {
		hit[i] = feed(dev_of(s, i), i == BD_HMD ? BDEV_HMD : i < BD_TRACKER ? BDEV_CTRL : BDEV_TRK, now, ok, &vis);
	}
	for (i = 0; i < VR_MAX_DEV; i++) {
		if (!(vis >> i & 1)) {
			forget(&tk[i]);
		}
	}
}

static void deliver(const struct bat_dev *b, int dev, int t) {
	int v;
	if (t < 0 || alert(dev, b->role, (int)lroundf(b->pct * 100), t == g_cfg.bat_ntiers - 1)) return;
	v = g_cfg.bat_tiers[t];
	tk[b->idx].fired[v >> 6 & 1] &= ~(1ull << (v & 63));
}

static bool same(const struct bat_dev *a, const struct bat_dev *b) {
	return a->present == b->present && a->known == b->known && a->charging == b->charging && a->pct == b->pct && a->idx == b->idx && a->role == b->role;
}

static bool same_snap(const struct bat_snap *a, const struct bat_snap *b) {
	int i;
	if (a->ntrk != b->ntrk || !same(&a->hmd, &b->hmd) || !same(&a->left, &b->left) || !same(&a->right, &b->right)) return false;
	for (i = 0; i < a->ntrk; i++) {
		if (!same(&a->trk[i], &b->trk[i])) return false;
	}
	return true;
}

static void reset(void) {
	int i;
	for (i = 0; i < VR_MAX_DEV; i++) {
		tk[i] = (struct track){ -1, -1, { 0, 0 } };
	}
	g_bat = (struct bat_snap){ .version = g_bat.version + 1, .hmd.idx = VR_NO_DEV, .left.idx = VR_NO_DEV, .right.idx = VR_NO_DEV, .trk_avg = -1 };
	last = -1e9;
}

void battery_init(void) {
	reset();
}

void battery_update(double now) {
	struct bat_snap s;
	struct bat_dev b;
	int hit[BD_TRACKER + MAX_TRACKERS], i;
	if (!due(now, last, g_dev.gen, gen)) return;
	last = now;
	gen = g_dev.gen;
	s = (struct bat_snap){ .version = g_bat.version, .hmd = rd(g_dev.hmd), .left = rd(g_dev.left), .right = rd(g_dev.right) };
	for (i = 0; i < g_dev.ntrk; i++) {
		b = rd_trk(g_dev.trk[i]);
		if (b.present) {
			s.trk[s.ntrk++] = b;
		}
	}
	order(&s);
	scan(&s, now, armed(), hit);
	for (i = 0; i < BD_TRACKER + s.ntrk; i++) {
		deliver(dev_of(&s, i), i, hit[i]);
	}
	if (same_snap(&s, &g_bat)) return;
	s.version++;
	g_bat = s;
}

void battery_test_alert(void) {
	alert(BD_RIGHT, BR_NONE, g_cfg.bat_tiers[g_cfg.bat_ntiers - 1], true);
}

void battery_shutdown(void) {
	reset();
}

#ifdef GL_TESTS
int battery_selftest(void) {
	static const int sets[][MAX_TIERS + 1] = { { 3, 35, 25, 15 }, { 3, 50, 30, 10 }, { 1, 35 } };
	static const struct { uint8_t trk, set; float t; int8_t pct; bool chg, ok; int8_t want; } seq[] = {
		{ 0, 1, 0, 40, 0, 1, -1 }, { 0, 0, 5, 10, 0, 1, -1 }, { 0, 0, 10, 40, 0, 1, -1 }, { 0, 0, 15, 36, 0, 1, -1 }, { 0, 0, 20, 35, 0, 1, -1 }, { 0, 0, 25, 34, 0, 1, 0 },
		{ 0, 0, 30, 30, 0, 1, -1 }, { 0, 0, 35, 25, 0, 1, -1 }, { 0, 0, 40, 24, 0, 1, 1 }, { 0, 0, 45, 20, 0, 1, -1 }, { 0, 0, 50, 14, 1, 1, -1 }, { 0, 0, 55, 13, 1, 1, -1 },
		{ 0, 0, 60, 13, 0, 1, -1 }, { 0, 0, 65, 12, 0, 1, 2 }, { 0, 0, 70, 11, 0, 1, -1 }, { 0, 0, 75, 19, 1, 1, -1 }, { 0, 0, 80, 20, 1, 1, -1 }, { 0, 0, 85, 29, 1, 1, -1 },
		{ 0, 0, 90, 30, 1, 1, -1 }, { 0, 0, 95, 30, 0, 1, -1 }, { 0, 0, 100, 25, 0, 1, -1 }, { 0, 0, 105, 24, 0, 1, 1 }, { 0, 0, 110, 15, 0, 0, -1 }, { 0, 0, 115, 14, 0, 0, -1 },
		{ 0, 0, 120, 14, 0, 1, 2 }, { 0, 0, 125, 14, 0, 1, -1 },
		{ 1, 0, 0, 10, 0, 1, -1 }, { 1, 0, 1, 10, 0, 1, -1 }, { 1, 0, 5, 10, 0, 1, -1 }, { 1, 0, 10, 10, 0, 1, 2 }, { 1, 0, 15, 9, 0, 1, -1 }, { 1, 0, 20, 30, 0, 1, -1 },
		{ 1, 0, 25, -1, 0, 1, -1 }, { 1, 0, 30, 10, 0, 1, -1 }, { 1, 0, 35, 10, 0, 1, -1 }, { 1, 0, 40, 10, 0, 1, 2 }, { 1, 0, 45, 0, 0, 1, -1 },
		{ 2, 0, 0, 30, 0, 1, -1 }, { 2, 0, 5, 30, 0, 1, -1 }, { 2, 0, 10, 30, 0, 1, 0 }, { 2, 2, 15, 30, 0, 1, 1 }, { 2, 0, 20, 30, 0, 1, -1 }, { 2, 3, 25, 30, 0, 1, -1 },
		{ 2, 3, 30, 41, 0, 1, -1 }, { 2, 3, 35, 35, 0, 1, -1 }, { 2, 3, 40, 35, 0, 1, 0 }, { 2, 1, 45, 35, 0, 1, -1 }, { 2, 1, 50, 24, 0, 1, -1 }, { 2, 1, 55, 24, 0, 1, 1 },
		{ 2, 0, 60, 14, 0, 1, -1 }, { 2, 0, 65, 13, 1, 1, -1 }, { 2, 0, 70, 13, 0, 1, -1 }, { 2, 0, 75, 12, 0, 1, 2 },
	};
	static const struct { float pct; int want; } lv[] = { { 1, BL_NORMAL }, { 0.36f, BL_NORMAL }, { 0.35f, BL_WARNING }, { 0.16f, BL_WARNING }, { 0.15f, BL_CRITICAL }, { 0, BL_CRITICAL } };
	static const struct { double t; uint32_t g; bool want; } poll[] = { { 0, 1, true }, { 1, 1, false }, { 4.9, 1, false }, { 5, 1, true }, { 7, 2, true }, { 8, 2, false }, { 11.9, 2, false }, { 12, 2, true } };
	static const struct { const char *s; int want; } rl[] = {
		{ "TrackerRole_LeftFoot", BR_LEFT_FOOT }, { "TrackerRole_Waist", BR_WAIST }, { "trackerrole_chest", BR_CHEST }, { "TrackerRole_RightAnkle", BR_RIGHT_ANKLE }, { "TrackerRole_None", BR_NONE }, { "TrackerRole_Handed", BR_HANDED },
		{ "vive_tracker_left_foot", BR_LEFT_FOOT }, { "vive_tracker_handed", BR_HANDED }, { "vive_tracker_right_shoulder", BR_RIGHT_SHOULDER }, { "vive_tracker", BR_NONE }, { "TrackerRole_LeftFootx", BR_NONE }, { "lighthouse", BR_NONE }, { "", BR_NONE },
	};
	static const TrackedDeviceIndex_t want_idx[] = { 5, 9, 4, 3, 7 };
	static const struct { double t; int8_t all, last, want_all, want_last; } many[] = { { 0, 40, 40, -1, -1 }, { 5, 34, 34, -1, -1 }, { 10, 34, 34, 0, 0 }, { 15, 34, 14, -1, -1 }, { 20, 34, 14, -1, 2 }, { 25, 34, 14, -1, -1 } };
	static struct notif n;
	static struct bat_snap sn;
	struct cfg saved = g_cfg;
	struct track t[3];
	struct bat_dev b;
	double prev = -1e9;
	bool worn = g_app.worn;
	uint32_t pg = 0;
	int hit[BD_TRACKER + MAX_TRACKERS], fail = 0, i, j, got;
	for (i = 0; i < (int)ARRAYSIZE(poll); i++) {
		got = due(poll[i].t, prev, poll[i].g, pg);
		if (got) {
			prev = poll[i].t;
			pg = poll[i].g;
		}
		if (got != poll[i].want) {
			glog("battery: poll at %.1f s (gen %u) is %d, expected %d", poll[i].t, poll[i].g, got, poll[i].want);
			fail++;
		}
	}
	for (i = 0; i < (int)ARRAYSIZE(rl); i++) {
		if (role_of(rl[i].s) != rl[i].want) {
			glog("battery: role of \"%s\" is %d, expected %d", rl[i].s, role_of(rl[i].s), rl[i].want);
			fail++;
		}
	}
	sn = (struct bat_snap){ .ntrk = 5 };
	sn.trk[0] = (struct bat_dev){ true, true, false, 0.5f, 7, BR_NONE };
	sn.trk[1] = (struct bat_dev){ true, true, true, 0.2f, 4, BR_LEFT_FOOT };
	sn.trk[2] = (struct bat_dev){ true, true, false, 0.8f, 9, BR_WAIST };
	sn.trk[3] = (struct bat_dev){ true, false, false, 0, 3, BR_NONE };
	sn.trk[4] = (struct bat_dev){ true, true, false, 0.3f, 5, BR_CHEST };
	order(&sn);
	for (i = 0; i < sn.ntrk; i++) {
		if (sn.trk[i].idx != want_idx[i]) {
			glog("battery: tracker %d is device %u, expected %u", i, sn.trk[i].idx, want_idx[i]);
			fail++;
		}
	}
	if (sn.trk_known != 4 || fabsf(sn.trk_avg - 0.45f) > 1e-5f) {
		glog("battery: tracker summary is %d known, average %.3f", sn.trk_known, sn.trk_avg);
		fail++;
	}
	sn.ntrk = 0;
	order(&sn);
	if (sn.trk_known || sn.trk_avg != -1) {
		glog("battery: empty tracker summary is wrong");
		fail++;
	}
	sn = (struct bat_snap){ .ntrk = MAX_TRACKERS };
	for (i = 0; i < MAX_TRACKERS; i++) {
		sn.trk[i] = (struct bat_dev){ true, i % 5 > 0, false, (float)i / MAX_TRACKERS, (TrackedDeviceIndex_t)(VR_MAX_DEV - 1 - i), i * 7 % BR_COUNT };
	}
	order(&sn);
	for (i = 1; i < MAX_TRACKERS; i++) {
		if (before(&sn.trk[i], &sn.trk[i - 1])) {
			glog("battery: tracker %d of %d is out of order", i + 1, MAX_TRACKERS);
			fail++;
		}
	}
	if (sn.trk_known != MAX_TRACKERS - (MAX_TRACKERS + 4) / 5) {
		glog("battery: %d of %d trackers report a level", sn.trk_known, MAX_TRACKERS);
		fail++;
	}
	g_cfg.bat_ntiers = 3;
	memcpy(g_cfg.bat_tiers, sets[0] + 1, sizeof(g_cfg.bat_tiers));
	g_cfg.bat_devices = BDEV_HMD | BDEV_CTRL | BDEV_TRK;
	reset();
	for (i = 0; i < (int)ARRAYSIZE(many); i++) {
		for (j = 0; j < MAX_TRACKERS; j++) {
			sn.trk[j] = (struct bat_dev){ true, true, false, (j == MAX_TRACKERS - 1 ? many[i].last : many[i].all) / 100.0f, (TrackedDeviceIndex_t)(VR_MAX_DEV - MAX_TRACKERS + j), BR_NONE };
		}
		scan(&sn, many[i].t, true, hit);
		for (j = 0; j < BD_TRACKER + MAX_TRACKERS; j++) {
			got = j < BD_TRACKER ? -1 : j == BD_TRACKER + MAX_TRACKERS - 1 ? many[i].want_last : many[i].want_all;
			if (hit[j] != got) {
				glog("battery: alert step %d, device %d fired tier %d, expected %d", i, j, hit[j], got);
				fail++;
			}
		}
	}
	reset();
	g_cfg = saved;
	for (i = 0; i < 3; i++) {
		t[i] = (struct track){ -1, -1, { 0, 0 } };
	}
	for (i = 0; i < (int)ARRAYSIZE(seq); i++) {
		if (seq[i].set) {
			g_cfg.bat_ntiers = sets[seq[i].set - 1][0];
			memcpy(g_cfg.bat_tiers, sets[seq[i].set - 1] + 1, sizeof(g_cfg.bat_tiers));
		}
		b = (struct bat_dev){ true, true, seq[i].chg, seq[i].pct / 100.0f, (TrackedDeviceIndex_t)i, BR_NONE };
		got = -1;
		if (seq[i].pct < 0) {
			forget(&t[seq[i].trk]);
		} else {
			got = check(&t[seq[i].trk], &b, seq[i].t, seq[i].ok);
		}
		if (got != seq[i].want) {
			glog("battery: step %d (device %d, t %.0f, %d%%) fired tier %d, expected %d", i, seq[i].trk, seq[i].t, seq[i].pct, got, seq[i].want);
			fail++;
		}
	}
	g_cfg = saved;
	g_cfg.bat_ntiers = 3;
	memcpy(g_cfg.bat_tiers, sets[0] + 1, sizeof(g_cfg.bat_tiers));
	for (i = 0; i < (int)ARRAYSIZE(lv); i++) {
		if (bat_level(lv[i].pct) != lv[i].want) {
			glog("battery: level of %.2f is %d, expected %d", lv[i].pct, bat_level(lv[i].pct), lv[i].want);
			fail++;
		}
	}
	g_cfg.bat_sound = true;
	build(&n, BD_TRACKER + MAX_TRACKERS - 1, BR_LEFT_FOOT, 15, true);
	if (n.kind != NK_CRITICAL || n.icon != NI_BATTERY || n.sfx != SFX_ALERT || n.bat_dev != BD_TRACKER + MAX_TRACKERS - 1 || n.bat_role != BR_LEFT_FOOT || n.bat_pct != 15 || n.source[0] || n.title[0] || n.body[0]) {
		glog("battery: critical tracker notification is wrong");
		fail++;
	}
	g_cfg.bat_sound = false;
	build(&n, BD_LEFT, BR_NONE, 30, false);
	if (n.kind != NK_NORMAL || n.sfx != SFX_NONE || n.bat_pct != 30 || n.bat_dev != BD_LEFT || n.bat_role != BR_NONE) {
		glog("battery: warning controller notification is wrong");
		fail++;
	}
	g_app.worn = g_cfg.bat_alerts = g_cfg.notif_on = true;
	got = armed();
	g_cfg.notif_on = false;
	if (!got || armed()) {
		glog("battery: alerts are not stopped by the notifications switch");
		fail++;
	}
	g_app.worn = worn;
	g_cfg = saved;
	return fail;
}
#endif
