/*
 * rr_mech_harness - a mechanical bounds-mapper for the Rabi-Ribi in-process
 * savestate, focused on the capture side.
 *
 * The blocker this rig is built around: a bad capture is NOT self-healing. If a
 * save is taken while the world is inconsistent, the snapshot is poisoned, and
 * every restore from it faithfully reproduces the corruption. So the real
 * question is not "can we copy memory" - it is "can we guarantee we never COMMIT
 * a poisoned capture", using only mechanisms the real engine actually has. The
 * game's threads are not ours, so a cooperative safe-point barrier (the trivial
 * win) is off the table; the engine has SuspendThread and little else.
 *
 * This harness therefore models capture as a strategy and measures, for each,
 * how often it commits / refuses / and - the number that matters - POISONS a
 * slot (commits a snapshot that then restores wrong):
 *
 *   RACE            copy while writers run                       (worst)
 *   SUSPEND         SuspendThread all, copy, resume              (the engine today,
 *                                                                 minus the settle
 *                                                                 loop it skips under Wine)
 *   SUSPEND+SETTLE  suspend, and only copy if no writer is in a
 *                   known hot region; else resume and retry      (the settle loop)
 *   SUSPEND+VERIFY  suspend, copy to scratch, resume, then VERIFY
 *                   the scratch; commit only if clean, else retry;
 *                   if no clean shot in K tries, REFUSE the save  (the proposed fix)
 *   BARRIER         cooperative safe point                        (ideal, but needs
 *                                                                 threads we control -
 *                                                                 the north star, not
 *                                                                 portable to the game)
 *
 * The claim under test: SUSPEND+VERIFY never poisons a slot. A save either
 * commits a snapshot proven consistent, or is refused and leaves the last good
 * snapshot intact. That converts "silent poisoned slot" into "an occasional
 * save you retry", which is a bound you can live inside.
 *
 * It is self-contained: no game, no real savestate.c. It stands up a small world
 * with the ownership shapes the docs name, runs its own byte snapshot/restore
 * over one pinned arena, and checks the restored state through the node's own
 * callback + a writer-maintained checksum (behaviour, never struct layout).
 *
 *   usage: rr_mech_harness[32].exe [soak-cycles] [workers]
 *   default: 4000 soak cycles, 6 workers.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ config */

#define ARENA_WANT_BASE ((void *)0x30000000)
#define ARENA_BYTES     (4u * 1024u * 1024u)
#define NODE_CAP        8192
#define NODE_MAGIC      0x5252424Bu /* 'RRBK' */
#define RING            48000
#define DET_DRAW        16
#define MAX_WORKERS     32
#define NPT             256
#define FILL_SPIN       3000        /* width of the deliberate torn window */
#define CAP_TRIES_DEF   256         /* retries for settle/verify before refusing */
static int g_cap_tries = CAP_TRIES_DEF; /* tunable, to force the refuse path */

typedef unsigned (*NodeFn)(unsigned serial, unsigned payload);

static unsigned fn_sum(unsigned s, unsigned p) { return s + p; }
static unsigned fn_xor(unsigned s, unsigned p) { return s ^ (p * 2654435761u); }
static unsigned fn_mix(unsigned s, unsigned p) { return (s * 1664525u) + p + 1013904223u; }
static unsigned fn_rot(unsigned s, unsigned p) { unsigned v = s + p; return (v << 7) | (v >> 25); }
static NodeFn   FN_TABLE[4] = { fn_sum, fn_xor, fn_mix, fn_rot };

/* capture strategies */
enum { CAP_RACE = 0, CAP_SUSPEND, CAP_SETTLE, CAP_VERIFY, CAP_BARRIER };
static const char *CAP_NAME[5] = { "RACE", "SUSPEND", "SUSPEND+SETTLE", "SUSPEND+VERIFY", "BARRIER" };

/* --------------------------------------------------------------- the world */

