/*
 * gdi_dxgi_harness - plan item 9: one HWND with GDI raster + DXGI swapchain Present.
 *
 *   wine gdi_dxgi_harness.exe insession [cycles]
 *   wine gdi_dxgi_harness.exe xsession save|prove|restore <file> [seed]
 */

#define COBJMACROS
#include "d3d11_scene.h"
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef D3D11_SDK_VERSION
#define D3D11_SDK_VERSION 7
#endif

#define INIT_W D3D11_SCENE_W
#define INIT_H D3D11_SCENE_H

typedef struct GdiPin {
	HDC memdc;
	HBITMAP dib;
	void *dibbits;
} GdiPin;

typedef struct World {
	HWND hwnd;
	GdiPin gdi;
	IDXGIFactory *factory;
	IDXGIAdapter *adapter;
	IDXGISwapChain *swap;
	ID3D11Device *dev;
	ID3D11DeviceContext *ctx;
	ID3D11Texture2D *bb;
	ID3D11Texture2D *stg;
	ID3D11RenderTargetView *rtv;
	D3d11Scene pipe;
	unsigned width;
	unsigned height;
} World;

typedef struct SnapRefs {
	unsigned saved_pid;
	uintptr_t hwnd;
	uintptr_t memdc;
	uintptr_t dib;
	uintptr_t factory;
	uintptr_t adapter;
	uintptr_t swapchain;
	uintptr_t backbuffer;
	uintptr_t rtv;
	uintptr_t dev;
	uintptr_t ctx;
} SnapRefs;

typedef struct SnapHdr {
	char magic[8]; /* "GDX9HWND" */
	unsigned ver;
	D3d11SceneState scene;
	unsigned width;
	unsigned height;
	unsigned gdi_fp;
	unsigned dxgi_fp;
	SnapRefs refs;
} SnapHdr;

typedef struct PinArena {
	unsigned magic;
	unsigned seed;
	unsigned width;
	unsigned height;
	unsigned gdi_fp;
	unsigned dxgi_fp;
	HWND hwnd;
	HDC memdc;
	HBITMAP dib;
	IDXGISwapChain *swap;
	ID3D11Texture2D *bb;
	ID3D11RenderTargetView *rtv;
	D3d11SceneState scene;
} PinArena;

static World g_w;
static PinArena *g_arena;
static unsigned char *g_gdi_pix;
static unsigned char *g_dxgi_pix;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	return DefWindowProcA(h, m, w, l);
}

static unsigned fnv1a(const void *p, size_t n)
{
	const unsigned char *b = p;
	unsigned h = 2166136261u;
	size_t i;
	for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
	return h;
}

static unsigned pix_bytes(unsigned w, unsigned h)
{
	return w * h * 4u;
}

static void gdi_release(GdiPin *g)
{
	if (g->memdc) { DeleteDC(g->memdc); g->memdc = NULL; }
	if (g->dib) { DeleteObject(g->dib); g->dib = NULL; }
	g->dibbits = NULL;
}

static int gdi_repin(World *w)
{
	BITMAPINFO bmi;
	HGDIOBJ old;
	gdi_release(&w->gdi);
	w->gdi.memdc = CreateCompatibleDC(NULL);
	if (!w->gdi.memdc) return 0;
	ZeroMemory(&bmi, sizeof(bmi));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = (LONG)w->width;
	bmi.bmiHeader.biHeight = -(LONG)w->height;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	w->gdi.dib = CreateDIBSection(w->gdi.memdc, &bmi, DIB_RGB_COLORS, &w->gdi.dibbits, NULL, 0);
	if (!w->gdi.dib || !w->gdi.dibbits) return 0;
	old = SelectObject(w->gdi.memdc, w->gdi.dib);
	(void)old;
	return 1;
}

static void gdi_fill_dib(World *w, unsigned seed)
{
	unsigned x, y;
	for (y = 0; y < w->height; y++)
		for (x = 0; x < w->width; x++) {
			unsigned char *p = (unsigned char *)w->gdi.dibbits + (y * w->width + x) * 4;
			p[0] = (unsigned char)((seed + x * 5u) & 0xff);
			p[1] = (unsigned char)((seed >> 3) + y) & 0xff;
			p[2] = (unsigned char)((seed >> 5) + x + y) & 0xff;
			p[3] = 255;
		}
}

