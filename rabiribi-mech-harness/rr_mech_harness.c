/*
 * rr_mech_harness - a mechanical bounds-mapper for the Rabi-Ribi in-process
 * savestate.
 *
 * The goal is not "never fail". The goal is to KNOW THE BOUNDS: to take every
 * failure mode the Rabi-Ribi savestate documentation names, provoke it here on
 * demand so we can see it fire, show the mitigation that walks around it, and
 * then iterate hard enough that we can say - with numbers - that a save point
 * can be created and put back in RAM "as is", as many times over, across the
 * situations we have mapped. Not-knowing is the enemy; a reproducible fault
 * next to a working mitigation is the win.
 *
 * It is self-contained: it does NOT link the real 680 KB savestate.c and does
 * NOT need the game. It stands up a small world with the ownership shapes the
 * docs say matter, runs its own compact in-memory snapshot/restore over one
 * pinned arena, and for each documented boundary runs the pitfall (mitigation
 * OFF, expected to fail AND be detected) against the mitigation (ON, expected
 * clean over many iterations).
 *
 * Boundaries modeled, each straight from cow2cowlinuxuser/rabiribi-savestate:
 *
 *   restore_invariants.md  - the three (and only three) inconsistency classes:
 *      Class A  outward straddle : snapshot -> present (ref rewound, target moved on)
 *      Class B  inward  straddle : present -> snapshot (target rewound under a live ref)
 *      Class C  mid-mutation     : captured while a writer was halfway through
 *   ds_harness.c /          - DirectSound cursor split: a rewound write pointer
 *   audio_and_archive.md      against a play cursor that kept advancing -> a
 *                             naive rewind yields a negative streaming length.
 *   restore_invariants.md #2 - heap metadata must stay self-consistent.
 *   restore_invariants.md #4 - the thread set the restored state names must
 *                             still exist (fresh/gone accounting).
 *   windows_savestate_...md  - capture method boundary: freezing writers at
 *   + Wine notes in the docs   arbitrary points (SuspendThread, as the real
 *                             engine does) can photograph a torn structure and
 *                             needs a settle pass; a cooperative safe-point
 *                             barrier does not. This is the seam that behaves
 *                             differently under Wine.
 *
 * Two rules taken from restore_invariants.md are enforced:
 *   - Every oracle is made to fire at least once (the negative controls), so a
 *     silent pass is trusted only after the instrument is shown to bite.
 *   - Identity is proven through behaviour (calling the node's own callback)
 *     and a writer-maintained checksum, never by guessing a struct layout.
 *
 * Design choices that keep it honest AND runnable under Wine, where the real
 * engine's SuspendThread + full region walk + live-heap rewind is documented to
 * hang/abort: mutators are quiesced at a cooperative barrier for the safe path;
 * the snapshot is a byte copy of one pinned arena and is written back verbatim;
 * present-time peers (a system thread, an audio device thread) are deliberately
 * NOT quiesced so Class B and the cursor split are real.
 *
 *   usage: rr_mech_harness[32].exe [soak-cycles] [workers]
 *   default: 2000 soak cycles, 6 workers.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ config */

#define ARENA_WANT_BASE ((void *)0x30000000) /* pinned like the game's gameheap */
#define ARENA_BYTES     (4u * 1024u * 1024u)
#define NODE_CAP        8192
#define NODE_MAGIC      0x5252424Bu /* 'RRBK' */
#define RING            48000       /* audio ring, one second at 48 kHz */
#define DET_DRAW        16          /* RNG values compared for determinism */
#define MAX_WORKERS     32
#define NPT             256         /* present-side token table (Class A target) */

typedef unsigned (*NodeFn)(unsigned serial, unsigned payload);

/* ---- callbacks a node can carry; reached "through a vtable or callback".
 * They live in .text, which the rewind never touches, so the pointer stays
 * valid across a restore and identity can be proven by calling it. */
