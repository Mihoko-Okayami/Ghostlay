#ifndef GFX_H
#define GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>
#include <objbase.h>

#define C_TEXT 0xF4F5F7u
#define C_SOFT 0xD3D7DEu
#define C_DIM 0xA9AFB9u
#define C_ICON 0xC9CED6u
#define C_DEVICE 0xD5DAE0u
#define C_RING 0x2C3038u
#define C_RING_CRIT 0x45282Du
#define C_INK 0x17191Eu
#define C_CARD 0x14161Bu
#define C_WARN 0xFF8A4Cu
#define C_CRIT 0xFF5C5Cu
#define C_CRIT_T 0xFF8A8Au
#define C_WARN_T 0xFFAD80u
#define GT_LINES(n) ((n) << 8)

struct gx_tex;
struct gx_bmp;

struct gx_color { float r, g, b, a; };

struct gx_stop { float pos; struct gx_color c; };

enum gx_family { GF_DISPLAY, GF_TEXT };

enum { GT_CENTER = 1, GT_RIGHT = 2, GT_ELLIPSIS = 4, GT_WRAP = 16, GT_TNUM = 32, GT_PNUM = 64, GT_TIGHT = 128 };

enum gx_icon {
	IC_GEAR, IC_STOPWATCH, IC_PREV, IC_PLAY, IC_PAUSE, IC_NEXT, IC_UNLINK, IC_BATTERY, IC_BELL, IC_BELL_OFF, IC_CHECK, IC_WARNING, IC_GHOST,
	IC_CHEVRON_DOWN, IC_CHEVRON_UP, IC_CHEVRON_LEFT, IC_CHEVRON_RIGHT, IC_PENCIL, IC_CLOSE, IC_PLUS, IC_MINUS,
	IC_SLIDERS, IC_WATCH, IC_POWER, IC_GLOBE, IC_INFO, IC_ALERT, IC_CHECK_CIRCLE, IC_RESET, IC_PALETTE, IC_HEADSET, IC_CONTROLLER, IC_CTRL_L, IC_CTRL_R, IC_TRACKER, IC_MOVE, IC_BACKSPACE, IC_BOLT, IC_HISTORY, IC_MOON, IC_MOON_FILL,
	IC_COUNT
};

extern uint32_t g_gfx_gen;

bool gfx_init(int adapter);
void gfx_shutdown(void);
struct gx_tex *gx_tex_new(int w, int h);
void gx_tex_free(struct gx_tex *t);
void *gx_tex_native(struct gx_tex *t);
bool gx_begin(struct gx_tex *t);
bool gx_end(void);
void gx_opacity(float a);
void gx_transform(float scale, float dx, float dy);
void gx_clip_push(float x, float y, float w, float h);
void gx_clip_pop(void);
void gx_layer_push(float a);
void gx_mask_push(float x, float y, float w, float h, const struct gx_stop *stops, int n);
void gx_layer_pop(void);
void gx_fill_rect(float x, float y, float w, float h, struct gx_color c);
void gx_fill_rrect(float x, float y, float w, float h, float r, struct gx_color c);
void gx_stroke_rrect(float x, float y, float w, float h, float r, float sw, struct gx_color c);
void gx_grad_rrect(float x, float y, float w, float h, float r, float x0, float y0, float x1, float y1, const struct gx_stop *stops, int n);
void gx_shadow_rrect(float x, float y, float w, float h, float r, float blur, float dy, struct gx_color c);
void gx_fill_circle(float cx, float cy, float r, struct gx_color c);
void gx_stroke_circle(float cx, float cy, float r, float sw, struct gx_color c);
void gx_fill_ellipse(float cx, float cy, float rx, float ry, struct gx_color c);
void gx_arc(float cx, float cy, float r, float sw, float start_deg, float sweep_deg, struct gx_color c);
void gx_line(float x0, float y0, float x1, float y1, float sw, struct gx_color c);
int gx_font(enum gx_family f, float size, int weight);
float gx_text(int font, const WCHAR *s, float x, float y, float w, float h, struct gx_color c, int flags);
float gx_measure(int font, const WCHAR *s, float w, int flags, int *lines);
void gx_ink(int font, const WCHAR *s, int flags, float *l, float *r);
float gx_baseline(int font);
void gx_icon(enum gx_icon i, float x, float y, float size, float sw, struct gx_color c);
struct gx_bmp *gx_bmp_new(int w, int h);
void gx_bmp_free(struct gx_bmp *b);
bool gx_bmp_set(struct gx_bmp *b, const void *pbgra, int stride);
void gx_bmp_draw(struct gx_bmp *b, float x, float y, float w, float h, float radius);
bool img_decode_mem(const void *data, size_t len, uint8_t *out);
bool img_local(const WCHAR *path);
bool img_decode_file(const WCHAR *path, uint8_t *out);
bool img_decode_stream(IStream *s, uint8_t *out);
struct gx_color col_rgb(uint32_t rgb);
struct gx_color col_rgba(uint32_t rgb, float a);
struct gx_color col_mix(struct gx_color a, struct gx_color b, float t);
struct gx_color col_accent(void);
struct gx_color col_accent_soft(void);
struct gx_color col_accent_mid(void);
struct gx_color col_on(uint32_t bg);
float col_luma(uint32_t rgb);
bool col_parse(const WCHAR *s, uint32_t *rgb);
void col_format(WCHAR out[8]);
uint32_t col_hsv(float h, float s, float v);
void col_to_hsv(uint32_t rgb, float *h, float *s, float *v);

#ifdef GL_TESTS
bool gx_save_png(struct gx_tex *t, const WCHAR *path);
#endif

#endif