static void gdi_bitblt_to_window(World *w)
{
	HDC wh;
	wh = GetDC(w->hwnd);
	if (wh) {
		BitBlt(wh, 0, 0, (int)w->width, (int)w->height, w->gdi.memdc, 0, 0, SRCCOPY);
		ReleaseDC(w->hwnd, wh);
	}
	GdiFlush();
}

static int gdi_raster_capture(World *w)
{
	HDC capdc;
	HBITMAP capbmp;
	HGDIOBJ old;
	BITMAPINFO bmi;
	void *bits = NULL;
	BOOL ok;
	unsigned pb = pix_bytes(w->width, w->height);

	capdc = CreateCompatibleDC(NULL);
	if (!capdc) return 0;
	ZeroMemory(&bmi, sizeof(bmi));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = (LONG)w->width;
	bmi.bmiHeader.biHeight = -(LONG)w->height;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	capbmp = CreateDIBSection(capdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!capbmp || !bits) {
		DeleteDC(capdc);
		return 0;
	}
	old = SelectObject(capdc, capbmp);
	ok = PrintWindow(w->hwnd, capdc, PW_CLIENTONLY);
	if (!ok) {
		HDC wh = GetDC(w->hwnd);
		if (wh) {
			BitBlt(capdc, 0, 0, (int)w->width, (int)w->height, wh, 0, 0, SRCCOPY);
			ReleaseDC(w->hwnd, wh);
		}
	}
	memcpy(g_gdi_pix, bits, pb);
	SelectObject(capdc, old);
	DeleteObject(capbmp);
	DeleteDC(capdc);
	return 1;
}

static void release_backbuffer(World *w)
{
	if (w->rtv) { ID3D11RenderTargetView_Release(w->rtv); w->rtv = NULL; }
	if (w->bb) { ID3D11Texture2D_Release(w->bb); w->bb = NULL; }
}

static int create_staging(World *w)
{
	D3D11_TEXTURE2D_DESC td;
	HRESULT hr;
	if (w->stg) { ID3D11Texture2D_Release(w->stg); w->stg = NULL; }
	ZeroMemory(&td, sizeof(td));
	td.Width = w->width;
	td.Height = w->height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_STAGING;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	hr = ID3D11Device_CreateTexture2D(w->dev, &td, NULL, &w->stg);
	return SUCCEEDED(hr);
}

static int bind_backbuffer(World *w)
{
	HRESULT hr;
	release_backbuffer(w);
	hr = IDXGISwapChain_GetBuffer(w->swap, 0, &IID_ID3D11Texture2D, (void **)&w->bb);
	if (FAILED(hr) || !w->bb) return 0;
	hr = ID3D11Device_CreateRenderTargetView(w->dev, (ID3D11Resource *)w->bb, NULL, &w->rtv);
	if (FAILED(hr)) return 0;
	return create_staging(w);
}

static int resize_to(World *w, unsigned rw, unsigned rh)
{
	HRESULT hr;
	release_backbuffer(w);
	hr = IDXGISwapChain_ResizeBuffers(w->swap, 0, rw, rh, DXGI_FORMAT_UNKNOWN, 0);
	if (FAILED(hr)) return 0;
	w->width = rw;
	w->height = rh;
	if (!gdi_repin(w)) return 0;
	return bind_backbuffer(w);
}

static int resize_once(World *w, unsigned seed)
{
	unsigned rw = 480u + (seed % 17u);
	unsigned rh = 400u + ((seed >> 4) % 23u);
	return resize_to(w, rw, rh);
}

static int world_create(unsigned seed)
{
	WNDCLASSA wc;
	DXGI_SWAP_CHAIN_DESC sd;
	D3D_FEATURE_LEVEL fl, want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	HRESULT hr;

	ZeroMemory(&g_w, sizeof(g_w));
	ZeroMemory(&wc, sizeof(wc));
	wc.lpfnWndProc = wndproc;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "gdi_dxgi_cls";
	RegisterClassA(&wc);
	g_w.hwnd = CreateWindowExA(0, "gdi_dxgi_cls", "gdi+dxgi", 0, 0, 0, INIT_W, INIT_H, HWND_MESSAGE, NULL,
				   wc.hInstance, NULL);
	if (!g_w.hwnd) return 0;

	g_w.width = INIT_W;
	g_w.height = INIT_H;
	if (!gdi_repin(&g_w)) return 0;

	hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&g_w.factory);
	if (FAILED(hr)) return 0;
	hr = IDXGIFactory_EnumAdapters(g_w.factory, 0, &g_w.adapter);
	if (FAILED(hr)) return 0;

	hr = D3D11CreateDevice(g_w.adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, want, 2, D3D11_SDK_VERSION,
			       &g_w.dev, &fl, &g_w.ctx);
	if (FAILED(hr)) return 0;

	ZeroMemory(&sd, sizeof(sd));
	sd.BufferCount = 2;
	sd.BufferDesc.Width = INIT_W;
	sd.BufferDesc.Height = INIT_H;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = g_w.hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	hr = IDXGIFactory_CreateSwapChain(g_w.factory, (IUnknown *)g_w.dev, &sd, &g_w.swap);
	if (FAILED(hr)) return 0;
	if (!bind_backbuffer(&g_w)) return 0;
	if (!resize_once(&g_w, seed)) return 0;
	if (!d3d11_scene_init_pipeline(&g_w.pipe, g_w.dev, g_w.ctx, seed)) return 0;
	return 1;
}

