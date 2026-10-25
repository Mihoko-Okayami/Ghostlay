#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <tlhelp32.h>

#include "gfx.h"
#include "main.h"
#include "vr.h"

struct vr_core {
	const struct {
		EVRInitError (*init)(struct vr_core *self, EVRApplicationType type, const char *info);
		void (*cleanup)(struct vr_core *self);
		EVRInitError (*valid)(struct vr_core *self, const char *version);
		void *(*get)(struct vr_core *self, const char *version, EVRInitError *error);
	} *v;
};

static struct VR_IVROverlay_FnTable *g_ovr;
struct vr_devs g_dev = { VR_NO_DEV, VR_NO_DEV, VR_NO_DEV, 0, { 0 }, 0 };
TrackedDevicePose_t g_pose[VR_MAX_DEV];
static struct VR_IVRSystem_FnTable *g_sys;
static struct VR_IVRApplications_FnTable *g_apps;
static struct VR_IVRInput_FnTable *g_inp;
static struct VR_IVRSettings_FnTable *g_set;
static HMODULE client;
static struct vr_core *core;
static HANDLE server;
static bool inited;
static int autolaunch = -1;
static float frame_s = 1.0f / 90, photons_s = 0.011f;
static EVRInitError last_err = EVRInitError_VRInitError_None;

static void ck(int e, const char *what) {
	static struct { const char *what; int e; } seen[32];
	static int n;
	int i;
	if (!e || n == 32) return;
	for (i = 0; i < n; i++) {
		if (seen[i].what == what && seen[i].e == e) return;
	}
	seen[n].what = what;
	seen[n++].e = e;
	glog("%s: %d", what, e);
}

struct v3 v3(float x, float y, float z) {
	return (struct v3){ x, y, z };
}

struct v3 v3_add(struct v3 a, struct v3 b) {
	return (struct v3){ a.x + b.x, a.y + b.y, a.z + b.z };
}

struct v3 v3_sub(struct v3 a, struct v3 b) {
	return (struct v3){ a.x - b.x, a.y - b.y, a.z - b.z };
}

struct v3 v3_mul(struct v3 a, float s) {
	return (struct v3){ a.x * s, a.y * s, a.z * s };
}

float v3_dot(struct v3 a, struct v3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static struct v3 v3_cross(struct v3 a, struct v3 b) {
	return (struct v3){ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

float v3_len(struct v3 a) {
	return sqrtf(v3_dot(a, a));
}

struct v3 v3_norm(struct v3 a) {
	float l = v3_len(a);
	return l > 1e-9f ? v3_mul(a, 1 / l) : a;
}

struct v3 v3_lerp(struct v3 a, struct v3 b, float t) {
	return v3_add(a, v3_mul(v3_sub(b, a), t));
}

HmdMatrix34_t m34_mul(const HmdMatrix34_t *a, const HmdMatrix34_t *b) {
	HmdMatrix34_t r;
	int i, j;
	for (i = 0; i < 3; i++) {
		for (j = 0; j < 4; j++) {
			r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j] + (j == 3 ? a->m[i][3] : 0);
		}
	}
	return r;
}

static HmdMatrix34_t m34_basis(struct v3 x, struct v3 y, struct v3 z, struct v3 p) {
	return (HmdMatrix34_t){ { { x.x, y.x, z.x, p.x }, { x.y, y.y, z.y, p.y }, { x.z, y.z, z.z, p.z } } };
}

HmdMatrix34_t m34_look(struct v3 pos, struct v3 eye, struct v3 up) {
	struct v3 z = v3_norm(v3_sub(eye, pos)), x = v3_norm(v3_cross(up, z));
	return m34_basis(x, v3_cross(z, x), z, pos);
}

HmdMatrix34_t m34_scale(const HmdMatrix34_t *a, float s) {
	HmdMatrix34_t r = *a;
	int i, j;
	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++) {
			r.m[i][j] *= s;
		}
	}
	return r;
}

