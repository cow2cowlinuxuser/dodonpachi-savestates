/*
 * audio_coexist - in-process savestate loop WITH a live XAudio2 COM engine (Wine).
 *
 * We intentionally stop at XAudio2Create (no mastering voice): CreateMasteringVoice
 * hangs/crashes on this headless VM with no ALSA/Pulse device. The pin-able COM
 * object is IXAudio2 itself; client PCM outside the arena models the content/cursor
 * split (same vocabulary as GDI DIB pixels + DirectSound cursor).
 *
 *   wine audio_coexist.exe [cycles]
 */

#undef COBJMACROS
#define INITGUID
#include <windows.h>
#include <xaudio2.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifndef XAUDIO2_DEFAULT_PROCESSOR
#define XAUDIO2_DEFAULT_PROCESSOR 0xFFFFFFFFu
#endif

typedef HRESULT (WINAPI *PFN_XAudio2Create)(IXAudio2 **, UINT, UINT);
static PFN_XAudio2Create pXAudio2Create;

static int load_xaudio2(void)
{
	HMODULE m = LoadLibraryA("xaudio2_9.dll");
	if (!m) m = LoadLibraryA("xaudio2_8.dll");
	if (!m) return 0;
	pXAudio2Create = (PFN_XAudio2Create)GetProcAddress(m, "XAudio2Create");
	return pXAudio2Create != NULL;
}

#define SAMPLES 2048
#define PCM_BYTES (SAMPLES * sizeof(short))
#define NODE_CAP 256
#define NODE_MAGIC 0x41554421u /* 'AUD!' */

typedef struct Node { unsigned magic, serial, self_lo, checksum; } Node;

typedef struct Arena {
	unsigned magic, clock, seed;
	unsigned write_pos;
	IXAudio2 *xa2;               /* COM object held from rewound memory */
	unsigned gen;
	int free_head; unsigned in_use;
	Node node[NODE_CAP];
} Arena;

typedef struct Held {
	unsigned char *snapshot, *scratch;
	unsigned have_snap, saved_clock, saved_seed, saved_fp;
	unsigned gen;
	unsigned com_valid, split_observed, reconciled, cursor_split, poisoned;
} Held;

static Arena *g_arena;
static Held *g_held;
static short *g_pcm;
static unsigned g_play_pos;
static CRITICAL_SECTION g_cs;
static volatile LONG g_stop, g_quiesce, g_parked;
static HANDLE g_worker[2];
static int g_nworkers = 2;

static unsigned fnv1a(const void *p, size_t n)
{
	const unsigned char *b = p;
	unsigned h = 2166136261u;
	size_t i;
	for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
	return h;
}

static unsigned node_ck(const Node *n)
{
	unsigned p[3] = { n->magic, n->serial, n->self_lo };
	return fnv1a(p, sizeof(p));
}

static void fill_pcm(unsigned seed)
{
	unsigned i;
	for (i = 0; i < SAMPLES; i++) {
		int v = (int)(seed * 1103515245u + i * 12345u);
		g_pcm[i] = (short)((v >> 16) & 0x7fff);
	}
}

static unsigned pcm_fingerprint(void) { return fnv1a(g_pcm, PCM_BYTES); }

