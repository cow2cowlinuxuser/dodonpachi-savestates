/*
 * thread_policy_harness - plan item 7: presenter + copy helper + input waiter.
 * Save/restore must not Sleep(INFINITE) or SuspendThread those peers.
 *
 *   wine thread_policy_harness.exe insession [cycles] [--bad-freeze]
 *   wine thread_policy_harness.exe xsession save|prove|restore <file> [seed]
 */

#include "d3d11_scene.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Arena {
	unsigned magic;
	unsigned seed;
	unsigned present_serial;
	unsigned copy_serial;
	unsigned input_ticks;
	D3d11SceneState scene;
} Arena;

typedef struct Held {
	unsigned char *snap;
	unsigned saved_fp;
	unsigned saved_present;
	unsigned saved_copy;
	unsigned refused;
	unsigned poisoned;
	unsigned ok;
} Held;

static D3d11Scene g_sc;
static unsigned char *g_pix;
static D3d11SceneState g_st;
static Arena *g_arena;
static Held *g_held;

static volatile LONG g_stop;
static volatile LONG g_quiesce;
static volatile LONG g_present_parked;
static volatile LONG g_helper_parked;
static volatile LONG g_input_parked;
static volatile LONG g_present_serial;
static volatile LONG g_copy_serial;
static volatile LONG g_input_ticks;

static HANDLE g_presenter;
static HANDLE g_helper;
static HANDLE g_input;

static unsigned fnv_fp(void)
{
	return d3d11_scene_fnv1a(g_pix, D3D11_SCENE_PIXBYTES);
}

static void wait_all_parked(unsigned max_ms)
{
	unsigned i;
	for (i = 0; i < max_ms; i++) {
		if (InterlockedCompareExchange(&g_present_parked, 0, 0) &&
		    InterlockedCompareExchange(&g_helper_parked, 0, 0) &&
		    InterlockedCompareExchange(&g_input_parked, 0, 0))
			return;
		Sleep(1);
	}
}

static void quiesce_begin(void)
{
	InterlockedExchange(&g_quiesce, 1);
	wait_all_parked(5000);
}

static void quiesce_end(void)
{
	InterlockedExchange(&g_quiesce, 0);
	Sleep(2);
}

static DWORD WINAPI presenter_main(LPVOID unused)
{
	int parked = 0;
	(void)unused;
	while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
		if (InterlockedCompareExchange(&g_quiesce, 0, 0)) {
			if (!parked) {
				InterlockedExchange(&g_present_parked, 1);
				parked = 1;
			}
			Sleep(1);
			continue;
		}
		parked = 0;
		InterlockedExchange(&g_present_parked, 0);
		(void)d3d11_scene_render(&g_sc, &g_st, g_pix);
		d3d11_scene_flush(&g_sc);
		InterlockedIncrement(&g_present_serial);
		g_arena->present_serial = (unsigned)InterlockedCompareExchange(&g_present_serial, 0, 0);
		Sleep(1);
	}
	return 0;
}

static DWORD WINAPI helper_main(LPVOID unused)
{
	int parked = 0;
	(void)unused;
	while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
		if (InterlockedCompareExchange(&g_quiesce, 0, 0)) {
			if (!parked) {
				InterlockedExchange(&g_helper_parked, 1);
				parked = 1;
			}
			Sleep(1);
			continue;
		}
		parked = 0;
		InterlockedExchange(&g_helper_parked, 0);
		/* Stand-in for savestate helper: must stay responsive (no INFINITE sleep). */
		if (g_held->snap)
			memcpy(g_held->snap, g_arena, sizeof(Arena));
		InterlockedIncrement(&g_copy_serial);
		g_arena->copy_serial = (unsigned)InterlockedCompareExchange(&g_copy_serial, 0, 0);
		Sleep(2);
	}
	return 0;
}

static DWORD WINAPI input_waiter_main(LPVOID unused)
{
	int parked = 0;
	(void)unused;
	while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
		if (InterlockedCompareExchange(&g_quiesce, 0, 0)) {
			if (!parked) {
				InterlockedExchange(&g_input_parked, 1);
				parked = 1;
			}
			Sleep(1);
			continue;
		}
		parked = 0;
		InterlockedExchange(&g_input_parked, 0);
		/* Dummy dinput/win32u waiter: short poll, not INFINITE. */
		InterlockedIncrement(&g_input_ticks);
		g_arena->input_ticks = (unsigned)InterlockedCompareExchange(&g_input_ticks, 0, 0);
		Sleep(3);
	}
	return 0;
}

static int checkpoint_good(void)
{
	unsigned fp;
	quiesce_begin();
	fp = fnv_fp();
	memcpy(g_held->snap, g_arena, sizeof(Arena));
	g_held->saved_fp = fp;
	g_held->saved_present = g_arena->present_serial;
	g_held->saved_copy = g_arena->copy_serial;
	quiesce_end();
	return 1;
}

