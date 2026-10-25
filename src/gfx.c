#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <objbase.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <dwrite_1.h>
#include <wincodec.h>

#include "cfg.h"
#include "gfx.h"
#include "main.h"

#define RT ((ID2D1RenderTarget *)dc)
#define FAC ((ID2D1Factory *)fac)
#define FNV0 0xCBF29CE484222325ull
#define NFONT 64
#define NLAY 96
#define NGEO 16
#define NGRAD 8
#define GT_ALIGN 3

static const GUID iid_d2d_factory1 = { 0xbb12d362, 0xdaee, 0x4b9a, { 0xaa, 0x1d, 0x14, 0xba, 0x40, 0x1c, 0xfa, 0x1f } };
static const GUID iid_dxgi_factory1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
static const GUID iid_dxgi_device = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
static const GUID iid_dxgi_device3 = { 0x6007896c, 0x3244, 0x4afd, { 0xbf, 0x18, 0xa6, 0xd3, 0xbe, 0xda, 0x50, 0x23 } };
static const GUID iid_dxgi_surface = { 0xcafcb56c, 0x6ac3, 0x4889, { 0xbf, 0x47, 0x9e, 0x23, 0xbb, 0xd2, 0x60, 0xec } };
static const GUID iid_dwrite_factory = { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };
static const GUID iid_dwrite_layout1 = { 0x9064d822, 0x80a7, 0x465c, { 0xa9, 0x86, 0xdf, 0x65, 0xf7, 0x8b, 0x8f, 0xeb } };
static const GUID clsid_wic = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID iid_wic = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID fmt_pbgra = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10 } };

struct gx_tex {
	int w, h;
	ID3D11Texture2D *tex;
	ID2D1Bitmap1 *bmp;
	struct gx_tex *next;
};

struct gx_bmp {
	int w, h;
	ID2D1Bitmap *bmp;
	ID2D1BitmapBrush *br;
	struct gx_bmp *next;
};

struct font {
	IDWriteTextFormat *fmt;
	IDWriteInlineObject *ell;
	int fam, weight;
	float size, asc, desc;
};

struct lay {
	IDWriteTextLayout *l;
	uint64_t key;
	int len, font, flags;
	float w, h;
	uint32_t used;
};

struct geo {
	uint64_t key;
	ID2D1PathGeometry *g;
	uint32_t used;
};

struct grad {
	uint64_t key;
	ID2D1GradientStopCollection *sc;
	ID2D1LinearGradientBrush *br;
};

struct icon_src {
	const char *d[2];
};

static const struct icon_src icon_src[IC_COUNT] = {
	[IC_GEAR] = { { "M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2zM9 12a3 3 0 1 0 6 0a3 3 0 1 0-6 0z" } },
	[IC_STOPWATCH] = { { "M4.5 13.5a7.5 7.5 0 1 0 15 0a7.5 7.5 0 1 0-15 0zM12 13.5V10M10 3h4" } },
	[IC_PREV] = { { "M6 6.5v11", "M18 6.5 9.5 12 18 17.5Z" } },
	[IC_PLAY] = { { NULL, "M8 5.5v13l10.5-6.5Z" } },
	[IC_PAUSE] = { { "M9 6.5v11M15 6.5v11" } },
	[IC_NEXT] = { { "M18 6.5v11", "M6 6.5 14.5 12 6 17.5Z" } },
	[IC_UNLINK] = { { "M9 17H7a5 5 0 0 1 0-10h2M15 7h2a5 5 0 0 1 0 10h-2M4 4l16 16" } },
	[IC_BATTERY] = { { "M5 7.5h11a2.5 2.5 0 0 1 2.5 2.5v4a2.5 2.5 0 0 1-2.5 2.5H5a2.5 2.5 0 0 1-2.5-2.5v-4A2.5 2.5 0 0 1 5 7.5zM21.5 10.5v3M6.2 10.6v2.8" } },
	[IC_BELL] = { { "M6 16v-5a6 6 0 0 1 12 0v5l1.5 2h-15ZM10 20.5a2 2 0 0 0 4 0" } },
	[IC_BELL_OFF] = { { "M8.6 4.6A6 6 0 0 1 18 9.6V13M6.3 9.4A6 6 0 0 0 6 11v5l-1.5 2H16M10 20.5a2 2 0 0 0 4 0M3.5 3.5l17 17" } },
	[IC_CHECK] = { { "M5 12.5l4.5 4.5L19 7.5" } },
	[IC_WARNING] = { { "M12 4 21 19.5H3ZM12 10v4M12 16.8v.2" } },
	[IC_GHOST] = { { "M5 20V11a7 7 0 0 1 14 0v9l-2.33-1.5L14.33 20 12 18.5 9.67 20 7.33 18.5ZM9.5 10v2M14.5 10v2" } },
	[IC_CHEVRON_DOWN] = { { "M6 9l6 6 6-6" } },
	[IC_CHEVRON_UP] = { { "M6 15l6-6 6 6" } },
	[IC_CHEVRON_LEFT] = { { "M15 6l-6 6 6 6" } },
	[IC_CHEVRON_RIGHT] = { { "M9 6l6 6-6 6" } },
	[IC_PENCIL] = { { "M4 20h4L19 9l-4-4L4 16ZM13.5 6.5l4 4" } },
	[IC_CLOSE] = { { "M6 6l12 12M18 6 6 18" } },
	[IC_PLUS] = { { "M12 6v12M6 12h12" } },
	[IC_MINUS] = { { "M6 12h12" } },
	[IC_SLIDERS] = { { "M4 7h9M17 7h3M4 17h3M11 17h9M13 7a2 2 0 1 0 4 0a2 2 0 1 0-4 0zM7 17a2 2 0 1 0 4 0a2 2 0 1 0-4 0z" } },
	[IC_WATCH] = { { "M9 6h6a3 3 0 0 1 3 3v6a3 3 0 0 1-3 3H9a3 3 0 0 1-3-3V9a3 3 0 0 1 3-3zM9 6V3h6v3M9 18v3h6v-3M12 9.5V12l1.5 1.5" } },
	[IC_POWER] = { { "M12 3v8M7 6.3a7.5 7.5 0 1 0 10 0" } },
	[IC_GLOBE] = { { "M3 12a9 9 0 1 0 18 0a9 9 0 1 0-18 0zM3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18" } },
	[IC_INFO] = { { "M3 12a9 9 0 1 0 18 0a9 9 0 1 0-18 0zM12 11v5M12 7.8v.2" } },
	[IC_ALERT] = { { "M3 12a9 9 0 1 0 18 0a9 9 0 1 0-18 0zM12 7.5v5.5M12 16.3v.2" } },
	[IC_CHECK_CIRCLE] = { { "M3 12a9 9 0 1 0 18 0a9 9 0 1 0-18 0zM8 12.3l2.8 2.8L16.2 9.5" } },
	[IC_RESET] = { { "M4.5 12a7.5 7.5 0 1 0 2.2-5.3M4.5 4.5v4h4" } },
	[IC_PALETTE] = { { "M12 3a9 9 0 1 0 0 18c1.2 0 1.8-.8 1.8-1.7 0-1.1-.9-1.4-.9-2.4 0-.9.8-1.6 1.7-1.6H17a4 4 0 0 0 4-4C21 6.6 17 3 12 3ZM6.3 11a1.2 1.2 0 1 0 2.4 0a1.2 1.2 0 1 0-2.4 0zM8.8 7a1.2 1.2 0 1 0 2.4 0a1.2 1.2 0 1 0-2.4 0zM13.8 7.5a1.2 1.2 0 1 0 2.4 0a1.2 1.2 0 1 0-2.4 0z" } },
	[IC_HEADSET] = { { "M3 9.5A2.5 2.5 0 0 1 5.5 7h13A2.5 2.5 0 0 1 21 9.5v5a2.5 2.5 0 0 1-2.5 2.5h-3l-2-2.5h-3L8.5 17h-3A2.5 2.5 0 0 1 3 14.5Z" } },
	[IC_CONTROLLER] = { { "M8 7a4 4 0 0 1 8 0v3a4 4 0 0 1-8 0zM12 14v7M10.8 7.5a1.2 1.2 0 1 0 2.4 0a1.2 1.2 0 1 0-2.4 0z" } },
	[IC_CTRL_L] = { { "M6.82 14.98A9 6.6-18 1 1 19.8 8.69M9.23 11.71L14.71 20.83A2.2 2.2 0 0 0 18.73 19.13L15.97 8.84A3.7 3.7 0 1 0 9.23 11.71ZM12.39 9.8h.01" } },
	[IC_CTRL_R] = { { "M17.18 14.98A9 6.6 18 1 0 4.2 8.69M14.77 11.71L9.29 20.83A2.2 2.2 0 0 1 5.27 19.13L8.03 8.84A3.7 3.7 0 1 1 14.77 11.71ZM11.61 9.8h-.01" } },
	[IC_TRACKER] = { { "M5 13a7 7 0 1 0 14 0a7 7 0 1 0-14 0zM9.5 13a2.5 2.5 0 1 0 5 0a2.5 2.5 0 1 0-5 0zM12 6V3" } },
	[IC_MOVE] = { { "M12 3v18M3 12h18M9 6l3-3 3 3M9 18l3 3 3-3M6 9l-3 3 3 3M18 9l3 3-3 3" } },
	[IC_BACKSPACE] = { { "M10 5a2 2 0 0 0-1.344.519l-6.328 5.74a1 1 0 0 0 0 1.481l6.328 5.741A2 2 0 0 0 10 19h10a2 2 0 0 0 2-2V7a2 2 0 0 0-2-2zM12 9l6 6M18 9l-6 6" } },
	[IC_BOLT] = { { NULL, "M13 2 4 14h7l-1 8 9-12h-7Z" } },
	[IC_HISTORY] = { { "M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8M3 3v5h5M12 7v5l4 2" } },
	[IC_MOON] = { { "M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z" } },
	[IC_MOON_FILL] = { { NULL, "M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z" } },
};

