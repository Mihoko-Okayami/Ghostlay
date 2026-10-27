#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#define WIDL_using_Windows_Storage_Streams
#define WIDL_using_Windows_Media_Control

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <objbase.h>
#include <initguid.h>
#include <roapi.h>
#include <winstring.h>
#include <shcore.h>
#include <windows.media.control.h>

#include "gfx.h"
#include "main.h"
#include "media.h"

#define PLAYING GlobalSystemMediaTransportControlsSessionPlaybackStatus_Playing

enum { R_SEL = 1, R_PLAY = 2, R_TL = 4, R_PROPS = 8, R_ALL = R_PLAY | R_TL | R_PROPS };

struct dlg;

struct dlg_vtbl {
	HRESULT (STDMETHODCALLTYPE *QueryInterface)(struct dlg *d, REFIID iid, void **out);
	ULONG (STDMETHODCALLTYPE *AddRef)(struct dlg *d);
	ULONG (STDMETHODCALLTYPE *Release)(struct dlg *d);
	HRESULT (STDMETHODCALLTYPE *Invoke)(struct dlg *d, IUnknown *sender, void *arg);
};

struct dlg {
	const struct dlg_vtbl *lpVtbl;
	const IID *iid;
	LONG bit;
};

static SRWLOCK lock = SRWLOCK_INIT;
static struct media_snap snap, w;
static uint8_t px[ICON_PX * ICON_PX * 4];
static HANDLE ev_quit, ev_wake, ev_done;
static struct worker wk;
static volatile LONG dirty, cmds, live;
static IUnknown *volatile await_id;
static IGlobalSystemMediaTransportControlsSessionManager *mgr;
static IGlobalSystemMediaTransportControlsSession *ses;
static IUnknown *ses_id;
static EventRegistrationToken tok[5];
static bool hooked[5], wdirty, cover_ok;
static int tries;
static double key_at, retry_at;