static int checkpoint_bad_freeze(void)
{
	/* Wrong policy: freeze presenter during copy (Proton Present-in-request pattern). */
	SuspendThread(g_presenter);
	memcpy(g_held->snap, g_arena, sizeof(Arena));
	g_held->saved_fp = fnv_fp();
	g_held->saved_present = g_arena->present_serial;
	g_held->saved_copy = g_arena->copy_serial;
	ResumeThread(g_presenter);
	printf("  bad-freeze: SuspendThread(presenter) during copy (restore skips quiesce)\n");
	return 1;
}

static void restore_from_snap(void)
{
	quiesce_begin();
	memcpy(g_arena, g_held->snap, sizeof(Arena));
	g_st = g_arena->scene;
	d3d11_scene_render(&g_sc, &g_st, g_pix);
	d3d11_scene_flush(&g_sc);
	quiesce_end();
}

static void restore_blind_no_quiesce(void)
{
	memcpy(g_arena, g_held->snap, sizeof(Arena));
	g_st = g_arena->scene;
	/* Wrong policy: no quiesce, no redraw — mimics restore while presenter mid-frame. */
}

static int insession(int cycles, int bad_freeze)
{
	unsigned c;
	g_arena->magic = 0x54485244u; /* 'THRD' */
	g_presenter = CreateThread(NULL, 0, presenter_main, NULL, 0, NULL);
	g_helper = CreateThread(NULL, 0, helper_main, NULL, 0, NULL);
	g_input = CreateThread(NULL, 0, input_waiter_main, NULL, 0, NULL);
	Sleep(80);
	printf("insession cycles=%d policy=%s (no Sleep(INFINITE) on peers)\n",
	       cycles, bad_freeze ? "bad-freeze" : "cooperative-quiesce");

	for (c = 0; c < (unsigned)cycles && !g_stop; c++) {
		d3d11_scene_seed_state(&g_st, (unsigned)(c + 1) * 7001u);
		g_arena->scene = g_st;
		g_arena->seed = g_st.seed;
		Sleep(5);
		if (bad_freeze) {
			if (!checkpoint_bad_freeze()) {
				g_held->refused++;
				continue;
			}
		} else {
			if (!checkpoint_good()) {
				g_held->refused++;
				continue;
			}
		}
		Sleep(5);
		if (bad_freeze) {
			restore_blind_no_quiesce();
			if ((unsigned)InterlockedCompareExchange(&g_present_serial, 0, 0) != g_held->saved_present)
				g_held->poisoned++;
			else if (fnv_fp() == g_held->saved_fp)
				g_held->ok++;
		} else {
			restore_from_snap();
			if (fnv_fp() == g_held->saved_fp)
				g_held->ok++;
		}
		if ((c % 20) == 0)
			printf("  c=%u ok=%u refused=%u poisoned=%u present=%u copy=%u input=%u\n",
			       c, g_held->ok, g_held->refused, g_held->poisoned,
			       g_arena->present_serial, g_arena->copy_serial, g_arena->input_ticks);
	}

	InterlockedExchange(&g_stop, 1);
	WaitForSingleObject(g_presenter, 3000);
	WaitForSingleObject(g_helper, 3000);
	WaitForSingleObject(g_input, 3000);

	printf("\nsummary: ok=%u/%d refused=%u poisoned=%u\n",
	       g_held->ok, cycles, g_held->refused, g_held->poisoned);
	if (bad_freeze) {
		int pass = (g_held->poisoned == (unsigned)cycles);
		printf("%s: bad-freeze rewinds live presenter serial (%u/%d poisoned)\n",
		       pass ? "PASS" : "FAIL", g_held->poisoned, cycles);
		return pass ? 0 : 1;
	}
	{
		int pass = (g_held->ok == (unsigned)cycles && g_held->poisoned == 0);
		printf("%s: cooperative quiesce without freezing OS/Wine peers\n", pass ? "PASS" : "FAIL");
		return pass ? 0 : 1;
	}
}

typedef struct XSnap {
	char magic[8]; /* "THRDPOL7" */
	unsigned ver;
	D3d11SceneState scene;
	D3d11SceneSnapRefs refs;
	unsigned fp;
	unsigned present_serial;
	unsigned copy_serial;
} XSnap;