typedef struct Node {
	unsigned magic;
	unsigned serial;
	unsigned payload;
	unsigned fn_idx;
	struct Node *self;   /* absolute pointer into the LIVE arena; rewound as bytes */
	int      next;
	unsigned answer;
	unsigned checksum;   /* written LAST */
	unsigned pindex;     /* Class A: outward index into Held.present_tok */
	unsigned expect_tok;
} Node;

typedef struct Arena {
	unsigned magic;
	unsigned clock;
	unsigned long long rng;
	unsigned write_cursor;
	int      free_head;
	unsigned in_use;
	unsigned worker_ops[MAX_WORKERS];
	Node     node[NODE_CAP];
} Arena;

typedef struct Config {
	int capture_mode;
	int classA_replay;
	int classB_exclude;
	int cursor_reseat;
	int classA_active;
	int classB_active;
} Config;

typedef struct Held {
	unsigned char *snapshot;   /* the committed save point */
	unsigned char *scratch;    /* tentative capture, verified before commit */
	unsigned have_snap;

	unsigned long long saved_rng;
	unsigned           saved_clock;
	unsigned           exp_draw[DET_DRAW];

	volatile LONG hw_cursor;

	Node *classA_ptr;
	unsigned present_tok[NPT];
	unsigned classA_saved_idx, classA_saved_tok;

	Node *classB_ptr;
	unsigned classB_expect;

	unsigned cycles, restores;
	unsigned saves_committed, saves_refused, poisoned;
	unsigned bad_identity, bad_determinism, bad_heap, bad_classA, bad_classB, bad_cursor;
	unsigned thr_gone, thr_fresh;
	unsigned cap_retries; /* total retries spent in settle/verify */
} Held;

/* --------------------------------------------------------------- globals */

static Arena *g_arena;
static Held  *g_held;
static Config g_cfg;

static CRITICAL_SECTION g_arena_cs;
static volatile LONG g_stop;
static volatile LONG g_quiesce;
static volatile LONG g_parked;
static volatile LONG g_rewinding;
static volatile LONG g_in_fill[MAX_WORKERS]; /* a worker is inside the fill hot region */
static int g_workers = 6;

static HANDLE g_worker[MAX_WORKERS];
static DWORD  g_worker_tid[MAX_WORKERS];
static HANDLE g_sys_thread, g_audio_thread;

/* ------------------------------------------------------------ small utils */

static unsigned long long xorshift64(unsigned long long x)
{ x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x ? x : 0x9E3779B97F4A7C15ull; }

static unsigned node_checksum(const Node *n)
{
	unsigned h = 2166136261u;
	const unsigned parts[6] = { n->magic, n->serial, n->payload, n->fn_idx,
				    (unsigned)(uintptr_t)n->self, n->answer };
	int i;
	for (i = 0; i < 6; i++) { h ^= parts[i]; h *= 16777619u; }
	return h;
}

/* -------------------------------------------------- arena node allocator */

static void alloc_init(void)
{
	int i;
	g_arena->free_head = 0;
	for (i = 0; i < NODE_CAP; i++) {
		g_arena->node[i].magic = 0;
		g_arena->node[i].next = (i + 1 < NODE_CAP) ? i + 1 : -1;
	}
	g_arena->in_use = 0;
}
static Node *node_alloc_locked(void)
{
	int idx = g_arena->free_head; Node *n;
	if (idx < 0) return NULL;
	n = &g_arena->node[idx];
	g_arena->free_head = n->next; n->next = -1; g_arena->in_use++;
	return n;
}
static void node_free_locked(Node *n)
{
	int idx = (int)(n - g_arena->node);
	n->magic = 0; n->answer = 0; n->checksum = 0;
	n->next = g_arena->free_head; g_arena->free_head = idx; g_arena->in_use--;
}
static void node_fill(Node *n, unsigned serial)
{
	n->magic = NODE_MAGIC;
	n->serial = serial;
	n->payload = serial * 2654435761u + 12345u;
	n->fn_idx = serial & 3u;
	n->self = n;
	n->pindex = serial % NPT;
	n->expect_tok = serial ^ 0xA5A5u;
	n->answer = FN_TABLE[n->fn_idx](n->serial, n->payload);
	/* deliberate, bounded torn window so the capture pitfalls reproduce */
	{ volatile unsigned s = 0; unsigned k; for (k = 0; k < FILL_SPIN; k++) s += k * 3u + 1u; (void)s; }
	n->checksum = node_checksum(n); /* LAST */
}