static HRESULT STDMETHODCALLTYPE dlg_qi(struct dlg *d, REFIID iid, void **out) {
	if (!out) return E_POINTER;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IAgileObject) || (d->iid && IsEqualIID(iid, d->iid))) {
		*out = d;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dlg_ref(struct dlg *d) {
	(void)d;
	return 1;
}

static HRESULT STDMETHODCALLTYPE dlg_invoke(struct dlg *d, IUnknown *sender, void *arg) {
	IUnknown *u;
	(void)arg;
	if (d->bit) {
		if (!(InterlockedOr(&dirty, d->bit) & d->bit)) {
			SetEvent(ev_wake);
		}
		return S_OK;
	}
	u = NULL;
	if (sender && SUCCEEDED(sender->lpVtbl->QueryInterface(sender, &IID_IUnknown, (void **)&u)) && u) {
		if (u == await_id) {
			SetEvent(ev_done);
		}
		u->lpVtbl->Release(u);
	}
	return S_OK;
}

static const struct dlg_vtbl dlg_vt = { dlg_qi, dlg_ref, dlg_ref, dlg_invoke };
static struct dlg d_done = { &dlg_vt, NULL, 0 };
static struct dlg d_cur = { &dlg_vt, &IID_ITypedEventHandler_GlobalSystemMediaTransportControlsSessionManager_CurrentSessionChangedEventArgs, R_SEL };
static struct dlg d_list = { &dlg_vt, &IID_ITypedEventHandler_GlobalSystemMediaTransportControlsSessionManager_SessionsChangedEventArgs, R_SEL };
static struct dlg d_tl = { &dlg_vt, &IID_ITypedEventHandler_GlobalSystemMediaTransportControlsSession_TimelinePropertiesChangedEventArgs, R_TL };
static struct dlg d_play = { &dlg_vt, &IID_ITypedEventHandler_GlobalSystemMediaTransportControlsSession_PlaybackInfoChangedEventArgs, R_PLAY | R_SEL };
static struct dlg d_props = { &dlg_vt, &IID_ITypedEventHandler_GlobalSystemMediaTransportControlsSession_MediaPropertiesChangedEventArgs, R_PROPS };

static bool ok(HRESULT hr, const char *what) {
	if (SUCCEEDED(hr)) return true;
	glog("media: %s failed 0x%08lx", what, (unsigned long)hr);
	return false;
}

static bool await(void *op, const IID *iid, const char *what) {
	IAsyncOperation_boolean *o = op;
	IUnknown *u = NULL;
	HANDLE hs[2];
	DWORD r;
	IAsyncInfo *ai;
	if (!ok(o->lpVtbl->QueryInterface(o, &IID_IUnknown, (void **)&u), what) || !u) return false;
	await_id = u;
	u->lpVtbl->Release(u);
	d_done.iid = iid;
	ResetEvent(ev_done);
	hs[0] = ev_quit;
	hs[1] = ev_done;
	r = ok(o->lpVtbl->put_Completed(o, (void *)&d_done), what) ? WaitForMultipleObjects(2, hs, FALSE, 5000) : WAIT_FAILED;
	await_id = NULL;
	if (r == WAIT_OBJECT_0 + 1) return true;
	if (r == WAIT_TIMEOUT) {
		glog("media: %s timed out", what);
	}
	ai = NULL;
	if (ok(o->lpVtbl->QueryInterface(o, &IID_IAsyncInfo, (void **)&ai), what) && ai) {
		ok(ai->lpVtbl->Cancel(ai), "cancel");
		ai->lpVtbl->Release(ai);
	}
	return false;
}

static void text(HSTRING h, WCHAR *out, int cap) {
	UINT32 n = 0, i;
	const WCHAR *p = WindowsGetStringRawBuffer(h, &n);
	if (!p) {
		p = L"";
		n = 0;
	}
	if (n >= (UINT32)cap) {
		n = (UINT32)cap - 1;
		if (n && IS_HIGH_SURROGATE(p[n - 1])) {
			n--;
		}
	}
	memcpy(out, p, n * sizeof(*out));
	out[n] = 0;
	for (i = 0; i < n; i++) {
		if (out[i] < 32 || (out[i] >= 0x7F && out[i] <= 0x9F) || out[i] == 0x2028 || out[i] == 0x2029) {
			out[i] = L' ';
		}
	}
	ok(WindowsDeleteString(h), "string");
}

double media_pos(const struct media_snap *m, double now) {
	double p = m->pos + (m->playing ? now - m->stamp : 0);
	return p < 0 ? 0 : m->dur > 0 && p > m->dur ? m->dur : p;
}

static void publish(void) {
	if (!wdirty) return;
	wdirty = false;
	AcquireSRWLockExclusive(&lock);
	w.version = snap.version + 1;
	snap = w;
	ReleaseSRWLockExclusive(&lock);
	app_wake();
}

static void set_playing(bool on) {
	double t;
	if (on == w.playing) return;
	t = now_s();
	w.pos = media_pos(&w, t);
	w.stamp = t;
	w.playing = on;
	wdirty = true;
}

static void set_cover(bool got, bool fresh) {
	if (got && (!w.has_cover || memcmp(w.cover, px, sizeof(px)))) {
		memcpy(w.cover, px, sizeof(px));
		w.has_cover = true;
		wdirty = true;
	} else if (!got && fresh && w.has_cover) {
		w.has_cover = false;
		wdirty = true;
	}
}

static int status(IGlobalSystemMediaTransportControlsSession *s) {
	IGlobalSystemMediaTransportControlsSessionPlaybackInfo *pi = NULL;
	GlobalSystemMediaTransportControlsSessionPlaybackStatus st = GlobalSystemMediaTransportControlsSessionPlaybackStatus_Closed;
	bool r;
	if (!ok(s->lpVtbl->GetPlaybackInfo(s, &pi), "playback info") || !pi) return -1;
	r = ok(pi->lpVtbl->get_PlaybackStatus(pi, &st), "playback status");
	pi->lpVtbl->Release(pi);
	return r ? (int)st : -1;
}

static IUnknown *ident(IGlobalSystemMediaTransportControlsSession *s) {
	IUnknown *u = NULL;
	if (!s) return NULL;
	if (!ok(s->lpVtbl->QueryInterface(s, &IID_IUnknown, (void **)&u), "session identity") || !u) return (IUnknown *)s;
	u->lpVtbl->Release(u);
	return u;
}

static IGlobalSystemMediaTransportControlsSession *pick(bool *lost) {
	IGlobalSystemMediaTransportControlsSession *cur = NULL, *s = NULL, *c;
	IVectorView_GlobalSystemMediaTransportControlsSession *v = NULL;
	UINT32 n = 0, i;
	HRESULT hr = mgr->lpVtbl->GetCurrentSession(mgr, &cur);
	*lost = hr == RPC_E_DISCONNECTED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) || hr == RPC_E_SERVER_DIED || hr == RPC_E_SERVER_DIED_DNE;
	if (!ok(hr, "current session")) {
		cur = NULL;
	}
	if (cur && status(cur) == PLAYING) return cur;
	if (ok(mgr->lpVtbl->GetSessions(mgr, &v), "sessions") && v) {
		if (!ok(v->lpVtbl->get_Size(v, &n), "session count")) {
			n = 0;
		}
		for (i = 0; i < n && !s; i++) {
			c = NULL;
			if (!ok(v->lpVtbl->GetAt(v, i, &c), "session") || !c) continue;
			if (status(c) == PLAYING) {
				s = c;
			} else {
				c->lpVtbl->Release(c);
			}
		}
		v->lpVtbl->Release(v);
	}
	if (!s) return cur;
	if (cur) {
		cur->lpVtbl->Release(cur);
	}
	return s;
}