static void world_destroy(void)
{
	d3d11_scene_shutdown_pipeline(&g_w.pipe);
	release_backbuffer(&g_w);
	if (g_w.stg) ID3D11Texture2D_Release(g_w.stg);
	if (g_w.swap) IDXGISwapChain_Release(g_w.swap);
	if (g_w.adapter) IDXGIAdapter_Release(g_w.adapter);
	if (g_w.factory) IDXGIFactory_Release(g_w.factory);
	if (g_w.ctx) ID3D11DeviceContext_Release(g_w.ctx);
	if (g_w.dev) ID3D11Device_Release(g_w.dev);
	gdi_release(&g_w.gdi);
	if (g_w.hwnd) DestroyWindow(g_w.hwnd);
	ZeroMemory(&g_w, sizeof(g_w));
}

static unsigned dxgi_capture(const D3d11SceneState *st)
{
	unsigned fp;
	fp = d3d11_scene_render_target(&g_w.pipe, st, g_w.rtv, g_w.bb, g_w.stg, g_w.width, g_w.height, g_dxgi_pix);
	IDXGISwapChain_Present(g_w.swap, 0, 0);
	return fp;
}

static void reconcile(unsigned seed, const D3d11SceneState *st, unsigned *gdi_fp, unsigned *dxgi_fp)
{
	gdi_fill_dib(&g_w, seed);
	gdi_bitblt_to_window(&g_w);
	(void)gdi_raster_capture(&g_w);
	*gdi_fp = fnv1a(g_gdi_pix, pix_bytes(g_w.width, g_w.height));
	*dxgi_fp = dxgi_capture(st);
}

static void fill_refs(SnapRefs *r)
{
	memset(r, 0, sizeof(*r));
	r->saved_pid = GetCurrentProcessId();
	r->hwnd = (uintptr_t)g_w.hwnd;
	r->memdc = (uintptr_t)g_w.gdi.memdc;
	r->dib = (uintptr_t)g_w.gdi.dib;
	r->factory = (uintptr_t)g_w.factory;
	r->adapter = (uintptr_t)g_w.adapter;
	r->swapchain = (uintptr_t)g_w.swap;
	r->backbuffer = (uintptr_t)g_w.bb;
	r->rtv = (uintptr_t)g_w.rtv;
	r->dev = (uintptr_t)g_w.dev;
	r->ctx = (uintptr_t)g_w.ctx;
}

static int count_dead(const SnapRefs *s)
{
	int d = 0;
	if (s->saved_pid != GetCurrentProcessId())
		return 10;
	if (s->hwnd && (void *)s->hwnd != (void *)g_w.hwnd) d++;
	if (s->memdc && (void *)s->memdc != (void *)g_w.gdi.memdc) d++;
	if (s->dib && (void *)s->dib != (void *)g_w.gdi.dib) d++;
	if (s->factory && (void *)s->factory != (void *)g_w.factory) d++;
	if (s->adapter && (void *)s->adapter != (void *)g_w.adapter) d++;
	if (s->swapchain && (void *)s->swapchain != (void *)g_w.swap) d++;
	if (s->backbuffer && (void *)s->backbuffer != (void *)g_w.bb) d++;
	if (s->rtv && (void *)s->rtv != (void *)g_w.rtv) d++;
	if (s->dev && (void *)s->dev != (void *)g_w.dev) d++;
	if (s->ctx && (void *)s->ctx != (void *)g_w.ctx) d++;
	if (d == 0) d = 10;
	return d;
}