/* Heap-metadata walk over an arbitrary buffer (snapshot/scratch/live). 0 = ok.
 * Indices are relative, so it works on any copy. */
static int heap_validate_buf(const Arena *a)
{
	int idx = a->free_head, steps = 0; unsigned free_seen = 0, i;
	static unsigned char onlist[NODE_CAP];
	memset(onlist, 0, sizeof(onlist));
	while (idx >= 0) {
		if (idx >= NODE_CAP) return 1;
		if (onlist[idx]) return 2;
		if (a->node[idx].magic == NODE_MAGIC) return 3;
		onlist[idx] = 1; free_seen++;
		idx = a->node[idx].next;
		if (++steps > NODE_CAP + 1) return 4;
	}
	if (free_seen + a->in_use != NODE_CAP) return 5;
	for (i = 0; i < NODE_CAP; i++)
		if (!onlist[i] && a->node[i].magic != NODE_MAGIC) return 6;
	return 0;
}
static int heap_validate(void)
{
	int rc; EnterCriticalSection(&g_arena_cs); rc = heap_validate_buf(g_arena); LeaveCriticalSection(&g_arena_cs); return rc;
}

/* Identity of every in-use node in a buffer. The self pointer is canonical: it
 * must equal the LIVE arena address of that slot, even in a copy (the bytes hold
 * the live address). Proven through the node's own callback + checksum. */
static unsigned identity_bad_buf(const Arena *a)
{
	unsigned bad = 0, i;
	for (i = 0; i < NODE_CAP; i++) {
		const Node *n = &a->node[i];
		if (n->magic != NODE_MAGIC) continue;
		if (n->self != &g_arena->node[i]) { bad++; continue; }
		if (n->fn_idx > 3) { bad++; continue; }
		if (n->answer != FN_TABLE[n->fn_idx](n->serial, n->payload)) { bad++; continue; }
		if (n->checksum != node_checksum(n)) { bad++; continue; }
	}
	return bad;
}
/* consistency of a tentative capture: identity + heap metadata. 0 = clean. */
static int capture_is_clean(const Arena *a)
{
	return identity_bad_buf(a) == 0 && heap_validate_buf(a) == 0;
}

/* --------------------------------------------------------- the barrier */

static void quiesce(void)
{ InterlockedExchange(&g_quiesce, 1); while (InterlockedCompareExchange(&g_parked, 0, 0) < g_workers && !g_stop) Sleep(0); }
static void release_workers(void)
{ InterlockedExchange(&g_quiesce, 0); while (InterlockedCompareExchange(&g_parked, 0, 0) > 0 && !g_stop) Sleep(0); }

static void suspend_all_workers(void) { int i; for (i = 0; i < g_workers; i++) if (g_worker[i]) SuspendThread(g_worker[i]); }
static void resume_all_workers(void)  { int i; for (i = 0; i < g_workers; i++) if (g_worker[i]) ResumeThread(g_worker[i]); }
static int  any_worker_in_fill(void)  { int i; for (i = 0; i < g_workers; i++) if (InterlockedCompareExchange(&g_in_fill[i], 0, 0)) return 1; return 0; }

/* --------------------------------------------------------- mutator thread */