HmdMatrix34_t m34_quat(struct quat q, struct v3 p) {
	float x = q.x, y = q.y, z = q.z, w = q.w;
	return (HmdMatrix34_t){ { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), p.x }, { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), p.y }, { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), p.z } } };
}

struct v3 m34_pos(const HmdMatrix34_t *m) {
	return (struct v3){ m->m[0][3], m->m[1][3], m->m[2][3] };
}

struct v3 m34_col(const HmdMatrix34_t *m, int i) {
	return (struct v3){ m->m[0][i], m->m[1][i], m->m[2][i] };
}

static struct v3 m34_vec(const HmdMatrix34_t *m, struct v3 v) {
	return (struct v3){ m->m[0][0] * v.x + m->m[0][1] * v.y + m->m[0][2] * v.z, m->m[1][0] * v.x + m->m[1][1] * v.y + m->m[1][2] * v.z, m->m[2][0] * v.x + m->m[2][1] * v.y + m->m[2][2] * v.z };
}

struct v3 m34_point(const HmdMatrix34_t *m, struct v3 p) {
	return v3_add(m34_vec(m, p), m34_pos(m));
}

struct quat q_from_m34(const HmdMatrix34_t *m) {
	struct v3 a = v3_norm(m34_col(m, 0)), b = v3_norm(m34_col(m, 1)), c = v3_norm(m34_col(m, 2));
	float t = a.x + b.y + c.z, s;
	if (t > 0) {
		s = sqrtf(t + 1) * 2;
		return (struct quat){ (b.z - c.y) / s, (c.x - a.z) / s, (a.y - b.x) / s, s / 4 };
	} else if (a.x > b.y && a.x > c.z) {
		s = sqrtf(1 + a.x - b.y - c.z) * 2;
		return (struct quat){ s / 4, (b.x + a.y) / s, (c.x + a.z) / s, (b.z - c.y) / s };
	} else if (b.y > c.z) {
		s = sqrtf(1 + b.y - a.x - c.z) * 2;
		return (struct quat){ (b.x + a.y) / s, s / 4, (c.y + b.z) / s, (c.x - a.z) / s };
	} else {
		s = sqrtf(1 + c.z - a.x - b.y) * 2;
		return (struct quat){ (c.x + a.z) / s, (c.y + b.z) / s, s / 4, (a.y - b.x) / s };
	}
}

struct quat q_slerp(struct quat a, struct quat b, float t) {
	float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w, wa = 1 - t, wb = t, th, s, l;
	struct quat r;
	if (d < 0) {
		b = (struct quat){ -b.x, -b.y, -b.z, -b.w };
		d = -d;
	}
	if (d < 0.9995f) {
		th = acosf(d);
		s = sinf(th);
		wa = sinf((1 - t) * th) / s;
		wb = sinf(t * th) / s;
	}
	r = (struct quat){ wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z, wa * a.w + wb * b.w };
	l = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
	return l > 1e-9f ? (struct quat){ r.x / l, r.y / l, r.z / l, r.w / l } : a;
}

static bool runtime(WCHAR *out, int cap) {
	WCHAR p[MAX_PATH];
	char buf[8192];
	const char *s, *e;
	HANDLE f;
	DWORD n = 0;
	int k;
	k = GetEnvironmentVariableW(L"LOCALAPPDATA", p, MAX_PATH);
	if (k <= 0 || k >= MAX_PATH - 32) return false;
	wcscpy(p + k, L"\\openvr\\openvrpaths.vrpath");
	f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE) return false;
	if (!ReadFile(f, buf, sizeof(buf) - 1, &n, NULL)) {
		n = 0;
	}
	CloseHandle(f);
	buf[n] = 0;
	e = buf + n;
	s = strstr(buf, "\"runtime\"");
	s = s ? strchr(s + 9, '[') : NULL;
	if (!s) return false;
	for (s++; s < e && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n'); s++) continue;
	return json_str(s, e, out, NULL, cap) && out[0];
}

