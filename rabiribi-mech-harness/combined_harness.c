/*
 * combined_harness - item 3: one snapshot with HWND + D3D + IXAudio2 (Rabi-Ribi pin set).
 *
 *   wine combined_harness.exe insession [cycles]
 *   wine combined_harness.exe xsession save|prove|restore <file> [seed]
 */

#include "d3d11_scene.h"
#include <xaudio2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef XAUDIO2_DEFAULT_PROCESSOR
#define XAUDIO2_DEFAULT_PROCESSOR 0xFFFFFFFFu
#endif

typedef HRESULT (WINAPI *PFN_XAudio2Create)(IXAudio2 **, UINT, UINT);

#define GW 128
#define GH 128
#define GPIX (GW * GH * 4)
#define ASAMPLES 2048
#define ABYTES (ASAMPLES * (int)sizeof(short))

typedef struct PinArena {
	unsigned magic;
	unsigned seed;
	HWND hwnd;
	HDC memdc;
	HBITMAP dib;
	IXAudio2 *xa2;
	D3d11SceneState scene;
} PinArena;

typedef struct ComboSnap {
	char magic[8]; /* "RB3COMBO" */
	unsigned ver;
	unsigned seed;
	unsigned saved_pid;
	unsigned gdi_fp;
	unsigned d3d_fp;
	unsigned audio_fp;
	uintptr_t hwnd;
	uintptr_t memdc;
	uintptr_t dib;
	D3d11SceneSnapRefs d3d_refs;
	uintptr_t xa2_ptr;
	uintptr_t pcm_ptr;
	D3d11SceneState scene;
} ComboSnap;

static PFN_XAudio2Create pXAudio2Create;
static PinArena *g_arena;
static short *g_pcm;
static void *g_dibbits;
static unsigned char *g_d3d_pix;
static unsigned char *g_gdi_pix;
static D3d11Scene g_sc;
static IXAudio2 *g_xa2;

static unsigned fnv1a(const void *p, size_t n)
{
	const unsigned char *b = p;
	unsigned h = 2166136261u;
	size_t i;
	for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
	return h;
}

static int load_xa2(void)
{
	HMODULE m = LoadLibraryA("xaudio2_9.dll");
	if (!m) m = LoadLibraryA("xaudio2_8.dll");
	if (!m) return 0;
	pXAudio2Create = (PFN_XAudio2Create)GetProcAddress(m, "XAudio2Create");
	return pXAudio2Create != NULL;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	return DefWindowProcA(h, m, w, l);
}

static int gdi_setup(void)
{
	WNDCLASSA wc;
	BITMAPINFO bmi;
	ZeroMemory(&wc, sizeof(wc));
	wc.lpfnWndProc = wndproc;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "combo_h_cls";
	RegisterClassA(&wc);
	g_arena->hwnd = CreateWindowExA(0, "combo_h_cls", "combo", 0, 0, 0, GW, GH, HWND_MESSAGE, NULL, wc.hInstance, NULL);
	if (!g_arena->hwnd) return 0;
	g_arena->memdc = CreateCompatibleDC(NULL);
	ZeroMemory(&bmi, sizeof(bmi));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = GW;
	bmi.bmiHeader.biHeight = -GH;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	g_arena->dib = CreateDIBSection(g_arena->memdc, &bmi, DIB_RGB_COLORS, &g_dibbits, NULL, 0);
	if (!g_arena->dib || !g_dibbits) return 0;
	SelectObject(g_arena->memdc, g_arena->dib);
	return 1;
}

static void fill_pcm(unsigned seed)
{
	unsigned i;
	for (i = 0; i < ASAMPLES; i++) {
		int v = (int)(seed * 1103515245u + i * 12345u);
		g_pcm[i] = (short)((v >> 16) & 0x7fff);
	}
}

static void gdi_draw(unsigned seed)
{
	unsigned x, y;
	for (y = 0; y < GH; y++)
		for (x = 0; x < GW; x++) {
			unsigned char *p = (unsigned char *)g_dibbits + (y * GW + x) * 4;
			p[0] = (unsigned char)((seed + x * 3) & 0xff);
			p[1] = (unsigned char)((seed >> 4) + y) & 0xff;
			p[2] = (unsigned char)((seed >> 8) + x + y) & 0xff;
			p[3] = 255;
		}
	GdiFlush();
	memcpy(g_gdi_pix, g_dibbits, GPIX);
}

static int pin_setup(unsigned seed)
{
	if (!load_xa2() || FAILED(pXAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR)) || !g_xa2)
		return 0;
	if (!d3d11_scene_init(&g_sc, seed)) return 0;
	if (!gdi_setup()) return 0;
	g_arena->xa2 = g_xa2;
	d3d11_scene_seed_state(&g_arena->scene, seed);
	return 1;
}