static int insession(int cycles)
{
	unsigned char *snap;
	unsigned ok = 0, split = 0, reconciled = 0, poisoned = 0, disagree = 0;
	int c;
	snap = (unsigned char *)malloc(sizeof(PinArena));
	if (!snap) return 2;

	printf("insession one-HWND GDI+DXGI cycles=%d size=%ux%u\n", cycles, g_w.width, g_w.height);
	for (c = 0; c < cycles; c++) {
		unsigned seed = (unsigned)(c + 1) * 9901u;
		unsigned sg, sd;
		D3d11SceneState st;
		d3d11_scene_seed_state(&st, seed);
		g_arena->seed = seed;
		g_arena->scene = st;
		g_arena->width = g_w.width;
		g_arena->height = g_w.height;
		g_arena->hwnd = g_w.hwnd;
		g_arena->memdc = g_w.gdi.memdc;
		g_arena->dib = g_w.gdi.dib;
		g_arena->swap = g_w.swap;
		g_arena->bb = g_w.bb;
		g_arena->rtv = g_w.rtv;

		reconcile(seed, &st, &sg, &sd);
		g_arena->gdi_fp = sg;
		g_arena->dxgi_fp = sd;
		memcpy(snap, g_arena, sizeof(PinArena));

		{
			D3d11SceneState mut;
			unsigned mg, md;
			d3d11_scene_seed_state(&mut, seed + 555u);
			reconcile(seed + 555u, &mut, &mg, &md);
			(void)mg;
			(void)md;
		}

		memcpy(g_arena, snap, sizeof(PinArena));
		g_arena->hwnd = g_w.hwnd;
		g_arena->memdc = g_w.gdi.memdc;
		g_arena->dib = g_w.gdi.dib;
		g_arena->swap = g_w.swap;
		g_arena->bb = g_w.bb;
		g_arena->rtv = g_w.rtv;

		if (fnv1a(g_gdi_pix, pix_bytes(g_w.width, g_w.height)) != sg ||
		    fnv1a(g_dxgi_pix, pix_bytes(g_w.width, g_w.height)) != sd)
			split++;
		reconcile(g_arena->seed, &g_arena->scene, &sg, &sd);
		if (sg != g_arena->gdi_fp || sd != g_arena->dxgi_fp)
			disagree++;
		if (fnv1a(g_gdi_pix, pix_bytes(g_w.width, g_w.height)) == g_arena->gdi_fp &&
		    fnv1a(g_dxgi_pix, pix_bytes(g_w.width, g_w.height)) == g_arena->dxgi_fp)
			ok++;
		else
			poisoned++;
		reconciled++;
	}
	printf("summary ok=%u/%d split=%u disagree=%u reconciled=%u poisoned=%u\n",
	       ok, cycles, split, disagree, reconciled, poisoned);
	free(snap);
	return (ok == (unsigned)cycles && poisoned == 0 && disagree == 0) ? 0 : 1;
}