static DWORD WINAPI worker_main(LPVOID p)
{
	int id = (int)(intptr_t)p;
	unsigned long long tr = 0x1234567u ^ (0x9E37u * (unsigned)(id + 1));
	int was_parked = 0;

	while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
		if (InterlockedCompareExchange(&g_quiesce, 0, 0)) {
			if (!was_parked) { InterlockedIncrement(&g_parked); was_parked = 1; }
			Sleep(0); continue;
		}
		if (was_parked) { InterlockedDecrement(&g_parked); was_parked = 0; }

		tr = xorshift64(tr);
		EnterCriticalSection(&g_arena_cs);
		if ((tr & 3) != 0) {
			Node *n = node_alloc_locked();
			if (n) {
				InterlockedExchange(&g_in_fill[id], 1); /* the "known hot region" */
				node_fill(n, (unsigned)(tr >> 20));
				InterlockedExchange(&g_in_fill[id], 0);
			}
		} else {
			int idx = (int)((tr >> 8) % NODE_CAP), scan;
			for (scan = 0; scan < 16; scan++) {
				Node *n = &g_arena->node[(idx + scan) % NODE_CAP];
				if (n->magic == NODE_MAGIC && n != g_held->classA_ptr &&
				    n != g_held->classB_ptr) { node_free_locked(n); break; }
			}
		}
		LeaveCriticalSection(&g_arena_cs);
		g_arena->worker_ops[id]++;
		if ((tr & 7) == 7) Sleep(0);
	}
	if (was_parked) InterlockedDecrement(&g_parked);
	return 0;
}

static DWORD WINAPI sys_main(LPVOID p)
{
	(void)p;
	while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
		Node *n = g_held->classB_ptr;
		if (n && !InterlockedCompareExchange(&g_rewinding, 0, 0))
			if (n->magic != NODE_MAGIC || n->self != n)
				InterlockedIncrement((volatile LONG *)&g_held->bad_classB);
		Sleep(1);
	}
	return 0;
}
static DWORD WINAPI audio_main(LPVOID p)
{
	(void)p;
	while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
		LONG c = InterlockedExchangeAdd(&g_held->hw_cursor, 64) + 64;
		if (c >= RING) InterlockedExchange(&g_held->hw_cursor, c % RING);
		Sleep(1);
	}
	return 0;
}

/* --------------------------------------------------- the capture engine */

/* Commit a verified-or-trusted buffer as the new save point. */
static void commit_from(const Arena *src)
{
	memcpy(g_held->snapshot, src, ARENA_BYTES);
	g_held->have_snap = 1;
	g_held->saved_rng = src->rng;
	g_held->saved_clock = src->clock;
	{ unsigned long long tmp = src->rng; int i;
	  for (i = 0; i < DET_DRAW; i++) { tmp = xorshift64(tmp); g_held->exp_draw[i] = (unsigned)tmp; } }
	if (g_cfg.classA_active && g_held->classA_ptr) {
		g_held->classA_saved_idx = g_held->classA_ptr->pindex;
		g_held->classA_saved_tok = g_held->present_tok[g_held->classA_ptr->pindex];
	}
}

/* Take a save point according to the strategy. Returns 1 if a snapshot was
 * committed, 0 if the save was refused (snapshot left untouched). */
static int checkpoint(void)
{
	Arena *scratch = (Arena *)g_held->scratch;
	int t;

	switch (g_cfg.capture_mode) {
	case CAP_BARRIER:
		quiesce();
		commit_from(g_arena);          /* consistent by construction */
		release_workers();
		return 1;

	case CAP_RACE:
		commit_from(g_arena);          /* copy while writers run - may be torn */
		return 1;

	case CAP_SUSPEND:
		suspend_all_workers();
		commit_from(g_arena);          /* frozen mid-update - may be torn */
		resume_all_workers();
		return 1;

	case CAP_SETTLE:
		/* only copy when no writer is caught in the known hot region */
		for (t = 0; t < g_cap_tries; t++) {
			suspend_all_workers();
			if (!any_worker_in_fill()) { commit_from(g_arena); resume_all_workers(); return 1; }
			resume_all_workers();
			g_held->cap_retries++;
			Sleep(0);
		}
		return 0; /* refused: never settled */

	case CAP_VERIFY:
		/* copy to scratch, resume immediately, then VERIFY the copy; commit
		 * only a copy proven consistent, else retry; refuse if none is. */
		for (t = 0; t < g_cap_tries; t++) {
			suspend_all_workers();
			memcpy(scratch, g_arena, ARENA_BYTES);
			resume_all_workers();
			if (capture_is_clean(scratch)) { commit_from(scratch); return 1; }
			g_held->cap_retries++;
			Sleep(0);
		}
		return 0; /* refused: could not get a clean shot */
	}
	return 0;
}