uint32_t g_gfx_gen;
static int adapter_idx;
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID2D1Factory1 *fac;
static ID2D1Device *d2dev;
static ID2D1DeviceContext *dc;
static ID2D1SolidColorBrush *brush;
static ID2D1StrokeStyle *round_st;
static IDWriteFactory *dw;
static IDWriteFontCollection *sysfc;
static IDWriteTypography *tnum, *pnum;
static const WCHAR *families[2];
static struct gx_tex *texs, *cur;
static struct gx_bmp *bmps;
static struct font fonts[NFONT];
static int nfonts;
static struct lay lays[NLAY];
static uint32_t lay_tick;
static struct geo geos[NGEO];
static uint32_t geo_tick;
static struct grad grads[NGRAD];
static int grad_next;
static ID2D1PathGeometry *icons[IC_COUNT][2];
static bool drawing;
static int clips, layers;
static float op = 1, xs = 1, xdx, xdy;
static ULONGLONG retry_at;

static void rel(void *pp) {
	IUnknown **p = pp;
	if (*p) {
		IUnknown_Release(*p);
		*p = NULL;
	}
}

static uint64_t fnv(const void *p, size_t n, uint64_t h) {
	const uint8_t *b = p;
	size_t i;
	for (i = 0; i < n; i++) {
		h = (h ^ b[i]) * 0x100000001B3ull;
	}
	return h;
}

static D2D1_MATRIX_3X2_F mat(float s, float dx, float dy) {
	return (D2D1_MATRIX_3X2_F){ .m = { { s, 0 }, { 0, s }, { dx, dy } } };
}

static ID2D1Brush *solid(float r, float g, float b, float a) {
	D2D1_COLOR_F c = { r, g, b, a };
	ID2D1SolidColorBrush_SetColor(brush, &c);
	return (ID2D1Brush *)brush;
}

static ID2D1Brush *col(struct gx_color c) {
	return solid(c.r, c.g, c.b, c.a * op);
}

static D2D1_ROUNDED_RECT rrect(float x, float y, float w, float h, float r) {
	r = fmaxf(0, fminf(r, fminf(w, h) / 2));
	return (D2D1_ROUNDED_RECT){ { x, y, x + w, y + h }, r, r };
}

static void tex_drop(struct gx_tex *t) {
	if (cur == t) {
		dc->lpVtbl->SetTarget(dc, NULL);
		cur = NULL;
	}
	rel(&t->bmp);
	rel(&t->tex);
}

static bool tex_make(struct gx_tex *t) {
	D3D11_TEXTURE2D_DESC d = { (UINT)t->w, (UINT)t->h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
	D2D1_BITMAP_PROPERTIES1 bp = { { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96, 96, D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, NULL };
	IDXGISurface *s = NULL;
	bool ok = dc && SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &t->tex)) && SUCCEEDED(ID3D11Texture2D_QueryInterface(t->tex, &iid_dxgi_surface, (void **)&s)) && SUCCEEDED(dc->lpVtbl->CreateBitmapFromDxgiSurface(dc, s, &bp, &t->bmp));
	rel(&s);
	if (!ok) {
		tex_drop(t);
	}
	return ok;
}

static void bmp_drop(struct gx_bmp *b) {
	rel(&b->br);
	rel(&b->bmp);
}