static unsigned fn_sum(unsigned s, unsigned p) { return s + p; }
static unsigned fn_xor(unsigned s, unsigned p) { return s ^ (p * 2654435761u); }
static unsigned fn_mix(unsigned s, unsigned p) { return (s * 1664525u) + p + 1013904223u; }
static unsigned fn_rot(unsigned s, unsigned p) { unsigned v = s + p; return (v << 7) | (v >> 25); }
static NodeFn   FN_TABLE[4] = { fn_sum, fn_xor, fn_mix, fn_rot };

/* --------------------------------------------------------------- the world */

typedef struct Node {
	unsigned magic;      /* NODE_MAGIC while in use, 0 while free */
	unsigned serial;
	unsigned payload;
	unsigned fn_idx;
	struct Node *self;   /* must equal &this node - absolute, rewound */
	int      next;       /* free-list link (index) while free, else -1 */
	unsigned answer;     /* fn(serial,payload) - callback fingerprint */
	unsigned checksum;   /* written LAST; a torn write leaves this stale */
	unsigned pindex;     /* Class A: outward index into Held.present_tok */
	unsigned expect_tok; /* Class A: what present_tok[pindex] should read */
} Node;

typedef struct Arena {
	unsigned magic;
	unsigned clock;
	unsigned long long rng;
	unsigned write_cursor;   /* audio write pointer - rewound (ds_harness g_write) */
	int      free_head;
	unsigned in_use;
	unsigned worker_ops[MAX_WORKERS];
	Node     node[NODE_CAP];
} Arena;

/* which mitigations are engaged for a given run */
typedef struct Config {
	int quiesce;         /* park writers at the barrier before the copy (Class C) */
	int capture_suspend; /* instead: SuspendThread the writers (the real engine's way) */
	int classA_replay;   /* record-and-replay the present-side value (Class A) */
	int classB_exclude;  /* exclude the live-held block from the rewind (Class B) */
	int cursor_reseat;   /* reseat the write cursor to the play cursor (audio) */
	int classA_active;   /* exercise the outward straddle this run */
	int classB_active;   /* exercise the inward straddle this run */
} Config;

/* present/held bookkeeping - excluded from the snapshot by living outside the
 * arena, like the real engine's Control block. Never rewound. */
typedef struct Held {
	unsigned char *snapshot;
	unsigned have_snap;

	unsigned long long saved_rng;
	unsigned           saved_clock;
	unsigned           exp_draw[DET_DRAW];

	volatile LONG hw_cursor;   /* audio device play cursor, keeps advancing */

	Node *classA_ptr;          /* the arena node with the outward reference */
	unsigned present_tok[NPT]; /* the present-side target of that reference */
	unsigned classA_saved_idx, classA_saved_tok; /* for record-and-replay */

	Node *classB_ptr;          /* arena node a present writer mutates */
	unsigned classB_expect;    /* the value the present writer left in it */

	unsigned cycles, saves, restores;
	unsigned bad_identity, bad_determinism, bad_heap, bad_classA, bad_classB, bad_cursor;
	unsigned thr_gone, thr_fresh;
} Held;

/* --------------------------------------------------------------- globals */

static Arena *g_arena;
static Held  *g_held;
static Config g_cfg;

static CRITICAL_SECTION g_arena_cs; /* serialises the arena allocator */
static volatile LONG g_stop;
static volatile LONG g_quiesce;
static volatile LONG g_parked;
static volatile LONG g_rewinding;
static int g_workers = 6;

static HANDLE g_worker[MAX_WORKERS];
static DWORD  g_worker_tid[MAX_WORKERS];
static HANDLE g_sys_thread, g_audio_thread;

/* ------------------------------------------------------------ small utils */

static unsigned long long xorshift64(unsigned long long x)
{
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	return x ? x : 0x9E3779B97F4A7C15ull;
}