static EVRInitError core_init(EVRApplicationType type) {
	WCHAR p[MAX_PATH];
	void *(*factory)(const char *name, int *code);
	int code = 0;
	EVRInitError e;
	if (!runtime(p, MAX_PATH - 24)) return EVRInitError_VRInitError_Init_PathRegistryNotFound;
	wcscat(p, L"\\bin\\vrclient_x64.dll");
	client = LoadLibraryExW(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!client) return EVRInitError_VRInitError_Init_VRClientDLLNotFound;
	factory = (__typeof__(factory))(void *)GetProcAddress(client, "VRClientCoreFactory");
	core = factory ? factory("IVRClientCore_003", &code) : NULL;
	e = !factory ? EVRInitError_VRInitError_Init_FactoryNotFound : !core ? EVRInitError_VRInitError_Init_InterfaceNotFound : core->v->init(core, type, NULL);
	if (e != EVRInitError_VRInitError_None) {
		FreeLibrary(client);
		client = NULL;
		core = NULL;
	}
	return e;
}

static void core_shutdown(void) {
	core->v->cleanup(core);
	FreeLibrary(client);
	client = NULL;
	core = NULL;
}

static void *table(const char *ver) {
	char name[64];
	EVRInitError e = EVRInitError_VRInitError_None;
	void *t;
	snprintf(name, sizeof(name), "FnTable:%s", ver);
	t = core->v->valid(core, ver) == EVRInitError_VRInitError_None ? core->v->get(core, name, &e) : NULL;
	return e == EVRInitError_VRInitError_None ? t : NULL;
}

static void manifest(const WCHAR *wp) {
	WCHAR exe[MAX_PATH];
	char raw[MAX_PATH * 3], name[MAX_PATH * 6], body[4096], *d;
	const char *s;
	int n;
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	to_utf8(exe, raw, sizeof(raw));
	d = name;
	for (s = raw; *s; s++) {
		if (*s == '\\' || *s == '"') {
			*d++ = '\\';
		}
		*d++ = *s;
	}
	*d = 0;
	n = snprintf(body, sizeof(body), "{\n\t\"source\": \"builtin\",\n\t\"applications\": [{\n\t\t\"app_key\": \"%s\",\n\t\t\"launch_type\": \"binary\",\n\t\t\"binary_path_windows\": \"%s\",\n\t\t\"is_dashboard_overlay\": true,\n\t\t\"strings\": { \"en_us\": { \"name\": \"Ghostlay\", \"description\": \"Wrist watch and notifications\" } }\n\t}]\n}\n", GL_APP_KEY, name);
	write_file(wp, body, n);
}

static HANDLE find_server(void) {
	PROCESSENTRY32W e;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0), h = NULL;
	BOOL more;
	if (snap == INVALID_HANDLE_VALUE) return NULL;
	e.dwSize = sizeof(e);
	for (more = Process32FirstW(snap, &e); more && !h; more = Process32NextW(snap, &e)) {
		if (!_wcsicmp(e.szExeFile, L"vrserver.exe")) {
			h = OpenProcess(SYNCHRONIZE, FALSE, e.th32ProcessID);
		}
	}
	CloseHandle(snap);
	return h;
}

static bool worn_level(void) {
	EDeviceActivityLevel l = g_sys->GetTrackedDeviceActivityLevel(0);
	return l == EDeviceActivityLevel_k_EDeviceActivityLevel_UserInteraction || l == EDeviceActivityLevel_k_EDeviceActivityLevel_UserInteraction_Timeout || l == EDeviceActivityLevel_k_EDeviceActivityLevel_Unknown;
}

