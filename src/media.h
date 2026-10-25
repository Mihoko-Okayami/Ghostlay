#ifndef MEDIA_H
#define MEDIA_H

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

#include "main.h"

enum { MEDIA_PREV, MEDIA_TOGGLE, MEDIA_NEXT };

struct media_snap {
	uint32_t version;
	bool active, playing, has_cover;
	double pos, dur, stamp;
	WCHAR title[160], artist[128];
	uint8_t cover[ICON_PX * ICON_PX * 4];
};

void media_init(void);
void media_enable(bool on);
void media_shutdown(void);
uint32_t media_version(void);
const struct media_snap *media_lock(void);
void media_unlock(void);
double media_pos(const struct media_snap *m, double now);
void media_cmd(int cmd);

#ifdef GL_TESTS
int media_selftest(void);
#endif

#endif