static int rewind_to_snapshot(void)
{
	Node saved_b; int have_b = 0;
	if (!g_held->have_snap) return -1;
	quiesce();
	if (g_cfg.classB_exclude && g_held->classB_ptr) { saved_b = *g_held->classB_ptr; have_b = 1; }
	InterlockedExchange(&g_rewinding, 1);
	memcpy(g_arena, g_held->snapshot, ARENA_BYTES);
	if (have_b) *g_held->classB_ptr = saved_b;
	InterlockedExchange(&g_rewinding, 0);
	g_held->restores++;
	if (g_cfg.classA_active && g_cfg.classA_replay)
		g_held->present_tok[g_held->classA_saved_idx] = g_held->classA_saved_tok;
	{
		long hw = InterlockedCompareExchange(&g_held->hw_cursor, 0, 0);
		long wr = (long)g_arena->write_cursor;
		long gap = hw - wr; if (gap < 0) gap += RING;
		if (gap != 0) { if (g_cfg.cursor_reseat) g_arena->write_cursor = (unsigned)hw; else g_held->bad_cursor++; }
	}
	/* workers stay parked; caller runs the oracles then releases */
	return 0;
}
static void park(int ms) { quiesce(); Sleep(ms); release_workers(); }

/* ------------------------------------------------------------ oracles */

static unsigned verify_identity(void) { return identity_bad_buf(g_arena); }
static int verify_determinism(void)
{
	unsigned long long tmp = g_arena->rng; int i;
	if (g_arena->clock != g_held->saved_clock) return 1;
	if (g_arena->rng != g_held->saved_rng) return 2;
	for (i = 0; i < DET_DRAW; i++) { tmp = xorshift64(tmp); if ((unsigned)tmp != g_held->exp_draw[i]) return 3; }
	return 0;
}
static int verify_classA(void)
{ Node *n = g_held->classA_ptr; if (!g_cfg.classA_active || !n) return 0; return (g_held->present_tok[n->pindex] != n->expect_tok) ? 1 : 0; }
static int verify_classB(void)
{ Node *n = g_held->classB_ptr; if (!g_cfg.classB_active || !n) return 0; return (n->payload != g_held->classB_expect) ? 1 : 0; }

/* ------------------------------------------------------------ one cycle */

static unsigned run_cycle(void)
{
	unsigned fail = 0; int det, hv, ca, cb, committed; unsigned id;

	g_arena->clock++;
	g_arena->rng = xorshift64(g_arena->rng);
	g_arena->write_cursor = (g_arena->write_cursor + 128) % RING;

	committed = checkpoint();
	if (committed) g_held->saves_committed++; else g_held->saves_refused++;

	Sleep(2);
	g_arena->clock += 2;
	g_arena->rng = xorshift64(g_arena->rng);
	if (g_cfg.classA_active && g_held->classA_ptr) {
		unsigned idx = g_held->classA_ptr->pindex;
		g_held->present_tok[idx] = g_held->classA_ptr->expect_tok + 0x1000u + g_held->cycles;
	}
	if (g_cfg.classB_active && g_held->classB_ptr) {
		EnterCriticalSection(&g_arena_cs);
		g_held->classB_ptr->payload += 0x777u;
		g_held->classB_ptr->answer = FN_TABLE[g_held->classB_ptr->fn_idx](
			g_held->classB_ptr->serial, g_held->classB_ptr->payload);
		g_held->classB_ptr->checksum = node_checksum(g_held->classB_ptr);
		g_held->classB_expect = g_held->classB_ptr->payload;
		LeaveCriticalSection(&g_arena_cs);
	}

	rewind_to_snapshot(); /* to the current committed snapshot; workers parked */
	id  = verify_identity();
	det = verify_determinism();
	hv  = heap_validate();
	ca  = verify_classA();
	cb  = verify_classB();
	release_workers();

	if (id)  { g_held->bad_identity++;    fail++; }
	if (det) { g_held->bad_determinism++; fail++; }
	if (hv)  { g_held->bad_heap++;        fail++; }
	if (ca)  { g_held->bad_classA++;      fail++; }
	if (cb)  { g_held->bad_classB++;      fail++; }

	if (fail && committed) g_held->poisoned++; /* a committed save that restored wrong */
	g_held->cycles++;
	return fail;
}

