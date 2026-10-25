#ifndef LASER_H
#define LASER_H

#include <stdbool.h>

#include "vr.h"

enum { LE_PASS, LE_MOVE, LE_LEAVE, LE_DOWN, LE_UP, LE_GRAB, LE_DROP, LE_PLACE };

enum { LR_HAPTIC = 1, LR_GRAB = 2 };

struct laser_target {
	struct ovl *ov;
	float w, h;
	HmdMatrix34_t *xf;
	int (*fn)(struct laser_target *t, int ev, float x, float y);
	int hand;
};

void laser_init(void);
void laser_shutdown(void);
void laser_add(struct laser_target *t);
void laser_stick(struct laser_target *t, bool on);
void laser_stick_pos(struct laser_target *t, float *x, float *y);
bool laser_update(double dt);

#ifdef GL_TESTS
int laser_selftest(void);
#endif

#endif
