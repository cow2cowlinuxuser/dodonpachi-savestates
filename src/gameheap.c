/* Giving the game a heap of its own.
 *
 * Every ownership heuristic in savestate.c exists for one reason: Rabi-Ribi
 * links ucrtbase, and ucrtbase does not create a heap. It calls GetProcessHeap
 * and allocates out of the same arena ntdll, GDI and combase are using. So the
 * game's state and Windows' state are interleaved block by block in one heap,
 * and every restore has to guess which is which. BLKOWNER, HELDVETO, VTABVETO,
 * the reachability closure and the recycled-block hazard are all consequences
 * of that single fact.
 *
 * DoDonPachi Resurrection is the control group, and it says what the other side
 * looks like. It links MSVCR100, which calls HeapCreate, so the game's runtime
 * heap has no other tenant. Its restore reported 208 regions, none by block, and
 * captured 1076 MB of 1089 - 98.8% of the process, with no ownership vote taken
 * anywhere. Rabi-Ribi's captures 59% and negotiates for the rest.
 *
 * We cannot change which runtime the game links. We can change where its
 * allocations land: create a heap, and redirect the executable's imports of
 * malloc and friends into it. What the game allocates from then on has no other
 * tenant, exactly as if it had linked MSVCR100.
 *
 * Three things make this safe rather than merely appealing.
 *
 * The redirect goes in after the process has started, so everything allocated
 * before it still lives on the process heap and will be freed through a pointer
 * we did not issue. free therefore dispatches: ours goes to our heap, everything
 * else goes to the runtime's own free, unchanged. The mixed period is correct by
 * construction rather than by timing.
 *
 * Ownership is not decided by address alone. A range table says which regions
 * our heap has been seen to occupy, and every block we issue carries an eight
 * byte header whose magic is keyed to the pointer it precedes. The range check
 * comes first because it cannot fault; the header is only read once the range
 * says the memory was ours, and it is what actually decides. A region that our
 * heap later hands back to the operating system can therefore never be mistaken
 * for ours if somebody else is given the same address.
 *
 * Allocations at or above GH_BIG stay with the runtime. A private heap satisfies
 * those with a dedicated VirtualAlloc that is released on free, which is the one
 * way a range in our table goes stale. They are also rare and long-lived, which
 * is to say they are not the state that fails to travel.
 *
 * Off unless D3D9SW_GAMEHEAP=1. This moves where the game's memory lives, and
 * nothing else in the engine has ever done that. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "savestate.h"
#include "logdir.h"

void savestate_log_line(const char *s);
int savestate_patch_iat(HMODULE mod, void *from, void *to);
int savestate_patch_iat_named(HMODULE mod, const char *dll, const char *fn, void *to,
			      void **prev);
void savestate_game_heap(HANDLE h);
void savestate_exclude(void *p, size_t bytes);
void *savestate_game_import_hook(const char *fn);
/* Defined further down with the note on why a fixed base is what makes a layout
 * reproducible. Declared here because the installer runs above it. */
static HANDLE gh_create_heap(void);
static void gh_heapcreate_hook(HMODULE mod);
static void gh_heapcreate_arm(void);
static void ct_arm(void);
static void ct_hook(HMODULE mod, const WCHAR *path);
static void ct_report(void);
static void clock_probe_install(HMODULE exe);
static int gh_wholesale(void);
static void gh_make_selfcontained(HANDLE h);
/* Public: 1 when `base` is the start of a heap we own AND have made self-contained
 * for a wholesale (freelist-and-all) restore. The savestate engine asks this to
 * decide whether a rewound heap region may be copied whole instead of block by
 * block. Returns 0 unless D3D9SW_WHOLESALE is set. */
int gameheap_owned_heap(uintptr_t base);

/* The import-mode installer runs at DLL attach, before the savestate log is
 * open, and anything said then would go nowhere. So it is said here instead
 * and replayed into the log by the first report. */
static char g_early[8192];
static int g_early_n, g_early_on;

static void ss_log(const char *fmt, ...)
{
	char b[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = wvsprintfA(b, fmt, ap);
	va_end(ap);
	if (g_early_on) {
		if (n > 0 && g_early_n + n < (int)sizeof(g_early)) {
			memcpy(g_early + g_early_n, b, (size_t)n);
			g_early_n += n;
		}
		return;
	}
	savestate_log_line(b);
}

static void gh_early_flush(void)
{
	char line[512];
	int i = 0, k = 0;

	if (!g_early_on)
		return;
	g_early_on = 0;
	for (i = 0; i < g_early_n; i++) {
		if (k < (int)sizeof(line) - 2)
			line[k++] = g_early[i];
		if (g_early[i] == '\n') {
			line[k] = 0;
			savestate_log_line(line);
			k = 0;
		}
	}
	if (k) {
		line[k++] = '\n';
		line[k] = 0;
		savestate_log_line(line);
	}
	g_early_n = 0;
}

/* Big enough that a private heap serves it from its own reservation rather than
 * out of a segment, which is the case where a freed block releases address space
 * that somebody else can then be given. */
#define GH_BIG (256u * 1024u)
#define GH_REG 512
#define GH_MAGIC ((uintptr_t)0x9E3779B9u)

typedef struct {
	uintptr_t magic;
	size_t size;
} GhHead;

static HANDLE g_heap SS_PRESENT;
static CRITICAL_SECTION g_cs;
static int g_ready;

static uintptr_t g_lo[GH_REG], g_hi[GH_REG];
static volatile LONG g_nreg;

/* D3D9SW_LAA_FALSE: a second region we own and the game does not know exists.
 * When on, the redirect and the floor serve blocks out of it instead of the
 * pinned arena, so the game runs on memory we control end to end. On a non-LAA
 * exe there is no space above 2 GB, so this competes with the game and is
 * EXPECTED to crash - that crash is the measurement. Reserved once and never
 * per-block freed, so it snapshots atomically like the arena. */
static HANDLE g_laa_heap SS_PRESENT;
static int g_laa_on;
static unsigned g_laa_mb;
static uintptr_t g_laa_lo, g_laa_hi;
/* The pinned arena's extent, set only on a successful fixed-base pin - a growable
 * HeapCreate fallback has no bounded extent and cannot be wholesale-restored. */
static uintptr_t g_heap_lo, g_heap_hi;
static int g_laa_high;			/* landed in the >2 GB band (exe really is LAA) */
static unsigned long g_laa_fell;	/* times the region was full and we fell back */
static void gh_create_laa_region(void);

static unsigned long g_alloc, g_freed_ours, g_freed_theirs, g_toobig, g_regfull;
static unsigned long g_stale, g_fellback;

/* Offsets the operator asked to watch, parsed at install. See gh_peek_one. */
#define GH_PEEK_MAX 8
static unsigned g_peek_off[GH_PEEK_MAX];
static int g_peek_n;
static void gh_peek_free(void *u);
static void gh_peek_parse(void);
static void gh_vorbis_free(const void *u, unsigned size);
static void gh_watch_parse(void);
static void gh_watch(const void *u, size_t n, unsigned site, unsigned ord,
		     const char *how);

/* -------------------------------------------------- the allocation trace
 *
 * D3D9SW_GHTRACE=N records the first N allocator operations - what kind, how
 * big, and where in the heap the block landed - and writes them to gh_trace.txt
 * beside the log.
 *
 * The reason to have it is an observation that is currently resting on two data
 * points. The player entity has been found at heap base + 0x7F4B8 in two
 * separate sessions whose heap bases differed (0DFB0000 and 0DC70000), which
 * says the heap's internal placement is a function of the allocation sequence
 * and nothing else - the base moves, the contents do not. If that holds over
 * tens of thousands of operations rather than one lucky pointer, then a
 * snapshot is relocatable and an address in it can be derived from a single
 * anchor. If it does not hold, everything built on it falls over, and it is far
 * cheaper to find that out from a diff of two traces than from a restore that
 * faults.
 *
 * Two runs, diff the offset column. Identical means deterministic.
 *
 * Constraints this sits under: it runs inside the game's malloc, on every call,
 * so it must not allocate, must not lock, and must not touch a file. A slot is
 * claimed with one interlocked increment into a buffer reserved up front, and
 * the whole thing is written out later from the census, which already runs on a
 * frame boundary. A lock here would change the timing of the very thing being
 * measured.
 */
#define GH_TR_ALLOC 'A'  /* served from our heap */
#define GH_TR_CALLOC 'C' /* served from our heap, zeroed */
#define GH_TR_REALLOC 'R'
#define GH_TR_FREE 'F'
#define GH_TR_BIG 'B'	 /* over GH_BIG, handed to the runtime */
#define GH_TR_OTHER 'O'	 /* freed something that was never ours */

typedef struct {
	unsigned op;
	unsigned size;
	unsigned off; /* from the heap base, or ~0 when it is not in our heap */
	unsigned site; /* the code that asked, as an RVA, or ~0 */
	unsigned ord;  /* how many that site had already asked for, or ~0 */
	unsigned tid;
} GhTr;

static GhTr *g_tr;
static volatile LONG g_tr_n;
static LONG g_tr_cap;

static void gh_trace_at(unsigned op, size_t n, const void *u, unsigned site, unsigned ord)
{
	LONG i;

	if (!g_tr)
		return;
	i = InterlockedIncrement(&g_tr_n) - 1;
	if (i >= g_tr_cap)
		return;
	g_tr[i].op = op;
	g_tr[i].size = (unsigned)n;
	/* A Windows heap handle is the address of its first segment, so the handle
	 * doubles as the base the offsets are measured from. */
	g_tr[i].off = (u && g_heap && (uintptr_t)u > (uintptr_t)g_heap)
			      ? (unsigned)((uintptr_t)u - (uintptr_t)g_heap)
			      : 0xFFFFFFFFu;
	g_tr[i].site = site;
	g_tr[i].ord = ord;
	g_tr[i].tid = GetCurrentThreadId();
}

static void gh_trace(unsigned op, size_t n, const void *u)
{
	gh_trace_at(op, n, u, 0xFFFFFFFFu, 0xFFFFFFFFu);
}

/* ---------------------------------------------------------------- block tags
 *
 * An address says where a block landed, not which block it is. Two sessions put
 * 96% of blocks at matching offsets under the stock heap, but a matching offset
 * is not evidence by itself: with a thousand blocks in a bounded arena some of
 * them coincide, and nothing in an address distinguishes the coincidences from
 * the real matches.
 *
 * So a block is named when it is asked for rather than recognised afterwards.
 * The name is the call site that asked plus how many times that site has asked
 * for that size already - the n-th block down this chute. The site is stable
 * because it is a code offset in an executable that does not change between
 * launches; the count is stable for as long as the game takes the same path,
 * which is the property actually under test.
 *
 * Size belongs in the key, not just the payload. A call site reached from two
 * paths allocating two different things would otherwise interleave its counts
 * and produce an ordinal that means nothing in either.
 *
 * The tags live beside the heap in a table keyed by address, NOT in GhHead.
 * Widening that header would push every block along and destroy the offset
 * agreement these tags exist to measure - the measurement would change the
 * thing being measured.
 *
 * Same constraints as the trace: this runs inside the game's malloc, on every
 * call, so it must not allocate, must not lock and must not touch a file. Both
 * tables are reserved up front and reached with interlocked operations only.
 */
/* Sized for a long run rather than for the live set. Retired entries are
 * tombstoned so that probe chains stay walkable, and although an insert reuses
 * the first tombstone it meets, a lookup has to walk past them - so what fills
 * this table is the total churn of a session, not its high-water mark. A
 * ten-minute run allocates tens of thousands of blocks against a live set of
 * about a thousand. */
#define GH_TAG_SLOTS 262144u
#define GH_TAG_MASK (GH_TAG_SLOTS - 1u)
#define GH_TAG_DEAD ((LONG)1) /* freed, but the probe chain must run through it */
/* Far larger than the ~1,200 blocks live at once, because retired slots are
 * tombstoned and only reused opportunistically: the table has to absorb the
 * whole churn of a run, not the high-water mark. */
#define GH_SITE_SLOTS 16384u
#define GH_SITE_MASK (GH_SITE_SLOTS - 1u)
#define GH_PROBE 64

typedef struct {
	volatile LONG addr; /* 0 free, 1 tombstone, otherwise the user pointer */
	unsigned site;
	unsigned size;
	unsigned ord;
} GhTag;

typedef struct {
	volatile LONG key; /* 0 free, otherwise the mixed (site, size) */
	unsigned site;
	unsigned size;
	volatile LONG next; /* how many this chute has produced */
} GhSite;

static GhTag *g_tag;
static GhSite *g_site;
static uintptr_t g_exe;	    /* so a site is an RVA and survives relocation */
static volatile LONG g_tag_full, g_site_full, g_tag_congested;

/* D3D9SW_GHTRACE=N. Reserved before the first allocation is served, because a
 * trace that starts late starts after the layout it is meant to explain has
 * already been decided; the tag tables likewise, or they cannot name the
 * long-lived blocks allocated first. */
static void gh_trace_arm(void)
{
	char v[16];
	unsigned cap, k;
	long want = 0;

	if (g_tr)
		return;
	cap = savestate_getenv("D3D9SW_GHTRACE", v, sizeof(v));
	for (k = 0; k < cap && v[k] >= '0' && v[k] <= '9'; k++)
		want = want * 10 + (v[k] - '0');
	if (want <= 0)
		return;
	g_tr = (GhTr *)VirtualAlloc(NULL, (SIZE_T)want * sizeof(GhTr), MEM_COMMIT | MEM_RESERVE,
				    PAGE_READWRITE);
	g_tr_cap = g_tr ? (LONG)want : 0;
	ss_log("gameheap: tracing the first %ld allocator operation(s) into gh_trace.txt%s\n",
	       want, g_tr ? "" : " - RESERVATION FAILED, tracing off");
	if (!g_tr)
		return;
	g_exe = (uintptr_t)GetModuleHandleA(NULL);
	g_tag = (GhTag *)VirtualAlloc(NULL, GH_TAG_SLOTS * sizeof(GhTag), MEM_COMMIT | MEM_RESERVE,
				      PAGE_READWRITE);
	g_site = (GhSite *)VirtualAlloc(NULL, GH_SITE_SLOTS * sizeof(GhSite),
					MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	ss_log("gameheap: naming blocks by (call site, ordinal) against image base %08lX%s\n",
	       (unsigned long)g_exe,
	       (g_tag && g_site) ? "" : " - TABLE RESERVATION FAILED, blocks will be unnamed");
}

static unsigned gh_mix(unsigned x)
{
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x;
}

/* One frame, via the compiler, rather than a stack walk: the game is built with
 * frame pointers omitted in places, so the EBP chain cannot be trusted, and one
 * frame is enough to name the chute. That frame is genuinely the game's own code
 * - it links its runtime statically and we detour its private copies, so there
 * is no runtime thunk sitting between the caller and here. */
static unsigned gh_site_of(void *ra)
{
	uintptr_t a = (uintptr_t)ra;

	if (!g_exe || a < g_exe)
		return 0xFFFFFFFFu;
	return (unsigned)(a - g_exe);
}

/* Matching on the mixed key alone, not on (site, size) as well, because the two
 * fields are written after the slot is claimed and a reader could otherwise see
 * them half set. A key collision would merge two chutes' counts; at 32 bits
 * mixed that is rare enough to accept in a diagnostic, and g_site_full reports
 * the case that actually matters, which is the table filling up. */
static unsigned gh_ordinal(unsigned site, unsigned size)
{
	unsigned key = gh_mix(site ^ (size * 2654435761u));
	unsigned h, i;

	if (!g_site)
		return 0xFFFFFFFFu;
	if (!key)
		key = 1;
	h = key & GH_SITE_MASK;
	for (i = 0; i < GH_PROBE; i++) {
		unsigned s = (h + i) & GH_SITE_MASK;
		LONG was = InterlockedCompareExchange(&g_site[s].key, (LONG)key, 0);

		if (was == 0) {
			g_site[s].site = site;
			g_site[s].size = size;
			return (unsigned)(InterlockedIncrement(&g_site[s].next) - 1);
		}
		if (was == (LONG)key)
			return (unsigned)(InterlockedIncrement(&g_site[s].next) - 1);
	}
	InterlockedIncrement(&g_site_full);
	return 0xFFFFFFFFu;
}

static void gh_tag_put(const void *u, unsigned site, unsigned size, unsigned ord)
{
	unsigned h, i;

	if (!g_tag || !u)
		return;
	h = gh_mix((unsigned)(uintptr_t)u) & GH_TAG_MASK;
	for (i = 0; i < GH_PROBE; i++) {
		unsigned s = (h + i) & GH_TAG_MASK;
		LONG was = InterlockedCompareExchange(&g_tag[s].addr,
						      (LONG)(uintptr_t)u, 0);

		/* A tombstone is reusable: an address is live at most once, so
		 * there cannot be an entry for this one further down the chain. */
		if (was != 0) {
			was = InterlockedCompareExchange(&g_tag[s].addr,
							 (LONG)(uintptr_t)u,
							 GH_TAG_DEAD);
			if (was != GH_TAG_DEAD)
				continue;
		}
		g_tag[s].site = site;
		g_tag[s].size = size;
		g_tag[s].ord = ord;
		return;
	}
	InterlockedIncrement(&g_tag_full);
}

/* Reads the tag and retires the slot in one go, because every caller is either
 * freeing the block or moving it. Leaving the entry behind would let the next
 * allocation to land on this address inherit a name that belongs to a block
 * that no longer exists, which is worse than having no name at all. */
static int gh_tag_take(const void *u, unsigned *site, unsigned *size, unsigned *ord)
{
	unsigned h, i;

	if (!g_tag || !u)
		return 0;
	h = gh_mix((unsigned)(uintptr_t)u) & GH_TAG_MASK;
	for (i = 0; i < GH_PROBE; i++) {
		unsigned s = (h + i) & GH_TAG_MASK;
		LONG a = g_tag[s].addr;

		if (a == 0)
			return 0; /* chain ended: this block was never tagged */
		if (a != (LONG)(uintptr_t)u)
			continue; /* including tombstones, which do not end it */
		*site = g_tag[s].site;
		*size = g_tag[s].size;
		*ord = g_tag[s].ord;
		InterlockedExchange(&g_tag[s].addr, GH_TAG_DEAD);
		return 1;
	}
	/* Reaching here means the probe ran out without meeting an empty slot,
	 * which is the table being too congested to search rather than the block
	 * being unknown - a different thing from the chain simply ending, and the
	 * one that means the numbers are degrading. Counted separately for that
	 * reason: walking off the end of a chain is normal and happens on every
	 * realloc that moved a block, since the tag was already taken. */
	InterlockedIncrement(&g_tag_congested);
	return 0;
}

/* Busy-block sidetable: heapwalk without invoking the heap.
 *
 * HeapWalk / HEAP_SEGMENT is a closed door on Wine. Scanning the saved
 * image for GhHead magic is a walk of the heap's bytes, not of the live
 * set: one session found 2 blocks against hundreds live, because the
 * scan jumped by aligned size and missed. We already intercept every
 * game malloc and free. The live (user, size) pairs are therefore a
 * thing we already know, kept in a table that is not itself on the heap.
 *
 * The live table is present-tense. A snapshot is taken after suspend_all
 * at save, packed into a second excluded array, and that is what restore
 * walks. After the copy, the live table is rewound to the snapshot so
 * free/realloc see the set the game believes is live.
 *
 * Same constraints as the tags: this runs inside the game's malloc, so
 * it must not allocate, must not lock, and must not touch a file. */
#define GH_BUSY_SLOTS 16384u
#define GH_BUSY_MASK (GH_BUSY_SLOTS - 1u)
#define GH_BUSY_DEAD ((LONG)1)

typedef struct {
	volatile LONG addr; /* 0 empty, 1 tomb, else the user pointer */
	size_t size;
} GhBusy;

typedef struct {
	void *head;
	size_t total;
} GhBusySnap;

static GhBusy *g_busy SS_PRESENT;
static volatile LONG g_busy_n, g_busy_full, g_busy_congested;
static GhBusySnap *g_busy_snap SS_PRESENT;
static unsigned g_busy_snap_n, g_busy_snap_cap;

static void gh_busy_put(const void *u, size_t n)
{
	unsigned h, i;

	if (!g_busy || !u)
		return;
	h = gh_mix((unsigned)(uintptr_t)u) & GH_BUSY_MASK;
	for (i = 0; i < GH_PROBE; i++) {
		unsigned s = (h + i) & GH_BUSY_MASK;
		LONG was = InterlockedCompareExchange(&g_busy[s].addr,
						      (LONG)(uintptr_t)u, 0);

		if (was != 0) {
			was = InterlockedCompareExchange(&g_busy[s].addr,
							 (LONG)(uintptr_t)u,
							 GH_BUSY_DEAD);
			if (was != GH_BUSY_DEAD)
				continue;
		}
		g_busy[s].size = n;
		InterlockedIncrement(&g_busy_n);
		return;
	}
	InterlockedIncrement(&g_busy_full);
}

static int gh_busy_take(const void *u)
{
	unsigned h, i;

	if (!g_busy || !u)
		return 0;
	h = gh_mix((unsigned)(uintptr_t)u) & GH_BUSY_MASK;
	for (i = 0; i < GH_PROBE; i++) {
		unsigned s = (h + i) & GH_BUSY_MASK;
		LONG a = g_busy[s].addr;

		if (a == 0)
			return 0;
		if (a != (LONG)(uintptr_t)u)
			continue;
		InterlockedExchange(&g_busy[s].addr, GH_BUSY_DEAD);
		InterlockedDecrement(&g_busy_n);
		return 1;
	}
	InterlockedIncrement(&g_busy_congested);
	return 0;
}

typedef void *(__cdecl *PFN_malloc)(size_t);
typedef void *(__cdecl *PFN_calloc)(size_t, size_t);
typedef void *(__cdecl *PFN_realloc)(void *, size_t);
typedef void(__cdecl *PFN_free)(void *);
typedef size_t(__cdecl *PFN_msize)(void *);
typedef void *(__cdecl *PFN_recalloc)(void *, size_t, size_t);
typedef void *(__cdecl *PFN_expand)(void *, size_t);

static PFN_malloc r_malloc;
static PFN_calloc r_calloc;
static PFN_realloc r_realloc;
static PFN_free r_free;
static PFN_msize r_msize;
static PFN_recalloc r_recalloc;
static PFN_expand r_expand;

/* Address only, and deliberately no dereference: this runs on every free in the
 * process, including pointers that were never ours. */
static int in_range(const void *p)
{
	uintptr_t a = (uintptr_t)p;
	LONG n = g_nreg;
	LONG i;

	for (i = 0; i < n; i++)
		if (a >= g_lo[i] && a < g_hi[i])
			return 1;
	return 0;
}

/* True while a pointer lives in the D3D9SW_LAA_FALSE region, so a realloc lands
 * on the heap that actually owns the block instead of the arena. */
static int in_laa(const void *p)
{
	uintptr_t a = (uintptr_t)p;

	return g_laa_hi && a >= g_laa_lo && a < g_laa_hi;
}

/* Where the redirect and the floor put a NEW block: the LAA_FALSE region when
 * that experiment is on, otherwise the pinned arena. A realloc must not use this
 * - it has to reuse the heap the existing block came from (see in_laa). */
static HANDLE gh_serving_heap(void)
{
	return (g_laa_on && g_laa_heap) ? g_laa_heap : g_heap;
}

/* D3D9SW_GHOWN (import mode, default on): the pinned span is served by this
 * allocator instead of RtlCreateHeap. An NT heap keeps Windows' state in its
 * header - links into ntdll's lock list, its entry in the process heap list,
 * a per-heap key that encodes every block header - and a cross-launch restore
 * put the old launch's copies of those back. Here every byte of state, lock
 * included, sits at the start of the span, so the span restores as plain data.
 *
 * Size classes 16, 24, 32, 48 ... 64 MB, a LIFO free list per class and a
 * bump pointer. No coalescing: a freed block only ever serves its own class. */
#define GHO_NCLASS 46
#define GHO_MAGIC ((uintptr_t)0x0DD5EED5u)
#define GHO_COMMIT (1u << 20)

typedef struct {
	volatile LONG lock;
	uintptr_t top, committed, end;
	void *free[GHO_NCLASS];
	unsigned long nalloc, nfree, nbad, nfull;
} GhOwn;

typedef struct {
	uintptr_t magic; /* GHO_MAGIC ^ the block's own address */
	uintptr_t cls;
} GhOwnBlk;

static GhOwn *g_own;

static int gho_class(SIZE_T n, SIZE_T *cap)
{
	unsigned b;

	if (n <= 16) {
		*cap = 16;
		return 0;
	}
	b = 31u - (unsigned)__builtin_clz((unsigned)(n - 1));
	if (n <= ((SIZE_T)3 << (b - 1))) {
		*cap = (SIZE_T)3 << (b - 1);
		return (int)(2 * (b - 4) + 1);
	}
	*cap = (SIZE_T)1 << (b + 1);
	return (int)(2 * (b + 1 - 4));
}

static SIZE_T gho_cap(int c)
{
	return (c & 1) ? (SIZE_T)3 << (c / 2 + 3) : (SIZE_T)1 << (c / 2 + 4);
}

static void gho_lock(void)
{
	while (InterlockedCompareExchange(&g_own->lock, 1, 0))
		SwitchToThread();
}

static void gho_unlock(void)
{
	InterlockedExchange(&g_own->lock, 0);
}

static int gho_init(void *base, SIZE_T size)
{
	if (!VirtualAlloc(base, GHO_COMMIT, MEM_COMMIT, PAGE_READWRITE))
		return 0;
	g_own = (GhOwn *)base;
	memset(g_own, 0, sizeof(*g_own));
	g_own->top = ((uintptr_t)base + sizeof(GhOwn) + 15) & ~(uintptr_t)15;
	g_own->committed = (uintptr_t)base + GHO_COMMIT;
	g_own->end = (uintptr_t)base + size;
	return 1;
}

static struct GhPlace *g_place;
static unsigned gl_my_role(void);
static GhOwnBlk *gp_take(unsigned role, unsigned site, int c, SIZE_T cap, unsigned *chute);
static int gp_give(GhOwnBlk *b, void *p);

static void *gho_alloc(SIZE_T n, int zero, unsigned site)
{
	SIZE_T cap;
	int c = gho_class(n, &cap);
	unsigned chute = 0, role = 0;
	GhOwnBlk *b;

	if (c >= GHO_NCLASS)
		return NULL;
	if (g_place)
		role = gl_my_role();
	gho_lock();
	if (g_place) {
		b = gp_take(role, site, c, cap, &chute);
		if (!b) {
			g_own->nfull++;
			gho_unlock();
			return NULL;
		}
	} else if ((b = (GhOwnBlk *)g_own->free[c]) != NULL) {
		g_own->free[c] = *(void **)(b + 1);
	} else {
		uintptr_t need = sizeof(GhOwnBlk) + cap, at = g_own->top;

		if (at + need > g_own->end) {
			g_own->nfull++;
			gho_unlock();
			return NULL;
		}
		while (at + need > g_own->committed) {
			if (!VirtualAlloc((void *)g_own->committed, GHO_COMMIT, MEM_COMMIT,
					  PAGE_READWRITE)) {
				g_own->nfull++;
				gho_unlock();
				return NULL;
			}
			g_own->committed += GHO_COMMIT;
		}
		g_own->top = at + need;
		b = (GhOwnBlk *)at;
	}
	b->magic = GHO_MAGIC ^ (uintptr_t)b;
	b->cls = (uintptr_t)c | ((uintptr_t)chute << 8);
	g_own->nalloc++;
	gho_unlock();
	if (zero)
		memset(b + 1, 0, n);
	return b + 1;
}

static GhOwnBlk *gho_blk(void *p)
{
	GhOwnBlk *b = (GhOwnBlk *)p - 1;

	if (!p || b->magic != (GHO_MAGIC ^ (uintptr_t)b) || (b->cls & 0xFF) >= GHO_NCLASS) {
		if (g_own)
			InterlockedIncrement((volatile LONG *)&g_own->nbad);
		return NULL;
	}
	return b;
}

static BOOL gho_free(void *p)
{
	GhOwnBlk *b = gho_blk(p);

	if (!b)
		return FALSE;
	gho_lock();
	b->magic = 0;
	if (!gp_give(b, p)) {
		*(void **)p = g_own->free[b->cls & 0xFF];
		g_own->free[b->cls & 0xFF] = b;
	}
	g_own->nfree++;
	gho_unlock();
	return TRUE;
}

/* Grows in place within the block's class, else fails; the callers already
 * move a block that HeapReAlloc could not resize. */
static void *gho_realloc(void *p, SIZE_T n)
{
	GhOwnBlk *b = gho_blk(p);

	return b && n <= gho_cap((int)(b->cls & 0xFF)) ? p : NULL;
}

/* D3D9SW_GHLEDGER=1: the arena keeps a record of every live block - who asked
 * for it, which thread role, how many that (role, site, size) had asked for
 * before, and when it arrived by operation, frame and millisecond. The record
 * sits at the start of the arena, so a save carries the map of the blocks it
 * holds and a restore puts the map back with them. Roles are named by thread
 * start address and the count of earlier threads with the same start, because
 * thread ids mean nothing in another launch. */
#define GL_CAP 65536u
#define GL_ORD 16384u
#define GL_ROLES 64
#define GL_DEAD 1u

typedef struct {
	unsigned addr, size, site, ord, role, arrival, role_op, frame, ms;
} GlEnt;

typedef struct {
	unsigned key, role, site, size, next;
} GlOrd;

typedef struct {
	char mod[32];
	unsigned rva, idx, ops, area, main;
} GlRole;

typedef struct {
	volatile LONG lock;
	unsigned arrival, live, full, ord_full, nroles;
	GlRole roles[GL_ROLES];
	GlOrd ords[GL_ORD];
	GlEnt ent[GL_CAP];
} GhLedger;

static GhLedger *g_led;
static DWORD g_gp_main;
static DWORD g_led_tls = TLS_OUT_OF_INDEXES;
static DWORD g_led_t0;
static volatile LONG g_led_frame SS_PRESENT;

void gameheap_frame(void)
{
	InterlockedIncrement(&g_led_frame);
}

static void gl_lock(void)
{
	while (InterlockedCompareExchange(&g_led->lock, 1, 0))
		SwitchToThread();
}

static void gl_unlock(void)
{
	InterlockedExchange(&g_led->lock, 0);
}

/* Every runtime thread starts in the runtime's own wrapper, so the routine it
 * was asked to run is taken from _beginthreadex and kept by thread id until the
 * thread first allocates. The thread is created suspended so it cannot ask
 * before it is listed. */
#define GL_TENT 256
static struct {
	volatile LONG tid;
	void *fn;
	unsigned seq;
} g_tent[GL_TENT];
static volatile LONG g_tent_n;
static uintptr_t(__cdecl *r_beginthreadex)(void *, unsigned, unsigned(__stdcall *)(void *),
					   void *, unsigned, unsigned *);

static uintptr_t __cdecl gh_beginthreadex(void *sec, unsigned stack,
					  unsigned(__stdcall *fn)(void *), void *arg,
					  unsigned flags, unsigned *tid)
{
	unsigned t = 0, i;
	uintptr_t h = r_beginthreadex(sec, stack, fn, arg, flags | CREATE_SUSPENDED, &t);

	if (h) {
		unsigned seq = 0;

		for (i = 0; i < GL_TENT; i++)
			if (g_tent[i].fn == (void *)fn)
				seq++;
		for (i = (unsigned)InterlockedIncrement(&g_tent_n) - 1; i < GL_TENT; i++)
			if (!InterlockedCompareExchange(&g_tent[i].tid, -1, 0)) {
				g_tent[i].fn = (void *)fn;
				g_tent[i].seq = seq;
				InterlockedExchange(&g_tent[i].tid, (LONG)t);
				break;
			}
		if (!(flags & CREATE_SUSPENDED))
			ResumeThread((HANDLE)h);
	}
	if (tid)
		*tid = t;
	return h;
}

/* The routine and how many threads were started on it before this one - the
 * order they were created in, not the order they first allocate in. */
static void *gl_entry_of_me(unsigned *seq)
{
	LONG me = (LONG)GetCurrentThreadId();
	unsigned i;

	for (i = 0; i < GL_TENT; i++)
		if (g_tent[i].tid == me) {
			*seq = g_tent[i].seq;
			InterlockedExchange(&g_tent[i].tid, 0);
			return g_tent[i].fn;
		}
	return NULL;
}

/* The thread's start as module and offset. Not under the ledger lock: the module
 * lookups take the loader lock, and a thread holding that may be allocating. */
static void gl_start(char *leaf_out, int cap, unsigned *rva, unsigned *seq)
{
	static LONG(NTAPI * qit)(HANDLE, ULONG, PVOID, ULONG, PULONG);
	void *start = gl_entry_of_me(seq);
	HMODULE m = NULL;
	char path[MAX_PATH], *leaf, *p;

	if (!qit)
		qit = (LONG(NTAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG))(void *)GetProcAddress(
			GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
	if (qit && !start)
		qit(GetCurrentThread(), 9 /* ThreadQuerySetWin32StartAddress */, &start,
		    sizeof(start), NULL);
	path[0] = 0;
	if (start && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				       (LPCSTR)start, &m))
		GetModuleFileNameA(m, path, sizeof(path));
	for (leaf = p = path; *p; p++)
		if (*p == '\\')
			leaf = p + 1;
	lstrcpynA(leaf_out, leaf[0] ? leaf : "?", cap);
	*rva = m ? (unsigned)((uintptr_t)start - (uintptr_t)m) : (unsigned)(uintptr_t)start;
}

/* Called with the ledger locked; leaf and rva from gl_start when the thread has
 * no role yet. */
static unsigned gl_role(const char *leaf, unsigned rva, unsigned seq)
{
	unsigned r = (unsigned)(uintptr_t)TlsGetValue(g_led_tls), i, idx = 0;

	if (r)
		return r - 1;
	for (i = 0; i < g_led->nroles; i++)
		if (g_led->roles[i].rva == rva && !lstrcmpiA(g_led->roles[i].mod, leaf))
			idx++;
	if (g_led->nroles >= GL_ROLES)
		r = GL_ROLES - 1;
	else {
		r = g_led->nroles++;
		lstrcpynA(g_led->roles[r].mod, leaf, sizeof(g_led->roles[r].mod));
		g_led->roles[r].rva = rva;
		g_led->roles[r].idx = seq != ~0u ? seq : idx;
		g_led->roles[r].main = GetCurrentThreadId() == g_gp_main;
	}
	TlsSetValue(g_led_tls, (void *)(uintptr_t)(r + 1));
	return r;
}

static unsigned gl_my_role(void)
{
	unsigned r = (unsigned)(uintptr_t)TlsGetValue(g_led_tls), rva = 0, seq = ~0u;
	char leaf[32];

	if (r)
		return r - 1;
	gl_start(leaf, sizeof(leaf), &rva, &seq);
	gl_lock();
	r = gl_role(leaf, rva, seq);
	gl_unlock();
	return r;
}

static unsigned gl_ord(unsigned role, unsigned site, unsigned size)
{
	unsigned key = gh_mix(site ^ (size * 2654435761u) ^ (role * 0x9E3779B9u)) | 1u, i;

	for (i = 0; i < GL_ORD; i++) {
		GlOrd *o = &g_led->ords[(key + i) & (GL_ORD - 1)];

		if (!o->key) {
			o->key = key;
			o->role = role;
			o->site = site;
			o->size = size;
		}
		if (o->key == key && o->role == role && o->site == site && o->size == size)
			return o->next++;
	}
	g_led->ord_full++;
	return 0xFFFFFFFFu;
}

static GlEnt *gl_find(unsigned addr, int make)
{
	unsigned h = gh_mix(addr), i;
	GlEnt *dead = NULL;

	for (i = 0; i < GL_CAP; i++) {
		GlEnt *e = &g_led->ent[(h + i) & (GL_CAP - 1)];

		if (e->addr == addr)
			return e;
		if (e->addr == GL_DEAD && !dead)
			dead = e;
		if (!e->addr)
			return make ? (dead ? dead : e) : NULL;
	}
	return make ? dead : NULL;
}

/* A block at an address already recorded is the same block resized in place,
 * so it keeps its identity. */
static void gl_put(const void *u, size_t n, unsigned site)
{
	GlEnt *e;
	unsigned role;

	if (!g_led || !u)
		return;
	role = gl_my_role();
	gl_lock();
	e = gl_find((unsigned)(uintptr_t)u, 1);
	if (!e)
		g_led->full++;
	else if (e->addr == (unsigned)(uintptr_t)u)
		e->size = (unsigned)n;
	else {
		e->addr = (unsigned)(uintptr_t)u;
		e->size = (unsigned)n;
		e->site = site;
		e->role = role;
		e->ord = gl_ord(role, site, (unsigned)n);
		e->arrival = g_led->arrival;
		e->role_op = g_led->roles[role].ops;
		e->frame = (unsigned)g_led_frame;
		e->ms = GetTickCount() - g_led_t0;
		g_led->live++;
	}
	g_led->arrival++;
	if (e)
		g_led->roles[e->role].ops++;
	gl_unlock();
}

static void gl_drop(const void *u)
{
	GlEnt *e;

	if (!g_led || !u)
		return;
	gl_lock();
	e = gl_find((unsigned)(uintptr_t)u, 0);
	if (e) {
		e->addr = GL_DEAD;
		g_led->live--;
	}
	g_led->arrival++;
	gl_unlock();
}

/* A block moved by realloc is the same block: the new address takes the old
 * one's identity. */
static void gl_move(const void *to, const void *from)
{
	GlEnt *a, *b;

	if (!g_led || !to || !from || to == from)
		return;
	gl_lock();
	a = gl_find((unsigned)(uintptr_t)from, 0);
	b = gl_find((unsigned)(uintptr_t)to, 0);
	if (a && b) {
		unsigned addr = b->addr, size = b->size;

		*b = *a;
		b->addr = addr;
		b->size = size;
	}
	gl_unlock();
}

static unsigned gh_knob(const char *name, unsigned def);

/* Carved from the front of the arena before any block is served, so the arena
 * layout with the ledger on is the same in every launch. */
static void gl_init(void)
{
	uintptr_t at, end;

	if (!g_own || !(gh_knob("D3D9SW_GHLEDGER", 0) || gh_knob("D3D9SW_GHPLACE", 0)))
		return;
	uintptr_t want;

	at = (g_own->top + 4095) & ~(uintptr_t)4095;
	end = (at + sizeof(GhLedger) + 15) & ~(uintptr_t)15;
	want = (end + 4095) & ~(uintptr_t)4095;
	if (end > g_own->end || (want > g_own->committed &&
				 !VirtualAlloc((void *)g_own->committed, want - g_own->committed,
					       MEM_COMMIT, PAGE_READWRITE))) {
		ss_log("gameheap: no room for the block ledger in the arena\n");
		return;
	}
	if (want > g_own->committed)
		g_own->committed = want;
	g_led_tls = TlsAlloc();
	if (g_led_tls == TLS_OUT_OF_INDEXES)
		return;
	g_led = (GhLedger *)at;
	memset(g_led, 0, sizeof(*g_led));
	g_own->top = end;
	g_led_t0 = GetTickCount();
	ss_log("gameheap: block ledger at %p, %u KB, %u live block(s) at most\n", (void *)g_led,
	       (unsigned)(sizeof(GhLedger) >> 10), GL_CAP);
	g_gp_main = GetCurrentThreadId();
}

/* D3D9SW_GHPLACE=1: a block's address follows from who asked for it, not from
 * when. Each thread role gets an area of the arena - the thread that loaded us
 * the big one, the game's other threads a large slot and everyone else's a
 * small one, picked by a hash of the role's name - and in its area each (site,
 * size class) gets chunks of its own and a free list of its own. Another
 * thread, or another site of the same thread, arriving earlier or later then
 * no longer moves the block. A role whose area fills spills into a shared
 * overflow area, which is placed by arrival again. */
#define GP_BIG 8
#define GP_BIG_SZ (2u << 20)
#define GP_SMALL 16
#define GP_SMALL_SZ (64u << 10)
#define GP_AREAS (1 + GP_BIG + GP_SMALL)
#define GP_CHUTES 8192u
#define GP_CHUNK_MAX (64u << 10)

typedef struct {
	unsigned key, role, site, cls, chunk, n;
	uintptr_t top, end;
	void *free;
} GpChute;

typedef struct {
	unsigned owner;
	uintptr_t top, end;
} GpArea;

typedef struct GhPlace {
	unsigned nchutes, chute_full, spilled, spilled_main, area_clash;
	uintptr_t over_top, over_end;
	GpArea area[GP_AREAS];
	GpChute ch[GP_CHUTES];
} GhPlace;

static char g_gp_exe[32];

static void gp_init(void)
{
	uintptr_t at, lo, avail, main_sz, rest;
	char path[MAX_PATH], *leaf, *p;
	unsigned i;

	if (!g_led || !gh_knob("D3D9SW_GHPLACE", 0))
		return;
	path[0] = 0;
	GetModuleFileNameA(NULL, path, sizeof(path));
	for (leaf = p = path; *p; p++)
		if (*p == '\\')
			leaf = p + 1;
	lstrcpynA(g_gp_exe, leaf, sizeof(g_gp_exe));
	at = (g_own->top + 4095) & ~(uintptr_t)4095;
	lo = (at + sizeof(GhPlace) + 0xFFFF) & ~(uintptr_t)0xFFFF;
	if (lo >= g_own->end || !VirtualAlloc((void *)at, sizeof(GhPlace), MEM_COMMIT,
					      PAGE_READWRITE)) {
		ss_log("gameheap: no room for block placement in the arena\n");
		return;
	}
	avail = g_own->end - lo;
	rest = (uintptr_t)GP_BIG * GP_BIG_SZ + (uintptr_t)GP_SMALL * GP_SMALL_SZ;
	if (avail < rest + (12u << 20)) {
		ss_log("gameheap: arena too small for block placement\n");
		return;
	}
	g_place = (GhPlace *)at;
	memset(g_place, 0, sizeof(*g_place));
	main_sz = (avail - rest - avail / 10) & ~(uintptr_t)0xFFFF;
	g_place->area[0].top = lo;
	g_place->area[0].end = lo + main_sz;
	for (at = lo + main_sz, i = 1; i < GP_AREAS; i++) {
		g_place->area[i].top = at;
		at += i <= GP_BIG ? GP_BIG_SZ : GP_SMALL_SZ;
		g_place->area[i].end = at;
	}
	g_place->over_top = at;
	g_place->over_end = g_own->end;
	g_own->top = g_own->end;
	ss_log("gameheap: block placement on - main area %u KB at %p, %u game slot(s) of %u KB, "
	       "%u other slot(s) of %u KB, overflow %u KB\n",
	       (unsigned)(main_sz >> 10), (void *)lo, GP_BIG, GP_BIG_SZ >> 10, GP_SMALL,
	       GP_SMALL_SZ >> 10, (unsigned)((g_place->over_end - g_place->over_top) >> 10));
}

static unsigned gp_area_of(unsigned role)
{
	GlRole *r = &g_led->roles[role];
	unsigned h, i, k = 0, first, count;
	const char *s;

	if (r->area)
		return r->area - 1;
	if (r->main) {
		r->area = 1;
		return 0;
	}
	h = gh_mix(r->rva);
	for (s = r->mod; *s; s++)
		h = gh_mix(h ^ (unsigned char)(*s | 0x20));
	h += r->idx;
	if (!lstrcmpiA(r->mod, g_gp_exe)) {
		first = 1;
		count = GP_BIG;
	} else {
		first = 1 + GP_BIG;
		count = GP_SMALL;
	}
	for (i = 0; i < count; i++) {
		k = first + (h + i) % count;
		if (!g_place->area[k].owner) {
			g_place->area[k].owner = h | 1;
			break;
		}
		if (i == 0)
			g_place->area_clash++;
	}
	r->area = i < count ? k + 1 : GP_AREAS + 1;
	return r->area - 1;
}

static uintptr_t gp_carve(unsigned role, uintptr_t size)
{
	unsigned a = gp_area_of(role);
	uintptr_t at;

	if (a < GP_AREAS && g_place->area[a].top + size <= g_place->area[a].end) {
		at = g_place->area[a].top;
		g_place->area[a].top += size;
	} else if (g_place->over_top + size <= g_place->over_end) {
		at = g_place->over_top;
		g_place->over_top += size;
		g_place->spilled++;
	} else if (g_place->area[0].end - size >= g_place->area[0].top) {
		/* From the far end down, so the main thread's own order is untouched. */
		g_place->area[0].end -= size;
		at = g_place->area[0].end;
		g_place->spilled_main++;
	} else
		return 0;
	return VirtualAlloc((void *)at, size, MEM_COMMIT, PAGE_READWRITE) ? at : 0;
}

/* Called with the arena locked. */
static GhOwnBlk *gp_take(unsigned role, unsigned site, int c, SIZE_T cap, unsigned *chute)
{
	unsigned key, i;
	uintptr_t need = sizeof(GhOwnBlk) + cap;
	GpChute *ch = NULL;
	GhOwnBlk *b;

	key = gh_mix(site ^ ((unsigned)c * 0x9E3779B9u) ^ (role * 0x85EBCA6Bu)) | 1u;
	for (i = 0; i < GP_CHUTES; i++) {
		GpChute *t = &g_place->ch[(key + i) & (GP_CHUTES - 1)];

		if (!t->key) {
			t->key = key;
			t->role = role;
			t->site = site;
			t->cls = (unsigned)c;
			g_place->nchutes++;
		}
		if (t->key == key && t->role == role && t->site == site && t->cls == (unsigned)c) {
			ch = t;
			break;
		}
	}
	if (!ch) {
		uintptr_t at;

		g_place->chute_full++;
		at = gp_carve(role, need);
		*chute = 0;
		return (GhOwnBlk *)at;
	}
	*chute = (unsigned)(ch - g_place->ch) + 1;
	ch->n++;
	if ((b = (GhOwnBlk *)ch->free) != NULL) {
		ch->free = *(void **)(b + 1);
		return b;
	}
	if (ch->top + need > ch->end) {
		unsigned k = ch->chunk ? ch->chunk * 2 : (unsigned)((1024 + need - 1) / need);
		uintptr_t at;

		if (k < 4)
			k = 4;
		if (k * need > GP_CHUNK_MAX)
			k = need >= GP_CHUNK_MAX ? 1 : (unsigned)(GP_CHUNK_MAX / need);
		at = gp_carve(role, k * need);
		if (!at)
			return NULL;
		ch->chunk = k;
		ch->top = at;
		ch->end = at + k * need;
	}
	b = (GhOwnBlk *)ch->top;
	ch->top += need;
	return b;
}

/* Called with the arena locked. */
static int gp_give(GhOwnBlk *b, void *p)
{
	unsigned c = (unsigned)(b->cls >> 8);
	GpChute *ch;

	if (!g_place || !c || c > GP_CHUTES)
		return 0;
	ch = &g_place->ch[c - 1];
	*(void **)p = ch->free;
	ch->free = b;
	return 1;
}

/* D3D9SW_MERGE_SITES: the saved contents of every block from the listed sites,
 * written into the live block with the same identity - role name, site, size
 * and ordinal. The saved ledger is read through fetch from the save at the
 * address the live one has, which placement makes the same in every launch.
 * Called with the process frozen. Returns blocks written, -1 if the save
 * holds no ledger where this launch has one. */
int gameheap_merge_sites(const unsigned *sites, int nsites,
			 int (*fetch)(void *ctx, uintptr_t a, void *dst, size_t n), void *ctx)
{
	GhLedger *sv;
	unsigned i, j, wrote = 0, unpaired = 0;
	int k;

	if (!g_led)
		return -1;
	sv = (GhLedger *)VirtualAlloc(NULL, sizeof(GhLedger), MEM_COMMIT | MEM_RESERVE,
				      PAGE_READWRITE);
	if (!sv)
		return -1;
	if (!fetch(ctx, (uintptr_t)g_led, sv, sizeof(GhLedger)) || sv->nroles > GL_ROLES) {
		VirtualFree(sv, 0, MEM_RELEASE);
		return -1;
	}
	for (i = 0; i < GL_CAP; i++) {
		const GlEnt *e = &sv->ent[i];
		const GlRole *sr;
		GlEnt *live = NULL;

		if (e->addr <= GL_DEAD || e->role >= sv->nroles)
			continue;
		for (k = 0; k < nsites && sites[k] != e->site; k++)
			;
		if (k == nsites)
			continue;
		sr = &sv->roles[e->role];
		for (j = 0; j < GL_CAP && !live; j++) {
			GlEnt *l = &g_led->ent[j];
			const GlRole *lr;

			if (l->addr <= GL_DEAD || l->site != e->site || l->size != e->size ||
			    l->ord != e->ord || l->role >= g_led->nroles)
				continue;
			lr = &g_led->roles[l->role];
			if (lr->rva == sr->rva && lr->idx == sr->idx && !lstrcmpiA(lr->mod, sr->mod))
				live = l;
		}
		if (!live) {
			if (unpaired++ < 8)
				ss_log("  merge: saved block %08X (site %08X, %X bytes, #%u) has "
				       "no live twin - left alone\n",
				       e->addr, e->site, e->size, e->ord);
			continue;
		}
		if (!fetch(ctx, e->addr, (void *)(uintptr_t)live->addr, e->size)) {
			ss_log("  merge: saved block %08X is not in the save\n", e->addr);
			continue;
		}
		if (wrote++ < 16)
			ss_log("  merge: block site %08X, %X bytes, #%u: saved %08X -> live %08X\n",
			       e->site, e->size, e->ord, e->addr, live->addr);
	}
	VirtualFree(sv, 0, MEM_RELEASE);
	if (unpaired)
		ss_log("  merge: %u saved block(s) of the listed sites had no live twin\n",
		       unpaired);
	return (int)wrote;
}

static void gl_write(const char *path)
{
	HANDLE f;
	char line[160];
	DWORD wrote;
	unsigned i;
	int k;

	if (!g_led)
		return;
	f = swlog_create(path, CREATE_ALWAYS);
	if (f == INVALID_HANDLE_VALUE)
		return;
	gl_lock();
	k = wsprintfA(line, "# ledger: %u live, %u operation(s), frame %ld, %u full, %u ordinal(s) unnamed\r\n",
		      g_led->live, g_led->arrival, (long)g_led_frame, g_led->full, g_led->ord_full);
	WriteFile(f, line, (DWORD)k, &wrote, NULL);
	if (g_place) {
		k = wsprintfA(line, "# placement: %u chute(s), %u without one, %u chunk(s) spilled, "
				    "%u more into the main area's far end, %u area clash(es), "
				    "overflow at %08X\r\n",
			      g_place->nchutes, g_place->chute_full, g_place->spilled,
			      g_place->spilled_main, g_place->area_clash,
			      (unsigned)g_place->over_top);
		WriteFile(f, line, (DWORD)k, &wrote, NULL);
	}
	for (i = 0; i < g_led->nroles; i++) {
		k = wsprintfA(line, "# role R%u %s+%X#%u %u op(s) area %d\r\n", i, g_led->roles[i].mod,
			      g_led->roles[i].rva, g_led->roles[i].idx, g_led->roles[i].ops,
			      (int)g_led->roles[i].area - 1);
		WriteFile(f, line, (DWORD)k, &wrote, NULL);
	}
	WriteFile(f, "# addr size site ordinal role arrival role_op frame ms\r\n", 56, &wrote, NULL);
	for (i = 0; i < GL_CAP; i++) {
		GlEnt *e = &g_led->ent[i];

		if (e->addr <= GL_DEAD)
			continue;
		k = wsprintfA(line, "%08X %X %08X %u R%u %u %u %u %u\r\n", e->addr, e->size, e->site,
			      e->ord, e->role, e->arrival, e->role_op, e->frame, e->ms);
		WriteFile(f, line, (DWORD)k, &wrote, NULL);
	}
	gl_unlock();
	CloseHandle(f);
}

static LPVOID gh_halloc(HANDLE h, DWORD flags, SIZE_T n, unsigned site)
{
	if (g_own && h == g_heap)
		return gho_alloc(n, (flags & HEAP_ZERO_MEMORY) != 0, site);
	return HeapAlloc(h, flags, n);
}

static BOOL gh_hfree(HANDLE h, DWORD flags, LPVOID p)
{
	if (g_own && h == g_heap)
		return gho_free(p);
	return HeapFree(h, flags, p);
}

static LPVOID gh_hrealloc(HANDLE h, DWORD flags, LPVOID p, SIZE_T n)
{
	if (g_own && h == g_heap)
		return gho_realloc(p, n);
	return HeapReAlloc(h, flags, p, n);
}

/* The whole reservation the block sits in, so one lookup covers a segment rather
 * than a page. Only ever called when a new allocation lands outside everything
 * we already know about, which after warm-up is almost never. */
static void note_region(const void *p)
{
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t base, end;
	LONG n;

	if (in_range(p))
		return;
	EnterCriticalSection(&g_cs);
	if (in_range(p) || VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi) ||
	    !mbi.AllocationBase) {
		LeaveCriticalSection(&g_cs);
		return;
	}
	base = (uintptr_t)mbi.AllocationBase;
	end = base;
	for (;;) {
		if (VirtualQuery((void *)end, &mbi, sizeof(mbi)) != sizeof(mbi))
			break;
		if ((uintptr_t)mbi.AllocationBase != base)
			break;
		end += mbi.RegionSize;
	}
	n = g_nreg;
	if (n < GH_REG && end > base) {
		g_lo[n] = base;
		g_hi[n] = end;
		/* Published last, so a reader that sees the count sees the bounds
		 * that go with it. */
		InterlockedIncrement(&g_nreg);
	} else if (n >= GH_REG) {
		g_regfull++;
	}
	LeaveCriticalSection(&g_cs);
}

/* The header, or nothing. Read only after in_range has agreed, so the memory is
 * inside a reservation our heap took and is safe to touch. The magic is keyed to
 * the pointer it precedes, so a stale range cannot launder somebody else's
 * block into ours. */
static GhHead *head_of(void *u)
{
	GhHead *h = (GhHead *)((char *)u - sizeof(GhHead));

	if (h->magic != (GH_MAGIC ^ (uintptr_t)u))
		return NULL;
	return h;
}

/* After a merge: every block the ledger now lists must carry its own header,
 * or the ledger and the arena it describes came from different launches. */
int gameheap_merge_proof(void)
{
	unsigned i, n = 0, bad = 0, gone = 0;

	if (!g_led)
		return 0;
	for (i = 0; i < GL_CAP; i++) {
		const GlEnt *e = &g_led->ent[i];
		MEMORY_BASIC_INFORMATION mbi;
		char *h;

		if (e->addr <= GL_DEAD)
			continue;
		n++;
		h = (char *)(uintptr_t)e->addr - sizeof(GhHead);
		if (!VirtualQuery(h, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
		    (char *)mbi.BaseAddress + mbi.RegionSize < (char *)(uintptr_t)e->addr) {
			if (gone++ < 8)
				ss_log("    proof: block %08X (site %X, %X bytes) is not committed\n",
				       e->addr, e->site, e->size);
			continue;
		}
		if (!head_of((void *)(uintptr_t)e->addr) && bad++ < 8)
			ss_log("    proof: block %08X (site %X, %X bytes, ord %u) has a foreign header\n",
			       e->addr, e->site, e->size, e->ord);
	}
	ss_log("  merge check: %u ledger block(s), %u with a foreign header, %u not committed\n", n,
	       bad, gone);
	return (int)(bad + gone);
}

/* Two ways to not be ours, and they could not be further apart.
 *
 * A pointer outside every reservation we took belongs to somebody else and must
 * go back to the runtime untouched. A pointer INSIDE one of our reservations
 * whose header does not check out is a block we issued whose header has since
 * been overwritten - and handing that to the runtime means calling RtlFreeHeap
 * with the process heap's handle and an address the process heap never issued,
 * which is how a heap gets corrupted rather than how one gets freed.
 *
 * That is not hypothetical. A restore rewinds our heap wholesale, so a block the
 * game allocated after the save has, the moment the copy lands, a header from
 * whatever older thing occupied those bytes. The game frees it a few hundred
 * frames later - tearing down audio it thinks is still playing - the magic does
 * not match, and the block goes out through the runtime to a heap that never
 * owned it. The fault lands inside ntdll's free path reading an uncommitted page
 * in OUR arena, which reads as a Windows bug until you know where the pointer
 * came from.
 *
 * g_stale has been counting exactly this case since the day it was written, and
 * the report has been calling it "a hole worth reading about". It was.
 *
 * The caller learns which kind of miss it had so it can refuse rather than
 * forward. Split out here rather than re-tested at each site because in_range is
 * a linear walk and this runs on every free in the process. */
static GhHead *ours_why(void *u, int *orphan)
{
	GhHead *h;

	*orphan = 0;
	if (!u || !g_heap || !in_range(u))
		return NULL;
	h = head_of(u);
	if (!h) {
		g_stale++;
		*orphan = 1;
	}
	return h;
}

/* ---- big blocks, import mode (D3D9SW_GAMEHEAP=2) ---------------------------
 *
 * A heap at a caller-chosen base is not growable, and a heap that cannot grow
 * refuses any block above about 512 KB - measured: 300 KB is served from inside
 * the region, 8 MB fails outright. PhysX's scene pools are bigger than that, and
 * sending them back to the runtime puts them on the process heap, which the
 * import mode leaves in the present. The bodies would rewind and the solver's
 * buffers they point into would not.
 *
 * So big blocks get a region of their own with an allocator simple enough that
 * all of its state - free list, high-water mark, lock - lives in the region's
 * first page. Restoring the region restores the allocator with it, which is the
 * property the process heap lacked. First fit over an address-ordered free list,
 * coalesced on free; there are hundreds of these blocks, not millions. Freed
 * memory stays committed, so the region's shape never changes under a save.
 *
 * The regions are chunks, added as the ones before fill and never released, so
 * no size has to be guessed up front and no range in the ownership table ever
 * goes stale. A chunk added after a save is simply not in that snapshot; with
 * reclaim off (the default) the restore leaves it alone, and whatever it holds
 * is unreachable from the rewound game - a leak, not a tear. A block bigger
 * than a chunk gets a chunk of its own size. */
typedef struct GhBigBlk {
	uintptr_t magic;
	size_t size; /* whole block, this header included */
	struct GhBigBlk *next; /* free list only */
	size_t owner; /* GH_BIG_SW for the renderer's, 0 for the game's */
} GhBigBlk;

#define GH_BIG_SW ((size_t)0x53574258u)

typedef struct {
	volatile LONG lock;
	size_t cap; /* the whole reservation, this page included */
	size_t top; /* bytes from the base handed out so far, this page included */
	size_t committed;
	GhBigBlk *free;
	size_t live, peak;
	unsigned long nalloc, nfail;
} GhBigArena;

#define GH_BIG_MAGIC ((uintptr_t)0x5BD1E995u)
#define GH_BIG_GRAIN 4096u
#define GH_BIG_COMMIT (1u << 20)
#define GH_BIG_CHUNKS 64

static GhBigArena *g_bigch[GH_BIG_CHUNKS];
static volatile LONG g_nbig, g_big_grow;
static size_t g_big_chunk; /* nonzero once import mode has armed big blocks */
static unsigned long g_big_bad, g_big_nochunk;
static int g_imports;		  /* installed by import table, not by detour */
static unsigned long g_migrated; /* runtime blocks moved to us by a realloc */
static void gh_add_range(uintptr_t lo, uintptr_t hi);
static unsigned gh_knob(const char *name, unsigned def);

static GhBigArena *big_chunk_of(const void *p)
{
	uintptr_t a = (uintptr_t)p;
	LONG i, n = g_nbig;

	for (i = 0; i < n; i++) {
		uintptr_t lo = (uintptr_t)g_bigch[i];

		if (a >= lo && a < lo + g_bigch[i]->cap)
			return g_bigch[i];
	}
	return NULL;
}

static int in_big(const void *p)
{
	return g_nbig && big_chunk_of(p) != NULL;
}

static void big_lock(GhBigArena *c)
{
	while (InterlockedCompareExchange(&c->lock, 1, 0))
		Sleep(0);
}

static void big_unlock(GhBigArena *c)
{
	InterlockedExchange(&c->lock, 0);
}

static GhBigBlk *chunk_alloc(GhBigArena *c, size_t need)
{
	uintptr_t lo = (uintptr_t)c;
	GhBigBlk **pp, *b = NULL;

	big_lock(c);
	for (pp = &c->free; *pp; pp = &(*pp)->next)
		if ((*pp)->size >= need) {
			b = *pp;
			break;
		}
	if (b) {
		if (b->size - need >= GH_BIG) {
			GhBigBlk *t = (GhBigBlk *)((char *)b + need);

			t->magic = 0;
			t->size = b->size - need;
			t->next = b->next;
			*pp = t;
			b->size = need;
		} else {
			*pp = b->next;
		}
	} else {
		if (need > c->cap - c->top) {
			big_unlock(c);
			return NULL;
		}
		if (c->top + need > c->committed) {
			size_t want = (c->top + need + GH_BIG_COMMIT - 1) &
				      ~(size_t)(GH_BIG_COMMIT - 1);

			if (want > c->cap)
				want = c->cap;
			if (!VirtualAlloc((void *)(lo + c->committed), want - c->committed,
					  MEM_COMMIT, PAGE_READWRITE)) {
				c->nfail++;
				big_unlock(c);
				return NULL;
			}
			c->committed = want;
		}
		b = (GhBigBlk *)(lo + c->top);
		b->size = need;
		c->top += need;
	}
	b->magic = GH_BIG_MAGIC ^ (uintptr_t)b;
	b->next = NULL;
	b->owner = 0;
	c->live += b->size;
	if (c->live > c->peak)
		c->peak = c->live;
	c->nalloc++;
	big_unlock(c);
	return b;
}

/* D3D9SW_GHBIG_PIN (default on in import mode): chunks are carved in order from
 * one span reserved at a fixed address, so a second session has them at the
 * same addresses as the first - one reservation, same allocation base, which is
 * what a cross-session restore checks. OS-placed chunks landed somewhere new
 * every launch and were most of the 49 regions a cross-session load refused. */
#define GH_BIGPIN_BASE 0x80000000u
static uintptr_t g_bigpin_base;
static SIZE_T g_bigpin_size, g_bigpin_used;
/* D3D9SW_GHBIG2_PIN/_MB: a second fixed span, used once the first is full. */
static uintptr_t g_bigpin2_base;
static SIZE_T g_bigpin2_size, g_bigpin2_used;

static uintptr_t bigpin_one(const char *at, const char *mbk, uintptr_t def, unsigned defmb,
			    SIZE_T *size)
{
	unsigned mb = gh_knob(mbk, defmb);
	uintptr_t base = gh_knob(at, def);
	void *res;

	if (!base || !mb)
		return 0;
	if (base == 1)
		base = GH_BIGPIN_BASE;
	res = VirtualAlloc((LPVOID)base, (SIZE_T)mb << 20, MEM_RESERVE, PAGE_READWRITE);
	if (!res || (uintptr_t)res != base) {
		ss_log("gameheap: could not pin %u MB for big blocks at %08lX (error %lu) - "
		       "chunks go wherever Windows puts them, and will not be at the same "
		       "addresses next session\n",
		       mb, (unsigned long)base, GetLastError());
		if (res)
			VirtualFree(res, 0, MEM_RELEASE);
		return 0;
	}
	*size = (SIZE_T)mb << 20;
	ss_log("gameheap: big-block chunks PINNED in %u MB at %08lX\n", mb,
	       (unsigned long)base);
	return base;
}

static void bigpin_reserve(void)
{
	g_bigpin_base = bigpin_one("D3D9SW_GHBIG_PIN", "D3D9SW_GHBIG_PIN_MB", 1, 1792,
				   &g_bigpin_size);
	if (g_bigpin_base)
		g_bigpin2_base = bigpin_one("D3D9SW_GHBIG2_PIN", "D3D9SW_GHBIG2_PIN_MB", 0, 0,
					    &g_bigpin2_size);
}

/* One more chunk, at least `need` bytes of blocks. Serialised so two threads
 * that both found every chunk full do not both reserve one. */
static GhBigArena *chunk_add(size_t need)
{
	SIZE_T cap = g_big_chunk;
	GhBigArena *c;
	void *res;
	LONG n;

	if (need + GH_BIG_GRAIN > cap)
		cap = (need + GH_BIG_GRAIN + 0xFFFFu) & ~(SIZE_T)0xFFFFu;
	n = g_nbig;
	if (n >= GH_BIG_CHUNKS)
		return NULL;
	/* The span's last stretch is still a fixed address: a block that fits in
	 * what is left takes all of it rather than going OS-placed. */
	if (g_bigpin_base && g_bigpin_used + cap > g_bigpin_size &&
	    g_bigpin_used + need + GH_BIG_GRAIN <= g_bigpin_size)
		cap = g_bigpin_size - g_bigpin_used;
	if (g_bigpin_base && g_bigpin_used + cap <= g_bigpin_size) {
		res = (void *)(g_bigpin_base + g_bigpin_used);
		if (!VirtualAlloc(res, GH_BIG_GRAIN, MEM_COMMIT, PAGE_READWRITE))
			return NULL;
		g_bigpin_used += cap;
	} else if (g_bigpin2_base &&
		   g_bigpin2_used + ((need + GH_BIG_GRAIN + 0xFFFFu) & ~(SIZE_T)0xFFFFu) <=
			   g_bigpin2_size) {
		cap = (need + GH_BIG_GRAIN + 0xFFFFu) & ~(SIZE_T)0xFFFFu;
		res = (void *)(g_bigpin2_base + g_bigpin2_used);
		if (!VirtualAlloc(res, GH_BIG_GRAIN, MEM_COMMIT, PAGE_READWRITE))
			return NULL;
		g_bigpin2_used += cap;
	} else {
		/* Past the span, a chunk is only as big as the block that asked for
		 * it: under a 2 GB ceiling a spare 64 MB reservation is what the next
		 * allocation fails for. */
		if (g_bigpin_base) {
			static LONG said;

			cap = (need + GH_BIG_GRAIN + 0xFFFFu) & ~(SIZE_T)0xFFFFu;
			if (!InterlockedExchange(&said, 1))
				ss_log("gameheap: the pinned big-block span is full - chunk %ld "
				       "and later go OS-placed, sized to their block\n",
				       (long)n);
		}
		res = VirtualAlloc(NULL, cap, MEM_RESERVE, PAGE_READWRITE);
		if (!res)
			return NULL;
		if (!VirtualAlloc(res, GH_BIG_GRAIN, MEM_COMMIT, PAGE_READWRITE)) {
			VirtualFree(res, 0, MEM_RELEASE);
			return NULL;
		}
	}
	c = (GhBigArena *)res;
	c->cap = cap;
	c->top = GH_BIG_GRAIN;
	c->committed = GH_BIG_GRAIN;
	gh_add_range((uintptr_t)res, (uintptr_t)res + cap);
	g_bigch[n] = c;
	InterlockedIncrement(&g_nbig);
	return c;
}

static void *big_alloc(size_t n)
{
	size_t need = (n + sizeof(GhBigBlk) + GH_BIG_GRAIN - 1) & ~(size_t)(GH_BIG_GRAIN - 1);
	GhBigBlk *b = NULL;
	GhBigArena *c;
	LONG i, seen = g_nbig;

	if (!g_big_chunk || need < n)
		return NULL;
	for (i = 0; i < seen && !b; i++)
		b = chunk_alloc(g_bigch[i], need);
	if (b)
		return b + 1;
	while (InterlockedCompareExchange(&g_big_grow, 1, 0))
		Sleep(0);
	for (i = seen; i < g_nbig && !b; i++)
		b = chunk_alloc(g_bigch[i], need);
	if (!b) {
		c = chunk_add(need);
		if (c)
			b = chunk_alloc(c, need);
		else
			g_big_nochunk++;
	}
	InterlockedExchange(&g_big_grow, 0);
	return b ? b + 1 : NULL;
}

/* Takes what big_alloc returned. A header that does not check out is leaked
 * rather than linked into the free list, for the same reason an orphan is. */
static void big_free(void *p)
{
	GhBigBlk *b = (GhBigBlk *)p - 1, **pp, *prev = NULL;
	GhBigArena *c = big_chunk_of(b);

	if (!c || b->magic != (GH_BIG_MAGIC ^ (uintptr_t)b)) {
		g_big_bad++;
		return;
	}
	big_lock(c);
	b->magic = 0;
	c->live -= b->size;
	for (pp = &c->free; *pp && *pp < b; pp = &(*pp)->next)
		prev = *pp;
	b->next = *pp;
	*pp = b;
	if (b->next && (char *)b + b->size == (char *)b->next) {
		b->size += b->next->size;
		b->next = b->next->next;
	}
	if (prev && (char *)prev + prev->size == (char *)b) {
		prev->size += b->size;
		prev->next = b->next;
	}
	big_unlock(c);
}

/* Every route that returns one of our blocks to its allocator. */
static void gh_raw_free(void *u, GhHead *h, DWORD flags)
{
	gl_drop(u);
	if (in_big(u))
		big_free(h);
	else
		gh_hfree(in_laa(u) ? g_laa_heap : g_heap, flags, h);
}

static void *give(void *raw, size_t n, unsigned site)
{
	GhHead *h = (GhHead *)raw;
	void *u = (char *)raw + sizeof(GhHead);

	h->magic = GH_MAGIC ^ (uintptr_t)u;
	h->size = n;
	note_region(u);
	gh_busy_put(u, n);
	gl_put(u, n, site);
	g_alloc++;
	return u;
}

/* The detour targets are the thin wrappers below. The work is in the _at forms
 * so that gameheap's own internal calls - realloc moving a block, recalloc -
 * can pass the site they were given instead of naming a line in this file. */
static void *gh_malloc_at(size_t n, unsigned site)
{
	void *raw;

	if (!g_ready || (n >= GH_BIG && !g_big_chunk)) {
		void *u = r_malloc(n);

		g_toobig += n >= GH_BIG;
		gh_trace(GH_TR_BIG, n, NULL);
		gh_watch(u, n, site, 0xFFFFFFFFu, "malloc-big");
		return u;
	}
	raw = n >= GH_BIG ? big_alloc(n + sizeof(GhHead))
			  : gh_halloc(gh_serving_heap(), 0, n + sizeof(GhHead), site);
	if (!raw) {
		g_fellback++;
		if (g_laa_on)
			g_laa_fell++;
		return r_malloc(n);
	}
	{
		void *u = give(raw, n, site);
		unsigned ord = gh_ordinal(site, (unsigned)n);

		gh_tag_put(u, site, (unsigned)n, ord);
		gh_trace_at(GH_TR_ALLOC, n, u, site, ord);
		gh_watch(u, n, site, ord, g_laa_on ? "malloc-laa" : "malloc-arena");
		return u;
	}
}

static void *gh_malloc(size_t n)
{
	return gh_malloc_at(n, gh_site_of(__builtin_return_address(0)));
}

static void *gh_calloc_at(size_t c, size_t s, unsigned site)
{
	size_t n = c * s;
	void *raw;

	if (c && n / c != s)
		return NULL;
	if (!g_ready || (n >= GH_BIG && !g_big_chunk)) {
		void *u = r_calloc(c, s);

		g_toobig += n >= GH_BIG;
		gh_trace(GH_TR_BIG, n, NULL);
		gh_watch(u, n, site, 0xFFFFFFFFu, "calloc-big");
		return u;
	}
	if (n >= GH_BIG) {
		raw = big_alloc(n + sizeof(GhHead));
		if (raw)
			memset(raw, 0, n + sizeof(GhHead));
	} else {
		raw = gh_halloc(gh_serving_heap(), HEAP_ZERO_MEMORY, n + sizeof(GhHead), site);
	}
	if (!raw) {
		g_fellback++;
		if (g_laa_on)
			g_laa_fell++;
		return r_calloc(c, s);
	}
	{
		void *u = give(raw, n, site);
		unsigned ord = gh_ordinal(site, (unsigned)n);

		gh_tag_put(u, site, (unsigned)n, ord);
		gh_trace_at(GH_TR_CALLOC, n, u, site, ord);
		gh_watch(u, n, site, ord, g_laa_on ? "calloc-laa" : "calloc-arena");
		return u;
	}
}

static void *gh_calloc(size_t c, size_t s)
{
	return gh_calloc_at(c, s, gh_site_of(__builtin_return_address(0)));
}

/* Leaking a block is a bounded cost. Corrupting the process heap is not, and it
 * is not even paid by the code that caused it - the damage surfaces later, in an
 * unrelated allocation, as a fault with nothing in its stack to explain itself.
 * So an orphan is dropped on the floor and counted, here and at every other
 * route out. The memory stays reserved in our arena, which is where it already
 * was and where nothing else will be given it. */
static unsigned long g_orphan_dropped;

static void gh_orphan_note(const char *what, void *p)
{
	if (!g_orphan_dropped++)
		ss_log("gameheap: %s of %p refused - the address is inside our arena "
		       "but its header is gone, so it is a block of ours whose head "
		       "was overwritten (a restore rewinding memory the game allocated "
		       "after the save does exactly this). Passing it to the runtime "
		       "would free it against a heap that never issued it. Dropped "
		       "instead; this leaks the block and keeps the heap intact\n",
		       what, p);
}

/* Defined with the allocation floor below. A dedicated-VirtualAlloc big block is
 * released here (and in gh_heapfree) before the arena logic runs: it is not in
 * the arena and must go back via VirtualFree, not HeapFree. Returns its size if
 * it was one of ours (and, with take, releases it). */
static unsigned long gh_va_lookup(void *u, int take);

static void gh_free(void *p)
{
	int orphan;
	GhHead *h;

	if (gh_va_lookup(p, 1))
		return;
	h = ours_why(p, &orphan);

	if (!h) {
		if (orphan) {
			gh_orphan_note("free", p);
			return;
		}
		if (p)
			g_freed_theirs++;
		if (p)
			gh_trace(GH_TR_OTHER, 0, NULL);
		r_free(p);
		return;
	}
	h->magic = 0;
	g_freed_ours++;
	{
		unsigned site = 0xFFFFFFFFu, size = 0, ord = 0xFFFFFFFFu;

		if (!gh_tag_take(p, &site, &size, &ord))
			site = ord = 0xFFFFFFFFu;
		gh_trace_at(GH_TR_FREE, h->size, p, site, ord);
	}
	gh_peek_free(p);
	gh_vorbis_free(p, (unsigned)h->size);
	gh_busy_take(p);
	/* Free against the heap that ISSUED the block. Under LAA_FALSE a block can
	 * live in the region rather than the arena, and freeing it against the wrong
	 * heap corrupts that heap's free list. in_laa is always false when the
	 * experiment is off, so the normal path is unchanged. */
	gh_raw_free(p, h, 0);
}

static void *gh_realloc_at(void *p, size_t n, unsigned site)
{
	int orphan;
	GhHead *h = ours_why(p, &orphan);
	void *raw, *q;
	size_t old;
	/* A realloc does not create an object, it resizes one, so the block keeps
	 * the name it was given when it was first asked for. Minting a fresh tag
	 * here would make one logical block look like two different ones across
	 * sessions that happened to grow it at different moments. */
	unsigned osite = 0xFFFFFFFFu, osize = 0, oord = 0xFFFFFFFFu;
	int named = 0;

	if (!p)
		return gh_malloc_at(n, site);
	if (!h) {
		if (orphan) {
			/* No header means no size, so there is nothing to copy
			 * forward. A fresh block of the size asked for is the only
			 * answer available, and it is the same answer the texture
			 * path already gives for a payload it cannot read back:
			 * rebuild empty rather than invent contents. */
			gh_orphan_note("realloc", p);
			return gh_malloc_at(n, site);
		}
		/* Not ours, so it stays where it is. Growing it into our heap
		 * would mean copying a block whose true size only the runtime
		 * knows.
		 *
		 * Except in import mode, where the runtime is a DLL that will say:
		 * there a block left behind is on the process heap, which stays in
		 * the present, so a container allocated at startup and grown later
		 * would never rewind. Moved over on its first resize instead. */
		if (g_imports && n) {
			size_t had = r_msize(p);
			void *q;

			if (had == (size_t)-1)
				return r_realloc(p, n);
			q = gh_malloc_at(n, site);
			if (!q)
				return NULL;
			memcpy(q, p, had < n ? had : n);
			r_free(p);
			g_migrated++;
			return q;
		}
		return r_realloc(p, n);
	}
	if (!n) {
		gh_free(p);
		return NULL;
	}
	old = h->size;
	named = gh_tag_take(p, &osite, &osize, &oord);
	if (n < GH_BIG && !in_big(p)) {
		/* Reuse the heap that issued this block, not the current serving heap:
		 * under LAA_FALSE an older arena block and a newer region block coexist,
		 * and reallocating one against the other's heap corrupts it. */
		raw = gh_hrealloc(in_laa(p) ? g_laa_heap : g_heap, 0, h,
				  n + sizeof(GhHead));
		if (raw)
			{
				void *u;

				gh_busy_take(p);
				u = give(raw, n, site);
				if (u != p) {
					gl_move(u, p);
					gl_drop(p);
				}

				if (!named) {
					osite = site;
					oord = gh_ordinal(site, (unsigned)n);
				}
				gh_tag_put(u, osite, osize ? osize : (unsigned)n,
					   oord);
				gh_trace_at(GH_TR_REALLOC, n, u, osite, oord);
				gh_watch(u, n, osite, oord, "realloc-arena");
				return u;
			}
	}
	/* Either it outgrew what we keep, or the heap could not extend it. Move
	 * it out to the runtime rather than fail: a realloc that returns NULL
	 * without freeing is correct C and a leak in most callers. */
	q = (n >= GH_BIG && !g_big_chunk) ? r_malloc(n) : gh_malloc_at(n, site);
	if (!q)
		return NULL;
	memcpy(q, p, old < n ? old : n);
	/* gh_malloc_at has just tagged q under this call site. Replace that with
	 * the name the block already had, so a move is invisible to identity. */
	if (named && n < GH_BIG) {
		unsigned d1, d2, d3;

		gh_tag_take(q, &d1, &d2, &d3);
		gh_tag_put(q, osite, osize, oord);
	}
	gl_move(q, p);
	gh_free(p);
	return q;
}

static void *gh_realloc(void *p, size_t n)
{
	return gh_realloc_at(p, n, gh_site_of(__builtin_return_address(0)));
}

static size_t gh_msize(void *p)
{
	int orphan;
	GhHead *h = ours_why(p, &orphan);

	if (h)
		return h->size;
	if (orphan) {
		/* Asking the runtime would have it read a header in our arena as
		 * though it were one of its own. Zero is a poor answer and a safe
		 * one. */
		gh_orphan_note("_msize", p);
		return 0;
	}
	return r_msize(p);
}

static void *gh_recalloc(void *p, size_t c, size_t s)
{
	size_t n = c * s;
	int orphan;
	GhHead *h = ours_why(p, &orphan);
	size_t old;
	void *q;
	/* Taken here and passed down, so the tag names the game's call site rather
	 * than the line below that forwards to it. */
	unsigned site = gh_site_of(__builtin_return_address(0));

	if (c && n / c != s)
		return NULL;
	if (!p)
		return gh_calloc_at(c, s, site);
	if (!h) {
		if (orphan) {
			gh_orphan_note("_recalloc", p);
			return gh_calloc_at(c, s, site);
		}
		return r_recalloc(p, c, s);
	}
	old = h->size;
	q = gh_realloc_at(p, n, site);
	if (q && n > old)
		memset((char *)q + old, 0, n - old);
	return q;
}

static void *gh_expand(void *p, size_t n)
{
	int orphan;
	GhHead *h = ours_why(p, &orphan);
	void *raw;

	if (!h) {
		if (orphan) {
			/* _expand never moves a block, so refusing is a normal
			 * answer the runtime already documents. */
			gh_orphan_note("_expand", p);
			return NULL;
		}
		return r_expand(p, n);
	}
	if (n >= GH_BIG || in_big(p))
		return NULL;
	raw = gh_hrealloc(in_laa(p) ? g_laa_heap : g_heap, HEAP_REALLOC_IN_PLACE_ONLY,
			  h, n + sizeof(GhHead));
	if (!raw)
		return NULL;
	h->size = n;
	gh_busy_take(p);
	gh_busy_put(p, n);
	return p;
}

/* Where the allocator actually is.
 *
 * The premise above is wrong in one detail that turns out to decide the method.
 * Rabi-Ribi does not link ucrtbase; it links the runtime statically. Its import
 * list is KERNEL32, USER32, GDI32, SHELL32 and STEAM_API and nothing else, so
 * the ucrtbase in the process belongs to Windows and the game never calls it.
 * That is why redirecting imports found nothing to redirect: there is no import
 * to redirect. The conclusion the log drew - that the game's runtime heap comes
 * from ucrtbase - named the right heap for the wrong reason.
 *
 * The allocator is in the game's own .text, and Ghidra found the whole of it by
 * working back from the floor that every allocator has to reach. Five functions
 * touch the runtime's heap handle, and only five:
 *
 *   malloc   0036E6B2 -> HeapAlloc      54 callers
 *   calloc   0037EBF7 -> HeapAlloc       2 callers   (__calloc_impl)
 *   realloc  0036E744 -> HeapReAlloc    15 callers
 *   free     00369754 -> HeapFree      107 callers
 *   _msize   0037805A -> HeapSize        2 callers
 *
 * All five read the handle from one global at 0118E588, which is __acrt_heap. A
 * static UCRT sets it to GetProcessHeap(), and that single assignment is the
 * origin of every ownership heuristic in savestate.c.
 *
 * The three other free-shaped functions in the image were the reason to go
 * looking. None of them touches HeapFree; they tail-call this one. The set is
 * closed, which is the property that makes hooking it safe - a block allocated
 * through us and released through a routine we missed would go to HeapFree on a
 * heap it does not belong to.
 *
 * Nothing outside the runtime shares these. The four other Heap* callers in the
 * image ask GetProcessHeap for themselves and free what they themselves
 * allocated; they never see a pointer of ours.
 *
 * So we detour the game's private copies rather than redirecting an import.
 * That is better than the original plan, not a fallback from it: the copies
 * belong to the game alone, so the redirect cannot reach Windows even by
 * accident. Windows keeps ucrtbase; the game gets a heap with no other tenant.
 *
 * Tempting and rejected: write our heap handle into __acrt_heap and change no
 * code at all. Every allocation already live at that moment came from the
 * process heap, and the next free would hand it to HeapFree against a heap that
 * never issued it. The swap is only safe before the runtime initialises, and we
 * are loaded by LoadLibrary long after. Detours dispatch per pointer, so the
 * mixed period is correct by construction. */
#define GH_PRO 7

/* Seven bytes because five is the jump and the eighth would split an
 * instruction. Both shapes end on an instruction boundary and neither contains
 * a relative operand, so the bytes can be moved to a trampoline unchanged. */
static const unsigned char kProSaveEsi[GH_PRO] = { 0x55, 0x8B, 0xEC, 0x56,
						   0x8B, 0x75, 0x08 };
static const unsigned char kProTestArg[GH_PRO] = { 0x55, 0x8B, 0xEC, 0x83,
						   0x7D, 0x08, 0x00 };

struct Site {
	const char *name;
	unsigned rva; /* 0 where this target has no such function */
	const unsigned char *pro;
	void **real;
	void *ours;
};

/* Deliberately not the aligned family, and not _strdup.
 *
 * _aligned_malloc and _aligned_free are a closed pair: leave both alone and
 * every aligned block is allocated and released by the same allocator, which is
 * self-consistent. Hook one and not the other and it is not. Aligning by hand on
 * our own heap is possible but it is more surface than the state it would buy.
 *
 * _strdup allocates through malloc, so its blocks arrive here on their own.
 *
 * _recalloc and _expand carry no RVA because this executable does not contain
 * them. They stay in the table so the log says we looked rather than leaving the
 * reader to wonder, and so their handlers are not quietly dropped if a later
 * target does have them. */
static struct Site g_sites[] = {
	{ "malloc", 0x0036E6B2, kProSaveEsi, (void **)&r_malloc, NULL },
	{ "calloc", 0x0037EBF7, kProSaveEsi, (void **)&r_calloc, NULL },
	{ "realloc", 0x0036E744, kProTestArg, (void **)&r_realloc, NULL },
	{ "free", 0x00369754, kProTestArg, (void **)&r_free, NULL },
	{ "_msize", 0x0037805A, kProTestArg, (void **)&r_msize, NULL },
	{ "_recalloc", 0, NULL, (void **)&r_recalloc, NULL },
	{ "_expand", 0, NULL, (void **)&r_expand, NULL }
};

#define GH_SITES ((int)(sizeof(g_sites) / sizeof(g_sites[0])))

/* The bytes have to be the ones we were promised.
 *
 * An RVA is only meaningful against the build it was read from. Point it at a
 * patched executable, a different version, or a target that is not this game at
 * all, and five bytes of jump land in the middle of an instruction - which does
 * not fail, it misbehaves. So every site is checked against the prologue Ghidra
 * recorded, and one mismatch stops the whole install. Patching four of five is
 * worse than patching none. */
static int site_ok(HMODULE exe, const struct Site *s)
{
	const unsigned char *t = (const unsigned char *)exe + s->rva;
	MEMORY_BASIC_INFORMATION mbi;

	if (!VirtualQuery(t, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
		return 0;
	return memcmp(t, s->pro, GH_PRO) == 0;
}

/* The displaced bytes, then a jump back to what follows them. Built for every
 * site before any target is written, because the moment the first jump goes in
 * another thread can arrive in our free and needs the real one to forward to. */
static void site_tramp(HMODULE exe, struct Site *s, unsigned char *tr)
{
	unsigned char *t = (unsigned char *)exe + s->rva;

	memcpy(tr, t, GH_PRO);
	tr[GH_PRO] = 0xE9;
	*(LONG *)(tr + GH_PRO + 1) = (LONG)(t + GH_PRO - (tr + GH_PRO + 5));
	*s->real = tr;
}

static int site_wire(HMODULE exe, struct Site *s)
{
	unsigned char *t = (unsigned char *)exe + s->rva;
	DWORD old;

	if (!VirtualProtect(t, GH_PRO, PAGE_EXECUTE_READWRITE, &old))
		return 0;
	t[0] = 0xE9;
	*(LONG *)(t + 1) = (LONG)((unsigned char *)s->ours - (t + 5));
	/* The two bytes the jump does not cover. Never executed, because nothing
	 * branches between a function's entry and its fourth byte, but a decoder
	 * reading them should see one instruction rather than half of one. */
	t[5] = 0x90;
	t[6] = 0x90;
	VirtualProtect(t, GH_PRO, old, &old);
	FlushInstructionCache(GetCurrentProcess(), t, GH_PRO);
	return 1;
}

/* The floor, under everything.
 *
 * The five detours cover the runtime's allocator, and the runtime's allocator
 * is not the only thing in this executable that frees memory. Three other
 * functions call HeapFree against GetProcessHeap() directly - a custom pair and
 * a teardown path - and they are not allocators we can replace: they take no
 * pointer argument, they read globals and release what they computed. There is
 * no entry point to detour.
 *
 * Whether any of them can ever be handed a block of ours is a question two
 * design notes wanted settled before arming anything. It does not need
 * settling, because it can be made moot. Every one of them reaches HeapFree
 * through a single import slot, and so does the runtime's own free. One patch
 * there puts an ownership check underneath every freer in the process,
 * including the ones nobody has identified.
 *
 * A count of zero here across a long session is the measurement those notes
 * asked for. A count above zero is a wrong-heap free that would have been
 * silent corruption. Either answer is worth having; only one of them needed
 * code to prevent. */
static BOOL(WINAPI *r_heapfree)(HANDLE, DWORD, LPVOID);
static unsigned long g_caught;

/* Defined with the log-only allocation floor below; used here so a forwarded
 * free can subtract the block from the floor's live-footprint tally. */
static int g_floor_on;
static int floor_live_take(void *p);

static BOOL WINAPI gh_heapfree(HANDLE heap, DWORD flags, LPVOID p)
{
	GhHead *h;

	int orphan = 0;

	if (gh_va_lookup(p, 1))
		return TRUE;
	/* Our own HeapFree calls do not come back through here - those go direct
	 * to the real one - so this only ever sees the game's. */
	if (g_ready && (h = ours_why(p, &orphan)) != NULL) {
		h->magic = 0;
		g_caught++;
		g_freed_ours++;
		gh_peek_free(p);
		gh_vorbis_free(p, (unsigned)h->size);
		gh_busy_take(p);
		gh_raw_free(p, h, flags);
		return TRUE;
	}
	/* The floor has to hold for the orphan case too, and this is the route the
	 * observed crash actually took: RtlFreeHeap called with the process heap's
	 * handle and an address inside our arena. */
	if (orphan) {
		gh_orphan_note("HeapFree", p);
		return TRUE;
	}
	if (g_floor_on)
		floor_live_take(p);
	return r_heapfree(heap, flags, p);
}

/* ---- allocation floor, log-only (D3D9SW_GHFLOOR=1) --------------------------
 *
 * The seven detours above own the executable's static CRT, so the game's own
 * malloc/free live in the pinned arena and rewind with it. Anything the game
 * reaches by another route - a direct HeapAlloc, or the system CRT that
 * ucrtbase routes to GetProcessHeap - lands on the process heap, which we do
 * not own. Those are the blocks a restore cannot keep in step: the game's
 * pointer to one rewinds to the save while the process-heap block has moved on,
 * and the next free walks metadata that no longer matches. The RtlFreeHeap
 * fault a few hundred frames after a BGM swap is exactly this.
 *
 * Before deciding what to redirect, name what escapes. This forwards HeapAlloc
 * and HeapReAlloc to the real functions UNCHANGED - it never redirects, so it
 * cannot move the baseline - and tallies, by the game call site, every result
 * that lands outside the arena. The report prints the finite list. That outline
 * is the whole point: it says whether owning the rest is a short list or a long
 * one, and it is the same divergence set a cross-session restore would have to
 * reconstruct. */
#define GH_FLOOR_SITES 256

static struct {
	void *ret;           /* the game call site that allocated off-arena */
	volatile LONG count;
	unsigned long bytes; /* cumulative requested; summed without a lock */
} g_floor[GH_FLOOR_SITES];
static volatile LONG g_floor_n;
static volatile LONG g_floor_over; /* escapes seen after the table filled */
static volatile LONG g_floor_a_calls, g_floor_r_calls; /* HeapAlloc vs HeapReAlloc */
static int g_floor_redirect; /* GHFLOOR>=2: serve escapes from the arena, not just log */
static int g_floor_va;       /* GHFLOOR>=3: big blocks on dedicated VirtualAlloc.
                              * EXPERIMENTAL and known-broken across >1 save: a freed
                              * VA reservation's address is reused, which the snapshot
                              * cannot capture atomically the way it does the arena. */

static LPVOID(WINAPI *r_floor_halloc)(HANDLE, DWORD, SIZE_T);
static LPVOID(WINAPI *r_floor_ralloc)(HANDLE, DWORD, LPVOID, SIZE_T);

/* Live-footprint map: address -> KB, so PEAK concurrent off-arena bytes can be
 * reported. That number, not the cumulative sum, decides whether owning this
 * site fits the 64 MB arena or needs it grown. Open-addressed with a tombstone
 * so deletions do not break probe chains; the live set stays small when the site
 * is a decode treadmill, which is the case we expect. Held in this DLL's data,
 * which is left in the present, so it is not disturbed by a rewind. */
#define GH_FLOOR_MAP 16384
#define GH_FLOOR_TOMB ((void *)~(uintptr_t)0)
static CRITICAL_SECTION g_floor_cs;
static int g_floor_cs_ready;
static struct {
	void *p;
	unsigned long kb;
} g_floor_map[GH_FLOOR_MAP];
static unsigned long g_floor_live_kb, g_floor_peak_kb;
static unsigned g_pin_mb; /* the pinned arena size, set when the heap is created */

static unsigned long gh_kb(size_t n)
{
	return (unsigned long)((n + 1023) >> 10);
}

static void floor_live_add(void *p, size_t n)
{
	unsigned h, i;

	if (!g_floor_cs_ready || !p)
		return;
	EnterCriticalSection(&g_floor_cs);
	h = (unsigned)(((uintptr_t)p >> 4) & (GH_FLOOR_MAP - 1));
	for (i = 0; i < GH_FLOOR_MAP; i++) {
		unsigned s = (h + i) & (GH_FLOOR_MAP - 1);

		if (!g_floor_map[s].p || g_floor_map[s].p == GH_FLOOR_TOMB) {
			g_floor_map[s].p = p;
			g_floor_map[s].kb = gh_kb(n);
			g_floor_live_kb += g_floor_map[s].kb;
			if (g_floor_live_kb > g_floor_peak_kb)
				g_floor_peak_kb = g_floor_live_kb;
			break;
		}
	}
	LeaveCriticalSection(&g_floor_cs);
}

/* Nonzero if the address was one of ours, so a forwarded free knows whether it
 * was a floor block. */
static int floor_live_take(void *p)
{
	unsigned h, i;
	int found = 0;

	if (!g_floor_cs_ready || !p)
		return 0;
	EnterCriticalSection(&g_floor_cs);
	h = (unsigned)(((uintptr_t)p >> 4) & (GH_FLOOR_MAP - 1));
	for (i = 0; i < GH_FLOOR_MAP; i++) {
		unsigned s = (h + i) & (GH_FLOOR_MAP - 1);

		if (!g_floor_map[s].p)
			break; /* empty slot ends the probe: not ours */
		if (g_floor_map[s].p == p) {
			if (g_floor_live_kb >= g_floor_map[s].kb)
				g_floor_live_kb -= g_floor_map[s].kb;
			g_floor_map[s].p = GH_FLOOR_TOMB;
			g_floor_map[s].kb = 0;
			found = 1;
			break;
		}
	}
	LeaveCriticalSection(&g_floor_cs);
	return found;
}

/* ---- dedicated VirtualAlloc for big floor blocks ---------------------------
 *
 * gh_floor_arena sends anything >= GH_BIG here instead of into the shared arena.
 * Each big block is its own VirtualAlloc reservation, so it never fragments the
 * fixed arena and never needs it grown - the arena then only ever holds small
 * blocks. The block is committed private memory, so a save captures it and it
 * rewinds with the game like any owned block; on free it is released with
 * VirtualFree, which - unlike HeapFree - reads no heap metadata and so cannot
 * fault on a block whose contents a restore rewound. Identified at free time by
 * a small present-tense registry rather than by the arena's in_range table, so
 * releasing one never leaves a stale range that could launder a later address
 * into ours. Blocks the game orphans across a restore leak their reservation -
 * a bounded, later-reclaimable cost, not a crash. */
#define GH_VA_MAP 4096
#define GH_VA_TOMB ((void *)~(uintptr_t)0)
static struct {
	void *p;
	unsigned long bytes;
} g_va_map[GH_VA_MAP];
static volatile LONG g_va_n; /* live big VA blocks */
static unsigned long g_va_total, g_va_live_kb, g_va_peak_kb;

static void gh_va_put(void *u, size_t bytes)
{
	unsigned hh, i;

	if (!g_floor_cs_ready || !u)
		return;
	EnterCriticalSection(&g_floor_cs);
	hh = (unsigned)(((uintptr_t)u >> 4) & (GH_VA_MAP - 1));
	for (i = 0; i < GH_VA_MAP; i++) {
		unsigned s = (hh + i) & (GH_VA_MAP - 1);

		if (!g_va_map[s].p || g_va_map[s].p == GH_VA_TOMB) {
			g_va_map[s].p = u;
			g_va_map[s].bytes = (unsigned long)bytes;
			InterlockedIncrement(&g_va_n);
			g_va_total++;
			g_va_live_kb += (unsigned long)((bytes + 1023) >> 10);
			if (g_va_live_kb > g_va_peak_kb)
				g_va_peak_kb = g_va_live_kb;
			break;
		}
	}
	LeaveCriticalSection(&g_floor_cs);
}

static unsigned long gh_va_lookup(void *u, int take)
{
	unsigned hh, i;
	unsigned long bytes = 0;

	if (!g_floor_cs_ready || !u || !g_va_n)
		return 0; /* no big blocks live: the common free pays nothing */
	EnterCriticalSection(&g_floor_cs);
	hh = (unsigned)(((uintptr_t)u >> 4) & (GH_VA_MAP - 1));
	for (i = 0; i < GH_VA_MAP; i++) {
		unsigned s = (hh + i) & (GH_VA_MAP - 1);

		if (!g_va_map[s].p)
			break; /* empty slot ends the probe: not one of ours */
		if (g_va_map[s].p == u) {
			bytes = g_va_map[s].bytes;
			if (take) {
				unsigned long kb = (bytes + 1023) >> 10;

				g_va_map[s].p = GH_VA_TOMB;
				g_va_map[s].bytes = 0;
				if (g_va_live_kb >= kb)
					g_va_live_kb -= kb;
				InterlockedDecrement(&g_va_n);
				VirtualFree((char *)u - sizeof(GhHead), 0, MEM_RELEASE);
			}
			break;
		}
	}
	LeaveCriticalSection(&g_floor_cs);
	return bytes;
}

static void *gh_va_alloc(size_t n, unsigned site)
{
	void *base = VirtualAlloc(NULL, sizeof(GhHead) + n, MEM_COMMIT | MEM_RESERVE,
				 PAGE_READWRITE);
	GhHead *h;
	void *u;

	(void)site;
	if (!base) {
		g_fellback++;
		return r_malloc(n); /* rare; escapes, which the caller then records */
	}
	h = (GhHead *)base;
	u = (char *)base + sizeof(GhHead);
	h->magic = GH_MAGIC ^ (uintptr_t)u; /* so a stray ours_why still reads sane */
	h->size = n;
	gh_va_put(u, n); /* VirtualAlloc already zeroed the pages, so zero-init is free */
	return u;
}

/* Per-site tally, plus feeding the live map. Keyed by call site, not by block,
 * so the table stays tiny; a race on first sight can duplicate a row, which the
 * report sums correctly either way. */
static void gh_floor_note(void *ret, void *p, size_t n)
{
	LONG i, seen;

	if (!p || in_range(p))
		return; /* already owned - not an escape */
	floor_live_add(p, n);
	seen = g_floor_n;
	for (i = 0; i < seen && i < GH_FLOOR_SITES; i++) {
		if (g_floor[i].ret == ret) {
			InterlockedIncrement(&g_floor[i].count);
			g_floor[i].bytes += (unsigned long)n;
			return;
		}
	}
	i = InterlockedIncrement(&g_floor_n) - 1;
	if (i >= GH_FLOOR_SITES) {
		InterlockedIncrement(&g_floor_over);
		return;
	}
	g_floor[i].ret = ret;
	g_floor[i].count = 1;
	g_floor[i].bytes = (unsigned long)n;
}

/* Arena allocation for the floor, WITHOUT the GH_BIG cap. gh_malloc_at sends
 * anything >= 256 KB back to the runtime, which for this site would leave the big
 * decode buffers - the very ones freed across a restore - on the process heap,
 * i.e. unfixed. The floor's point is to own them, so it takes them into the arena
 * at any size, falling back to the runtime only if the arena is genuinely out of
 * room, which the report then shows as a remaining escape (the signal to grow
 * D3D9SW_GHPIN_MB). give() stamps the same header the free paths already read, so
 * both HeapFree and static-CRT free reclaim these exactly like the game's own. */
static void *gh_floor_arena(size_t n, int zero, unsigned site)
{
	void *raw;

	if (!g_ready)
		return zero ? r_calloc(1, n) : r_malloc(n);
	/* Big blocks CAN get their own VirtualAlloc, released on free, so they neither
	 * fragment the fixed arena nor need it grown - but only under GHFLOOR>=3,
	 * because that path is not snapshot-safe across more than one save (a freed
	 * reservation's address is reused and the snapshot cannot track it). Default
	 * (GHFLOOR=2) keeps everything in the atomically-captured arena. */
	if (g_floor_va && n >= GH_BIG)
		return gh_va_alloc(n, site);
	raw = gh_halloc(gh_serving_heap(), zero ? HEAP_ZERO_MEMORY : 0,
			n + sizeof(GhHead), site);
	if (!raw) {
		g_fellback++;
		if (g_laa_on)
			g_laa_fell++;
		return zero ? r_calloc(1, n) : r_malloc(n);
	}
	{
		void *u = give(raw, n, site);
		unsigned ord = gh_ordinal(site, (unsigned)n);

		gh_tag_put(u, site, (unsigned)n, ord);
		gh_trace_at(zero ? GH_TR_CALLOC : GH_TR_ALLOC, n, u, site, ord);
		gh_watch(u, n, site, ord, g_laa_on ? "floor-laa" : "floor-arena");
		return u;
	}
}

static LPVOID WINAPI gh_floor_heapalloc(HANDLE heap, DWORD flags, SIZE_T n)
{
	void *ret = __builtin_return_address(0);
	LPVOID p;

	/* Redirect (GHFLOOR>=2): serve from the pinned arena so the block rewinds
	 * with the game and its pointer stays in step across a restore. Reuses the
	 * same header/tag path as the game's own malloc, so both free routes - the
	 * HeapFree floor and the static-CRT free - already reclaim it. HEAP_ZERO_MEMORY
	 * is honoured. If the arena cannot take it (>= GH_BIG, or full) gh_*_at fall
	 * back to the runtime and the block escapes again, which the tally still
	 * records - so a resize need shows up instead of silently corrupting. */
	if (g_floor_redirect && g_ready) {
		unsigned site = gh_site_of(ret);

		p = gh_floor_arena(n, (flags & HEAP_ZERO_MEMORY) != 0, site);
		if (g_floor_on) {
			InterlockedIncrement(&g_floor_a_calls);
			/* Only a result that reached neither the arena nor a VA block is a
			 * real escape worth recording; owned blocks are not escapes. */
			if (p && !in_range(p) && !gh_va_lookup(p, 0))
				gh_floor_note(ret, p, n);
		}
		return p;
	}
	p = r_floor_halloc ? r_floor_halloc(heap, flags, n) : HeapAlloc(heap, flags, n);
	/* The block lands on whatever heap the game passed - foreign to us. This is
	 * the leaf where the 0ED6BEB0-class decoder object is born when it is NOT
	 * redirected, so the watch has to see it here, not just on the arena paths. */
	gh_watch(p, n, gh_site_of(ret), 0xFFFFFFFFu, "heap-fwd");
	if (g_floor_on) {
		InterlockedIncrement(&g_floor_a_calls);
		gh_floor_note(ret, p, n);
	}
	return p;
}

static LPVOID WINAPI gh_floor_heaprealloc(HANDLE heap, DWORD flags, LPVOID q, SIZE_T n)
{
	void *ret = __builtin_return_address(0);
	LPVOID p;

	/* A realloc of a dedicated VA block: this game never reallocs the floor's
	 * blocks, but never hand a VA pointer to the runtime's realloc either. Grow
	 * by hand - allocate the new size, copy, release the old reservation. */
	if (g_floor_va && q) {
		unsigned long vb = gh_va_lookup(q, 0);

		if (vb) {
			p = gh_floor_arena(n, 0, gh_site_of(ret));
			if (p) {
				CopyMemory(p, q, (size_t)vb < n ? (size_t)vb : n);
				gh_va_lookup(q, 1); /* release the old VA */
			}
			if (g_floor_on)
				InterlockedIncrement(&g_floor_r_calls);
			return p;
		}
	}
	/* Never hand an arena pointer to the runtime; in redirect mode serve a fresh
	 * block from the arena too. An arena q always goes through gh_realloc_at,
	 * whatever the mode, because forwarding it to the real heap would corrupt. */
	if (g_ready && ((q && in_range(q)) || (g_floor_redirect && !q))) {
		p = gh_realloc_at(q, n, gh_site_of(ret));
		if (g_floor_on) {
			InterlockedIncrement(&g_floor_r_calls);
			gh_floor_note(ret, p, n);
		}
		return p;
	}
	p = r_floor_ralloc ? r_floor_ralloc(heap, flags, q, n) : HeapReAlloc(heap, flags, q, n);
	gh_watch(p, n, gh_site_of(ret), 0xFFFFFFFFu, "heaprealloc-fwd");
	if (g_floor_on) {
		InterlockedIncrement(&g_floor_r_calls);
		/* Only on success: a failed realloc leaves the old block live. */
		if (p) {
			if (q)
				floor_live_take(q);
			gh_floor_note(ret, p, n);
		}
	}
	return p;
}

static int on(void)
{
	char v[8];
	DWORD n = savestate_getenv("D3D9SW_GAMEHEAP", v, sizeof(v));

	return n > 0 && n < sizeof(v) && v[0] == '1';
}

/* ------------------------------------------------ VirtualAlloc placement log
 *
 * The cross-session wall is not pointers - the module reloc pass shifts those -
 * and not threads, which the restore reports zero newer than the save. It is the
 * game's own large VirtualAlloc reservations: a dozen MEM_PRIVATE/RW regions,
 * ~340 MB, that no heap of ours claims and that Windows scatters to different
 * addresses every launch. A byte snapshot of one only restores if it comes back
 * at the same base - the very property the pinned arena gives the CRT heap, and
 * the reason a heap at a fixed base reproduces its whole layout.
 *
 * Before pinning any of them we have to know the placement is worth pinning:
 * that the sequence of big reservations - which call site, in what order, at
 * what size - is the SAME up to the save point across two fresh launches. If it
 * is, a deterministic (module, call site, ordinal) key can hand each region a
 * stable slot in a reserved pool. If it is not, this log is what says so, and
 * says what to key on instead.
 *
 * This changes nothing: every call is forwarded to the real VirtualAlloc. It
 * only records, and only for reservations at or above the threshold. Off unless
 * D3D9SW_VALOG=1; D3D9SW_VALOG_KB overrides the 1 MB floor. The return address
 * is resolved to its module so a reservation made from ucrtbase or the game's
 * mod hook is named by its own code, not misread as an executable offset. The
 * offset is module-relative, so it is the same across launches despite ASLR -
 * which is exactly the key a later pinning pass would use. */
static LPVOID (WINAPI *r_valloc)(LPVOID, SIZE_T, DWORD, DWORD);
static int g_valog;
static SIZE_T g_valog_min = 1024u * 1024u;
static volatile LONG g_valog_seq;
/* Reentrancy guard for the LOGGING path only, shared by both hooks. It is a
 * global interlocked try-lock, not a per-thread __thread latch: these hooks run
 * on early and loader threads that do not yet have this DLL's thread-local
 * storage set up, and touching a __thread there dereferences a null TLS array
 * and faults (a mov ecx,[ecx+eax*4] with ecx=0). The allocation itself is always
 * forwarded; only the logging - GetModuleFileNameA, ss_log - is fenced, and a
 * re-entrant or concurrent caller that finds the lock held simply forwards and
 * records nothing, which costs at most a missed log line. */
static volatile LONG g_hooklog;

static LPVOID WINAPI gh_valog(LPVOID addr, SIZE_T size, DWORD type, DWORD prot)
{
	void *ret = __builtin_return_address(0);
	LPVOID (WINAPI *real)(LPVOID, SIZE_T, DWORD, DWORD) =
		r_valloc ? r_valloc : VirtualAlloc;
	LPVOID p = real(addr, size, type, prot);

	if (g_valog && p && size >= g_valog_min && (type & MEM_RESERVE) &&
	    InterlockedCompareExchange(&g_hooklog, 1, 0) == 0) {
		HMODULE m = NULL;
		uintptr_t mb = 0, off = (uintptr_t)ret;
		char path[MAX_PATH], *name = (char *)"?";
		unsigned ord;
		long seq = InterlockedIncrement(&g_valog_seq) - 1;

		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				       (LPCSTR)ret, &m) && m) {
			char *s, *q;

			mb = (uintptr_t)m;
			off = (uintptr_t)ret - mb;
			if (GetModuleFileNameA(m, path, sizeof(path))) {
				for (s = path, q = path; *q; q++)
					if (*q == '\\' || *q == '/')
						s = q + 1;
				name = s;
			}
		}
		/* Keyed on (module-relative site, size), both stable across launches,
		 * so the same site produces the same ordinal run in two sessions - the
		 * thing a determinism diff compares. */
		ord = gh_ordinal((unsigned)off, (unsigned)size);
		ss_log("valog: #%ld  %-22s +%08lX ord %u  size %lu KB  "
		       "type 0x%X prot 0x%X  -> base %08lX%s\n",
		       seq, name, (unsigned long)off, ord,
		       (unsigned long)(size >> 10), (unsigned)type, (unsigned)prot,
		       (unsigned long)(uintptr_t)p,
		       addr ? " (caller-fixed base)" : "");
		InterlockedExchange(&g_hooklog, 0);
	}
	return p;
}

/* One layer down: NtAllocateVirtualMemory, the choke point every private
 * reservation passes through - VirtualAlloc, VirtualAllocEx, and any direct Nt
 * call all funnel here. The VirtualAlloc import net over 21 modules saw nothing,
 * so the game's big regions either resolve VirtualAlloc via GetProcAddress or
 * call Nt directly; either way they cross this line. Inline-hooked (the import
 * table is no use - nobody imports the ntdll stub), log-only, so it names who
 * allocates the 55-70 MB regions and becomes the point a pinning steer later
 * plugs into. D3D9SW_NTLOG=1. */
static LONG (WINAPI *r_ntav)(HANDLE, PVOID *, ULONG_PTR, PSIZE_T, ULONG, ULONG);
static USHORT (NTAPI *r_backtrace)(ULONG, ULONG, PVOID *, PULONG);
static int g_ntlog;
static volatile LONG g_ntstack_left = 12; /* full call-chain dumps for amdxx32 */

/* Basename of the module an address lands in, and the offset within it, into
 * buf. "?" when the address is in no module (heap, stack, jitted). */
static const char *gh_mod_of(const void *addr, char *buf, unsigned cap, uintptr_t *off)
{
	HMODULE m = NULL;

	*off = (uintptr_t)addr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			       (LPCSTR)addr, &m) &&
	    m) {
		char path[MAX_PATH], *s, *q;

		*off = (uintptr_t)addr - (uintptr_t)m;
		if (GetModuleFileNameA(m, path, sizeof(path))) {
			for (s = path, q = path; *q; q++)
				if (*q == '\\' || *q == '/')
					s = q + 1;
			lstrcpynA(buf, s, (int)cap);
			return buf;
		}
	}
	return "?";
}

static int gh_is_sys(const char *n)
{
	return !lstrcmpiA(n, "ntdll.dll") || !lstrcmpiA(n, "kernelbase.dll") ||
	       !lstrcmpiA(n, "kernel32.dll");
}

/* ---- our own VirtualAlloc regions, for the software-exclude path -----------
 *
 * When D3D9SW_SWEXCLUDE arms the Nt hooks, every reservation whose originator is
 * THIS DLL - the software renderer's textures, surfaces, framebuffers and
 * GPU-present staging - is recorded here and dropped when freed, so
 * build_exclusions can hold the live set in the present and out of the snapshot.
 * They are ours and regenerable: the game re-creates its resources and
 * gpu_dxgi_reconcile re-uploads the framebuffer after a restore.
 *
 * Open-addressed with the base as the claim word via CAS, so add and remove need
 * no lock the loader could be holding. The table lives in this DLL's data, which
 * is held in the present, so it survives a restore intact - the regions it names
 * are held in the present too. */
#define GH_OURVA_MAX 8192
static volatile LONG g_ourva[GH_OURVA_MAX]; /* base, 0 = free slot */
static SIZE_T g_ourva_size[GH_OURVA_MAX];
static volatile LONG g_ourva_hw;   /* high-water mark for iteration */
static volatile LONG g_ourva_full; /* adds dropped because the table was full */
static HMODULE g_ourmod SS_PRESENT;           /* this DLL (d3d11.dll) */
static int g_swexcl;

static void ourva_add(uintptr_t base, SIZE_T size)
{
	int i;

	if (!base)
		return;
	for (i = 0; i < GH_OURVA_MAX; i++) {
		if (g_ourva[i] == 0 &&
		    InterlockedCompareExchange(&g_ourva[i], (LONG)base, 0) == 0) {
			LONG hw;

			g_ourva_size[i] = size;
			do {
				hw = g_ourva_hw;
				if (i < hw)
					break;
			} while (InterlockedCompareExchange(&g_ourva_hw, i + 1, hw) != hw);
			return;
		}
	}
	InterlockedIncrement(&g_ourva_full);
}

static void ourva_del(uintptr_t base)
{
	int i, hw = g_ourva_hw;

	if (!base)
		return;
	for (i = 0; i < hw; i++)
		if ((uintptr_t)(LONG)g_ourva[i] == base) {
			InterlockedExchange(&g_ourva[i], 0);
			return;
		}
}

/* A subsystem inside this DLL that must be REWOUND with the game rather than held
 * in the present - the software audio, whose voice and PCM buffers seam into a
 * malformed stream if they stay present-tense while the game's audio state winds
 * back - calls this right after its VirtualAlloc to drop the region the ORIGIN
 * hook just recorded. Renderer memory (the reason SWEXCLUDE exists) is not
 * unheld; only the caller's own regions are. */
void gameheap_va_unhold(void *base)
{
	ourva_del((uintptr_t)base);
}

int gameheap_own_va_ranges(uintptr_t *base, uintptr_t *size, int max)
{
	int i, hw = g_ourva_hw, n = 0;

	for (i = 0; i < hw && n < max; i++) {
		uintptr_t b = (uintptr_t)(LONG)g_ourva[i];

		if (b) {
			base[n] = b;
			size[n] = (uintptr_t)g_ourva_size[i];
			n++;
		}
	}
	return n;
}

/* Every big reservation NTLOG saw, so the save-time census can say which of
 * the big allocations it lists were made after the hooks armed, and from where.
 * One not in here predates gameheap_install. Append-only and lock-free: the
 * slot is claimed before it is filled, and base is written last. */
#define GH_VAORG_CAP 1024
static struct {
	volatile uintptr_t base;
	uintptr_t size, off;
	char mod[24];
} g_vaorg[GH_VAORG_CAP];
static volatile LONG g_vaorg_n;

static void vaorg_add(uintptr_t base, uintptr_t size, const char *mod, uintptr_t off)
{
	LONG i = InterlockedIncrement(&g_vaorg_n) - 1;

	if (i >= GH_VAORG_CAP)
		return;
	g_vaorg[i].size = size;
	g_vaorg[i].off = off;
	lstrcpynA(g_vaorg[i].mod, mod, sizeof(g_vaorg[i].mod));
	MemoryBarrier();
	g_vaorg[i].base = base;
}

/* The i-th reservation NTLOG saw: 1 with base, size and originating module,
 * 0 past the end or for a slot still being filled. */
int gameheap_va_origin_at(int i, uintptr_t *base, uintptr_t *size, const char **mod)
{
	LONG n = g_vaorg_n;

	if (n > GH_VAORG_CAP)
		n = GH_VAORG_CAP;
	if (i < 0 || i >= n || !g_vaorg[i].base)
		return 0;
	*base = g_vaorg[i].base;
	*size = g_vaorg[i].size;
	*mod = g_vaorg[i].mod;
	return 1;
}

int gameheap_va_origin_count(void)
{
	return g_vaorg_n > GH_VAORG_CAP ? GH_VAORG_CAP : (int)g_vaorg_n;
}

/* 1 and "module+offset" in out when NTLOG saw a reservation at exactly base;
 * the latest one wins, since a freed base can be handed out again. */
int gameheap_va_origin(uintptr_t base, char *out, int cap)
{
	LONG n = g_vaorg_n, i;

	if (n > GH_VAORG_CAP)
		n = GH_VAORG_CAP;
	for (i = n - 1; i >= 0; i--)
		if (g_vaorg[i].base == base) {
			char t[48];

			wsprintfA(t, "%.23s+%lX", g_vaorg[i].mod,
				  (unsigned long)g_vaorg[i].off);
			lstrcpynA(out, t, cap);
			return 1;
		}
	return 0;
}

static LONG WINAPI gh_ntav(HANDLE proc, PVOID *base, ULONG_PTR zb, PSIZE_T size,
			   ULONG type, ULONG prot)
{
	void *ret = __builtin_return_address(0);
	PVOID in = base ? *base : NULL;
	/* Forward first, always, on any thread - no thread-local touched here, so a
	 * loader thread without our TLS cannot fault (the __thread version did). */
	LONG st = r_ntav(proc, base, zb, size, type, prot);
	SIZE_T got = size ? *size : 0;
	void *rbase;
	void *bt[32];
	USHORT nfr = 0;
	char onm[64] = "?";
	uintptr_t ooff = 0;
	int ours = 0;

	/* Only a successful self reservation is ours to track or log. MEM_COMMIT
	 * with no base reserves too, and it is how the heap takes every large
	 * block; those are logged, but the exclusion keeps to MEM_RESERVE. */
	if ((!g_ntlog && !g_swexcl) || st < 0 || proc != (HANDLE)(LONG_PTR)-1 ||
	    !((type & MEM_RESERVE) || (!in && (type & MEM_COMMIT))))
		return st;
	rbase = base ? *base : NULL;

	/* Originator: walk past the system layers (this stub, ntdll, kernelbase's
	 * VirtualAllocEx, kernel32) to the first frame in real code. That module is
	 * whose allocation this is - our renderer (hold it in the present) or the
	 * game / Steam (leave it). Needed for both the log and the exclude, so it
	 * runs outside the log lock. The whole frame array is kept so the log can
	 * dump the full chain when it is hunting who pulls a driver in. */
	if (r_backtrace) {
		int fi;

		nfr = r_backtrace(1, 32, bt, NULL);
		for (fi = 0; fi < nfr; fi++) {
			char tn[64];
			uintptr_t to;
			HMODULE fm = NULL;
			const char *tnm = gh_mod_of(bt[fi], tn, sizeof(tn), &to);

			if (tnm[0] == '?' || gh_is_sys(tnm))
				continue;
			lstrcpynA(onm, tnm, sizeof(onm));
			ooff = to;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					   (LPCSTR)bt[fi], &fm);
			ours = (fm && fm == g_ourmod);
			break;
		}
	}

	/* Record ours for exclusion - any size, no scope floor, lock-free table. */
	if (g_swexcl && ours && rbase && (type & MEM_RESERVE))
		ourva_add((uintptr_t)rbase, got);
	if (g_ntlog && rbase && got >= g_valog_min)
		vaorg_add((uintptr_t)rbase, got, onm, ooff);

	if (g_ntlog && got >= g_valog_min &&
	    InterlockedCompareExchange(&g_hooklog, 1, 0) == 0) {
		char inm[64];
		uintptr_t ioff;
		const char *iname = gh_mod_of(ret, inm, sizeof(inm), &ioff);
		long seq = InterlockedIncrement(&g_valog_seq) - 1;

		ss_log("ntlog: #%ld  via %s+%lX  ORIGIN %s+%lX  size %lu KB  type 0x%X "
		       "prot 0x%X  -> base %08lX%s\n",
		       seq, iname, (unsigned long)ioff, onm, (unsigned long)ooff,
		       (unsigned long)(got >> 10), (unsigned)type, (unsigned)prot,
		       (unsigned long)(uintptr_t)rbase, ours ? "  [OURS->held]" : "");
		/* The whole call chain for the driver we are hunting, a few times, so
		 * the frame ABOVE the amdxx32 frames names who actually pulled it in
		 * (our code, the game, DXGI, DWM). Capped so the log stays readable. */
		if (!lstrcmpiA(onm, "amdxx32.dll") && g_ntstack_left > 0 &&
		    InterlockedDecrement(&g_ntstack_left) >= 0) {
			int fi;

			ss_log("  ntstack #%ld - who pulled in amdxx32:\n", seq);
			for (fi = 0; fi < nfr; fi++) {
				char tn[80];
				uintptr_t to;
				const char *tnm = gh_mod_of(bt[fi], tn, sizeof(tn), &to);

				ss_log("      [%2d] %-22s +%lX\n", fi, tnm,
				       (unsigned long)to);
			}
		}
		InterlockedExchange(&g_hooklog, 0);
	}
	return st;
}

/* NtFreeVirtualMemory, so a released renderer region leaves the owned table and
 * cannot go stale - which would hold in the present an address the game may take
 * next. Base is read before the call, since MEM_RELEASE can clear it. Only self
 * releases matter; a decommit keeps the reservation. */
static LONG (WINAPI *r_ntfv)(HANDLE, PVOID *, PSIZE_T, ULONG);

static LONG WINAPI gh_ntfv(HANDLE proc, PVOID *base, PSIZE_T size, ULONG freetype)
{
	uintptr_t b = (g_swexcl && base && proc == (HANDLE)(LONG_PTR)-1 &&
		       (freetype & MEM_RELEASE))
			      ? (uintptr_t)*base
			      : 0;
	LONG st = r_ntfv(proc, base, size, freetype);

	if (b && st >= 0)
		ourva_del(b);
	return st;
}

/* Inline-detour one ntdll export. The x86 WOW64 stub opens with
 * `mov eax, <service number>` (B8 imm32, 5 bytes) - a whole, position-independent
 * instruction - so 5 bytes displace cleanly. Refuse anything else rather than
 * corrupt an unknown prologue. The displaced bytes plus a jump back become the
 * trampoline, returned in *realout for the hook to forward through. */
/* The trampolines' fixed home. The hooks reach them through pointers in our
 * rewound .data, so a load from another launch must find them where the saving
 * launch had them - a per-launch address there was executed by the first
 * thread to allocate after every cross-launch load. 32 bytes per hook. */
#define GH_HOME_TRAMP 0x5FEF0000u

static unsigned char *tramp_alloc(void)
{
	static unsigned char *page;
	static int used;

	if (!page) {
		page = (unsigned char *)VirtualAlloc((void *)GH_HOME_TRAMP, 4096,
						     MEM_COMMIT | MEM_RESERVE,
						     PAGE_EXECUTE_READWRITE);
		if (!page) {
			ss_log("gameheap: NT hook - trampoline home %08lX taken (error %lu); "
			       "placed by the OS, so a load from another launch will jump "
			       "to the old one\n",
			       (unsigned long)GH_HOME_TRAMP, GetLastError());
			page = (unsigned char *)VirtualAlloc(NULL, 4096,
							     MEM_COMMIT | MEM_RESERVE,
							     PAGE_EXECUTE_READWRITE);
		}
		if (!page)
			return NULL;
	}
	if (used + 32 > 4096)
		return NULL;
	used += 32;
	return page + used - 32;
}

static int ntdll_inline_hook(const char *name, void *hookfn, void **realout)
{
	HMODULE nt = GetModuleHandleA("ntdll.dll");
	unsigned char *t = nt ? (unsigned char *)GetProcAddress(nt, name) : NULL;
	unsigned char *tr;
	DWORD old;

	*realout = NULL;
	if (!t) {
		ss_log("gameheap: NT hook - %s not found\n", name);
		return 0;
	}
	/* E9 is an earlier detour - the steam_api.dll shim arms its recorder on
	 * this export before the game starts. Chain behind it: the trampoline is a
	 * jmp to its hook, which forwards to the real stub itself. */
	if (t[0] != 0xB8 && t[0] != 0xE9) {
		ss_log("gameheap: NT hook - %s stub at %p starts 0x%02X, not the expected "
		       "B8; not hooking\n",
		       name, (void *)t, t[0]);
		return 0;
	}
	tr = tramp_alloc();
	if (!tr) {
		ss_log("gameheap: NT hook - trampoline page refused for %s\n", name);
		return 0;
	}
	if (t[0] == 0xE9) {
		unsigned char *prev = t + 5 + *(LONG *)(t + 1);

		tr[0] = 0xE9;
		*(LONG *)(tr + 1) = (LONG)(prev - (tr + 5));
		ss_log("gameheap: NT hook - %s already detoured to %p; chaining behind it\n",
		       name, (void *)prev);
	} else {
		memcpy(tr, t, 5);     /* the displaced mov eax, SSN */
		tr[5] = 0xE9;         /* jmp back to the rest of the stub */
		*(LONG *)(tr + 6) = (LONG)((t + 5) - (tr + 10));
	}
	*realout = tr;
	if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old)) {
		ss_log("gameheap: NT hook - could not make %s writable\n", name);
		*realout = NULL;
		return 0;
	}
	t[0] = 0xE9; /* jmp hookfn */
	*(LONG *)(t + 1) = (LONG)((unsigned char *)hookfn - (t + 5));
	VirtualProtect(t, 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), t, 5);
	ss_log("gameheap: NT hook - %s inline-detoured at %p, trampoline %p\n", name,
	       (void *)t, (void *)tr);
	return 1;
}

/* Arms the allocation hook (always) and, for the software-exclude path, the free
 * hook too. g_ntlog / g_swexcl must already be set so the detours behave the
 * moment they are wired. */
static int nt_hooks_install(int with_free)
{
	HMODULE nt = GetModuleHandleA("ntdll.dll");

	if (!nt)
		return 0;
	r_backtrace = (USHORT(NTAPI *)(ULONG, ULONG, PVOID *, PULONG))GetProcAddress(
		nt, "RtlCaptureStackBackTrace");
	/* This DLL, so the originator walk can tell our own reservations apart. */
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCSTR)&gh_ntav, &g_ourmod);
	if (!ntdll_inline_hook("NtAllocateVirtualMemory", (void *)gh_ntav,
			       (void **)&r_ntav)) {
		g_ntlog = g_swexcl = 0;
		return 0;
	}
	if (with_free) {
		if (!ntdll_inline_hook("NtFreeVirtualMemory", (void *)gh_ntfv,
				       (void **)&r_ntfv))
			ss_log("gameheap: SWEXCLUDE - free hook failed; held regions would "
			       "go stale on free, so exclusion is UNSAFE - disabling it\n");
		if (!r_ntfv)
			g_swexcl = 0; /* no free tracking -> do not record, to stay safe */
	}
	ss_log("gameheap: NT hooks armed - alloc%s, this DLL %p. %s\n",
	       (with_free && r_ntfv) ? "+free" : "", (void *)g_ourmod,
	       g_swexcl ? "SWEXCLUDE: renderer VirtualAlloc regions are held in the "
			  "present and rebuilt on restore"
		        : (g_ntlog ? "NTLOG: logging only" : "inactive"));
	return 1;
}

int gameheap_install(void)
{
	HMODULE exe = GetModuleHandleA(NULL);
	unsigned char *pool;
	int live = 0, bad = 0, wired = 0;
	int i;

	g_sites[0].ours = (void *)gh_malloc;
	g_sites[1].ours = (void *)gh_calloc;
	g_sites[2].ours = (void *)gh_realloc;
	g_sites[3].ours = (void *)gh_free;
	g_sites[4].ours = (void *)gh_msize;
	g_sites[5].ours = (void *)gh_recalloc;
	g_sites[6].ours = (void *)gh_expand;

	if (g_ready || !on())
		return 0;
	if (!exe)
		return 0;
	/* Before any allocation is served, because the free path reads the result
	 * and the block we most want to watch is freed during startup. */
	gh_peek_parse();
	gh_watch_parse();

	/* Look before touching anything. */
	for (i = 0; i < GH_SITES; i++) {
		if (!g_sites[i].rva)
			continue;
		live++;
		if (site_ok(exe, &g_sites[i]))
			continue;
		bad++;
		ss_log("gameheap: %-10s at +%08X does not begin with the bytes this "
		       "build was told to expect\n",
		       g_sites[i].name, g_sites[i].rva);
	}
	if (!live || bad) {
		ss_log("gameheap: %d of %d allocator site(s) did not match, so nothing "
		       "was touched. These offsets were read from one build of one "
		       "executable and mean nothing against another\n",
		       bad, live);
		return 0;
	}

	/* Executable, and ours, so nothing the game does can reclaim it. */
	pool = (unsigned char *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE,
					     PAGE_EXECUTE_READWRITE);
	if (!pool) {
		ss_log("gameheap: no memory for trampolines, error %lu\n", GetLastError());
		return 0;
	}
	gh_trace_arm();
	g_heap = gh_create_heap();
	if (!g_heap) {
		ss_log("gameheap: HeapCreate failed, error %lu\n", GetLastError());
		VirtualFree(pool, 0, MEM_RELEASE);
		return 0;
	}
	gh_create_laa_region(); /* opt-in; no-op unless D3D9SW_LAA_FALSE is set */
	if (gh_wholesale()) {
		/* Before a single allocation, so the LFH never gets a foothold and every
		 * block this heap serves is placed by the standard front end whose state
		 * lives entirely inside the region we rewind. */
		gh_make_selfcontained(g_heap);
		if (g_laa_heap)
			gh_make_selfcontained(g_laa_heap);
		ss_log("gameheap: WHOLESALE - LFH disabled on the owned heap(s); the "
		       "savestate engine may now restore their whole region atomically, "
		       "freelist and all, instead of block by block\n");
	}
	InitializeCriticalSection(&g_cs);
	savestate_own_cs(&g_cs);

	/* Always, not only under GHTRACE: the tag table names blocks for a
	 * diagnostic; this table IS the live set restore walks. Excluded so a
	 * rewind cannot take the journal away while it is being used to rewind
	 * the heap the journal describes. */
	g_busy = (GhBusy *)VirtualAlloc(NULL, GH_BUSY_SLOTS * sizeof(GhBusy),
					MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	g_busy_snap = (GhBusySnap *)VirtualAlloc(NULL,
						GH_BUSY_SLOTS * sizeof(GhBusySnap),
						MEM_COMMIT | MEM_RESERVE,
						PAGE_READWRITE);
	if (g_busy && g_busy_snap) {
		g_busy_snap_cap = GH_BUSY_SLOTS;
		savestate_exclude(g_busy, GH_BUSY_SLOTS * sizeof(GhBusy));
		savestate_exclude(g_busy_snap,
				  GH_BUSY_SLOTS * sizeof(GhBusySnap));
		ss_log("gameheap: busy-block sidetable at %p (%u slots), snapshot "
		       "at %p - Wine restore walks this instead of HeapWalk or a "
		       "linear GhHead scan\n",
		       (void *)g_busy, GH_BUSY_SLOTS, (void *)g_busy_snap);
	} else {
		if (g_busy) {
			VirtualFree(g_busy, 0, MEM_RELEASE);
			g_busy = NULL;
		}
		if (g_busy_snap) {
			VirtualFree(g_busy_snap, 0, MEM_RELEASE);
			g_busy_snap = NULL;
		}
		ss_log("gameheap: busy-block sidetable reservation failed - restore "
		       "falls back to scanning saved bytes for GhHead magic\n");
	}

	/* Every forwarding path complete before any jump exists, then ready, then
	 * the jumps. A call arriving midway through the last step reaches a
	 * handler that can already forward. */
	for (i = 0; i < GH_SITES; i++)
		if (g_sites[i].rva)
			site_tramp(exe, &g_sites[i], pool + (size_t)i * 16);
	g_ready = 1;
	for (i = 0; i < GH_SITES; i++) {
		if (!g_sites[i].rva)
			continue;
		if (site_wire(exe, &g_sites[i]))
			wired++;
		else
			ss_log("gameheap: %-10s could not be made writable\n",
			       g_sites[i].name);
	}
	{
		void *prev = NULL;
		/* By name. kernel32!HeapFree is a forwarder to ntdll!RtlFreeHeap,
		 * so the address GetProcAddress returns is not necessarily the one
		 * in the import slot - which is why matching by address found
		 * nothing and said so. */
		int n = savestate_patch_iat_named(exe, "KERNEL32.dll", "HeapFree",
						  (void *)gh_heapfree, &prev);

		if (!n)
			n = savestate_patch_iat_named(exe, NULL, "HeapFree",
						      (void *)gh_heapfree, &prev);
		r_heapfree = n ? (BOOL(WINAPI *)(HANDLE, DWORD, LPVOID))prev : NULL;
		ss_log("gameheap: HeapFree %d import slot(s) patched as a floor under "
		       "every freer in the executable, the three that are not "
		       "allocators included%s\n",
		       n, n ? "" : " - the census below cannot measure what it claims");
	}
	{
		char floorv[16];

		if (savestate_getenv("D3D9SW_GHFLOOR", floorv, sizeof(floorv)) > 0 &&
		    floorv[0] >= '1' && floorv[0] <= '9') {
			void *prev = NULL;
			int na, nr;

			InitializeCriticalSection(&g_floor_cs);

			savestate_own_cs(&g_floor_cs);
			g_floor_cs_ready = 1;
			g_floor_on = 1;
			g_floor_redirect = floorv[0] >= '2';
			g_floor_va = floorv[0] >= '3';
			na = savestate_patch_iat_named(exe, "KERNEL32.dll", "HeapAlloc",
						       (void *)gh_floor_heapalloc, &prev);
			if (!na)
				na = savestate_patch_iat_named(exe, NULL, "HeapAlloc",
							       (void *)gh_floor_heapalloc, &prev);
			r_floor_halloc = na ? (LPVOID(WINAPI *)(HANDLE, DWORD, SIZE_T))prev : NULL;
			prev = NULL;
			nr = savestate_patch_iat_named(exe, "KERNEL32.dll", "HeapReAlloc",
						       (void *)gh_floor_heaprealloc, &prev);
			if (!nr)
				nr = savestate_patch_iat_named(exe, NULL, "HeapReAlloc",
							       (void *)gh_floor_heaprealloc, &prev);
			r_floor_ralloc = nr ? (LPVOID(WINAPI *)(HANDLE, DWORD, LPVOID, SIZE_T))prev
					    : NULL;
			ss_log("gameheap: allocation floor %s%s - HeapAlloc %d slot(s), "
			       "HeapReAlloc %d slot(s) patched. %s\n",
			       g_floor_redirect ? "REDIRECT" : "LOG-ONLY",
			       g_floor_va ? " + VA-BIG (experimental, not >1-save safe)" : "",
			       na, nr,
			       g_floor_redirect
				       ? "off-arena HeapAlloc is served from the pinned arena; "
					 "escape count should fall to zero"
				       : "nothing is redirected; the outline prints in the report");
		}
	}
	{
		char vv[16];

		if (savestate_getenv("D3D9SW_VALOG", vv, sizeof(vv)) > 0 && vv[0] == '1') {
			/* Every loaded module's import of VirtualAlloc, not just the exe and
			 * the CRT: the first pass hooked those two and saw nothing, so the
			 * 55-70 MB regions a cross-session restore drops are placed from some
			 * other module (or below kernel32 entirely). Cast the net over all of
			 * them, minus our own shims, whose reservations we do not carry. If
			 * this still shows nothing, the path is GetProcAddress-resolved or a
			 * direct Nt call and the hook has to go one layer down. */
			static const char *const ours[] = {
				"d3d11.dll", "dxgi.dll", "xaudio2_9.dll", "xinput1_4.dll",
				"dsound.dll", "opengl32.dll", "d3d9.dll", "d3d9_sw.dll",
				/* The core allocators: hooking their VirtualAlloc import is
				 * both pointless (heap growth goes through
				 * NtAllocateVirtualMemory, not kernel32!VirtualAlloc, and
				 * these do not call their own export via an import) and
				 * hazardous - they run under the loader lock, where our
				 * logging path can deadlock. The game's own big allocations
				 * come from the exe and its libraries, never from here. */
				"ntdll.dll", "kernel32.dll", "kernelbase.dll"
			};
			char kb[16];
			int total = 0, hooked = 0, skipped = 0;
			HANDLE snap;

			if (savestate_getenv("D3D9SW_VALOG_KB", kb, sizeof(kb)) > 0) {
				unsigned k = 0, ci;

				for (ci = 0; kb[ci] >= '0' && kb[ci] <= '9'; ci++)
					k = k * 10u + (unsigned)(kb[ci] - '0');
				if (k)
					g_valog_min = (SIZE_T)k * 1024u;
			}
			snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
			if (snap != INVALID_HANDLE_VALUE) {
				MODULEENTRY32 me;

				me.dwSize = sizeof(me);
				if (Module32First(snap, &me)) {
					do {
						unsigned oi, mine = 0;
						void *prev = NULL;
						int n;

						for (oi = 0; oi < sizeof(ours) / sizeof(ours[0]); oi++)
							if (!lstrcmpiA(me.szModule, ours[oi])) {
								mine = 1;
								break;
							}
						if (mine) {
							skipped++;
							continue;
						}
						n = savestate_patch_iat_named((HMODULE)me.hModule,
									      "KERNEL32.dll",
									      "VirtualAlloc",
									      (void *)gh_valog, &prev);
						if (!n)
							n = savestate_patch_iat_named(
								(HMODULE)me.hModule, NULL,
								"VirtualAlloc", (void *)gh_valog,
								&prev);
						if (n && prev && !r_valloc)
							r_valloc = (LPVOID(WINAPI *)(
								LPVOID, SIZE_T, DWORD, DWORD))prev;
						if (n)
							hooked++;
						total += n;
					} while (Module32Next(snap, &me));
				}
				CloseHandle(snap);
			}
			g_valog = total > 0;
			ss_log("gameheap: VALOG %s - %d VirtualAlloc import slot(s) across %d "
			       "module(s) (%d of ours skipped), logging reservations >= %lu KB "
			       "by (module, call site, ordinal). Forwarded unchanged; only "
			       "records placement%s\n",
			       g_valog ? "ON" : "FAILED", total, hooked, skipped,
			       (unsigned long)(g_valog_min >> 10),
			       g_valog ? "" : " - no VirtualAlloc import found to patch");
		}
	}
	{
		char nl[8], sx[8];
		int ntlog = savestate_getenv("D3D9SW_NTLOG", nl, sizeof(nl)) > 0 &&
			    nl[0] == '1';
		int swx = savestate_getenv("D3D9SW_SWEXCLUDE", sx, sizeof(sx)) > 0 &&
			  sx[0] == '1';

		if (ntlog || swx) {
			g_ntlog = ntlog;
			g_swexcl = swx;
			nt_hooks_install(swx); /* alloc always; free hook only for exclude */
		}
	}
	clock_probe_install(exe);
	savestate_game_heap(g_heap);
	ss_log("gameheap: %d of %d allocator site(s) in the game's own code now run "
	       "on a private heap at %p. The runtime is linked statically, so these "
	       "are the game's copies and nothing Windows uses passes through them. "
	       "Allocations under %lu KB from here on have no other tenant, which is "
	       "the arrangement DDPR gets from MSVCR100 for free\n",
	       wired, live, (void *)g_heap, (unsigned long)(GH_BIG >> 10));
	return wired;
}

/* ------------------------------------- import mode (D3D9SW_GAMEHEAP=2)
 *
 * The same private heap for a game that links the C runtime as a DLL, which is
 * the easy case the detours above had to work around. Haydee is one: every one
 * of its modules - haydee.dll, game.dll and the three PhysX DLLs - takes malloc
 * and friends from api-ms-win-crt-heap, which is ucrtbase, which allocates from
 * the process heap. That shared heap is what the restore could neither rewind
 * (Windows keeps low-fragmentation bookkeeping for it outside its segments, and
 * the first allocation after a restore divided by a zero block count) nor hold
 * back (the game's objects would stay in the future).
 *
 * So the import slots are pointed at us, module by module, and the game's
 * allocations land in a heap nothing else uses. Selected by '2' rather than '1'
 * so a game configured for the detour build is never touched by this one, and
 * installed only from the OpenGL wrapper's attach, which is the one build that
 * runs early enough to see these modules before their own initialisation.
 *
 * What makes it safe to be partial: frees dispatch per pointer, so blocks the
 * runtime issued before the install still go back to it; and the runtime's own
 * HeapFree, HeapReAlloc and HeapSize imports are patched as a floor, so a block
 * of ours that reaches ucrtbase by any route - another module's free, a realloc
 * inside the runtime, _aligned_free - is caught there instead of being handed
 * to the process heap. */
typedef void *(__cdecl *PFN_aligned_malloc)(size_t, size_t);

static LPVOID(WINAPI *r_crt_hra)(HANDLE, DWORD, LPVOID, SIZE_T);
static SIZE_T(WINAPI *r_crt_hsz)(HANDLE, DWORD, LPCVOID);
static WCHAR g_game_dir[MAX_PATH];
static int g_game_dir_n;
static HMODULE g_self SS_PRESENT;
static const char *g_rt_name = "ucrtbase";

static int gh_exe_imports(const char *dll)
{
	unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
	IMAGE_IMPORT_DESCRIPTOR *imp;

	if (!rva)
		return 0;
	for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva); imp->Name; imp++)
		if (!lstrcmpiA((const char *)(base + imp->Name), dll))
			return 1;
	return 0;
}
static unsigned long g_mods_patched, g_slots_patched, g_mods_unbound;

/* ucrtbase's layout, reproduced exactly: the block the allocator issued, stored
 * in the pointer-sized word below the aligned address. That is what lets each
 * side free the other's blocks - the runtime's _aligned_free reads our word and
 * frees our block through its free, which the floor catches; ours reads the
 * runtime's word and frees its block through gh_free, which forwards it. */
static void *__cdecl gh_aligned_malloc(size_t n, size_t a)
{
	uintptr_t raw, p;

	if (!a || (a & (a - 1)))
		return NULL;
	if (a < sizeof(void *))
		a = sizeof(void *);
	if (n > (size_t)-1 - a - sizeof(void *))
		return NULL;
	raw = (uintptr_t)gh_malloc_at(n + a - 1 + sizeof(void *),
				      gh_site_of(__builtin_return_address(0)));
	if (!raw)
		return NULL;
	p = (raw + sizeof(void *) + a - 1) & ~(uintptr_t)(a - 1);
	((uintptr_t *)p)[-1] = raw;
	return (void *)p;
}

static void __cdecl gh_aligned_free(void *p)
{
	if (p)
		gh_free((void *)((uintptr_t *)((uintptr_t)p & ~(uintptr_t)(sizeof(void *) - 1)))[-1]);
}

static LPVOID WINAPI gh_crt_heaprealloc(HANDLE heap, DWORD flags, LPVOID p, SIZE_T n)
{
	int orphan = 0;
	GhHead *h = g_ready ? ours_why(p, &orphan) : NULL;

	if (h) {
		g_caught++;
		if (flags & HEAP_REALLOC_IN_PLACE_ONLY)
			return gh_expand(p, n);
		return gh_realloc_at(p, n ? n : 1, 0xFFFFFFFFu);
	}
	if (orphan) {
		gh_orphan_note("HeapReAlloc", p);
		return NULL;
	}
	if (g_ntlog && n >= 0x10000) {
		static volatile LONG left = 32;
		char m[32];
		uintptr_t o;

		if (InterlockedDecrement(&left) >= 0)
			ss_log("gameheap: runtime HeapReAlloc passed through - %p to %lu bytes, "
			       "from %s+%lX\n",
			       p, (unsigned long)n,
			       gh_mod_of(__builtin_return_address(0), m, sizeof(m), &o),
			       (unsigned long)o);
	}
	return r_crt_hra(heap, flags, p, n);
}

static SIZE_T WINAPI gh_crt_heapsize(HANDLE heap, DWORD flags, LPCVOID p)
{
	int orphan = 0;
	GhHead *h = g_ready ? ours_why((void *)p, &orphan) : NULL;

	if (h)
		return h->size;
	if (orphan) {
		gh_orphan_note("HeapSize", (void *)p);
		return 0;
	}
	return r_crt_hsz(heap, flags, p);
}

/* D3D9SW_GHRT_ALLOC=1: the runtime's own HeapAlloc on its own heap - the
 * allocations it makes for itself and for MSVCP100 - is served from our arena
 * too. That heap is made before this DLL loads, moves between launches, and
 * grows segments wherever there is room. The floor above already frees, resizes
 * and sizes these blocks. */
static LPVOID(WINAPI *r_rt_halloc)(HANDLE, DWORD, SIZE_T);
static HANDLE g_rt_crtheap SS_PRESENT;
static volatile LONG g_rt_served, g_rt_passed;

static LPVOID WINAPI gh_rt_heapalloc(HANDLE heap, DWORD flags, SIZE_T n)
{
	if (g_ready && heap == g_rt_crtheap) {
		unsigned site = gh_site_of(__builtin_return_address(0));
		void *raw = n >= GH_BIG && g_big_chunk
				    ? big_alloc(n + sizeof(GhHead))
				    : gh_halloc(gh_serving_heap(), flags & HEAP_ZERO_MEMORY,
						n + sizeof(GhHead), site);

		if (raw) {
			if ((flags & HEAP_ZERO_MEMORY) && n >= GH_BIG)
				memset((char *)raw + sizeof(GhHead), 0, n);
			InterlockedIncrement(&g_rt_served);
			return give(raw, n, site);
		}
	}
	if (InterlockedIncrement(&g_rt_passed) <= 64 && g_ntlog) {
		void *fr[4] = { 0 };
		char m[4][32];
		uintptr_t o[4];
		int k, nf = RtlCaptureStackBackTrace(1, 4, fr, NULL);

		for (k = 0; k < 4; k++) {
			const char *s = "-";

			o[k] = 0;
			if (k < nf)
				s = gh_mod_of(fr[k], m[k], 32, &o[k]);
			if (s != m[k])
				lstrcpynA(m[k], s, 32);
		}
		ss_log("gameheap: runtime HeapAlloc passed through - heap %p%s, %lu bytes, "
		       "from %s+%lX < %s+%lX < %s+%lX < %s+%lX\n",
		       (void *)heap,
		       !g_ready ? " (not ready)"
		       : heap == g_rt_crtheap ? " (ours failed)"
					      : " (not the runtime's heap)",
		       (unsigned long)n, m[0], (unsigned long)o[0], m[1], (unsigned long)o[1],
		       m[2], (unsigned long)o[2], m[3], (unsigned long)o[3]);
	}
	return r_rt_halloc(heap, flags, n);
}

/* One import slot by name. Unlike savestate_patch_iat_named this records the
 * original in *real BEFORE the slot changes, so a thread that arrives through
 * the new slot never finds a forwarding pointer still unset; and it refuses a
 * slot the loader has not yet resolved, which would otherwise be overwritten
 * with the real address the moment it was. */
static int gh_iat_swap(HMODULE mod, const char *dll, const char *fn, void *to, void **real)
{
	unsigned char *base = (unsigned char *)mod;
	IMAGE_NT_HEADERS *nt;
	IMAGE_IMPORT_DESCRIPTOR *imp;
	DWORD rva;
	int n = 0;

	if (!mod || ((IMAGE_DOS_HEADER *)base)->e_magic != IMAGE_DOS_SIGNATURE)
		return 0;
	nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return 0;
	rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
	if (!rva)
		return 0;
	for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva); imp->Name; imp++) {
		IMAGE_THUNK_DATA *orig, *cur;

		if (dll && lstrcmpiA((const char *)(base + imp->Name), dll))
			continue;
		if (!imp->OriginalFirstThunk)
			continue;
		orig = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
		cur = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
		for (; orig->u1.AddressOfData; orig++, cur++) {
			DWORD old;

			if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG)
				continue;
			if (lstrcmpA((const char *)((IMAGE_IMPORT_BY_NAME *)(base + orig->u1.AddressOfData))->Name,
				     fn))
				continue;
			if (cur->u1.Function == orig->u1.AddressOfData)
				return -1;
			if (cur->u1.Function == (ULONG_PTR)to) {
				n++;
				continue;
			}
			if (real && !*real)
				*real = (void *)cur->u1.Function;
			if (!VirtualProtect(cur, sizeof(void *), PAGE_READWRITE, &old))
				continue;
			cur->u1.Function = (ULONG_PTR)to;
			VirtualProtect(cur, sizeof(void *), old, &old);
			n++;
		}
	}
	return n;
}

static const struct {
	const char *name;
	void *ours;
} kImpFns[] = {
	{ "malloc", (void *)gh_malloc },
	{ "calloc", (void *)gh_calloc },
	{ "realloc", (void *)gh_realloc },
	{ "free", (void *)gh_free },
	{ "_msize", (void *)gh_msize },
	{ "_recalloc", (void *)gh_recalloc },
	{ "_expand", (void *)gh_expand },
	{ "_aligned_malloc", (void *)gh_aligned_malloc },
	{ "_aligned_free", (void *)gh_aligned_free },
	/* MSVCR100 exports operator new and delete, and DDPR's modules import them
	 * along with _malloc_crt; ucrt games link them statically instead. */
	{ "_malloc_crt", (void *)gh_malloc },
	{ "??2@YAPAXI@Z", (void *)gh_malloc },
	{ "??_U@YAPAXI@Z", (void *)gh_malloc },
	{ "??3@YAXPAX@Z", (void *)gh_free },
	{ "??_V@YAXPAX@Z", (void *)gh_free },
};

/* The runtime whose imports are redirected: ucrtbase by default, MSVCR100 when
 * the executable links it. Only the chosen runtime's names are patched, because
 * a block we do not own is handed back to that runtime's free. */
static const char *kImpDlls[] = { "api-ms-win-crt-heap-l1-1-0.dll", "ucrtbase.dll" };

/* Our other wrappers sit in the game folder too and keep the runtime's heap.
 * This one does not: its GL object tables describe the same moment the game's
 * state does and have always rewound with it, back when that meant the process
 * heap. The engine in this module never calls malloc, so nothing it holds
 * across a restore lands in the heap being restored. */
static const char *const kNotGame[] = { "opengl32.dll", "d3d9.dll",	 "d3d9_sw.dll",
					"d3d11.dll",	"dxgi.dll",	 "dsound.dll",
					"xaudio2_9.dll", "xinput1_4.dll" };

static int gh_is_game_module(HMODULE mod, const WCHAR *path)
{
	const WCHAR *leaf = path;
	char base[64];
	unsigned i;
	int k;

	if (mod == g_self)
		return 1;
	if (!g_game_dir_n)
		return 0;
	for (k = 0; k < g_game_dir_n; k++) {
		WCHAR a = path[k], b = g_game_dir[k];

		if (!a)
			return 0;
		if (a >= 'A' && a <= 'Z')
			a += 32;
		if (b >= 'A' && b <= 'Z')
			b += 32;
		if (a != b)
			return 0;
	}
	for (i = 0; path[i]; i++)
		if (path[i] == '\\' || path[i] == '/')
			leaf = path + i + 1;
	for (i = 0; leaf[i] && i < sizeof(base) - 1; i++)
		base[i] = (char)leaf[i];
	base[i] = 0;
	for (i = 0; i < sizeof(kNotGame) / sizeof(kNotGame[0]); i++)
		if (!lstrcmpiA(base, kNotGame[i]))
			return 0;
	return 1;
}

/* The game's own VirtualAlloc and VirtualFree, journalled into a ring held in
 * the present so the history from before a restore is still there after it.
 *
 * Haydee keeps arenas of VirtualAlloc'd blocks chained through a word at +0x0C,
 * and after a room-to-room restore its arena reset read the chain into a block
 * that was already released. Whether that block was freed earlier in the same
 * walk, by another reset, or never put back by the restore is a question only
 * the order of the calls answers, and the fault report prints this ring. */
typedef struct {
	char op; /* A alloc, F free, R restore */
	DWORD tid;
	uintptr_t addr, size, ret, caller;
	DWORD how;
} GhVaEv;

#define GH_VAJ_N 1024u
typedef struct {
	volatile LONG next;
	GhVaEv e[GH_VAJ_N];
} GhVaJ;

static GhVaJ *g_vaj SS_PRESENT;
static int g_vaj_held;
static LPVOID(WINAPI *r_va)(LPVOID, SIZE_T, DWORD, DWORD);
static BOOL(WINAPI *r_vf)(LPVOID, SIZE_T, DWORD);

/* Fixed homes for our held-in-the-present buffers, under the PhysX tracer's.
 * The buffers are excluded from saves but the pointers to them are in our own
 * rewound .data, so a load from another launch must find them where the saving
 * launch had them. The raw span stops at GH_HOMES for this. */
#define GH_HOMES 0x5FC00000u
#define GH_HOME_RES 0x5FC00000u
#define GH_HOME_VAJ 0x5FCA0000u
#define GH_HOME_AL 0x5FCB0000u

static void *gh_home_alloc(uintptr_t home, SIZE_T size)
{
	LPVOID(WINAPI * va)(LPVOID, SIZE_T, DWORD, DWORD) = r_va ? r_va : VirtualAlloc;
	void *p = va((void *)home, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

	if (!p) {
		ss_log("gameheap: fixed home %08lX (%lu KB) taken - placed by the OS, so a "
		       "load from another launch will point at the old one\n",
		       (unsigned long)home, (unsigned long)(size >> 10));
		p = va(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	}
	return p;
}

static GhVaEv *vaj_put(char op, uintptr_t addr, uintptr_t size, uintptr_t ret, DWORD how,
		       void *caller)
{
	GhVaEv *e;

	if (!g_vaj)
		return NULL;
	e = &g_vaj->e[(unsigned)(InterlockedIncrement(&g_vaj->next) - 1) % GH_VAJ_N];
	e->op = op;
	e->tid = GetCurrentThreadId();
	e->addr = addr;
	e->size = size;
	e->ret = ret;
	e->how = how;
	e->caller = (uintptr_t)caller;
	return e;
}

/* Every reservation the game makes itself, by 64 KB granule, held in the
 * present like the journal so it describes the address space as it really is.
 *
 * Haydee reserves its arena blocks one granule at a time, thousands of them,
 * and releases them wholesale on a room change. Two things go wrong across a
 * restore without this. A block the game released after a save leaves a hole
 * the save still needs, and a new thread's TEB - placed at page granularity -
 * can land in it, refusing every later restore of that save. And blocks the
 * game reserved after a save are unknown to the rewound game, so each restore
 * leaked them, and the next save captured the leak: 237 regions grew to 8392. */
enum { GH_RES_FREE, GH_RES_LIVE, GH_RES_PARKED };
typedef struct {
	volatile LONG seq;
	LONG parked, released, held_again;
	struct {
		LONG born;
		unsigned char state;
	} g[65536];
} GhRes;

static GhRes *g_res SS_PRESENT;
_Static_assert(sizeof(GhRes) <= GH_HOME_VAJ - GH_HOME_RES, "GhRes outgrew its home");
_Static_assert(sizeof(GhVaJ) <= GH_HOME_AL - GH_HOME_VAJ, "GhVaJ outgrew its home");
static int g_res_held;

int savestate_alloc_saved(uintptr_t alloc_base, int slotno);

/* D3D9SW_GHRAW_PIN (default on): the game's own VirtualAlloc(NULL, ...)
 * reservations are carved by 64 KB granule from one span at a fixed address,
 * instead of wherever Windows puts them. A release
 * decommits and frees the granules; the span itself is never given back. The
 * map is in our image's data, so it rewinds with the game and always agrees
 * with what the restored game thinks it holds. Placed by Windows, these blocks
 * were the regions a cross-session load still refused once modules and chunks
 * stood still. */
#define GH_RAW_GRAN 0x10000u
#define GH_RAW_MAX 8192u
static uintptr_t g_raw_base;
static unsigned g_raw_n, g_raw_full_said;
static unsigned short g_raw_len[GH_RAW_MAX]; /* run length at its first granule */
static unsigned char g_raw_used[GH_RAW_MAX];
static volatile LONG g_raw_lock;

static void raw_reserve(uintptr_t base, uintptr_t top)
{
	SIZE_T size;

	if (!gh_knob("D3D9SW_GHRAW_PIN", 1) || top <= base)
		return;
	size = top - base;
	if (size > (SIZE_T)GH_RAW_MAX * GH_RAW_GRAN)
		size = (SIZE_T)GH_RAW_MAX * GH_RAW_GRAN;
	for (; size >= 64u << 20; size -= 64u << 20)
		if (VirtualAlloc((LPVOID)base, size, MEM_RESERVE, PAGE_READWRITE) ==
		    (LPVOID)base) {
			g_raw_base = base;
			g_raw_n = (unsigned)(size / GH_RAW_GRAN);
			ss_log("gameheap: the game's own reservations PINNED in %lu MB at "
			       "%08lX\n",
			       (unsigned long)(size >> 20), (unsigned long)base);
			return;
		}
	ss_log("gameheap: could not pin a span for the game's own reservations at %08lX "
	       "(error %lu) - Windows places them\n",
	       (unsigned long)base, GetLastError());
}

static LPVOID raw_take(SIZE_T size, DWORD type, DWORD prot)
{
	unsigned need = (unsigned)((size + GH_RAW_GRAN - 1) / GH_RAW_GRAN), i, run = 0;
	LPVOID p = NULL;

	if (!need || need > 0xFFFFu)
		return NULL;
	while (InterlockedCompareExchange(&g_raw_lock, 1, 0))
		YieldProcessor();
	for (i = 0; i < g_raw_n; i++) {
		run = g_raw_used[i] ? 0 : run + 1;
		if (run == need)
			break;
	}
	if (run == need) {
		unsigned at = i + 1 - need, k;

		p = (LPVOID)(g_raw_base + (uintptr_t)at * GH_RAW_GRAN);
		/* A restore rewinds the map but not the pages: granules taken after
		 * the save read as free yet may still be committed, and the game is
		 * owed zeroed memory. */
		r_vf(p, (SIZE_T)need * GH_RAW_GRAN, MEM_DECOMMIT);
		if ((type & MEM_COMMIT) && !r_va(p, size, MEM_COMMIT, prot))
			p = NULL;
		else {
			for (k = at; k < at + need; k++)
				g_raw_used[k] = 1;
			g_raw_len[at] = (unsigned short)need;
		}
	} else if (g_raw_full_said < 8) {
		unsigned taken = 0, k;

		for (k = 0; k < g_raw_n; k++)
			taken += g_raw_used[k];
		g_raw_full_said++;
		ss_log("gameheap: %lu KB does not fit in the pinned span for the game's own "
		       "reservations (%u of %u 64 KB granules taken) - placed by Windows "
		       "instead\n",
		       (unsigned long)(size >> 10), taken, g_raw_n);
	}
	g_raw_lock = 0;
	return p;
}

/* 1 if addr was ours to release (and is now released), 0 if not in the span. */
static int raw_give(LPVOID addr)
{
	uintptr_t a = (uintptr_t)addr;
	unsigned at, k, n;

	if (!g_raw_base || a < g_raw_base || a >= g_raw_base + (uintptr_t)g_raw_n * GH_RAW_GRAN)
		return 0;
	at = (unsigned)((a - g_raw_base) / GH_RAW_GRAN);
	while (InterlockedCompareExchange(&g_raw_lock, 1, 0))
		YieldProcessor();
	n = g_raw_len[at];
	if (n && g_raw_used[at]) {
		r_vf(addr, (SIZE_T)n * GH_RAW_GRAN, MEM_DECOMMIT);
		for (k = at; k < at + n && k < g_raw_n; k++)
			g_raw_used[k] = 0;
		g_raw_len[at] = 0;
	}
	g_raw_lock = 0;
	return 1;
}

static LPVOID WINAPI gh_va_j(LPVOID addr, SIZE_T size, DWORD type, DWORD prot)
{
	LPVOID p;

	if (!addr && (type & MEM_RESERVE) && g_raw_base && !(type & (MEM_PHYSICAL | MEM_LARGE_PAGES)) &&
	    (p = raw_take(size, type, prot)) != NULL) {
		vaj_put('A', 0, size, (uintptr_t)p, type, __builtin_return_address(0));
		return p;
	}
	p = r_va(addr, size, type, prot);

	if (!(type & MEM_RESERVE) && p == addr)
		return p; /* commit inside an existing reservation: too frequent to keep */
	vaj_put('A', (uintptr_t)addr, size, (uintptr_t)p, type, __builtin_return_address(0));
	if (g_res && p && (type & MEM_RESERVE) && !((uintptr_t)p & 0xFFFF)) {
		unsigned i = (unsigned)((uintptr_t)p >> 16);
		g_res->g[i].born = InterlockedIncrement(&g_res->seq);
		g_res->g[i].state = GH_RES_LIVE;
	}
	return p;
}

static BOOL WINAPI gh_vf_j(LPVOID addr, SIZE_T size, DWORD type)
{
	GhVaEv *e = vaj_put('F', (uintptr_t)addr, size, 0, type, __builtin_return_address(0));
	unsigned i = (unsigned)((uintptr_t)addr >> 16);
	BOOL ok;

	if ((type & MEM_RELEASE) && raw_give(addr)) {
		if (e)
			e->ret = 1;
		return TRUE;
	}

	if (g_res && (type & MEM_RELEASE) && !((uintptr_t)addr & 0xFFFF) &&
	    g_res->g[i].state == GH_RES_LIVE && savestate_alloc_saved((uintptr_t)addr, -1)) {
		/* Released as far as the game knows; reserved as far as anyone
		 * else can tell, so the address is still free for a restore. */
		ok = r_vf(addr, 0, MEM_DECOMMIT);
		if (ok) {
			g_res->g[i].state = GH_RES_PARKED;
			InterlockedIncrement(&g_res->parked);
		}
		if (e) {
			e->ret = (uintptr_t)ok;
			e->how |= 0x80000000u;
		}
		return ok;
	}
	ok = r_vf(addr, size, type);
	if (g_res && ok && (type & MEM_RELEASE) && !((uintptr_t)addr & 0xFFFF))
		g_res->g[i].state = GH_RES_FREE;
	if (e)
		e->ret = (uintptr_t)ok;
	return ok;
}

LONG gameheap_res_seq(void)
{
	return g_res ? g_res->seq : 0;
}

/* After a restore's copy, threads still suspended. Blocks reserved after the
 * save the game was wound back to are unknown to it now: released, or kept
 * reserved if another save holds them. Parked blocks this save held are
 * committed again and live; the rest stay parked. */
void gameheap_res_restored(int slotno, LONG seq_at_save)
{
	unsigned i;
	LONG released = 0, parked = 0, again = 0;

	if (!g_res || !seq_at_save)
		return;
	for (i = 1; i < 65536; i++) {
		uintptr_t a = (uintptr_t)i << 16;
		unsigned char st = g_res->g[i].state;

		if (st == GH_RES_PARKED) {
			if (savestate_alloc_saved(a, slotno)) {
				g_res->g[i].state = GH_RES_LIVE;
				again++;
			}
			continue;
		}
		if (st != GH_RES_LIVE || g_res->g[i].born <= seq_at_save ||
		    savestate_alloc_saved(a, slotno))
			continue;
		if (savestate_alloc_saved(a, -1)) {
			if (r_vf((LPVOID)a, 0, MEM_DECOMMIT)) {
				g_res->g[i].state = GH_RES_PARKED;
				parked++;
			}
		} else if (r_vf((LPVOID)a, 0, MEM_RELEASE)) {
			g_res->g[i].state = GH_RES_FREE;
			released++;
		}
	}
	ss_log("  game blocks: %ld reserved since this save released, %ld kept for another "
	       "save, %ld held back since an earlier release and live again; %ld release(s) "
	       "parked so far\n",
	       (long)released, (long)parked, (long)again, (long)g_res->parked);
}

/* After a save: parked blocks no save holds any more are let go for real. */
void gameheap_res_saved(void)
{
	unsigned i;
	LONG n = 0;

	ct_report();
	if (!g_res)
		return;
	for (i = 1; i < 65536; i++)
		if (g_res->g[i].state == GH_RES_PARKED &&
		    !savestate_alloc_saved((uintptr_t)i << 16, -1) &&
		    r_vf((LPVOID)((uintptr_t)i << 16), 0, MEM_RELEASE)) {
			g_res->g[i].state = GH_RES_FREE;
			n++;
		}
	if (n)
		ss_log("  game blocks: %ld held-back release(s) no save needs any more, let go\n",
		       (long)n);
}

void gameheap_va_mark_restore(void)
{
	vaj_put('R', 0, 0, 0, 0, NULL);
}

/* Last n events, oldest first. Plain formatting only: called from the fault
 * report, which may be running on a thread that holds a heap lock. */
void gameheap_va_dump(unsigned n)
{
	unsigned end, i, start;
	MEMORY_BASIC_INFORMATION q;

	if (!g_vaj)
		return;
	if (VirtualQuery(g_vaj, &q, sizeof(q)) != sizeof(q) || q.State != MEM_COMMIT ||
	    (q.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
		ss_log("       game VirtualAlloc journal at %p is unreadable (state %lx "
		       "protect %lx, home %08lX) - not dumped\n",
		       (void *)g_vaj, (unsigned long)q.State, (unsigned long)q.Protect,
		       (unsigned long)GH_HOME_VAJ);
		return;
	}
	if (!g_vaj->next)
		return;
	end = (unsigned)g_vaj->next;
	if (n > GH_VAJ_N)
		n = GH_VAJ_N;
	start = end > n ? end - n : 0;
	ss_log("       game VirtualAlloc/VirtualFree, last %u of %u call(s), oldest first:\n",
	       end - start, end);
	for (i = start; i < end; i++) {
		GhVaEv *e = &g_vaj->e[i % GH_VAJ_N];

		if (e->op == 'R')
			ss_log("         #%u  ---- restore ----\n", i);
		else if (e->op == 'A')
			ss_log("         #%u  alloc %p +%lx type %lx -> %p, tid %lu, from %p\n", i,
			       (void *)e->addr, (unsigned long)e->size, (unsigned long)e->how,
			       (void *)e->ret, (unsigned long)e->tid, (void *)e->caller);
		else
			ss_log("         #%u  free  %p +%lx type %lx -> %s, tid %lu, from %p\n", i,
			       (void *)e->addr, (unsigned long)e->size, (unsigned long)e->how,
			       e->ret ? "ok" : "FAILED", (unsigned long)e->tid,
			       (void *)e->caller);
	}
}

/* D3D9SW_PHYSX=0 stops PhysX stepping: the scene keeps every actor, answers
 * queries and takes new bodies, but nothing moves under simulation.
 *
 * Only for the PhysX3_x86.dll Haydee ships (3.3.4, the timestamp below), whose
 * NpScene vtable sits at a known offset; any other build is left alone. Haydee
 * steps synchronously - simulate(dt, NULL, NULL, 0, true) then
 * fetchResults(true) at haydee.dll+171DFB - so with no completion task a
 * skipped step has nothing waiting on it. */
#define PX_TIMESTAMP 0x56b28579u
#define PX_NPSCENE_VT 0x188c84u
enum { PX_SLOT_SIMULATE = 54, PX_SLOT_CHECK = 57, PX_SLOT_FETCH = 58 };

static void *g_px_real_sim, *g_px_real_check, *g_px_real_fetch;
static int g_px_freeze = -1;
static const char *g_px_status = "PhysX3_x86.dll not seen";
static volatile LONG g_px_steps, g_px_skipped;
/* Thread inside simulate..fetchResults, 0 when the scene is idle. Rewinds with
 * the game on purpose: it describes the threads a restore brings back. */
static volatile LONG g_px_busy;

/* D3D9SW_PHYSX_TRACE=1 (default): every method table in PhysX3_x86.dll is
 * routed through stubs that count calls arriving from outside the DLL and
 * remember the caller. At each step, a method that had been quiet for
 * PXT_QUIET steps and is called again gets a log line, so every-frame traffic
 * stays silent and a one-off action (a jump, a body spawned or released) shows
 * up with the game address that made it. Counters and stubs are excluded from
 * saves: the tick count runs on through restores.
 *
 * This build has no RTTI, so tables are found as the .rdata addresses its code
 * carries relocations for that start a run of code pointers (294 tables, 4346
 * methods in 3.3.4). Labelled by RVA; NpScene's is named. */
#define PXT_MAX 8192
#define PXT_TABLES 1024
#define PXT_STUB 64
#define PXT_HOME 0x5FF00000u
#define PXT_CODE_HOME 0x5FF40000u
#define PXT_QUIET 8

typedef struct {
	volatile LONG n;
	void *volatile caller;
	void *real;
	LONG seen, last;
	unsigned rva, slot;
} PxtSlot;

typedef struct {
	PxtSlot s[PXT_MAX];
	LONG ns, nvt, tick;
	DWORD t0;
	unsigned start[PXT_TABLES];
} PxTrace;

static PxTrace *g_pxt SS_PRESENT;
static unsigned char *g_pxt_code SS_PRESENT;
static int g_pxt_held;

static void pxt_where(void *a, char *out)
{
	HMODULE m = NULL;
	char p[MAX_PATH];
	const char *leaf = p;
	int i;

	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			       (LPCSTR)a, &m) ||
	    !GetModuleFileNameA(m, p, sizeof(p))) {
		wsprintfA(out, "%p", a);
		return;
	}
	for (i = 0; p[i]; i++)
		if (p[i] == '\\' || p[i] == '/')
			leaf = p + i + 1;
	wsprintfA(out, "%s+%lX", leaf, (unsigned long)((uintptr_t)a - (uintptr_t)m));
}

/* step=1 advances the tick (called at each simulate); 0 just reports what the
 * game called since, so a save or restore line has everything before it. */
static void pxt_scan(int step)
{
	char line[480], item[160], at[96];
	int k = 0;
	LONG i, tick;

	if (!g_pxt)
		return;
	tick = step ? ++g_pxt->tick : g_pxt->tick;
	for (i = 0; i < g_pxt->ns; i++) {
		PxtSlot *s = &g_pxt->s[i];
		LONG n = s->n, d = n - s->seen;
		int quiet, len;

		if (!d)
			continue;
		s->seen = n;
		quiet = tick - s->last > PXT_QUIET;
		s->last = tick;
		if (!quiet)
			continue;
		pxt_where(s->caller, at);
		if (s->rva == PX_NPSCENE_VT)
			len = wsprintfA(item, " NpScene[%u] x%ld from %s;", s->slot, (long)d, at);
		else
			len = wsprintfA(item, " vt%X[%u] x%ld from %s;", s->rva, s->slot, (long)d,
					at);
		if (k + len >= (int)sizeof(line) - 1) {
			line[k] = 0;
			ss_log("physx tick %ld, %lu ms:%s\n", (long)tick,
			       (unsigned long)(GetTickCount() - g_pxt->t0), line);
			k = 0;
		}
		memcpy(line + k, item, (size_t)len);
		k += len;
	}
	if (k) {
		line[k] = 0;
		ss_log("physx tick %ld, %lu ms:%s\n", (long)tick,
		       (unsigned long)(GetTickCount() - g_pxt->t0), line);
	}
}

#if defined(__i386__) || defined(_M_IX86)
/* D3D9SW_PHYSX_JOURNAL=1 (default off): every call the game makes into PhysX is
 * written down - method, object, the first six argument words, caller, thread
 * and what came back - to physx_journal.csv beside the log, at each report and
 * when the buffer fills. Without symbols this is how the methods get names:
 * a call whose return value turns up later as another call's object made that
 * object. The return is caught by swapping the caller's return address for
 * pxj_ret and keeping the real one on a per-thread stack, so callbacks that
 * re-enter PhysX nest. Off by default: it costs a few hundred cycles a call. */
#define PXJ_MAX (1 << 18)
#define PXJ_DEPTH 64
typedef struct {
	unsigned slot, tid, self, caller, tick, ret, ret2, done;
	unsigned a[6];
} PxjRec;
static PxjRec *g_pxj SS_PRESENT;
static volatile LONG g_pxj_n;
static LONG g_pxj_dumped;
static __thread unsigned t_pxj_ret[PXJ_DEPTH], t_pxj_rec[PXJ_DEPTH];
static __thread int t_pxj_depth;

static void pxj_ret(void);

/* f[0] the stub's slot (replaced by the real method), f[1] the return address,
 * f[2..] the arguments; pushal left ecx (this) at f[-2]. */
static void __cdecl pxj_enter(unsigned *f)
{
	PxtSlot *s = (PxtSlot *)(uintptr_t)f[0];
	PxjRec *r;
	LONG i;
	int d = t_pxj_depth;

	f[0] = (unsigned)(uintptr_t)s->real;
	if (d >= PXJ_DEPTH || (i = InterlockedIncrement(&g_pxj_n) - 1) >= PXJ_MAX)
		return;
	r = &g_pxj[i];
	r->slot = (unsigned)(s - g_pxt->s);
	r->tid = GetCurrentThreadId();
	r->self = f[-2];
	r->caller = f[1];
	r->tick = (unsigned)g_pxt->tick;
	r->a[0] = f[2], r->a[1] = f[3], r->a[2] = f[4];
	r->a[3] = f[5], r->a[4] = f[6], r->a[5] = f[7];
	t_pxj_ret[d] = f[1];
	t_pxj_rec[d] = (unsigned)i;
	t_pxj_depth = d + 1;
	f[1] = (unsigned)(uintptr_t)pxj_ret;
}

/* f[0] takes the real return address; the method's eax and edx sit in the
 * pushal frame at f[-1] and f[-3]. Must not touch x87: a float result is in
 * st0. */
static void __cdecl pxj_leave(unsigned *f)
{
	int d = --t_pxj_depth;
	PxjRec *r = &g_pxj[t_pxj_rec[d]];

	f[0] = t_pxj_ret[d];
	r->ret = f[-1];
	r->ret2 = f[-3];
	r->done = 1;
}

static void(__cdecl *volatile g_pxj_enter_fn)(unsigned *) = pxj_enter;
static void(__cdecl *volatile g_pxj_leave_fn)(unsigned *) = pxj_leave;

__attribute__((naked)) static void pxj_entry(void)
{
	__asm__ __volatile__("pushal\n\t"
			     "leal 32(%%esp), %%eax\n\t"
			     "pushl %%eax\n\t"
			     "call *%0\n\t"
			     "addl $4, %%esp\n\t"
			     "popal\n\t"
			     "ret\n\t"
			     :
			     : "m"(g_pxj_enter_fn)
			     : "memory");
}

__attribute__((naked)) static void pxj_ret(void)
{
	__asm__ __volatile__("pushl $0\n\t"
			     "pushal\n\t"
			     "leal 32(%%esp), %%ecx\n\t"
			     "pushl %%ecx\n\t"
			     "call *%0\n\t"
			     "addl $4, %%esp\n\t"
			     "popal\n\t"
			     "ret\n\t"
			     :
			     : "m"(g_pxj_leave_fn)
			     : "memory");
}

static void pxj_dump(void)
{
	LONG n = g_pxj_n, i;
	HANDLE f;
	static char buf[1 << 16];
	char at[96], line[400];
	DWORD put;
	int len, nb = 0;

	if (!g_pxj)
		return;
	if (n > PXJ_MAX)
		n = PXJ_MAX;
	if (n <= g_pxj_dumped)
		return;
	f = CreateFileA("physx_journal.csv", FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
			g_pxj_dumped ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	if (!g_pxj_dumped) {
		len = wsprintfA(line, "i,tick,tid,table,slot,self,a0,a1,a2,a3,a4,a5,ret,ret2,caller\r\n");
		WriteFile(f, line, (DWORD)len, &put, NULL);
	}
	for (i = g_pxj_dumped; i < n; i++) {
		const PxjRec *r = &g_pxj[i];
		const PxtSlot *s = &g_pxt->s[r->slot];

		pxt_where((void *)(uintptr_t)r->caller, at);
		len = wsprintfA(line, "%ld,%u,%u,%s%X,%u,%08X,%08X,%08X,%08X,%08X,%08X,%08X,", (long)i,
				r->tick, r->tid, s->rva == PX_NPSCENE_VT ? "NpScene_" : "vt", s->rva,
				s->slot, r->self, r->a[0], r->a[1], r->a[2], r->a[3], r->a[4],
				r->a[5]);
		if (r->done)
			len += wsprintfA(line + len, "%08X,%08X,%s\r\n", r->ret, r->ret2, at);
		else
			len += wsprintfA(line + len, "-,-,%s\r\n", at);
		if (nb + len > (int)sizeof(buf)) {
			WriteFile(f, buf, (DWORD)nb, &put, NULL);
			nb = 0;
		}
		memcpy(buf + nb, line, (size_t)len);
		nb += len;
	}
	if (nb)
		WriteFile(f, buf, (DWORD)nb, &put, NULL);
	CloseHandle(f);
	ss_log("physx journal: %ld call(s) written to physx_journal.csv (%ld this time)%s\n",
	       (long)n, (long)(n - g_pxj_dumped),
	       g_pxj_n > PXJ_MAX ? " - FULL, later calls not recorded" : "");
	g_pxj_dumped = n;
}

/* cmp [esp],lo / jb count / cmp [esp],hi / jb skip
 * count: push eax / mov eax,[esp+4] / mov [caller],eax / pop eax / lock inc [n]
 *        and with the journal on: push slot / jmp pxj_entry
 * skip:  jmp [real] */
static void pxt_stub(unsigned char *c, PxtSlot *s, uintptr_t lo, uintptr_t hi)
{
	uintptr_t a;
	int skip = g_pxj ? 46 : 36;

	c[0] = 0x81, c[1] = 0x3C, c[2] = 0x24, memcpy(c + 3, &lo, 4);
	c[7] = 0x72, c[8] = 9;
	c[9] = 0x81, c[10] = 0x3C, c[11] = 0x24, memcpy(c + 12, &hi, 4);
	c[16] = 0x72, c[17] = (unsigned char)(skip - 18);
	c[18] = 0x50, c[19] = 0x8B, c[20] = 0x44, c[21] = 0x24, c[22] = 0x04;
	a = (uintptr_t)&s->caller, c[23] = 0xA3, memcpy(c + 24, &a, 4);
	c[28] = 0x58;
	a = (uintptr_t)&s->n, c[29] = 0xF0, c[30] = 0xFF, c[31] = 0x05, memcpy(c + 32, &a, 4);
	if (g_pxj) {
		LONG rel = (LONG)((uintptr_t)pxj_entry - (uintptr_t)(c + 46));
		a = (uintptr_t)s, c[36] = 0x68, memcpy(c + 37, &a, 4);
		c[41] = 0xE9, memcpy(c + 42, &rel, 4);
	}
	a = (uintptr_t)&s->real, c[skip] = 0xFF, c[skip + 1] = 0x25, memcpy(c + skip + 2, &a, 4);
}

static int pxt_exec(const unsigned char *b, const IMAGE_NT_HEADERS *nt, uintptr_t v)
{
	const IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
	unsigned i;

	for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
		if ((sh[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) &&
		    v >= (uintptr_t)b + sh[i].VirtualAddress &&
		    v < (uintptr_t)b + sh[i].VirtualAddress + sh[i].Misc.VirtualSize)
			return 1;
	return 0;
}

static void pxt_wrap(const unsigned char *b, const IMAGE_NT_HEADERS *nt, unsigned rva,
		     unsigned end, void **scene_vt)
{
	uintptr_t lo = (uintptr_t)b, hi = lo + nt->OptionalHeader.SizeOfImage;
	void **vt = (void **)(b + rva);
	unsigned n = 0, i;
	DWORD old;

	while (n < 400 && rva + 4 * n < end && pxt_exec(b, nt, (uintptr_t)vt[n]))
		n++;
	if (!n || !VirtualProtect(vt, n * sizeof(void *), PAGE_READWRITE, &old))
		return;
	for (i = 0; i < n && g_pxt->ns < PXT_MAX; i++) {
		PxtSlot *s = &g_pxt->s[g_pxt->ns];
		unsigned char *c = g_pxt_code + g_pxt->ns * PXT_STUB;

		if (vt == scene_vt && i >= PX_SLOT_SIMULATE && i <= PX_SLOT_FETCH)
			continue;
		s->real = vt[i];
		s->rva = rva;
		s->slot = i;
		s->last = -1000;
		pxt_stub(c, s, lo, hi);
		vt[i] = c;
		g_pxt->ns++;
	}
	VirtualProtect(vt, n * sizeof(void *), old, &old);
	g_pxt->nvt++;
}

static void pxt_install(const unsigned char *b, const IMAGE_NT_HEADERS *nt, void **scene_vt)
{
	const IMAGE_DATA_DIRECTORY *rd =
		&nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
	const unsigned char *r = b + rd->VirtualAddress, *rend = r + rd->Size;
	unsigned size = nt->OptionalHeader.SizeOfImage, nst = 0, i, j;

	if (!gh_knob("D3D9SW_PHYSX_TRACE", 1) || !rd->VirtualAddress || !rd->Size ||
	    IsBadReadPtr(r, rd->Size))
		return;
	/* Fixed homes, because PhysX's method tables rewind with the game and hold
	 * stub addresses: a load from another launch must find the stubs where the
	 * saving launch had them. The raw span stops short of PXT_HOME for this. */
	g_pxt = r_va ? r_va((void *)PXT_HOME, sizeof(PxTrace), MEM_COMMIT | MEM_RESERVE,
			    PAGE_READWRITE)
		     : VirtualAlloc((void *)PXT_HOME, sizeof(PxTrace), MEM_COMMIT | MEM_RESERVE,
				    PAGE_READWRITE);
	g_pxt_code = r_va ? r_va((void *)PXT_CODE_HOME, PXT_MAX * PXT_STUB,
				 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)
			  : VirtualAlloc((void *)PXT_CODE_HOME, PXT_MAX * PXT_STUB,
					 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!g_pxt || !g_pxt_code)
		ss_log("physx trace: fixed home at %08X taken, tracer placed by the OS - "
		       "a load from another launch will jump into the old stubs\n",
		       PXT_HOME);
	if (!g_pxt)
		g_pxt = r_va ? r_va(NULL, sizeof(PxTrace), MEM_COMMIT | MEM_RESERVE,
				    PAGE_READWRITE)
			     : VirtualAlloc(NULL, sizeof(PxTrace), MEM_COMMIT | MEM_RESERVE,
					    PAGE_READWRITE);
	if (!g_pxt_code)
		g_pxt_code = r_va ? r_va(NULL, PXT_MAX * PXT_STUB, MEM_COMMIT | MEM_RESERVE,
					 PAGE_EXECUTE_READWRITE)
				  : VirtualAlloc(NULL, PXT_MAX * PXT_STUB,
						 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!g_pxt || !g_pxt_code) {
		g_pxt = NULL;
		return;
	}
	g_pxt->t0 = GetTickCount();
	if (gh_knob("D3D9SW_PHYSX_JOURNAL", 0))
		g_pxj = (PxjRec *)VirtualAlloc(NULL, PXJ_MAX * sizeof(PxjRec),
					       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	/* Every absolute address in code is in the relocation list. Those that
	 * point at a data word starting two or more code pointers are tables. */
	while (r + 8 <= rend) {
		const IMAGE_BASE_RELOCATION *blk = (const IMAGE_BASE_RELOCATION *)r;
		const WORD *e = (const WORD *)(r + 8);
		unsigned ne;

		if (blk->SizeOfBlock < 8 || r + blk->SizeOfBlock > rend)
			break;
		ne = (blk->SizeOfBlock - 8) / 2;
		for (i = 0; i < ne; i++) {
			unsigned at = blk->VirtualAddress + (e[i] & 0xfff), t;
			uintptr_t v;

			if ((e[i] >> 12) != IMAGE_REL_BASED_HIGHLOW || at + 4 > size ||
			    !pxt_exec(b, nt, (uintptr_t)b + at))
				continue;
			v = *(const uintptr_t *)(b + at);
			if (v < (uintptr_t)b || v + 8 > (uintptr_t)b + size || (v & 3) ||
			    pxt_exec(b, nt, v) || !pxt_exec(b, nt, ((const uintptr_t *)v)[0]) ||
			    !pxt_exec(b, nt, ((const uintptr_t *)v)[1]))
				continue;
			t = (unsigned)(v - (uintptr_t)b);
			for (j = 0; j < nst && g_pxt->start[j] != t; j++)
				;
			if (j == nst && nst < PXT_TABLES)
				g_pxt->start[nst++] = t;
		}
		r += blk->SizeOfBlock;
	}
	for (i = 1; i < nst; i++)
		for (j = i; j && g_pxt->start[j - 1] > g_pxt->start[j]; j--) {
			unsigned t = g_pxt->start[j];

			g_pxt->start[j] = g_pxt->start[j - 1];
			g_pxt->start[j - 1] = t;
		}
	for (i = 0; i < nst; i++)
		pxt_wrap(b, nt, g_pxt->start[i], i + 1 < nst ? g_pxt->start[i + 1] : size,
			 scene_vt);
}
#else
static void pxt_install(const unsigned char *b, const IMAGE_NT_HEADERS *nt, void **scene_vt)
{
	(void)b, (void)nt, (void)scene_vt;
}

static void pxj_dump(void)
{
}
#endif

/* Called by the engine as a save or restore begins: waits out a step another
 * thread is in the middle of, and puts the moment on the trace's timeline. */
void gameheap_physx_quiet(const char *what, int slot)
{
	DWORD t0 = GetTickCount();
	LONG tid = g_px_busy, me = (LONG)GetCurrentThreadId();
	const char *how = "scene idle";
	char b[64];

	if (!g_px_real_sim)
		return;
	if (tid == me)
		how = "called from inside a step";
	else if (tid) {
		while (g_px_busy && GetTickCount() - t0 < 500)
			Sleep(1);
		if (g_px_busy)
			how = "a step was still running after 500 ms - went ahead";
		else {
			wsprintfA(b, "waited %lu ms for a step to finish",
				  (unsigned long)(GetTickCount() - t0));
			how = b;
		}
	}
	pxt_scan(0);
	ss_log("physx: %s slot %d at tick %ld, step %ld - %s\n", what, slot,
	       g_pxt ? (long)g_pxt->tick : 0L, (long)g_px_steps, how);
}

void gameheap_physx_restored(void)
{
	if (!g_px_real_sim)
		return;
	ss_log("physx: restored at tick %ld, back to step %ld; the scene came back %s\n",
	       g_pxt ? (long)g_pxt->tick : 0L, (long)g_px_steps,
	       g_px_busy ? "MID-STEP" : "idle");
}

static void __thiscall px_simulate(void *self, float dt, void *task, void *scratch,
				   unsigned scratch_n, int control)
{
	pxt_scan(1);
	if (g_px_freeze) {
		InterlockedIncrement(&g_px_skipped);
		return;
	}
	InterlockedIncrement(&g_px_steps);
	g_px_busy = (LONG)GetCurrentThreadId();
	((void(__thiscall *)(void *, float, void *, void *, unsigned, int))g_px_real_sim)(
		self, dt, task, scratch, scratch_n, control);
}

static unsigned char __thiscall px_check(void *self, int block)
{
	if (g_px_freeze)
		return 1;
	return ((unsigned char(__thiscall *)(void *, int))g_px_real_check)(self, block);
}

static unsigned char __thiscall px_fetch(void *self, int block, unsigned *err)
{
	unsigned char r;

	if (g_px_freeze) {
		if (err)
			*err = 0;
		return 1;
	}
	r = ((unsigned char(__thiscall *)(void *, int, unsigned *))g_px_real_fetch)(self, block,
										    err);
	if (r)
		g_px_busy = 0;
	return r;
}

static void gh_physx_hook(HMODULE mod, const WCHAR *path)
{
	const unsigned char *b = (const unsigned char *)mod;
	const IMAGE_NT_HEADERS *nt;
	void **vt;
	DWORD old;
	const WCHAR *leaf = path;
	unsigned i;

	for (i = 0; path[i]; i++)
		if (path[i] == '\\' || path[i] == '/')
			leaf = path + i + 1;
	if (lstrcmpiW(leaf, L"PhysX3_x86.dll") || g_px_real_sim)
		return;
	if (g_px_freeze < 0)
		g_px_freeze = gh_knob("D3D9SW_PHYSX", 1) == 0;
	nt = (const IMAGE_NT_HEADERS *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
	if (nt->FileHeader.TimeDateStamp != PX_TIMESTAMP) {
		g_px_status = "PhysX3_x86.dll is not the 3.3.4 build this knows - left alone";
		return;
	}
	vt = (void **)(b + PX_NPSCENE_VT);
	if (vt[0] != (void *)(b + 0x4a600) || vt[1] != (void *)(b + 0x449e0) ||
	    vt[PX_SLOT_SIMULATE] != (void *)(b + 0x45bb0) ||
	    vt[PX_SLOT_CHECK] != (void *)(b + 0x45d50) ||
	    vt[PX_SLOT_FETCH] != (void *)(b + 0x45d90)) {
		g_px_status = "PhysX scene vtable not where expected - left alone";
		return;
	}
	pxt_install(b, nt, vt);
	if (!VirtualProtect(&vt[PX_SLOT_SIMULATE], 5 * sizeof(void *), PAGE_READWRITE, &old)) {
		g_px_status = "PhysX scene vtable could not be made writable - left alone";
		return;
	}
	g_px_real_sim = vt[PX_SLOT_SIMULATE];
	g_px_real_check = vt[PX_SLOT_CHECK];
	g_px_real_fetch = vt[PX_SLOT_FETCH];
	vt[PX_SLOT_SIMULATE] = (void *)px_simulate;
	vt[PX_SLOT_CHECK] = (void *)px_check;
	vt[PX_SLOT_FETCH] = (void *)px_fetch;
	VirtualProtect(&vt[PX_SLOT_SIMULATE], 5 * sizeof(void *), old, &old);
	g_px_status = g_px_freeze ? "scene stepping OFF by D3D9SW_PHYSX=0" : "scene stepping on";
}

/* ---- OpenAL objects across a restore ---------------------------------------
 * The game's audio lives in OpenAL32/wrap_oal from the system folder, which a
 * restore leaves in the present. So after one, OpenAL still has every source and
 * buffer the game made since the save (forgotten by the game, and leaking
 * voices each time), lacks the ones it deleted since (still in use by the game),
 * and its streaming queues no longer match what the game will unqueue.
 *
 * Two records of the same objects: g_al_game in our image's data, which rewinds
 * with the game and so always says what the game believes, and g_al_real, held
 * in the present, which says what OpenAL has. After a restore the first AL call
 * the game makes brings OpenAL back in line - on the game's own thread, never
 * from inside the restore, where a suspended thread may hold OpenAL's lock.
 * Deletes are held back (stopped and emptied) while any save exists, so a
 * restore has something to bring back. D3D9SW_REWIND_AL=0 turns it off. */
#define AL_SOURCE_STATE 0x1010
#define AL_PLAYING 0x1012
#define AL_BUFFER 0x1009
#define AL_MAXQ 16
#define AL_NSRC 1024u
#define AL_NBUF 8192u
#define AL_NPROP 12
#define AL_NLPROP 8

/* The last value the game set for one property; is_int marks alSourcei's. */
typedef struct {
	int param;
	unsigned char n, is_int;
	int iv;
	float v[6];
} AlProp;

typedef struct {
	unsigned id;
	unsigned char live, parked, is_static;
	int state;
	int nq;
	unsigned q[AL_MAXQ];
	int np;
	AlProp p[AL_NPROP];
} AlSrc;
/* data: in g_al_game only, a copy of what the game last uploaded, on the game
 * heap so it comes back with a restore - another launch's OpenAL never saw it. */
typedef struct {
	unsigned id;
	unsigned char live, parked;
	int fmt, size, freq, cap;
	void *data;
} AlBuf;
typedef struct {
	AlSrc s[AL_NSRC];
	AlBuf b[AL_NBUF];
	int nlp;
	AlProp lp[AL_NLPROP];
	LONG full;
} AlTab;
/* A game id from another launch, and the object made for it in this one. */
typedef struct {
	unsigned g, r;
} AlMap;
typedef struct {
	AlTab t;
	volatile LONG lock, pending, purge, trace;
	unsigned nsm, nbm;
	AlMap sm[AL_NSRC], bm[AL_NBUF];
} AlReal;

static AlTab g_al_game;
static AlReal *g_al_real SS_PRESENT;
_Static_assert(sizeof(AlReal) <= 0x5FEE0000u - GH_HOME_AL, "AlReal outgrew its home");
static int g_al_held;

static void(__cdecl *r_alGenSources)(int, unsigned *);
static void(__cdecl *r_alDeleteSources)(int, const unsigned *);
static void(__cdecl *r_alGenBuffers)(int, unsigned *);
static void(__cdecl *r_alDeleteBuffers)(int, const unsigned *);
static void(__cdecl *r_alSourceQueueBuffers)(unsigned, int, const unsigned *);
static void(__cdecl *r_alSourceUnqueueBuffers)(unsigned, int, unsigned *);
static void(__cdecl *r_alSourcei)(unsigned, int, int);
static void(__cdecl *r_alSourcePlay)(unsigned);
static void(__cdecl *r_alSourceStop)(unsigned);
static void(__cdecl *r_alGetSourcei)(unsigned, int, int *);
static void(__cdecl *r_alSourcef)(unsigned, int, float);
static void(__cdecl *r_alSource3f)(unsigned, int, float, float, float);
static void(__cdecl *r_alSourcefv)(unsigned, int, const float *);
static void(__cdecl *r_alListenerf)(int, float);
static void(__cdecl *r_alListenerfv)(int, const float *);
static int(__cdecl *r_alGetError)(void);
static char(__cdecl *r_alIsSource)(unsigned);
static char(__cdecl *r_alIsBuffer)(unsigned);
static void(__cdecl *r_alBufferData)(unsigned, int, const void *, int, int);
static void(__cdecl *r_alBufferi)(unsigned, int, int);

int savestate_any_valid(void);

/* The game's AL calls for a while after each restore, which is where a source
 * that comes back silent shows what the game did with it. */
#define AL_TRACE_CALLS 150
#define al_trace(...)                                          \
	do {                                                   \
		if (g_al_real->trace > 0) {                    \
			g_al_real->trace--;                    \
			ss_log("openal trace: " __VA_ARGS__); \
		}                                              \
	} while (0)

static void al_prop_set(AlProp *p, int *np, int max, int param, int is_int, int iv, int n,
			const float *v)
{
	int k;
	AlProp *e = NULL;

	for (k = 0; k < *np; k++)
		if (p[k].param == param) {
			e = &p[k];
			break;
		}
	if (!e) {
		if (*np >= max)
			return;
		e = &p[(*np)++];
		e->param = param;
	}
	e->is_int = (unsigned char)is_int;
	e->iv = iv;
	e->n = (unsigned char)n;
	for (k = 0; k < n; k++)
		e->v[k] = v[k];
}

static void al_prop_apply_src(unsigned id, const AlProp *e)
{
	if (e->is_int)
		r_alSourcei(id, e->param, e->iv);
	else if (e->n == 3)
		r_alSource3f(id, e->param, e->v[0], e->v[1], e->v[2]);
	else
		r_alSourcef(id, e->param, e->v[0]);
}

static void al_prop_apply_listener(const AlProp *e)
{
	if (e->n == 1)
		r_alListenerf(e->param, e->v[0]);
	else
		r_alListenerfv(e->param, e->v);
}

static AlSrc *al_src(AlTab *t, unsigned id, int create)
{
	unsigned h = (id * 2654435761u) & (AL_NSRC - 1), i;
	AlSrc *tomb = NULL;

	for (i = 0; i < AL_NSRC; i++) {
		AlSrc *e = &t->s[(h + i) & (AL_NSRC - 1)];
		if (e->id == id && (e->live || e->parked))
			return e;
		if (!e->id)
			break;
		if (!e->live && !e->parked && !tomb)
			tomb = e;
	}
	if (!create)
		return NULL;
	if (!tomb && i < AL_NSRC)
		tomb = &t->s[(h + i) & (AL_NSRC - 1)];
	if (!tomb) {
		t->full++;
		return NULL;
	}
	memset(tomb, 0, sizeof(*tomb));
	tomb->id = id;
	return tomb;
}

static AlBuf *al_buf(AlTab *t, unsigned id, int create)
{
	unsigned h = (id * 2654435761u) & (AL_NBUF - 1), i;
	AlBuf *tomb = NULL;

	for (i = 0; i < AL_NBUF; i++) {
		AlBuf *e = &t->b[(h + i) & (AL_NBUF - 1)];
		if (e->id == id && (e->live || e->parked))
			return e;
		if (!e->id)
			break;
		if (!e->live && !e->parked && !tomb)
			tomb = e;
	}
	if (!create)
		return NULL;
	if (!tomb && i < AL_NBUF)
		tomb = &t->b[(h + i) & (AL_NBUF - 1)];
	if (!tomb) {
		t->full++;
		return NULL;
	}
	memset(tomb, 0, sizeof(*tomb));
	tomb->id = id;
	return tomb;
}

static void al_lock(void)
{
	while (InterlockedCompareExchange(&g_al_real->lock, 1, 0))
		YieldProcessor();
}

static void al_unlock(void)
{
	InterlockedExchange(&g_al_real->lock, 0);
}

static void al_set_queue(AlSrc *s, const unsigned *q, int n, int is_static)
{
	int k;
	if (!s)
		return;
	if (n > AL_MAXQ)
		n = AL_MAXQ;
	for (k = 0; k < n; k++)
		s->q[k] = q[k];
	s->nq = n;
	s->is_static = (unsigned char)is_static;
}

static void al_queue_push(AlSrc *s, const unsigned *b, int n)
{
	int k;
	for (k = 0; s && k < n && s->nq < AL_MAXQ; k++)
		s->q[s->nq++] = b[k];
	if (s)
		s->is_static = 0;
}

static void al_queue_pop(AlSrc *s, int n)
{
	if (!s)
		return;
	if (n > s->nq)
		n = s->nq;
	memmove(s->q, s->q + n, (size_t)(s->nq - n) * sizeof(unsigned));
	s->nq -= n;
}

/* Puts a source's queue and play state back to what the game believes. */
static void al_reset_source(const AlSrc *g, AlSrc *r)
{
	r_alSourceStop(g->id);
	r_alSourcei(g->id, AL_BUFFER, 0);
	if (g->is_static && g->nq == 1)
		r_alSourcei(g->id, AL_BUFFER, (int)g->q[0]);
	else if (g->nq)
		r_alSourceQueueBuffers(g->id, g->nq, g->q);
	if (g->state == AL_PLAYING)
		r_alSourcePlay(g->id);
	memcpy(r->q, g->q, sizeof(r->q));
	r->nq = g->nq;
	r->is_static = g->is_static;
	r->state = g->state;
	r->live = 1;
	r->parked = 0;
}

/* Called with the lock held, on whichever game thread touches OpenAL first. */
static void al_reconcile(void)
{
	AlTab *R = &g_al_real->t, *G = &g_al_game;
	unsigned i;
	LONG src_gone = 0, buf_gone = 0, src_back = 0, src_reset = 0, buf_back = 0, props = 0;
	int err;

	for (i = 0; i < AL_NSRC; i++) {
		AlSrc *r = &R->s[i];
		AlSrc *g;
		if (!r->id || (!r->live && !r->parked))
			continue;
		g = al_src(G, r->id, 0);
		if (g && g->live)
			continue;
		r_alSourceStop(r->id);
		r_alSourcei(r->id, AL_BUFFER, 0);
		r_alDeleteSources(1, &r->id);
		r->live = r->parked = 0;
		src_gone++;
	}
	r_alGetError();
	/* Sources before buffers: a buffer still queued on a source cannot be
	 * deleted, and the reset below is what takes it off. */
	for (i = 0; i < AL_NSRC; i++) {
		const AlSrc *g = &G->s[i];
		AlSrc *r;
		int k, why = 0, rst, rnq;
		if (!g->id || !g->live)
			continue;
		r = al_src(R, g->id, 0);
		if (!r)
			continue;
		rst = r->state;
		rnq = r->nq;
		if (r->parked)
			why = 1;
		else if (r->nq != g->nq || r->state != g->state ||
			 memcmp(r->q, g->q, (size_t)g->nq * sizeof(unsigned)))
			why = 2;
		if (why) {
			al_reset_source(g, r);
			if (why == 1)
				src_back++;
			else
				src_reset++;
		}
		/* Volume, pitch, looping and position are whatever the present last
		 * set, and the rewound game believes its own values are in effect. */
		for (k = 0; k < g->np; k++)
			al_prop_apply_src(g->id, &g->p[k]);
		props += g->np;
		if (why)
			ss_log("openal:   source %u %s - game has it %s with %d queued%s, OpenAL "
			       "had it %s with %d; %d setting(s) put back\n",
			       g->id, why == 1 ? "brought back" : "re-queued",
			       g->state == AL_PLAYING ? "playing" : "not playing", g->nq,
			       g->is_static ? " (static)" : "",
			       rst == AL_PLAYING ? "playing" : "not playing", rnq, g->np);
	}
	for (i = 0; i < (unsigned)G->nlp; i++)
		al_prop_apply_listener(&G->lp[i]);
	for (i = 0; i < AL_NBUF; i++) {
		AlBuf *r = &R->b[i];
		AlBuf *g;
		if (!r->id || (!r->live && !r->parked))
			continue;
		g = al_buf(G, r->id, 0);
		if (g && g->live) {
			if (r->parked) {
				r->parked = 0;
				r->live = 1;
				buf_back++;
			}
			continue;
		}
		r_alDeleteBuffers(1, &r->id);
		r->live = r->parked = 0;
		buf_gone++;
	}
	err = r_alGetError();
	ss_log("openal: after the restore - %ld source(s) and %ld buffer(s) made since the "
	       "save deleted, %ld source(s) and %ld buffer(s) deleted since brought back, "
	       "%ld source(s) re-queued to match the game, %ld source and %d listener "
	       "setting(s) put back%s; OpenAL error after: %04X\n",
	       (long)src_gone, (long)buf_gone, (long)src_back, (long)buf_back,
	       (long)src_reset, (long)props, G->nlp,
	       (R->full || G->full) ? " (a table filled; some untracked)" : "", err);
	g_al_real->trace = AL_TRACE_CALLS;
}

/* Deletes held back for a save that has since been replaced. */
static void al_purge(void)
{
	AlTab *R = &g_al_real->t;
	unsigned i;

	for (i = 0; i < AL_NSRC; i++)
		if (R->s[i].parked) {
			r_alDeleteSources(1, &R->s[i].id);
			R->s[i].parked = 0;
		}
	for (i = 0; i < AL_NBUF; i++)
		if (R->b[i].parked) {
			r_alDeleteBuffers(1, &R->b[i].id);
			R->b[i].parked = 0;
		}
}

/* Ids from another launch. This OpenAL hands out pointers as ids, so a game
 * restored from another process names sources and buffers that do not exist
 * here. The first time the game uses such an id it gets an object of its own,
 * and every call after is translated. Called with the lock held. */
static unsigned al_xb(unsigned g)
{
	unsigned k, r = 0;

	if (!g)
		return 0;
	for (k = 0; k < g_al_real->nbm; k++)
		if (g_al_real->bm[k].g == g)
			return g_al_real->bm[k].r;
	if (r_alIsBuffer(g))
		return g;
	r_alGenBuffers(1, &r);
	if (!r || g_al_real->nbm >= AL_NBUF)
		return r ? r : g;
	g_al_real->bm[g_al_real->nbm].g = g;
	g_al_real->bm[g_al_real->nbm++].r = r;
	{
		AlBuf *rb = al_buf(&g_al_real->t, r, 1), *gb = al_buf(&g_al_game, g, 0);
		if (rb)
			rb->live = 1;
		if (gb && gb->data)
			r_alBufferData(r, gb->fmt, gb->data, gb->size, gb->freq);
		al_trace("buffer %u is from another launch - now buffer %u, %d byte(s) put back\n",
			 g, r, gb && gb->data ? gb->size : 0);
	}
	return r;
}

static unsigned al_xb_back(unsigned r)
{
	unsigned k;

	for (k = 0; k < g_al_real->nbm; k++)
		if (g_al_real->bm[k].r == r)
			return g_al_real->bm[k].g;
	return r;
}

static unsigned al_xs(unsigned g)
{
	unsigned k, r = 0, q[AL_MAXQ];
	const AlSrc *gs;
	AlSrc *rs;
	int i;

	if (!g)
		return 0;
	for (k = 0; k < g_al_real->nsm; k++)
		if (g_al_real->sm[k].g == g)
			return g_al_real->sm[k].r;
	if (r_alIsSource(g))
		return g;
	r_alGenSources(1, &r);
	if (!r || g_al_real->nsm >= AL_NSRC)
		return r ? r : g;
	g_al_real->sm[g_al_real->nsm].g = g;
	g_al_real->sm[g_al_real->nsm++].r = r;
	/* Settings, queue and play state as the game believes them; a queued
	 * buffer comes back empty, finishes at once, and the stream refills it. */
	gs = al_src(&g_al_game, g, 0);
	rs = al_src(&g_al_real->t, r, 1);
	if (rs)
		rs->live = 1;
	if (gs) {
		for (i = 0; i < gs->np; i++)
			al_prop_apply_src(r, &gs->p[i]);
		for (i = 0; i < gs->nq; i++)
			q[i] = al_xb(gs->q[i]);
		if (gs->is_static && gs->nq == 1)
			r_alSourcei(r, AL_BUFFER, (int)q[0]);
		else if (gs->nq)
			r_alSourceQueueBuffers(r, gs->nq, q);
		if (rs) {
			memcpy(rs->q, q, (size_t)gs->nq * sizeof(unsigned));
			rs->nq = gs->nq;
			rs->is_static = gs->is_static;
			rs->state = gs->state;
		}
		if (gs->state == AL_PLAYING)
			r_alSourcePlay(r);
	}
	r_alGetError();
	al_trace("source %u is from another launch - now source %u (%d queued, %s)\n", g, r,
		 gs ? gs->nq : 0, gs && gs->state == AL_PLAYING ? "playing" : "stopped");
	return r;
}

/* The mapped id without making one, for deletes. */
static unsigned al_xs_find(unsigned g, int drop)
{
	unsigned k;

	for (k = 0; k < g_al_real->nsm; k++)
		if (g_al_real->sm[k].g == g) {
			unsigned r = g_al_real->sm[k].r;
			if (drop)
				g_al_real->sm[k] = g_al_real->sm[--g_al_real->nsm];
			return r;
		}
	return g;
}

static unsigned al_xb_find(unsigned g, int drop)
{
	unsigned k;

	for (k = 0; k < g_al_real->nbm; k++)
		if (g_al_real->bm[k].g == g) {
			unsigned r = g_al_real->bm[k].r;
			if (drop)
				g_al_real->bm[k] = g_al_real->bm[--g_al_real->nbm];
			return r;
		}
	return g;
}

static void al_enter(void)
{
	al_lock();
	if (g_al_real->pending) {
		g_al_real->pending = 0;
		al_reconcile();
	} else if (g_al_real->purge && !savestate_any_valid()) {
		g_al_real->purge = 0;
		al_purge();
	}
}

static void __cdecl h_alGenSources(int n, unsigned *ids)
{
	int k;
	al_enter();
	r_alGenSources(n, ids);
	for (k = 0; k < n; k++) {
		AlSrc *r = al_src(&g_al_real->t, ids[k], 1), *g = al_src(&g_al_game, ids[k], 1);
		if (r)
			r->live = 1;
		if (g)
			g->live = 1;
	}
	al_unlock();
}

static void __cdecl h_alDeleteSources(int n, const unsigned *ids)
{
	int k, keep = savestate_any_valid();
	al_enter();
	for (k = 0; k < n; k++) {
		unsigned rid = al_xs_find(ids[k], 1);
		AlSrc *g = al_src(&g_al_game, ids[k], 0), *r = al_src(&g_al_real->t, rid, 0);
		if (g)
			g->live = 0;
		if (keep && r) {
			r_alSourceStop(rid);
			r_alSourcei(rid, AL_BUFFER, 0);
			r->nq = 0;
			r->live = 0;
			r->parked = 1;
			g_al_real->purge = 1;
		} else {
			if (r)
				r->live = r->parked = 0;
			r_alDeleteSources(1, &rid);
		}
	}
	al_unlock();
}

static void __cdecl h_alGenBuffers(int n, unsigned *ids)
{
	int k;
	al_enter();
	r_alGenBuffers(n, ids);
	for (k = 0; k < n; k++) {
		AlBuf *r = al_buf(&g_al_real->t, ids[k], 1), *g = al_buf(&g_al_game, ids[k], 1);
		if (r)
			r->live = 1;
		if (g)
			g->live = 1;
	}
	al_unlock();
}

static void __cdecl h_alDeleteBuffers(int n, const unsigned *ids)
{
	int k, keep = savestate_any_valid();
	al_enter();
	for (k = 0; k < n; k++) {
		unsigned rid = al_xb_find(ids[k], 1);
		AlBuf *g = al_buf(&g_al_game, ids[k], 0), *r = al_buf(&g_al_real->t, rid, 0);
		if (g) {
			g->live = 0;
			if (g->data)
				gh_free(g->data);
			g->data = NULL;
			g->cap = g->size = 0;
		}
		if (keep && r) {
			r->live = 0;
			r->parked = 1;
			g_al_real->purge = 1;
		} else {
			if (r)
				r->live = r->parked = 0;
			r_alDeleteBuffers(1, &rid);
		}
	}
	al_unlock();
}

static void __cdecl h_alSourceQueueBuffers(unsigned src, int n, const unsigned *b)
{
	unsigned rs, rb[64];
	int k;
	al_enter();
	rs = al_xs(src);
	if (!b || n <= 0 || n > 64) {
		r_alSourceQueueBuffers(rs, n, b);
		al_unlock();
		return;
	}
	for (k = 0; k < n; k++)
		rb[k] = al_xb(b[k]);
	r_alSourceQueueBuffers(rs, n, rb);
	al_queue_push(al_src(&g_al_real->t, rs, 0), rb, n);
	al_queue_push(al_src(&g_al_game, src, 0), b, n);
	al_trace("queue source %u +%d buffer(s) (first %u)\n", src, n, n > 0 ? b[0] : 0);
	al_unlock();
}

static void __cdecl h_alSourceUnqueueBuffers(unsigned src, int n, unsigned *b)
{
	unsigned tmp[64] = { 0 }, rs;
	int k, done;
	al_enter();
	rs = al_xs(src);
	/* OpenAL refuses an unqueue of more than have finished and writes nothing
	 * out, so an id coming back is what says it happened. Into our own array:
	 * games pass their list of stream buffers here, and a refused call must
	 * leave it as it was. */
	if (!b || n <= 0 || n > 64) {
		r_alSourceUnqueueBuffers(rs, n, b);
		al_unlock();
		return;
	}
	r_alSourceUnqueueBuffers(rs, n, tmp);
	done = tmp[0] != 0;
	if (done) {
		for (k = 0; k < n; k++)
			b[k] = al_xb_back(tmp[k]);
		al_queue_pop(al_src(&g_al_real->t, rs, 0), n);
		al_queue_pop(al_src(&g_al_game, src, 0), n);
	}
	al_trace("unqueue source %u %d buffer(s)%s\n", src, n, done ? "" : " - refused");
	al_unlock();
}

static void __cdecl h_alSourcei(unsigned src, int param, int v)
{
	unsigned rs;
	al_enter();
	rs = al_xs(src);
	r_alSourcei(rs, param, param == AL_BUFFER ? (int)al_xb((unsigned)v) : v);
	if (param == AL_BUFFER) {
		unsigned b = (unsigned)v, rb = al_xb(b);
		al_set_queue(al_src(&g_al_real->t, rs, 0), &rb, v ? 1 : 0, 1);
		al_set_queue(al_src(&g_al_game, src, 0), &b, v ? 1 : 0, 1);
	} else {
		AlSrc *g = al_src(&g_al_game, src, 0);
		if (g)
			al_prop_set(g->p, &g->np, AL_NPROP, param, 1, v, 0, NULL);
	}
	al_trace("source %u int %04X = %d\n", src, param, v);
	al_unlock();
}

static void al_note_src_prop(unsigned src, int param, int n, const float *v)
{
	AlSrc *g = al_src(&g_al_game, src, 0);
	if (g)
		al_prop_set(g->p, &g->np, AL_NPROP, param, 0, 0, n, v);
}

static void __cdecl h_alSourcef(unsigned src, int param, float v)
{
	al_enter();
	r_alSourcef(al_xs(src), param, v);
	al_note_src_prop(src, param, 1, &v);
	if (param == 0x100A)
		al_trace("source %u gain = %d/1000\n", src, (int)(v * 1000.0f));
	al_unlock();
}

static void __cdecl h_alSource3f(unsigned src, int param, float x, float y, float z)
{
	float v[3] = { x, y, z };
	al_enter();
	r_alSource3f(al_xs(src), param, x, y, z);
	al_note_src_prop(src, param, 3, v);
	al_unlock();
}

/* Of the float vectors a source takes, position, direction and velocity are
 * the three-element ones; the rest are scalars passed by address. */
static void __cdecl h_alSourcefv(unsigned src, int param, const float *v)
{
	al_enter();
	r_alSourcefv(al_xs(src), param, v);
	if (v)
		al_note_src_prop(src, param,
				 (param == 0x1004 || param == 0x1005 || param == 0x1006) ? 3 : 1, v);
	al_unlock();
}

static void __cdecl h_alListenerf(int param, float v)
{
	al_enter();
	r_alListenerf(param, v);
	al_prop_set(g_al_game.lp, &g_al_game.nlp, AL_NLPROP, param, 0, 0, 1, &v);
	if (param == 0x100A)
		al_trace("listener gain = %d/1000\n", (int)(v * 1000.0f));
	al_unlock();
}

/* AL_ORIENTATION (0x100F) is six floats; position and velocity are three. */
static void __cdecl h_alListenerfv(int param, const float *v)
{
	al_enter();
	r_alListenerfv(param, v);
	if (v)
		al_prop_set(g_al_game.lp, &g_al_game.nlp, AL_NLPROP, param, 0, 0,
			    param == 0x100F ? 6 : (param == 0x100A ? 1 : 3), v);
	al_unlock();
}

static void al_note_state(unsigned src, int st)
{
	AlSrc *r = al_src(&g_al_real->t, al_xs_find(src, 0), 0), *g = al_src(&g_al_game, src, 0);
	if (r)
		r->state = st;
	if (g)
		g->state = st;
}

static void __cdecl h_alSourcePlay(unsigned src)
{
	al_enter();
	r_alSourcePlay(al_xs(src));
	al_note_state(src, AL_PLAYING);
	al_trace("play source %u\n", src);
	al_unlock();
}

static void __cdecl h_alSourceStop(unsigned src)
{
	al_enter();
	r_alSourceStop(al_xs(src));
	al_note_state(src, 0x1014);
	al_trace("stop source %u\n", src);
	al_unlock();
}

static void __cdecl h_alGetSourcei(unsigned src, int param, int *v)
{
	AlSrc *g;
	al_enter();
	r_alGetSourcei(al_xs(src), param, v);
	if (param == AL_BUFFER && v)
		*v = (int)al_xb_back((unsigned)*v);
	if (param == AL_SOURCE_STATE && v) {
		g = al_src(&g_al_game, src, 0);
		if (g && g->state != *v)
			al_trace("source %u state now %04X (was %04X)\n", src, *v, g->state);
		al_note_state(src, *v);
	}
	al_unlock();
}

static void __cdecl h_alBufferData(unsigned b, int fmt, const void *data, int size, int freq)
{
	static int keep = -1;
	AlBuf *gb;

	if (keep < 0)
		keep = (int)gh_knob("D3D9SW_AL_KEEP", 1);
	al_enter();
	r_alBufferData(al_xb(b), fmt, data, size, freq);
	gb = keep && data && size > 0 ? al_buf(&g_al_game, b, 1) : NULL;
	if (gb) {
		if (gb->cap < size) {
			if (gb->data)
				gh_free(gb->data);
			gb->data = gh_malloc_at((size_t)size, 0xA1DA7Au);
			gb->cap = gb->data ? size : 0;
		}
		if (gb->data) {
			memcpy(gb->data, data, (size_t)size);
			gb->fmt = fmt;
			gb->size = size;
			gb->freq = freq;
		}
	}
	al_unlock();
}

static void __cdecl h_alBufferi(unsigned b, int param, int v)
{
	al_enter();
	r_alBufferi(al_xb(b), param, v);
	al_unlock();
}

/* After a restore's copy; the work waits for the game's next AL call. */
void gameheap_al_restored(void)
{
	if (g_al_real) {
		/* Whoever held the lock in the present has just had its context replaced. */
		InterlockedExchange(&g_al_real->lock, 0);
		InterlockedExchange(&g_al_real->pending, 1);
	}
}

static void gh_al_hook(HMODULE mod)
{
	static const struct {
		const char *name;
		void *ours;
		void **real;
	} fns[] = {
		{ "alGenSources", (void *)h_alGenSources, (void **)&r_alGenSources },
		{ "alDeleteSources", (void *)h_alDeleteSources, (void **)&r_alDeleteSources },
		{ "alGenBuffers", (void *)h_alGenBuffers, (void **)&r_alGenBuffers },
		{ "alDeleteBuffers", (void *)h_alDeleteBuffers, (void **)&r_alDeleteBuffers },
		{ "alSourceQueueBuffers", (void *)h_alSourceQueueBuffers,
		  (void **)&r_alSourceQueueBuffers },
		{ "alSourceUnqueueBuffers", (void *)h_alSourceUnqueueBuffers,
		  (void **)&r_alSourceUnqueueBuffers },
		{ "alSourcei", (void *)h_alSourcei, (void **)&r_alSourcei },
		{ "alSourcePlay", (void *)h_alSourcePlay, (void **)&r_alSourcePlay },
		{ "alSourceStop", (void *)h_alSourceStop, (void **)&r_alSourceStop },
		{ "alGetSourcei", (void *)h_alGetSourcei, (void **)&r_alGetSourcei },
		{ "alSourcef", (void *)h_alSourcef, (void **)&r_alSourcef },
		{ "alSource3f", (void *)h_alSource3f, (void **)&r_alSource3f },
		{ "alSourcefv", (void *)h_alSourcefv, (void **)&r_alSourcefv },
		{ "alListenerf", (void *)h_alListenerf, (void **)&r_alListenerf },
		{ "alListenerfv", (void *)h_alListenerfv, (void **)&r_alListenerfv },
		{ "alBufferData", (void *)h_alBufferData, (void **)&r_alBufferData },
		{ "alBufferi", (void *)h_alBufferi, (void **)&r_alBufferi },
		{ "alGetError", NULL, (void **)&r_alGetError },
		{ "alIsSource", NULL, (void **)&r_alIsSource },
		{ "alIsBuffer", NULL, (void **)&r_alIsBuffer },
	};
	HMODULE al = GetModuleHandleA("OpenAL32.dll");
	unsigned k;

	if (!al || !r_va || mod == g_self || !gh_knob("D3D9SW_REWIND_AL", 1))
		return;
	if (!g_al_real) {
		g_al_real = (AlReal *)gh_home_alloc(GH_HOME_AL, sizeof(AlReal));
		if (!g_al_real)
			return;
	}
	for (k = 0; k < sizeof(fns) / sizeof(fns[0]); k++)
		if (!*fns[k].real)
			*fns[k].real = (void *)GetProcAddress(al, fns[k].name);
	for (k = 0; k < sizeof(fns) / sizeof(fns[0]); k++)
		if (!*fns[k].real)
			return;
	for (k = 0; k < sizeof(fns) / sizeof(fns[0]); k++)
		if (fns[k].ours)
			gh_iat_swap(mod, "OpenAL32.dll", fns[k].name, fns[k].ours, NULL);
}

/* The game's open C streams. A FILE lives in the runtime's own heap, which we
 * leave in the present, so a load from another launch brings back the game's
 * FILE pointers with nothing behind them. Path, mode and position are enough to
 * open the stream again there and hand the game the new pointer. */
#define GH_STREAM_MAX 64
typedef struct {
	void *f;
	WCHAR path[MAX_PATH];
	char mode[8];
} GhStream;

static GhStream g_streams[GH_STREAM_MAX];
static volatile LONG g_streams_lock;
static void *(__cdecl *r_fopen)(const char *, const char *);
static int(__cdecl *r_fopen_s)(void **, const char *, const char *);
static void *(__cdecl *r_wfopen)(const WCHAR *, const WCHAR *);
static int(__cdecl *r_wfopen_s)(void **, const WCHAR *, const WCHAR *);
static int(__cdecl *r_fclose)(void *);

static void gh_stream_note(void *f, const WCHAR *wpath, const char *path, const WCHAR *wmode,
			   const char *mode)
{
	int i, k;

	if (!f)
		return;
	while (InterlockedCompareExchange(&g_streams_lock, 1, 0))
		YieldProcessor();
	for (i = 0; i < GH_STREAM_MAX && g_streams[i].f; i++)
		;
	if (i < GH_STREAM_MAX) {
		g_streams[i].f = f;
		if (wpath)
			lstrcpynW(g_streams[i].path, wpath, MAX_PATH);
		else if (!MultiByteToWideChar(CP_ACP, 0, path, -1, g_streams[i].path, MAX_PATH))
			g_streams[i].path[0] = 0;
		for (k = 0; k < 7 && (wmode ? wmode[k] : mode[k]); k++)
			g_streams[i].mode[k] = (char)(wmode ? wmode[k] : mode[k]);
		g_streams[i].mode[k] = 0;
	}
	InterlockedExchange(&g_streams_lock, 0);
}

static void *__cdecl gh_fopen(const char *path, const char *mode)
{
	void *f = r_fopen(path, mode);

	if (path && mode)
		gh_stream_note(f, NULL, path, NULL, mode);
	return f;
}

static int __cdecl gh_fopen_s(void **out, const char *path, const char *mode)
{
	int e = r_fopen_s(out, path, mode);

	if (!e && out && path && mode)
		gh_stream_note(*out, NULL, path, NULL, mode);
	return e;
}

static void *__cdecl gh_wfopen(const WCHAR *path, const WCHAR *mode)
{
	void *f = r_wfopen(path, mode);

	if (path && mode)
		gh_stream_note(f, path, NULL, mode, NULL);
	return f;
}

static int __cdecl gh_wfopen_s(void **out, const WCHAR *path, const WCHAR *mode)
{
	int e = r_wfopen_s(out, path, mode);

	if (!e && out && path && mode)
		gh_stream_note(*out, path, NULL, mode, NULL);
	return e;
}

static int __cdecl gh_fclose(void *f)
{
	int i;

	while (InterlockedCompareExchange(&g_streams_lock, 1, 0))
		YieldProcessor();
	for (i = 0; i < GH_STREAM_MAX; i++)
		if (g_streams[i].f == f)
			g_streams[i].f = NULL;
	InterlockedExchange(&g_streams_lock, 0);
	return r_fclose(f);
}

static void *gh_crt_fn(const char *name)
{
	HMODULE crt = GetModuleHandleA("ucrtbase.dll");

	return crt ? (void *)GetProcAddress(crt, name) : NULL;
}

/* Called with the game suspended, so no lock: a thread frozen holding it would
 * never let go. The position comes from the _nolock teller for the same reason. */
int gameheap_streams(uintptr_t *f, WCHAR (*path)[MAX_PATH], char (*mode)[8], long long *pos,
		     int max)
{
	long long(__cdecl *tell)(void *) = (long long(__cdecl *)(void *))gh_crt_fn(
		"_ftelli64_nolock");
	int i, n = 0;

	for (i = 0; i < GH_STREAM_MAX && n < max; i++) {
		MEMORY_BASIC_INFORMATION mi;

		if (!g_streams[i].f || !g_streams[i].path[0])
			continue;
		if (!VirtualQuery(g_streams[i].f, &mi, sizeof(mi)) || mi.State != MEM_COMMIT ||
		    !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
			g_streams[i].f = NULL;
			continue;
		}
		f[n] = (uintptr_t)g_streams[i].f;
		lstrcpynW(path[n], g_streams[i].path, MAX_PATH);
		memcpy(mode[n], g_streams[i].mode, 8);
		pos[n] = tell ? tell(g_streams[i].f) : -1;
		n++;
	}
	return n;
}

/* Writing modes come back as "r+", keeping b or t: the file already holds what
 * the game wrote, and "w" would truncate it. */
void *gameheap_stream_reopen(uintptr_t old, const WCHAR *path, const char *mode, long long pos)
{
	void *(__cdecl *open)(const WCHAR *, const WCHAR *) =
		r_wfopen ? r_wfopen
			 : (void *(__cdecl *)(const WCHAR *, const WCHAR *))gh_crt_fn("_wfopen");
	int(__cdecl *seek)(void *, long long, int) =
		(int(__cdecl *)(void *, long long, int))gh_crt_fn("_fseeki64");
	WCHAR wm[8];
	void *f;
	int k, n = 0;

	if (!open)
		return NULL;
	if (strchr(mode, 'w') || strchr(mode, 'a')) {
		wm[n++] = 'r';
		wm[n++] = '+';
		if (strchr(mode, 'b'))
			wm[n++] = 'b';
		else if (strchr(mode, 't'))
			wm[n++] = 't';
	} else {
		for (k = 0; mode[k] && n < 7; k++)
			wm[n++] = (WCHAR)mode[k];
	}
	wm[n] = 0;
	while (InterlockedCompareExchange(&g_streams_lock, 1, 0))
		YieldProcessor();
	for (k = 0; k < GH_STREAM_MAX; k++)
		if ((uintptr_t)g_streams[k].f == old)
			g_streams[k].f = NULL;
	InterlockedExchange(&g_streams_lock, 0);
	f = open(path, wm);
	if (!f)
		return NULL;
	if (pos > 0 && seek)
		seek(f, pos, 0);
	gh_stream_note(f, path, NULL, NULL, mode);
	return f;
}

static void gh_stream_hook(HMODULE mod)
{
	static const char *const dlls[] = { "api-ms-win-crt-stdio-l1-1-0.dll", "ucrtbase.dll" };
	unsigned d;

	if (mod == g_self || !gh_knob("D3D9SW_STREAMS", 1))
		return;
	for (d = 0; d < sizeof(dlls) / sizeof(dlls[0]); d++) {
		gh_iat_swap(mod, dlls[d], "fopen", (void *)gh_fopen, (void **)&r_fopen);
		gh_iat_swap(mod, dlls[d], "fopen_s", (void *)gh_fopen_s, (void **)&r_fopen_s);
		gh_iat_swap(mod, dlls[d], "_wfopen", (void *)gh_wfopen, (void **)&r_wfopen);
		gh_iat_swap(mod, dlls[d], "_wfopen_s", (void *)gh_wfopen_s, (void **)&r_wfopen_s);
		gh_iat_swap(mod, dlls[d], "fclose", (void *)gh_fclose, (void **)&r_fclose);
	}
}

/* Borderless instead of exclusive fullscreen. A GL game's fullscreen is a
 * desktop-sized popup plus a display mode switch, and Haydee minimizes itself
 * through ShowWindow whenever it loses focus. Without the mode switch and that
 * one ShowWindow the popup is a borderless window that alt-tabs like any other.
 * A minimize asked for by the user goes through DefWindowProc, not these. */
static LONG WINAPI gh_cds(DEVMODEA *dm, DWORD flags)
{
	(void)dm;
	(void)flags;
	return DISP_CHANGE_SUCCESSFUL;
}

static BOOL(WINAPI *r_showwindow)(HWND, int);

static BOOL WINAPI gh_showwindow(HWND h, int cmd)
{
	if (cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED || cmd == SW_SHOWMINNOACTIVE ||
	    cmd == SW_FORCEMINIMIZE)
		return IsWindowVisible(h);
	return r_showwindow(h, cmd);
}

static void gh_window_hook(HMODULE mod, const WCHAR *path)
{
	int a, b;

	if (mod == g_self || !gh_knob("D3D9SW_BORDERLESS", 0))
		return;
	a = gh_iat_swap(mod, NULL, "ChangeDisplaySettingsA", (void *)gh_cds, NULL);
	b = gh_iat_swap(mod, NULL, "ShowWindow", (void *)gh_showwindow, (void **)&r_showwindow);
	if (a > 0 || b > 0)
		ss_log("gameheap: %ls - borderless, %d mode switch and %d ShowWindow import(s) "
		       "taken over\n",
		       path, a, b);
}

/* DirectInput's objects live in its own heap, which a load from another launch
 * leaves in the present, so the game's saved device pointers lead nowhere. It
 * gets proxies instead, handed out lowest-free-first from a fixed home and held
 * out of the rewind: same creation order, same proxy address in every launch,
 * each forwarding to whatever device is live here. */
#define GH_HOME_DI 0x5FEE0000u
#define GH_DI_MAX 64

typedef struct {
	const void *const *vt;
	void *real;
} GhDiProxy;

typedef struct {
	volatile LONG lock;
	LONG made;
	GhDiProxy p[GH_DI_MAX];
} GhDiHome;

static GhDiHome *g_di SS_PRESENT;
static int g_di_held;
static HRESULT(WINAPI *r_di8create)(HINSTANCE, DWORD, const GUID *, void **,
				    void *) SS_PRESENT;

#if defined(__i386__) || defined(_M_IX86)
/* this is the first stdcall argument: swap the proxy for the live object and
 * jump into the live object's own method. */
#define GH_DI_FWD(i)                                                                     \
	__asm__(".text\n.globl _gh_di_fwd" #i "\n_gh_di_fwd" #i ":\n"                    \
		"\tmovl 4(%esp), %eax\n\tmovl 4(%eax), %eax\n\tmovl %eax, 4(%esp)\n"     \
		"\tmovl (%eax), %ecx\n\tjmp *" #i "*4(%ecx)\n");                          \
	void gh_di_fwd##i(void);
GH_DI_FWD(0) GH_DI_FWD(1) GH_DI_FWD(3) GH_DI_FWD(4) GH_DI_FWD(5) GH_DI_FWD(6)
GH_DI_FWD(7) GH_DI_FWD(8) GH_DI_FWD(9) GH_DI_FWD(10) GH_DI_FWD(11) GH_DI_FWD(12)
GH_DI_FWD(13) GH_DI_FWD(14) GH_DI_FWD(15) GH_DI_FWD(16) GH_DI_FWD(17) GH_DI_FWD(18)
GH_DI_FWD(19) GH_DI_FWD(20) GH_DI_FWD(21) GH_DI_FWD(22) GH_DI_FWD(23) GH_DI_FWD(24)
GH_DI_FWD(25) GH_DI_FWD(26) GH_DI_FWD(27) GH_DI_FWD(28) GH_DI_FWD(29) GH_DI_FWD(30)
GH_DI_FWD(31)

static ULONG WINAPI gh_di_release(GhDiProxy *self);
static HRESULT WINAPI gh_di_createdevice(GhDiProxy *self, const GUID *g, void **out,
					 void *outer);

/* IDirectInput8A/W: 11 methods, CreateDevice at 3. */
static const void *const g_di_vt8[11] = {
	(void *)gh_di_fwd0, (void *)gh_di_fwd1,		(void *)gh_di_release,
	(void *)gh_di_createdevice, (void *)gh_di_fwd4, (void *)gh_di_fwd5,
	(void *)gh_di_fwd6, (void *)gh_di_fwd7,		(void *)gh_di_fwd8,
	(void *)gh_di_fwd9, (void *)gh_di_fwd10,
};

/* IDirectInputDevice8A/W: 32 methods. */
static const void *const g_di_vtdev[32] = {
	(void *)gh_di_fwd0,  (void *)gh_di_fwd1,  (void *)gh_di_release, (void *)gh_di_fwd3,
	(void *)gh_di_fwd4,  (void *)gh_di_fwd5,  (void *)gh_di_fwd6,	 (void *)gh_di_fwd7,
	(void *)gh_di_fwd8,  (void *)gh_di_fwd9,  (void *)gh_di_fwd10,	 (void *)gh_di_fwd11,
	(void *)gh_di_fwd12, (void *)gh_di_fwd13, (void *)gh_di_fwd14,	 (void *)gh_di_fwd15,
	(void *)gh_di_fwd16, (void *)gh_di_fwd17, (void *)gh_di_fwd18,	 (void *)gh_di_fwd19,
	(void *)gh_di_fwd20, (void *)gh_di_fwd21, (void *)gh_di_fwd22,	 (void *)gh_di_fwd23,
	(void *)gh_di_fwd24, (void *)gh_di_fwd25, (void *)gh_di_fwd26,	 (void *)gh_di_fwd27,
	(void *)gh_di_fwd28, (void *)gh_di_fwd29, (void *)gh_di_fwd30,	 (void *)gh_di_fwd31,
};

static void *gh_di_wrap(void *real, const void *const *vt, const char *what)
{
	int i;

	while (InterlockedCompareExchange(&g_di->lock, 1, 0))
		SwitchToThread();
	for (i = 0; i < GH_DI_MAX && g_di->p[i].real; i++)
		;
	if (i < GH_DI_MAX) {
		g_di->p[i].vt = vt;
		g_di->p[i].real = real;
		g_di->made++;
	}
	InterlockedExchange(&g_di->lock, 0);
	if (i >= GH_DI_MAX) {
		ss_log("dinput: out of proxies, %s %p handed out as it is\n", what, real);
		return real;
	}
	ss_log("dinput: %s %p behind proxy %d at %p\n", what, real, i, (void *)&g_di->p[i]);
	return &g_di->p[i];
}

static ULONG WINAPI gh_di_release(GhDiProxy *self)
{
	void *r = self->real;
	ULONG n = ((ULONG(WINAPI *)(void *))(*(void ***)r)[2])(r);

	if (!n)
		self->real = NULL;
	return n;
}

static HRESULT WINAPI gh_di_createdevice(GhDiProxy *self, const GUID *g, void **out,
					 void *outer)
{
	void *r = self->real;
	HRESULT hr = ((HRESULT(WINAPI *)(void *, const GUID *, void **, void *))(
		*(void ***)r)[3])(r, g, out, outer);

	if (SUCCEEDED(hr) && out && *out)
		*out = gh_di_wrap(*out, g_di_vtdev, "device");
	return hr;
}

static HRESULT WINAPI gh_di8create(HINSTANCE h, DWORD v, const GUID *riid, void **out,
				   void *outer)
{
	HRESULT hr = r_di8create(h, v, riid, out, outer);

	if (SUCCEEDED(hr) && out && *out && !outer)
		*out = gh_di_wrap(*out, g_di_vt8, "IDirectInput8");
	return hr;
}

static int gh_di_home(void)
{
	if (!g_di)
		g_di = (GhDiHome *)gh_home_alloc(GH_HOME_DI, sizeof(GhDiHome));
	return g_di != NULL;
}

/* CLSID_DirectInput8: what the game actually asks COM for. */
static const GUID k_clsid_di8 = { 0x25E609E4, 0xB259, 0x11CF,
				  { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static HRESULT(WINAPI *r_cocreate)(const GUID *, void *, DWORD, const GUID *,
				   void **) SS_PRESENT;

static HRESULT WINAPI gh_cocreate(const GUID *clsid, void *outer, DWORD ctx, const GUID *iid,
				  void **out)
{
	HRESULT hr = r_cocreate(clsid, outer, ctx, iid, out);

	if (SUCCEEDED(hr) && out && *out && !outer && clsid &&
	    !memcmp(clsid, &k_clsid_di8, sizeof(GUID)) && gh_di_home()) {
		ss_log("dinput: CoCreateInstance for interface %08lX\n",
		       iid ? (unsigned long)iid->Data1 : 0ul);
		*out = gh_di_wrap(*out, g_di_vt8, "IDirectInput8");
	}
	return hr;
}

static FARPROC WINAPI gh_getprocaddress(HMODULE m, LPCSTR name)
{
	FARPROC f = GetProcAddress(m, name);

	if (f && ((ULONG_PTR)name >> 16) && !lstrcmpA(name, "DirectInput8Create")) {
		if (gh_di_home()) {
			r_di8create = (HRESULT(WINAPI *)(HINSTANCE, DWORD, const GUID *, void **,
							 void *))(void *)f;
			f = (FARPROC)(void *)gh_di8create;
		}
	}
	return f;
}

static void gh_di_hook(HMODULE mod, const WCHAR *path)
{
	const WCHAR *leaf = path;
	int i;

	for (i = 0; path[i]; i++)
		if (path[i] == '\\' || path[i] == '/')
			leaf = path + i + 1;
	if (mod == g_self || !gh_knob("D3D9SW_DIPROXY", 0))
		return;
	/* A static import (DDPR's default.exe) goes straight to DINPUT8. */
	if (gh_di_home() && gh_iat_swap(mod, "DINPUT8.dll", "DirectInput8Create",
					(void *)gh_di8create, (void **)&r_di8create) > 0)
		ss_log("gameheap: %ls - imported DirectInput8Create will hand out proxies\n",
		       path);
	/* Only the module that asks for DirectInput at run time: launcher.exe
	 * carries Steam's DRM, and a hooked import there ends the process before
	 * it starts. */
	if (lstrcmpiW(leaf, L"haydee.dll"))
		return;
	if (gh_iat_swap(mod, NULL, "GetProcAddress", (void *)gh_getprocaddress, NULL) > 0)
		ss_log("gameheap: %ls - DirectInput8Create will hand out proxies\n", path);
	if (gh_iat_swap(mod, NULL, "CoCreateInstance", (void *)gh_cocreate,
			(void **)&r_cocreate) > 0)
		ss_log("gameheap: %ls - CoCreateInstance(DirectInput8) will hand out proxies\n",
		       path);
}
#else
static void gh_di_hook(HMODULE mod, const WCHAR *path)
{
	(void)mod;
	(void)path;
}
#endif

/* game.dll is loaded late by launcher.exe and opts into ASLR, so it lands
 * wherever the loader finds room - 69490000 in one launch, 69500000 in the next -
 * and every restored pointer into it is then wrong. Its image is mapped as
 * usual, recognised by name, and mapped again at a fixed home; the loader
 * relocates any image not at its preferred base anyway. */
#define GH_HOME_GAMEDLL 0x3FF00000u

typedef LONG(NTAPI *PFN_GhNtMap)(HANDLE, HANDLE, PVOID *, ULONG_PTR, SIZE_T, PLARGE_INTEGER,
				 PSIZE_T, DWORD, ULONG, ULONG);
static PFN_GhNtMap r_ntmap;
static LONG(NTAPI *r_ntunmap)(HANDLE, PVOID);
static int g_gamedll_pinned;

static LONG NTAPI gh_ntmap(HANDLE sec, HANDLE proc, PVOID *base, ULONG_PTR zb, SIZE_T commit,
			   PLARGE_INTEGER off, PSIZE_T vsize, DWORD inh, ULONG at, ULONG prot)
{
	SIZE_T asked = vsize ? *vsize : 0;
	int watch = !g_gamedll_pinned && proc == GetCurrentProcess() && base && !*base;
	LONG st = r_ntmap(sec, proc, base, zb, commit, off, vsize, inh, at, prot);
	MEMORY_BASIC_INFORMATION mbi;
	WCHAR fn[MAX_PATH];
	DWORD n;
	PVOID was, home;
	SIZE_T vs;
	LONG st2;

	if (!watch || st < 0 || !*base || !VirtualQuery(*base, &mbi, sizeof(mbi)) ||
	    mbi.Type != MEM_IMAGE)
		return st;
	n = K32GetMappedFileNameW(GetCurrentProcess(), *base, fn, MAX_PATH);
	if (n < 9 || lstrcmpiW(fn + n - 9, L"\\game.dll"))
		return st;
	g_gamedll_pinned = 1;
	was = *base;
	if ((uintptr_t)was == GH_HOME_GAMEDLL)
		return st;
	r_ntunmap(proc, was);
	home = (PVOID)GH_HOME_GAMEDLL;
	vs = asked;
	st2 = r_ntmap(sec, proc, &home, zb, commit, off, &vs, inh, at, prot);
	if (st2 >= 0) {
		ss_log("gameheap: game.dll PINNED at %p (the loader chose %p)\n", home, was);
		*base = home;
	} else {
		ss_log("gameheap: game.dll could not go to its home %08lX (status %08lX) - "
		       "left where the loader puts it\n",
		       (unsigned long)GH_HOME_GAMEDLL, (unsigned long)st2);
		home = NULL;
		vs = asked;
		st2 = r_ntmap(sec, proc, &home, zb, commit, off, &vs, inh, at, prot);
		*base = home;
	}
	if (vsize)
		*vsize = vs;
	return st2;
}

static void gh_gamedll_pin(void)
{
	HMODULE nt = GetModuleHandleA("ntdll.dll");

	if (!gh_knob("D3D9SW_PIN_GAMEDLL", 1) || GetModuleHandleA("game.dll"))
		return;
	r_ntunmap = (LONG(NTAPI *)(HANDLE, PVOID))(void *)GetProcAddress(nt, "NtUnmapViewOfSection");
	if (r_ntunmap)
		ntdll_inline_hook("NtMapViewOfSection", (void *)gh_ntmap, (void **)&r_ntmap);
}

/* Handle values from another launch that something else holds here: the game's
 * calls on them go to the stand-in the load made (savestate_hx). */
HANDLE savestate_hx(HANDLE h);
int savestate_hx_close(HANDLE h);

static DWORD WINAPI gh_wfso(HANDLE h, DWORD ms)
{
	return WaitForSingleObject(savestate_hx(h), ms);
}

static DWORD WINAPI gh_wfsoex(HANDLE h, DWORD ms, BOOL alert)
{
	return WaitForSingleObjectEx(savestate_hx(h), ms, alert);
}

static DWORD WINAPI gh_wfmoex(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, BOOL alert)
{
	HANDLE t[MAXIMUM_WAIT_OBJECTS];
	DWORD k;

	if (n > MAXIMUM_WAIT_OBJECTS || !hs)
		return WaitForMultipleObjectsEx(n, hs, all, ms, alert);
	for (k = 0; k < n; k++)
		t[k] = savestate_hx(hs[k]);
	return WaitForMultipleObjectsEx(n, t, all, ms, alert);
}

static DWORD WINAPI gh_wfmo(DWORD n, const HANDLE *hs, BOOL all, DWORD ms)
{
	return gh_wfmoex(n, hs, all, ms, FALSE);
}

static BOOL WINAPI gh_setevent(HANDLE h)
{
	return SetEvent(savestate_hx(h));
}

static BOOL WINAPI gh_resetevent(HANDLE h)
{
	return ResetEvent(savestate_hx(h));
}

static BOOL WINAPI gh_closehandle(HANDLE h)
{
	return savestate_hx_close(h) ? TRUE : CloseHandle(h);
}

static BOOL WINAPI gh_releasemutex(HANDLE h)
{
	return ReleaseMutex(savestate_hx(h));
}

static DWORD WINAPI gh_resumethread(HANDLE h)
{
	return ResumeThread(savestate_hx(h));
}

static BOOL WINAPI gh_setthreadpriority(HANDLE h, int p)
{
	return SetThreadPriority(savestate_hx(h), p);
}

static DWORD_PTR WINAPI gh_setthreadaffinity(HANDLE h, DWORD_PTR m)
{
	return SetThreadAffinityMask(savestate_hx(h), m);
}

static void gh_handle_hook(HMODULE mod)
{
	static const struct {
		const char *name;
		void *to;
	} fns[] = {
		{ "WaitForSingleObject", (void *)gh_wfso },
		{ "WaitForSingleObjectEx", (void *)gh_wfsoex },
		{ "WaitForMultipleObjects", (void *)gh_wfmo },
		{ "WaitForMultipleObjectsEx", (void *)gh_wfmoex },
		{ "SetEvent", (void *)gh_setevent },
		{ "ResetEvent", (void *)gh_resetevent },
		{ "CloseHandle", (void *)gh_closehandle },
		{ "ReleaseMutex", (void *)gh_releasemutex },
		{ "ResumeThread", (void *)gh_resumethread },
		{ "SetThreadPriority", (void *)gh_setthreadpriority },
		{ "SetThreadAffinityMask", (void *)gh_setthreadaffinity },
	};
	unsigned k;

	if (mod == g_self || !gh_knob("D3D9SW_HX", 1))
		return;
	for (k = 0; k < sizeof(fns) / sizeof(fns[0]); k++)
		gh_iat_swap(mod, NULL, fns[k].name, fns[k].to, NULL);
}

/* steam_api keeps each module's interface table in that module's data and
 * refills it only when the table's counter differs from its own call counter.
 * A load from another launch brings back the old table with a counter that can
 * equal the new launch's, so the next achievement calls into a steamclient
 * object from the dead process. Zeroing the counter makes every call refill;
 * 0 is what a never-filled table holds, so before SteamAPI_Init nothing changes. */
typedef struct {
	void (*init)(void *);
	uintptr_t counter;
} GhSteamCtx;

/* Looked up per call: our own data is rewound too, so a cached pointer could be
 * the old launch's steam_api. */
static void *__cdecl gh_steam_ctx(void *p)
{
	HMODULE sa = GetModuleHandleA("steam_api.dll");
	void *(__cdecl *real)(void *) =
		sa ? (void *(__cdecl *)(void *))(void *)GetProcAddress(sa, "SteamInternal_ContextInit")
		   : NULL;

	if (!real)
		return p ? (char *)p + sizeof(GhSteamCtx) : NULL;
	if (p)
		((GhSteamCtx *)p)->counter = 0;
	return real(p);
}

static void gh_steam_hook(HMODULE mod)
{
	if (mod == g_self || !gh_knob("D3D9SW_STEAM_CTX", 1))
		return;
	gh_iat_swap(mod, "steam_api.dll", "SteamInternal_ContextInit", (void *)gh_steam_ctx,
		    NULL);
}

/* D3D9SW_XA27=1: a game that asks COM for XAudio2 2.7 gets the software engine
 * instead. Its voices then live in xa2_sw's fixed arena and rewind with the
 * game, where the real engine's heap moved between launches and its mixer
 * thread called into state a restore had replaced. */
HRESULT xa2_sw_create27(void **out);
static HRESULT(WINAPI *r_xa_cocreate)(const GUID *, void *, DWORD, const GUID *, void **);
static const GUID k_clsid_xa27 = { 0x5A508685, 0xA254, 0x4FBA,
				   { 0x9B, 0x82, 0x9A, 0x24, 0xB0, 0x03, 0x06, 0xAF } };
static const GUID k_clsid_xa27d = { 0xDB05EA35, 0x0329, 0x4D4B,
				    { 0xA5, 0x3A, 0x6D, 0xEA, 0xD0, 0x3D, 0x38, 0x52 } };

static HRESULT WINAPI gh_xa_cocreate(const GUID *clsid, void *outer, DWORD ctx, const GUID *iid,
				     void **out)
{
	if (clsid && !outer && (!memcmp(clsid, &k_clsid_xa27, sizeof(GUID)) ||
				!memcmp(clsid, &k_clsid_xa27d, sizeof(GUID))))
		return xa2_sw_create27(out);
	return r_xa_cocreate(clsid, outer, ctx, iid, out);
}

static void gh_xa27_hook(HMODULE mod, const WCHAR *path)
{
	if (mod == g_self || !gh_knob("D3D9SW_XA27", 0))
		return;
	if (gh_iat_swap(mod, "ole32.dll", "CoCreateInstance", (void *)gh_xa_cocreate,
			(void **)&r_xa_cocreate) > 0)
		ss_log("gameheap: %ls - CoCreateInstance(XAudio2 2.7) goes to xa2_sw\n", path);
}

static void gh_patch_module(HMODULE mod, const WCHAR *path)
{
	unsigned f, d;
	int total = 0, unbound = 0;

	gh_heapcreate_hook(mod);
	ct_hook(mod, path);
	if (!gh_is_game_module(mod, path))
		return;
	gh_physx_hook(mod, path);
	gh_al_hook(mod);
	gh_stream_hook(mod);
	gh_window_hook(mod, path);
	gh_di_hook(mod, path);
	gh_handle_hook(mod);
	gh_steam_hook(mod);
	gh_xa27_hook(mod, path);
	if (g_led && mod != g_self)
		gh_iat_swap(mod, NULL, "_beginthreadex", (void *)gh_beginthreadex,
			    (void **)&r_beginthreadex);
	if (g_vaj && mod != g_self) {
		gh_iat_swap(mod, NULL, "VirtualAlloc", (void *)gh_va_j, NULL);
		gh_iat_swap(mod, NULL, "VirtualFree", (void *)gh_vf_j, NULL);
	}
	if (mod != g_self) {
		static const char *const rewound[] = { "QueryPerformanceCounter",
						       "GetTickCount", "timeGetTime",
						       "CreateEventA", "CreateEventW",
						       "CreateMutexA", "CreateThread" };
		unsigned k;

		for (k = 0; k < sizeof(rewound) / sizeof(rewound[0]); k++) {
			void *to = savestate_game_import_hook(rewound[k]);

			if (to)
				gh_iat_swap(mod, NULL, rewound[k], to, NULL);
		}
	}
	for (f = 0; f < sizeof(kImpFns) / sizeof(kImpFns[0]); f++)
		for (d = 0; d < sizeof(kImpDlls) / sizeof(kImpDlls[0]); d++) {
			int n = gh_iat_swap(mod, kImpDlls[d], kImpFns[f].name,
					    kImpFns[f].ours, NULL);

			if (n < 0)
				unbound = 1;
			else
				total += n;
		}
	if (unbound) {
		g_mods_unbound++;
		ss_log("gameheap: %ls - imports not resolved yet, left alone\n", path);
		return;
	}
	if (!total)
		return;
	g_mods_patched++;
	g_slots_patched += (unsigned long)total;
	ss_log("gameheap: %ls - %d allocator import(s) now on the private heap\n", path,
	       total);
}

typedef struct {
	USHORT len, cap;
	PWSTR buf;
} GhUStr;

typedef struct {
	ULONG flags;
	GhUStr *full, *base;
	PVOID dll_base;
	ULONG size;
} GhDllNote;

/* Delivered after the new module's imports are bound and before its own
 * initialisation runs (measured), which is the moment that matters: game.dll
 * arrives by LoadLibrary long after attach, and patching it here means even its
 * static constructors allocate from us. */
static VOID CALLBACK gh_dll_loaded(ULONG reason, GhDllNote *d, PVOID ctx)
{
	WCHAR path[MAX_PATH];
	unsigned n;

	(void)ctx;
	if (reason != 1 || !d || !d->full || !d->full->buf)
		return;
	n = d->full->len / sizeof(WCHAR);
	if (n >= MAX_PATH)
		return;
	memcpy(path, d->full->buf, n * sizeof(WCHAR));
	path[n] = 0;
	gh_patch_module((HMODULE)d->dll_base, path);
}

static void gh_patch_loaded(void)
{
	HMODULE mods[512];
	DWORD need = 0, i;
	WCHAR path[MAX_PATH];

	if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need))
		return;
	need /= sizeof(HMODULE);
	if (need > 512)
		need = 512;
	for (i = 0; i < need; i++)
		if (GetModuleFileNameW(mods[i], path, MAX_PATH))
			gh_patch_module(mods[i], path);
}

static void gh_add_range(uintptr_t lo, uintptr_t hi)
{
	LONG n = g_nreg;

	if (n >= GH_REG || !hi)
		return;
	g_lo[n] = lo;
	g_hi[n] = hi;
	InterlockedIncrement(&g_nreg);
}

/* After a merge took the pinned big-block spans from the save, the chunks in
 * them are the save's, but the list naming them is this launch's - made at a
 * different moment, so it can miss chunks the save had. A block in one of those
 * is then not ours as far as gh_free can tell, and goes to a heap that never
 * issued it. Read the chunks back from their own headers instead. */
static SIZE_T big_walk(uintptr_t base, SIZE_T size, LONG *n)
{
	SIZE_T off = 0;

	while (base && off + GH_BIG_GRAIN <= size && *n < GH_BIG_CHUNKS) {
		GhBigArena *c = (GhBigArena *)(base + off);
		MEMORY_BASIC_INFORMATION mbi;
		LONG k;

		if (!VirtualQuery(c, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !c->cap ||
		    c->cap > size - off || (c->cap & 0xFFFFu) || c->top > c->cap)
			break;
		g_bigch[(*n)++] = c;
		for (k = 0; k < g_nreg && !((uintptr_t)c >= g_lo[k] && (uintptr_t)c < g_hi[k]); k++)
			;
		if (k == g_nreg)
			gh_add_range((uintptr_t)c, (uintptr_t)c + c->cap);
		off += c->cap;
	}
	return off;
}

/* D3D9SW_SWARENA=base, D3D9SW_SWARENA_MB: the renderer's DEFAULT-pool memory -
 * the game's render targets, depth surfaces, default textures and buffers - in
 * a reservation of its own, excluded from every save and never taken by a
 * merge. The device reset after a load has the game release and remake all of
 * it, so none of it is state. A managed texture is state: the game never makes
 * it again, so its pixels stay in the big-block span with the object naming it.
 *
 * Freed blocks stay committed. A rewound object can name a block until the game
 * releases it, and its reads and the blank after a load must not fault. The
 * table describes the present; gameheap_swa_sweep frees what nothing names. */
#define SWA_EXT 8192u
#define SWA_MIN (64u * 1024u)
#define SWA_HDR 64u

typedef struct {
	unsigned off, size, used;
} SwaExt;

static struct {
	uintptr_t base;
	size_t size, live, peak;
	SwaExt *ext;
	unsigned n, fails, stale, held, sweeps, swept_n;
	size_t swept;
	volatile LONG lock;
} g_swa SS_PRESENT;

HANDLE sw_heap(void);

static void swa_lock(void)
{
	while (InterlockedCompareExchange(&g_swa.lock, 1, 0))
		Sleep(0);
}

static void swa_unlock(void)
{
	InterlockedExchange(&g_swa.lock, 0);
}

static void swa_reserve(void)
{
	uintptr_t base = gh_knob("D3D9SW_SWARENA", 0);
	unsigned mb = gh_knob("D3D9SW_SWARENA_MB", 320);
	void *res, *ext;

	if (!base || !mb)
		return;
	res = VirtualAlloc((void *)base, (SIZE_T)mb << 20, MEM_RESERVE, PAGE_READWRITE);
	ext = VirtualAlloc(NULL, SWA_EXT * sizeof(SwaExt), MEM_RESERVE | MEM_COMMIT,
			   PAGE_READWRITE);
	if (!res || (uintptr_t)res != base || !ext) {
		ss_log("swarena: could not reserve %u MB at %08lX (error %lu) - DEFAULT-pool "
		       "memory stays in the big-block span\n",
		       mb, (unsigned long)base, GetLastError());
		if (res)
			VirtualFree(res, 0, MEM_RELEASE);
		if (ext)
			VirtualFree(ext, 0, MEM_RELEASE);
		return;
	}
	g_swa.ext = (SwaExt *)ext;
	g_swa.ext[0].off = 0;
	g_swa.ext[0].size = mb << 20;
	g_swa.ext[0].used = 0;
	g_swa.n = 1;
	g_swa.size = (size_t)mb << 20;
	g_swa.base = base;
	ss_log("swarena: %u MB at %08lX for DEFAULT-pool memory, held out of every save\n", mb,
	       (unsigned long)base);
}

/* The extent holding offset `off`; the table tiles the arena in order. */
static SwaExt *swa_find(size_t off)
{
	unsigned lo = 0, hi = g_swa.n;

	while (lo < hi) {
		unsigned mid = (lo + hi) / 2;
		SwaExt *e = &g_swa.ext[mid];

		if (off < e->off)
			hi = mid;
		else if (off >= (size_t)e->off + e->size)
			lo = mid + 1;
		else
			return e;
	}
	return NULL;
}

/* Zeroed, or NULL to let the caller use the span. */
void *gameheap_swa_alloc(size_t n)
{
	size_t want = (n + SWA_HDR + 0xFFFu) & ~(size_t)0xFFFu;
	unsigned i;
	unsigned char *raw = NULL;

	if (!g_swa.base || n < SWA_MIN || want < n || want > g_swa.size)
		return NULL;
	swa_lock();
	for (i = 0; i < g_swa.n; i++) {
		SwaExt *e = &g_swa.ext[i];

		if (e->used || e->size < want)
			continue;
		if (e->size > want) {
			if (g_swa.n >= SWA_EXT)
				continue;
			memmove(e + 2, e + 1, (g_swa.n - i - 1) * sizeof(SwaExt));
			e[1].off = e->off + (unsigned)want;
			e[1].size = e->size - (unsigned)want;
			e[1].used = 0;
			e->size = (unsigned)want;
			g_swa.n++;
		}
		raw = (unsigned char *)VirtualAlloc((void *)(g_swa.base + e->off), e->size,
						    MEM_COMMIT, PAGE_READWRITE);
		if (!raw)
			break;
		e->used = 1;
		g_swa.live += e->size;
		if (g_swa.live > g_swa.peak)
			g_swa.peak = g_swa.live;
		break;
	}
	if (!raw)
		g_swa.fails++;
	swa_unlock();
	if (!raw)
		return NULL;
	memset(raw + SWA_HDR, 0, n);
	((void **)(raw + SWA_HDR))[-1] = raw;
	return raw + SWA_HDR;
}

static void swa_release(SwaExt *e)
{
	unsigned i = (unsigned)(e - g_swa.ext);

	e->used = 0;
	g_swa.live -= e->size;
	if (i + 1 < g_swa.n && !e[1].used) {
		e->size += e[1].size;
		memmove(e + 1, e + 2, (g_swa.n - i - 2) * sizeof(SwaExt));
		g_swa.n--;
	}
	if (i > 0 && !e[-1].used) {
		e[-1].size += e->size;
		memmove(e, e + 1, (g_swa.n - i - 1) * sizeof(SwaExt));
		g_swa.n--;
	}
}

/* 1 if p is in the arena, freed or not. The pointer decides, never the word
 * below it: a rewound object can name a block since reused, whose header is
 * someone else's. A pointer that is not the start of a live block is left. */
int gameheap_swa_free(void *p)
{
	size_t off = (uintptr_t)p - g_swa.base;
	SwaExt *e;

	if (!g_swa.base || off >= g_swa.size)
		return 0;
	swa_lock();
	e = swa_find(off);
	if (e && e->used && (size_t)e->off + SWA_HDR == off)
		swa_release(e);
	else
		g_swa.stale++;
	swa_unlock();
	return 1;
}

size_t gameheap_swa_size(void *p)
{
	size_t off = (uintptr_t)p - g_swa.base, n = 0;
	SwaExt *e;

	if (!g_swa.base || off >= g_swa.size)
		return 0;
	swa_lock();
	e = swa_find(off);
	if (e && e->used && (size_t)e->off + SWA_HDR == off)
		n = e->size - SWA_HDR;
	swa_unlock();
	return n;
}

static void swa_scan(uintptr_t lo, uintptr_t hi, unsigned char *mark, const SwaExt *snap,
		     unsigned nsnap)
{
	while (lo < hi) {
		MEMORY_BASIC_INFORMATION mbi;
		uintptr_t end, a;

		if (!VirtualQuery((void *)lo, &mbi, sizeof(mbi)))
			return;
		end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		if (end > hi)
			end = hi;
		if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
		    (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
				    PAGE_EXECUTE_WRITECOPY))) {
			for (a = (lo + 3) & ~(uintptr_t)3; a + 4 <= end; a += 4) {
				size_t off = *(const uintptr_t *)a - g_swa.base;
				unsigned l = 0, h = nsnap;

				if (off >= g_swa.size)
					continue;
				while (l < h) {
					unsigned m = (l + h) / 2;

					if (off < snap[m].off)
						h = m;
					else if (off >= (size_t)snap[m].off + snap[m].size)
						l = m + 1;
					else {
						if (off >= (size_t)snap[m].off + SWA_HDR)
							mark[m] = 1;
						break;
					}
				}
			}
		}
		lo = end;
	}
}

/* After the device reset that follows a load: every arena block nothing names
 * is one a rewound or replaced object held, or one made since the save that the
 * rewound game has no handle to. The objects naming arena blocks live in the
 * game heap (or ours, with D3D9SW_GHSW_OWN) and this image; anything named from
 * elsewhere stays, which only ever errs toward keeping. */
void gameheap_swa_sweep(void)
{
	SwaExt *snap;
	unsigned char *mark;
	unsigned i, nsnap, nfreed = 0;
	size_t freed = 0;
	DWORD t0 = GetTickCount();
	MEMORY_BASIC_INFORMATION mbi;

	if (!g_swa.base)
		return;
	snap = (SwaExt *)VirtualAlloc(NULL, SWA_EXT * (sizeof(SwaExt) + 1),
				      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!snap)
		return;
	mark = (unsigned char *)(snap + SWA_EXT);
	swa_lock();
	nsnap = g_swa.n;
	memcpy(snap, g_swa.ext, nsnap * sizeof(SwaExt));
	swa_unlock();
	swa_scan(g_heap_lo, g_heap_hi, mark, snap, nsnap);
	if (sw_heap() && VirtualQuery(sw_heap(), &mbi, sizeof(mbi))) {
		uintptr_t ab = (uintptr_t)mbi.AllocationBase, e = ab;

		while (VirtualQuery((void *)e, &mbi, sizeof(mbi)) &&
		       (uintptr_t)mbi.AllocationBase == ab)
			e = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		swa_scan(ab, e, mark, snap, nsnap);
	}
	if (g_self) {
		const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)g_self;
		const IMAGE_NT_HEADERS *nt =
			(const IMAGE_NT_HEADERS *)((const char *)g_self + dos->e_lfanew);

		swa_scan((uintptr_t)g_self, (uintptr_t)g_self + nt->OptionalHeader.SizeOfImage,
			 mark, snap, nsnap);
	}
	swa_lock();
	for (i = 0; i < nsnap; i++) {
		SwaExt *e;

		if (!snap[i].used || mark[i])
			continue;
		e = swa_find(snap[i].off);
		if (!e || !e->used || e->off != snap[i].off || e->size != snap[i].size)
			continue;
		nfreed++;
		freed += e->size;
		swa_release(e);
	}
	g_swa.sweeps++;
	g_swa.swept_n += nfreed;
	g_swa.swept += freed;
	swa_unlock();
	VirtualFree(snap, 0, MEM_RELEASE);
	ss_log("swarena: sweep freed %u block(s), %lu MB nothing named; %lu MB live after, %lu ms\n",
	       nfreed, (unsigned long)(freed >> 20), (unsigned long)(g_swa.live >> 20),
	       (unsigned long)(GetTickCount() - t0));
}

static void swa_report(void)
{
	if (!g_swa.base)
		return;
	if (!g_swa.held) {
		savestate_exclude((void *)g_swa.base, g_swa.size);
		savestate_exclude(g_swa.ext, SWA_EXT * sizeof(SwaExt));
		g_swa.held = 1;
	}
	ss_log("swarena: %lu MB live in %u extent(s), peak %lu MB of %lu; %u allocation(s) "
	       "fell back to the span, %u stale free(s); %u sweep(s) freed %u block(s), %lu MB\n",
	       (unsigned long)(g_swa.live >> 20), g_swa.n, (unsigned long)(g_swa.peak >> 20),
	       (unsigned long)(g_swa.size >> 20), g_swa.fails, g_swa.stale, g_swa.sweeps,
	       g_swa.swept_n, (unsigned long)(g_swa.swept >> 20));
}

/* D3D9SW_TEXPACK_BASE, D3D9SW_TEXPACK_MB: an address range taken here, before
 * the game can place anything in it, for the renderer's texture cache. The
 * renderer keeps it out of every save. */
static uintptr_t g_tpk_base SS_PRESENT;
static size_t g_tpk_size SS_PRESENT;

static void tpk_reserve(void)
{
	uintptr_t base = gh_knob("D3D9SW_TEXPACK_BASE", 0);
	unsigned mb = gh_knob("D3D9SW_TEXPACK_MB", 500);

	if (!base || !mb)
		return;
	if (VirtualAlloc((void *)base, (SIZE_T)mb << 20, MEM_RESERVE, PAGE_READWRITE) !=
	    (void *)base) {
		ss_log("texpack: could not reserve %u MB at %08lX (error %lu) - managed textures "
		       "stay in the big-block span\n",
		       mb, (unsigned long)base, GetLastError());
		return;
	}
	g_tpk_base = base;
	g_tpk_size = (size_t)mb << 20;
	ss_log("texpack: %u MB reserved at %08lX for the texture cache\n", mb,
	       (unsigned long)base);
}

uintptr_t gameheap_texpack_region(size_t *size)
{
	*size = g_tpk_size;
	return g_tpk_base;
}

void gameheap_merge_chunks(void)
{
	GhBigArena *old[GH_BIG_CHUNKS];
	LONG n = 0, was = g_nbig, i;

	if (!g_big_chunk || !g_bigpin_base)
		return;
	memcpy(old, g_bigch, sizeof(old));
	g_bigpin_used = big_walk(g_bigpin_base, g_bigpin_size, &n);
	g_bigpin2_used = big_walk(g_bigpin2_base, g_bigpin2_size, &n);
	/* Chunks Windows placed were not in the spans and were not taken. */
	for (i = 0; i < was && n < GH_BIG_CHUNKS; i++) {
		uintptr_t a = (uintptr_t)old[i];

		if (!(a >= g_bigpin_base && a < g_bigpin_base + g_bigpin_size) &&
		    !(g_bigpin2_base && a >= g_bigpin2_base && a < g_bigpin2_base + g_bigpin2_size))
			g_bigch[n++] = old[i];
	}
	InterlockedExchange(&g_nbig, n);
	ss_log("  merge: big-block chunks read back from the save: %ld (this launch had %ld), "
	       "%lu KB and %lu KB of the pinned spans carved\n",
	       (long)n, (long)was, (unsigned long)(g_bigpin_used >> 10),
	       (unsigned long)(g_bigpin2_used >> 10));
}

int gameheap_import_mode(void)
{
	return g_ready && g_imports;
}

/* MSVCR100's own heap when import mode redirected MSVCR100, else NULL. Unlike
 * ucrtbase it is the game's alone, and it still holds what the game allocated
 * before the redirect went in. */
HANDLE gameheap_rt_heap(void)
{
	HMODULE m;
	intptr_t(__cdecl * get)(void);

	if (!g_ready || !g_imports || lstrcmpA(g_rt_name, "MSVCR100"))
		return NULL;
	m = GetModuleHandleA("MSVCR100.dll");
	get = m ? (intptr_t(__cdecl *)(void))(void *)GetProcAddress(m, "_get_heap_handle") : NULL;
	return get ? (HANDLE)get() : NULL;
}

/* The wrapper's own allocator (sw_malloc), served from the private heap in
 * import mode. Its usual home is a growable HeapCreate heap, and a growable heap
 * turns the low-fragmentation front end on, whose bookkeeping lives outside the
 * region we rewind - the restore that first survived Haydee's audio died six
 * frames later inside RtlAllocateHeap on exactly that heap. Same layout as
 * sw_malloc (the issued block one word below the aligned address), and NULL
 * rather than a runtime fallback when full, so the caller can tell whose block
 * it holds from the block alone.
 *
 * D3D9SW_GHSW_OWN=1 declines the small ones, which then go to sw_heap - pinned
 * by D3D9SW_GHHEAPS, so not growable. That keeps the small arena the game's
 * alone, which D3D9SW_MERGE needs to take it whole. */
static int g_sw_own;

void *gameheap_sw_alloc(size_t n, size_t a, const void *caller)
{
	size_t want = n + a + sizeof(void *);
	unsigned site = (unsigned)(uintptr_t)caller; /* our image never moves */
	void *raw;
	uintptr_t p;

	if (!g_ready || !g_imports || want < n)
		return NULL;
	if (g_sw_own && want < GH_BIG)
		return NULL;
	raw = want >= GH_BIG ? big_alloc(want + sizeof(GhHead))
			     : gh_halloc(g_heap, 0, want + sizeof(GhHead), site);
	if (!raw) {
		g_fellback++;
		return NULL;
	}
	if (want >= GH_BIG)
		((GhBigBlk *)raw - 1)->owner = GH_BIG_SW;
	raw = give(raw, want, site);
	p = ((uintptr_t)raw + sizeof(void *) + a - 1) & ~(uintptr_t)(a - 1);
	((void **)p)[-1] = raw;
	return (void *)p;
}

/* Takes the issued block (the word below what sw_malloc returned). An orphan -
 * inside our reservations, header gone, e.g. a block a restore rewound under a
 * renderer object - is claimed too, so gh_free drops it instead of sw_free
 * handing it to a heap that never issued it. */
int gameheap_sw_owns(void *raw)
{
	int orphan;

	return g_ready && g_imports && (ours_why(raw, &orphan) != NULL || orphan);
}

void gameheap_sw_free(void *raw)
{
	gh_free(raw);
}

size_t gameheap_sw_size(void *raw)
{
	int orphan;
	GhHead *h = ours_why(raw, &orphan);

	return h ? h->size : 0;
}

int gameheap_install_imports(void)
{
	char v[8];
	DWORD n = savestate_getenv("D3D9SW_GAMEHEAP", v, sizeof(v));
	HMODULE ucrt = GetModuleHandleA("ucrtbase.dll");
	HMODULE nt = GetModuleHandleA("ntdll.dll");
	LONG(NTAPI * reg)(ULONG, PVOID, PVOID, PVOID *);
	unsigned big_mb = gh_knob("D3D9SW_GHBIG_MB", 64);
	void *cookie = NULL;
	int floor = 0, i;
	WCHAR *slash = NULL;

	if (g_ready || n == 0 || n >= sizeof(v) || v[0] != '2')
		return 0;
	g_early_on = 1;
	g_imports = 1;
	g_sw_own = gh_knob("D3D9SW_GHSW_OWN", 0);
	g_exe = (uintptr_t)GetModuleHandleA(NULL);
	gh_trace_arm();
	if (gh_exe_imports("MSVCR100.dll") && GetModuleHandleA("MSVCR100.dll")) {
		ucrt = GetModuleHandleA("MSVCR100.dll");
		kImpDlls[0] = kImpDlls[1] = "MSVCR100.dll";
		g_rt_name = "MSVCR100";
	}
	if (!ucrt) {
		ss_log("gameheap: import mode asked for, but ucrtbase is not loaded - "
		       "this game does not take its runtime from a DLL\n");
		return 0;
	}
	r_malloc = (PFN_malloc)(void *)GetProcAddress(ucrt, "malloc");
	r_calloc = (PFN_calloc)(void *)GetProcAddress(ucrt, "calloc");
	r_realloc = (PFN_realloc)(void *)GetProcAddress(ucrt, "realloc");
	r_free = (PFN_free)(void *)GetProcAddress(ucrt, "free");
	r_msize = (PFN_msize)(void *)GetProcAddress(ucrt, "_msize");
	r_recalloc = (PFN_recalloc)(void *)GetProcAddress(ucrt, "_recalloc");
	r_expand = (PFN_expand)(void *)GetProcAddress(ucrt, "_expand");
	if (!r_malloc || !r_calloc || !r_realloc || !r_free || !r_msize || !r_recalloc ||
	    !r_expand) {
		ss_log("gameheap: ucrtbase is missing an allocator export - nothing "
		       "touched\n");
		return 0;
	}
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCSTR)(void *)gameheap_install_imports, &g_self);
	g_game_dir_n = (int)GetModuleFileNameW(NULL, g_game_dir, MAX_PATH);
	for (i = 0; i < g_game_dir_n; i++)
		if (g_game_dir[i] == '\\' || g_game_dir[i] == '/')
			slash = g_game_dir + i;
	g_game_dir_n = slash ? (int)(slash - g_game_dir) + 1 : 0;

	g_heap = gh_create_heap();
	if (!g_heap || !g_heap_hi) {
		ss_log("gameheap: import mode needs a heap at a reserved base, and did "
		       "not get one - nothing redirected. A growable heap would bring the "
		       "low-fragmentation bookkeeping back outside the region\n");
		return 0;
	}
	InitializeCriticalSection(&g_cs);
	savestate_own_cs(&g_cs);
	gh_add_range(g_heap_lo, g_heap_hi);
	gh_gamedll_pin();
	if (big_mb) {
		g_big_chunk = (SIZE_T)big_mb << 20;
		bigpin_reserve();
		swa_reserve();
		tpk_reserve();
		/* Below our own image at 0x60000000: the game holds ~355 MB of these by
		 * the title screen, more than fits above the big-block span. */
		raw_reserve(0x40000000u, GH_HOMES);
		if (chunk_add(0))
			ss_log("gameheap: big blocks (%u KB and up) in %u MB chunks, first "
			       "at %p\n",
			       GH_BIG >> 10, big_mb, (void *)g_bigch[0]);
		else
			ss_log("gameheap: could not reserve a %u MB big-block chunk (error "
			       "%lu) - will retry on the first big block\n",
			       big_mb, GetLastError());
	}

	floor += gh_iat_swap(ucrt, NULL, "HeapFree", (void *)gh_heapfree,
			     (void **)&r_heapfree) > 0;
	floor += gh_iat_swap(ucrt, NULL, "HeapReAlloc", (void *)gh_crt_heaprealloc,
			     (void **)&r_crt_hra) > 0;
	floor += gh_iat_swap(ucrt, NULL, "HeapSize", (void *)gh_crt_heapsize,
			     (void **)&r_crt_hsz) > 0;
	if (floor != 3) {
		ss_log("gameheap: only %d of %s's 3 heap imports could be "
		       "patched - without the floor a block of ours freed inside the "
		       "runtime would reach the process heap, so nothing is "
		       "redirected\n",
		       floor, g_rt_name);
		return 0;
	}
	g_ready = 1;
	savestate_game_heap(g_heap);
	{
		HMODULE k32 = GetModuleHandleA("kernel32.dll");

		r_va = k32 ? (LPVOID(WINAPI *)(LPVOID, SIZE_T, DWORD, DWORD))(void *)
				     GetProcAddress(k32, "VirtualAlloc")
			   : NULL;
		r_vf = k32 ? (BOOL(WINAPI *)(LPVOID, SIZE_T, DWORD))(void *)GetProcAddress(
				     k32, "VirtualFree")
			   : NULL;
		if (r_va && r_vf) {
			g_vaj = (GhVaJ *)gh_home_alloc(GH_HOME_VAJ, sizeof(GhVaJ));
			if (gh_knob("D3D9SW_GHRES", 1))
				g_res = (GhRes *)gh_home_alloc(GH_HOME_RES, sizeof(GhRes));
		}
	}
	if (gh_knob("D3D9SW_NTLOG", 0)) {
		g_ntlog = 1;
		nt_hooks_install(0);
	}
	if (gh_knob("D3D9SW_GHRT_ALLOC", 0) && (g_rt_crtheap = gameheap_rt_heap()) != NULL) {
		int n = gh_iat_swap(ucrt, NULL, "HeapAlloc", (void *)gh_rt_heapalloc,
				    (void **)&r_rt_halloc);

		ss_log("gameheap: %s's own HeapAlloc on its heap %p %s\n", g_rt_name,
		       (void *)g_rt_crtheap,
		       n > 0 ? "now served from the private heap" : "could NOT be patched");
		if (n <= 0)
			g_rt_crtheap = NULL;
	}
	gh_heapcreate_arm();
	ct_arm();
	gh_patch_loaded();
	reg = nt ? (LONG(NTAPI *)(ULONG, PVOID, PVOID, PVOID *))(void *)GetProcAddress(
			   nt, "LdrRegisterDllNotification")
		 : NULL;
	ss_log("gameheap: IMPORT MODE - %lu module(s), %lu import slot(s) on a private "
	       "heap at %p (%u MB); %s's HeapFree/HeapReAlloc/HeapSize are the "
	       "floor; later loads %s\n",
	       g_mods_patched, g_slots_patched, (void *)g_heap, g_pin_mb, g_rt_name,
	       (reg && reg(0, (PVOID)gh_dll_loaded, NULL, &cookie) >= 0)
		       ? "patched as they arrive"
		       : "NOT WATCHED - LdrRegisterDllNotification failed");
	return (int)g_slots_patched;
}

/* ------------------------------------------------- where the heap itself goes
 *
 * D3D9SW_GHPIN puts the private heap at an address we choose instead of one
 * Windows chooses.
 *
 * This is the change that makes a layout reproducible, and the evidence for it
 * is unusually direct. Two sessions were made to issue byte-identical
 * allocation sequences - same 2,133 operations, same order - and replayed
 * against four allocators. HeapCreate put 93% of the blocks at different
 * offsets both times. Disabling the low-fragmentation heap changed nothing,
 * which killed the obvious theory. RtlCreateHeap at a fixed base reproduced
 * every block, at every checkpoint, exactly.
 *
 * The only difference between those cases is the base, so the base is the
 * cause: HeapCreate has no address parameter, Windows picks one, and the
 * internal layout follows from it.
 *
 * Worth being clear about why an address matters when a snapshot could simply
 * record offsets. It is not the snapshot that needs the address, it is the
 * contents: the blocks are full of pointers to each other, and a pointer is
 * only meaningful against the layout it was written for. Restoring the bytes
 * faithfully into a heap that moved is how a restored pointer ends up aimed at
 * whatever now occupies that address.
 *
 * A caller-supplied base means the heap cannot grow - there is nothing after
 * the reservation it is entitled to take. That is deliberate. Peak live in this
 * heap has been measured at 0.47 MB and everything at or above 256 KB is handed
 * to the runtime anyway, so the reservation below is orders of magnitude clear;
 * and if it is ever wrong, gh_malloc already falls back to the runtime and
 * g_fellback counts it, so the failure is a report rather than a crash.
 */
/* 0x20000000, not 0x50000000: the startup probe (d3d11_sw arena_probe) measured
 * the largest contiguous free hole at each candidate base, and 0x20000000 has by
 * far the most room (~1.15 GB early vs ~0.4 GB at 0x50000000). At 128 MB either
 * base takes, but 0x50000000 could not fit a larger arena - the 512 MB attempt
 * there failed with error 487 and fell back to an OS-placed base, which is NOT
 * the same address next session and so breaks cross-session pointers. Pinning at
 * 0x20000000 keeps the fixed base for any arena size we are likely to want,
 * meaning the arena stays DETERMINISTIC across sessions and drops out of the
 * relocation problem entirely. The d3d11 texture arena is steered off this base
 * so the two do not fight for it. */
#define GH_PIN_BASE 0x20000000u

static PVOID(NTAPI *p_RtlCreateHeap)(ULONG, PVOID, SIZE_T, SIZE_T, PVOID, PVOID);

static unsigned gh_knob(const char *name, unsigned def)
{
	char v[24];
	unsigned n = savestate_getenv(name, v, sizeof(v));
	unsigned got = 0, k = 0;
	int hex = (n > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X'));

	if (!n)
		return def;
	for (k = hex ? 2 : 0; k < n; k++) {
		if (v[k] >= '0' && v[k] <= '9')
			got = got * (hex ? 16 : 10) + (unsigned)(v[k] - '0');
		else if (hex && v[k] >= 'a' && v[k] <= 'f')
			got = got * 16 + (unsigned)(v[k] - 'a') + 10;
		else if (hex && v[k] >= 'A' && v[k] <= 'F')
			got = got * 16 + (unsigned)(v[k] - 'A') + 10;
		else
			break;
	}
	return got;
}

static HANDLE gh_create_heap(void)
{
	/* Import mode pins by default: a heap at a caller-chosen base never turns
	 * on the low-fragmentation front end (measured: still mode 0 after 20000
	 * same-size blocks, where a HeapCreate heap switches to 2), so all of its
	 * bookkeeping stays inside the region a restore copies. */
	unsigned want = gh_knob("D3D9SW_GHPIN", g_imports ? 1 : 0);
	unsigned mb = gh_knob("D3D9SW_GHPIN_MB", g_imports ? 256 : 64);
	uintptr_t base;
	SIZE_T size;
	void *res;
	HMODULE nt;
	HANDLE h;

	g_pin_mb = 0; /* set below only if the pin actually succeeds */
	if (!want)
		return HeapCreate(0, 1u << 20, 0);
	/* 1 means "pin it, you pick"; anything larger is an address. */
	base = (want == 1) ? GH_PIN_BASE : (uintptr_t)want;
	size = (SIZE_T)mb * 1024u * 1024u;

	nt = GetModuleHandleA("ntdll.dll");
	p_RtlCreateHeap = nt ? (PVOID(NTAPI *)(ULONG, PVOID, SIZE_T, SIZE_T, PVOID,
					       PVOID))GetProcAddress(nt, "RtlCreateHeap")
			     : NULL;
	if (!p_RtlCreateHeap) {
		ss_log("gameheap: RtlCreateHeap is not exported, so the heap cannot "
		       "be pinned - falling back to HeapCreate and a layout that "
		       "will not reproduce\n");
		return HeapCreate(0, 1u << 20, 0);
	}
	/* Reserved first and checked, rather than trusting RtlCreateHeap to honour
	 * the address: landing somewhere else silently would look like success and
	 * produce a session whose traces quietly do not compare. */
	res = VirtualAlloc((LPVOID)base, size, MEM_RESERVE, PAGE_READWRITE);
	if (!res || (uintptr_t)res != base) {
		DWORD err = GetLastError();

		if (res) {
			VirtualFree(res, 0, MEM_RELEASE);
			res = NULL;
		}
		/* The fixed base is routinely occupied - 0x50000000 loses to whatever ASLR
		 * or a module put there - and the old answer was a growable HeapCreate,
		 * which cannot be pinned OR wholesale-restored (its segments come and go
		 * outside any bounded region). An OS-placed reservation of the SAME size is
		 * just as ownable and just as wholesale-able; it only gives up the fixed
		 * base, which determinism wants but survival does not. So prefer it, and
		 * keep HeapCreate as the last resort only. This is what lets D3D9SW_WHOLESALE
		 * engage without D3D9SW_LAA_FALSE, since the arena is now always bounded. */
		res = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE);
		if (!res) {
			ss_log("gameheap: cannot reserve %u MB at %08lX (error %lu) nor "
			       "anywhere else (error %lu) - falling back to a growable "
			       "HeapCreate that cannot be wholesale-restored\n",
			       mb, (unsigned long)base, err, GetLastError());
			return HeapCreate(0, 1u << 20, 0);
		}
		ss_log("gameheap: could not pin %u MB at %08lX (error %lu); reserved it "
		       "OS-placed at %08lX instead - bounded and wholesale-able, just not "
		       "at the fixed base (determinism traces will not compare)\n",
		       mb, (unsigned long)base, err, (unsigned long)(uintptr_t)res);
	}
	if (g_imports && gh_knob("D3D9SW_GHOWN", 1)) {
		if (!gho_init(res, size)) {
			ss_log("gameheap: cannot commit the first page at %p (error %lu) - "
			       "falling back to HeapCreate\n",
			       res, GetLastError());
			VirtualFree(res, 0, MEM_RELEASE);
			return HeapCreate(0, 1u << 20, 0);
		}
		ss_log("gameheap: heap PINNED at %p, %u MB, served by our own allocator - "
		       "no Windows heap state inside the span (D3D9SW_GHOWN=0 for "
		       "RtlCreateHeap)\n",
		       res, mb);
		g_pin_mb = mb;
		g_heap_lo = (uintptr_t)res;
		g_heap_hi = (uintptr_t)res + size;
		gl_init();
		gp_init();
		return (HANDLE)res;
	}
	h = (HANDLE)p_RtlCreateHeap(0, res, size, 1u << 20, NULL, NULL);
	if (!h) {
		ss_log("gameheap: RtlCreateHeap refused the base %08lX - falling back "
		       "to HeapCreate\n",
		       (unsigned long)base);
		VirtualFree(res, 0, MEM_RELEASE);
		return HeapCreate(0, 1u << 20, 0);
	}
	ss_log("gameheap: heap PINNED at %p, %u MB reserved, not growable. Two "
	       "sessions that issue the same allocations should now place every "
	       "block at the same address, not merely at the same set of "
	       "addresses\n",
	       (void *)h, mb);
	if ((uintptr_t)h != base)
		ss_log("gameheap: NOTE the handle %p is not the base %08lX we asked "
		       "for - offsets are measured from the handle\n",
		       (void *)h, (unsigned long)base);
	g_pin_mb = mb; /* the pin held; report the real, pinned size */
	g_heap_lo = (uintptr_t)h;
	g_heap_hi = (uintptr_t)h + size;
	return h;
}

/* D3D9SW_GHHEAPS=<address>: Windows heaps created by the game, its runtime or
 * this DLL are put in fixed slots from that address, in creation order, instead
 * of wherever HeapCreate puts them. A rewound heap that moves between launches
 * is a region a cross-session load cannot restore. Each slot is
 * D3D9SW_GHHEAPS_MB (default 8) and the heap cannot grow past it, which also
 * keeps the low-fragmentation front end off. Every creation is logged with its
 * caller, pinned or not. */
#define GH_HP_SLOTS 6
static HANDLE(WINAPI *r_heapcreate)(DWORD, SIZE_T, SIZE_T);
static uintptr_t g_hp_base;
static SIZE_T g_hp_slot;
static LONG g_hp_n;
/* D3D9SW_GHHEAPS_DXC=1: d3dcompiler_43's heaps in the top two slots. */
#define GH_HP_DXC (2u << 20)
static int g_hp_dxc;
static LONG g_hp_dxc_n;
#define GH_HP_LOW (GH_HP_SLOTS - (g_hp_dxc ? 2 : 0))

/* A heap in the D3D9SW_GHHEAPS slots was made there by us for the game, the
 * runtime, D3DX or the compiler, whatever module holds the most pointers into it. */
int gameheap_slot_heap(uintptr_t h)
{
	return g_hp_base && g_hp_slot && h >= g_hp_base &&
	       h < g_hp_base + (uintptr_t)GH_HP_SLOTS * g_hp_slot;
}

static HANDLE WINAPI gh_heapcreate(DWORD opts, SIZE_T init, SIZE_T max)
{
	void *ret = __builtin_return_address(0);
	HMODULE from = NULL;
	WCHAR path[MAX_PATH];
	const WCHAR *leaf;
	HANDLE h = NULL;
	int pin, i;

	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCWSTR)ret, &from);
	path[0] = 0;
	if (from)
		GetModuleFileNameW(from, path, MAX_PATH);
	leaf = path;
	for (i = 0; path[i]; i++)
		if (path[i] == '\\' || path[i] == '/')
			leaf = path + i + 1;
	/* D3D9SW_GHHEAPS_DXC=1: the shader compiler's heaps too. D3DX's effects keep
	 * their state there, so a save that takes the game's objects without them
	 * has the game holding effects whose insides are this launch's. */
	pin = g_hp_base && from &&
	      (gh_is_game_module(from, path) || from == GetModuleHandleA("MSVCR100.dll"));
	/* Its heaps are small and made in their own order, so they get the top two
	 * slots cut in 2 MB pieces and the slots below keep the order they had. */
	if (g_hp_base && from && g_hp_dxc && from == GetModuleHandleA("d3dcompiler_43.dll") &&
	    p_RtlCreateHeap) {
		LONG k = InterlockedIncrement(&g_hp_dxc_n) - 1;
		uintptr_t at = g_hp_base + (GH_HP_SLOTS - 2) * g_hp_slot + (uintptr_t)k * GH_HP_DXC;

		if (k < (LONG)(2 * g_hp_slot / GH_HP_DXC))
			h = (HANDLE)p_RtlCreateHeap(
				opts & (HEAP_NO_SERIALIZE | HEAP_GENERATE_EXCEPTIONS |
					HEAP_CREATE_ENABLE_EXECUTE),
				(void *)at, GH_HP_DXC, 0x10000, NULL, NULL);
		ss_log("gameheap: HeapCreate from %ls (%p) - %s compiler slot %ld at %08lX%s\n", leaf,
		       ret, h ? "PINNED in" : "could not pin in", (long)k, (unsigned long)at,
		       h ? "" : ", left to Windows");
		if (h)
			return h;
		pin = 1;
	} else if (pin && p_RtlCreateHeap) {
		LONG k = InterlockedIncrement(&g_hp_n) - 1;
		uintptr_t at = g_hp_base + (uintptr_t)k * g_hp_slot;
		SIZE_T commit = (init + 0xFFFu) & ~(SIZE_T)0xFFFu;

		if (commit < 0x10000u)
			commit = 0x10000u;
		if (commit > g_hp_slot)
			commit = g_hp_slot;
		if (k < GH_HP_LOW)
			h = (HANDLE)p_RtlCreateHeap(
				opts & (HEAP_NO_SERIALIZE | HEAP_GENERATE_EXCEPTIONS |
					HEAP_CREATE_ENABLE_EXECUTE),
				(void *)at, g_hp_slot, commit, NULL, NULL);
		ss_log("gameheap: HeapCreate from %ls (%p) - %s slot %ld at %08lX%s\n", leaf,
		       ret, h ? "PINNED in" : "could not pin in", (long)k,
		       (unsigned long)at, h ? "" : ", left to Windows");
	}
	if (!h) {
		h = r_heapcreate(opts, init, max);
		if (!pin)
			ss_log("gameheap: HeapCreate from %ls (%p) at %p, not pinned\n",
			       path[0] ? leaf : L"?", ret, (void *)h);
	}
	return h;
}

static void gh_heapcreate_hook(HMODULE mod)
{
	if (g_hp_base && mod)
		gh_iat_swap(mod, NULL, "HeapCreate", (void *)gh_heapcreate,
			    (void **)&r_heapcreate);
}

static void gh_heapcreate_arm(void)
{
	HMODULE nt = GetModuleHandleA("ntdll.dll");

	static const char *const rts[] = { "msvcrt.dll", "MSVCR100.dll", "MSVCP100.dll" };
	unsigned r;

	for (r = 0; r < sizeof(rts) / sizeof(rts[0]); r++) {
		HMODULE m = GetModuleHandleA(rts[r]);
		intptr_t(__cdecl * get)(void) =
			m ? (intptr_t(__cdecl *)(void))(void *)GetProcAddress(m, "_get_heap_handle")
			  : NULL;

		ss_log("gameheap: before us, %s's heap is %p (process heap %p)\n", rts[r],
		       get ? (void *)get() : NULL, (void *)GetProcessHeap());
	}
	g_hp_base = gh_knob("D3D9SW_GHHEAPS", 0);
	g_hp_slot = (SIZE_T)gh_knob("D3D9SW_GHHEAPS_MB", 8) << 20;
	g_hp_dxc = gh_knob("D3D9SW_GHHEAPS_DXC", 0) != 0;
	if (!p_RtlCreateHeap && nt)
		p_RtlCreateHeap = (PVOID(NTAPI *)(ULONG, PVOID, SIZE_T, SIZE_T, PVOID,
						  PVOID))GetProcAddress(nt, "RtlCreateHeap");
	if (!g_hp_base || !g_hp_slot) {
		g_hp_base = 0;
		return;
	}
	/* Reserved whole and now, before anything else in this DLL allocates, so a
	 * heap created late (ours is made at the first save) still finds its slot. */
	{
		int k;

		for (k = 0; k < GH_HP_SLOTS; k++) {
			void *at = (void *)(g_hp_base + (uintptr_t)k * g_hp_slot);

			if (VirtualAlloc(at, g_hp_slot, MEM_RESERVE, PAGE_READWRITE) != at) {
				ss_log("gameheap: cannot reserve heap slot %d at %p (error "
				       "%lu) - heaps are left to Windows\n",
				       k, at, GetLastError());
				while (k-- > 0)
					VirtualFree((void *)(g_hp_base + (uintptr_t)k * g_hp_slot), 0,
						    MEM_RELEASE);
				g_hp_base = 0;
				return;
			}
		}
	}
	ss_log("gameheap: heaps from the game, its runtime and this DLL pinned in "
	       "%d slot(s) of %lu MB from %08lX\n",
	       GH_HP_SLOTS, (unsigned long)(g_hp_slot >> 20), (unsigned long)g_hp_base);
}

/* D3D9SW_CRTTAP: the msvcrt allocator imports of the Microsoft DLLs the game
 * calls - D3DX, its shader compiler, XAudio2 2.7. msvcrt's heap is made before
 * this DLL loads and lands somewhere else every launch. 1 counts every call per
 * module and passes it through; 2 also serves them from a heap at
 * D3D9SW_CRTTAP_AT (D3D9SW_CRTTAP_MB, default 32), and a block it did not make
 * goes back to msvcrt. The tally is logged at every save. */
enum { CT_MALLOC, CT_CALLOC, CT_REALLOC, CT_FREE, CT_STRDUP, CT_NEW, CT_DELETE, CT_AMALLOC,
       CT_AFREE, CT_NFN };
static const char *const kCtFn[CT_NFN] = { "malloc",	   "calloc",	     "realloc",
					   "free",	   "_strdup",	     "??2@YAPAXI@Z",
					   "??3@YAXPAX@Z", "_aligned_malloc", "_aligned_free" };
static const char *const kCtMod[] = { "d3dx9_43.dll", "d3dcompiler_43.dll", "XAudio2_7.dll" };
#define CT_NMOD 3

typedef struct {
	uintptr_t lo, hi;
	volatile LONG calls[CT_NFN];
	volatile LONG live, bytes, foreign, spill;
} CtMod;

static CtMod g_ct[CT_NMOD + 1]; /* the last row is any other caller */
static int g_ct_mode;
static HANDLE g_ct_heap SS_PRESENT;
static uintptr_t g_ct_lo, g_ct_hi;
static void *(__cdecl *rc_malloc)(size_t);
static void *(__cdecl *rc_calloc)(size_t, size_t);
static void *(__cdecl *rc_realloc)(void *, size_t);
static void(__cdecl *rc_free)(void *);
static size_t(__cdecl *rc_msize)(void *);
static void *(__cdecl *rc_new)(size_t);
static void *(__cdecl *rc_amalloc)(size_t, size_t);
static void(__cdecl *rc_afree)(void *);

static CtMod *ct_mod(void *ra)
{
	int i;

	for (i = 0; i < CT_NMOD; i++)
		if ((uintptr_t)ra >= g_ct[i].lo && (uintptr_t)ra < g_ct[i].hi)
			return &g_ct[i];
	return &g_ct[CT_NMOD];
}

static int ct_ours(const void *p)
{
	return (uintptr_t)p >= g_ct_lo && (uintptr_t)p < g_ct_hi;
}

static void *ct_alloc(CtMod *m, size_t n, int zero)
{
	void *p;

	if (g_ct_heap) {
		p = HeapAlloc(g_ct_heap, zero ? HEAP_ZERO_MEMORY : 0, n ? n : 1);
		if (p) {
			InterlockedIncrement(&m->live);
			InterlockedExchangeAdd(&m->bytes, (LONG)HeapSize(g_ct_heap, 0, p));
			return p;
		}
		InterlockedIncrement(&m->spill);
	}
	p = zero ? rc_calloc(1, n) : rc_malloc(n);
	if (p) {
		InterlockedIncrement(&m->live);
		InterlockedExchangeAdd(&m->bytes, (LONG)rc_msize(p));
	}
	return p;
}

static void ct_release(CtMod *m, void *p)
{
	if (!p)
		return;
	InterlockedDecrement(&m->live);
	if (ct_ours(p)) {
		InterlockedExchangeAdd(&m->bytes, -(LONG)HeapSize(g_ct_heap, 0, p));
		HeapFree(g_ct_heap, 0, p);
		return;
	}
	if (g_ct_heap)
		InterlockedIncrement(&m->foreign);
	InterlockedExchangeAdd(&m->bytes, -(LONG)rc_msize(p));
	rc_free(p);
}

#define CT_ENTER(fn)                                                \
	CtMod *m = ct_mod(__builtin_return_address(0)); \
	InterlockedIncrement(&m->calls[fn])

static void *__cdecl ct_malloc(size_t n)
{
	CT_ENTER(CT_MALLOC);
	return ct_alloc(m, n, 0);
}

static void *__cdecl ct_calloc(size_t c, size_t s)
{
	CT_ENTER(CT_CALLOC);
	if (s && c > (size_t)-1 / s)
		return NULL;
	return ct_alloc(m, c * s, 1);
}

static void *__cdecl ct_new(size_t n)
{
	void *p;
	CT_ENTER(CT_NEW);
	p = ct_alloc(m, n, 0);
	return p ? p : rc_new(n);
}

static void __cdecl ct_free(void *p)
{
	CT_ENTER(CT_FREE);
	ct_release(m, p);
}

static void __cdecl ct_delete(void *p)
{
	CT_ENTER(CT_DELETE);
	ct_release(m, p);
}

static char *__cdecl ct_strdup(const char *s)
{
	size_t n;
	char *p;
	CT_ENTER(CT_STRDUP);
	if (!s)
		return NULL;
	n = (size_t)lstrlenA(s) + 1;
	p = (char *)ct_alloc(m, n, 0);
	if (p)
		memcpy(p, s, n);
	return p;
}

static void *__cdecl ct_realloc(void *p, size_t n)
{
	size_t o;
	void *q;
	CT_ENTER(CT_REALLOC);
	if (!p)
		return ct_alloc(m, n, 0);
	if (!n) {
		ct_release(m, p);
		return NULL;
	}
	if (ct_ours(p)) {
		o = HeapSize(g_ct_heap, 0, p);
		q = HeapReAlloc(g_ct_heap, 0, p, n);
		if (q) {
			InterlockedExchangeAdd(&m->bytes, (LONG)HeapSize(g_ct_heap, 0, q) - (LONG)o);
			return q;
		}
	} else if (g_ct_heap) {
		o = rc_msize(p);
	} else {
		o = rc_msize(p);
		q = rc_realloc(p, n);
		if (q)
			InterlockedExchangeAdd(&m->bytes, (LONG)rc_msize(q) - (LONG)o);
		return q;
	}
	q = ct_alloc(m, n, 0);
	if (!q)
		return NULL;
	memcpy(q, p, o < n ? o : n);
	ct_release(m, p);
	return q;
}

static void *__cdecl ct_amalloc(size_t n, size_t a)
{
	void *p;
	CT_ENTER(CT_AMALLOC);
	if (g_ct_heap && a && !(a & (a - 1))) {
		void *raw;

		if (a < sizeof(void *))
			a = sizeof(void *);
		raw = HeapAlloc(g_ct_heap, 0, n + a + sizeof(void *));
		if (raw) {
			uintptr_t u = ((uintptr_t)raw + sizeof(void *) + a - 1) & ~(uintptr_t)(a - 1);

			((void **)u)[-1] = raw;
			InterlockedIncrement(&m->live);
			InterlockedExchangeAdd(&m->bytes, (LONG)HeapSize(g_ct_heap, 0, raw));
			return (void *)u;
		}
		InterlockedIncrement(&m->spill);
	}
	p = rc_amalloc(n, a);
	if (p)
		InterlockedIncrement(&m->live);
	return p;
}

static void __cdecl ct_afree(void *p)
{
	CT_ENTER(CT_AFREE);
	if (!p)
		return;
	InterlockedDecrement(&m->live);
	if (ct_ours(p)) {
		void *raw = ((void **)p)[-1];

		InterlockedExchangeAdd(&m->bytes, -(LONG)HeapSize(g_ct_heap, 0, raw));
		HeapFree(g_ct_heap, 0, raw);
		return;
	}
	if (g_ct_heap)
		InterlockedIncrement(&m->foreign);
	rc_afree(p);
}

static void ct_arm(void)
{
	HMODULE crt = GetModuleHandleA("msvcrt.dll");
	uintptr_t at;
	SIZE_T size;

	g_ct_mode = (int)gh_knob("D3D9SW_CRTTAP", 0);
	if (!g_ct_mode)
		return;
	if (crt) {
		rc_malloc = (void *(__cdecl *)(size_t))(void *)GetProcAddress(crt, "malloc");
		rc_calloc = (void *(__cdecl *)(size_t, size_t))(void *)GetProcAddress(crt, "calloc");
		rc_realloc = (void *(__cdecl *)(void *, size_t))(void *)GetProcAddress(crt, "realloc");
		rc_free = (void(__cdecl *)(void *))(void *)GetProcAddress(crt, "free");
		rc_msize = (size_t(__cdecl *)(void *))(void *)GetProcAddress(crt, "_msize");
		rc_new = (void *(__cdecl *)(size_t))(void *)GetProcAddress(crt, "??2@YAPAXI@Z");
		rc_amalloc = (void *(__cdecl *)(size_t, size_t))(void *)GetProcAddress(
			crt, "_aligned_malloc");
		rc_afree = (void(__cdecl *)(void *))(void *)GetProcAddress(crt, "_aligned_free");
	}
	if (!rc_malloc || !rc_calloc || !rc_realloc || !rc_free || !rc_msize || !rc_new ||
	    !rc_amalloc || !rc_afree) {
		ss_log("gameheap: crttap off - msvcrt.dll or one of its allocators is missing\n");
		g_ct_mode = 0;
		return;
	}
	at = gh_knob("D3D9SW_CRTTAP_AT", 0);
	size = (SIZE_T)gh_knob("D3D9SW_CRTTAP_MB", 32) << 20;
	if (g_ct_mode >= 2 && p_RtlCreateHeap) {
		DWORD err = 0;

		/* Without an address of its own it takes the next D3D9SW_GHHEAPS
		 * slot, already reserved and in the same order every launch. */
		if (!at && g_hp_base && g_hp_n < GH_HP_LOW) {
			LONG k = InterlockedIncrement(&g_hp_n) - 1;

			at = g_hp_base + (uintptr_t)k * g_hp_slot;
			size = g_hp_slot;
			g_ct_heap = (HANDLE)p_RtlCreateHeap(0, (void *)at, size, 0x10000, NULL,
							    NULL);
			if (!g_ct_heap)
				err = GetLastError();
		} else if (at) {
			if (VirtualAlloc((void *)at, size, MEM_RESERVE, PAGE_READWRITE) == (void *)at) {
				g_ct_heap = (HANDLE)p_RtlCreateHeap(0, (void *)at, size, 0x10000, NULL,
								    NULL);
				if (!g_ct_heap) {
					err = GetLastError();
					VirtualFree((void *)at, 0, MEM_RELEASE);
				}
			} else {
				err = GetLastError();
			}
		}
		if (g_ct_heap) {
			g_ct_lo = at;
			g_ct_hi = at + size;
		} else {
			ss_log("gameheap: crttap heap not made at %08lX, error %lu\n",
			       (unsigned long)at, err);
		}
	}
	ss_log("gameheap: crttap %d - msvcrt allocators of d3dx9_43, d3dcompiler_43 and "
	       "XAudio2_7 %s\n",
	       g_ct_mode,
	       g_ct_heap ? "served from our heap" : "counted and passed through to msvcrt");
	if (g_ct_mode >= 2)
		ss_log("gameheap: crttap heap %s at %08lX (%lu MB)\n",
		       g_ct_heap ? "made" : "NOT made", (unsigned long)at,
		       (unsigned long)(size >> 20));
}

static void ct_hook(HMODULE mod, const WCHAR *path)
{
	const WCHAR *leaf = path;
	char base[64];
	int i, k, n = 0;

	if (!g_ct_mode || !mod || !path)
		return;
	for (i = 0; path[i]; i++)
		if (path[i] == '\\' || path[i] == '/')
			leaf = path + i + 1;
	for (i = 0; leaf[i] && i < (int)sizeof(base) - 1; i++)
		base[i] = (char)leaf[i];
	base[i] = 0;
	for (k = 0; k < CT_NMOD; k++)
		if (!lstrcmpiA(base, kCtMod[k]))
			break;
	if (k == CT_NMOD)
		return;
	{
		IMAGE_NT_HEADERS *nt =
			(IMAGE_NT_HEADERS *)((unsigned char *)mod +
					     ((IMAGE_DOS_HEADER *)mod)->e_lfanew);

		g_ct[k].lo = (uintptr_t)mod;
		g_ct[k].hi = (uintptr_t)mod + nt->OptionalHeader.SizeOfImage;
	}
	{
		void *const ours[CT_NFN] = { (void *)ct_malloc, (void *)ct_calloc,
					     (void *)ct_realloc, (void *)ct_free,
					     (void *)ct_strdup, (void *)ct_new,
					     (void *)ct_delete, (void *)ct_amalloc,
					     (void *)ct_afree };

		for (i = 0; i < CT_NFN; i++) {
			int r = gh_iat_swap(mod, "msvcrt.dll", kCtFn[i], ours[i], NULL);

			if (r > 0)
				n += r;
		}
	}
	ss_log("gameheap: crttap on %s at %p - %d import slot(s)\n", kCtMod[k], (void *)mod, n);
}

static void ct_report(void)
{
	int k, f;
	char line[512];

	if (g_rt_crtheap)
		ss_log("  runtime HeapAlloc: %ld served from the private heap, %ld passed to "
		       "the runtime's heap\n",
		       (long)g_rt_served, (long)g_rt_passed);
	if (!g_ct_mode)
		return;
	ss_log("  crttap: msvcrt allocations by the Microsoft DLLs since launch%s\n",
	       g_ct_heap ? "" : " (passed through)");
	for (k = 0; k <= CT_NMOD; k++) {
		CtMod *m = &g_ct[k];
		int o = 0;

		if (k < CT_NMOD && !m->lo)
			continue;
		for (f = 0; f < CT_NFN; f++)
			if (m->calls[f])
				o += wsprintfA(line + o, " %s %ld", kCtFn[f], (long)m->calls[f]);
		if (!o)
			lstrcpyA(line, " no calls");
		ss_log("    %-18s live %ld, %ld KB; freed elsewhere-made %ld, spilled %ld;%s\n",
		       k < CT_NMOD ? kCtMod[k] : "other caller", (long)m->live,
		       (long)m->bytes / 1024, (long)m->foreign, (long)m->spill, line);
	}
}

/* D3D9SW_LAA_FALSE - the "own a region the game does not know about" experiment.
 *
 * Reserve a second block and, once on, the redirect and floor serve every new
 * allocation from it (gh_serving_heap), so the game runs out of memory we hold.
 * The point is to see whether a region we control, off to one side, can back the
 * game's own state without the game or Steam noticing - and, on a 32-bit non-LAA
 * exe, to find the wall: there is no space above 2 GB, so this competes with the
 * game's ~1.4 GB and is EXPECTED to hit an allocation failure or a crash. That is
 * the reading, not a bug. On an exe patched LAA (tools/laa.ps1) the >2 GB band
 * exists and we try to land there first, where the game never allocates.
 *
 * Called after gh_create_heap so RtlCreateHeap is resolved; it re-resolves if the
 * arena was a plain HeapCreate and never went down the pin path. */
static void gh_create_laa_region(void)
{
	unsigned mb = gh_knob("D3D9SW_LAA_MB", 512);
	SIZE_T size = (SIZE_T)mb * 1024u * 1024u;
	void *res;

	if (!gh_knob("D3D9SW_LAA_FALSE", 0))
		return;
	g_laa_mb = mb;
	if (!p_RtlCreateHeap) {
		HMODULE nt = GetModuleHandleA("ntdll.dll");

		p_RtlCreateHeap = nt ? (PVOID(NTAPI *)(ULONG, PVOID, SIZE_T, SIZE_T, PVOID,
						       PVOID))GetProcAddress(nt, "RtlCreateHeap")
				     : NULL;
	}
	if (!p_RtlCreateHeap) {
		ss_log("gameheap: LAA_FALSE asked for but RtlCreateHeap is unavailable\n");
		return;
	}
	/* The >2 GB band only exists if the exe is LAA; try it first so a patched exe
	 * puts our region where the game's own allocator never reaches. */
	res = VirtualAlloc((LPVOID)0x80000000u, size, MEM_RESERVE, PAGE_READWRITE);
	g_laa_high = (res != NULL);
	if (!res)
		res = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE); /* OS places it */
	if (!res) {
		ss_log("gameheap: LAA_FALSE could not reserve %u MB anywhere (error %lu) - "
		       "no hole this large exists; that is the 2 GB ceiling talking, and "
		       "is itself the answer\n",
		       mb, GetLastError());
		return;
	}
	g_laa_heap = (HANDLE)p_RtlCreateHeap(0, res, size, 1u << 20, NULL, NULL);
	if (!g_laa_heap) {
		ss_log("gameheap: LAA_FALSE reserved %u MB at %08lX but RtlCreateHeap "
		       "refused it (error %lu)\n",
		       mb, (unsigned long)(uintptr_t)res, GetLastError());
		VirtualFree(res, 0, MEM_RELEASE);
		return;
	}
	g_laa_lo = (uintptr_t)res;
	g_laa_hi = (uintptr_t)res + size;
	g_laa_on = 1;
	ss_log("gameheap: LAA_FALSE region PINNED at %08lX, %u MB, %s. Every "
	       "redirected allocation from here on comes out of it - memory we own "
	       "and the game does not know exists. Any crash past this line is the "
	       "experiment reporting, not a regression\n",
	       (unsigned long)(uintptr_t)res, mb,
	       g_laa_high ? "in the >2 GB band, so the exe really is LAA"
			  : "inside the shared 2 GB - it competes with the game");
}

/* D3D9SW_WHOLESALE - the owned-heap wholesale-restore probe.
 *
 * Block-by-block restore leaves the allocator's free lists alone on purpose,
 * because rewinding them on the process default heap desynchronises from the
 * allocator state that lives OUTSIDE the region (the LFH's per-processor buckets
 * above all) and kills the process. But a heap WE created can be made to keep all
 * of its bookkeeping inside its own region: turn the Low Fragmentation Heap off
 * and the free lists, headers and lookaside all live in the bytes we rewind. Then
 * the region can be restored whole - freelist and all - and a block the game
 * allocated after the save simply ceases to exist, which is the address-reuse
 * crash's actual cure. This is opt-in and separate from the working config. */
static int gh_wholesale(void)
{
	static int v = -1;

	if (v < 0)
		v = (int)gh_knob("D3D9SW_WHOLESALE", 0);
	return v;
}

static void gh_make_selfcontained(HANDLE h)
{
	ULONG std = 0; /* HeapCompatibilityInformation: 0 = standard heap, no LFH */

	if (h)
		HeapSetInformation(h, HeapCompatibilityInformation, &std, sizeof(std));
}

/* The owned-heap base anchors for a relocatable (cross-session) restore. A saved
 * pointer into the arena is base+offset with a STABLE offset (the whole point of
 * the pin), so cross-session it becomes new_base+offset - all the relocation pass
 * needs is the base recorded at save and the new base read here at restore.
 * Modules (the exe included) are anchored separately by the savestate module
 * table; this covers the non-module owned heaps. Stable order: arena, then the
 * LAA_FALSE region. Writes up to `max` {base,size} pairs, returns the count.
 * Ranges are reported ONLY when bounded (a pinned or OS-placed reservation) -
 * a growable HeapCreate fallback has no fixed extent to relocate against. */
int gameheap_anchor_ranges(uintptr_t *base, uintptr_t *size, int max)
{
	int n = 0;

	if (g_heap_lo && g_heap_hi && n < max) {
		base[n] = g_heap_lo;
		size[n] = g_heap_hi - g_heap_lo;
		n++;
	}
	if (g_laa_lo && g_laa_hi && n < max) {
		base[n] = g_laa_lo;
		size[n] = g_laa_hi - g_laa_lo;
		n++;
	}
	return n;
}

/* Every span we pinned at a fixed address: the heap, the game's own
 * reservations and the big-block chunks. For labelling only. */
int gameheap_pin_ranges(uintptr_t *base, uintptr_t *size, const char **what, int max)
{
	int n = 0;

	if (g_heap_lo && g_heap_hi && n < max) {
		base[n] = g_heap_lo;
		size[n] = g_heap_hi - g_heap_lo;
		what[n++] = "gameheap pin: heap";
	}
	if (g_raw_base && g_raw_n && n < max) {
		base[n] = g_raw_base;
		size[n] = (uintptr_t)g_raw_n * GH_RAW_GRAN;
		what[n++] = "gameheap pin: game reservations";
	}
	if (g_bigpin_base && g_bigpin_size && n < max) {
		base[n] = g_bigpin_base;
		size[n] = g_bigpin_size;
		what[n++] = "gameheap pin: big blocks";
	}
	if (g_bigpin2_base && g_bigpin2_size && n < max) {
		base[n] = g_bigpin2_base;
		size[n] = g_bigpin2_size;
		what[n++] = "gameheap pin: big blocks, second span";
	}
	return n;
}

int gameheap_owned_heap(uintptr_t base)
{
	if (!gh_wholesale())
		return 0;
	/* Range, not equality: a heap's committed memory can surface as several
	 * VirtualQuery regions at different bases within its reservation, and every
	 * one of them must be restored whole for the freelist to come back intact. */
	if (g_heap_hi && base >= g_heap_lo && base < g_heap_hi)
		return 1;
	if (g_laa_hi && base >= g_laa_lo && base < g_laa_hi)
		return 1;
	return 0;
}

/* Written from the census rather than from the allocator, because this opens a
 * file and formats text and neither belongs on a path the game takes millions of
 * times. Rewritten in full each time rather than appended, so the file always
 * describes one run from its first allocation and two runs can be diffed
 * directly. */
static void gh_trace_write(const char *path)
{
	HANDLE f;
	char line[64];
	LONG n = g_tr_n, i;
	DWORD wrote = 0;

	if (!g_tr)
		return;
	if (n > g_tr_cap)
		n = g_tr_cap;
	f = swlog_create(path, CREATE_ALWAYS);
	if (f == INVALID_HANDLE_VALUE) {
		ss_log("gameheap: cannot write %s, error %lu\n", path, GetLastError());
		return;
	}
	/* The base goes in a comment rather than into the offsets, which are what a
	 * diff compares. Two runs with different bases and identical offset columns
	 * is the whole result. */
	wsprintfA(line, "# heap %08lX  %ld op(s)%s\r\n", (unsigned long)(uintptr_t)g_heap,
		  (long)n, (g_tr_n > g_tr_cap) ? "  TRUNCATED" : "");
	WriteFile(f, line, (DWORD)lstrlenA(line), &wrote, NULL);
	/* site is an RVA into the executable and ordinal is the count that site
	 * had already reached, so the pair names the block independently of where
	 * it landed. Readers that only want the first three columns still work:
	 * the extra two are appended, not inserted. */
	wsprintfA(line, "# image %08lX\r\n", (unsigned long)g_exe);
	WriteFile(f, line, (DWORD)lstrlenA(line), &wrote, NULL);
	/* Threads are numbered in the order they first allocate, so the column
	 * compares across launches where the ids themselves never match. */
	WriteFile(f, "# op size offset site ordinal thread\r\n", 38, &wrote, NULL);
	{
		unsigned tids[64];
		int nt = 0, t;

		for (i = 0; i < n; i++) {
			int k;

			for (t = 0; t < nt && tids[t] != g_tr[i].tid; t++)
				;
			if (t == nt && nt < 64)
				tids[nt++] = g_tr[i].tid;
			k = wsprintfA(line, "%c %08lX %08lX %08lX %08lX T%d\r\n",
				      (char)g_tr[i].op, (unsigned long)g_tr[i].size,
				      (unsigned long)g_tr[i].off, (unsigned long)g_tr[i].site,
				      (unsigned long)g_tr[i].ord, t);
			WriteFile(f, line, (DWORD)k, &wrote, NULL);
		}
	}
	CloseHandle(f);
	ss_log("gameheap: %s written, %ld of %ld operation(s)%s\n", path, (long)n,
	       (long)g_tr_n,
	       (g_tr_n > g_tr_cap) ? " - the buffer filled, raise D3D9SW_GHTRACE" : "");
	if (g_tag_full || g_site_full || g_tag_congested)
		ss_log("gameheap: %ld block(s) and %ld chute(s) went unnamed, %ld "
		       "lookup(s) gave up searching - the tag tables are too small "
		       "for a run this long, so names after that point are short\n",
		       (long)g_tag_full, (long)g_site_full, (long)g_tag_congested);
}

/* --------------------------------------------- where the session seed enters
 *
 * D3D9SW_CLOCKPROBE=1 reports every call the game makes to the three
 * calendar-time functions it imports, with the caller's RVA and - the part that
 * makes it useful - the allocator operation number at the time of the call.
 *
 * The question it answers. Two sessions from an identical save allocate
 * identically for 3,363 operations and then differ in the length of one string.
 * Something the game knows must therefore differ before that point while
 * leaving no trace in the heap until it is formatted, and a value read from the
 * calendar is the obvious candidate: it changes every run by construction, it
 * is usually stored in a fixed-size structure or a register rather than an
 * allocation, and it becomes visible exactly when somebody prints it.
 *
 * The operation number is what turns this from a list into evidence. Clock
 * calls and allocations are two streams with no common clock of their own; the
 * trace index is a counter both can be read against, so a call reported at
 * operation 3,300 is one that happened in the window where the divergence was
 * introduced, and a call at operation 7,000 is not.
 *
 * Only the calendar functions are hooked, not QueryPerformanceCounter or
 * timeGetTime. Those are read every frame and would bury the rare calls that
 * matter under thousands that do not - and a per-frame timer is a poor seed
 * anyway, being exactly what the game would use for animation.
 *
 * These are imports, so patching an import slot is enough. That is not true of
 * the allocator, which the game links statically and which had to be detoured
 * instruction by instruction. */
static void(WINAPI *r_GetLocalTime)(LPSYSTEMTIME);
static void(WINAPI *r_GetSystemTime)(LPSYSTEMTIME);
static void(WINAPI *r_GetSystemTimeAsFileTime)(LPFILETIME);
static volatile LONG g_clk_n;

static void clk_note(const char *what, void *ra)
{
	LONG i = InterlockedIncrement(&g_clk_n);
	uintptr_t a = (uintptr_t)ra;

	/* Bounded, because a call in a loop would otherwise fill the log with the
	 * one thing we already know. The first calls are the ones that can have
	 * seeded anything. */
	if (i > 48)
		return;
	if (g_exe && a >= g_exe)
		ss_log("clock: %s called from +%08lX at allocator operation %ld\n",
		       what, (unsigned long)(a - g_exe), (long)g_tr_n);
	else
		ss_log("clock: %s called from %p (outside the executable) at "
		       "allocator operation %ld\n",
		       what, ra, (long)g_tr_n);
}

static void WINAPI gh_GetLocalTime(LPSYSTEMTIME t)
{
	clk_note("GetLocalTime", __builtin_return_address(0));
	r_GetLocalTime(t);
}

static void WINAPI gh_GetSystemTime(LPSYSTEMTIME t)
{
	clk_note("GetSystemTime", __builtin_return_address(0));
	r_GetSystemTime(t);
}

static void WINAPI gh_GetSystemTimeAsFileTime(LPFILETIME t)
{
	clk_note("GetSystemTimeAsFileTime", __builtin_return_address(0));
	r_GetSystemTimeAsFileTime(t);
}

static void clock_probe_install(HMODULE exe)
{
	void *prev;
	int n = 0;

	if (!gh_knob("D3D9SW_CLOCKPROBE", 0))
		return;
	if (!g_exe)
		g_exe = (uintptr_t)exe;
	prev = NULL;
	if (savestate_patch_iat_named(exe, "KERNEL32.dll", "GetLocalTime",
				      (void *)gh_GetLocalTime, &prev) &&
	    prev) {
		r_GetLocalTime = (void(WINAPI *)(LPSYSTEMTIME))prev;
		n++;
	}
	prev = NULL;
	if (savestate_patch_iat_named(exe, "KERNEL32.dll", "GetSystemTime",
				      (void *)gh_GetSystemTime, &prev) &&
	    prev) {
		r_GetSystemTime = (void(WINAPI *)(LPSYSTEMTIME))prev;
		n++;
	}
	prev = NULL;
	if (savestate_patch_iat_named(exe, "KERNEL32.dll", "GetSystemTimeAsFileTime",
				      (void *)gh_GetSystemTimeAsFileTime, &prev) &&
	    prev) {
		r_GetSystemTimeAsFileTime = (void(WINAPI *)(LPFILETIME))prev;
		n++;
	}
	ss_log("clock: %d of 3 calendar import(s) patched - every call will be "
	       "reported with the allocator operation number it happened at, so "
	       "it can be lined up against where two runs part\n",
	       n);
}

/* D3D9SW_GHPEEK=off,off,... prints what is actually stored at those heap
 * offsets, as hex and as text.
 *
 * Added for one specific question. Two runs of the game, from an identical save
 * and visibly doing the same things, allocate identically for 3,363 operations
 * and then differ in the size of a single block: 0x25 bytes in one, 0x2E in the
 * other, from the same call site with the same ordinal at the same offset. That
 * 9-byte difference rounds up to an 8-byte shift and drags every later block
 * along with it, which is the whole of the disagreement between those runs.
 *
 * A size that changes between runs at a fixed point in the sequence is almost
 * always a string, and a string can be read. The trace can say a block changed
 * size; only its contents can say why.
 *
 * Reading is guarded by VirtualQuery rather than assumed safe: the offset comes
 * from a human typing into a config file, and a wrong one would otherwise take
 * the game down inside a diagnostic. */
static void gh_peek_one(unsigned off)
{
	MEMORY_BASIC_INFORMATION mbi;
	const unsigned char *p = (const unsigned char *)g_heap + off;
	char line[128];
	unsigned row;

	if (!g_heap)
		return;
	if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
	    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
		ss_log("gameheap: peek +%08lX is not readable\n", (unsigned long)off);
		return;
	}
	for (row = 0; row < 4; row++) {
		const unsigned char *r = p + row * 16;
		char *o = line;
		unsigned k;

		o += wsprintfA(o, "gameheap: peek +%08lX ", (unsigned long)(off + row * 16));
		for (k = 0; k < 16; k++)
			o += wsprintfA(o, "%02X ", r[k]);
		*o++ = '|';
		for (k = 0; k < 16; k++)
			*o++ = (r[k] >= 32 && r[k] < 127) ? (char)r[k] : '.';
		*o++ = '|';
		*o = 0;
		ss_log("%s\n", line);
	}
}

/* Parsed once at install rather than read at each use. The free path calls into
 * this, and reading a knob from there would put config machinery underneath
 * every free in the process. */
static void gh_peek_parse(void)
{
	char v[96];
	unsigned n = savestate_getenv("D3D9SW_GHPEEK", v, sizeof(v));
	unsigned k, cur = 0;
	int any = 0;

	g_peek_n = 0;
	if (!n || n >= sizeof(v))
		return;
	for (k = 0; k <= n; k++) {
		char c = (k < n) ? v[k] : 0;

		if (c >= '0' && c <= '9') {
			cur = cur * 16 + (unsigned)(c - '0');
			any = 1;
		} else if (c >= 'a' && c <= 'f') {
			cur = cur * 16 + (unsigned)(c - 'a') + 10;
			any = 1;
		} else if (c >= 'A' && c <= 'F') {
			cur = cur * 16 + (unsigned)(c - 'A') + 10;
			any = 1;
		} else if (c == 'x' || c == 'X') {
			cur = 0; /* tolerate a 0x prefix */
		} else {
			if (any && g_peek_n < GH_PEEK_MAX)
				g_peek_off[g_peek_n++] = cur;
			cur = 0;
			any = 0;
		}
	}
}

static void gh_peek(void)
{
	int i;

	for (i = 0; i < g_peek_n; i++)
		gh_peek_one(g_peek_off[i]);
}

/* The save is the wrong moment for a transient.
 *
 * The block this was built to read - the 37-vs-46 byte string at +0xB8610 that
 * splits two otherwise identical sessions - is allocated during startup and
 * freed long before any save. Peeking at report time found four saves' worth of
 * zeroes, which says only that nothing had reused the address yet.
 *
 * A freed block still holds what was written to it right up until the allocator
 * takes it back, so the last honest moment to read one is the instant before it
 * goes. That is here. */
/* -------------------------------------------------- the vorbis witness
 *
 * The block whose size differs between two runs turned out to be the Vorbis
 * vendor string: 0x25 holds "Xiph.Org libVorbis I 20150105 (????)" and 0x2E
 * holds the 20140122 one, whose codename is Latin-1 and so 45 bytes rather than
 * the 47 an ASCII transliteration of it would suggest. This comment named the
 * 20101101 build before the run that settled it, which is a trap worth leaving
 * marked: that build's vendor string is also 45 bytes, so the arithmetic alone
 * cannot tell the two apart and only the logged string can. The music was not
 * all encoded with the same
 * library version, so the allocator's stream depends on which track is being
 * decoded - that is game state, not chance. Naming the track at the operation
 * number it loads at turns the divergence from a nuisance into a coordinate.
 *
 * Reads only, on the free path, capped so a long session cannot bury the log. */
#define GH_VORB_LINES 96
static int g_vorb_on = -1;
static LONG g_vorb_said;

static void gh_vorb_text(char *out, int cap, const unsigned char *p, unsigned n)
{
	unsigned i;

	if ((unsigned)(cap - 1) < n)
		n = (unsigned)(cap - 1);
	for (i = 0; i < n; i++) {
		unsigned char c = p[i];
		if (!c)
			break;
		out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
	}
	out[i] = 0;
}

static void gh_vorbis_free(const void *u, unsigned size)
{
	const unsigned char *p = (const unsigned char *)u;
	char text[128];
	unsigned i;
	int tag = 0;

	if (g_vorb_on < 0)
		g_vorb_on = (int)gh_knob("D3D9SW_GHVORBIS", 0);
	if (!g_vorb_on || !u || size < 8 || size > 512)
		return;
	if (g_vorb_said >= GH_VORB_LINES)
		return;

	if (size >= 19 && !memcmp(p, "Xiph.Org libVorbis", 18)) {
		gh_vorb_text(text, sizeof(text), p, size);
		InterlockedIncrement(&g_vorb_said);
		ss_log("vorbis: comment header freed at allocator operation %ld - "
		       "%lu byte(s), 0x%lX, vendor \"%s\"\n",
		       (long)g_tr_n, (unsigned long)size, (unsigned long)size, text);
		return;
	}

	/* A comment is KEY=value in plain ASCII. TITLE is the one that would name
	 * the track; the others are kept because this encoder wrote very few and
	 * whichever it did write is the only handle on identity we have. */
	for (i = 0; i < size; i++) {
		unsigned char c = p[i];
		if (!c)
			break;
		if (c == '=' && i)
			tag = 1;
		else if (c < 0x20 || c >= 0x7F)
			return;
	}
	if (!tag || i < 3)
		return;
	gh_vorb_text(text, sizeof(text), p, size);
	InterlockedIncrement(&g_vorb_said);
	ss_log("vorbis: tag freed at allocator operation %ld - \"%s\"\n",
	       (long)g_tr_n, text);
}

static void gh_peek_free(void *u)
{
	unsigned off;
	int i;

	if (g_peek_n <= 0 || !u || !g_heap)
		return;
	off = (unsigned)((char *)u - (char *)g_heap);
	for (i = 0; i < g_peek_n; i++) {
		if (g_peek_off[i] != off)
			continue;
		ss_log("gameheap: the watched block at +%08lX is being freed - this is "
		       "what it held:\n",
		       (unsigned long)off);
		gh_peek_one(off);
		return;
	}
}

/* -------------------------------------------------- the allocation watch
 *
 * GHVORBIS names the moment a track loads; this names the block a track load
 * hands back. Set D3D9SW_GHWATCH to the size(s) of the block being chased - the
 * 0ED6BEB0-class Vorbis decoder object is ~3.6 KB - and/or D3D9SW_GHWATCHSITE to
 * a call-site RVA, and every matching allocation prints, at the instant it is
 * served: the allocator operation number it lands at (the SAME clock GHVORBIS
 * prints its landmark on, so the two line up directly), the call site as
 * rabiribi.exe+RVA, the returned address, and whether that address is in our
 * arena or on a heap we do not own.
 *
 * The point is provenance across a restore. The block that faults after a
 * restore-across-a-track-change is read through a pointer that came back wrong;
 * to fix it we first need to know which code allocates it and where in the load
 * sequence, so a second run can confirm the site is deterministic. If the watch
 * stays silent for a size known to crash, that is itself the answer: the block
 * is born on a path we never owned (foreign heap, no redirect, no tag), which is
 * exactly why a restore cannot keep it consistent.
 *
 * Sizes are matched exactly, because the trace has shown block sizes are
 * deterministic to the byte at a fixed point in the sequence. One ss_log per
 * match, capped; on a miss the only cost is the size compare, and every path
 * this sits on is already handing back a pointer - it allocates nothing. */
#define GH_WATCH_MAX 8
#define GH_WATCH_LINES 256
static unsigned g_watch_sz[GH_WATCH_MAX];
static int g_watch_sz_n;
static unsigned g_watch_site[GH_WATCH_MAX];
static int g_watch_site_n;
static LONG g_watch_said;

/* Comma/space separated, each token decimal or 0x-hex. Parsed once at install so
 * the hot path only ever reads the parsed array, never a config string. */
static int gh_watch_list(const char *knob, unsigned *out, int max)
{
	char v[96];
	unsigned n = savestate_getenv(knob, v, sizeof(v));
	unsigned k, cur = 0;
	int got = 0, any = 0, hex = 0;

	if (!n || n >= sizeof(v))
		return 0;
	for (k = 0; k <= n; k++) {
		char c = (k < n) ? v[k] : 0;

		if (c == 'x' || c == 'X') {
			hex = 1; /* a 0x prefix; drop the leading 0 already taken */
			cur = 0;
		} else if (c >= '0' && c <= '9') {
			cur = cur * (hex ? 16u : 10u) + (unsigned)(c - '0');
			any = 1;
		} else if (hex && c >= 'a' && c <= 'f') {
			cur = cur * 16u + (unsigned)(c - 'a') + 10u;
			any = 1;
		} else if (hex && c >= 'A' && c <= 'F') {
			cur = cur * 16u + (unsigned)(c - 'A') + 10u;
			any = 1;
		} else {
			if (any && got < max)
				out[got++] = cur;
			cur = 0;
			any = 0;
			hex = 0;
		}
	}
	return got;
}

static void gh_watch_parse(void)
{
	char line[192];
	char *o = line;
	int i;

	g_watch_sz_n = gh_watch_list("D3D9SW_GHWATCH", g_watch_sz, GH_WATCH_MAX);
	g_watch_site_n = gh_watch_list("D3D9SW_GHWATCHSITE", g_watch_site, GH_WATCH_MAX);
	if (!g_watch_sz_n && !g_watch_site_n)
		return;
	o += wsprintfA(o, "gameheap: watch armed -");
	for (i = 0; i < g_watch_sz_n; i++)
		o += wsprintfA(o, " size %lu(0x%lX)", (unsigned long)g_watch_sz[i],
			       (unsigned long)g_watch_sz[i]);
	for (i = 0; i < g_watch_site_n; i++)
		o += wsprintfA(o, " site+%08lX", (unsigned long)g_watch_site[i]);
	ss_log("%s\n", line);
}

static void gh_watch(const void *u, size_t n, unsigned site, unsigned ord,
		     const char *how)
{
	char base[64];
	int i, hit = 0;

	if ((g_watch_sz_n <= 0 && g_watch_site_n <= 0) || !u)
		return;
	for (i = 0; i < g_watch_sz_n && !hit; i++)
		if (g_watch_sz[i] == (unsigned)n)
			hit = 1;
	for (i = 0; i < g_watch_site_n && !hit; i++)
		if (site != 0xFFFFFFFFu && g_watch_site[i] == site)
			hit = 1;
	if (!hit)
		return;
	/* Cap like the vorbis witness so a size that recurs cannot bury the log. */
	if (InterlockedIncrement(&g_watch_said) > GH_WATCH_LINES)
		return;

	if (in_range(u)) {
		wsprintfA(base, "arena +%08lX",
			  (unsigned long)((uintptr_t)u - (uintptr_t)g_heap));
	} else {
		MEMORY_BASIC_INFORMATION mbi;

		/* Only on a match, so the VirtualQuery is off the hot path. Naming the
		 * allocation base tells arena from a foreign heap at a glance - the
		 * whole point being to catch the block born where we do not own it. */
		if (VirtualQuery(u, &mbi, sizeof(mbi)))
			wsprintfA(base, "foreign, base %08lX",
				  (unsigned long)(uintptr_t)mbi.AllocationBase);
		else
			wsprintfA(base, "foreign");
	}
	if (ord == 0xFFFFFFFFu)
		ss_log("gameheap: WATCH op %ld  %-16s  %lu byte(s) (0x%lX)  "
		       "site rabiribi.exe+%08lX  -> %08lX  [%s]\n",
		       (long)g_tr_n, how, (unsigned long)n, (unsigned long)n,
		       (unsigned long)site, (unsigned long)(uintptr_t)u, base);
	else
		ss_log("gameheap: WATCH op %ld  %-16s  %lu byte(s) (0x%lX)  "
		       "site rabiribi.exe+%08lX ord %lu  -> %08lX  [%s]\n",
		       (long)g_tr_n, how, (unsigned long)n, (unsigned long)n,
		       (unsigned long)site, (unsigned long)ord,
		       (unsigned long)(uintptr_t)u, base);
}

/* Numbered as well as named, because a run can now be cut at several frames and
 * the comparison is per cut: gh_trace2.txt from one session against gh_trace2.txt
 * from another. gh_trace.txt keeps holding the latest, so anything that already
 * reads it carries on working. */
static void gh_trace_dump(void)
{
	static int cut;
	char name[32];

	if (!g_tr)
		return;
	/* The frame-1 save lands in the same frame the heap is installed, so it
	 * would otherwise write an empty trace over gh_trace.txt and peek at a
	 * heap that has served nothing. An empty dump answers no question. */
	if (g_tr_n <= 0) {
		ss_log("gameheap: nothing traced yet, the dump is skipped so the "
		       "last real one survives\n");
		return;
	}
	cut++;
	wsprintfA(name, "gh_trace%d.txt", cut);
	gh_trace_write(name);
	gh_trace_write("gh_trace.txt");
	gh_peek();
}

/* Read-only sibling of gh_tag_take for the fault reporter: names the block an
 * address belongs to WITHOUT retiring the tag. The tag table lives in our own
 * image, not the game's heap, so a restore that rewinds the block to zeroes
 * leaves its birth certificate - call site, size, ordinal - intact. That is
 * exactly the object we want named: one the game allocated after a save and the
 * restore then zeroed, whose own contents can no longer say what it was.
 *
 * Returns 1 when p is a tagged block's start, 2 when p falls inside one (a field
 * pointer, not the head), 0 when it matches nothing we served. site is an exe
 * RVA, size is bytes, ord is the n-th block that call site produced, and base -
 * when asked - is the block's start. Safe from a fault handler: reads only, no
 * lock, and it rejects anything outside our regions before the wider scan. */
int gameheap_whatis(const void *p, unsigned *site, unsigned *size, unsigned *ord,
		    uintptr_t *base)
{
	uintptr_t a = (uintptr_t)p;
	unsigned h, i;

	if (!g_tag || !p || !in_range(p))
		return 0;
	/* Exact first: one hashed probe chain, which is the common case when the
	 * faulting pointer is the object the game passed a method as `this`. */
	h = gh_mix((unsigned)a) & GH_TAG_MASK;
	for (i = 0; i < GH_PROBE; i++) {
		unsigned s = (h + i) & GH_TAG_MASK;
		LONG ad = g_tag[s].addr;

		if (ad == 0)
			break; /* chain ended without a hit; try the interior scan */
		if (ad == (LONG)a) {
			*site = g_tag[s].site;
			*size = g_tag[s].size;
			*ord = g_tag[s].ord;
			if (base)
				*base = a;
			return 1;
		}
	}
	/* Interior: the address is a field inside a block. There is no range index,
	 * so walk the table once and take the tightest block that contains it. This
	 * only ever runs from a one-shot fault report, never on the hot path. */
	{
		uintptr_t best_base = 0;
		unsigned best_size = 0, best_i = 0;
		int hit = 0;

		for (i = 0; i < GH_TAG_SLOTS; i++) {
			LONG ad = g_tag[i].addr;
			uintptr_t b;

			if (ad == 0 || ad == GH_TAG_DEAD)
				continue;
			b = (uintptr_t)ad;
			if (a >= b && a - b < g_tag[i].size &&
			    (!hit || g_tag[i].size < best_size)) {
				best_base = b;
				best_size = g_tag[i].size;
				best_i = i;
				hit = 1;
			}
		}
		if (hit) {
			*site = g_tag[best_i].site;
			*size = g_tag[best_i].size;
			*ord = g_tag[best_i].ord;
			if (base)
				*base = best_base;
			return 2;
		}
	}
	return 0;
}

void gameheap_report(void)
{
	gh_early_flush();
	gh_trace_dump();
	gl_write("gh_ledger.txt");
	swa_report();
	if (g_vaj && !g_vaj_held) {
		/* Not at attach: excluding starts the engine's helper thread, which
		 * must not happen under the loader lock. */
		savestate_exclude(g_vaj, sizeof(GhVaJ));
		g_vaj_held = 1;
	}
	if (g_res && !g_res_held) {
		savestate_exclude(g_res, sizeof(GhRes));
		g_res_held = 1;
	}
	if (g_al_real && !g_al_held) {
		savestate_exclude(g_al_real, sizeof(AlReal));
		g_al_held = 1;
	}
	if (g_di && !g_di_held) {
		savestate_exclude(g_di, sizeof(GhDiHome));
		g_di_held = 1;
	}
	if (g_pxt && !g_pxt_held) {
		savestate_exclude(g_pxt, sizeof(PxTrace));
		savestate_exclude(g_pxt_code, PXT_MAX * PXT_STUB);
#if defined(__i386__) || defined(_M_IX86)
		if (g_pxj)
			savestate_exclude(g_pxj, PXJ_MAX * sizeof(PxjRec));
#endif
		g_pxt_held = 1;
	}
	pxj_dump();
	ss_log("physx: %s; %ld step(s) run, %ld skipped\n", g_px_status, (long)g_px_steps,
	       (long)g_px_skipped);
	if (g_pxt)
		ss_log("physx trace: %ld method(s) in %ld table(s) watched, tick %ld\n",
		       (long)g_pxt->ns, (long)g_pxt->nvt, (long)g_pxt->tick);
	if (g_ready && g_imports) {
		size_t live = 0, peak = 0, com = 0;
		unsigned long nb = 0, fails = 0;
		LONG i;

		for (i = 0; i < g_nbig; i++) {
			live += g_bigch[i]->live;
			peak += g_bigch[i]->peak;
			com += g_bigch[i]->committed;
			nb += g_bigch[i]->nalloc;
			fails += g_bigch[i]->nfail;
		}
		ss_log("gameheap: import mode - %lu module(s), %lu slot(s); %lu allocation(s) "
		       "served, %lu freed to us, %lu passed back to the runtime, %lu "
		       "runtime block(s) migrated on realloc, %lu caught at ucrtbase's "
		       "floor\n",
		       g_mods_patched, g_slots_patched, g_alloc, g_freed_ours,
		       g_freed_theirs, g_migrated, g_caught);
		ss_log("gameheap: import mode - big blocks: %ld chunk(s), %lu served, %lu KB "
		       "live, %lu KB peak, %lu KB committed; %lu commit failure(s), %lu "
		       "chunk(s) refused, %lu bad header(s)%s\n",
		       (long)g_nbig, nb, (unsigned long)(live >> 10),
		       (unsigned long)(peak >> 10), (unsigned long)(com >> 10), fails,
		       g_big_nochunk, g_big_bad,
		       (fails || g_big_nochunk || g_big_bad) ? " <<< read this" : "");
		if (g_fellback || g_orphan_dropped || g_stale || g_mods_unbound)
			ss_log("gameheap: import mode - %lu fell back to the runtime (heap "
			       "full: raise D3D9SW_GHPIN_MB), %lu orphan(s) dropped, %lu "
			       "headerless pointer(s), %lu module(s) seen unbound\n",
			       g_fellback, g_orphan_dropped, g_stale, g_mods_unbound);
		return;
	}
	if (!g_ready) {
		if (on())
			ss_log("gameheap: asked for, but not installed\n");
		return;
	}
	ss_log("gameheap: %lu allocation(s) served, %lu freed to us, %lu passed back "
	       "to the runtime, %lu too big to take, %lu region(s) mapped, %ld "
	       "busy live (sidetable)%s\n",
	       g_alloc, g_freed_ours, g_freed_theirs, g_toobig, (unsigned long)g_nreg,
	       (long)g_busy_n, g_busy ? "" : " - NO TABLE");
	if (g_busy_full || g_busy_congested)
		ss_log("gameheap: %ld busy insert(s) missed the table, %ld take(s) "
		       "gave up searching - the sidetable is too small or too "
		       "full of tombstones\n",
		       (long)g_busy_full, (long)g_busy_congested);
	if (g_laa_on)
		ss_log("gameheap: LAA_FALSE region %08lX..%08lX (%u MB, %s) served the "
		       "game; %lu allocation(s) fell back because it was full (0 = it "
		       "held the whole session)\n",
		       (unsigned long)g_laa_lo, (unsigned long)g_laa_hi, g_laa_mb,
		       g_laa_high ? ">2 GB band" : "shared 2 GB", g_laa_fell);
	/* The number two design notes wanted before anything was armed. Zero says
	 * the runtime's allocator and the executable's other Heap callers never
	 * traded a block; anything else says they do, and says we caught it. */
	ss_log("gameheap: %lu block(s) of ours reached HeapFree by a route that is "
	       "not the runtime's free%s\n",
	       g_caught,
	       r_heapfree ? "" : " - UNMEASURED, the HeapFree floor is not in");
	if (g_orphan_dropped)
		ss_log("gameheap: %lu block(s) dropped rather than freed - inside our "
		       "arena with no header, so each one would have gone to a heap "
		       "that never issued it%s\n",
		       g_orphan_dropped,
		       " <<< every one of those was a heap corruption avoided");
	if (g_fellback || g_regfull || g_stale)
		ss_log("gameheap: %lu allocation(s) fell back to the runtime, %lu region(s) "
		       "past the table, %lu pointer(s) inside our range with no header - "
		       "any of the three is a hole worth reading about before trusting "
		       "this\n",
		       g_fellback, g_regfull, g_stale);
	if (g_floor_on) {
		HMODULE exe = GetModuleHandleA(NULL);
		uintptr_t base = (uintptr_t)exe;
		LONG i, n = g_floor_n;

		const char *fit;

		if (n > GH_FLOOR_SITES)
			n = GH_FLOOR_SITES;
		if (g_pin_mb && g_floor_peak_kb > (unsigned long)g_pin_mb * 1024)
			fit = " <<< PEAK EXCEEDS THE ARENA - it would need to grow to own this";
		else if (g_pin_mb)
			fit = " - fits the arena, so this site is redirectable as-is";
		else
			fit = "";
		ss_log("gameheap: allocation floor - %ld HeapAlloc + %ld HeapReAlloc "
		       "call(s) through the floor; OFF-ARENA live now %lu KB, PEAK %lu KB "
		       "(0 = every call owned) vs %u MB pinned arena%s\n",
		       (long)g_floor_a_calls, (long)g_floor_r_calls, g_floor_live_kb,
		       g_floor_peak_kb, g_pin_mb, fit);
		if (g_floor_live_kb)
			ss_log("gameheap: allocation floor - %lu KB still live at report: held "
			       "across, or freed by a route we do not intercept\n",
			       g_floor_live_kb);
		if (g_va_total)
			ss_log("gameheap: allocation floor - big blocks on dedicated "
			       "VirtualAlloc: %lu served, %ld live, %lu KB live, PEAK %lu KB. "
			       "Owned and freed on free, off the arena - so the arena can stay "
			       "small\n",
			       g_va_total, (long)g_va_n, g_va_live_kb, g_va_peak_kb);
		if (!n)
			ss_log("gameheap: allocation floor - no off-arena call site recorded\n");
		else {
			ss_log("gameheap: allocation floor - %ld game call site(s) outside the "
			       "arena%s:\n",
			       (long)n, g_floor_over ? ", plus more after the table filled" : "");
			for (i = 0; i < n; i++)
				ss_log("    rabiribi.exe+%08X  %ld alloc(s), %lu byte(s) cumulative\n",
				       (unsigned)((uintptr_t)g_floor[i].ret - base),
				       (long)g_floor[i].count, g_floor[i].bytes);
		}
	}
}

/* ---- heap census: the definitive size of the boundary we would own ---------
 *
 * The allocation floor above sees only what flows through the EXE's patched
 * HeapAlloc IAT. This measures the truth underneath: every heap the process has
 * (GetProcessHeaps), and for each its committed and live-busy bytes, block
 * count, biggest block, and a size histogram - walked directly with HeapWalk. It
 * answers the one question the "become the game's allocator" plan turns on: is
 * the game's live state a 50 MB boundary or a 1 GB one, and is it one heap or
 * many. The game's runtime (MSVCR100 via DxLib) makes its own heap with
 * HeapCreate, so it shows up here as a [game/other] heap distinct from our arena
 * and the process default.
 *
 * Read-only; forwarded to nobody. Must run while all threads are still LIVE, so
 * HeapLock can serialise each walk normally - walking a heap whose lock a
 * suspended thread holds would hang, which is why the save calls this BEFORE it
 * freezes. Each heap is guarded and skipped on failure so one uncooperative heap
 * never sinks the census, and the walk is bounded. 32-bit process, so every size
 * fits unsigned long as KB (wvsprintfA has no %llu). Off unless D3D9SW_HEAPCENSUS
 * is set - it walks every block, so it is not free. */
void gameheap_heap_census(void)
{
	char v[8], v2[8];
	DWORD wc = savestate_getenv("D3D9SW_HEAPCENSUS", v, sizeof(v));
	DWORD wa = savestate_getenv("D3D9SW_ASSETEXCL", v2, sizeof(v2));
	int census_on = (wc > 0 && wc < sizeof(v) && v[0] == '1');
	int asset_on = (wa > 0 && wa < sizeof(v2) && v2[0] == '1');
	HANDLE heaps[128];
	DWORD nheaps, i;
	HANDLE def = GetProcessHeap();
	unsigned long tot_commit_kb = 0, tot_busy_kb = 0, tot_blocks = 0;
	char exe_path[MAX_PATH], *exe_base = exe_path;
	DWORD el;

	/* The walk runs for either knob: the census logs, or ASSETEXCL needs the same
	 * per-heap profile to decide which heaps to hand savestate for holding. */
	if (!census_on && !asset_on)
		return;

	/* The game's own module name, so a busy block whose first word points into it
	 * is a game object (vtable/self-pointer) rather than a Windows one. */
	el = GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
	if (el && el < sizeof(exe_path)) {
		char *q;

		for (q = exe_path; *q; q++)
			if (*q == '\\' || *q == '/')
				exe_base = q + 1;
	} else {
		lstrcpynA(exe_path, "?", sizeof(exe_path));
	}

	nheaps = GetProcessHeaps(128, heaps);
	if (!nheaps) {
		ss_log("heapcensus: GetProcessHeaps returned 0 (err %lu)\n",
		       GetLastError());
		return;
	}
	ss_log("heapcensus: %lu process heap(s); default=%08lX ours-arena=%08lX "
	       "ours-laa=%08lX\n",
	       nheaps, (unsigned long)(uintptr_t)def, (unsigned long)(uintptr_t)g_heap,
	       (unsigned long)(uintptr_t)g_laa_heap);
	if (nheaps > 128)
		nheaps = 128;

	for (i = 0; i < nheaps; i++) {
		HANDLE h = heaps[i];
		PROCESS_HEAP_ENTRY e;
		unsigned long commit_kb = 0, busy_kb = 0, blocks = 0, regions = 0;
		unsigned long hist[6];        /* <256B <4K <64K <256K <1M >=1M */
		unsigned long maxblk = 0;
		unsigned long guard = 0;
		/* PROBE 1 - content owner attribution: which module the first word of each
		 * busy block points into. game = the EXE (a game object), ourmod = our
		 * shims, othermod = any other DLL (Windows/Steam), data = not a pointer.
		 * A heap dominated by game pointers is the game's private heap - a
		 * pin-candidate we can own wholesale; one dominated by othermod pointers is
		 * Windows'/Steam' and must stay in the present. Sampling is capped so 15k
		 * blocks do not turn into 15k loader-lock lookups. */
		unsigned long c_game = 0, c_ours = 0, c_mod = 0, c_data = 0, sampled = 0;
		/* PROBE 2 - content signature: a cheap rolling hash over every busy block
		 * (size + first word). Two census runs across a map transition can be
		 * diffed: a heap whose sig turns over wholesale while size holds is ASSETS
		 * the game reloaded (exclude, do not own); a stable sig is STATE (own). */
		unsigned long sig = 0;
		int locked, walked = 0, k;
		int ours = (h == g_heap) || (g_laa_heap && h == g_laa_heap);
		const char *tag = (h == g_heap) ? " [OURS arena]" :
				  (g_laa_heap && h == g_laa_heap) ? " [OURS laa]" :
				  (h == def) ? " [default]" : " [game/other]";

		for (k = 0; k < 6; k++)
			hist[k] = 0;

		locked = HeapLock(h) ? 1 : 0;
		memset(&e, 0, sizeof(e));
		while (HeapWalk(h, &e)) {
			walked = 1;
			if (e.wFlags & PROCESS_HEAP_REGION) {
				regions++;
				commit_kb += (unsigned long)((e.Region.dwCommittedSize +
							     1023) >> 10);
			} else if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
				unsigned long sz = (unsigned long)e.cbData;

				blocks++;
				busy_kb += (sz + 1023) >> 10;
				if (sz > maxblk)
					maxblk = sz;
				if (sz < 256) hist[0]++;
				else if (sz < 4096) hist[1]++;
				else if (sz < 65536) hist[2]++;
				else if (sz < 262144) hist[3]++;
				else if (sz < 1048576) hist[4]++;
				else hist[5]++;
				if (sz >= 4 && e.lpData) {
					unsigned long w =
						*(volatile unsigned long *)e.lpData;

					sig = sig * 1000003ul + w + sz;
					if (sampled < 512) {
						char mn[64];
						uintptr_t mo;
						const char *m = gh_mod_of(
							(const void *)(uintptr_t)w,
							mn, sizeof(mn), &mo);
						sampled++;
						if (m[0] == '?')
							c_data++;
						else if (!lstrcmpiA(m, exe_base))
							c_game++;
						else if (!lstrcmpiA(m, "d3d11.dll") ||
							 !lstrcmpiA(m, "dxgi.dll") ||
							 !lstrcmpiA(m, "xaudio2_9.dll") ||
							 !lstrcmpiA(m, "xinput1_4.dll"))
							c_ours++;
						else
							c_mod++;
					}
				}
			}
			if (++guard > 20000000ul)
				break;          /* bound the walk */
		}
		if (locked)
			HeapUnlock(h);

		if (!walked) {
			ss_log("heapcensus: heap %08lX%s - HeapWalk yielded nothing "
			       "(err %lu)\n",
			       (unsigned long)(uintptr_t)h, tag, GetLastError());
			continue;
		}
		ss_log("heapcensus: heap %08lX%s - committed %lu KB, busy %lu KB in %lu "
		       "block(s), %lu region(s), biggest %lu KB\n",
		       (unsigned long)(uintptr_t)h, tag, commit_kb, busy_kb, blocks,
		       regions, (maxblk + 1023) >> 10);
		ss_log("    sizes: <256B %lu, <4K %lu, <64K %lu, <256K %lu, <1M %lu, "
		       ">=1M %lu\n",
		       hist[0], hist[1], hist[2], hist[3], hist[4], hist[5]);
		ss_log("    owner (of %lu sampled): game %lu, ours %lu, othermod %lu, "
		       "data %lu; sig %08lX%s\n",
		       sampled, c_game, c_ours, c_mod, c_data, sig,
		       (!ours && sampled && c_game > c_mod && c_game >= c_data)
			       ? "  <<< GAME-dominated private heap - pin candidate"
			       : "");
		/* Option 3: a large, pure-data, non-default, non-ours HeapCreate heap with
		 * (almost) no code pointers is the game's asset heap - the one that
		 * ballooned across the room change. Hand it to savestate to hold in the
		 * present so the game reloads it, instead of rewinding it to a moved base.
		 * The ~2 game-pointer slack tolerates a stray vtable in an asset wrapper. */
		if (asset_on && !ours && h != def && commit_kb >= 1024 && sampled &&
		    c_data * 10 >= sampled * 9 && c_game <= 2) {
			savestate_note_asset_heap(h);
			ss_log("    -> ASSETEXCL: heap %08lX marked regenerable ASSET - it "
			       "will be held in the present and reloaded by the game\n",
			       (unsigned long)(uintptr_t)h);
		}
		if (!ours) {
			tot_commit_kb += commit_kb;
			tot_busy_kb += busy_kb;
			tot_blocks += blocks;
		}
	}
	ss_log("heapcensus: NON-OURS total - committed %lu KB (%lu MB), busy %lu KB "
	       "(%lu MB), %lu block(s). THIS is the boundary to own for cross-session "
	       "(arena is %u MB)\n",
	       tot_commit_kb, tot_commit_kb >> 10, tot_busy_kb, tot_busy_kb >> 10,
	       tot_blocks, g_pin_mb);
}

/* HEAPBLOCKS for this heap under Wine, without HeapWalk. Wine has no Windows
 * HEAP_SEGMENT, so the engine used to memcpy the whole region - lists, TLS
 * objects a LEFT RUNNING mmdevapi thread still holds, everything. The magic
 * is keyed to the user pointer, so a word that merely looks busy does not
 * match. */
int gameheap_saved_block(const void *saved, void *live_head, size_t remain, size_t *n)
{
	const GhHead *h;
	uintptr_t user;
	size_t total;

	if (!saved || !live_head || !n || remain < sizeof(GhHead))
		return 0;
	h = (const GhHead *)saved;
	user = (uintptr_t)live_head + sizeof(GhHead);
	if (h->magic != (GH_MAGIC ^ user))
		return 0;
	if (!h->size || h->size >= GH_BIG)
		return 0;
	total = sizeof(GhHead) + h->size;
	if (total > remain)
		return 0;
	*n = total;
	return 1;
}

unsigned gameheap_busy_snapshot(void)
{
	unsigned i, n = 0;

	g_busy_snap_n = 0;
	if (!g_busy || !g_busy_snap)
		return 0;
	for (i = 0; i < GH_BUSY_SLOTS; i++) {
		LONG a = g_busy[i].addr;
		size_t sz;

		if (a == 0 || a == GH_BUSY_DEAD)
			continue;
		sz = g_busy[i].size;
		if (!sz || sz >= GH_BIG)
			continue;
		if (n >= g_busy_snap_cap)
			break;
		g_busy_snap[n].head = (char *)(uintptr_t)a - sizeof(GhHead);
		g_busy_snap[n].total = sizeof(GhHead) + sz;
		n++;
	}
	g_busy_snap_n = n;
	return n;
}

unsigned gameheap_busy_saved_count(void)
{
	return g_busy_snap_n;
}

int gameheap_busy_saved_at(unsigned i, void **head, size_t *total)
{
	if (!head || !total || !g_busy_snap || i >= g_busy_snap_n)
		return 0;
	*head = g_busy_snap[i].head;
	*total = g_busy_snap[i].total;
	return 1;
}

void gameheap_busy_rewind(void)
{
	unsigned i;

	if (!g_busy || !g_busy_snap || !g_busy_snap_n)
		return;
	memset(g_busy, 0, GH_BUSY_SLOTS * sizeof(GhBusy));
	g_busy_n = 0;
	g_busy_full = 0;
	g_busy_congested = 0;
	for (i = 0; i < g_busy_snap_n; i++) {
		void *u = (char *)g_busy_snap[i].head + sizeof(GhHead);
		size_t sz = g_busy_snap[i].total - sizeof(GhHead);

		gh_busy_put(u, sz);
	}
}