/* ------------------------------------------------------------ setup */

static void world_init(void)
{
	unsigned i; Node *a, *b;
	g_arena->magic = NODE_MAGIC; g_arena->clock = 0;
	g_arena->rng = 0x0123456789ABCDEFull; g_arena->write_cursor = 0;
	memset(g_arena->worker_ops, 0, sizeof(g_arena->worker_ops));
	alloc_init();
	for (i = 0; i < NPT; i++) g_held->present_tok[i] = 0;
	for (i = 0; i < NODE_CAP / 2; i++) { Node *n = node_alloc_locked(); if (!n) break; node_fill(n, i * 7u + 1u); }
	a = node_alloc_locked(); node_fill(a, 0xA0000001u);
	g_held->classA_ptr = a; g_held->present_tok[a->pindex] = a->expect_tok;
	b = node_alloc_locked(); node_fill(b, 0xB0000001u);
	g_held->classB_ptr = b; g_held->classB_expect = b->payload;
}

static Config cfg_all_on(void)
{
	Config c; memset(&c, 0, sizeof(c));
	c.capture_mode = CAP_BARRIER; c.classA_replay = 1; c.classB_exclude = 1; c.cursor_reseat = 1;
	return c;
}

/* Rebuild a clean world AND take a guaranteed-consistent baseline snapshot, so
 * there is always a good save point to fall back to. A pitfall's damage is not
 * self-healing, so each experiment starts fresh. */
static void heal_world(void)
{
	quiesce();
	world_init();
	commit_from(g_arena);   /* baseline: consistent under the barrier */
	release_workers();
}

static void zero_scoreboard(void)
{
	g_held->bad_identity = g_held->bad_determinism = g_held->bad_heap = 0;
	g_held->bad_classA = g_held->bad_cursor = 0;
	InterlockedExchange((volatile LONG *)&g_held->bad_classB, 0);
	g_held->saves_committed = g_held->saves_refused = g_held->poisoned = 0;
	g_held->cap_retries = 0;
}

/* ---------------------------------------------------------- runs */

typedef struct { unsigned iters, id, ca, cb, cursor; } Score;
static Score run_scenario(Config cfg, int n)
{
	Score s; int i;
	g_cfg = cfg; heal_world(); zero_scoreboard(); memset(&s, 0, sizeof(s));
	for (i = 0; i < n && !g_stop; i++) run_cycle();
	s.iters = (unsigned)n; s.id = g_held->bad_identity; s.ca = g_held->bad_classA;
	s.cb = g_held->bad_classB; s.cursor = g_held->bad_cursor;
	return s;
}
static void row(const char *name, unsigned off_fail, unsigned off_n, unsigned on_fail, unsigned on_n)
{
	printf("  %-26s | pitfall %4u/%-4u %-9s | mitigated %3u/%-4u %s\n",
	       name, off_fail, off_n, off_fail ? "DETECTED" : "not seen",
	       on_fail, on_n, on_fail ? "LEAK" : "clean");
}

typedef struct { unsigned committed, refused, poisoned, retries, iters; } Cap;
static Cap measure_capture(int mode, int n)
{
	Config c = cfg_all_on(); Cap r; int i;
	c.capture_mode = mode;
	g_cfg = c; heal_world(); zero_scoreboard();
	for (i = 0; i < n && !g_stop; i++) run_cycle();
	r.committed = g_held->saves_committed; r.refused = g_held->saves_refused;
	r.poisoned = g_held->poisoned; r.retries = g_held->cap_retries; r.iters = (unsigned)n;
	return r;
}

