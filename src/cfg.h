#ifndef CFG_H
#define CFG_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_TIERS 5
#define DEFAULT_PORT 42069

enum { WRIST_LEFT, WRIST_RIGHT };

enum { BDEV_HMD = 1, BDEV_CTRL = 2, BDEV_TRK = 4 };

enum { CR_WATCH_SIZE, CR_WATCH_ANGLE, CR_WATCH_OPACITY, CR_NOTIF_DIST, CR_NOTIF_ANGLE, CR_NOTIF_HOFF, CR_NOTIF_SIZE, CR_VOLUME, CR_DUR_MIN, CR_DUR_MAX, CR_COUNT };

struct cfg {
	int lang;
	bool autolaunch;
	int wrist;
	int watch_size, watch_opacity, watch_angle;
	bool clock24;
	uint32_t accent;
	bool media;
	bool notif_on;
	float notif_dist, notif_hoff;
	int notif_angle, notif_size;
	int dur_min, dur_max;
	bool notif_sound;
	int notif_volume;
	bool udp_on;
	int udp_port;
	bool discord;
	bool bat_alerts;
	int bat_ntiers;
	int bat_tiers[MAX_TIERS];
	unsigned int bat_devices;
	bool bat_sound;
};

extern struct cfg g_cfg;
extern uint32_t g_cfg_gen;

void cfg_defaults(struct cfg *c);
void cfg_clamp(struct cfg *c);
float cfg_norm(int r);
float cfg_snap(int r, float t);
bool cfg_set(int r, float v);
void cfg_load(void);
void cfg_changed(void);
void cfg_update(double now);
void cfg_flush(void);

#ifdef GL_TESTS
int cfg_selftest(void);
#endif

#endif