static bool wire(IGlobalSystemMediaTransportControlsSession *s) {
	IUnknown *id = ident(s);
	WCHAR aumid[256], app[64];
	char u[256];
	HSTRING h = NULL;
	if (id == ses_id) {
		if (s) {
			s->lpVtbl->Release(s);
		}
		return false;
	}
	if (ses) {
		if (hooked[2]) {
			ok(ses->lpVtbl->remove_TimelinePropertiesChanged(ses, tok[2]), "remove TimelinePropertiesChanged");
		}
		if (hooked[3]) {
			ok(ses->lpVtbl->remove_PlaybackInfoChanged(ses, tok[3]), "remove PlaybackInfoChanged");
		}
		if (hooked[4]) {
			ok(ses->lpVtbl->remove_MediaPropertiesChanged(ses, tok[4]), "remove MediaPropertiesChanged");
		}
		ses->lpVtbl->Release(ses);
	}
	ses = s;
	ses_id = id;
	memset(&w, 0, sizeof(w));
	wdirty = true;
	cover_ok = false;
	tries = 0;
	retry_at = 0;
	hooked[2] = s && ok(s->lpVtbl->add_TimelinePropertiesChanged(s, (void *)&d_tl, &tok[2]), "TimelinePropertiesChanged");
	hooked[3] = s && ok(s->lpVtbl->add_PlaybackInfoChanged(s, (void *)&d_play, &tok[3]), "PlaybackInfoChanged");
	hooked[4] = s && ok(s->lpVtbl->add_MediaPropertiesChanged(s, (void *)&d_props, &tok[4]), "MediaPropertiesChanged");
	if (!s) {
		glog("media: no session");
		return true;
	}
	text(ok(s->lpVtbl->get_SourceAppUserModelId(s, &h), "app id") ? h : NULL, aumid, ARRAYSIZE(aumid));
	app_name(aumid, app, ARRAYSIZE(app));
	to_utf8(app, u, sizeof(u));
	glog("media: session %s", u);
	return true;
}