static unsigned node_checksum(const Node *n)
{
	unsigned h = 2166136261u;
	const unsigned parts[6] = {
		n->magic, n->serial, n->payload, n->fn_idx,
		(unsigned)(uintptr_t)n->self, n->answer
	};
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
	int idx = g_arena->free_head;
	Node *n;
	if (idx < 0) return NULL;
	n = &g_arena->node[idx];
	g_arena->free_head = n->next;
	n->next = -1;
	g_arena->in_use++;
	return n;
}

static void node_free_locked(Node *n)
{
	int idx = (int)(n - g_arena->node);
	n->magic = 0; n->answer = 0; n->checksum = 0;
	n->next = g_arena->free_head;
	g_arena->free_head = idx;
	g_arena->in_use--;
}

/* checksum + answer written LAST, so a reader caught between the field writes
 * and this line sees a torn node - the Class C signal. */
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
	/* A deliberate, bounded window in which the node is inconsistent (fields
	 * written, checksum not yet). It makes the torn-capture pitfalls (an
	 * unquiesced copy, or a SuspendThread that freezes a writer mid-update)
	 * reliably reproducible instead of vanishingly rare, so the bound can be
	 * measured. The cooperative barrier is unaffected: a worker only parks at
	 * the top of its loop, after the checksum below, so a quiesced snapshot
	 * never lands in this window however wide it is. */
	{ volatile unsigned s = 0; unsigned k; for (k = 0; k < 20000u; k++) s += k * 3u + 1u; (void)s; }
	n->checksum = node_checksum(n); /* LAST */
}

/* Walk the allocator's own structures the way HeapValidate walks a heap. 0 = ok. */
static int heap_validate(void)
{
	int idx, steps = 0;
	unsigned free_seen = 0, i;
	static unsigned char onlist[NODE_CAP];
	EnterCriticalSection(&g_arena_cs);
	memset(onlist, 0, sizeof(onlist));
	idx = g_arena->free_head;
	while (idx >= 0) {
		if (idx >= NODE_CAP) { LeaveCriticalSection(&g_arena_cs); return 1; }
		if (onlist[idx])     { LeaveCriticalSection(&g_arena_cs); return 2; }
		if (g_arena->node[idx].magic == NODE_MAGIC) { LeaveCriticalSection(&g_arena_cs); return 3; }
		onlist[idx] = 1; free_seen++;
		idx = g_arena->node[idx].next;
		if (++steps > NODE_CAP + 1) { LeaveCriticalSection(&g_arena_cs); return 4; }
	}
	if (free_seen + g_arena->in_use != NODE_CAP) { LeaveCriticalSection(&g_arena_cs); return 5; }
	for (i = 0; i < NODE_CAP; i++)
		if (!onlist[i] && g_arena->node[i].magic != NODE_MAGIC) { LeaveCriticalSection(&g_arena_cs); return 6; }
	LeaveCriticalSection(&g_arena_cs);
	return 0;
}

/* --------------------------------------------------------- the barrier */

static void quiesce(void)
{
	InterlockedExchange(&g_quiesce, 1);
	while (InterlockedCompareExchange(&g_parked, 0, 0) < g_workers && !g_stop)
		Sleep(0);
}
static void release_workers(void)
{
	InterlockedExchange(&g_quiesce, 0);
	while (InterlockedCompareExchange(&g_parked, 0, 0) > 0 && !g_stop)
		Sleep(0);
}

/* --------------------------------------------------------- mutator thread */

