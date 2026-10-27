#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include "audio.h"
#include "battery.h"
#include "cfg.h"
#include "gfx.h"
#include "lang.h"
#include "laser.h"
#include "main.h"
#include "media.h"
#include "notif.h"
#include "panel.h"
#include "udp.h"
#include "vr.h"
#include "watch.h"
#include "winnotif.h"

struct app g_app;
static LARGE_INTEGER qpf;
static HANDLE log_file = INVALID_HANDLE_VALUE, log_con;
static SRWLOCK log_lock = SRWLOCK_INIT;
static uint32_t applied_gen;
static const char esc[] = "\"\\/bfnrt", unesc[] = "\"\\/\b\f\n\r\t";

double now_s(void) {
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart / (double)qpf.QuadPart;
}

int app_uptime(double now) {
	return g_app.vr && now > g_app.vr_t0 ? (int)(now - g_app.vr_t0) : 0;
}

void app_wake(void) {
	if (g_app.wake) {
		SetEvent(g_app.wake);
	}
}

void app_quit(void) {
	g_app.quit = true;
	app_wake();
}

void to_utf8(const WCHAR *s, char *out, int cap) {
	if (cap <= 0) return;
	if (WideCharToMultiByte(CP_UTF8, 0, s, -1, out, cap, NULL, NULL) <= 0) {
		out[0] = 0;
	}
}

uint32_t utf8_next(const char **pp, const char *e) {
	const unsigned char *p = (const unsigned char *)*pp;
	uint32_t c = *p++;
	int n = c < 0xC2 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : c < 0xF5 ? 3 : 0, i;
	uint32_t min = n == 1 ? 0x80 : n == 2 ? 0x800 : 0x10000;
	c &= 0x3Fu >> n;
	for (i = 0; i < n; i++, p++) {
		if ((const char *)p >= e || (*p & 0xC0) != 0x80) {
			n = 0;
			break;
		}
		c = c << 6 | (*p & 0x3F);
	}
	if (!n || c < min || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000)) {
		(*pp)++;
		return 0xFFFD;
	}
	*pp = (const char *)p;
	return c;
}

int utf16_put(WCHAR *w, int n, int cap, uint32_t c) {
	int k = c > 0xFFFF ? 2 : 1;
	if (n + k >= cap) return cap - 1;
	if (k == 2) {
		w[n++] = (WCHAR)(0xD800 + ((c - 0x10000) >> 10));
		w[n++] = (WCHAR)(0xDC00 + (c & 0x3FF));
	} else {
		w[n++] = (WCHAR)c;
	}
	w[n] = 0;
	return n;
}