int main(int argc, char **argv)
{
	int soak = argc > 1 ? atoi(argv[1]) : 4000;
	int i; void *base; int per = 200;

	g_workers = argc > 2 ? atoi(argv[2]) : 6;
	if (g_workers < 1) g_workers = 1; if (g_workers > MAX_WORKERS) g_workers = MAX_WORKERS;

	printf("rr_mech_harness: %d-bit, %d workers, %d iters/arm, %d soak cycles, fill_spin=%d\n",
	       (int)(sizeof(void *) * 8), g_workers, per, soak, FILL_SPIN);

	InitializeCriticalSection(&g_arena_cs);
	g_held = (Held *)VirtualAlloc(NULL, sizeof(Held), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!g_held) { printf("held alloc failed\n"); return 2; }
	memset(g_held, 0, sizeof(Held));
	g_held->snapshot = (unsigned char *)VirtualAlloc(NULL, ARENA_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	g_held->scratch  = (unsigned char *)VirtualAlloc(NULL, ARENA_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!g_held->snapshot || !g_held->scratch) { printf("snapshot alloc failed\n"); return 2; }
	base = VirtualAlloc(ARENA_WANT_BASE, ARENA_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!base) base = VirtualAlloc(NULL, ARENA_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!base) { printf("arena alloc failed\n"); return 2; }
	g_arena = (Arena *)base;
	printf("arena at %p (%s)\n", (void *)g_arena, base == ARENA_WANT_BASE ? "pinned" : "floating");

	g_cfg = cfg_all_on();
	world_init();
	g_audio_thread = CreateThread(NULL, 0, audio_main, NULL, 0, NULL);
	g_sys_thread = CreateThread(NULL, 0, sys_main, NULL, 0, NULL);
	for (i = 0; i < g_workers; i++) g_worker[i] = CreateThread(NULL, 0, worker_main, (LPVOID)(intptr_t)i, 0, &g_worker_tid[i]);
	Sleep(50);

	/* ---- negative controls ---- */
	printf("\n===== negative controls (each MUST fire) =====\n");
	{
		Config c; int tries, caught = 0; unsigned before;
		memset(&c, 0, sizeof(c)); c.capture_mode = CAP_RACE; g_cfg = c; heal_world(); zero_scoreboard();
		for (tries = 0; tries < 300 && !caught; tries++) { before = g_held->bad_identity; run_cycle(); if (g_held->bad_identity > before) caught = 1; }
		printf("  C1 mid-mutation capture : %s\n", caught ? "DETECTED (torn node seen)" : "not seen");
		g_cfg = cfg_all_on(); heal_world();
		{ Node *n = g_held->classA_ptr; unsigned sa = n->answer; n->answer ^= 1u;
		  printf("  C2 corrupt-node detect  : %s\n", verify_identity() ? "DETECTED" : "MISSED"); n->answer = sa; }
		{ unsigned long long s = g_arena->rng; commit_from(g_arena); g_arena->rng ^= 0x1u;
		  printf("  C3 determinism-break    : %s\n", verify_determinism() ? "DETECTED" : "MISSED"); g_arena->rng = s; }
	}

	/* ---- restore-side boundary map (capture = BARRIER) ---- */
	printf("\n===== restore-side boundary map =====\n");
	printf("  scenario                   | mitigation OFF (provoke)     | mitigation ON\n");
	printf("  ---------------------------+------------------------------+----------------------\n");
	{
		Config off, on; Score so, sn;
		off = cfg_all_on(); off.classA_active = 1; off.classA_replay = 0;
		on  = cfg_all_on(); on.classA_active = 1;  on.classA_replay = 1;
		so = run_scenario(off, per); sn = run_scenario(on, per);
		row("Class A outward straddle", so.ca, so.iters, sn.ca, sn.iters);

		off = cfg_all_on(); off.classB_active = 1; off.classB_exclude = 0;
		on  = cfg_all_on(); on.classB_active = 1;  on.classB_exclude = 1;
		so = run_scenario(off, per); sn = run_scenario(on, per);
		row("Class B inward straddle", so.cb, so.iters, sn.cb, sn.iters);

		off = cfg_all_on(); off.cursor_reseat = 0;
		on  = cfg_all_on(); on.cursor_reseat = 1;
		so = run_scenario(off, per); sn = run_scenario(on, per);
		row("Audio cursor split", so.cursor, so.iters, sn.cursor, sn.iters);
	}

	/* ---- capture strategy: which one never poisons a slot? ---- */
	printf("\n===== capture strategy: poisoned is the number that matters =====\n");
	printf("  strategy         | committed | refused | retries | poisoned | verdict\n");
	printf("  -----------------+-----------+---------+---------+----------+--------\n");
	{
		int modes[5] = { CAP_RACE, CAP_SUSPEND, CAP_SETTLE, CAP_VERIFY, CAP_BARRIER };
		int m;
		for (m = 0; m < 5; m++) {
			Cap r = measure_capture(modes[m], per);
			printf("  %-16s | %9u | %7u | %7u | %8u | %s\n",
			       CAP_NAME[modes[m]], r.committed, r.refused, r.retries, r.poisoned,
			       r.poisoned ? "POISONS" : "safe");
		}
		/* Force the fail-closed path: a tiny retry budget makes VERIFY refuse
		 * often under this contention. The point is that a refusal is NOT a
		 * corruption - poisoned must still be 0. */
		g_cap_tries = 2;
		{
			Cap r = measure_capture(CAP_VERIFY, per);
			printf("  %-16s | %9u | %7u | %7u | %8u | %s\n",
			       "VERIFY cap=2", r.committed, r.refused, r.retries, r.poisoned,
			       r.poisoned ? "POISONS" : "safe (refused, not corrupt)");
		}
		g_cap_tries = CAP_TRIES_DEF;
	}

	/* ---- soak with the proposed fix (SUSPEND+VERIFY), stressors on ---- */
	printf("\n===== soak: SUSPEND+VERIFY (portable fix), Class A+B active =====\n");
	{
		Config on = cfg_all_on(); on.capture_mode = CAP_VERIFY; on.classA_active = 1; on.classB_active = 1;
		unsigned fails = 0; int c; unsigned r0;
		g_cfg = on; heal_world(); zero_scoreboard(); r0 = g_held->restores;
		for (c = 0; c < soak && !g_stop; c++) {
			fails += run_cycle();
			if ((c % 500) == 0)
				printf("  soak %5d/%d: committed=%u refused=%u poisoned=%u\n",
				       c, soak, g_held->saves_committed, g_held->saves_refused, g_held->poisoned);
		}
		park(3);
		printf("  soak done: %d cycles, %u restores, committed=%u refused=%u retries=%u poisoned=%u\n",
		       soak, g_held->restores - r0, g_held->saves_committed, g_held->saves_refused,
		       g_held->cap_retries, g_held->poisoned);
		printf("  restore breakdown: id=%u det=%u heap=%u A=%u B=%u cursor=%u\n",
		       g_held->bad_identity, g_held->bad_determinism, g_held->bad_heap,
		       g_held->bad_classA, g_held->bad_classB, g_held->bad_cursor);
	}

	InterlockedExchange(&g_stop, 1);
	for (i = 0; i < g_workers; i++) if (g_worker[i]) WaitForSingleObject(g_worker[i], 1000);
	if (g_sys_thread) WaitForSingleObject(g_sys_thread, 1000);
	if (g_audio_thread) WaitForSingleObject(g_audio_thread, 1000);
	DeleteCriticalSection(&g_arena_cs);

	{
		int safe = (g_held->poisoned == 0);
		printf("\n===== verdict =====\n");
		printf("%s: SUSPEND+VERIFY committed only snapshots proven consistent and refused the rest; "
		       "no slot was poisoned across the soak.\n", safe ? "PASS" : "FAIL");
		printf("A refused save is a retry, not a corruption. That is the bound we can live inside.\n");
		return safe ? 0 : 1;
	}
}