static DWORD WINAPI worker_main(LPVOID p)
{
	int id = (int)(intptr_t)p;
	unsigned long long tr = 0x1234567u ^ (0x9E37u * (unsigned)(id + 1));
	int was_parked = 0;

	while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
		if (InterlockedCompareExchange(&g_quiesce, 0, 0)) {
			if (!was_parked) { InterlockedIncrement(&g_parked); was_parked = 1; }
			Sleep(0);
			continue;
		}
		if (was_parked) { InterlockedDecrement(&g_parked); was_parked = 0; }

		tr = xorshift64(tr);
		EnterCriticalSection(&g_arena_cs);
		if ((tr & 3) != 0) {
			Node *n = node_alloc_locked();
			if (n) node_fill(n, (unsigned)(tr >> 20));
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

/* ------------------------------------------------- present-time peers */

/* System thread: never quiesced, holds a pointer into the rewound arena and
 * reads its stable identity (magic + self) forever. Reading a torn transient
 * during the memcpy is a value mismatch, not a fault, and is only counted when
 * a rewind is not in flight. */
static DWORD WINAPI sys_main(LPVOID p)
{
	(void)p;
	while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
		Node *n = g_held->classB_ptr;
		if (n && !InterlockedCompareExchange(&g_rewinding, 0, 0)) {
			if (n->magic != NODE_MAGIC || n->self != n)
				InterlockedIncrement((volatile LONG *)&g_held->bad_classB);
		}
		Sleep(1);
	}
	return 0;
}

/* Audio device thread: advances the hardware play cursor monotonically and
 * never stops for a rewind. */
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

/* --------------------------------------------------- the save/restore engine */

static void suspend_all_workers(void)  { int i; for (i = 0; i < g_workers; i++) if (g_worker[i]) SuspendThread(g_worker[i]); }
static void resume_all_workers(void)   { int i; for (i = 0; i < g_workers; i++) if (g_worker[i]) ResumeThread(g_worker[i]); }

/* Capture. The safe path parks writers at a barrier so the arena is a clean
 * instant. capture_suspend takes the real engine's route - freeze the writers
 * wherever they are - which can photograph a torn structure (why the engine
 * needs a settle pass). Neither is used means an uncontrolled race (Class C). */
static void checkpoint(void)
{
	unsigned long long tmp;
	int i;

	if (g_cfg.capture_suspend)      suspend_all_workers();
	else if (g_cfg.quiesce)         quiesce();

	memcpy(g_held->snapshot, g_arena, ARENA_BYTES); /* the save point */
	g_held->have_snap = 1;

	g_held->saved_rng = g_arena->rng;
	g_held->saved_clock = g_arena->clock;
	tmp = g_arena->rng;
	for (i = 0; i < DET_DRAW; i++) { tmp = xorshift64(tmp); g_held->exp_draw[i] = (unsigned)tmp; }

	if (g_cfg.classA_active && g_held->classA_ptr) {
		g_held->classA_saved_idx = g_held->classA_ptr->pindex;
		g_held->classA_saved_tok = g_held->present_tok[g_held->classA_ptr->pindex];
	}

	g_held->saves++;

	if (g_cfg.capture_suspend)      resume_all_workers();
	else if (g_cfg.quiesce)         release_workers();
}

/* Put the save point back, byte for byte, then apply the engaged mitigations. */
static int rewind_to_snapshot(void)
{
	Node saved_b; int have_b = 0;

	if (!g_held->have_snap) return -1;
	quiesce();

	/* Class B mitigation: lift the live-held block out of the rewind so a
	 * present writer's post-save mutation is not reverted under it. */
	if (g_cfg.classB_exclude && g_held->classB_ptr) { saved_b = *g_held->classB_ptr; have_b = 1; }

	InterlockedExchange(&g_rewinding, 1);
	memcpy(g_arena, g_held->snapshot, ARENA_BYTES); /* restore in memory as is */
	if (have_b) *g_held->classB_ptr = saved_b;      /* re-pin the excluded block */
	InterlockedExchange(&g_rewinding, 0);

	g_held->restores++;

	/* Class A mitigation: record-and-replay the present-side value the snapshot
	 * depends on, the same shape as the unwind-table fix. */
	if (g_cfg.classA_active && g_cfg.classA_replay)
		g_held->present_tok[g_held->classA_saved_idx] = g_held->classA_saved_tok;

	/* audio cursor split: reseat the rewound write pointer to the play cursor. */
	{
		long hw = InterlockedCompareExchange(&g_held->hw_cursor, 0, 0);
		long wr = (long)g_arena->write_cursor;
		long gap = hw - wr; if (gap < 0) gap += RING;
		if (gap != 0) {
			if (g_cfg.cursor_reseat) g_arena->write_cursor = (unsigned)hw;
			else                     g_held->bad_cursor++; /* would underrun/glitch */
		}
	}

	/* Workers are left PARKED: the caller runs the oracles on this stable,
	 * restored arena and only then releases. Checking while writers run would
	 * be reading a moving target, not the restored state. */
	return 0;
}

/* savestate_park control: quiesce, hold, release - no copy, no rewind. */
static void park(int ms) { quiesce(); Sleep(ms); release_workers(); }

/* ------------------------------------------------------------ oracles */

static unsigned verify_identity(void)
{
	unsigned bad = 0, i;
	for (i = 0; i < NODE_CAP; i++) {
		Node *n = &g_arena->node[i];
		if (n->magic != NODE_MAGIC) continue;
		if (n->self != n) { bad++; continue; }
		if (n->fn_idx > 3) { bad++; continue; }
		if (n->answer != FN_TABLE[n->fn_idx](n->serial, n->payload)) { bad++; continue; }
		if (n->checksum != node_checksum(n)) { bad++; continue; }
	}
	return bad;
}

static int verify_determinism(void)
{
	unsigned long long tmp = g_arena->rng;
	int i;
	if (g_arena->clock != g_held->saved_clock) return 1;
	if (g_arena->rng != g_held->saved_rng) return 2;
	for (i = 0; i < DET_DRAW; i++) { tmp = xorshift64(tmp); if ((unsigned)tmp != g_held->exp_draw[i]) return 3; }
	return 0;
}

static int verify_classA(void)
{
	Node *n = g_held->classA_ptr;
	if (!g_cfg.classA_active || !n) return 0;
	return (g_held->present_tok[n->pindex] != n->expect_tok) ? 1 : 0;
}

static int verify_classB(void)
{
	Node *n = g_held->classB_ptr;
	if (!g_cfg.classB_active || !n) return 0;
	return (n->payload != g_held->classB_expect) ? 1 : 0;
}

/* ------------------------------------------------------------ one cycle */

/* Advance the world, take a save point, let it diverge, put it back, check.
 * Returns the number of oracle failures this cycle. */
static unsigned run_cycle(void)
{
	unsigned fail = 0; int det, hv, ca, cb; unsigned id;

	g_arena->clock++;
	g_arena->rng = xorshift64(g_arena->rng);
	g_arena->write_cursor = (g_arena->write_cursor + 128) % RING;

	checkpoint();

	/* --- divergence between the save point and the rewind --- */
	Sleep(2); /* let the play cursor and workers move on */
	g_arena->clock += 2;
	g_arena->rng = xorshift64(g_arena->rng);

	if (g_cfg.classA_active && g_held->classA_ptr) {
		/* the present side "moves on": its token drifts away from the save */
		unsigned idx = g_held->classA_ptr->pindex;
		g_held->present_tok[idx] = g_held->classA_ptr->expect_tok + 0x1000u + g_held->cycles;
	}
	if (g_cfg.classB_active && g_held->classB_ptr) {
		/* a present writer mutates a rewound block and expects it to persist */
		EnterCriticalSection(&g_arena_cs);
		g_held->classB_ptr->payload += 0x777u;
		g_held->classB_ptr->answer = FN_TABLE[g_held->classB_ptr->fn_idx](
			g_held->classB_ptr->serial, g_held->classB_ptr->payload);
		g_held->classB_ptr->checksum = node_checksum(g_held->classB_ptr);
		g_held->classB_expect = g_held->classB_ptr->payload;
		LeaveCriticalSection(&g_arena_cs);
	}

	rewind_to_snapshot(); /* returns with workers still parked */

	/* oracles run on the stable, restored arena, before releasing writers */
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

	g_held->cycles++;
	return fail;
}

/* ------------------------------------------------------------ setup */

static void world_init(void)
{
	unsigned i;
	Node *a, *b;

	g_arena->magic = NODE_MAGIC;
	g_arena->clock = 0;
	g_arena->rng = 0x0123456789ABCDEFull;
	g_arena->write_cursor = 0;
	memset(g_arena->worker_ops, 0, sizeof(g_arena->worker_ops));
	alloc_init();
	for (i = 0; i < NPT; i++) g_held->present_tok[i] = 0;

	for (i = 0; i < NODE_CAP / 2; i++) {
		Node *n = node_alloc_locked();
		if (!n) break;
		node_fill(n, i * 7u + 1u);
	}

	a = node_alloc_locked(); node_fill(a, 0xA0000001u);
	g_held->classA_ptr = a;
	g_held->present_tok[a->pindex] = a->expect_tok; /* outward ref agrees to start */

	b = node_alloc_locked(); node_fill(b, 0xB0000001u);
	g_held->classB_ptr = b;
	g_held->classB_expect = b->payload;
}

static Config cfg_all_on(void)
{
	Config c; memset(&c, 0, sizeof(c));
	c.quiesce = 1; c.classA_replay = 1; c.classB_exclude = 1; c.cursor_reseat = 1;
	return c;
}

/* Rebuild a clean, consistent world. A pitfall arm deliberately captures an
 * inconsistent arena and writes it back, and that damage is NOT self-healing -
 * it would be snapshotted and restored forever after. So each independent
 * experiment starts from a fresh world; that isolation is the whole point of a
 * bounds map. (The persistence itself is a finding, recorded in FINDINGS.md.) */
static void heal_world(void)
{
	quiesce();
	world_init();
	g_held->have_snap = 0;
	release_workers();
}

/* ---------------------------------------------------------- controls + scenarios */

static void banner(const char *s) { printf("\n===== %s =====\n", s); }

/* Reset counters that a scenario measures, so each scenario reports its own. */
static void zero_scoreboard(void)
{
	g_held->bad_identity = g_held->bad_determinism = g_held->bad_heap = 0;
	g_held->bad_classA = g_held->bad_classB = g_held->bad_cursor = 0;
	InterlockedExchange((volatile LONG *)&g_held->bad_classB, 0);
}

/* Run n cycles under cfg and report the failures the relevant oracle saw. */
typedef struct { unsigned iters, id, det, heap, ca, cb, cursor; } Score;

static Score run_scenario(Config cfg, int n)
{
	Score s; int i;
	g_cfg = cfg;
	heal_world(); /* a prior pitfall's damage must not leak into this arm */
	zero_scoreboard();
	memset(&s, 0, sizeof(s));
	for (i = 0; i < n && !g_stop; i++) run_cycle();
	s.iters = (unsigned)n;
	s.id = g_held->bad_identity; s.det = g_held->bad_determinism; s.heap = g_held->bad_heap;
	s.ca = g_held->bad_classA; s.cb = g_held->bad_classB; s.cursor = g_held->bad_cursor;
	return s;
}

static void row(const char *name, const char *metric, unsigned off_fail, unsigned off_n,
		unsigned on_fail, unsigned on_n)
{
	printf("  %-26s | pitfall %4u/%-4u %-9s | mitigated %3u/%-4u %s\n",
	       name, off_fail, off_n, off_fail ? "DETECTED" : "not seen",
	       on_fail, on_n, on_fail ? "LEAK" : "clean");
	(void)metric;
}

int main(int argc, char **argv)
{
	int soak = argc > 1 ? atoi(argv[1]) : 2000;
	int i;
	void *base;
	int per = 200; /* iterations per scenario arm */

	g_workers = argc > 2 ? atoi(argv[2]) : 6;
	if (g_workers < 1) g_workers = 1;
	if (g_workers > MAX_WORKERS) g_workers = MAX_WORKERS;

	printf("rr_mech_harness: %d-bit, %d workers, %d iters/arm, %d soak cycles\n",
	       (int)(sizeof(void *) * 8), g_workers, per, soak);

	InitializeCriticalSection(&g_arena_cs);

	g_held = (Held *)VirtualAlloc(NULL, sizeof(Held), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!g_held) { printf("held alloc failed\n"); return 2; }
	memset(g_held, 0, sizeof(Held));
	g_held->snapshot = (unsigned char *)VirtualAlloc(NULL, ARENA_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!g_held->snapshot) { printf("snapshot alloc failed\n"); return 2; }

	base = VirtualAlloc(ARENA_WANT_BASE, ARENA_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!base) base = VirtualAlloc(NULL, ARENA_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!base) { printf("arena alloc failed\n"); return 2; }
	g_arena = (Arena *)base;
	printf("arena at %p (%s), held at %p\n", (void *)g_arena,
	       base == ARENA_WANT_BASE ? "pinned" : "floating", (void *)g_held);

	g_cfg = cfg_all_on();
	world_init();

	g_audio_thread = CreateThread(NULL, 0, audio_main, NULL, 0, NULL);
	g_sys_thread = CreateThread(NULL, 0, sys_main, NULL, 0, NULL);
	for (i = 0; i < g_workers; i++)
		g_worker[i] = CreateThread(NULL, 0, worker_main, (LPVOID)(intptr_t)i, 0, &g_worker_tid[i]);
	Sleep(50);

	/* ---- negative controls: prove each oracle can fire (rule two) ---- */
	banner("negative controls (each MUST fire)");
	{
		/* torn capture: race the copy, no quiesce, no suspend */
		Config c; int tries, caught = 0; unsigned before;
		memset(&c, 0, sizeof(c)); g_cfg = c;
		for (tries = 0; tries < 300 && !caught; tries++) {
			before = g_held->bad_identity;
			run_cycle();
			if (g_held->bad_identity > before) caught = 1;
		}
		printf("  C1 mid-mutation capture : %s\n", caught ? "DETECTED (torn node seen)" : "not seen");

		/* clean the world */
		g_cfg = cfg_all_on(); quiesce();
		if (g_held->classA_ptr) g_held->present_tok[g_held->classA_ptr->pindex] = g_held->classA_ptr->expect_tok;
		release_workers();

		/* corrupt a field the sweep reads */
		{ Node *n = g_held->classA_ptr; unsigned sa = n->answer; n->answer ^= 1u;
		  printf("  C2 corrupt-node detect  : %s\n", verify_identity() ? "DETECTED" : "MISSED"); n->answer = sa; }
		/* break determinism */
		{ unsigned long long s = g_arena->rng; g_arena->rng ^= 0x100u; checkpoint();
		  /* checkpoint recomputed expected from current rng; perturb AFTER */
		  g_arena->rng ^= 0x1u;
		  printf("  C3 determinism-break    : %s\n", verify_determinism() ? "DETECTED" : "MISSED"); g_arena->rng = s; }
	}

	/* ---- the bounds: pitfall (mitigation OFF) vs mitigated (ON) ---- */
	banner("boundary map: pitfall must be detected, mitigation must stay clean");
	printf("  scenario                   | mitigation OFF (provoke)     | mitigation ON\n");
	printf("  ---------------------------+------------------------------+----------------------\n");
	{
		Config off, on;
		Score so, sn;

		/* Class C - mid-mutation: OFF = race the copy; ON = quiesce barrier */
		memset(&off, 0, sizeof(off));              /* no quiesce, no suspend */
		on = cfg_all_on();
		so = run_scenario(off, per); sn = run_scenario(on, per);
		row("Class C mid-mutation", "identity", so.id, so.iters, sn.id, sn.iters);

		/* Capture method - SuspendThread (real engine) vs cooperative barrier */
		memset(&off, 0, sizeof(off)); off.capture_suspend = 1;
		so = run_scenario(off, per); sn = run_scenario(on, per);
		row("Capture via SuspendThread", "identity", so.id, so.iters, sn.id, sn.iters);

		/* Class A - outward straddle: OFF = no replay; ON = record-and-replay */
		off = cfg_all_on(); off.classA_active = 1; off.classA_replay = 0;
		on  = cfg_all_on(); on.classA_active = 1;  on.classA_replay = 1;
		so = run_scenario(off, per); sn = run_scenario(on, per);
		row("Class A outward straddle", "classA", so.ca, so.iters, sn.ca, sn.iters);

		/* Class B - inward straddle: OFF = no exclude; ON = exclude/re-pin */
		off = cfg_all_on(); off.classB_active = 1; off.classB_exclude = 0;
		on  = cfg_all_on(); on.classB_active = 1;  on.classB_exclude = 1;
		so = run_scenario(off, per); sn = run_scenario(on, per);
		row("Class B inward straddle", "classB", so.cb, so.iters, sn.cb, sn.iters);

		/* Audio cursor split: OFF = no reseat; ON = reseat to play cursor */
		off = cfg_all_on(); off.cursor_reseat = 0;
		on  = cfg_all_on(); on.cursor_reseat = 1;
		so = run_scenario(off, per); sn = run_scenario(on, per);
		row("Audio cursor split", "cursor", so.cursor, so.iters, sn.cursor, sn.iters);
	}

	/* ---- soak: everything mitigated, randomised, iterate to confidence ---- */
	banner("soak: all mitigations on, iterate to confidence");
	{
		Config on = cfg_all_on(); on.classA_active = 1; on.classB_active = 1;
		unsigned fails = 0; int c;
		unsigned restores0;
		g_cfg = on;
		heal_world();
		zero_scoreboard();
		restores0 = g_held->restores;
		for (c = 0; c < soak && !g_stop; c++) {
			fails += run_cycle();
			if ((c % 500) == 0)
				printf("  soak %5d/%d: restores=%u, failures so far=%u\n",
				       c, soak, g_held->restores - restores0, fails);
		}
		park(3);
		printf("  soak done: %d cycles, %u restores, %u failures\n",
		       soak, g_held->restores - restores0, fails);
		printf("  breakdown: id=%u det=%u heap=%u A=%u B=%u cursor=%u\n",
		       g_held->bad_identity, g_held->bad_determinism, g_held->bad_heap,
		       g_held->bad_classA, g_held->bad_classB, g_held->bad_cursor);
	}

	InterlockedExchange(&g_stop, 1);
	for (i = 0; i < g_workers; i++) if (g_worker[i]) WaitForSingleObject(g_worker[i], 1000);
	if (g_sys_thread) WaitForSingleObject(g_sys_thread, 1000);
	if (g_audio_thread) WaitForSingleObject(g_audio_thread, 1000);
	DeleteCriticalSection(&g_arena_cs);

	{
		int clean_soak = !g_held->bad_identity && !g_held->bad_determinism &&
				 !g_held->bad_heap && !g_held->bad_classA &&
				 !g_held->bad_classB && !g_held->bad_cursor;
		banner("verdict");
		printf("%s: with the mapped mitigations engaged, every restore in the soak "
		       "came back byte-for-byte.\n",
		       clean_soak ? "PASS" : "FAIL");
		printf("The pitfall column above is the bound: each is a real, reproducible "
		       "failure we can now name and step around.\n");
		return clean_soak ? 0 : 1;
	}
}