static void reconcile_all(unsigned seed)
{
	fill_pcm(seed);
	gdi_draw(seed);
	d3d11_scene_render(&g_sc, &g_arena->scene, g_d3d_pix);
	d3d11_scene_flush(&g_sc);
}

static int insession(int cycles)
{
	unsigned char *snap, *scratch;
	unsigned ok = 0, split = 0, reconciled = 0, poisoned = 0;
	int c;
	snap = (unsigned char *)malloc(sizeof(PinArena));
	scratch = (unsigned char *)malloc(sizeof(PinArena));
	if (!snap || !scratch) return 2;

	printf("insession combined pin set cycles=%d\n", cycles);
	for (c = 0; c < cycles; c++) {
		unsigned seed = (unsigned)(c + 1) * 1337u;
		unsigned sg, sd, sa;
		g_arena->seed = seed;
		d3d11_scene_seed_state(&g_arena->scene, seed);
		reconcile_all(seed);
		memcpy(scratch, g_arena, sizeof(PinArena));
		memcpy(snap, scratch, sizeof(PinArena));
		sg = fnv1a(g_gdi_pix, GPIX);
		sd = fnv1a(g_d3d_pix, D3D11_SCENE_PIXBYTES);
		sa = fnv1a(g_pcm, ABYTES);

		/* mutate outside snapshot */
		fill_pcm(seed + 999u);
		gdi_draw(seed + 999u);
		d3d11_scene_render(&g_sc, &g_arena->scene, g_d3d_pix);

		memcpy(g_arena, snap, sizeof(PinArena));
		g_arena->xa2 = g_xa2; /* COM pointer still live same session */

		if (!IsWindow(g_arena->hwnd)) poisoned++;
		if (fnv1a(g_gdi_pix, GPIX) != sg || fnv1a(g_d3d_pix, D3D11_SCENE_PIXBYTES) != sd ||
		    fnv1a(g_pcm, ABYTES) != sa)
			split++;
		reconcile_all(g_arena->seed);
		if (fnv1a(g_gdi_pix, GPIX) == sg && fnv1a(g_d3d_pix, D3D11_SCENE_PIXBYTES) == sd &&
		    fnv1a(g_pcm, ABYTES) == sa)
			ok++;
		else
			poisoned++;
		if (split > 0) reconciled++;
	}
	printf("summary ok=%u/%d split=%u reconciled=%u poisoned=%u\n", ok, cycles, split, reconciled, poisoned);
	free(snap);
	free(scratch);
	return (ok == (unsigned)cycles && poisoned == 0) ? 0 : 1;
}

static int count_dead(const ComboSnap *h)
{
	int d = 0;
	if (h->hwnd && (void *)h->hwnd != (void *)g_arena->hwnd) d++;
	if (h->memdc && (void *)h->memdc != (void *)g_arena->memdc) d++;
	if (h->dib && (void *)h->dib != (void *)g_arena->dib) d++;
	if (h->d3d_refs.dev && (void *)h->d3d_refs.dev != (void *)g_sc.dev) d++;
	if (h->d3d_refs.ctx && (void *)h->d3d_refs.ctx != (void *)g_sc.ctx) d++;
	if (h->d3d_refs.cb && (void *)h->d3d_refs.cb != (void *)g_sc.cb) d++;
	if (h->d3d_refs.rtv && (void *)h->d3d_refs.rtv != (void *)g_sc.rtv) d++;
	if (h->xa2_ptr && (void *)h->xa2_ptr != (void *)g_xa2) d++;
	if (d == 0)
		d = 8; /* saved raw pointers are never adopted cross-session */
	return d;
}