int hex_digit(int c) {
	return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

static int hex4(const char *p, const char *e) {
	int v = 0, i, h;
	if (e - p < 4) return -1;
	for (i = 0; i < 4; i++) {
		h = hex_digit(p[i]);
		if (h < 0) return -1;
		v = v << 4 | h;
	}
	return v;
}

const char *json_str(const char *p, const char *e, WCHAR *w, char *a, int cap) {
	int n = 0, v, lo;
	uint32_t c;
	const char *m;
	if (p >= e || *p++ != '"') return NULL;
	while (p < e && *p != '"') {
		c = (unsigned char)*p;
		if (c == '\\' && e - p > 1 && p[1] == 'u') {
			v = hex4(p + 2, e);
			lo = -1;
			if (v < 0) return NULL;
			p += 6;
			if (v >= 0xD800 && v < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
				lo = hex4(p + 2, e);
			}
			if (lo >= 0xDC00 && lo < 0xE000) {
				c = 0x10000 + ((uint32_t)(v - 0xD800) << 10) + (uint32_t)(lo - 0xDC00);
				p += 6;
			} else {
				c = v >= 0xD800 && v < 0xE000 ? 0xFFFD : (uint32_t)v;
			}
		} else if (c == '\\') {
			m = e - p > 1 && p[1] ? strchr(esc, p[1]) : NULL;
			if (!m) return NULL;
			c = (unsigned char)unesc[m - esc];
			p += 2;
		} else if (c >= 0x80) {
			c = utf8_next(&p, e);
		} else {
			p++;
		}
		if (w) {
			n = utf16_put(w, n, cap, c);
		} else if (a && n + 1 < cap) {
			a[n++] = c < 0x80 ? (char)c : '?';
		}
	}
	if (p >= e) return NULL;
	if (w) {
		w[n] = 0;
	}
	if (a) {
		a[n] = 0;
	}
	return p + 1;
}

int clampi(int v, int lo, int hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

float clampf(float v, float lo, float hi) {
	return v >= lo ? v <= hi ? v : hi : lo;
}

void app_name(const WCHAR *id, WCHAR *out, int cap) {
	const WCHAR *b = id, *p;
	int n;
	for (p = id; *p; p++) {
		if (*p == L'!' || *p == L'\\' || *p == L'/') {
			b = p + 1;
		}
	}
	n = (int)wcslen(b);
	if (n > 4 && !_wcsicmp(b + n - 4, L".exe")) {
		n -= 4;
	}
	n = n < cap - 1 ? n : cap - 1;
	memcpy(out, b, n * sizeof(*out));
	out[n] = 0;
}

void put(WCHAR *d, int cap, const WCHAR *s) {
	wcsncpy(d, s, cap - 1);
	d[cap - 1] = 0;
}

void write_file(const WCHAR *path, const char *s, int n) {
	static char old[8192];
	DWORD r = 0;
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (h != INVALID_HANDLE_VALUE) {
		ReadFile(h, old, sizeof(old), &r, NULL);
		CloseHandle(h);
	}
	if (r == (DWORD)n && !memcmp(old, s, n)) return;
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE || !WriteFile(h, s, n, &r, NULL) || r != (DWORD)n) {
		glog("cannot write %ls", path);
	}
	if (h != INVALID_HANDLE_VALUE) {
		CloseHandle(h);
	}
}

bool worker_start(struct worker *w, HANDLE stop, LPTHREAD_START_ROUTINE fn, void *arg) {
	w->th = stop ? CreateThread(NULL, 0, fn, arg, 0, NULL) : NULL;
	w->stop = w->th ? stop : NULL;
	return w->th != NULL;
}

bool worker_stop(struct worker *w, DWORD ms, const char *name) {
	bool done;
	if (!w->th) return true;
	SetEvent(w->stop);
	done = WaitForSingleObject(w->th, ms) == WAIT_OBJECT_0;
	if (!done) {
		glog("%s worker stuck, abandoning", name);
		w->stuck = true;
	}
	CloseHandle(w->th);
	w->th = NULL;
	return done;
}

void glog(const char *fmt, ...) {
	char b[1024];
	int n, k;
	va_list a;
	DWORD w;
	n = snprintf(b, sizeof(b), "%9.3f ", now_s() - g_app.t0);
	va_start(a, fmt);
	k = vsnprintf(b + n, sizeof(b) - n - 2, fmt, a);
	va_end(a);
	n = k < 0 ? n : n + k > (int)sizeof(b) - 3 ? (int)sizeof(b) - 3 : n + k;
	b[n++] = '\r';
	b[n++] = '\n';
	AcquireSRWLockExclusive(&log_lock);
	if (log_file != INVALID_HANDLE_VALUE) {
		WriteFile(log_file, b, n, &w, NULL);
	}
	if (log_con) {
		WriteFile(log_con, b, n, &w, NULL);
	}
	ReleaseSRWLockExclusive(&log_lock);
}

static void log_open(const WCHAR *dir, const WCHAR *name, bool console) {
	WCHAR p[MAX_PATH];
	swprintf(p, MAX_PATH, L"%ls\\%ls", dir, name);
	log_file = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (!console) return;
	log_con = GetStdHandle(STD_OUTPUT_HANDLE);
	if (log_con && log_con != INVALID_HANDLE_VALUE) return;
	log_con = AttachConsole(ATTACH_PARENT_PROCESS) ? CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL) : NULL;
	if (log_con == INVALID_HANDLE_VALUE) {
		log_con = NULL;
	}
}

