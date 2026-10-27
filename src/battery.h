#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>
#include <stdint.h>

#include "vr.h"

enum { BD_HMD, BD_LEFT, BD_RIGHT, BD_TRACKER };

enum { BL_NORMAL, BL_WARNING, BL_CRITICAL };

enum { BR_NONE, BR_CHEST, BR_WAIST, BR_LEFT_SHOULDER, BR_RIGHT_SHOULDER, BR_LEFT_ELBOW, BR_RIGHT_ELBOW, BR_LEFT_WRIST, BR_RIGHT_WRIST, BR_HANDED, BR_LEFT_KNEE, BR_RIGHT_KNEE, BR_LEFT_ANKLE, BR_RIGHT_ANKLE, BR_LEFT_FOOT, BR_RIGHT_FOOT, BR_CAMERA, BR_KEYBOARD, BR_COUNT };

struct bat_dev {
	bool present, known, charging;
	float pct;
	TrackedDeviceIndex_t idx;
	int role;
};

struct bat_snap {
	uint32_t version;
	struct bat_dev hmd, left, right;
	int ntrk;
	struct bat_dev trk[MAX_TRACKERS];
	int trk_known;
	float trk_avg;
};

extern struct bat_snap g_bat;

int bat_level(float pct);
void battery_init(void);
void battery_update(double now);
void battery_test_alert(void);
int bat_role_str(int role);
void battery_shutdown(void);

#ifdef GL_TESTS
void bat_summary(struct bat_snap *s);
int battery_selftest(void);
#endif

#endif