static void vr_devices(void) {
	TrackedDeviceIndex_t i;
	if (!g_sys) return;
	g_dev.hmd = g_sys->IsTrackedDeviceConnected(0) ? 0 : VR_NO_DEV;
	g_dev.left = g_sys->GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole_TrackedControllerRole_LeftHand);
	g_dev.right = g_sys->GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole_TrackedControllerRole_RightHand);
	g_dev.ntrk = 0;
	for (i = 1; i < VR_MAX_DEV && g_dev.ntrk < MAX_TRACKERS; i++) {
		if (g_sys->GetTrackedDeviceClass(i) == ETrackedDeviceClass_TrackedDeviceClass_GenericTracker && g_sys->IsTrackedDeviceConnected(i)) {
			g_dev.trk[g_dev.ntrk++] = i;
		}
	}
	g_dev.gen++;
}

bool vr_connect(void) {
	EVRInitError e = core_init(EVRApplicationType_VRApplication_Background);
	float hz;
	if (e == EVRInitError_VRInitError_None) {
		core_shutdown();
		e = core_init(EVRApplicationType_VRApplication_Overlay);
	}
	if (e != EVRInitError_VRInitError_None) {
		if (e != last_err) {
			glog("VR init error %d", (int)e);
		}
		last_err = e;
		return false;
	}
	inited = true;
	last_err = EVRInitError_VRInitError_None;
	g_sys = table(IVRSystem_Version);
	g_ovr = table(IVROverlay_Version);
	g_apps = table(IVRApplications_Version);
	g_inp = table(IVRInput_Version);
	g_set = table(IVRSettings_Version);
	if (!g_inp) {
		glog("VR interface unavailable: %s", IVRInput_Version);
	}
	if (!g_sys || !g_ovr || !g_apps) {
		glog("VR interfaces unavailable: %s %s %s", IVRSystem_Version, IVROverlay_Version, IVRApplications_Version);
		vr_disconnect();
		return false;
	}
	hz = vr_prop_float(0, ETrackedDeviceProperty_Prop_DisplayFrequency_Float, 90);
	frame_s = 1.0f / (hz > 20 ? hz : 90);
	photons_s = vr_prop_float(0, ETrackedDeviceProperty_Prop_SecondsFromVsyncToPhotons_Float, 0.011f);
	g_app.worn = worn_level();
	memset(g_pose, 0, sizeof(g_pose));
	vr_devices();
	autolaunch = -1;
	server = find_server();
	if (!server) {
		glog("vrserver.exe not found: a SteamVR crash will not be detected");
	}
	glog("SteamVR %s connected: %.0f Hz, adapter %d, headset %s", g_sys->GetRuntimeVersion(), hz, vr_adapter(), g_app.worn ? "worn" : "not worn");
	return true;
}

void vr_disconnect(void) {
	if (inited) {
		core_shutdown();
	}
	inited = false;
	if (server) {
		CloseHandle(server);
	}
	server = NULL;
	g_sys = NULL;
	g_ovr = NULL;
	g_apps = NULL;
	g_inp = NULL;
	g_set = NULL;
	g_app.worn = false;
}

bool vr_alive(void) {
	return !server || WaitForSingleObject(server, 0) == WAIT_TIMEOUT;
}

void vr_events(void) {
	struct VREvent_t e;
	bool devs = false, level = false;
	if (!g_sys) return;
	while (g_sys->PollNextEvent(&e, sizeof(e))) {
		switch (e.eventType) {
		case EVREventType_VREvent_Quit:
			glog("SteamVR is quitting");
			g_sys->AcknowledgeQuit_Exiting();
			g_app.quit = true;
			break;
		case EVREventType_VREvent_TrackedDeviceActivated:
		case EVREventType_VREvent_TrackedDeviceDeactivated:
		case EVREventType_VREvent_TrackedDeviceRoleChanged:
			devs = true;
			break;
		case EVREventType_VREvent_TrackedDeviceUserInteractionStarted:
		case EVREventType_VREvent_TrackedDeviceUserInteractionEnded:
		case EVREventType_VREvent_EnterStandbyMode:
		case EVREventType_VREvent_LeaveStandbyMode:
			level = true;
			break;
		}
	}
	if (devs) {
		vr_devices();
	}
	if (level) {
		g_app.worn = worn_level();
	}
}