static void paths(void) {
	WCHAR *s, ad[MAX_PATH];
	DWORD n;
	GetModuleFileNameW(NULL, g_app.dir, MAX_PATH);
	s = wcsrchr(g_app.dir, L'\\');
	if (s) {
		*s = 0;
	}
	n = GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
	swprintf(g_app.data, MAX_PATH, L"%ls\\Ghostlay", n && n < MAX_PATH - 10 ? ad : g_app.dir);
}

static void arm(HANDLE timer, double secs) {
	LARGE_INTEGER due;
	due.QuadPart = -(LONGLONG)(secs > 0 ? secs * 1e7 : 1);
	SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0);
}

static bool start_vr(void) {
	if (!vr_connect()) return false;
	if (!gfx_init(vr_adapter())) {
		glog("graphics init failed");
		vr_disconnect();
		return false;
	}
	audio_init();
	battery_init();
	watch_init();
	notif_init();
	panel_init();
	laser_init();
	udp_init();
	winnotif_init();
	media_init();
	applied_gen = 0;
	g_app.vr_t0 = now_s();
	g_app.vr = true;
	return true;
}

static void stop_vr(void) {
	laser_shutdown();
	media_shutdown();
	winnotif_shutdown();
	udp_shutdown();
	panel_shutdown();
	notif_shutdown();
	watch_shutdown();
	battery_shutdown();
	audio_shutdown();
	gfx_shutdown();
	vr_disconnect();
	g_app.vr = false;
}

static void apply_cfg(void) {
	if (applied_gen == g_cfg_gen) return;
	applied_gen = g_cfg_gen;
	notif_config(g_cfg.notif_on);
	udp_config(g_cfg.notif_on && g_cfg.udp_on, g_cfg.udp_port);
	winnotif_config(g_cfg.notif_on && g_cfg.discord);
	media_enable(g_cfg.media);
	vr_autolaunch(g_cfg.autolaunch);
}

static double frame(double now, double dt) {
	bool fast;
	if (!vr_alive()) {
		glog("SteamVR is gone without quitting, reconnecting");
		cfg_flush();
		stop_vr();
		return 5;
	}
	vr_events();
	if (g_app.quit) return 0;
	apply_cfg();
	vr_poses();
	fast = laser_update(dt);
	fast |= watch_update(dt);
	fast |= notif_update(dt);
	fast |= panel_update();
	battery_update(now);
	cfg_update(now);
	if (!g_app.worn) return 0.5;
	return fast ? 1.0 / 90 : 1.0 / 20;
}

static int run(void) {
	HANDLE timer, hs[2];
	double last, retry = 0;
	DWORD r;
	MSG m;
	double now, dt, period;
	timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (!timer) {
		timer = CreateWaitableTimerW(NULL, FALSE, NULL);
	}
	if (!timer) return 1;
	hs[0] = timer;
	hs[1] = g_app.wake;
	last = now_s();
	arm(timer, 0);
	while (!g_app.quit) {
		r = MsgWaitForMultipleObjectsEx(2, hs, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
			if (m.message == WM_QUIT) {
				g_app.quit = true;
			}
			DispatchMessageW(&m);
		}
		if (g_app.quit || r == WAIT_OBJECT_0 + 2) continue;
		now = now_s();
		dt = now - last;
		last = now;
		dt = dt > 0.25 ? 0.25 : dt;
		if (!g_app.vr && now >= retry && !start_vr()) {
			retry = now + 5;
		}
		if (!g_app.vr) {
			arm(timer, retry - now);
			continue;
		}
		period = frame(now, dt);
		arm(timer, now + period - now_s());
	}
	cfg_flush();
	if (g_app.vr) {
		stop_vr();
	}
	CloseHandle(timer);
	glog("exit");
	return 0;
}

#ifdef GL_TESTS
int st_ck(bool ok, const char *mod, const char *what) {
	if (!ok) {
		glog("%s selftest failed: %s", mod, what);
	}
	return !ok;
}