static bool load_cover(IRandomAccessStreamReference *th) {
	IAsyncOperation_IRandomAccessStreamWithContentType *op = NULL;
	IRandomAccessStreamWithContentType *ras = NULL;
	IStream *stm = NULL;
	bool r = false;
	if (!ok(th->lpVtbl->OpenReadAsync(th, &op), "cover open") || !op) return false;
	if (await(op, &IID_IAsyncOperationCompletedHandler_IRandomAccessStreamWithContentType, "cover open") && ok(op->lpVtbl->GetResults(op, &ras), "cover stream") && ras) {
		if (ok(CreateStreamOverRandomAccessStream((IUnknown *)ras, &IID_IStream, (void **)&stm), "cover IStream") && stm) {
			r = img_decode_stream(stm, px);
			stm->lpVtbl->Release(stm);
		}
		ras->lpVtbl->Release(ras);
	}
	op->lpVtbl->Release(op);
	return r;
}

static bool read_props(double t) {
	IAsyncOperation_GlobalSystemMediaTransportControlsSessionMediaProperties *op = NULL;
	IGlobalSystemMediaTransportControlsSessionMediaProperties *mp = NULL;
	IRandomAccessStreamReference *th = NULL;
	WCHAR title[ARRAYSIZE(w.title)], artist[ARRAYSIZE(w.artist)];
	HSTRING h = NULL;
	bool r, fresh;
	if (!ok(ses->lpVtbl->TryGetMediaPropertiesAsync(ses, &op), "media properties") || !op) return false;
	r = await(op, &IID_IAsyncOperationCompletedHandler_GlobalSystemMediaTransportControlsSessionMediaProperties, "media properties") && ok(op->lpVtbl->GetResults(op, &mp), "media properties result") && mp;
	op->lpVtbl->Release(op);
	if (!r) return false;
	text(ok(mp->lpVtbl->get_Title(mp, &h), "title") ? h : NULL, title, ARRAYSIZE(title));
	h = NULL;
	text(ok(mp->lpVtbl->get_Artist(mp, &h), "artist") ? h : NULL, artist, ARRAYSIZE(artist));
	fresh = wcscmp(title, w.title) || wcscmp(artist, w.artist);
	if (fresh) {
		wcscpy(w.title, title);
		wcscpy(w.artist, artist);
		w.active = title[0] != 0;
		key_at = t;
		cover_ok = false;
		tries = 0;
		wdirty = true;
	}
	if (!ok(mp->lpVtbl->get_Thumbnail(mp, &th), "thumbnail")) {
		th = NULL;
	}
	if (!th) {
		set_cover(false, true);
	} else if (fresh || t - key_at < 8 || (!cover_ok && tries < 3)) {
		tries++;
		cover_ok = load_cover(th);
		set_cover(cover_ok, fresh);
	}
	if (th) {
		th->lpVtbl->Release(th);
	}
	mp->lpVtbl->Release(mp);
	retry_at = !w.has_cover && t - key_at < 7 ? t + 1 : 0;
	return fresh;
}

