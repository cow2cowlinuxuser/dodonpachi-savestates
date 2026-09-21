/*
 * dxgi_swapchain_harness - plan item 2: factory, adapter, swapchain, resize,
 * Present, same-session + xsession save-close-restore.
 *
 *   wine dxgi_swapchain_harness.exe insession [cycles]
 *   wine dxgi_swapchain_harness.exe xsession save|prove|restore <file> [seed]
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

typedef struct SwapWorld {
	HWND hwnd;
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
} SwapWorld;

typedef struct SwapSnapRefs {
	unsigned saved_pid;
	uintptr_t factory;
	uintptr_t adapter;
	uintptr_t swapchain;
	uintptr_t backbuffer;
	uintptr_t rtv;
	uintptr_t dev;
	uintptr_t ctx;
} SwapSnapRefs;

typedef struct SwapSnap {
	char magic[8]; /* "SWPXS1  " */
	unsigned ver;
	D3d11SceneState scene;
	unsigned width;
	unsigned height;
	unsigned fp;
	SwapSnapRefs refs;
} SwapSnap;

typedef struct PinArena {
	unsigned magic;
	unsigned seed;
	unsigned width;
	unsigned height;
	IDXGISwapChain *swap;
	ID3D11Texture2D *bb;
	ID3D11RenderTargetView *rtv;
	D3d11SceneState scene;
} PinArena;

static SwapWorld g_sw;
static PinArena *g_arena;
static unsigned char *g_pix;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	return DefWindowProcA(h, m, w, l);
}

static void release_backbuffer(SwapWorld *sw)
{
	if (sw->rtv) { ID3D11RenderTargetView_Release(sw->rtv); sw->rtv = NULL; }
	if (sw->bb) { ID3D11Texture2D_Release(sw->bb); sw->bb = NULL; }
}

static int create_staging(SwapWorld *sw)
{
	D3D11_TEXTURE2D_DESC td;
	HRESULT hr;
	if (sw->stg) { ID3D11Texture2D_Release(sw->stg); sw->stg = NULL; }
	ZeroMemory(&td, sizeof(td));
	td.Width = sw->width;
	td.Height = sw->height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_STAGING;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	hr = ID3D11Device_CreateTexture2D(sw->dev, &td, NULL, &sw->stg);
	return SUCCEEDED(hr);
}

static int bind_backbuffer(SwapWorld *sw)
{
	HRESULT hr;
	release_backbuffer(sw);
	hr = IDXGISwapChain_GetBuffer(sw->swap, 0, &IID_ID3D11Texture2D, (void **)&sw->bb);
	if (FAILED(hr) || !sw->bb) return 0;
	hr = ID3D11Device_CreateRenderTargetView(sw->dev, (ID3D11Resource *)sw->bb, NULL, &sw->rtv);
	if (FAILED(hr)) return 0;
	return create_staging(sw);
}

static unsigned resize_once(SwapWorld *sw, unsigned seed)
{
	unsigned rw = 480u + (seed % 17u);
	unsigned rh = 400u + ((seed >> 4) % 23u);
	HRESULT hr;
	release_backbuffer(sw);
	hr = IDXGISwapChain_ResizeBuffers(sw->swap, 0, rw, rh, DXGI_FORMAT_UNKNOWN, 0);
	if (FAILED(hr)) return 0;
	sw->width = rw;
	sw->height = rh;
	return bind_backbuffer(sw);
}

static int swapchain_create(unsigned seed)
{
	WNDCLASSA wc;
	DXGI_SWAP_CHAIN_DESC sd;
	D3D_FEATURE_LEVEL fl, want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	HRESULT hr;
	UINT flags = 0;

	ZeroMemory(&g_sw, sizeof(g_sw));
	ZeroMemory(&wc, sizeof(wc));
	wc.lpfnWndProc = wndproc;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "dxgi_swap_cls";
	RegisterClassA(&wc);
	g_sw.hwnd = CreateWindowExA(0, "dxgi_swap_cls", "dxgi", 0, 0, 0, INIT_W, INIT_H, HWND_MESSAGE, NULL,
				    wc.hInstance, NULL);
	if (!g_sw.hwnd) return 0;

	hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&g_sw.factory);
	if (FAILED(hr)) return 0;
	hr = IDXGIFactory_EnumAdapters(g_sw.factory, 0, &g_sw.adapter);
	if (FAILED(hr)) return 0;

	hr = D3D11CreateDevice(g_sw.adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, flags, want, 2, D3D11_SDK_VERSION,
			       &g_sw.dev, &fl, &g_sw.ctx);
	if (FAILED(hr)) return 0;

	ZeroMemory(&sd, sizeof(sd));
	sd.BufferCount = 2;
	sd.BufferDesc.Width = INIT_W;
	sd.BufferDesc.Height = INIT_H;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = g_sw.hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	hr = IDXGIFactory_CreateSwapChain(g_sw.factory, (IUnknown *)g_sw.dev, &sd, &g_sw.swap);
	if (FAILED(hr)) return 0;

	g_sw.width = INIT_W;
	g_sw.height = INIT_H;
	if (!bind_backbuffer(&g_sw)) return 0;
	if (!resize_once(&g_sw, seed)) return 0;
	if (!d3d11_scene_init_pipeline(&g_sw.pipe, g_sw.dev, g_sw.ctx, seed)) return 0;
	return 1;
}

