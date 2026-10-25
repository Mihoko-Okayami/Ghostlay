#ifndef MAIN_H
#define MAIN_H

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

#ifndef VERSION
#define VERSION "dev"
#endif

#define VERSION_PREFIX (*VERSION >= '0' && *VERSION <= '9' ? "v" : "")
#define GL_APP_KEY "ghostlay.overlay"
#define ICON_PX 96
#define PI_F 3.14159265f
#define DEG (PI_F / 180)

struct app {
	HANDLE wake;
	bool vr, worn, quit;
	double t0, vr_t0;
	WCHAR dir[MAX_PATH];
	WCHAR data[MAX_PATH];
};

struct worker {
	HANDLE th, stop;
	bool stuck;
};

extern struct app g_app;

double now_s(void);
int app_uptime(double now);
void app_wake(void);
void app_quit(void);
void glog(const char *fmt, ...);
void to_utf8(const WCHAR *s, char *out, int cap);
uint32_t utf8_next(const char **pp, const char *e);
int utf16_put(WCHAR *w, int n, int cap, uint32_t c);
int hex_digit(int c);
const char *json_str(const char *p, const char *e, WCHAR *w, char *a, int cap);
int clampi(int v, int lo, int hi);
float clampf(float v, float lo, float hi);
void app_name(const WCHAR *id, WCHAR *out, int cap);
void put(WCHAR *d, int cap, const WCHAR *s);
void write_file(const WCHAR *path, const char *s, int n);
bool worker_start(struct worker *w, HANDLE stop, LPTHREAD_START_ROUTINE fn, void *arg);
bool worker_stop(struct worker *w, DWORD ms, const char *name);

#ifdef GL_TESTS
int st_ck(bool ok, const char *mod, const char *what);
#endif

#endif