static void read_tl(bool fresh) {
	IGlobalSystemMediaTransportControlsSessionTimelineProperties *tl = NULL;
	TimeSpan a = { 0 }, b = { 0 }, p = { 0 };
	DateTime lu = { 0 };
	FILETIME ft;
	bool r;
	double t, age, at, dur, pos, was, d0;
	if (!ok(ses->lpVtbl->GetTimelineProperties(ses, &tl), "timeline") || !tl) return;
	r = ok(tl->lpVtbl->get_StartTime(tl, &a), "start time") && ok(tl->lpVtbl->get_EndTime(tl, &b), "end time") && ok(tl->lpVtbl->get_Position(tl, &p), "position") && ok(tl->lpVtbl->get_LastUpdatedTime(tl, &lu), "update time");
	tl->lpVtbl->Release(tl);
	if (!r) return;
	GetSystemTimePreciseAsFileTime(&ft);
	t = now_s();
	age = (double)((INT64)((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - lu.UniversalTime) / 1e7;
	at = lu.UniversalTime > 0 && age > 0 && age < 43200 ? t - age : t;
	dur = ((double)b.Duration - (double)a.Duration) / 1e7;
	pos = ((double)p.Duration - (double)a.Duration) / 1e7;
	dur = dur > 0 && dur < 1e7 ? dur : 0;
	pos = pos < 1e7 ? pos : 0;
	if (!fresh && at < w.stamp - 2 && fabs(dur - w.dur) < 0.5) return;
	was = media_pos(&w, t);
	d0 = w.dur;
	w.pos = pos < 0 ? 0 : dur > 0 && pos > dur ? dur : pos;
	w.stamp = at;
	w.dur = dur;
	if (fabs(media_pos(&w, t) - was) > 0.75 || fabs(dur - d0) > 0.25) {
		wdirty = true;
	}
}

static bool read_pass(unsigned int p, double t) {
	bool lost = false, fresh;
	int st;
	if ((p & R_SEL) && wire(pick(&lost))) {
		p |= R_ALL;
	}
	if (lost || !ses) return lost;
	if (p & R_PLAY) {
		st = status(ses);
		if (st >= 0) {
			set_playing(st == PLAYING);
		}
	}
	fresh = (p & R_PROPS) && read_props(t);
	if (fresh || (p & R_TL)) {
		read_tl(fresh);
	}
	return false;
}

static void run_cmds(LONG c) {
	IAsyncOperation_boolean *op;
	boolean done;
	HRESULT hr;
	int i;
	for (i = MEDIA_PREV; i <= MEDIA_NEXT; i++) {
		if (!(c & 1 << i) || !ses) continue;
		op = NULL;
		done = 0;
		hr = i == MEDIA_PREV ? ses->lpVtbl->TrySkipPreviousAsync(ses, &op) : i == MEDIA_NEXT ? ses->lpVtbl->TrySkipNextAsync(ses, &op) : ses->lpVtbl->TryTogglePlayPauseAsync(ses, &op);
		if (!ok(hr, "command") || !op) continue;
		if (i == MEDIA_TOGGLE) {
			set_playing(!w.playing);
			publish();
		}
		if (await(op, &IID_IAsyncOperationCompletedHandler_boolean, "command") && ok(op->lpVtbl->GetResults(op, &done), "command result") && !done) {
			glog("media: command %d refused", i);
		}
		op->lpVtbl->Release(op);
	}
}

static bool start(void) {
	HSTRING_HEADER hh;
	HSTRING cls = NULL;
	IGlobalSystemMediaTransportControlsSessionManagerStatics *st = NULL;
	IAsyncOperation_GlobalSystemMediaTransportControlsSessionManager *op = NULL;
	bool r;
	if (!ok(WindowsCreateStringReference(RuntimeClass_Windows_Media_Control_GlobalSystemMediaTransportControlsSessionManager, ARRAYSIZE(RuntimeClass_Windows_Media_Control_GlobalSystemMediaTransportControlsSessionManager) - 1, &hh, &cls), "class name")) return false;
	if (!ok(RoGetActivationFactory(cls, &IID_IGlobalSystemMediaTransportControlsSessionManagerStatics, (void **)&st), "activation") || !st) return false;
	r = ok(st->lpVtbl->RequestAsync(st, &op), "RequestAsync") && op;
	st->lpVtbl->Release(st);
	if (!r) return false;
	r = await(op, &IID_IAsyncOperationCompletedHandler_GlobalSystemMediaTransportControlsSessionManager, "RequestAsync") && ok(op->lpVtbl->GetResults(op, &mgr), "session manager") && mgr;
	op->lpVtbl->Release(op);
	if (!r) return false;
	hooked[0] = ok(mgr->lpVtbl->add_CurrentSessionChanged(mgr, (void *)&d_cur, &tok[0]), "CurrentSessionChanged");
	hooked[1] = ok(mgr->lpVtbl->add_SessionsChanged(mgr, (void *)&d_list, &tok[1]), "SessionsChanged");
	return true;
}

static void stop(void) {
	wire(NULL);
	if (!mgr) return;
	if (hooked[0]) {
		ok(mgr->lpVtbl->remove_CurrentSessionChanged(mgr, tok[0]), "remove CurrentSessionChanged");
	}
	if (hooked[1]) {
		ok(mgr->lpVtbl->remove_SessionsChanged(mgr, tok[1]), "remove SessionsChanged");
	}
	hooked[0] = hooked[1] = false;
	mgr->lpVtbl->Release(mgr);
	mgr = NULL;
}

static bool loop(void) {
	HANDLE hs[2] = { ev_quit, ev_wake };
	unsigned int pend = 0;
	double due = 0, last = -1, safety = 0, t, next;
	bool armed = false;
	DWORD r;
	LONG c;
	wdirty = true;
	for (;;) {
		t = now_s();
		next = armed && due < safety ? due : safety;
		next = retry_at > 0 && retry_at < next ? retry_at : next;
		r = WaitForMultipleObjects(2, hs, FALSE, next > t ? (DWORD)((next - t) * 1000) + 1 : 0);
		if (r != WAIT_OBJECT_0 + 1 && r != WAIT_TIMEOUT) return false;
		t = now_s();
		c = InterlockedExchange(&cmds, 0);
		if (c) {
			run_cmds(c);
			pend |= R_ALL;
			due = now_s() + 0.15;
			armed = true;
		}
		if (!armed && dirty) {
			due = fmax(t, last + 0.5);
			armed = true;
		}
		if (retry_at > 0 && t >= retry_at) {
			pend |= R_PROPS;
			due = armed ? due : t;
			retry_at = 0;
			armed = true;
		}
		if (t >= safety) {
			pend |= R_SEL | R_ALL;
			due = t;
			armed = true;
		}
		if (!armed || now_s() < due) continue;
		if (read_pass(pend | (unsigned int)InterlockedExchange(&dirty, 0), t)) {
			glog("media: session manager lost, restarting");
			return true;
		}
		pend = 0;
		armed = false;
		last = t;
		safety = t + 30;
		publish();
	}
}

static DWORD WINAPI worker(void *arg) {
	DWORD delay;
	ev_quit = arg;
	if (!ok(RoInitialize(RO_INIT_MULTITHREADED), "RoInitialize")) {
		InterlockedDecrement(&live);
		return 1;
	}
	for (delay = 2000;; delay = delay < 30000 ? delay * 2 : 60000) {
		if (start()) {
			delay = 2000;
			if (!loop()) break;
		}
		stop();
		publish();
		if (WaitForSingleObject(ev_quit, delay) != WAIT_TIMEOUT) break;
	}
	stop();
	publish();
	RoUninitialize();
	InterlockedDecrement(&live);
	return 0;
}

void media_init(void) {
	if (!ev_wake) {
		ev_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
	}
	if (!ev_done) {
		ev_done = CreateEventW(NULL, FALSE, FALSE, NULL);
	}
	if (!ev_wake || !ev_done) {
		glog("media: CreateEvent failed %lu", GetLastError());
	}
}

static void halt(DWORD ms) {
	if (worker_stop(&wk, ms, "media") && wk.stop) {
		CloseHandle(wk.stop);
	}
	wk.stop = NULL;
}

static void reap(void) {
	if (wk.th && WaitForSingleObject(wk.th, 0) == WAIT_OBJECT_0) {
		halt(0);
	}
}

void media_enable(bool on) {
	HANDLE q;
	reap();
	if (on == (wk.th != NULL) || !ev_wake || !ev_done) return;
	if (!on) {
		halt(500);
		return;
	}
	if (live) {
		glog("media: the abandoned worker is still running, not restarting");
		return;
	}
	dirty = cmds = 0;
	q = CreateEventW(NULL, TRUE, FALSE, NULL);
	InterlockedIncrement(&live);
	if (worker_start(&wk, q, worker, q)) return;
	glog("media: cannot start the worker %lu", GetLastError());
	InterlockedDecrement(&live);
	if (q) {
		CloseHandle(q);
	}
}

void media_shutdown(void) {
	HANDLE *hs[2] = { &ev_wake, &ev_done };
	HANDLE h;
	int i;
	halt(2000);
	if (live) return;
	for (i = 0; i < 2; i++) {
		h = *hs[i];
		*hs[i] = NULL;
		if (h) {
			CloseHandle(h);
		}
	}
}

uint32_t media_version(void) {
	uint32_t v;
	AcquireSRWLockShared(&lock);
	v = snap.version;
	ReleaseSRWLockShared(&lock);
	return v;
}

const struct media_snap *media_lock(void) {
	AcquireSRWLockShared(&lock);
	return &snap;
}

void media_unlock(void) {
	ReleaseSRWLockShared(&lock);
}

void media_cmd(int cmd) {
	reap();
	if (cmd < MEDIA_PREV || cmd > MEDIA_NEXT || !wk.th) return;
	InterlockedOr(&cmds, 1 << cmd);
	SetEvent(ev_wake);
}

#ifdef GL_TESTS
static bool wait_snap(uint32_t v0) {
	HANDLE hs[2] = { wk.th, g_app.wake };
	double end = now_s() + 5, left;
	while (media_version() == v0) {
		left = end - now_s();
		if (left <= 0 || WaitForMultipleObjects(2, hs, FALSE, (DWORD)(left * 1000) + 1) != WAIT_OBJECT_0 + 1) return false;
	}
	return true;
}

static void dump(void) {
	const struct media_snap *m = media_lock();
	glog("media: v%u active %d playing %d title %u artist %u chars at %.1f of %.1f s, cover %s", m->version, m->active, m->playing, (unsigned int)wcslen(m->title), (unsigned int)wcslen(m->artist), media_pos(m, now_s()), m->dur, m->has_cover ? "yes" : "no");
	media_unlock();
}

int media_selftest(void) {
	static const WCHAR raw[] = L"a\nb\tc\u2028d\x85" L"e";
	static const WCHAR *const names[][2] = { { L"Spotify.exe", L"Spotify" }, { L"SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify", L"Spotify" }, { L"Chrome", L"Chrome" }, { L"{6D809377-6AF0-444B-8957-A3773F02200E}\\VideoLAN\\VLC\\vlc.exe", L"vlc" } };
	WCHAR b[64];
	int fail = 0, i, run;
	uint32_t v0;
	const struct media_snap *m;
	bool idle;
	HSTRING h;
	if (SUCCEEDED(WindowsCreateString(raw, ARRAYSIZE(raw) - 1, &h))) {
		text(h, b, ARRAYSIZE(b));
		if (wcscmp(b, L"a b c d e")) {
			glog("media: control characters kept in a title");
			fail++;
		}
	}
	for (i = 0; i < (int)ARRAYSIZE(names); i++) {
		app_name(names[i][0], b, ARRAYSIZE(b));
		if (wcscmp(b, names[i][1])) {
			glog("media: app name case %d failed", i);
			fail++;
		}
	}
	media_init();
	for (run = 0; run < 2; run++) {
		v0 = media_version();
		media_enable(true);
		if (!wk.th || !wait_snap(v0)) {
			glog("media: run %d produced no snapshot", run);
			fail++;
			break;
		}
		dump();
		media_enable(false);
		m = media_lock();
		idle = !m->active && !m->has_cover && !m->title[0];
		media_unlock();
		if (wk.th || !idle) {
			glog("media: disable left state behind");
			fail++;
		}
	}
	media_shutdown();
	return fail;
}
#endif