static int render(const WCHAR *dir) {
	WCHAR full[MAX_PATH], *s;
	char u[MAX_PATH * 3];
	int a, b, c, d;
	if (!GetFullPathNameW(dir, MAX_PATH, full, NULL)) return 1;
	for (s = wcschr(full, L'\\'); s; s = wcschr(s + 1, L'\\')) {
		*s = 0;
		CreateDirectoryW(full, NULL);
		*s = L'\\';
	}
	CreateDirectoryW(full, NULL);
	to_utf8(full, u, sizeof(u));
	if (!gfx_init(-1)) {
		glog("render: graphics init failed");
		return 1;
	}
	a = watch_render_samples(full);
	b = notif_render_samples(full);
	c = panel_render_samples(full);
	d = winnotif_render_samples(full);
	glog("render to %s: watch %d, notifications %d, panel %d, discord %d", u, a, b, c, d);
	gfx_shutdown();
	return !a || !b || !c || !d;
}

static int selftest(void) {
	static const struct { const char *name; int (*fn)(void); } t[] = { { "cfg", cfg_selftest }, { "lang", lang_selftest }, { "udp", udp_selftest }, { "toast", winnotif_selftest }, { "bat", battery_selftest }, { "watch", watch_selftest }, { "notif", notif_selftest }, { "media", media_selftest }, { "laser", laser_selftest } };
	int fail = 0, i, r;
	for (i = 0; i < (int)ARRAYSIZE(t); i++) {
		r = t[i].fn();
		glog("selftest %-5s %s (%d failures)", t[i].name, r ? "FAILED" : "ok", r);
		fail += r;
	}
	glog("selftest %s", fail ? "FAILED" : "passed");
	return fail != 0;
}
#else
static int selftest(void) {
	glog("--selftest and --render are only in ghostlay-test.exe (build.bat test)");
	return 1;
}

static int render(const WCHAR *dir) {
	(void)dir;
	return selftest();
}
#endif

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show) {
	int argc, rc = 0, toasts = 0, i;
	LPWSTR *argv;
	const WCHAR *render_dir = NULL;
	bool test = false;
	HANDLE mx = NULL;
	WCHAR logs[MAX_PATH];
	(void)inst;
	(void)prev;
	(void)cmd;
	(void)show;
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
	QueryPerformanceFrequency(&qpf);
	g_app.t0 = now_s();
	paths();
	argc = 0;
	argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	for (i = 1; argv && i < argc; i++) {
		if (!wcscmp(argv[i], L"--selftest")) {
			test = true;
		} else if (!wcscmp(argv[i], L"--render") && i + 1 < argc) {
			render_dir = argv[++i];
		} else if (!wcscmp(argv[i], L"--toasts")) {
			toasts = i + 1 < argc && argv[i + 1][0] >= L'0' && argv[i + 1][0] <= L'9' ? _wtoi(argv[++i]) : 60;
			toasts = toasts > 0 ? toasts : 60;
		}
	}
	if (!test && !render_dir && !toasts) {
		mx = CreateMutexW(NULL, TRUE, L"Ghostlay");
		if (GetLastError() == ERROR_ALREADY_EXISTS) {
			CloseHandle(mx);
			LocalFree(argv);
			return 0;
		}
	}
	swprintf(logs, MAX_PATH, L"%ls\\logs", g_app.data);
	CreateDirectoryW(g_app.data, NULL);
	CreateDirectoryW(logs, NULL);
	log_open(logs, mx ? L"ghostlay.log" : L"ghostlay-offline.log", !mx);
	g_app.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	glog("Ghostlay %s%s", VERSION_PREFIX, VERSION);
	if (mx) {
		cfg_load();
	} else {
		cfg_defaults(&g_cfg);
	}
	if (test) {
		rc = selftest();
	} else if (render_dir) {
		rc = render(render_dir);
	} else if (toasts) {
		rc = winnotif_toasts(toasts);
	} else {
		rc = run();
	}
	CoUninitialize();
	CloseHandle(g_app.wake);
	if (mx) {
		CloseHandle(mx);
	}
	LocalFree(argv);
	if (log_file != INVALID_HANDLE_VALUE) {
		CloseHandle(log_file);
	}
	return rc;
}