void vr_poses(void) {
	float since = 0, p = photons_s;
	uint64_t frame = 0;
	if (!g_sys) return;
	if (g_sys->GetTimeSinceLastVsync(&since, &frame) && frame_s > since) {
		p += frame_s - since;
	}
	g_sys->GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin_TrackingUniverseStanding, p, g_pose, VR_MAX_DEV);
}

int vr_adapter(void) {
	int32_t i = -1;
	if (g_sys) {
		g_sys->GetDXGIOutputInfo(&i);
	}
	return i;
}

const HmdMatrix34_t *vr_pose(TrackedDeviceIndex_t d) {
	if (d >= VR_MAX_DEV || !g_pose[d].bPoseIsValid || !g_pose[d].bDeviceIsConnected) return NULL;
	return &g_pose[d].mDeviceToAbsoluteTracking;
}

float vr_prop_float(TrackedDeviceIndex_t d, ETrackedDeviceProperty p, float def) {
	ETrackedPropertyError e = ETrackedPropertyError_TrackedProp_Success;
	float v;
	if (!g_sys || d >= VR_MAX_DEV) return def;
	v = g_sys->GetFloatTrackedDeviceProperty(d, p, &e);
	return e == ETrackedPropertyError_TrackedProp_Success ? v : def;
}

bool vr_prop_bool(TrackedDeviceIndex_t d, ETrackedDeviceProperty p) {
	ETrackedPropertyError e = ETrackedPropertyError_TrackedProp_Success;
	bool v;
	if (!g_sys || d >= VR_MAX_DEV) return false;
	v = g_sys->GetBoolTrackedDeviceProperty(d, p, &e);
	return e == ETrackedPropertyError_TrackedProp_Success && v;
}

int vr_prop_str(TrackedDeviceIndex_t d, ETrackedDeviceProperty p, char *out, int cap) {
	ETrackedPropertyError e = ETrackedPropertyError_TrackedProp_Success;
	uint32_t n;
	if (cap <= 0) return 0;
	out[0] = 0;
	if (!g_sys || d >= VR_MAX_DEV) return 0;
	n = g_sys->GetStringTrackedDeviceProperty(d, p, out, (uint32_t)cap, &e);
	if (e != ETrackedPropertyError_TrackedProp_Success || !n || n > (uint32_t)cap) {
		out[0] = 0;
		return 0;
	}
	out[n - 1] = 0;
	return (int)n - 1;
}

bool vr_setting_str(const char *sec, const char *key, char *out, int cap) {
	EVRSettingsError e = EVRSettingsError_VRSettingsError_None;
	if (cap <= 0) return false;
	out[0] = 0;
	if (!g_set) return false;
	g_set->GetString((char *)sec, (char *)key, out, (uint32_t)cap, &e);
	out[cap - 1] = 0;
	if (e == EVRSettingsError_VRSettingsError_None) return out[0] != 0;
	out[0] = 0;
	return false;
}

void vr_haptic(TrackedDeviceIndex_t d, float seconds) {
	float us;
	if (!g_sys || d >= VR_MAX_DEV) return;
	us = seconds * 1e6f;
	g_sys->TriggerHapticPulse(d, 0, (unsigned short)clampf(us, 0, 3999));
}

void vr_autolaunch(bool on) {
	WCHAR wp[MAX_PATH];
	char path[MAX_PATH * 3];
	if (!g_apps || autolaunch == (int)on) return;
	autolaunch = on;
	swprintf(wp, MAX_PATH, L"%ls\\ghostlay.vrmanifest", g_app.data);
	to_utf8(wp, path, sizeof(path));
	if (on) {
		manifest(wp);
		ck(g_apps->AddApplicationManifest(path, false), "AddApplicationManifest");
		if (!g_apps->GetApplicationAutoLaunch(GL_APP_KEY)) {
			ck(g_apps->SetApplicationAutoLaunch(GL_APP_KEY, true), "SetApplicationAutoLaunch");
		}
		glog("registered in SteamVR with autolaunch");
	} else if (g_apps->IsApplicationInstalled(GL_APP_KEY)) {
		ck(g_apps->SetApplicationAutoLaunch(GL_APP_KEY, false), "SetApplicationAutoLaunch");
		ck(g_apps->RemoveApplicationManifest(path), "RemoveApplicationManifest");
		DeleteFileW(wp);
		glog("unregistered from SteamVR");
	}
}