static int audio_setup(void)
{
	HRESULT hr;

	if (!load_xaudio2()) {
		printf("XAudio2 DLL not available\n");
		return 0;
	}
	hr = pXAudio2Create(&g_arena->xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
	if (FAILED(hr) || !g_arena->xa2) {
		printf("XAudio2Create failed hr=0x%08lx\n", (unsigned long)hr);
		return 0;
	}
	g_arena->gen = g_held->gen = 1;
	g_arena->write_pos = 0;
	printf("  IXAudio2=%p (engine only, no mastering voice on headless VM)\n", (void *)g_arena->xa2);
	return 1;
}

static int com_still_valid(void)
{
	ULONG n;
	if (!g_arena->xa2) return 0;
	n = g_arena->xa2->lpVtbl->AddRef(g_arena->xa2);
	g_arena->xa2->lpVtbl->Release(g_arena->xa2);
	return n > 0;
}

static void nodes_seed(void)
{
	unsigned i;
	g_arena->in_use = 0;
	for (i = 0; i < NODE_CAP; i++) {
		Node *n = &g_arena->node[i];
		if (i & 1) {
			n->magic = NODE_MAGIC; n->serial = i * 7 + 1; n->self_lo = i;
			n->checksum = node_ck(n); g_arena->in_use++;
		} else {
			n->magic = 0; n->self_lo = i; n->serial = 0; n->checksum = 0;
		}
	}
}

static unsigned identity_bad(const Arena *a)
{
	unsigned bad = 0, i;
	for (i = 0; i < NODE_CAP; i++) {
		const Node *n = &a->node[i];
		if (n->magic != NODE_MAGIC) continue;
		if (n->self_lo != i || n->checksum != node_ck(n)) bad++;
	}
	return bad;
}

static void quiesce(void)
{
	InterlockedExchange(&g_quiesce, 1);
	while (InterlockedCompareExchange(&g_parked, 0, 0) < g_nworkers && !g_stop) Sleep(0);
}

static void release_workers(void)
{
	InterlockedExchange(&g_quiesce, 0);
	while (InterlockedCompareExchange(&g_parked, 0, 0) > 0 && !g_stop) Sleep(0);
}

static DWORD WINAPI worker(LPVOID p)
{
	int id = (int)(intptr_t)p;
	unsigned long long tr = 0x51u ^ (unsigned)(id + 1) * 2654435761u;
	int parked = 0;
	while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
		if (InterlockedCompareExchange(&g_quiesce, 0, 0)) {
			if (!parked) { InterlockedIncrement(&g_parked); parked = 1; }
			Sleep(0); continue;
		}
		if (parked) { InterlockedDecrement(&g_parked); parked = 0; }
		tr ^= tr << 13; tr ^= tr >> 7; tr ^= tr << 17;
		EnterCriticalSection(&g_cs);
		{
			unsigned idx = (unsigned)(tr % NODE_CAP);
			Node *n = &g_arena->node[idx];
			if (n->magic == NODE_MAGIC) {
				n->magic = 0; n->checksum = 0;
				if (g_arena->in_use) g_arena->in_use--;
			} else {
				n->magic = NODE_MAGIC; n->serial = (unsigned)(tr >> 13);
				n->self_lo = idx; n->checksum = node_ck(n); g_arena->in_use++;
			}
		}
		LeaveCriticalSection(&g_cs);
	}
	if (parked) InterlockedDecrement(&g_parked);
	return 0;
}

#define ARENA_BYTES (sizeof(Arena))

static int checkpoint(void)
{
	Arena *scr = (Arena *)g_held->scratch;
	quiesce();
	memcpy(scr, g_arena, ARENA_BYTES);
	if (identity_bad(scr) == 0) {
		memcpy(g_held->snapshot, scr, ARENA_BYTES);
		g_held->have_snap = 1;
		g_held->saved_clock = scr->clock;
		g_held->saved_seed = scr->seed;
		release_workers();
		return 1;
	}
	release_workers();
	return 0;
}

static void restore(void) { quiesce(); memcpy(g_arena, g_held->snapshot, ARENA_BYTES); }

static void mutate_outside(void)
{
	unsigned i;
	g_play_pos = (g_play_pos + 512) % SAMPLES;
	g_arena->write_pos = (g_arena->write_pos + 128) % SAMPLES;
	for (i = 0; i < 64; i++)
		g_pcm[(g_play_pos + i) % SAMPLES] ^= (short)(0x1234 + i);
}

static void reconcile(void)
{
	fill_pcm(g_arena->seed);
	g_arena->write_pos = g_play_pos;
}

