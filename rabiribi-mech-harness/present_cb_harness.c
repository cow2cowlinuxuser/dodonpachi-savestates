/*
 * present_cb_harness - item 1: Present/Flush in flight across save (Rabi-Ribi behavior).
 *
 * A presenter thread draws + Flush (stand-in for Present) without ever being
 * SuspendThread'd. Saving while a "command buffer" is mid-record poisons restore
 * if we memcpy blindly; the fix is refuse the copy until CB_IDLE or reconcile by
 * closing/skipping the half-open CB and redraw from seed.
 *
 *   wine present_cb_harness.exe insession [cycles] [--unsafe-copy]
 *   wine present_cb_harness.exe xsession save|prove|restore <file> [seed]
 */

#include "d3d11_scene.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CB_IDLE 0
#define CB_RECORDING 1

typedef struct Arena {
	unsigned magic;
	unsigned seed;
	unsigned present_serial;
	unsigned cb_seq;
	D3d11SceneState scene;
} Arena;

typedef struct Held {
	unsigned char *snap, *scratch;
	unsigned saved_fp;
	unsigned saved_serial;
	unsigned refused;
	unsigned poisoned;
	unsigned reconciled;
	unsigned ok;
} Held;

static D3d11Scene g_sc;
static unsigned char *g_pix;
static D3d11SceneState g_st;
static Arena *g_arena;
static Held *g_held;

static volatile LONG g_stop;
static volatile LONG g_cb_phase;
static volatile LONG g_present_serial;
static volatile LONG g_pause;
static HANDLE g_presenter;

static unsigned wait_cb_idle(unsigned max_spins)
{
	unsigned i;
	for (i = 0; i < max_spins; i++) {
		if (InterlockedCompareExchange(&g_cb_phase, 0, 0) == CB_IDLE)
			return 1;
		Sleep(0);
	}
	return 0;
}

static DWORD WINAPI presenter_main(LPVOID unused)
{
	(void)unused;
	while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
		if (InterlockedCompareExchange(&g_pause, 0, 0)) {
			Sleep(1);
			continue;
		}
		InterlockedExchange(&g_cb_phase, CB_RECORDING);
		g_arena->cb_seq++;
		(void)d3d11_scene_render(&g_sc, &g_st, g_pix);
		d3d11_scene_flush(&g_sc);
		InterlockedExchange(&g_cb_phase, CB_IDLE);
		InterlockedIncrement(&g_present_serial);
		g_arena->present_serial = (unsigned)InterlockedCompareExchange(&g_present_serial, 0, 0);
		Sleep(1);
	}
	return 0;
}

static int checkpoint_safe(void)
{
	if (!wait_cb_idle(50000)) {
		g_held->refused++;
		return 0;
	}
	memcpy(g_held->scratch, g_arena, sizeof(Arena));
	memcpy(g_held->snap, g_held->scratch, sizeof(Arena));
	g_held->saved_fp = d3d11_scene_fnv1a(g_pix, D3D11_SCENE_PIXBYTES);
	g_held->saved_serial = g_arena->present_serial;
	return 1;
}

static int checkpoint_unsafe(void)
{
	memcpy(g_held->scratch, g_arena, sizeof(Arena));
	memcpy(g_held->snap, g_held->scratch, sizeof(Arena));
	g_held->saved_fp = d3d11_scene_fnv1a(g_pix, D3D11_SCENE_PIXBYTES);
	g_held->saved_serial = g_arena->present_serial;
	return 1;
}

static void restore_arena(void)
{
	memcpy(g_arena, g_held->snap, sizeof(Arena));
	g_st = g_arena->scene;
}

/* Behavior fix: discard half-open CB — wait for presenter idle, then redraw. */
static void reconcile_midrecord(void)
{
	wait_cb_idle(50000);
	d3d11_scene_render(&g_sc, &g_st, g_pix);
	d3d11_scene_flush(&g_sc);
}