bool vr_input_manifest(const char *path) {
	EVRInputError e;
	if (!g_inp) return false;
	e = g_inp->SetActionManifestPath((char *)path);
	ck(e, "SetActionManifestPath");
	return e == EVRInputError_VRInputError_None;
}

uint64_t vr_input_handle(int kind, const char *name) {
	uint64_t h = 0;
	EVRInputError e;
	if (!g_inp) return 0;
	if (kind == VI_SET) {
		e = g_inp->GetActionSetHandle((char *)name, &h);
	} else if (kind == VI_ACTION) {
		e = g_inp->GetActionHandle((char *)name, &h);
	} else {
		e = g_inp->GetInputSourceHandle((char *)name, &h);
	}
	ck(e, name);
	return e == EVRInputError_VRInputError_None ? h : 0;
}

void vr_input_sync(uint64_t set, uint64_t over, uint64_t dev) {
	struct VRActiveActionSet_t a[2] = { { set, 0, 0, 0, 0 }, { over, dev, 0, 0, k_nActionSetOverlayGlobalPriorityMax } };
	if (!g_inp || !set) return;
	ck(g_inp->UpdateActionState(a, sizeof(a[0]), over ? 2 : 1), "UpdateActionState");
}

bool vr_analog(uint64_t action, uint64_t src, float *x, float *y) {
	struct InputAnalogActionData_t d;
	bool ok;
	ok = g_inp && action && g_inp->GetAnalogActionData(action, &d, sizeof(d), src) == EVRInputError_VRInputError_None && d.bActive;
	*x = ok ? d.x : 0;
	*y = ok ? d.y : 0;
	return ok;
}

bool ovl_create(struct ovl *o, const char *key, const char *name) {
	EVROverlayError e;
	memset(o, 0, sizeof(*o));
	o->alpha = 1;
	o->width = 1;
	o->rgb = 0xFFFFFF;
	if (!g_ovr) return false;
	e = g_ovr->CreateOverlay((char *)key, (char *)name, &o->h);
	if (e != EVROverlayError_VROverlayError_None) {
		glog("CreateOverlay %s: %d", key, (int)e);
		o->h = 0;
		return false;
	}
	ck(g_ovr->SetOverlayFlag(o->h, VROverlayFlags_IsPremultiplied, true), "SetOverlayFlag");
	ck(g_ovr->SetOverlayFlag(o->h, VROverlayFlags_NoBackside, true), "SetOverlayFlag");
	return true;
}

void ovl_destroy(struct ovl *o) {
	if (o->h && g_ovr) {
		g_ovr->DestroyOverlay(o->h);
	}
	memset(o, 0, sizeof(*o));
}

void ovl_show(struct ovl *o, bool on) {
	if (!o->h || o->shown == on) return;
	o->shown = on;
	if (on) {
		ck(g_ovr->ShowOverlay(o->h), "ShowOverlay");
	} else {
		ck(g_ovr->HideOverlay(o->h), "HideOverlay");
	}
}

void ovl_alpha(struct ovl *o, float a) {
	if (!o->h || o->alpha == a) return;
	o->alpha = a;
	ck(g_ovr->SetOverlayAlpha(o->h, a), "SetOverlayAlpha");
}

void ovl_width(struct ovl *o, float m) {
	if (!o->h || o->width == m) return;
	o->width = m;
	ck(g_ovr->SetOverlayWidthInMeters(o->h, m), "SetOverlayWidthInMeters");
}