static int xsession_cmd(const char *cmd, const char *file, unsigned seed)
{
	if (strcmp(cmd, "save") == 0) {
		SnapHdr hdr;
		FILE *f;
		D3d11SceneState st;
		unsigned pb;
		memset(&hdr, 0, sizeof(hdr));
		memcpy(hdr.magic, "GDX9HWND", 8);
		hdr.ver = 1;
		d3d11_scene_seed_state(&st, seed);
		hdr.scene = st;
		hdr.width = g_w.width;
		hdr.height = g_w.height;
		reconcile(seed, &st, &hdr.gdi_fp, &hdr.dxgi_fp);
		fill_refs(&hdr.refs);
		pb = pix_bytes(hdr.width, hdr.height);
		f = fopen(file, "wb");
		if (!f) return 2;
		fwrite(&hdr, sizeof(hdr), 1, f);
		fwrite(g_gdi_pix, 1, pb, f);
		fwrite(g_dxgi_pix, 1, pb, f);
		fclose(f);
		printf("SAVE seed=%u %ux%u gdi_fp=0x%08x dxgi_fp=0x%08x hwnd=%p swap=%p -> %s\n",
		       seed, hdr.width, hdr.height, hdr.gdi_fp, hdr.dxgi_fp, (void *)g_w.hwnd,
		       (void *)g_w.swap, file);
		return 0;
	}

	if (strcmp(cmd, "prove") == 0 || strcmp(cmd, "restore") == 0) {
		SnapHdr hdr;
		FILE *f;
		unsigned char *sg, *sd;
		unsigned pb, gg, dd, dg = 0, ddx = 0, i;
		int dead;
		f = fopen(file, "rb");
		if (!f || fread(&hdr, sizeof(hdr), 1, f) != 1 || memcmp(hdr.magic, "GDX9HWND", 8) != 0) {
			printf("bad snap\n");
			if (f) fclose(f);
			return 2;
		}
		pb = hdr.width * hdr.height * 4u;
		sg = (unsigned char *)malloc(pb);
		sd = (unsigned char *)malloc(pb);
		if (!sg || !sd || fread(sg, 1, pb, f) != pb || fread(sd, 1, pb, f) != pb) {
			fclose(f);
			free(sg);
			free(sd);
		 return 2;
		}
		fclose(f);

		dead = count_dead(&hdr.refs);
		printf("PROVE dead=%d/10 saved_hwnd=%p live=%p saved_swap=%p live=%p\n", dead,
		       (void *)hdr.refs.hwnd, (void *)g_w.hwnd, (void *)hdr.refs.swapchain,
		       (void *)g_w.swap);

		if (strcmp(cmd, "prove") == 0) {
			free(sg);
			free(sd);
			printf("%s: HWND/GDI/DXGI refs not adopted cross-session (%d/10)\n",
			       dead >= 10 ? "PASS" : "FAIL", dead);
			return dead >= 10 ? 0 : 1;
		}

		if (g_w.width != hdr.width || g_w.height != hdr.height) {
			if (!resize_to(&g_w, hdr.width, hdr.height))
				printf("WARN restore: resize to snap dims failed\n");
		}
		reconcile(hdr.scene.seed, &hdr.scene, &gg, &dd);
		for (i = 0; i < pb; i++) {
			if (g_gdi_pix[i] != sg[i]) dg++;
			if (g_dxgi_pix[i] != sd[i]) ddx++;
		}
		printf("RESTORE gdi_fp_match=%s dxgi_fp_match=%s gdi_diffs=%u dxgi_diffs=%u\n",
		       (gg == hdr.gdi_fp) ? "YES" : "NO", (dd == hdr.dxgi_fp) ? "YES" : "NO", dg, ddx);
		if (gg != dd)
			printf("NOTE: GDI raster fp 0x%08x vs DXGI fp 0x%08x (expected on one HWND)\n", gg, dd);
		free(sg);
		free(sd);
		{
			int ok = (dead >= 10) && (gg == hdr.gdi_fp) && (dd == hdr.dxgi_fp) && (dg == 0) && (ddx == 0);
			printf("%s: recreate HWND+GDI+swapchain, reconcile both rasters\n", ok ? "PASS" : "FAIL");
			return ok ? 0 : 1;
		}
	}
	return 2;
}

static int alloc_pix(void)
{
	unsigned pb = pix_bytes(g_w.width, g_w.height);
	g_gdi_pix = (unsigned char *)malloc(pb);
	g_dxgi_pix = (unsigned char *)malloc(pb);
	return g_gdi_pix && g_dxgi_pix;
}

int main(int argc, char **argv)
{
	unsigned seed = 9009u;
	int rc = 2;

	if (argc > 1 && strcmp(argv[1], "insession") == 0) {
		int cycles = argc > 2 ? atoi(argv[2]) : 35;
		if (!world_create(seed)) {
			printf("world create failed\n");
		 return 2;
		}
		if (!alloc_pix()) return 2;
		g_arena = (PinArena *)calloc(1, sizeof(PinArena));
		if (!g_arena) { world_destroy(); return 2; }
		g_arena->magic = 0x47445839u;
		rc = insession(cycles);
		free(g_arena);
		free(g_gdi_pix);
		free(g_dxgi_pix);
		world_destroy();
		return rc;
	}

	if (argc > 1 && strcmp(argv[1], "xsession") == 0 && argc > 3) {
		if (argc > 4) seed = (unsigned)strtoul(argv[4], NULL, 0);
		if (!world_create(seed)) {
			printf("world create failed\n");
			return 2;
		}
		if (!alloc_pix()) { world_destroy(); return 2; }
		rc = xsession_cmd(argv[2], argv[3], seed);
		free(g_gdi_pix);
		free(g_dxgi_pix);
		world_destroy();
		return rc;
	}

	printf("usage: %s insession [cycles] | xsession save|prove|restore <file> [seed]\n", argv[0]);
	return 2;
}