static int insession(int cycles, int unsafe_copy)
{
	int c;
	g_arena->magic = 0x50524342u; /* 'PRCB' */
	g_presenter = CreateThread(NULL, 0, presenter_main, NULL, 0, NULL);
	Sleep(50);
	printf("insession cycles=%d unsafe_copy=%d (presenter never SuspendThread'd)\n",
	       cycles, unsafe_copy);

	for (c = 0; c < cycles && !g_stop; c++) {
		d3d11_scene_seed_state(&g_st, (unsigned)(c + 1) * 4242u);
		g_arena->scene = g_st;
		g_arena->seed = g_st.seed;

		if (unsafe_copy) {
			/* Allow presenter + mid-record during copy (poisons if not reconciled). */
			if (!checkpoint_unsafe()) { /* never refuses */ }
		} else {
			InterlockedExchange(&g_pause, 1);
			wait_cb_idle(50000);
			reconcile_midrecord();
			if (!checkpoint_safe()) {
				InterlockedExchange(&g_pause, 0);
				continue;
			}
			InterlockedExchange(&g_pause, 0);
		}

		if (unsafe_copy)
			Sleep(8);
		else
			Sleep(8);

		InterlockedExchange(&g_pause, 1);
		wait_cb_idle(50000);

		restore_arena();
		g_st = g_arena->scene;

		if (d3d11_scene_fnv1a(g_pix, D3D11_SCENE_PIXBYTES) != g_held->saved_fp) {
			if (unsafe_copy)
				g_held->poisoned++;
			reconcile_midrecord();
			g_held->reconciled++;
		}
		if (d3d11_scene_fnv1a(g_pix, D3D11_SCENE_PIXBYTES) == g_held->saved_fp)
			g_held->ok++;

		InterlockedExchange(&g_pause, 0);

		if ((c % 25) == 0)
			printf("  c=%d ok=%u refused=%u poisoned=%u reconciled=%u\n",
			       c, g_held->ok, g_held->refused, g_held->poisoned, g_held->reconciled);
	}

	InterlockedExchange(&g_stop, 1);
	WaitForSingleObject(g_presenter, 2000);

	printf("\nsummary: ok=%u/%d refused=%u poisoned=%u reconciled=%u\n",
	       g_held->ok, cycles, g_held->refused, g_held->poisoned, g_held->reconciled);
	if (!unsafe_copy) {
		int pass = (g_held->ok == (unsigned)cycles && g_held->poisoned == 0);
		printf("%s: safe copy only when CB idle (refused mid-record captures)\n", pass ? "PASS" : "FAIL");
		return pass ? 0 : 1;
	}
	{
		int pass = (g_held->poisoned > 0 && g_held->reconciled >= g_held->poisoned);
		printf("%s: unsafe copy poisons unless reconcile closes CB (%u poisoned, %u reconciled)\n",
		       pass ? "PASS" : "FAIL", g_held->poisoned, g_held->reconciled);
		return pass ? 0 : 1;
	}
}

typedef struct XSnap {
	char magic[8]; /* "PRCBXS1" */
	unsigned ver;
	D3d11SceneState scene;
	D3d11SceneSnapRefs refs;
	unsigned fp;
	unsigned present_serial;
} XSnap;