void ovl_curvature(struct ovl *o, float c) {
	if (!o->h) return;
	o->curve = c;
	ck(g_ovr->SetOverlayCurvature(o->h, c), "SetOverlayCurvature");
}

void ovl_sort(struct ovl *o, uint32_t order) {
	if (!o->h) return;
	ck(g_ovr->SetOverlaySortOrder(o->h, order), "SetOverlaySortOrder");
}

void ovl_abs(struct ovl *o, const HmdMatrix34_t *m) {
	if (!o->h || (o->xf_kind == XF_ABS && !memcmp(&o->xf, m, sizeof(*m)))) return;
	o->xf_kind = XF_ABS;
	o->xf = *m;
	ck(g_ovr->SetOverlayTransformAbsolute(o->h, ETrackingUniverseOrigin_TrackingUniverseStanding, &o->xf), "SetOverlayTransformAbsolute");
}

void ovl_rel(struct ovl *o, TrackedDeviceIndex_t dev, const HmdMatrix34_t *m) {
	if (!o->h || (o->xf_kind == XF_REL && o->xf_dev == dev && !memcmp(&o->xf, m, sizeof(*m)))) return;
	o->xf_kind = XF_REL;
	o->xf_dev = dev;
	o->xf = *m;
	ck(g_ovr->SetOverlayTransformTrackedDeviceRelative(o->h, dev, &o->xf), "SetOverlayTransformTrackedDeviceRelative");
}

void ovl_color(struct ovl *o, uint32_t rgb) {
	if (!o->h || o->rgb == rgb) return;
	o->rgb = rgb;
	ck(g_ovr->SetOverlayColor(o->h, (rgb >> 16 & 255) / 255.0f, (rgb >> 8 & 255) / 255.0f, (rgb & 255) / 255.0f), "SetOverlayColor");
}

void ovl_mouse_scale(struct ovl *o, float w, float h) {
	HmdVector2_t s = { { w, h } };
	if (!o->h) return;
	ck(g_ovr->SetOverlayMouseScale(o->h, &s), "SetOverlayMouseScale");
}

void ovl_texture(struct ovl *o, struct gx_tex *t) {
	void *n = t ? gx_tex_native(t) : NULL;
	Texture_t tx;
	if (!o->h || !n) return;
	tx = (Texture_t){ n, ETextureType_TextureType_DirectX, EColorSpace_ColorSpace_Auto };
	ck(g_ovr->SetOverlayTexture(o->h, &tx), "SetOverlayTexture");
}

void ovl_raw(struct ovl *o, const uint8_t *rgba, int w, int h) {
	if (!o->h) return;
	ck(g_ovr->SetOverlayRaw(o->h, (void *)rgba, (uint32_t)w, (uint32_t)h, 4), "SetOverlayRaw");
}

void ovl_clear(struct ovl *o) {
	if (!o->h) return;
	g_ovr->ClearOverlayTexture(o->h);
}

bool ovl_world(const struct ovl *o, HmdMatrix34_t *m) {
	const HmdMatrix34_t *p = o->xf_kind == XF_REL ? vr_pose(o->xf_dev) : NULL;
	if (o->xf_kind == XF_ABS) {
		*m = o->xf;
		return true;
	}
	if (!p) return false;
	*m = m34_mul(p, &o->xf);
	return true;
}

bool ovl_hit(const struct ovl *o, struct v3 src, struct v3 dir, float *dist, float *u, float *v) {
	struct VROverlayIntersectionParams_t q = { { { src.x, src.y, src.z } }, { { dir.x, dir.y, dir.z } }, ETrackingUniverseOrigin_TrackingUniverseStanding };
	struct VROverlayIntersectionResults_t r;
	if (!o->h || !g_ovr->ComputeOverlayIntersection(o->h, &q, &r)) return false;
	*dist = r.fDistance;
	*u = r.vUVs.v[0];
	*v = r.vUVs.v[1];
	return true;
}