int main(int argc, char **argv)
{
	int cycles = argc > 1 ? atoi(argv[1]) : 150;
	int c, i;

	InitializeCriticalSection(&g_cs);
	g_pcm = (short *)VirtualAlloc(NULL, PCM_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	g_held = (Held *)VirtualAlloc(NULL, sizeof(Held), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!g_pcm || !g_held) { printf("alloc failed\n"); return 2; }
	memset(g_held, 0, sizeof(Held));
	g_held->snapshot = (unsigned char *)VirtualAlloc(NULL, ARENA_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	g_held->scratch  = (unsigned char *)VirtualAlloc(NULL, ARENA_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	g_arena = (Arena *)VirtualAlloc(NULL, ARENA_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!g_held->snapshot || !g_held->scratch || !g_arena) { printf("alloc failed\n"); return 2; }
	memset(g_arena, 0, ARENA_BYTES);
	g_arena->magic = NODE_MAGIC;

	if (!audio_setup()) { printf("XAudio2 setup failed\n"); return 2; }
	nodes_seed();
	printf("pid=%lu; savestate loop WITH IXAudio2 COM in-process\n", (unsigned long)GetCurrentProcessId());
	printf("cycles=%d workers=%d\n", cycles, g_nworkers);

	for (i = 0; i < g_nworkers; i++)
		g_worker[i] = CreateThread(NULL, 0, worker, (LPVOID)(intptr_t)i, 0, NULL);
	Sleep(30);

	for (c = 0; c < cycles && !g_stop; c++) {
		g_arena->clock++;
		g_arena->seed = (unsigned)(c + 1) * 7919u;
		fill_pcm(g_arena->seed);
		g_arena->write_pos = 0;
		g_play_pos = 0;
		if (!checkpoint()) { /* refused */ }
		g_held->saved_fp = pcm_fingerprint();

		mutate_outside();
		restore();

		if (com_still_valid()) g_held->com_valid++;
		if (pcm_fingerprint() != g_held->saved_fp) g_held->split_observed++;
		if (g_arena->write_pos < g_play_pos) g_held->cursor_split++;

		reconcile();
		if (pcm_fingerprint() == g_held->saved_fp) g_held->reconciled++;

		if (identity_bad(g_arena) != 0) g_held->poisoned++;
		release_workers();

		if ((c % 50) == 0)
			printf("  cycle %d: com_valid=%u split=%u cursor_split=%u reconciled=%u poisoned=%u\n",
			       c, g_held->com_valid, g_held->split_observed, g_held->cursor_split,
			       g_held->reconciled, g_held->poisoned);
	}

	InterlockedExchange(&g_stop, 1);
	for (i = 0; i < g_nworkers; i++)
		if (g_worker[i]) WaitForSingleObject(g_worker[i], 1000);

	printf("\nsummary (%d cycles, IXAudio2 live throughout):\n", cycles);
	printf("  COM object valid after restore : %u/%d\n", g_held->com_valid, cycles);
	printf("  PCM content split observed     : %u/%d\n", g_held->split_observed, cycles);
	printf("  write/play cursor split        : %u/%d\n", g_held->cursor_split, cycles);
	printf("  reconciled by refill+reseat    : %u/%d\n", g_held->reconciled, cycles);
	printf("  arena poisoned                 : %u\n", g_held->poisoned);

	{
		int ok = (g_held->com_valid == (unsigned)cycles &&
			  g_held->reconciled == (unsigned)cycles &&
			  g_held->poisoned == 0 &&
			  g_held->split_observed > 0);
		printf("\n%s: IXAudio2 COM did not block the loop; engine persists same-session; "
		       "PCM/cursor split reconciled from seed.\n", ok ? "PASS" : "FAIL");
		if (g_arena->xa2) g_arena->xa2->lpVtbl->Release(g_arena->xa2);
		DeleteCriticalSection(&g_cs);
		return ok ? 0 : 1;
	}
}