static bool bmp_make(struct gx_bmp *b) {
	D2D1_SIZE_U sz = { (UINT32)b->w, (UINT32)b->h };
	D2D1_BITMAP_PROPERTIES bp = { { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96, 96 };
	D2D1_BITMAP_BRUSH_PROPERTIES bbp = { D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR };
	bool ok = dc && SUCCEEDED(ID2D1RenderTarget_CreateBitmap(RT, sz, NULL, 0, &bp, &b->bmp)) && SUCCEEDED(RT->lpVtbl->CreateBitmapBrush(RT, b->bmp, &bbp, NULL, &b->br));
	if (!ok) {
		bmp_drop(b);
	}
	return ok;
}

static void dev_drop(void) {
	struct gx_tex *t;
	struct gx_bmp *b;
	int i;
	for (t = texs; t; t = t->next) {
		tex_drop(t);
	}
	for (b = bmps; b; b = b->next) {
		bmp_drop(b);
	}
	for (i = 0; i < NGRAD; i++) {
		rel(&grads[i].br);
		rel(&grads[i].sc);
	}
	drawing = false;
	rel(&brush);
	rel(&dc);
	rel(&d2dev);
	if (ctx) {
		ID3D11DeviceContext_ClearState(ctx);
		ID3D11DeviceContext_Flush(ctx);
	}
	rel(&ctx);
	rel(&dev);
}

static bool dev_make(void) {
	IDXGIFactory1 *xf = NULL;
	IDXGIAdapter1 *ad = NULL;
	DXGI_ADAPTER_DESC1 ds = { 0 };
	char name[256] = "default";
	UINT fl = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
	HRESULT hr;
	IDXGIDevice *xd = NULL;
	D2D1_COLOR_F white = { 1, 1, 1, 1 };
	bool ok;
	struct gx_tex *t;
	struct gx_bmp *b;
	if (adapter_idx >= 0 && SUCCEEDED(CreateDXGIFactory1(&iid_dxgi_factory1, (void **)&xf)) && SUCCEEDED(IDXGIFactory1_EnumAdapters1(xf, adapter_idx, &ad)) && SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &ds))) {
		to_utf8(ds.Description, name, sizeof(name));
	}
	rel(&xf);
	hr = D3D11CreateDevice((IDXGIAdapter *)ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, fl, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx);
	rel(&ad);
	if (FAILED(hr)) {
		glog("gfx: hardware device failed (0x%08lx), using WARP", hr);
		strcpy(name, "WARP");
		hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, fl, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx);
	}
	if (FAILED(hr)) return false;
	ok = SUCCEEDED(ID3D11Device_QueryInterface(dev, &iid_dxgi_device, (void **)&xd)) && SUCCEEDED(ID2D1Factory1_CreateDevice(fac, xd, &d2dev)) && SUCCEEDED(ID2D1Device_CreateDeviceContext(d2dev, D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)) && SUCCEEDED(ID2D1RenderTarget_CreateSolidColorBrush(RT, &white, NULL, &brush));
	rel(&xd);
	if (!ok) return false;
	ID2D1RenderTarget_SetTextAntialiasMode(RT, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
	for (t = texs; t; t = t->next) {
		tex_make(t);
	}
	for (b = bmps; b; b = b->next) {
		bmp_make(b);
	}
	glog("gfx: device on %s (adapter %d)", name, adapter_idx);
	return true;
}

static bool dev_ensure(void) {
	if (dc) return true;
	if (!fac || GetTickCount64() < retry_at) return false;
	dev_drop();
	if (dev_make()) {
		g_gfx_gen++;
		return true;
	}
	dev_drop();
	retry_at = GetTickCount64() + 2000;
	return false;
}

void gfx_shutdown(void) {
	int i;
	while (texs) {
		gx_tex_free(texs);
	}
	while (bmps) {
		gx_bmp_free(bmps);
	}
	dev_drop();
	for (i = 0; i < NLAY; i++) {
		rel(&lays[i].l);
	}
	for (i = 0; i < nfonts; i++) {
		rel(&fonts[i].ell);
		rel(&fonts[i].fmt);
	}
	for (i = 0; i < NGEO; i++) {
		rel(&geos[i].g);
	}
	for (i = 0; i < IC_COUNT; i++) {
		rel(&icons[i][0]);
		rel(&icons[i][1]);
	}
	rel(&tnum);
	rel(&pnum);
	rel(&sysfc);
	rel(&dw);
	rel(&round_st);
	rel(&fac);
}

bool gfx_init(int adapter) {
	static const WCHAR *want[2] = { L"Segoe UI Variable Display", L"Segoe UI Variable Text" };
	D2D1_FACTORY_OPTIONS fo = { D2D1_DEBUG_LEVEL_NONE };
	D2D1_STROKE_STYLE_PROPERTIES sp = { D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_SOLID, 0 };
	DWRITE_FONT_FEATURE ft = { DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1 }, fp = { DWRITE_FONT_FEATURE_TAG_PROPORTIONAL_FIGURES, 1 };
	bool ok;
	int i;
	UINT32 idx;
	BOOL ex;
	if (fac) return dev_ensure();
	adapter_idx = adapter;
	ok = SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &iid_d2d_factory1, &fo, (void **)&fac)) && SUCCEEDED(ID2D1Factory_CreateStrokeStyle(FAC, &sp, NULL, 0, &round_st)) && SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &iid_dwrite_factory, (IUnknown **)&dw)) && SUCCEEDED(IDWriteFactory_GetSystemFontCollection(dw, &sysfc, FALSE)) && SUCCEEDED(IDWriteFactory_CreateTypography(dw, &tnum)) && SUCCEEDED(IDWriteTypography_AddFontFeature(tnum, ft)) && SUCCEEDED(IDWriteFactory_CreateTypography(dw, &pnum)) && SUCCEEDED(IDWriteTypography_AddFontFeature(pnum, fp)) && dev_make();
	if (!ok) {
		glog("gfx: init failed");
		gfx_shutdown();
		return false;
	}
	for (i = 0; i < 2; i++) {
		ex = FALSE;
		IDWriteFontCollection_FindFamilyName(sysfc, want[i], &idx, &ex);
		families[i] = ex ? want[i] : L"Segoe UI";
		if (!ex) {
			glog("gfx: %ls missing, using Segoe UI", want[i]);
		}
	}
	gx_measure(gx_font(GF_DISPLAY, 22, 600), L"0123456789:%", 0, GT_TNUM, NULL);
	gx_measure(gx_font(GF_TEXT, 20, 500), L"Headset 0123456789", 0, 0, NULL);
	return true;
}

struct gx_tex *gx_tex_new(int w, int h) {
	struct gx_tex *t;
	if (w <= 0 || h <= 0) return NULL;
	t = calloc(1, sizeof(*t));
	if (!t) return NULL;
	t->w = w;
	t->h = h;
	if (dc && !tex_make(t)) {
		free(t);
		return NULL;
	}
	t->next = texs;
	texs = t;
	return t;
}

void gx_tex_free(struct gx_tex *t) {
	struct gx_tex **p;
	IDXGIDevice3 *x3;
	if (!t) return;
	for (p = &texs; *p; p = &(*p)->next) {
		if (*p == t) {
			*p = t->next;
			break;
		}
	}
	tex_drop(t);
	free(t);
	if (!dc) return;
	x3 = NULL;
	ID2D1Device_ClearResources(d2dev, 0);
	ID3D11DeviceContext_Flush(ctx);
	if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &iid_dxgi_device3, (void **)&x3))) {
		IDXGIDevice3_Trim(x3);
	}
	rel(&x3);
}

void *gx_tex_native(struct gx_tex *t) {
	return t ? t->tex : NULL;
}