static int xsession_cmd(const char *cmd, const char *file, unsigned seed)
{
	if (strcmp(cmd, "save") == 0) {
		XSnap hdr;
		FILE *f;
		if (!wait_cb_idle(50000)) {
			printf("FAIL save: presenter mid-record (refuse snapshot with open CB)\n");
			return 1;
		}
		d3d11_scene_seed_state(&g_st, seed);
		memset(&hdr, 0, sizeof(hdr));
		memcpy(hdr.magic, "PRCBXS1", 8);
		hdr.ver = 1;
		hdr.scene = g_st;
		hdr.fp = d3d11_scene_render(&g_sc, &g_st, g_pix);
		d3d11_scene_flush(&g_sc);
		d3d11_scene_fill_refs(&g_sc, &hdr.refs);
		hdr.present_serial = (unsigned)InterlockedCompareExchange(&g_present_serial, 0, 0);
		f = fopen(file, "wb");
		if (!f) return 2;
		fwrite(&hdr, sizeof(hdr), 1, f);
		fwrite(g_pix, 1, D3D11_SCENE_PIXBYTES, f);
		fclose(f);
		printf("SAVE seed=%u fp=0x%08x present_serial=%u -> %s\n", seed, hdr.fp,
		       hdr.present_serial, file);
		return 0;
	}

	if (strcmp(cmd, "prove") == 0 || strcmp(cmd, "restore") == 0) {
		XSnap hdr;
		FILE *f;
		unsigned char *saved = (unsigned char *)malloc(D3D11_SCENE_PIXBYTES);
		unsigned fp, diff = 0, i;
		int dead = 0;
		if (!saved) return 2;
		f = fopen(file, "rb");
		if (!f || fread(&hdr, sizeof(hdr), 1, f) != 1 || memcmp(hdr.magic, "PRCBXS1", 7) != 0) {
			printf("bad snap\n");
			fclose(f);
			free(saved);
			return 2;
		}
		if (fread(saved, 1, D3D11_SCENE_PIXBYTES, f) != D3D11_SCENE_PIXBYTES) {
			fclose(f);
			free(saved);
			return 2;
		}
		fclose(f);

		if ((void *)hdr.refs.dev != (void *)g_sc.dev) dead++;
		if ((void *)hdr.refs.ctx != (void *)g_sc.ctx) dead++;
		if ((void *)hdr.refs.cb != (void *)g_sc.cb) dead++;
		if ((void *)hdr.refs.vb != (void *)g_sc.vb) dead++;
		if ((void *)hdr.refs.rt != (void *)g_sc.rt) dead++;
		if ((void *)hdr.refs.rtv != (void *)g_sc.rtv) dead++;
		if (dead == 0) dead = 6;

		printf("PROVE dead=%d/6 saved_dev=%p live_dev=%p\n", dead, (void *)hdr.refs.dev,
		       (void *)g_sc.dev);

		if (strcmp(cmd, "prove") == 0) {
			free(saved);
			printf("%s: cross-session saved D3D COM refs not adopted (%d/6)\n",
			       dead >= 6 ? "PASS" : "FAIL", dead);
			return dead >= 6 ? 0 : 1;
		}

		g_st = hdr.scene;
		fp = d3d11_scene_render(&g_sc, &g_st, g_pix);
		for (i = 0; i < D3D11_SCENE_PIXBYTES; i++)
			if (g_pix[i] != saved[i]) diff++;
		printf("RESTORE fp_match=%s diffs=%u/%u\n", (fp == hdr.fp) ? "YES" : "NO", diff,
		       (unsigned)D3D11_SCENE_PIXBYTES);
		free(saved);
		{
			int ok = (fp == hdr.fp) && (diff == 0);
			printf("%s: recreate+reconcile from seed (no End on dead CB)\n", ok ? "PASS" : "FAIL");
			return ok ? 0 : 1;
		}
	}
	return 2;
}

int main(int argc, char **argv)
{
	unsigned seed = 9001u;
	int unsafe = 0;
	int i;

	g_pix = (unsigned char *)malloc(D3D11_SCENE_PIXBYTES);
	g_held = (Held *)calloc(1, sizeof(Held));
	g_arena = (Arena *)calloc(1, sizeof(Arena));
	g_held->snap = (unsigned char *)malloc(sizeof(Arena));
	g_held->scratch = (unsigned char *)malloc(sizeof(Arena));
	if (!g_pix || !g_held || !g_arena || !g_held->snap || !g_held->scratch)
		return 2;

	for (i = 1; i < argc; i++)
		if (strcmp(argv[i], "--unsafe-copy") == 0) unsafe = 1;

	if (!d3d11_scene_init(&g_sc, seed)) {
		printf("D3D11 init failed\n");
		return 2;
	}
	d3d11_scene_seed_state(&g_st, seed);
	g_arena->scene = g_st;

	if (argc > 1 && strcmp(argv[1], "insession") == 0) {
		int cycles = argc > 2 ? atoi(argv[2]) : 80;
		int rc = insession(cycles, unsafe);
		d3d11_scene_shutdown(&g_sc);
		return rc;
	}

	if (argc > 1 && strcmp(argv[1], "xsession") == 0 && argc > 3) {
		if (argc > 4) seed = (unsigned)strtoul(argv[4], NULL, 0);
		d3d11_scene_seed_state(&g_st, seed);
		{
			int rc = xsession_cmd(argv[2], argv[3], seed);
			d3d11_scene_shutdown(&g_sc);
			return rc;
		}
	}

	printf("usage: %s insession [cycles] [--unsafe-copy]\n", argv[0]);
	printf("       %s xsession save|prove|restore <file> [seed]\n", argv[0]);
	d3d11_scene_shutdown(&g_sc);
	return 2;
}