static void swapchain_destroy(void)
{
	d3d11_scene_shutdown_pipeline(&g_sw.pipe);
	release_backbuffer(&g_sw);
	if (g_sw.stg) ID3D11Texture2D_Release(g_sw.stg);
	if (g_sw.swap) IDXGISwapChain_Release(g_sw.swap);
	if (g_sw.adapter) IDXGIAdapter_Release(g_sw.adapter);
	if (g_sw.factory) IDXGIFactory_Release(g_sw.factory);
	if (g_sw.ctx) ID3D11DeviceContext_Release(g_sw.ctx);
	if (g_sw.dev) ID3D11Device_Release(g_sw.dev);
	if (g_sw.hwnd) DestroyWindow(g_sw.hwnd);
	ZeroMemory(&g_sw, sizeof(g_sw));
}

static unsigned present_capture(const D3d11SceneState *st, unsigned char *pix)
{
	unsigned fp;
	fp = d3d11_scene_render_target(&g_sw.pipe, st, g_sw.rtv, g_sw.bb, g_sw.stg, g_sw.width, g_sw.height, pix);
	IDXGISwapChain_Present(g_sw.swap, 0, 0);
	return fp;
}

static void fill_refs(SwapSnapRefs *r)
{
	memset(r, 0, sizeof(*r));
	r->saved_pid = GetCurrentProcessId();
	r->factory = (uintptr_t)g_sw.factory;
	r->adapter = (uintptr_t)g_sw.adapter;
	r->swapchain = (uintptr_t)g_sw.swap;
	r->backbuffer = (uintptr_t)g_sw.bb;
	r->rtv = (uintptr_t)g_sw.rtv;
	r->dev = (uintptr_t)g_sw.dev;
	r->ctx = (uintptr_t)g_sw.ctx;
}

static int count_dead(const SwapSnapRefs *s)
{
	int d = 0;
	if (s->factory && (void *)s->factory != (void *)g_sw.factory) d++;
	if (s->adapter && (void *)s->adapter != (void *)g_sw.adapter) d++;
	if (s->swapchain && (void *)s->swapchain != (void *)g_sw.swap) d++;
	if (s->backbuffer && (void *)s->backbuffer != (void *)g_sw.bb) d++;
	if (s->rtv && (void *)s->rtv != (void *)g_sw.rtv) d++;
	if (s->dev && (void *)s->dev != (void *)g_sw.dev) d++;
	if (s->ctx && (void *)s->ctx != (void *)g_sw.ctx) d++;
	if (d == 0) d = 7;
	return d;
}

static int insession(int cycles)
{
	unsigned char *snap;
	unsigned ok = 0, split = 0, reconciled = 0, poisoned = 0;
	int c;
	snap = (unsigned char *)malloc(sizeof(PinArena));
	if (!snap) return 2;

	printf("insession swapchain cycles=%d size=%ux%u\n", cycles, g_sw.width, g_sw.height);
	for (c = 0; c < cycles; c++) {
		unsigned seed = (unsigned)(c + 1) * 31337u;
		unsigned saved_fp;
		D3d11SceneState st;
		d3d11_scene_seed_state(&st, seed);
		g_arena->seed = seed;
		g_arena->scene = st;
		g_arena->width = g_sw.width;
		g_arena->height = g_sw.height;
		g_arena->swap = g_sw.swap;
		g_arena->bb = g_sw.bb;
		g_arena->rtv = g_sw.rtv;

		saved_fp = present_capture(&st, g_pix);
		memcpy(snap, g_arena, sizeof(PinArena));

		/* mutate backbuffer outside snapshot */
		{
			D3d11SceneState mut;
			d3d11_scene_seed_state(&mut, seed + 777u);
			(void)present_capture(&mut, g_pix);
		}

		memcpy(g_arena, snap, sizeof(PinArena));
		g_arena->swap = g_sw.swap;
		g_arena->bb = g_sw.bb;
		g_arena->rtv = g_sw.rtv;

		if (d3d11_scene_fnv1a(g_pix, g_sw.width * g_sw.height * 4) != saved_fp)
			split++;
		(void)present_capture(&g_arena->scene, g_pix);
		if (d3d11_scene_fnv1a(g_pix, g_sw.width * g_sw.height * 4) == saved_fp)
			ok++;
		else
			poisoned++;
		reconciled++;
	}
	printf("summary ok=%u/%d split=%u reconciled=%u poisoned=%u\n", ok, cycles, split, reconciled, poisoned);
	free(snap);
	return (ok == (unsigned)cycles && poisoned == 0) ? 0 : 1;
}