bool gx_begin(struct gx_tex *t) {
	D2D1_COLOR_F z = { 0, 0, 0, 0 };
	D2D1_MATRIX_3X2_F m;
	if (drawing || !t || !dev_ensure() || (!t->bmp && !tex_make(t))) return false;
	if (cur != t) {
		dc->lpVtbl->SetTarget(dc, (ID2D1Image *)t->bmp);
		cur = t;
	}
	m = mat(1, 0, 0);
	ID2D1RenderTarget_BeginDraw(RT);
	ID2D1RenderTarget_SetTransform(RT, &m);
	ID2D1RenderTarget_Clear(RT, &z);
	op = xs = 1;
	xdx = xdy = 0;
	clips = layers = 0;
	drawing = true;
	return true;
}

bool gx_end(void) {
	HRESULT hr;
	if (!drawing) return false;
	for (; layers > 0; layers--) {
		ID2D1RenderTarget_PopLayer(RT);
	}
	for (; clips > 0; clips--) {
		ID2D1RenderTarget_PopAxisAlignedClip(RT);
	}
	drawing = false;
	hr = ID2D1RenderTarget_EndDraw(RT, NULL, NULL);
	ID3D11DeviceContext_Flush(ctx);
	if (hr != D2DERR_RECREATE_TARGET && ID3D11Device_GetDeviceRemovedReason(dev) == S_OK) return SUCCEEDED(hr);
	glog("gfx: device lost (0x%08lx), recreating", hr);
	dev_drop();
	retry_at = 0;
	dev_ensure();
	return false;
}

void gx_opacity(float a) {
	op = fmaxf(0, fminf(1, a));
}

void gx_transform(float scale, float dx, float dy) {
	D2D1_MATRIX_3X2_F m;
	xs = scale;
	xdx = dx;
	xdy = dy;
	if (!drawing) return;
	m = mat(scale, dx, dy);
	ID2D1RenderTarget_SetTransform(RT, &m);
}

