#ifndef NOTIF_H
#define NOTIF_H

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

#include "main.h"

#define HIST_CAP 20
#define HIST_PX (ICON_PX / 2)

enum { NK_NORMAL, NK_CRITICAL };

enum { NI_DEFAULT, NI_WARNING, NI_ERROR, NI_IMAGE, NI_BATTERY, NI_INITIAL };

enum { NO_UDP, NO_DISCORD, NO_TEST };

struct gx_bmp;

struct notif {
	int kind, icon, sfx;
	float timeout, volume, opacity;
	int bat_dev, bat_role, bat_pct, origin, count;
	WCHAR title[128], body[512], source[64];
	uint8_t img[ICON_PX * ICON_PX * 4];
};

struct notif_hist {
	uint32_t id;
	bool missed, seen;
	int kind, icon, origin, count, bat_dev, bat_role, bat_pct;
	SYSTEMTIME at;
	WCHAR title[128], body[160], source[64];
	uint8_t img[HIST_PX * HIST_PX * 4];
};

void notif_init(void);
void notif_shutdown(void);
void notif_blank(struct notif *n, int origin);
void notif_config(bool on);
bool notif_push(const struct notif *n);
bool notif_has_room(void);
void notif_test(void);
bool notif_update(double dt);
void notif_bat_text(int dev, int role, int pct, WCHAR *name, WCHAR *value);
void notif_medallion(int icon, int kind, const WCHAR *title, struct gx_bmp *b, float cx, float cy, float r);
bool notif_dnd(void);
void notif_dnd_set(bool on);
int notif_hist_read(int n, struct notif_hist *out);
uint32_t notif_hist_version(void);
void notif_hist_open(bool open);
bool notif_hist_seen(uint32_t id);
int notif_badge(WCHAR *b, int cap);

#ifdef GL_TESTS
extern bool (*notif_tap)(const struct notif *n);

int notif_selftest(void);
void notif_shrink(uint8_t *d, const uint8_t *s);
bool notif_render_card(const struct notif *n, float f, const WCHAR *path);
int notif_render_samples(const WCHAR *dir);
#endif

#endif