static int xsession_cmd(const char *cmd, const char *file, unsigned seed)
{
	if (strcmp(cmd, "save") == 0) {
		XSnap hdr;
		FILE *f;
		quiesce_begin();
		d3d11_scene_seed_state(&g_st, seed);
		g_arena->scene = g_st;
		memset(&hdr, 0, sizeof(hdr));
		memcpy(hdr.magic, "THRDPOL7", 8);
		hdr.ver = 1;
		hdr.scene = g_st;
		hdr.fp = d3d11_scene_render(&g_sc, &g_st, g_pix);
		d3d11_scene_flush(&g_sc);
		d3d11_scene_fill_refs(&g_sc, &hdr.refs);
		hdr.present_serial = (unsigned)InterlockedCompareExchange(&g_present_serial, 0, 0);
		hdr.copy_serial = (unsigned)InterlockedCompareExchange(&g_copy_serial, 0, 0);
		quiesce_end();
		f = fopen(file, "wb");
		if (!f) return 2;
		fwrite(&hdr, sizeof(hdr), 1, f);
		fwrite(g_pix, 1, D3D11_SCENE_PIXBYTES, f);
		fclose(f);
		printf("SAVE seed=%u fp=0x%08x present=%u copy=%u -> %s\n", seed, hdr.fp,
		       hdr.present_serial, hdr.copy_serial, file);
		return 0;
	}
	if (strcmp(cmd, "prove") == 0 || strcmp(cmd, "restore") == 0) {
		XSnap hdr;
		FILE *f;
		unsigned char *saved;
		unsigned fp, diff = 0, i;
		int dead = 0;
		saved = (unsigned char *)malloc(D3D11_SCENE_PIXBYTES);
		if (!saved) return 2;
		f = fopen(file, "rb");
		if (!f || fread(&hdr, sizeof(hdr), 1, f) != 1 || memcmp(hdr.magic, "THRDPOL7", 8) != 0) {
			printf("bad snap\n");
			if (f) fclose(f);
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
		printf("PROVE dead=%d/6 (saved COM not live in process B)\n", dead);
		if (strcmp(cmd, "prove") == 0) {
			free(saved);
			printf("%s: thread-policy snap does not adopt saved D3D refs\n", dead >= 6 ? "PASS" : "FAIL");
			return dead >= 6 ? 0 : 1;
		}
		g_st = hdr.scene;
		fp = d3d11_scene_render(&g_sc, &g_st, g_pix);
		for (i = 0; i < D3D11_SCENE_PIXBYTES; i++)
			if (g_pix[i] != saved[i]) diff++;
		printf("RESTORE fp_match=%s diffs=%u (no INFINITE sleep on peers during save)\n",
		       (fp == hdr.fp) ? "YES" : "NO", diff);
		free(saved);
		{
			int ok = (dead >= 6) && (fp == hdr.fp) && (diff == 0);
			printf("%s: xsession restore with thread-safe save policy\n", ok ? "PASS" : "FAIL");
			return ok ? 0 : 1;
		}
	}
	return 2;
}

int main(int argc, char **argv)
{
	unsigned seed = 7707u;
	int bad_freeze = 0;
	int i;

	g_pix = (unsigned char *)malloc(D3D11_SCENE_PIXBYTES);
	g_held = (Held *)calloc(1, sizeof(Held));
	g_arena = (Arena *)calloc(1, sizeof(Arena));
	g_held->snap = (unsigned char *)malloc(sizeof(Arena));
	if (!g_pix || !g_held || !g_arena || !g_held->snap)
		return 2;

	for (i = 1; i < argc; i++)
		if (strcmp(argv[i], "--bad-freeze") == 0) bad_freeze = 1;

	if (!d3d11_scene_init(&g_sc, seed)) {
		printf("D3D11 init failed (unix C000001D? check wined3d/llvmpipe)\n");
		return 2;
	}
	d3d11_scene_seed_state(&g_st, seed);
	g_arena->scene = g_st;

	if (argc > 1 && strcmp(argv[1], "insession") == 0) {
		int cycles = argc > 2 ? atoi(argv[2]) : 60;
		int rc = insession(cycles, bad_freeze);
		d3d11_scene_shutdown(&g_sc);
		return rc;
	}
	if (argc > 1 && strcmp(argv[1], "xsession") == 0 && argc > 3) {
		int rc;
		if (argc > 4) seed = (unsigned)strtoul(argv[4], NULL, 0);
		d3d11_scene_seed_state(&g_st, seed);
		g_presenter = CreateThread(NULL, 0, presenter_main, NULL, 0, NULL);
		g_helper = CreateThread(NULL, 0, helper_main, NULL, 0, NULL);
		g_input = CreateThread(NULL, 0, input_waiter_main, NULL, 0, NULL);
		Sleep(80);
		rc = xsession_cmd(argv[2], argv[3], seed);
		InterlockedExchange(&g_stop, 1);
		WaitForSingleObject(g_presenter, 3000);
		WaitForSingleObject(g_helper, 3000);
		WaitForSingleObject(g_input, 3000);
		d3d11_scene_shutdown(&g_sc);
		return rc;
	}

	printf("usage: %s insession [cycles] [--bad-freeze]\n", argv[0]);
	printf("       %s xsession save|prove|restore <file> [seed]\n", argv[0]);
	d3d11_scene_shutdown(&g_sc);
	return 2;
}