void gx_clip_push(float x, float y, float w, float h) {
	D2D1_RECT_F r;
	if (!drawing) return;
	r = (D2D1_RECT_F){ x, y, x + w, y + h };
	ID2D1RenderTarget_PushAxisAlignedClip(RT, &r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
	clips++;
}

void gx_clip_pop(void) {
	if (!drawing || !clips) return;
	ID2D1RenderTarget_PopAxisAlignedClip(RT);
	clips--;
}

static ID2D1Brush *grad_brush(const struct gx_stop *stops, int n, float x0, float y0, float x1, float y1, float a) {
	uint64_t key;
	struct grad *g = NULL;
	int i;
	D2D1_GRADIENT_STOP gs[8];
	D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lp;
	if (!stops || n < 1) return NULL;
	n = n > 8 ? 8 : n;
	key = fnv(stops, n * sizeof(*stops), FNV0);
	for (i = 0; i < NGRAD && !g; i++) {
		if (grads[i].br && grads[i].key == key) {
			g = &grads[i];
		}
	}
	if (!g) {
		g = &grads[grad_next];
		grad_next = (grad_next + 1) % NGRAD;
		rel(&g->br);
		rel(&g->sc);
		for (i = 0; i < n; i++) {
			gs[i] = (D2D1_GRADIENT_STOP){ stops[i].pos, { stops[i].c.r, stops[i].c.g, stops[i].c.b, stops[i].c.a } };
		}
		lp = (D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES){ { x0, y0 }, { x1, y1 } };
		if (FAILED(RT->lpVtbl->CreateGradientStopCollection(RT, gs, n, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &g->sc)) || FAILED(ID2D1RenderTarget_CreateLinearGradientBrush(RT, &lp, NULL, g->sc, &g->br))) {
			rel(&g->sc);
			return NULL;
		}
		g->key = key;
	}
	ID2D1LinearGradientBrush_SetStartPoint(g->br, ((D2D1_POINT_2F){ x0, y0 }));
	ID2D1LinearGradientBrush_SetEndPoint(g->br, ((D2D1_POINT_2F){ x1, y1 }));
	ID2D1LinearGradientBrush_SetOpacity(g->br, a);
	return (ID2D1Brush *)g->br;
}

static void push(D2D1_RECT_F r, float a, ID2D1Brush *mask) {
	D2D1_LAYER_PARAMETERS1 p = { r, NULL, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, { .m = { { 1, 0 }, { 0, 1 }, { 0, 0 } } }, a, mask, D2D1_LAYER_OPTIONS1_NONE };
	dc->lpVtbl->PushLayer(dc, &p, NULL);
	layers++;
}

void gx_layer_push(float a) {
	if (!drawing) return;
	push((D2D1_RECT_F){ -FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX }, a, NULL);
}

void gx_mask_push(float x, float y, float w, float h, const struct gx_stop *stops, int n) {
	if (!drawing) return;
	push((D2D1_RECT_F){ x, y, x + w, y + h }, 1, grad_brush(stops, n, x, 0, x + w, 0, 1));
}

void gx_layer_pop(void) {
	if (!drawing || !layers) return;
	ID2D1RenderTarget_PopLayer(RT);
	layers--;
}

void gx_fill_rect(float x, float y, float w, float h, struct gx_color c) {
	D2D1_RECT_F r;
	if (!drawing) return;
	r = (D2D1_RECT_F){ x, y, x + w, y + h };
	ID2D1RenderTarget_FillRectangle(RT, &r, col(c));
}

void gx_fill_rrect(float x, float y, float w, float h, float r, struct gx_color c) {
	D2D1_ROUNDED_RECT rr;
	if (!drawing || w <= 0 || h <= 0) return;
	rr = rrect(x, y, w, h, r);
	ID2D1RenderTarget_FillRoundedRectangle(RT, &rr, col(c));
}

void gx_stroke_rrect(float x, float y, float w, float h, float r, float sw, struct gx_color c) {
	float k;
	D2D1_ROUNDED_RECT rr;
	if (!drawing || w <= sw || h <= sw || sw <= 0) return;
	k = sw / 2;
	rr = rrect(x + k, y + k, w - sw, h - sw, fminf(r, fminf(w, h) / 2) - k);
	ID2D1RenderTarget_DrawRoundedRectangle(RT, &rr, col(c), sw, NULL);
}

void gx_grad_rrect(float x, float y, float w, float h, float r, float x0, float y0, float x1, float y1, const struct gx_stop *stops, int n) {
	ID2D1Brush *br;
	D2D1_ROUNDED_RECT rr;
	if (!drawing || w <= 0 || h <= 0 || !(br = grad_brush(stops, n, x0, y0, x1, y1, op))) return;
	rr = rrect(x, y, w, h, r);
	ID2D1RenderTarget_FillRoundedRectangle(RT, &rr, br);
}

void gx_shadow_rrect(float x, float y, float w, float h, float r, float blur, float dy, struct gx_color c) {
	const int n = 16;
	float a, sig, step, prev = 0, e, t, la;
	int k;
	D2D1_ROUNDED_RECT rr;
	if (!drawing) return;
	a = c.a * op;
	sig = blur * 0.70710678f;
	step = 2 * blur / n;
	for (k = 0; k <= n; k++) {
		e = blur - k * step;
		t = k == n ? a : a * 0.5f * erfcf((e - step / 2) / sig);
		la = 1 - (1 - t) / (1 - prev);
		prev = t;
		if (w + 2 * e <= 0 || h + 2 * e <= 0 || la <= 0) continue;
		rr = rrect(x - e, y + dy - e, w + 2 * e, h + 2 * e, r + e);
		ID2D1RenderTarget_FillRoundedRectangle(RT, &rr, solid(c.r, c.g, c.b, la));
	}
}

void gx_fill_circle(float cx, float cy, float r, struct gx_color c) {
	gx_fill_ellipse(cx, cy, r, r, c);
}

void gx_stroke_circle(float cx, float cy, float r, float sw, struct gx_color c) {
	D2D1_ELLIPSE e;
	if (!drawing || r <= 0) return;
	e = (D2D1_ELLIPSE){ { cx, cy }, r, r };
	ID2D1RenderTarget_DrawEllipse(RT, &e, col(c), sw, NULL);
}

void gx_fill_ellipse(float cx, float cy, float rx, float ry, struct gx_color c) {
	D2D1_ELLIPSE e;
	if (!drawing || rx <= 0 || ry <= 0) return;
	e = (D2D1_ELLIPSE){ { cx, cy }, rx, ry };
	ID2D1RenderTarget_FillEllipse(RT, &e, col(c));
}

static ID2D1GeometrySink *geo_open(ID2D1PathGeometry **g) {
	ID2D1GeometrySink *s = NULL;
	if (FAILED(ID2D1Factory_CreatePathGeometry(FAC, g))) return NULL;
	if (SUCCEEDED(ID2D1PathGeometry_Open(*g, &s))) return s;
	rel(g);
	return NULL;
}

static bool geo_close(ID2D1PathGeometry **g, ID2D1GeometrySink *s) {
	bool ok = SUCCEEDED(ID2D1GeometrySink_Close(s));
	ID2D1GeometrySink_Release(s);
	if (!ok) {
		rel(g);
	}
	return ok;
}

static struct geo *geo_find(uint64_t key) {
	struct geo *v = &geos[0], *e;
	int i;
	for (i = 0; i < NGEO; i++) {
		e = &geos[i];
		if (e->g && e->key == key) {
			e->used = ++geo_tick;
			return e;
		}
		if (!e->g || (v->g && e->used < v->used)) {
			v = e;
		}
	}
	rel(&v->g);
	v->key = key;
	v->used = ++geo_tick;
	return v;
}

void gx_arc(float cx, float cy, float r, float sw, float start_deg, float sweep_deg, struct gx_color c) {
	float k[5] = { cx, cy, r, start_deg, sweep_deg }, a0, a1;
	struct geo *e;
	ID2D1GeometrySink *s;
	D2D1_ARC_SEGMENT as;
	if (!drawing || r <= 0 || sweep_deg <= 0) return;
	if (sweep_deg >= 360) {
		gx_stroke_circle(cx, cy, r, sw, c);
		return;
	}
	e = geo_find(fnv(k, sizeof(k), FNV0 ^ 1));
	if (!e->g) {
		s = geo_open(&e->g);
		if (!s) return;
		a0 = start_deg * DEG;
		a1 = (start_deg + sweep_deg) * DEG;
		as = (D2D1_ARC_SEGMENT){ { cx + r * sinf(a1), cy - r * cosf(a1) }, { r, r }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, sweep_deg > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL };
		ID2D1GeometrySink_BeginFigure(s, ((D2D1_POINT_2F){ cx + r * sinf(a0), cy - r * cosf(a0) }), D2D1_FIGURE_BEGIN_HOLLOW);
		ID2D1GeometrySink_AddArc(s, &as);
		ID2D1GeometrySink_EndFigure(s, D2D1_FIGURE_END_OPEN);
		if (!geo_close(&e->g, s)) return;
	}
	ID2D1RenderTarget_DrawGeometry(RT, (ID2D1Geometry *)e->g, col(c), sw, round_st);
}

void gx_line(float x0, float y0, float x1, float y1, float sw, struct gx_color c) {
	if (!drawing) return;
	ID2D1RenderTarget_DrawLine(RT, ((D2D1_POINT_2F){ x0, y0 }), ((D2D1_POINT_2F){ x1, y1 }), col(c), sw, round_st);
}

static const char *skip(const char *p) {
	while (*p == ' ' || *p == ',') {
		p++;
	}
	return p;
}

static const char *num(const char *p, float *v) {
	float s, r = 0, k;
	p = skip(p);
	s = *p == '-' ? -1 : 1;
	p += *p == '-' || *p == '+';
	for (; *p >= '0' && *p <= '9'; p++) {
		r = r * 10 + (*p - '0');
	}
	if (*p == '.') {
		for (k = 0.1f; *++p >= '0' && *p <= '9'; k *= 0.1f) {
			r += (*p - '0') * k;
		}
	}
	*v = s * r;
	return p;
}

static ID2D1PathGeometry *svg_path(const char *d) {
	ID2D1PathGeometry *g = NULL;
	ID2D1GeometrySink *s = geo_open(&g);
	float cx = 0, cy = 0, sx = 0, sy = 0, a[7] = { 0 }, ox, oy;
	bool open = false;
	char cmd = 0, u;
	const char *p;
	int na, i;
	D2D1_ARC_SEGMENT as;
	D2D1_BEZIER_SEGMENT bz;
	if (!s) return NULL;
	for (p = skip(d); *p; p = skip(p)) {
		if ((*p | 32) >= 'a' && (*p | 32) <= 'z') {
			cmd = *p++;
		} else if (cmd == 'M' || cmd == 'm') {
			cmd = cmd == 'M' ? 'L' : 'l';
		} else if (!cmd || (cmd | 32) == 'z') {
			break;
		}
		u = cmd & ~32;
		na = u == 'A' ? 7 : u == 'C' ? 6 : u == 'H' || u == 'V' ? 1 : u == 'Z' ? 0 : 2;
		for (i = 0; i < na; i++) {
			p = num(p, &a[i]);
		}
		ox = cmd >= 'a' ? cx : 0;
		oy = cmd >= 'a' ? cy : 0;
		if (u == 'M' || u == 'Z') {
			if (open) {
				ID2D1GeometrySink_EndFigure(s, u == 'Z' ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
			}
			open = false;
			cx = sx = u == 'M' ? a[0] + ox : sx;
			cy = sy = u == 'M' ? a[1] + oy : sy;
			continue;
		}
		if (!open) {
			ID2D1GeometrySink_BeginFigure(s, ((D2D1_POINT_2F){ cx, cy }), D2D1_FIGURE_BEGIN_FILLED);
			sx = cx;
			sy = cy;
			open = true;
		}
		if (u == 'A') {
			cx = a[5] + ox;
			cy = a[6] + oy;
			as = (D2D1_ARC_SEGMENT){ { cx, cy }, { a[0], a[1] }, a[2], a[4] != 0 ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, a[3] != 0 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL };
			ID2D1GeometrySink_AddArc(s, &as);
		} else if (u == 'C') {
			bz = (D2D1_BEZIER_SEGMENT){ { a[0] + ox, a[1] + oy }, { a[2] + ox, a[3] + oy }, { a[4] + ox, a[5] + oy } };
			cx = a[4] + ox;
			cy = a[5] + oy;
			ID2D1GeometrySink_AddBezier(s, &bz);
		} else {
			cx = u == 'V' ? cx : a[0] + ox;
			cy = u == 'H' ? cy : u == 'V' ? a[0] + oy : a[1] + oy;
			ID2D1GeometrySink_AddLine(s, ((D2D1_POINT_2F){ cx, cy }));
		}
	}
	if (open) {
		ID2D1GeometrySink_EndFigure(s, D2D1_FIGURE_END_OPEN);
	}
	return geo_close(&g, s) ? g : NULL;
}

void gx_icon(enum gx_icon i, float x, float y, float size, float sw, struct gx_color c) {
	int k;
	float s;
	D2D1_MATRIX_3X2_F m, m0;
	ID2D1Brush *b;
	if (!drawing || i < 0 || i >= IC_COUNT || size <= 0) return;
	for (k = 0; k < 2; k++) {
		if (!icons[i][k] && icon_src[i].d[k]) {
			icons[i][k] = svg_path(icon_src[i].d[k]);
		}
	}
	s = size / 24;
	m = mat(xs * s, xs * x + xdx, xs * y + xdy);
	m0 = mat(xs, xdx, xdy);
	b = col(c);
	ID2D1RenderTarget_SetTransform(RT, &m);
	if (icons[i][0]) {
		ID2D1RenderTarget_DrawGeometry(RT, (ID2D1Geometry *)icons[i][0], b, sw, round_st);
	}
	if (icons[i][1]) {
		ID2D1RenderTarget_FillGeometry(RT, (ID2D1Geometry *)icons[i][1], b, NULL);
		ID2D1RenderTarget_DrawGeometry(RT, (ID2D1Geometry *)icons[i][1], b, sw, round_st);
	}
	ID2D1RenderTarget_SetTransform(RT, &m0);
}

int gx_font(enum gx_family f, float size, int weight) {
	int fam = f == GF_TEXT, i;
	for (i = 0; i < nfonts; i++) {
		if (fonts[i].fam == fam && fonts[i].size == size && fonts[i].weight == weight) return i;
	}
	if (nfonts == NFONT) return 0;
	fonts[nfonts] = (struct font){ .fam = fam, .weight = weight, .size = size };
	return nfonts++;
}

static struct font *font_get(int id) {
	struct font *f;
	UINT32 idx;
	BOOL ex = FALSE;
	IDWriteFontFamily *ff = NULL;
	IDWriteFont *fn = NULL;
	DWRITE_FONT_METRICS m;
	if (id < 0 || id >= nfonts || !dw || !families[0]) return NULL;
	f = &fonts[id];
	if (f->fmt) return f;
	if (FAILED(IDWriteFactory_CreateTextFormat(dw, families[f->fam], NULL, (DWRITE_FONT_WEIGHT)f->weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, f->size, L"en-us", &f->fmt))) return NULL;
	IDWriteFactory_CreateEllipsisTrimmingSign(dw, f->fmt, &f->ell);
	f->asc = f->size * 1.079f;
	f->desc = f->size * 0.251f;
	if (SUCCEEDED(IDWriteFontCollection_FindFamilyName(sysfc, families[f->fam], &idx, &ex)) && ex && SUCCEEDED(IDWriteFontCollection_GetFontFamily(sysfc, idx, &ff)) && SUCCEEDED(IDWriteFontFamily_GetFirstMatchingFont(ff, (DWRITE_FONT_WEIGHT)f->weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &fn))) {
		IDWriteFont_GetMetrics(fn, &m);
		f->asc = f->size * m.ascent / m.designUnitsPerEm;
		f->desc = f->size * m.descent / m.designUnitsPerEm;
	}
	rel(&fn);
	rel(&ff);
	return f;
}

static struct lay *layout(int font, const WCHAR *s, float w, int flags) {
	struct font *f = font_get(font);
	struct lay *v = &lays[0], *e;
	int n, i, al;
	uint64_t key;
	DWRITE_TEXT_RANGE all;
	DWRITE_TRIMMING tr = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
	IDWriteTextLayout1 *l1;
	if (!f || !s) return NULL;
	n = (int)wcslen(s);
	w = w > 0 ? w : 0;
	key = fnv(s, n * sizeof(*s), fnv(&w, sizeof(w), FNV0 ^ (uint64_t)font << 32 ^ (uint32_t)flags));
	for (i = 0; i < NLAY; i++) {
		e = &lays[i];
		if (e->l && e->key == key && e->len == n && e->font == font && e->flags == flags && e->w == w) {
			e->used = ++lay_tick;
			return e;
		}
		if (!e->l || (v->l && e->used < v->used)) {
			v = e;
		}
	}
	rel(&v->l);
	if (FAILED(IDWriteFactory_CreateTextLayout(dw, s, n, f->fmt, w > 0 ? w : 1e6f, 1e6f, &v->l))) return NULL;
	all = (DWRITE_TEXT_RANGE){ 0, (UINT32)n };
	al = flags & GT_ALIGN;
	IDWriteTextLayout_SetTextAlignment(v->l, w > 0 && al == GT_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER : w > 0 && al == GT_RIGHT ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
	IDWriteTextLayout_SetWordWrapping(v->l, flags & GT_WRAP ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
	if (flags & GT_ELLIPSIS && f->ell) {
		IDWriteTextLayout_SetTrimming(v->l, &tr, f->ell);
	}
	if (flags & (GT_TNUM | GT_PNUM)) {
		IDWriteTextLayout_SetTypography(v->l, flags & GT_TNUM ? tnum : pnum, all);
	}
	if (flags & GT_TIGHT && SUCCEEDED(IDWriteTextLayout_QueryInterface(v->l, &iid_dwrite_layout1, (void **)&l1))) {
		IDWriteTextLayout1_SetCharacterSpacing(l1, 0, -0.02f * f->size, 0, all);
		IDWriteTextLayout1_Release(l1);
	}
	v->key = key;
	v->len = n;
	v->font = font;
	v->flags = flags;
	v->w = w;
	v->h = -1;
	v->used = ++lay_tick;
	return v;
}

static void lay_height(struct lay *e, float h) {
	struct font *f;
	int lines;
	if (e->h == h) return;
	f = &fonts[e->font];
	lines = e->flags >> 8 & 255;
	e->h = h;
	if (h > 0) {
		IDWriteTextLayout_SetLineSpacing(e->l, DWRITE_LINE_SPACING_METHOD_UNIFORM, h, (h - f->asc - f->desc) / 2 + f->asc);
	} else {
		IDWriteTextLayout_SetLineSpacing(e->l, DWRITE_LINE_SPACING_METHOD_DEFAULT, 0, 0);
	}
	IDWriteTextLayout_SetMaxHeight(e->l, lines && h > 0 ? lines * h + 0.5f : 1e6f);
}

float gx_text(int font, const WCHAR *s, float x, float y, float w, float h, struct gx_color c, int flags) {
	struct lay *e;
	DWRITE_TEXT_METRICS m;
	int al;
	if (!drawing || !s || !*s) return 0;
	e = layout(font, s, w, flags);
	if (!e) return 0;
	lay_height(e, h);
	if (FAILED(IDWriteTextLayout_GetMetrics(e->l, &m))) return 0;
	al = flags & GT_ALIGN;
	if (w <= 0) {
		x -= al == GT_CENTER ? m.width / 2 : al == GT_RIGHT ? m.width : 0;
	}
	ID2D1RenderTarget_DrawTextLayout(RT, ((D2D1_POINT_2F){ x, y }), e->l, col(c), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
	return m.width;
}

float gx_measure(int font, const WCHAR *s, float w, int flags, int *lines) {
	DWRITE_TEXT_METRICS m;
	struct lay *e = s && *s ? layout(font, s, w, flags) : NULL;
	int ml;
	if (!e || FAILED(IDWriteTextLayout_GetMetrics(e->l, &m))) {
		if (lines) {
			*lines = 0;
		}
		return 0;
	}
	if (lines) {
		ml = flags >> 8 & 255;
		*lines = ml && (int)m.lineCount > ml ? ml : (int)m.lineCount;
	}
	return m.width;
}

void gx_ink(int font, const WCHAR *s, int flags, float *l, float *r) {
	DWRITE_OVERHANG_METRICS o;
	float w = gx_measure(font, s, 0, flags, NULL);
	struct lay *e = w > 0 ? layout(font, s, w, flags) : NULL;
	bool ok = e && SUCCEEDED(IDWriteTextLayout_GetOverhangMetrics(e->l, &o));
	*l = ok ? -o.left : 0;
	*r = ok ? w + o.right : w;
}

float gx_baseline(int font) {
	struct font *f = font_get(font);
	if (!f) return 18;
	return (24 - f->asc - f->desc) / 2 + f->asc;
}

struct gx_bmp *gx_bmp_new(int w, int h) {
	struct gx_bmp *b;
	if (w <= 0 || h <= 0) return NULL;
	b = calloc(1, sizeof(*b));
	if (!b) return NULL;
	b->w = w;
	b->h = h;
	if (dc && !bmp_make(b)) {
		free(b);
		return NULL;
	}
	b->next = bmps;
	bmps = b;
	return b;
}

void gx_bmp_free(struct gx_bmp *b) {
	struct gx_bmp **p;
	if (!b) return;
	for (p = &bmps; *p; p = &(*p)->next) {
		if (*p == b) {
			*p = b->next;
			break;
		}
	}
	bmp_drop(b);
	free(b);
}

bool gx_bmp_set(struct gx_bmp *b, const void *pbgra, int stride) {
	return b && b->bmp && pbgra && SUCCEEDED(ID2D1Bitmap_CopyFromMemory(b->bmp, NULL, pbgra, (UINT32)stride));
}

void gx_bmp_draw(struct gx_bmp *b, float x, float y, float w, float h, float radius) {
	D2D1_MATRIX_3X2_F m;
	D2D1_ROUNDED_RECT rr;
	if (!drawing || !b || !b->br || w <= 0 || h <= 0) return;
	m = (D2D1_MATRIX_3X2_F){ .m = { { w / b->w, 0 }, { 0, h / b->h }, { x, y } } };
	rr = rrect(x, y, w, h, radius);
	ID2D1BitmapBrush_SetTransform(b->br, &m);
	ID2D1BitmapBrush_SetOpacity(b->br, op);
	ID2D1RenderTarget_FillRoundedRectangle(RT, &rr, (ID2D1Brush *)b->br);
}

static IWICImagingFactory *wic(void) {
	IWICImagingFactory *f = NULL;
	CoCreateInstance(&clsid_wic, NULL, CLSCTX_INPROC_SERVER, &iid_wic, (void **)&f);
	return f;
}

#ifdef GL_TESTS
static const GUID fmt_bgra = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };
static const GUID fmt_png = { 0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf } };

bool gx_save_png(struct gx_tex *t, const WCHAR *path) {
	D3D11_TEXTURE2D_DESC d;
	ID3D11Texture2D *st = NULL;
	D3D11_MAPPED_SUBRESOURCE ms;
	UINT row, i;
	uint8_t *px, *o;
	const uint8_t *in;
	bool ok;
	int y, a, k, v;
	IWICImagingFactory *f;
	IWICStream *s = NULL;
	IWICBitmapEncoder *en = NULL;
	IWICBitmapFrameEncode *fr = NULL;
	WICPixelFormatGUID pf = fmt_bgra;
	if (!t || !t->tex || !dev || !path) return false;
	d = (D3D11_TEXTURE2D_DESC){ (UINT)t->w, (UINT)t->h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0 };
	row = t->w * 4;
	px = malloc((size_t)row * t->h);
	ok = px && SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st));
	if (ok) {
		ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)t->tex);
		ok = SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &ms));
	}
	if (ok) {
		for (y = 0; y < t->h; y++) {
			in = (const uint8_t *)ms.pData + (size_t)y * ms.RowPitch;
			o = px + (size_t)y * row;
			for (i = 0; i < row; i += 4) {
				a = in[i + 3];
				for (k = 0; k < 3; k++) {
					v = a ? (in[i + k] * 255 + a / 2) / a : 0;
					o[i + k] = (uint8_t)(v > 255 ? 255 : v);
				}
				o[i + 3] = (uint8_t)a;
			}
		}
		ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
	}
	rel(&st);
	f = ok ? wic() : NULL;
	ok = f && SUCCEEDED(IWICImagingFactory_CreateStream(f, &s)) && SUCCEEDED(IWICStream_InitializeFromFilename(s, path, GENERIC_WRITE)) && SUCCEEDED(IWICImagingFactory_CreateEncoder(f, &fmt_png, NULL, &en)) && SUCCEEDED(IWICBitmapEncoder_Initialize(en, (IStream *)s, WICBitmapEncoderNoCache)) && SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(en, &fr, NULL)) && SUCCEEDED(IWICBitmapFrameEncode_Initialize(fr, NULL)) && SUCCEEDED(IWICBitmapFrameEncode_SetSize(fr, t->w, t->h)) && SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(fr, &pf)) && IsEqualGUID(&pf, &fmt_bgra) && SUCCEEDED(IWICBitmapFrameEncode_WritePixels(fr, t->h, row, row * t->h, px)) && SUCCEEDED(IWICBitmapFrameEncode_Commit(fr)) && SUCCEEDED(IWICBitmapEncoder_Commit(en));
	rel(&fr);
	rel(&en);
	rel(&s);
	rel(&f);
	free(px);
	return ok;
}
#endif