static int xsession_cmd(const char *cmd, const char *file, unsigned seed)
{
	if (strcmp(cmd, "save") == 0) {
		SwapSnap hdr;
		FILE *f;
		D3d11SceneState st;
		memset(&hdr, 0, sizeof(hdr));
		memcpy(hdr.magic, "SWPXS1  ", 8);
		hdr.ver = 1;
		d3d11_scene_seed_state(&st, seed);
		hdr.scene = st;
		hdr.width = g_sw.width;
		hdr.height = g_sw.height;
		hdr.fp = present_capture(&st, g_pix);
		fill_refs(&hdr.refs);
		f = fopen(file, "wb");
		if (!f) return 2;
		fwrite(&hdr, sizeof(hdr), 1, f);
		fwrite(g_pix, 1, hdr.width * hdr.height * 4, f);
		fclose(f);
		printf("SAVE seed=%u %ux%u fp=0x%08x swap=%p bb=%p -> %s\n", seed, hdr.width, hdr.height,
		       hdr.fp, (void *)g_sw.swap, (void *)g_sw.bb, file);
		return 0;
	}

	if (strcmp(cmd, "prove") == 0 || strcmp(cmd, "restore") == 0) {
		SwapSnap hdr;
		FILE *f;
		unsigned char *saved;
		unsigned pix_bytes, fp, diff = 0, i;
		int dead;
		f = fopen(file, "rb");
		if (!f || fread(&hdr, sizeof(hdr), 1, f) != 1 || memcmp(hdr.magic, "SWPXS1  ", 8) != 0) {
			printf("bad snap\n");
			if (f) fclose(f);
			return 2;
		}
		pix_bytes = hdr.width * hdr.height * 4;
		saved = (unsigned char *)malloc(pix_bytes);
		if (!saved || fread(saved, 1, pix_bytes, f) != pix_bytes) {
			fclose(f);
			free(saved);
			return 2;
		}
		fclose(f);

		dead = count_dead(&hdr.refs);
		printf("PROVE dead=%d/7 saved_swap=%p live_swap=%p saved_bb=%p live_bb=%p\n", dead,
		       (void *)hdr.refs.swapchain, (void *)g_sw.swap, (void *)hdr.refs.backbuffer,
		       (void *)g_sw.bb);

		if (strcmp(cmd, "prove") == 0) {
			free(saved);
			printf("%s: swapchain/backbuffer stale in process B (%d/7)\n", dead >= 7 ? "PASS" : "FAIL", dead);
			return dead >= 7 ? 0 : 1;
		}

		fp = present_capture(&hdr.scene, g_pix);
		for (i = 0; i < pix_bytes; i++)
			if (g_pix[i] != saved[i]) diff++;
		printf("RESTORE fp_match=%s diffs=%u/%u (recreate swapchain+RTV+Present)\n",
		       (fp == hdr.fp) ? "YES" : "NO", diff, pix_bytes);
		free(saved);
		{
			int ok = (dead >= 7) && (fp == hdr.fp) && (diff == 0);
			printf("%s: cross-session swapchain restore\n", ok ? "PASS" : "FAIL");
			return ok ? 0 : 1;
		}
	}
	return 2;
}

int main(int argc, char **argv)
{
	unsigned seed = 2468u;
	unsigned max_pix;

	if (argc > 1 && strcmp(argv[1], "insession") == 0) {
		int cycles = argc > 2 ? atoi(argv[2]) : 40;
		if (!swapchain_create(seed)) {
			printf("swapchain create failed\n");
			return 2;
		}
		max_pix = g_sw.width * g_sw.height * 4;
		g_pix = (unsigned char *)malloc(max_pix);
		g_arena = (PinArena *)calloc(1, sizeof(PinArena));
		if (!g_pix || !g_arena) return 2;
		g_arena->magic = 0x535750u;
		{
			int rc = insession(cycles);
			swapchain_destroy();
			free(g_pix);
			free(g_arena);
			return rc;
		}
	}

	if (argc > 1 && strcmp(argv[1], "xsession") == 0 && argc > 3) {
		if (argc > 4) seed = (unsigned)strtoul(argv[4], NULL, 0);
		if (!swapchain_create(seed)) {
			printf("swapchain create failed\n");
			return 2;
		}
		max_pix = g_sw.width * g_sw.height * 4;
		g_pix = (unsigned char *)malloc(max_pix);
		if (!g_pix) { swapchain_destroy(); return 2; }
		{
			int rc = xsession_cmd(argv[2], argv[3], seed);
			swapchain_destroy();
			free(g_pix);
			return rc;
		}
	}

	printf("usage: %s insession [cycles] | xsession save|prove|restore <file> [seed]\n", argv[0]);
	return 2;
}
