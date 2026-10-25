#ifndef VR_H
#define VR_H

#include <stdbool.h>
#include <stdint.h>
#pragma push_macro("__WIN32")
#pragma push_macro("EXTERN_C")
#undef __WIN32
#include <openvr_capi.h>
#pragma pop_macro("EXTERN_C")
#pragma pop_macro("__WIN32")

#include "gfx.h"

#define VR_MAX_DEV 64
#define VR_NO_DEV 0xFFFFFFFFu
#define MAX_TRACKERS 32

enum { XF_NONE, XF_ABS, XF_REL };

enum { VI_SET, VI_ACTION, VI_SOURCE };

struct v3 { float x, y, z; };

struct quat { float x, y, z, w; };

struct ovl {
	VROverlayHandle_t h;
	bool shown;
	float alpha, width, curve;
	uint32_t rgb;
	int xf_kind;
	TrackedDeviceIndex_t xf_dev;
	HmdMatrix34_t xf;
};

struct vr_devs {
	TrackedDeviceIndex_t hmd, left, right;
	int ntrk;
	TrackedDeviceIndex_t trk[MAX_TRACKERS];
	uint32_t gen;
};

extern struct vr_devs g_dev;
extern TrackedDevicePose_t g_pose[VR_MAX_DEV];

struct v3 v3(float x, float y, float z);
struct v3 v3_add(struct v3 a, struct v3 b);
struct v3 v3_sub(struct v3 a, struct v3 b);
struct v3 v3_mul(struct v3 a, float s);
float v3_dot(struct v3 a, struct v3 b);
float v3_len(struct v3 a);
struct v3 v3_norm(struct v3 a);
struct v3 v3_lerp(struct v3 a, struct v3 b, float t);
HmdMatrix34_t m34_mul(const HmdMatrix34_t *a, const HmdMatrix34_t *b);
HmdMatrix34_t m34_look(struct v3 pos, struct v3 eye, struct v3 up);
HmdMatrix34_t m34_scale(const HmdMatrix34_t *a, float s);
HmdMatrix34_t m34_quat(struct quat q, struct v3 pos);
struct v3 m34_pos(const HmdMatrix34_t *m);
struct v3 m34_col(const HmdMatrix34_t *m, int i);
struct v3 m34_point(const HmdMatrix34_t *m, struct v3 p);
struct quat q_from_m34(const HmdMatrix34_t *m);
struct quat q_slerp(struct quat a, struct quat b, float t);
bool vr_connect(void);
void vr_disconnect(void);
bool vr_alive(void);
void vr_events(void);
void vr_poses(void);
int vr_adapter(void);
const HmdMatrix34_t *vr_pose(TrackedDeviceIndex_t d);
float vr_prop_float(TrackedDeviceIndex_t d, ETrackedDeviceProperty p, float def);
bool vr_prop_bool(TrackedDeviceIndex_t d, ETrackedDeviceProperty p);
int vr_prop_str(TrackedDeviceIndex_t d, ETrackedDeviceProperty p, char *out, int cap);
bool vr_setting_str(const char *sec, const char *key, char *out, int cap);
void vr_haptic(TrackedDeviceIndex_t d, float seconds);
void vr_autolaunch(bool on);
bool vr_input_manifest(const char *path);
uint64_t vr_input_handle(int kind, const char *name);
void vr_input_sync(uint64_t set, uint64_t over, uint64_t dev);
bool vr_analog(uint64_t action, uint64_t src, float *x, float *y);
bool ovl_create(struct ovl *o, const char *key, const char *name);
void ovl_destroy(struct ovl *o);
void ovl_show(struct ovl *o, bool on);
void ovl_alpha(struct ovl *o, float a);
void ovl_width(struct ovl *o, float m);
void ovl_curvature(struct ovl *o, float c);
void ovl_sort(struct ovl *o, uint32_t order);
void ovl_abs(struct ovl *o, const HmdMatrix34_t *m);
void ovl_rel(struct ovl *o, TrackedDeviceIndex_t dev, const HmdMatrix34_t *m);
void ovl_color(struct ovl *o, uint32_t rgb);
void ovl_mouse_scale(struct ovl *o, float w, float h);
void ovl_texture(struct ovl *o, struct gx_tex *t);
void ovl_raw(struct ovl *o, const uint8_t *rgba, int w, int h);
void ovl_clear(struct ovl *o);
bool ovl_world(const struct ovl *o, HmdMatrix34_t *m);
bool ovl_hit(const struct ovl *o, struct v3 src, struct v3 dir, float *dist, float *u, float *v);

#endif