static bool decode(IWICImagingFactory *f, IWICBitmapDecoder *d, uint8_t *out) {
	IWICBitmapFrameDecode *fr = NULL;
	IWICFormatConverter *cv = NULL;
	IWICBitmapClipper *cl = NULL;
	IWICBitmapScaler *sc = NULL;
	UINT fw = 0, fh = 0;
	bool ok = d && out && SUCCEEDED(IWICBitmapDecoder_GetFrame(d, 0, &fr)) && SUCCEEDED(IWICBitmapFrameDecode_GetSize(fr, &fw, &fh)) && fw && fh && fw <= 2048 && fh <= 2048;
	WICRect r;
	if (ok) {
		r = (WICRect){ 0, 0, (INT)fw, (INT)fh };
		if (fw > fh) {
			r.Width = (INT)fh;
			r.X = ((INT)fw - r.Width) / 2;
		} else {
			r.Height = (INT)fw;
			r.Y = ((INT)fh - r.Height) / 2;
		}
		ok = SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &cv)) && SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &fmt_pbgra, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom)) && SUCCEEDED(IWICImagingFactory_CreateBitmapClipper(f, &cl)) && SUCCEEDED(IWICBitmapClipper_Initialize(cl, (IWICBitmapSource *)cv, &r)) && SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(f, &sc)) && SUCCEEDED(IWICBitmapScaler_Initialize(sc, (IWICBitmapSource *)cl, ICON_PX, ICON_PX, r.Width < ICON_PX ? WICBitmapInterpolationModeCubic : WICBitmapInterpolationModeFant)) && SUCCEEDED(IWICBitmapScaler_CopyPixels(sc, NULL, ICON_PX * 4, ICON_PX * ICON_PX * 4, out));
	}
	rel(&sc);
	rel(&cl);
	rel(&cv);
	rel(&fr);
	return ok;
}