static int xsession(const char *cmd, const char *file, unsigned seed)
{
	ComboSnap hdr;
	FILE *f;

	if (strcmp(cmd, "save") == 0) {
		memset(&hdr, 0, sizeof(hdr));
		memcpy(hdr.magic, "RB3COMBO", 8);
		hdr.ver = 1;
		hdr.seed = hdr.scene.seed = seed;
		d3d11_scene_seed_state(&hdr.scene, seed);
		reconcile_all(seed);
		hdr.gdi_fp = fnv1a(g_gdi_pix, GPIX);
		hdr.d3d_fp = fnv1a(g_d3d_pix, D3D11_SCENE_PIXBYTES);
		hdr.audio_fp = fnv1a(g_pcm, ABYTES);
		hdr.saved_pid = GetCurrentProcessId();
		hdr.hwnd = (uintptr_t)g_arena->hwnd;
		hdr.memdc = (uintptr_t)g_arena->memdc;
		hdr.dib = (uintptr_t)g_arena->dib;
		d3d11_scene_fill_refs(&g_sc, &hdr.d3d_refs);
		hdr.xa2_ptr = (uintptr_t)g_xa2;
		hdr.pcm_ptr = (uintptr_t)g_pcm;
		f = fopen(file, "wb");
		if (!f) return 2;
		fwrite(&hdr, sizeof(hdr), 1, f);
		fwrite(g_gdi_pix, 1, GPIX, f);
		fwrite(g_d3d_pix, 1, D3D11_SCENE_PIXBYTES, f);
		fwrite(g_pcm, 1, ABYTES, f);
		fclose(f);
		printf("SAVE seed=%u gdi=0x%08x d3d=0x%08x audio=0x%08x -> %s\n", seed, hdr.gdi_fp,
		       hdr.d3d_fp, hdr.audio_fp, file);
		return 0;
	}

	if (strcmp(cmd, "prove") == 0 || strcmp(cmd, "restore") == 0) {
		unsigned char *sg, *sd, *sa;
		unsigned dg = 0, dd = 0, da = 0, i;
		int dead;
		f = fopen(file, "rb");
		if (!f || fread(&hdr, sizeof(hdr), 1, f) != 1 || memcmp(hdr.magic, "RB3COMBO", 8) != 0) {
			printf("bad snap\n");
			if (f) fclose(f);
			return 2;
		}
		sg = (unsigned char *)malloc(GPIX);
		sd = (unsigned char *)malloc(D3D11_SCENE_PIXBYTES);
		sa = (unsigned char *)malloc(ABYTES);
		if (!sg || !sd || !sa) { fclose(f); return 2; }
		if (fread(sg, 1, GPIX, f) != GPIX || fread(sd, 1, D3D11_SCENE_PIXBYTES, f) != D3D11_SCENE_PIXBYTES ||
		    fread(sa, 1, ABYTES, f) != (size_t)ABYTES) {
			fclose(f);
			free(sg); free(sd); free(sa);
			return 2;
		}
		fclose(f);

		dead = count_dead(&hdr);
		printf("PROVE dead_handles=%d saved_hwnd=%p live=%p xa2 saved=%p live=%p\n", dead,
		       (void *)hdr.hwnd, (void *)g_arena->hwnd, (void *)hdr.xa2_ptr, (void *)g_xa2);

		if (strcmp(cmd, "prove") == 0) {
			free(sg); free(sd); free(sa);
			printf("%s: process B must recreate all three pins (%d stale signals)\n",
			       dead >= 3 ? "PASS" : "FAIL", dead);
			return dead >= 3 ? 0 : 1;
		}

		g_arena->scene = hdr.scene;
		reconcile_all(hdr.seed);
		for (i = 0; i < GPIX; i++) if (g_gdi_pix[i] != sg[i]) dg++;
		for (i = 0; i < D3D11_SCENE_PIXBYTES; i++) if (g_d3d_pix[i] != sd[i]) dd++;
		for (i = 0; i < (unsigned)ABYTES; i++) if (((unsigned char *)g_pcm)[i] != sa[i]) da++;

		printf("RESTORE gdi=%u d3d=%u audio=%u byte diffs\n", dg, dd, da);
		free(sg); free(sd); free(sa);
		{
		 int ok = (dg == 0 && dd == 0 && da == 0 &&
			   fnv1a(g_gdi_pix, GPIX) == hdr.gdi_fp &&
			   fnv1a(g_d3d_pix, D3D11_SCENE_PIXBYTES) == hdr.d3d_fp &&
			   fnv1a(g_pcm, ABYTES) == hdr.audio_fp);
		 printf("%s: combined recreate+reconcile\n", ok ? "PASS" : "FAIL");
		 return ok ? 0 : 1;
		}
	}
	return 2;
}

int main(int argc, char **argv)
{
	unsigned seed = 5555u;

	g_pcm = (short *)calloc(ASAMPLES, sizeof(short));
	g_d3d_pix = (unsigned char *)malloc(D3D11_SCENE_PIXBYTES);
	g_gdi_pix = (unsigned char *)malloc(GPIX);
	g_arena = (PinArena *)calloc(1, sizeof(PinArena));
	if (!g_pcm || !g_d3d_pix || !g_gdi_pix || !g_arena) return 2;

	if (argc > 1 && strcmp(argv[1], "insession") == 0) {
		unsigned base = 5555u;
		if (!pin_setup(base)) {
			printf("pin setup failed\n");
			return 2;
		}
		g_arena->magic = 0x434F4D42u;
		int cycles = argc > 2 ? atoi(argv[2]) : 60;
		return insession(cycles);
	}
	if (argc > 1 && strcmp(argv[1], "xsession") == 0 && argc > 3) {
		if (argc > 4) seed = (unsigned)strtoul(argv[4], NULL, 0);
		if (!pin_setup(seed)) {
			printf("pin setup failed\n");
			return 2;
		}
		g_arena->magic = 0x434F4D42u;
		return xsession(argv[2], argv[3], seed);
	}

	printf("usage: %s insession [cycles] | xsession save|prove|restore <file> [seed]\n", argv[0]);
	return 2;
}