bool img_decode_mem(const void *data, size_t len, uint8_t *out) {
	IWICImagingFactory *f;
	IWICStream *s = NULL;
	IWICBitmapDecoder *d = NULL;
	bool ok;
	if (!data || !len || len > 0x7FFFFFFF) return false;
	f = wic();
	ok = f && SUCCEEDED(IWICImagingFactory_CreateStream(f, &s)) && SUCCEEDED(IWICStream_InitializeFromMemory(s, (BYTE *)data, (DWORD)len)) && SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(f, (IStream *)s, NULL, WICDecodeMetadataCacheOnDemand, &d)) && decode(f, d, out);
	rel(&d);
	rel(&s);
	rel(&f);
	return ok;
}

bool img_local(const WCHAR *path) {
	WCHAR root[4] = { 0, L':', L'\\', 0 };
	bool local;
	UINT dt;
	DWORD at;
	local = ((path[0] | 32) >= L'a' && (path[0] | 32) <= L'z') && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
	root[0] = path[0];
	dt = local ? GetDriveTypeW(root) : DRIVE_UNKNOWN;
	at = dt == DRIVE_FIXED || dt == DRIVE_REMOVABLE ? GetFileAttributesW(path) : INVALID_FILE_ATTRIBUTES;
	return at != INVALID_FILE_ATTRIBUTES && !(at & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS));
}

bool img_decode_file(const WCHAR *path, uint8_t *out) {
	HANDLE fh = path && *path ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_NO_RECALL | FILE_FLAG_SEQUENTIAL_SCAN, NULL) : INVALID_HANDLE_VALUE;
	LARGE_INTEGER n = { 0 };
	DWORD got = 0;
	void *p;
	bool ok;
	if (fh == INVALID_HANDLE_VALUE) return false;
	p = GetFileType(fh) == FILE_TYPE_DISK && GetFileSizeEx(fh, &n) && n.QuadPart > 0 && n.QuadPart <= 16 << 20 ? malloc((size_t)n.QuadPart) : NULL;
	ok = p && ReadFile(fh, p, (DWORD)n.QuadPart, &got, NULL) && got == (DWORD)n.QuadPart && img_decode_mem(p, got, out);
	free(p);
	CloseHandle(fh);
	return ok;
}

bool img_decode_stream(IStream *s, uint8_t *out) {
	IWICImagingFactory *f;
	IWICBitmapDecoder *d = NULL;
	bool ok;
	if (!s) return false;
	f = wic();
	ok = f && SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(f, s, NULL, WICDecodeMetadataCacheOnDemand, &d)) && decode(f, d, out);
	rel(&d);
	rel(&f);
	return ok;
}

struct gx_color col_rgba(uint32_t rgb, float a) {
	return (struct gx_color){ (rgb >> 16 & 255) / 255.0f, (rgb >> 8 & 255) / 255.0f, (rgb & 255) / 255.0f, a };
}

struct gx_color col_rgb(uint32_t rgb) {
	return col_rgba(rgb, 1);
}

struct gx_color col_mix(struct gx_color a, struct gx_color b, float t) {
	return (struct gx_color){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}

struct gx_color col_accent(void) {
	return col_rgb(g_cfg.accent);
}

struct gx_color col_accent_soft(void) {
	return col_rgba(g_cfg.accent, 0x33 / 255.0f);
}

struct gx_color col_accent_mid(void) {
	return col_rgba(g_cfg.accent, 0x80 / 255.0f);
}

static float lin(uint32_t c) {
	float v = (c & 255) / 255.0f;
	return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

float col_luma(uint32_t rgb) {
	return 0.2126f * lin(rgb >> 16) + 0.7152f * lin(rgb >> 8) + 0.0722f * lin(rgb);
}

struct gx_color col_on(uint32_t bg) {
	float l = col_luma(bg) + 0.05f;
	return col_rgb(l / (col_luma(C_INK) + 0.05f) >= (col_luma(C_TEXT) + 0.05f) / l ? C_INK : C_TEXT);
}

bool col_parse(const WCHAR *s, uint32_t *rgb) {
	uint32_t v = 0;
	int i, d;
	if (!s) return false;
	s += *s == L'#';
	for (i = 0; i < 6; i++) {
		d = hex_digit(s[i]);
		if (d < 0) return false;
		v = v << 4 | (uint32_t)d;
	}
	if (s[6]) return false;
	*rgb = v;
	return true;
}

void col_format(WCHAR out[8]) {
	swprintf(out, 8, L"#%06X", g_cfg.accent & 0xFFFFFF);
}

uint32_t col_hsv(float h, float s, float v) {
	uint32_t o = 0;
	int n;
	float k;
	h = fmodf(h, 360);
	h += h < 0 ? 360 : 0;
	s = fmaxf(0, fminf(1, s));
	v = fmaxf(0, fminf(1, v));
	for (n = 5; n >= 1; n -= 2) {
		k = fmodf(n + h / 60, 6);
		o = o << 8 | (uint32_t)lroundf((v - v * s * fmaxf(0, fminf(fminf(k, 4 - k), 1))) * 255);
	}
	return o;
}

void col_to_hsv(uint32_t rgb, float *h, float *s, float *v) {
	float r = (rgb >> 16 & 255) / 255.0f, g = (rgb >> 8 & 255) / 255.0f, b = (rgb & 255) / 255.0f, mx = fmaxf(r, fmaxf(g, b)), d = mx - fminf(r, fminf(g, b));
	*v = mx;
	*s = mx > 0 ? d / mx : 0;
	*h = 60 * (d <= 0 ? 0 : mx == r ? fmodf((g - b) / d + 6, 6) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4);
}
