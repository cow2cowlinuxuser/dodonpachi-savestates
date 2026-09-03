/* Rewind for a process we do not own, driven from the graphics wrapper we
 * already inject.
 *
 * Windows has no checkpoint/restore. PssCaptureSnapshot captures a process but
 * cannot put one back, so the state is reconstructed by hand: suspend every
 * game thread, copy its writable memory and register state aside, and write
 * both back on demand.
 *
 * Two things make that tractable here rather than merely theoretical. The
 * renderer is software, so every render target, texture and piece of pipeline
 * state is ordinary process memory we already own - there is no driver-side
 * state to rewind, which is normally the blocker. And the scope is a
 * within-session rewind, so no handle, socket or file has to survive being
 * recreated; the kernel objects the game holds are never closed, and their
 * handle values are still valid when the old memory goes back.
 *
 * Three constraints shape everything below.
 *
 * Nothing this file owns may appear in the snapshot. The bookkeeping describes
 * the restore that is in progress, so if the restore writes over it the loop
 * destroys its own instructions as it runs. Every allocation here is therefore
 * registered in an exclusion list that the region walk consults, and no CRT
 * heap is touched between suspend and resume - a rewind of the heap under a
 * live malloc is the same bug wearing a different hat.
 *
 * The target is a 32-bit process holding well over a gigabyte. A snapshot
 * cannot be a flat buffer in that address space, so it lives in a
 * pagefile-backed section and is streamed through a small sliding view. The
 * section may exceed what the process could ever map at once; only the window
 * costs address space.
 *
 * Present runs on one of the game's own threads, so the code requesting a
 * rewind stands on a stack the rewind must overwrite. The work happens on a
 * private helper thread whose stack is excluded, and the requesting thread
 * parks in a user-mode spin - never a kernel wait, which cannot be reliably
 * resumed after SetThreadContext - at a fixed instruction, so the context
 * captured during a save is always valid to restore into. */

/* Which of the shipped DLLs this is. Logs arrive from other people's machines
 * with no way to tell which binary produced them, and a build that differs only
 * in code generation looks identical from the outside. */
#ifndef D3D9SW_VARIANT
#define D3D9SW_VARIANT stock
#endif
#define SW_VARIANT_STR2(x) #x
#define SW_VARIANT_STR(x) SW_VARIANT_STR2(x)

#define SW_ALLOC_IMPL
#include "savestate.h"
#include "swalloc.h"
#include "swrast.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <tlhelp32.h>
#include <windows.h>

#define SS_MAX_REGIONS 65536
#define SS_MAX_THREADS 256
#define SS_MAX_EXCL 1024
#define SS_MAX_HEAPS 64
#define SS_MAX_MODS 256
#define SS_MAX_SEGS 256
#define SS_VIEW_BYTES (32u * 1024u * 1024u)

typedef struct Region {
	uintptr_t base;
	uintptr_t size;
	uintptr_t alloc_base;
	DWORD prot;
	DWORD type;
} Region;

/* The TEB is excluded from the rewind because it is kernel-managed, but the
 * first 64 thread-local storage slots live inside it. Those are the game's own
 * data and have to come back, so they are lifted out and restored on their own
 * - 256 bytes that can matter more than every texture in the snapshot. */
#if defined(_M_IX86) || defined(__i386__)
#define TEB_TLS_SLOTS 0xE10
#else
#define TEB_TLS_SLOTS 0x1480
#endif
#define TLS_MINIMUM_SLOTS 64

typedef struct ThreadState {
	DWORD tid;
	DWORD pad[3];
	void *tls[TLS_MINIMUM_SLOTS];
	int have_tls;
	CONTEXT ctx __attribute__((aligned(16)));
} ThreadState;

/* The one piece of kernel state a rewind can genuinely put back.
 *
 * Rewinding memory restores the game's own record of where it is in a file, but
 * the file pointer the kernel holds does not move, so every later read comes
 * from the wrong offset. Nothing faults at the time; it faults when the garbage
 * is eventually parsed, which looks like a restore that worked and a crash
 * minutes afterwards.
 *
 * The path hash guards against a handle value being closed and reissued for a
 * different file between the save and the restore, in which case seeking it
 * would corrupt an unrelated read. */
typedef struct FileState {
	HANDLE h;
	long long pos;
	unsigned hash;
} FileState;

#define SS_MAX_FILES 1024

/* Both users of a handle's name run inside the suspended window, and on the load
 * path they run after the process heap has been put back. GetFinalPathNameByHandleA
 * allocates a wide scratch buffer from that heap to convert through, so it asked
 * a rewound allocator for memory: Rabbit and Steel died in
 * RtlpLowFragHeapAllocFromContext on a null bucket, reached from
 * GetFinalPathNameByHandleA on our own helper thread. The log's last line was the
 * phase immediately before this call and "files:" never printed.
 *
 * This is the rule the fault path below already keeps for the same reason, and
 * for the same API family - see the comment on GetModuleFileNameA reaching
 * RtlAllocateHeap. The save path simply never applied it here.
 *
 * NtQueryInformationFile writes into the caller's buffer and allocates nothing.
 * What it returns is the volume-relative name rather than the DOS path, which
 * suits both callers: one hashes it for identity across a save, the other wants
 * the basename. Losing the drive letter cannot make two different files hash
 * alike unless they also share a path on different volumes, and a handle whose
 * name we cannot read hashes to 0 and is then left alone rather than seeked. */
typedef struct {
	union {
		LONG Status;
		PVOID Pointer;
	} u;
	ULONG_PTR Information;
} SS_IOSB;

typedef LONG(NTAPI *PFN_NtQueryFile)(HANDLE, SS_IOSB *, PVOID, ULONG, ULONG);

static PFN_NtQueryFile query_file(void)
{
	/* Resolved once at init, never lazily. This static lives in our own data
	 * section, which travels with our heap and so is inside the snapshot at
	 * the default setting - a restore puts it back to whatever it held at the
	 * save. If that were NULL the next call would re-enter GetProcAddress,
	 * i.e. the loader, from inside the suspended window on a rewound heap:
	 * the very thing this function exists to avoid. Priming it at init keeps
	 * the restored value correct because it is the same value. */
	static PFN_NtQueryFile fn;
	if (!fn) {
		HMODULE nt = GetModuleHandleA("ntdll.dll");
		if (nt)
			fn = (PFN_NtQueryFile)(void *)GetProcAddress(
				nt, "NtQueryInformationFile");
	}
	return fn;
}

/* Characters written, 0 if the name could not be read. Narrowed to bytes: every
 * comparison and hash below is over the same narrowing, so a non-ASCII path is
 * still self-consistent. */
static DWORD file_name_of(HANDLE h, char *buf, DWORD max)
{
	PFN_NtQueryFile fn = query_file();
	struct {
		ULONG len;
		WCHAR name[512];
	} fni;
	SS_IOSB iosb;
	ULONG chars, i;
	if (!fn || max < 2)
		return 0;
	memset(&iosb, 0, sizeof(iosb));
	fni.len = 0;
	/* 9 = FileNameInformation. A negative status includes the overflow case,
	 * where a name longer than this buffer would still be partly written -
	 * treated as failure, matching what the old code did when the path did
	 * not fit, so a truncated name can never be hashed as a whole one. */
	if (fn(h, &iosb, &fni, sizeof(fni), 9) < 0)
		return 0;
	chars = fni.len / (ULONG)sizeof(WCHAR);
	if (chars > sizeof(fni.name) / sizeof(fni.name[0]))
		return 0;
	if (chars > max - 1)
		return 0;
	for (i = 0; i < chars; i++)
		buf[i] = fni.name[i] > 0x7F ? '?' : (char)fni.name[i];
	buf[chars] = 0;
	return chars;
}

static unsigned path_hash(HANDLE h)
{
	char buf[MAX_PATH];
	DWORD n = file_name_of(h, buf, sizeof(buf));
	unsigned hash = 2166136261u;
	DWORD i;
	if (n == 0)
		return 0;
	for (i = 0; i < n; i++) {
		hash ^= (unsigned char)buf[i];
		hash *= 16777619u;
	}
	return hash ? hash : 1;
}

/* Handle values are multiples of four and densely packed from the bottom, so a
 * bounded sweep finds them without pulling in undocumented query classes. The
 * process handle count says when to stop looking. */
static HANDLE g_logh;

static int ours_by_name(HANDLE h)
{
	char buf[MAX_PATH];
	DWORD n = file_name_of(h, buf, sizeof(buf));
	const char *base;
	if (n == 0)
		return 0;
	base = strrchr(buf, '\\');
	base = base ? base + 1 : buf;
	/* This wrapper's own log and state files must not be seeked back with the
	 * game's, and the d3d11 variant names them after itself, so recognising
	 * only the d3d9 prefix would hand our own log to the rewind. */
	return strncmp(base, "d3d9_sw", 7) == 0 || strncmp(base, "d3d11_sw", 8) == 0;
}

static int for_each_file(FileState *out, int max)
{
	DWORD total = 0;
	unsigned v;
	int found = 0, seen = 0;
	if (!GetProcessHandleCount(GetCurrentProcess(), &total))
		total = 4096;
	for (v = 4; v <= 0xFFFF && found < max && (DWORD)seen < total + 64; v += 4) {
		HANDLE h = (HANDLE)(uintptr_t)v;
		LARGE_INTEGER zero, pos;
		DWORD flags;
		if (!GetHandleInformation(h, &flags))
			continue;
		seen++;
		if (GetFileType(h) != FILE_TYPE_DISK)
			continue;
		/* Nothing this wrapper writes. Seeking our own logs back to where
		 * they were at the save makes every later line overwrite the
		 * account of the restore that wrote it, which is how "exclude"
		 * ends up spelled "clude". The trace and the frame CSV are ours
		 * too, and they are observation rather than game state. */
		if (h == g_logh || ours_by_name(h))
			continue;
		zero.QuadPart = 0;
		if (!SetFilePointerEx(h, zero, &pos, FILE_CURRENT))
			continue;
		out[found].h = h;
		out[found].pos = pos.QuadPart;
		out[found].hash = path_hash(h);
		found++;
	}
	return found;
}

static void ss_log(const char *fmt, ...);
static const char *ss_heap_of(uintptr_t at);
static int module_of(uintptr_t v);

/* Reporting a fault without allocating.
 *
 * The handler runs on whatever thread faulted, and one of the ways this game
 * dies is an access violation raised inside the allocator, with the heap's lock
 * already held by that same thread. Anything the handler does that allocates
 * then blocks on a lock it can never get, and the crash we were trying to
 * describe becomes a hang instead - which is exactly what happened, with
 * GetModuleFileNameA reaching RtlAllocateHeap from inside the report.
 *
 * So the fault path owns its formatting: a stack buffer, one WriteFile, and
 * module names read from the table gathered at the last save rather than asked
 * of the loader. It handles only the conversions used below. */
static void ss_raw(const char *fmt, ...);
static void fmt_report_for(DWORD tid);
static const char *ss_module(uintptr_t v, unsigned *off);
static int region_excluded(uintptr_t base, uintptr_t size);
#if !defined(_M_IX86) && !defined(__i386__)
static void ss_where_reg(const char *name, uintptr_t v);
#endif

/* Reads a setting from the process environment, falling back to a d3d9_sw.cfg
 * file beside the log. Declared here because the first knob is read long before
 * the control block is defined.
 *
 * The file exists because the environment is not reliably ours to set. A game
 * started from a launcher inherits that launcher's environment, not the one in
 * the shell where the setting was typed, and there is no way to tell the two
 * apart from inside the process - a knob that was never delivered looks exactly
 * like a knob that was delivered and ignored. That ambiguity cost a whole OSFE
 * sitting: a run intended to test one setting silently tested the default, and
 * the only reason it was caught afterwards was that the setting happens to
 * change two other numbers in the log.
 *
 * Same reasoning the mono gc reporting below already uses, generalised: make
 * the setting reachable by a route the user controls, and then say out loud
 * what was actually read. */
static DWORD ss_getenv(const char *name, char *buf, DWORD cap);

/* Who called, read off the stack rather than unwound.
 *
 * A divide by zero inside ntdll says the allocator found a zero where a block
 * size belonged, but not which heap it was serving or who asked. There are no
 * symbols here and no unwind tables worth trusting across a frame-pointer-free
 * system DLL, so this does the crude thing: it reads words off the stack and
 * keeps the ones that point into executable image memory. Some will be stale
 * leftovers rather than live return addresses, but the sequence of module names
 * is enough to tell a caller in the game from one in a library. */
/* True for an address that is committed and executable. Returns 2 for a mapped
 * image and 1 for private executable memory, so a caller can tell a library
 * frame from a JIT-compiled one.
 *
 * This used to demand MEM_IMAGE, and in a process built around a JIT that is
 * simply wrong: Mono emits managed code into PRIVATE committed pages, which are
 * executable and are genuine code but are not a mapped image and never will be.
 * The effect was that the stack walk stopped at the first managed frame EVERY
 * time and reported it as "NOT EXECUTABLE CODE", which reads as corruption and
 * is nothing of the kind. A real game fault stopped at frame 2 that way and the
 * address was ordinary JIT code. A diagnostic that mislabels the normal case as
 * damage is worse than no diagnostic. */
static int ss_is_code(uintptr_t v)
{
	MEMORY_BASIC_INFORMATION mbi;
	const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
			   PAGE_EXECUTE_WRITECOPY;

	if (v < 0x10000)
		return 0;
	if (VirtualQuery((LPCVOID)v, &mbi, sizeof(mbi)) != sizeof(mbi))
		return 0;
	if (mbi.State != MEM_COMMIT || (mbi.Protect & exec) == 0)
		return 0;
	return mbi.Type == MEM_IMAGE ? 2 : 1;
}

static int ss_readable(uintptr_t p, size_t n);

#if !defined(_M_IX86) && !defined(__i386__)
#ifndef UNW_FLAG_NHANDLER
#define UNW_FLAG_NHANDLER 0
#endif

/* A real unwind, using the same tables the operating system uses to dispatch an
 * exception, in place of guessing.
 *
 * The guess is what this replaced, and it had to go because it produced a
 * confident wrong answer. The old version swept 2048 bytes of stack and printed
 * every word that pointed into executable image memory, in stack-address order.
 * That prints dead return addresses from calls that returned long ago
 * indistinguishably from live ones, in an order that is not call order, and it
 * did exactly that: a chain naming mono-2.0-bdwgc.dll between two ucrtbase
 * frames, from which a whole theory was built about Mono calling the C runtime -
 * a thing Mono's import table shows it cannot do. Three cycles have now been
 * spent on stories whose only support was this function.
 *
 * The frames it prints are ordered, real, and each one genuinely called the next.
 * The cost is that it stops early rather than inventing: a function with no
 * unwind data, or a corrupted table, ends the walk. A SHORT UNWOUND CHAIN MEANS
 * THE WALK STOPPED, NOT THAT THE STACK WAS SHALLOW - the two are
 * indistinguishable from the outside, so neither reading is available and both
 * must be resisted.
 *
 * Deliberate irony worth recording: unwinding JIT-compiled code needs
 * RtlLookupFunctionEntry to find a growable function table, which is the exact
 * structure the rewind boundary bisects and which this project hooks and
 * reconciles. So Mono frames are the ones least likely to resolve here, and a
 * walk that dies on entering Mono is itself a signal.
 *
 * Every dereference is checked because this runs inside a fault handler, where
 * assuming anything about memory is how a diagnostic becomes a second crash. */
static void ss_unwind(const CONTEXT *c)
{
	CONTEXT ctx = *c;
	int depth;

	for (depth = 0; depth < 24; depth++) {
		PRUNTIME_FUNCTION fn;
		ULONG64 base = 0, establisher = 0;
		PVOID hdata = NULL;
		uintptr_t prev_sp = (uintptr_t)ctx.Rsp;
		const char *name;
		unsigned off;

		if (!ss_is_code((uintptr_t)ctx.Rip)) {
			/* Not a report of failure - it is the most interesting line the
			 * walk can produce. Control is at an address that is not code,
			 * which is what the half-pointer faults look like. */
			ss_raw("       frame %d  %p  NOT EXECUTABLE CODE - the walk stops "
			       "here\n",
			       depth, (void *)(uintptr_t)ctx.Rip);
			return;
		}
		if (ss_is_code((uintptr_t)ctx.Rip) == 1) {
			/* Executable, committed, but private rather than a mapped image:
			 * JIT-compiled managed code. Ordinary, and previously reported as
			 * "NOT EXECUTABLE CODE" - the loudest possible way to say
			 * "everything is fine here".
			 *
			 * The walk still stops, but for the honest reason: RtlVirtualUnwind
			 * needs a function table, and Mono publishes those dynamically
			 * through RtlAddGrowableFunctionTable. This engine already tracks
			 * that registration, and the game's own log shows tables coming
			 * back with changed entry counts that are deliberately left alone -
			 * so a managed frame is exactly where the walk should be expected
			 * to end, and saying so is worth more than a wrong label. */
			ss_raw("       frame %d  %p  JIT-COMPILED CODE (private executable "
			       "memory, not a mapped image) - normal, and the walk stops "
			       "here because managed frames need Mono's dynamic unwind "
			       "tables\n",
			       depth, (void *)(uintptr_t)ctx.Rip);
			return;
		}
		name = ss_module((uintptr_t)ctx.Rip, &off);
		ss_raw("       frame %d  %p  %s+%X\n", depth, (void *)(uintptr_t)ctx.Rip,
		       name ? name : "unknown", off);

		fn = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
		if (!fn) {
			/* A leaf function has no unwind data by design: nothing is pushed
			 * beyond the return address, so [rsp] is the caller. Applied once
			 * only - guessing this repeatedly is the old scan again. */
			if (depth || !ss_readable((uintptr_t)ctx.Rsp, sizeof(ULONG64))) {
				ss_raw("       frame %d has no unwind data, so the walk stops "
				       "(NOT a shallow stack)\n",
				       depth);
				return;
			}
			ctx.Rip = *(const ULONG64 *)(uintptr_t)ctx.Rsp;
			ctx.Rsp += sizeof(ULONG64);
			continue;
		}
		RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &hdata, &establisher,
				 NULL);
		if (!ctx.Rip)
			return; /* the bottom of the thread, reached properly */
		/* A frame that does not move the stack pointer would spin forever. */
		if ((uintptr_t)ctx.Rsp <= prev_sp) {
			ss_raw("       the unwind stopped making progress at %p\n",
			       (void *)(uintptr_t)ctx.Rsp);
			return;
		}
	}
	ss_raw("       (unwind truncated at 24 frames)\n");
}
#endif

/* Kept, but demoted and relabelled. This is a GUESS: words on the stack that
 * could be code addresses, including ones belonging to calls that returned long
 * ago. It is printed after the real unwind, and only because when the unwind
 * stops early these words are the only thing left - as leads to check, never as
 * a call chain. The label on every line now says so. */
static void ss_callers(const CONTEXT *c)
{
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t sp, p, top;
	const char *name;
	unsigned off;
	int shown = 0;

	if (!c)
		return;
#if !defined(_M_IX86) && !defined(__i386__)
	ss_unwind(c);
#endif
#if defined(_M_IX86) || defined(__i386__)
	sp = (uintptr_t)c->Esp;
#else
	sp = (uintptr_t)c->Rsp;
#endif
	if (!sp || VirtualQuery((LPCVOID)sp, &mbi, sizeof(mbi)) != sizeof(mbi) ||
	    mbi.State != MEM_COMMIT)
		return;
	top = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
	if (top > sp + 2048)
		top = sp + 2048;

	for (p = sp; p + sizeof(void *) <= top && shown < 8; p += sizeof(void *)) {
		uintptr_t v = *(const uintptr_t *)p;

		if (!ss_is_code(v))
			continue;
		name = ss_module(v, &off);
		ss_raw("       stack-scan GUESS (may be a dead frame) %p  %s+%X\n", (void *)v,
		       name ? name : "unknown", off);
		shown++;
	}
}

/* Making the clock go back with everything else.
 *
 * The game reads wall time from QueryPerformanceCounter, GetTickCount and
 * timeGetTime. A rewind restores its record of when things happened but cannot
 * move the actual clock, so the first frame after a restore sees a jump of
 * however long play continued past the save - which is a frame delta of
 * seconds, fed into logic that expects sixteen milliseconds.
 *
 * These are reachable in a way ntdll's heap was not: the game calls them
 * through its own import table, so redirecting its entries touches nothing else
 * in the process. Steam and the system keep real time; only the game's view is
 * shifted, by an offset chosen at each restore to make its clock continue from
 * the moment of the save. */
typedef struct TimeBase {
	LONGLONG qpc;
	DWORD tick, tgt;
} TimeBase;

/* The events the game creates, and nothing else's.
 *
 * Two of the three game threads sit in an ntdll wait during normal play, so the
 * signalled state of those events is real simulation state that a memory rewind
 * cannot reach. Restoring it wrongly is worse than not restoring it: reset an
 * event the game had set and the waiter sleeps through work its rewound memory
 * says is pending, which is a hang rather than a crash.
 *
 * Scope comes from watching CreateEventA in the game's import table rather than
 * sweeping the handle table, because a swept handle could belong to Steam or the
 * loader, and quietly resetting one of those is how you deadlock a process that
 * was otherwise fine. */
#define SS_MAX_EVENTS 512

typedef struct EventState {
	HANDLE h;
	int signalled;
} EventState;

typedef struct FaultRec {
	DWORD code, tid;
	uintptr_t pc, at;
	LONG frame;
} FaultRec;

/* Lives outside the snapshot so a rewind cannot erase the history of what went
 * wrong before it. */
typedef struct EventTrack {
	volatile LONG n;
	HANDLE h[SS_MAX_EVENTS];
	volatile LONG nfault;
	FaultRec fault[32];
	LONG npark;
	uintptr_t park[32];
	volatile LONG ndbg;
	char dbg[8][160];
} EventTrack;

static EventTrack *g_events;

/* System threads park in the same handful of wait stubs every time, so printing
 * all of them on every save buries the log. Only an address never seen before is
 * worth a line - that is the one that would mean a thread was caught somewhere
 * it cannot safely be left, like inside the allocator. */
static int park_is_new(uintptr_t pc)
{
	int i;
	if (!g_events)
		return 1;
	for (i = 0; i < g_events->npark && i < 32; i++)
		if (g_events->park[i] == pc)
			return 0;
	if (g_events->npark >= 32)
		return 0;
	g_events->park[g_events->npark++] = pc;
	return 1;
}

static EventTrack *g_events;
static HANDLE(WINAPI *g_real_createevent)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR);
static int g_hooked_time, g_hooked_event;

static HANDLE WINAPI hook_createevent(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL init,
				      LPCSTR name)
{
	HANDLE h = g_real_createevent(sa, manual, init, name);
	if (h && g_events) {
		LONG i = InterlockedIncrement(&g_events->n) - 1;
		if (i >= 0 && i < SS_MAX_EVENTS)
			g_events->h[i] = h;
	}
	return h;
}

/* Called with every game thread suspended. A zero wait is the only way to read a
 * signalled bit, and on an auto-reset event it consumes what it read, so it goes
 * straight back. On a manual-reset event the set is what was already true. */
static int event_probe(HANDLE h)
{
	if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0)
		return 0;
	SetEvent(h);
	return 1;
}

static int events_capture(EventState *out, int max)
{
	int n = 0, i, cnt;
	DWORD flags;
	if (!g_events)
		return 0;
	cnt = (int)g_events->n;
	if (cnt > SS_MAX_EVENTS)
		cnt = SS_MAX_EVENTS;
	for (i = 0; i < cnt && n < max; i++) {
		HANDLE h = g_events->h[i];
		if (!h || !GetHandleInformation(h, &flags))
			continue; /* closed since it was created */
		out[n].h = h;
		out[n].signalled = event_probe(h);
		n++;
	}
	return n;
}

static LONGLONG g_qpc_off;
static DWORD g_tick_off, g_tgt_off;
static BOOL(WINAPI *g_real_qpc)(LARGE_INTEGER *);
static DWORD(WINAPI *g_real_tick)(void);
static DWORD(WINAPI *g_real_tgt)(void);

static BOOL WINAPI hook_qpc(LARGE_INTEGER *p)
{
	LARGE_INTEGER v;
	if (!g_real_qpc(&v))
		return FALSE;
	p->QuadPart = v.QuadPart - g_qpc_off;
	return TRUE;
}

static DWORD WINAPI hook_tick(void)
{
	return g_real_tick() - g_tick_off;
}

static DWORD WINAPI hook_tgt(void)
{
	return g_real_tgt() - g_tgt_off;
}

static void time_now(TimeBase *t)
{
	LARGE_INTEGER v;
	v.QuadPart = 0;
	if (g_real_qpc)
		g_real_qpc(&v);
	else
		QueryPerformanceCounter(&v);
	t->qpc = v.QuadPart - g_qpc_off;
	t->tick = (g_real_tick ? g_real_tick() : GetTickCount()) - g_tick_off;
	t->tgt = (g_real_tgt ? g_real_tgt() : 0) - g_tgt_off;
}

/* Rechooses the offsets so the game's clock reads what it read at the save.
 *
 * Off by default, because measurement said so. Long rewinds worked reliably
 * while the clock ran forward, and started dying once it was wound back, with a
 * six second jump surviving and a two minute one not. Patching the executable's
 * imports only moves the game's own clock; middleware it hands a timestamp to
 * still reads real time, so a long rewind leaves the game looking minutes behind
 * to code that was never rewound. A big frame delta after a restore turned out
 * to be a problem this game does not have, and this was a cure for it. */
static void time_rewind(const TimeBase *t)
{
	static int enabled = -1;
	LARGE_INTEGER v;
	DWORD back;
	if (!g_real_qpc)
		return;
	if (enabled < 0) {
		char c[8];
		DWORD got = ss_getenv("D3D9SW_REWIND_CLOCK", c, sizeof(c));
		enabled = (got > 0 && got < sizeof(c) && c[0] == '1');
	}
	back = (g_real_tick() - g_tick_off) - t->tick;
	if (!enabled) {
		ss_log("  clock: left running, %.1f s ahead of the save\n", back / 1000.0);
		return;
	}
	g_real_qpc(&v);
	g_qpc_off = v.QuadPart - t->qpc;
	g_tick_off = g_real_tick() - t->tick;
	if (g_real_tgt)
		g_tgt_off = g_real_tgt() - t->tgt;
	ss_log("  clock: wound back %.1f s\n", back / 1000.0);
}

/* Redirects one imported function wherever the module refers to it. Matching on
 * the resolved address rather than the name catches every descriptor without
 * having to care which library the loader decided it came from. */
static int patch_iat(HMODULE mod, void *from, void *to)
{
	unsigned char *base = (unsigned char *)mod;
	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
	IMAGE_NT_HEADERS *nt;
	IMAGE_IMPORT_DESCRIPTOR *imp;
	DWORD rva;
	int n = 0;

	if (!mod || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return 0;
	nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return 0;
	rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
	if (!rva)
		return 0;
	for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva); imp->Name; imp++) {
		void **thunk = (void **)(base + imp->FirstThunk);
		for (; *thunk; thunk++) {
			DWORD old;
			if (*thunk != from)
				continue;
			if (!VirtualProtect(thunk, sizeof(void *), PAGE_READWRITE, &old))
				continue;
			*thunk = to;
			VirtualProtect(thunk, sizeof(void *), old, &old);
			n++;
		}
	}
	return n;
}

static void ss_exclude(void *p, size_t n);

#if !defined(_M_IX86) && !defined(__i386__)
/* Replaces a resolved function pointer wherever a module keeps a copy of it in
 * its own writable data.
 *
 * patch_iat is no use for an API a module resolved with GetProcAddress rather
 * than imported, and that is the normal arrangement for anything that only
 * exists on newer Windows than the module targets. The first attempt at hooking
 * Mono's unwind-table calls patched the import table and reported "0
 * unwind-table import(s)" - a fix that silently did nothing, which is the worst
 * kind. The three name strings sit in Mono's .rdata as GetProcAddress arguments,
 * so the pointers themselves are in its globals.
 *
 * Matching on the address rather than the name, for the same reason patch_iat
 * does: it finds every copy without having to know how many the module keeps. */
static int patch_data_ptr(HMODULE mod, void *from, void *to)
{
	unsigned char *base = (unsigned char *)mod;
	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
	IMAGE_NT_HEADERS *nt;
	IMAGE_SECTION_HEADER *sec;
	unsigned i;
	int n = 0;

	if (!mod || !from || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return 0;
	nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return 0;
	sec = IMAGE_FIRST_SECTION(nt);
	for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
		void **p, **end;

		if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE))
			continue;
		p = (void **)(base + sec->VirtualAddress);
		end = (void **)(base + sec->VirtualAddress + sec->Misc.VirtualSize);
		for (; p + 1 <= end; p++) {
			DWORD old;

			if (*p != from)
				continue;
			if (!VirtualProtect(p, sizeof(void *), PAGE_READWRITE, &old))
				continue;
			*p = to;
			VirtualProtect(p, sizeof(void *), old, &old);
			n++;
		}
	}
	return n;
}

/* Mono's JIT registers unwind information for every chunk of code it emits, and
 * ntdll keeps those registrations in a list threaded through heap blocks whose
 * head is a global inside ntdll's own image.
 *
 * That is a data structure with one end on each side of the rewind boundary, and
 * it was killing the process. Measured in a dump rather than reasoned about:
 * after a restore, ntdll!RtlpDynamicFunctionTable's Blink pointed at
 * 0000021780879470, which !heap -x reported as an LFH FREE block in the game's
 * C-runtime heap and which read as all zeroes. The next method Unity called that
 * had not been compiled yet went through mini_method_compile ->
 * mono_arch_unwindinfo_insert_range_in_table -> RtlAddGrowableFunctionTable,
 * whose InsertTailList guard found tail->Flink was not the head and issued
 * __fastfail(FAST_FAIL_CORRUPT_LIST_ENTRY). Silent, because int 29h goes only to
 * a debugger. Four different heap policies failed to shift it because the head
 * is not in any heap - it is in ntdll, which we hold under all of them.
 *
 * So this list is repaired through ntdll's own API instead of by rewinding
 * bytes: every registration is recorded here, and a restore puts the registered
 * set back to what it was when the snapshot was taken. Going through the API
 * also keeps the AVL tree ntdll maintains alongside the list (TreeMin/TreeMax,
 * built by RtlAvlInsertNodeEx) consistent, which rewinding the head would not.
 *
 * Records live outside the snapshot, so they describe the present even as the
 * memory they describe is replaced. */
enum { SS_MAX_FNTAB = 4096 };

typedef struct {
	void *handle; /* what ntdll returned, and what Delete takes back */
	void **out;   /* where the caller stored it, so a replay can correct it */
	void *funcs;
	DWORD count, max;
	ULONG_PTR base, end;
	char live; /* registered with ntdll at this moment */
	/* The same, as it stood when the snapshot was taken. */
	char was_live;
	DWORD was_count;
} FnTab;

typedef struct FnTabs {
	volatile LONG n;
	volatile LONG lock;
	LONG removed, readded, failed, drifted, skipped, unstuck;
	FnTab t[SS_MAX_FNTAB];
} FnTabs;

static FnTabs *g_fntabs;
static DWORD(WINAPI *g_real_fnadd)(void **, void *, DWORD, DWORD, ULONG_PTR, ULONG_PTR);
static void(WINAPI *g_real_fngrow)(void *, DWORD);
static void(WINAPI *g_real_fndel)(void *);
/* Counted per entry point rather than in total, because Add without Delete is a
 * partial hook that would look like it worked and then leak stale tables. */
static int g_hooked_fntab, g_hooked_add, g_hooked_grow, g_hooked_del;

/* Mono compiles on more than one thread, so registration is concurrent. A spin
 * rather than a critical section because this runs inside the JIT's own path.
 *
 * Bounded, and it gives up rather than waiting. An unbounded spin here is a
 * deadlock waiting to happen: the section is not the "few stores" an earlier
 * version of this comment claimed, because fntab_find is a linear scan and the
 * table is past two hundred entries, and a holder can stop making progress for
 * reasons that have nothing to do with contention - see fntab_unstick.
 *
 * Giving up costs one unrecorded registration, which makes the reconciliation
 * miss one table. Waiting costs the process. */
static int fntab_lock(void)
{
	int spins = 0;

	while (InterlockedCompareExchange(&g_fntabs->lock, 1, 0)) {
		if (++spins > 200000) {
			InterlockedIncrement(&g_fntabs->skipped);
			return 0;
		}
		YieldProcessor();
	}
	return 1;
}

static void fntab_unlock(void)
{
	InterlockedExchange(&g_fntabs->lock, 0);
}

static FnTab *fntab_find(void *handle)
{
	LONG i;

	for (i = 0; i < g_fntabs->n; i++)
		if (g_fntabs->t[i].handle == handle)
			return &g_fntabs->t[i];
	return NULL;
}

static DWORD WINAPI hook_fnadd(void **out, void *funcs, DWORD count, DWORD max,
			       ULONG_PTR base, ULONG_PTR end)
{
	DWORD rc = g_real_fnadd(out, funcs, count, max, base, end);

	if (rc == 0 && g_fntabs && out && *out && fntab_lock()) {
		{
			/* Handles are heap blocks and get recycled, so an address we
			 * have seen before is a new table reusing it. */
			FnTab *e = fntab_find(*out);

			if (!e && g_fntabs->n < SS_MAX_FNTAB)
				e = &g_fntabs->t[g_fntabs->n++];
			if (e) {
				e->handle = *out;
				e->out = out;
				e->funcs = funcs;
				e->count = count;
				e->max = max;
				e->base = base;
				e->end = end;
				e->live = 1;
			}
		}
		fntab_unlock();
	}
	return rc;
}

static void WINAPI hook_fngrow(void *handle, DWORD count)
{
	if (g_fntabs && fntab_lock()) {
		{
			FnTab *e = fntab_find(handle);
			if (e)
				e->count = count;
		}
		fntab_unlock();
	}
	g_real_fngrow(handle, count);
}

static void WINAPI hook_fndel(void *handle)
{
	if (g_fntabs && fntab_lock()) {
		{
			FnTab *e = fntab_find(handle);
			if (e)
				e->live = 0;
		}
		fntab_unlock();
	}
	g_real_fndel(handle);
}

/* Called with every thread suspended, so this is the registered set at the
 * instant of the snapshot. */
static void fntab_note_save(void)
{
	LONG i;

	if (!g_fntabs)
		return;
	for (i = 0; i < g_fntabs->n; i++) {
		g_fntabs->t[i].was_live = g_fntabs->t[i].live;
		g_fntabs->t[i].was_count = g_fntabs->t[i].count;
	}
}

/* Off returns to the behaviour before any of this existed: tables are still
 * recorded, and the counts still reported, but a restore leaves ntdll's list
 * alone. Kept because the first working version of the reconciliation hung the
 * restore, and being able to get a completing restore back in one line of
 * config is worth more than the tidiness of deleting the switch. */
static int fntab_mode(void)
{
	static int v = -1;
	if (v < 0) {
		char b[16];
		DWORD n = ss_getenv("D3D9SW_FNTAB", b, sizeof(b));
		v = (n && n < sizeof(b)) ? atoi(b) : 1;
	}
	return v;
}

/* Half of the reconciliation, and it has to run BEFORE a single byte of memory
 * is put back.
 *
 * Deleting a table makes ntdll free its heap block. Done after the restore, that
 * hands a block back to a heap the rewind has already returned to a state where
 * the block is free - a double free, and a fresh corruption in place of the one
 * being fixed. Done here, the block goes back to the present-day heap that is
 * about to be discarded wholesale, which costs nothing.
 *
 * Only tables registered since the snapshot are removed. The first version also
 * removed every table whose entry count had changed, intending to re-register it
 * at the saved count, and that is what hung three restores in a row: Mono grows
 * tables constantly, so nearly all 112-odd qualified, and re-registering them
 * meant a hundred heap allocations after the restore - see fntab_reconcile_up.
 * A count left too high describes functions that no longer exist, in an array
 * whose bytes the restore put back, and only misleads an unwind that walks that
 * exact range. A hang is certain and that is a maybe, so the count is now left
 * alone and merely counted. */
static void fntab_reconcile_down(void)
{
	LONG i;

	if (!g_fntabs || !fntab_mode())
		return;
	g_fntabs->removed = 0;
	g_fntabs->readded = 0;
	g_fntabs->failed = 0;
	g_fntabs->drifted = 0;
	for (i = 0; i < g_fntabs->n; i++) {
		FnTab *e = &g_fntabs->t[i];

		if (!e->live)
			continue;
		if (e->was_live) {
			if (e->count != e->was_count)
				g_fntabs->drifted++;
			continue;
		}
		g_real_fndel(e->handle);
		e->live = 0;
		g_fntabs->removed++;
	}
	/* Written before the restore rather than with the totals afterwards, so a
	 * restore that never returns still says how much work it was given. */
	ss_log("  unwind tables: %ld deregistered before the restore, %ld tracked, "
	       "%ld with a changed count left as they are\n",
	       (long)g_fntabs->removed, (long)g_fntabs->n, (long)g_fntabs->drifted);
}

/* Forces our own record lock free, and must run after every restore whether the
 * reconciliation is enabled or not.
 *
 * The records deliberately sit outside the snapshot, so that a rewind cannot
 * take away the description of what the rewind is about to do. The lock word
 * sits there with them - and that is the trap. A Mono thread suspended inside
 * hook_fnadd holding this lock has its instruction pointer put back to save
 * time by the restore, so it never reaches the unlock, while the lock word it
 * set is in memory the restore does not touch. Held forever, by a thread that
 * no longer believes it ever acquired it, and every later JIT registration
 * queues behind it: the process wedges with no fault and no crash.
 *
 * Safe to do unconditionally, because every thread is suspended at this point,
 * so there is no holder that could still be making progress. This is the same
 * rule that applies to the game's locks - the correct value of any lock after a
 * restore is free - applied to one of ours. */
static void fntab_unstick(void)
{
	if (!g_fntabs)
		return;
	if (InterlockedExchange(&g_fntabs->lock, 0)) {
		g_fntabs->unstuck++;
		ss_log("  unwind tables: record lock was held across the restore and has "
		       "been forced free; without this the next JIT registration would "
		       "have spun forever\n");
	}
	if (g_fntabs->skipped)
		ss_log("  unwind tables: %ld registration(s) went unrecorded after the lock "
		       "refused to be taken\n",
		       (long)g_fntabs->skipped);
}

/* The other half, after the restore: anything the snapshot had registered and
 * that Mono has dropped since goes back.
 *
 * This is the dangerous end of the mechanism and the reason it is now kept as
 * small as possible. RtlAddGrowableFunctionTable allocates, every thread is
 * suspended, and the heap it allocates from has just had its lock and its free
 * lists reverted to whatever they were at the save. If the save caught that lock
 * held by a thread that is currently suspended, the first allocation waits for a
 * release that cannot come until we resume, and we do not resume until this
 * returns. That is a frozen process with no fault and a log that stops in the
 * middle of the load, which is exactly how the first version failed.
 *
 * Mono rarely discards compiled code, so in the ordinary case this does nothing
 * at all and never touches the allocator. The count is logged either way, so a
 * restore that stops here can be told apart from one that had nothing to do.
 *
 * The new handle is written back into the caller's own variable, because that
 * variable lives in the rewound heap and now holds the handle from the save.
 * Left alone, Mono would eventually pass that stale value to
 * RtlDeleteGrowableFunctionTable and we would be back where we started. */
static void fntab_reconcile_up(void)
{
	LONG i, want = 0;

	if (!g_fntabs || !fntab_mode())
		return;
	for (i = 0; i < g_fntabs->n; i++)
		if (!g_fntabs->t[i].live && g_fntabs->t[i].was_live)
			want++;
	if (!want) {
		ss_log("  unwind tables: nothing to put back\n");
		return;
	}
	ss_log("  unwind tables: putting %ld back, which allocates\n", (long)want);
	for (i = 0; i < g_fntabs->n; i++) {
		FnTab *e = &g_fntabs->t[i];
		void *h = NULL;

		if (e->live || !e->was_live)
			continue;
		if (g_real_fnadd(&h, e->funcs, e->was_count, e->max, e->base, e->end) != 0 ||
		    !h) {
			g_fntabs->failed++;
			continue;
		}
		e->handle = h;
		e->count = e->was_count;
		e->live = 1;
		if (e->out)
			*e->out = h;
		g_fntabs->readded++;
	}
	ss_log("  unwind tables: %ld put back, %ld could not be re-registered\n",
	       (long)g_fntabs->readded, (long)g_fntabs->failed);
}

/* Redirects the three ntdll entry points Mono uses for this. All three are
 * needed: without Grow, a table extended in place after the save would be put
 * back at the wrong count; without Delete, a table Mono dropped between save and
 * restore would look live and never be re-registered.
 *
 * Both the import table and the module's data are searched. Mono resolves these
 * with GetProcAddress, so in practice the data search is the one that finds
 * them, but the cost of trying both is one walk of a table that is usually
 * empty of them, and a different Mono build could do it either way.
 *
 * Retried on every save because Mono resolves lazily: patching before it has
 * called GetProcAddress would simply be overwritten by the result. */
static void fntab_hook(void)
{
	HMODULE nt = GetModuleHandleA("ntdll.dll");
	HMODULE mono = GetModuleHandleA("mono-2.0-bdwgc.dll");

	if (!mono)
		mono = GetModuleHandleA("mono-2.0-sgen.dll");
	if (!mono)
		mono = GetModuleHandleA("mono.dll");
	if (!nt || !mono || !g_fntabs)
		return;
	if (!g_real_fnadd) {
		g_real_fnadd = (DWORD(WINAPI *)(void **, void *, DWORD, DWORD, ULONG_PTR,
						ULONG_PTR))(void *)
			GetProcAddress(nt, "RtlAddGrowableFunctionTable");
		g_real_fngrow = (void(WINAPI *)(void *, DWORD))(void *)GetProcAddress(
			nt, "RtlGrowFunctionTable");
		g_real_fndel = (void(WINAPI *)(void *))(void *)GetProcAddress(
			nt, "RtlDeleteGrowableFunctionTable");
	}
	if (!g_real_fnadd || !g_real_fngrow || !g_real_fndel)
		return;
	g_hooked_fntab += patch_iat(mono, (void *)g_real_fnadd, (void *)hook_fnadd);
	g_hooked_fntab += patch_iat(mono, (void *)g_real_fngrow, (void *)hook_fngrow);
	g_hooked_fntab += patch_iat(mono, (void *)g_real_fndel, (void *)hook_fndel);
	g_hooked_add += patch_data_ptr(mono, (void *)g_real_fnadd, (void *)hook_fnadd);
	g_hooked_grow += patch_data_ptr(mono, (void *)g_real_fngrow, (void *)hook_fngrow);
	g_hooked_del += patch_data_ptr(mono, (void *)g_real_fndel, (void *)hook_fndel);
	g_hooked_fntab += g_hooked_add + g_hooked_grow + g_hooked_del;
}

/* Allocated eagerly, before Mono is necessarily loaded, because the exclusion
 * list is fixed at start-up: a later allocation would be excluded once and then
 * dropped the next time the list was rebuilt, and these records rewinding
 * underneath us is the exact failure they exist to prevent. */
static void fntab_alloc(void)
{
	if (!g_fntabs)
		g_fntabs = (FnTabs *)VirtualAlloc(NULL, sizeof(FnTabs),
						  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static void fntab_exclude(void)
{
	if (g_fntabs)
		ss_exclude(g_fntabs, sizeof(FnTabs));
}

/* Reported at every save, because a partial hook is the dangerous state: Add
 * without Delete would record tables and never notice them going away, and the
 * reconciliation would put back registrations Mono had deliberately dropped. */
static void fntab_report(void)
{
	/* All three zero is the benign case and used to be reported as the alarming
	 * one. These hooks work by finding the resolved pointers in a module's data,
	 * so zero of all three means nothing in the process ever resolved them -
	 * i.e. there is no JIT here, no growable function tables exist, and there is
	 * nothing for a restore to repair. Rabbit and Steel is not a Unity game and
	 * has no Mono at all, and it got a warning naming Mono by name every save.
	 *
	 * Partial is the dangerous state and is still worth shouting about: Add
	 * without Delete would record tables and never notice them going away, and
	 * the reconciliation would put back registrations the JIT deliberately
	 * dropped. */
	if (!g_hooked_add && !g_hooked_grow && !g_hooked_del) {
		ss_log("  unwind tables: nothing in this process has resolved ntdll's "
		       "growable-function-table calls, so there is no JIT unwind state "
		       "to repair\n");
		return;
	}
	if (!g_fntabs || !g_hooked_add || !g_hooked_grow || !g_hooked_del) {
		ss_log("  WARNING: the unwind-table calls are only PARTLY hooked "
		       "(add %d, grow %d, delete %d), so a restore cannot reliably repair "
		       "ntdll's dynamic function table list\n",
		       g_hooked_add, g_hooked_grow, g_hooked_del);
		return;
	}
	ss_log("  unwind tables: %ld registered at the snapshot, %d pointer(s) hooked%s\n",
	       (long)g_fntabs->n, g_hooked_fntab,
	       fntab_mode() ? "" : ", reconciliation OFF by D3D9SW_FNTAB=0");
}

/* Called every frame.
 *
 * Mono resolves these with GetProcAddress at the point of first use, so there is
 * no single moment at which hooking is guaranteed to work: too early and the
 * resolution overwrites us, and the only way to know we are too early is that
 * nothing matched. So keep trying. Patching is idempotent - a slot already
 * holding our hook no longer matches the address being searched for - so a
 * repeated attempt either finds something new or does nothing at all.
 *
 * The scan is a pointer-wide compare over Mono's writable sections, a few tens
 * of thousands of comparisons, which is affordable per frame while it is still
 * finding things and not worth paying forever once it has. After all three land
 * it drops to roughly once a second, which is there purely to catch Mono
 * re-resolving them. */
static void fntab_keep_hooked(void)
{
	static unsigned tick;
	int settled = g_hooked_add && g_hooked_grow && g_hooked_del;

	if (settled && (++tick & 15u))
		return;
	fntab_hook();
}
#else
static void fntab_note_save(void)
{
}
static void fntab_reconcile_down(void)
{
}
static void fntab_reconcile_up(void)
{
}
static void fntab_unstick(void)
{
}
static void fntab_hook(void)
{
}
static void fntab_alloc(void)
{
}
static void fntab_exclude(void)
{
}
static void fntab_report(void)
{
}
static void fntab_keep_hooked(void)
{
}
#endif

/* Locks are not state worth keeping.
 *
 * A critical section captured while some thread held it comes back held. If that
 * thread's context was restored too it will resume inside the section and
 * release it, and nothing is wrong. If it was not - because it exited between
 * the save and the restore, or was created after the save - the section is held
 * forever by a thread that has no idea it owns it, and every later entry takes
 * ntdll's contended path.
 *
 * That path is where the harness dies, in about six seconds, with two or more
 * threads compiling: an access violation reading NULL at
 * ntdll!RtlpEnterCriticalSectionContended, reached from
 * RtlEnterCriticalSection+0xf2 inside Mono, zero frames after the restore. The
 * jit-info oracle passes on the same restore, so the memory the section
 * describes is consistent - it is the section itself that is wrong. With one
 * compiling thread the same run survives a hundred cycles, which is what
 * "contended" in that function name says out loud.
 *
 * It is also the most plausible account of the OSFE hang where all 118 threads
 * sat in NtWaitForSingleObject with nobody runnable.
 *
 * So sections are recorded as they are created and inspected after every
 * restore. What is deliberately NOT done is resetting all of them: a section
 * whose owner is coming back must be left exactly as the snapshot had it, or the
 * owner's LeaveCriticalSection underflows a section we have just declared free
 * and we have replaced one corruption with another. Only sections whose owner
 * will not resume are reinitialised, which is the same argument as the unwind
 * tables - repair the half the rewind cannot describe, and touch nothing else. */
enum { SS_MAX_CS = 16384 };

typedef struct {
	CRITICAL_SECTION *cs;
	DWORD spin;
	char live;
} CsRec;

typedef struct CsRecs {
	volatile LONG n;
	volatile LONG lock;
	volatile LONG overflow, skipped;
	LONG held, reset, kept, unreadable, outside;
	CsRec t[SS_MAX_CS];
} CsRecs;

static CsRecs *g_cs;
static void(WINAPI *g_real_cs_init)(CRITICAL_SECTION *);
static BOOL(WINAPI *g_real_cs_initsc)(CRITICAL_SECTION *, DWORD);
static BOOL(WINAPI *g_real_cs_initex)(CRITICAL_SECTION *, DWORD, DWORD);
static void(WINAPI *g_real_cs_del)(CRITICAL_SECTION *);
static int g_hooked_cs, g_hooked_data;

/* 1 records and repairs, 0 does neither and does not even install the hooks,
 * 2 installs the hooks and throws away what they see.
 *
 * Mode 2 exists to answer one question by experiment rather than by argument.
 * Turning the mechanism on costs a post-restore stall of the full 500 ms the
 * harness will wait, on 99 of 100 restores, while every counter it keeps says it
 * did nothing: no section came back held, none was repaired, none went
 * unrecorded, the table never filled. Two guesses at where the cost hides -
 * a record lock stuck across the restore, and two entry points resolving to the
 * same address - were both measured and both wrong. So mode 2 splits the
 * remaining candidates cleanly: it keeps the redirected imports and the extra
 * call through our code, and removes the bookkeeping entirely.
 *

 * Off has to mean off all the way down, because the hooks are not free of
 * consequence: they redirect an import in every loaded module, ntdll and the C
 * runtime included, and they run our code inside somebody else's initialisation
 * path. When a run that used to survive starts dying, "was it the repair or was
 * it the hooking" has to be answerable by one variable, and a knob that only
 * disabled the repair could not answer it. */
static int locks_mode(void)
{
	static int v = -1;
	if (v < 0) {
		char b[16];
		DWORD n = ss_getenv("D3D9SW_LOCKS", b, sizeof(b));
		v = (n && n < sizeof(b)) ? atoi(b) : 1;
	}
	return v;
}

/* Bounded, and it gives up rather than waiting, for the reason spelled out on
 * fntab_lock: this runs on the caller's thread inside somebody else's
 * initialisation path, and a wait that cannot end costs the process. */
static int cs_lock(void)
{
	int spins = 0;

	while (InterlockedCompareExchange(&g_cs->lock, 1, 0)) {
		if (++spins > 200000) {
			InterlockedIncrement(&g_cs->skipped);
			return 0;
		}
		YieldProcessor();
	}
	return 1;
}

/* Open addressing on the address, not a linear scan. Programs initialise
 * sections in their thousands during start-up, and a scan of sixteen thousand
 * entries per call would turn a bookkeeping aside into a visible cost. */
static CsRec *cs_slot(CRITICAL_SECTION *cs, int create)
{
	unsigned h = (unsigned)(((uintptr_t)cs >> 4) * 2654435761u) & (SS_MAX_CS - 1);
	unsigned i;

	for (i = 0; i < SS_MAX_CS; i++) {
		CsRec *e = &g_cs->t[(h + i) & (SS_MAX_CS - 1)];

		if (e->cs == cs)
			return e;
		if (!e->cs) {
			if (!create)
				return NULL;
			e->cs = cs;
			g_cs->n++;
			return e;
		}
	}
	if (create)
		InterlockedIncrement(&g_cs->overflow);
	return NULL;
}

static void cs_note(CRITICAL_SECTION *cs, DWORD spin)
{
	if (!g_cs || !cs || locks_mode() == 2 || !cs_lock())
		return;
	{
		CsRec *e = cs_slot(cs, 1);
		if (e) {
			e->spin = spin;
			e->live = 1;
		}
	}
	InterlockedExchange(&g_cs->lock, 0);
}

static void cs_forget(CRITICAL_SECTION *cs)
{
	if (!g_cs || !cs || locks_mode() == 2 || !cs_lock())
		return;
	{
		/* The key is kept as a tombstone so the probe chains behind it stay
		 * intact; a later section at the same address simply reuses the slot. */
		CsRec *e = cs_slot(cs, 0);
		if (e)
			e->live = 0;
	}
	InterlockedExchange(&g_cs->lock, 0);
}

/* Forces the record lock free after a restore, for the reason spelled out at
 * length on fntab_unstick: the lock word lives in excluded memory, so a thread
 * suspended inside cs_note holding it is rewound to before it acquired it and
 * never reaches the unlock, while the word it set survives untouched.
 *
 * Leaving this out was measurable rather than theoretical. With the hooks on, 99
 * of 100 restores left the worker thread making no progress for the full 500 ms
 * the harness was willing to wait; with the hooks off, the same run resumed in
 * 0.01 ms on average. A stuck record lock costs every later section operation a
 * 200000-spin wait before it gives up, and a runtime that initialises sections
 * as often as Mono does pays that over and over.
 *
 * Safe unconditionally: every thread is suspended here, so no holder exists that
 * could still be making progress. */
static void cs_unstick(void)
{
	if (!g_cs)
		return;
	if (InterlockedExchange(&g_cs->lock, 0))
		ss_log("  locks: record lock was held across the restore and has been "
		       "forced free\n");
}

static void WINAPI hook_cs_init(CRITICAL_SECTION *cs)
{
	g_real_cs_init(cs);
	cs_note(cs, 0);
}

static BOOL WINAPI hook_cs_initsc(CRITICAL_SECTION *cs, DWORD spin)
{
	BOOL r = g_real_cs_initsc(cs, spin);
	if (r)
		cs_note(cs, spin);
	return r;
}

static BOOL WINAPI hook_cs_initex(CRITICAL_SECTION *cs, DWORD spin, DWORD flags)
{
	BOOL r = g_real_cs_initex(cs, spin, flags);
	if (r)
		cs_note(cs, spin);
	return r;
}

static void WINAPI hook_cs_del(CRITICAL_SECTION *cs)
{
	/* Forgotten first. After the real call the section is no longer a section,
	 * and a restore that arrived in between would be inspecting freed memory. */
	cs_forget(cs);
	g_real_cs_del(cs);
}

static void cs_alloc(void)
{
	if (!g_cs)
		g_cs = (CsRecs *)VirtualAlloc(NULL, sizeof(CsRecs), MEM_COMMIT | MEM_RESERVE,
					      PAGE_READWRITE);
}

static void cs_exclude(void)
{
	if (g_cs)
		ss_exclude(g_cs, sizeof(CsRecs));
}

/* Patched in every module rather than just one, because unlike Mono's unwind
 * calls these are ordinary imports used by everything: the game, its C runtime,
 * Mono, the GC, and any overlay that let itself in. A section we did not see
 * created is a section we cannot repair. */
static int patch_every_module(void *from, void *to);
static int patch_every_module_data(void *from, void *to);

/* Repeated rather than done once, and the first version's "return if already
 * resolved" is why: it patched at start-up, when the only modules present were
 * the executable and the C runtime, both of which had already created their
 * sections. Mono arrives later with its own import table, so the very run this
 * was built for reported "0 tracked" - a mechanism that looked installed and saw
 * nothing. Exactly the mistake the unwind-table hooks made first, for exactly
 * the same reason.
 *
 * Patching is idempotent: a slot already holding our hook no longer matches the
 * address being searched for, so a repeat either finds a module that arrived
 * since or does nothing. */
static void cs_hook(void)
{
	HMODULE k32 = GetModuleHandleA("kernel32.dll");

	if (!k32 || !g_cs || !locks_mode())
		return;
	if (!g_real_cs_init) {
		g_real_cs_init = (void(WINAPI *)(CRITICAL_SECTION *))(void *)GetProcAddress(
			k32, "InitializeCriticalSection");
		g_real_cs_initsc =
			(BOOL(WINAPI *)(CRITICAL_SECTION *, DWORD))(void *)GetProcAddress(
				k32, "InitializeCriticalSectionAndSpinCount");
		g_real_cs_initex =
			(BOOL(WINAPI *)(CRITICAL_SECTION *, DWORD, DWORD))(void *)
				GetProcAddress(k32, "InitializeCriticalSectionEx");
		g_real_cs_del = (void(WINAPI *)(CRITICAL_SECTION *))(void *)GetProcAddress(
			k32, "DeleteCriticalSection");
		if (!g_real_cs_init || !g_real_cs_initsc || !g_real_cs_del) {
			g_real_cs_init = NULL;
			return;
		}
		/* Printed because these four names need not be four functions. All of
		 * them are kernel32 forwarders into ntdll, and if any two resolve to the
		 * same address then patching by address redirects both - so a caller of
		 * InitializeCriticalSectionEx, which returns BOOL and takes flags, can
		 * land in a hook that returns nothing and drops them. A caller reading
		 * that as failure goes down an error path, and an error path formats a
		 * message, which is where these runs are dying. */
		ss_log("  locks: init %p, initsc %p, initex %p, delete %p%s\n",
		       (void *)g_real_cs_init, (void *)g_real_cs_initsc,
		       (void *)g_real_cs_initex, (void *)g_real_cs_del,
		       ((void *)g_real_cs_init == (void *)g_real_cs_initsc ||
			(void *)g_real_cs_init == (void *)g_real_cs_initex ||
			(void *)g_real_cs_initsc == (void *)g_real_cs_initex)
			       ? "  <-- ALIASED, patching by address cannot tell them apart"
			       : "");
	}
	g_hooked_cs += patch_every_module((void *)g_real_cs_init, (void *)hook_cs_init);
	g_hooked_cs += patch_every_module((void *)g_real_cs_initsc, (void *)hook_cs_initsc);
	if (g_real_cs_initex)
		g_hooked_cs +=
			patch_every_module((void *)g_real_cs_initex, (void *)hook_cs_initex);
	g_hooked_cs += patch_every_module((void *)g_real_cs_del, (void *)hook_cs_del);
	/* Only while nothing has been recorded. If sections are being seen, the
	 * import patches are doing the job and this scan is pure cost; if none are,
	 * something is holding a resolved copy somewhere else and this is the only
	 * way to reach it. */
	if (!g_cs->n) {
		g_hooked_data +=
			patch_every_module_data((void *)g_real_cs_init, (void *)hook_cs_init);
		g_hooked_data += patch_every_module_data((void *)g_real_cs_initsc,
							(void *)hook_cs_initsc);
		if (g_real_cs_initex)
			g_hooked_data += patch_every_module_data((void *)g_real_cs_initex,
								(void *)hook_cs_initex);
		g_hooked_data +=
			patch_every_module_data((void *)g_real_cs_del, (void *)hook_cs_del);
	}
}

/* Eager while nothing has been recorded, then a sixteenth of the rate.
 *
 * Rate-limiting from the start was another way of doing nothing: the harness
 * calls the guard eight times between loading Mono and initialising it, so a
 * "every sixteenth call" hook never ran in the one window that mattered, and
 * Mono created all of its sections unobserved. Once sections are arriving the
 * hooks are demonstrably in place and the only reason to repeat is a module that
 * loads later. */
static void cs_keep_hooked(void)
{
	static unsigned tick;

	if (g_cs && g_cs->n && (++tick & 15u))
		return;
	cs_hook();
}

/* A crash minutes after a restore is the hard kind to reason about, because by
 * then the evidence is a dead process and a guess. This costs nothing until
 * something faults, and then it names the instruction, the module it belongs to,
 * the address that was touched, and how long the restore had been holding. It
 * does not handle the fault - the game's own handler still runs, and the process
 * still dies the way it would have. */
static volatile LONG g_faults;
static volatile LONG g_frames_since_load = -1;

/* One fault reports at a time.
 *
 * Two threads faulted at the same instant and their reports interleaved line by
 * line: two rsp lines, two r13 lines, and no way to tell which register belonged
 * to which thread. A bounded spin rather than a real lock, because a lock here
 * can be the very thing that is broken, and after the spin it prints anyway -
 * interleaved evidence still beats no evidence. Nested faults on the same thread
 * pass straight through so a fault inside the report cannot deadlock it. */
static volatile LONG g_fault_gate;

static int fault_gate_enter(void)
{
	LONG me = (LONG)GetCurrentThreadId();
	int spins;

	if (g_fault_gate == me)
		return 0;
	/* The budget was 100000 spins, which sounds generous and is not: a report is
	 * fifty-odd lines and every one is a synchronous WriteFile on a handle opened
	 * FILE_FLAG_WRITE_THROUGH, so the holder is waiting on the disk, not on the
	 * processor. Two Unity job threads faulted on the same instruction and the
	 * second one blew through the budget and printed into the middle of the
	 * first, producing a block with two stacks, two frame 0 lines and duplicated
	 * registers - which reads as one incoherent fault rather than two identical
	 * ones, and is exactly how it was first misread. Raised to something sized
	 * against disk latency instead of against a lock. Reports are capped at eight
	 * per session, so the worst case is bounded. */
	for (spins = 0; spins < 20000000; spins++) {
		if (InterlockedCompareExchange(&g_fault_gate, me, 0) == 0)
			return 1;
		YieldProcessor();
	}
	return 0;
}

static void fault_gate_leave(int held)
{
	if (held)
		InterlockedExchange(&g_fault_gate, 0);
}

/* Ranges the last restore decommitted because they were drift no heap claimed.
 * Decommit rather than release was chosen so that a wrong guess faults at a
 * known address instead of silently landing in reused memory; this table is what
 * makes the address known. */
static uintptr_t g_dc_base[64];
static uintptr_t g_dc_size[64];
static int g_dc_n;

static int decommitted_drift(uintptr_t at, uintptr_t *base, uintptr_t *size)
{
	int i;

	for (i = 0; i < g_dc_n; i++)
		if (at >= g_dc_base[i] && at - g_dc_base[i] < g_dc_size[i]) {
			*base = g_dc_base[i];
			*size = g_dc_size[i];
			return 1;
		}
	return 0;
}

static LONG CALLBACK ss_fault_log(EXCEPTION_POINTERS *ep)
{
	DWORD code = ep->ExceptionRecord->ExceptionCode;
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t pc;
	int held;

	pc = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;

	/* OutputDebugString arrives as an exception rather than a call, which is
	 * a gift: it means the game's own diagnostics pass through here without
	 * a debugger attached. When it decides to quit, whatever it printed on
	 * the way out is the closest thing to an explanation we will ever get. */
	if ((code == DBG_PRINTEXCEPTION_C || code == 0x4001000A) && g_events &&
	    ep->ExceptionRecord->NumberParameters >= 2) {
		const char *msg = (const char *)ep->ExceptionRecord->ExceptionInformation[1];
		SIZE_T len = (SIZE_T)ep->ExceptionRecord->ExceptionInformation[0];
		MEMORY_BASIC_INFORMATION mb;
		if (msg && len && VirtualQuery(msg, &mb, sizeof(mb)) == sizeof(mb) &&
		    mb.State == MEM_COMMIT) {
			LONG i = InterlockedIncrement(&g_events->ndbg) - 1;
			char *slot = g_events->dbg[(unsigned)i & 7];
			SIZE_T n = len < sizeof(g_events->dbg[0]) - 1
					   ? len
					   : sizeof(g_events->dbg[0]) - 1;
			SIZE_T k;
			if (code == 0x4001000A) {
				const wchar_t *w = (const wchar_t *)msg;
				for (k = 0; k + 1 < n && w[k]; k++)
					slot[k] = (char)(w[k] < 128 ? w[k] : '?');
				slot[k] = 0;
			} else {
				memcpy(slot, msg, n);
				slot[n] = 0;
			}
			for (k = 0; slot[k]; k++)
				if (slot[k] == '\r' || slot[k] == '\n')
					slot[k] = ' ';
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}

	/* Everything goes in the ring, including the C++ throws that are usually
	 * routine, because a throw nobody catches is one of the ways this game
	 * can leave without saying anything. The ring is what the exit hooks
	 * print when that happens. */
	if (g_events) {
		LONG i = InterlockedIncrement(&g_events->nfault) - 1;
		FaultRec *f = &g_events->fault[(unsigned)i & 31];
		f->code = code;
		f->tid = GetCurrentThreadId();
		f->pc = pc;
		f->at = (code == EXCEPTION_ACCESS_VIOLATION &&
			 ep->ExceptionRecord->NumberParameters >= 2)
				? (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1]
				: 0;
		f->frame = g_frames_since_load;
	}

	switch (code) {
	case EXCEPTION_ACCESS_VIOLATION:
	case EXCEPTION_IN_PAGE_ERROR:
	case EXCEPTION_ILLEGAL_INSTRUCTION:
	case EXCEPTION_PRIV_INSTRUCTION:
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
	case EXCEPTION_STACK_OVERFLOW:
		break;
	default:
		return EXCEPTION_CONTINUE_SEARCH;
	}
	if (InterlockedIncrement(&g_faults) > 8)
		return EXCEPTION_CONTINUE_SEARCH;

	held = fault_gate_enter();
	if (!held && g_fault_gate != (LONG)GetCurrentThreadId())
		/* Printing anyway is the right call - interleaved evidence beats none -
		 * but printing anyway in SILENCE is not, because the result looks like
		 * one coherent report and gets read as one. Say it out loud so the lines
		 * below are known to be shuffled with another thread's. */
		ss_raw("WARNING: another thread is already writing a fault report and did "
		       "not finish in time. The lines below are INTERLEAVED with thread "
		       "%u's and must be split by hand before being read.\n",
		       (unsigned long)g_fault_gate);
	{
		unsigned off = 0;
		const char *in = ss_module(pc, &off);
		ss_raw("fault: %08X at %p in %s+%X, thread %u, %d frame(s) since the last restore\n",
		       (unsigned long)code, (void *)pc, in ? in : "unknown", off,
		       (unsigned long)GetCurrentThreadId(), (long)g_frames_since_load);
	}
	if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
	    ep->ExceptionRecord->NumberParameters >= 2) {
		uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
		const char *how = ep->ExceptionRecord->ExceptionInformation[0] == 8 ? "executing"
				  : ep->ExceptionRecord->ExceptionInformation[0] ? "writing"
										: "reading";
		const char *heap = ss_heap_of(at);
		const char *who = "unmapped";
		unsigned off = 0;
		void *alloc = NULL;
		if (VirtualQuery((LPCVOID)at, &mbi, sizeof(mbi)) == sizeof(mbi) &&
		    mbi.State != MEM_FREE && mbi.Type == MEM_IMAGE) {
			const char *m = ss_module(at, &off);
			who = m ? m : "an image";
		} else if (mbi.State == MEM_COMMIT)
			who = "committed data";
		else if (mbi.State == MEM_RESERVE)
			who = "reserved but not committed";
		if (mbi.State != MEM_FREE)
			alloc = mbi.AllocationBase;
		ss_raw("       %s %p (%s%s), allocation base %p\n", how, (void *)at, who,
		       heap ? heap : "", alloc);
		{
			uintptr_t db, dz;

			if (decommitted_drift(at, &db, &dz))
				ss_raw("       VERDICT: that address is inside %p+%lx, which the "
				       "last restore decommitted as unclaimed drift. Something "
				       "in the present still held it\n",
				       (void *)db, (unsigned long)dz);
		}
	}
/* Spelled as "not 32-bit x86" to match how the rest of this file selects. This
 * file builds for five targets, three of them 32-bit, and the register names
 * below only exist in the 64-bit CONTEXT. */
#if !defined(_M_IX86) && !defined(__i386__)
	if (ep->ContextRecord) {
		static const char *const nm[16] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp",
						    "rsi", "rdi", "r8",	 "r9",	"r10", "r11",
						    "r12", "r13", "r14", "r15" };
		const CONTEXT *c = ep->ContextRecord;
		const DWORD64 gpr[16] = { c->Rax, c->Rcx, c->Rdx, c->Rbx, c->Rsp, c->Rbp,
					  c->Rsi, c->Rdi, c->R8,  c->R9,  c->R10, c->R11,
					  c->R12, c->R13, c->R14, c->R15 };
		int i;

		for (i = 0; i < 16; i++)
			ss_where_reg(nm[i], (uintptr_t)gpr[i]);

		/* Names the register the fault came out of, which the loop above cannot.
		 * ss_where_reg returns early on MEM_FREE, because a register pointing at
		 * nothing usually has nothing to say - but the faulting address is the
		 * one case where it has everything to say, and it is unmapped by
		 * definition or there would be no fault. So eight faults writing to
		 * 0x000000DC00000000 and its siblings were logged without ever recording
		 * which register carried the value, and the whole half-pointer family was
		 * described for two sessions without that one fact.
		 *
		 * Also decomposes the value against rsp, because the family's signature
		 * is a 64-bit quantity with one half current and one half degenerate, and
		 * asking arithmetic directly beats recognising it by eye later. */
		if (ep->ExceptionRecord->NumberParameters >= 2) {
			uintptr_t bad = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];

			/* Matched with a displacement, not just exactly, because the
			 * commonest access violation of all is `mov reg,[base+field]`
			 * on a null base - and there the faulting address is the FIELD
			 * OFFSET, so no register equals it and an exact test names
			 * nothing. That happened: a game fault reading 0x20 printed
			 * eight registers and omitted rbx, the only one that mattered,
			 * because rbx was 0 and ss_where_reg drops values below 0x10000
			 * as small integers rather than pointers. The register had to be
			 * recovered afterwards by disassembling mono - 8B 43 20,
			 * `mov eax,[rbx+20h]`. The disassembly should confirm this line,
			 * not substitute for it. */
			for (i = 0; i < 16; i++) {
				uintptr_t disp = bad - (uintptr_t)gpr[i];

				if (disp > 0xFFFF)
					continue;
				if (disp == 0)
					ss_raw("       THE FAULTING ADDRESS IS IN %s\n", nm[i]);
				else
					ss_raw("       THE FAULTING ACCESS IS [%s+%lX], and %s "
					       "holds %p%s\n",
					       nm[i], (unsigned long)disp, nm[i],
					       (void *)(uintptr_t)gpr[i],
					       gpr[i] ? "" : " - a NULL base, so this is a null "
							     "dereference and the number above is "
							     "a STRUCTURE FIELD OFFSET, not an "
							     "address");
			}
			/* Checked before the half-pointer tests, because -1 satisfies
			 * the "low half all ones" test and is nothing to do with that
			 * family. Reporting it as a SHAPE implied kinship with the
			 * half-pointer signature and invited exactly the wrong reading
			 * - that garbage had been read in. All ones is not garbage; it
			 * is the most common sentinel there is, and this project has
			 * already retired one theory built on mistaking it for damage
			 * (the -1 first word of a CRITICAL_SECTION is DebugInfo for a
			 * section created with RTL_CRITICAL_SECTION_FLAG_NO_DEBUG_INFO,
			 * i.e. an intact object). */
			if (bad == (uintptr_t)-1)
				ss_raw("       THIS IS A SENTINEL, NOT A CORRUPT POINTER: the "
				       "address is exactly -1 - INVALID_HANDLE_VALUE, "
				       "(void*)-1, an 'absent' or 'not yet set' marker, or a "
				       "sign-extended 0xFFFFFFFF. Something dereferenced a "
				       "value meaning 'nothing here' without checking it, "
				       "which is a missing test on a field that was cleared "
				       "or never filled - NOT random data read as a pointer. "
				       "It is also NOT the half-pointer family, whose shape "
				       "is a high half present with a low half of ZERO\n");
			else if (bad >> 32 && (bad & 0xFFFFFFFFu) == 0)
				ss_raw("       SHAPE: high half %08lX present, low half ZERO; "
				       "rsp's high half is %08lX (%s)\n",
				       (unsigned long)(bad >> 32),
				       (unsigned long)((uintptr_t)c->Rsp >> 32),
				       (bad >> 32) == ((uintptr_t)c->Rsp >> 32) ? "SAME - a stack "
										  "address that "
										  "lost its low "
										  "half"
									       : "different");
			else if ((bad & 0xFFFFFFFFu) == 0xFFFFFFFFu && bad >> 32)
				ss_raw("       SHAPE: high half %08lX present, low half ALL ONES "
				       "- a 64-bit value whose low dword was -1 when it was "
				       "read\n",
				       (unsigned long)(bad >> 32));
		}

		/* The second unexplained pattern, asked directly rather than inferred.
		 * Two faults inside mono wrote to 0x2200000000 and 0xDE00000000 while
		 * rsp was 0x22AFBFF5C8 and 0xDE25FFF938 - both times the stack pointer
		 * with its low 32 bits cleared. Twice, in independent runs, is not
		 * coincidence, and a pointer derived from a stack address that has lost
		 * its low half is what a stale or half-written stack bound looks like.
		 *
		 * The bounds come free: this handler runs ON the faulting thread, so its
		 * TEB is the current one. DeallocationStack is the reservation base,
		 * which is what a garbage collector scanning a thread would use, so it
		 * is worth reporting next to the committed limits. */
		if (ep->ExceptionRecord->NumberParameters >= 2) {
			uintptr_t bad = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
			const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();

			if (bad && (bad & 0xFFFFFFFFu) == 0 &&
			    (bad >> 32) == ((uintptr_t)c->Rsp >> 32))
				ss_raw("       PATTERN: the faulting address IS rsp with its "
				       "low 32 bits cleared (rsp=%p)\n",
				       (void *)(uintptr_t)c->Rsp);
			ss_raw("       this thread's stack: base %p, limit %p, reservation "
			       "base %p\n",
			       tib->StackBase, tib->StackLimit,
			       *(void *const *)((const char *)tib + 0x1478));
		}
	}
#endif
	fmt_report_for(GetCurrentThreadId());
	ss_callers(ep->ContextRecord);
	fault_gate_leave(held);
	return EXCEPTION_CONTINUE_SEARCH;
}

/* This game dies politely: it installs its own unhandled-exception filter and
 * calls TerminateProcess, so a fatal error looks like the window simply closing
 * and Windows never reports anything. These hooks make the departure say who
 * asked for it and what the process had just been complaining about. */
static void exit_report(const char *why, void *caller)
{
	MEMORY_BASIC_INFORMATION mbi;
	char name[MAX_PATH] = "?";
	int i, n;

	if (VirtualQuery(caller, &mbi, sizeof(mbi)) == sizeof(mbi))
		GetModuleFileNameA((HMODULE)mbi.AllocationBase, name, sizeof(name));
	ss_log("exit: %s called from %p in %s, %ld frame(s) since the last restore\n", why,
	       caller, name, g_frames_since_load);
	/* An allocation that failed for want of address space is the quietest way
	 * for a 32-bit process to die, so the shape of the map goes in the
	 * obituary. */
	{
		MEMORY_BASIC_INFORMATION m;
		uintptr_t a = 0, used = 0, freeb = 0, big = 0;
		while (VirtualQuery((LPCVOID)a, &m, sizeof(m)) == sizeof(m)) {
			uintptr_t end = (uintptr_t)m.BaseAddress + m.RegionSize;
			if (end <= a)
				break;
			if (m.State == MEM_FREE) {
				freeb += m.RegionSize;
				if (m.RegionSize > big)
					big = m.RegionSize;
			} else {
				used += m.RegionSize;
			}
			a = end;
		}
		ss_log("      address space at death: %u MB used, %u MB free, largest block %u MB\n",
		       (unsigned)(used >> 20), (unsigned)(freeb >> 20), (unsigned)(big >> 20));
	}
	if (!g_events)
		return;
	n = (int)g_events->nfault;
	if (n > 32)
		n = 32;
	for (i = n - 1; i >= 0 && i > n - 9; i--) {
		FaultRec *f = &g_events->fault[(unsigned)(g_events->nfault - (n - i)) & 31];
		ss_log("      prior exception %08lX at %p", (unsigned long)f->code, (void *)f->pc);
		if (f->at)
			ss_log(" touching %p", (void *)f->at);
		ss_log(", thread %lu, frame %ld\n", (unsigned long)f->tid, f->frame);
	}
	if (!n)
		ss_log("      no exception preceded this, so the process chose to leave\n");
	{
		int d = (int)g_events->ndbg, first = d > 8 ? d - 8 : 0, j;
		for (j = first; j < d; j++)
			ss_log("      it said: %s\n", g_events->dbg[(unsigned)j & 7]);
	}
}

/* Every way out of a process funnels through ExitProcess eventually, but not
 * always through the game's own imports: a normal shutdown returns from WinMain
 * and lets the CRT call it, which reaches kernel32 via MSVCR100's import table
 * rather than the executable's. Patching every loaded module catches the exits
 * we would otherwise only be able to infer from a log that simply stops. */
static int patch_every_module(void *from, void *to)
{
	MODULEENTRY32 me;
	HMODULE self = NULL;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
	int n = 0;

	if (snap == INVALID_HANDLE_VALUE)
		return 0;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCSTR)&patch_every_module, &self);
	me.dwSize = sizeof(me);
	if (Module32First(snap, &me)) {
		do {
			if ((HMODULE)me.modBaseAddr != self)
				n += patch_iat((HMODULE)me.modBaseAddr, from, to);
		} while (Module32Next(snap, &me));
	}
	CloseHandle(snap);
	return n;
}

#if !defined(_M_IX86) && !defined(__i386__)
/* The same sweep, but through writable data rather than import tables.
 *
 * Needed because an import table is not the only place a resolved address is
 * kept. Mono resolves entry points with GetProcAddress and caches them in its
 * own globals - that is how it reaches the unwind-table functions, and the
 * critical-section hooks found six imports and recorded nothing, which is the
 * same symptom. Expensive enough that the caller is expected to stop asking
 * once it has found what it needs. */
static int patch_every_module_data(void *from, void *to)
{
	MODULEENTRY32 me;
	HMODULE self = NULL;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
	int n = 0;

	if (snap == INVALID_HANDLE_VALUE)
		return 0;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCSTR)&patch_every_module_data, &self);
	me.dwSize = sizeof(me);
	if (Module32First(snap, &me)) {
		do {
			if ((HMODULE)me.modBaseAddr != self)
				n += patch_data_ptr((HMODULE)me.modBaseAddr, from, to);
		} while (Module32Next(snap, &me));
	}
	CloseHandle(snap);
	return n;
}
#else
static int patch_every_module_data(void *from, void *to)
{
	(void)from;
	(void)to;
	return 0;
}
#endif

static void(WINAPI *g_real_exitprocess)(UINT);
static BOOL(WINAPI *g_real_terminate)(HANDLE, UINT);
static void(__cdecl *g_real_exit)(int);
static void(__cdecl *g_real_uexit)(int);
static void(__cdecl *g_real_amsg)(int);

static void WINAPI hook_exitprocess(UINT code)
{
	exit_report("ExitProcess", __builtin_return_address(0));
	g_real_exitprocess(code);
}

static BOOL WINAPI hook_terminate(HANDLE p, UINT code)
{
	exit_report("TerminateProcess", __builtin_return_address(0));
	return g_real_terminate(p, code);
}

static void __cdecl hook_exit(int c)
{
	exit_report("exit", __builtin_return_address(0));
	g_real_exit(c);
}

static void __cdecl hook_uexit(int c)
{
	exit_report("_exit", __builtin_return_address(0));
	g_real_uexit(c);
}

/* The CRT's "this program has requested the runtime to terminate it in an
 * unusual way" path - the R6xxx dialog. */
static void __cdecl hook_amsg(int c)
{
	exit_report("_amsg_exit", __builtin_return_address(0));
	g_real_amsg(c);
}

/* Mono finishes a fatal error with abort(), which reaches process death inside
 * ntdll rather than through any import we can watch, so the abort call itself is
 * the last observable point. */
static void(__cdecl *g_real_abort)(void);

static void __cdecl hook_abort(void)
{
	exit_report("abort", __builtin_return_address(0));
	g_real_abort();
}

/* The runtimes a game might link, most specific first. The D3D9 title's is
 * tried first so its behaviour is unchanged; a Unity player links the UCRT
 * instead, where the previously hardcoded name matched nothing and left every
 * CRT exit path unhooked. */
static const char *const kCrtNames[] = { "MSVCR100.dll", "MSVCR120.dll", "MSVCR110.dll",
					 "ucrtbase.dll", "msvcrt.dll" };

static HMODULE game_crt_module(void)
{
	size_t i;
	for (i = 0; i < sizeof(kCrtNames) / sizeof(kCrtNames[0]); i++) {
		HMODULE m = GetModuleHandleA(kCrtNames[i]);
		if (m)
			return m;
	}
	return NULL;
}

/* Idempotent, because it runs at the first D3D call to catch events created
 * during start-up and again once the helper exists, by which point winmm may
 * finally be loaded. Already-redirected entries no longer match the real
 * address, so a second pass finds only what the first one missed. */
void savestate_hooks_install(void)
{
	HMODULE k32 = GetModuleHandleA("kernel32.dll");
	HMODULE mm = GetModuleHandleA("winmm.dll");
	HMODULE exe = GetModuleHandleA(NULL);
	int n = 0;

	if (!g_events) {
		g_events = (EventTrack *)VirtualAlloc(NULL, sizeof(EventTrack),
						      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		AddVectoredExceptionHandler(1, ss_fault_log);
	}
	fntab_alloc();
	cs_alloc();
	/* Earlier is strictly better here: a section created before the hooks land
	 * is invisible for the life of the process, and the ones created earliest
	 * are the C runtime's and the loader's. */
	cs_hook();
	/* Retried on every pass, because Mono loads when Unity starts scripting and
	 * may well not be present the first time this runs. Being hooked before the
	 * first save is what matters, not before Mono's first compile: a table
	 * registered earlier and untouched since has identical contents on both
	 * sides of the rewind and needs nothing done to it. */
	fntab_hook();
	if (k32 && !g_real_createevent) {
		g_real_createevent = (HANDLE(WINAPI *)(LPSECURITY_ATTRIBUTES, BOOL, BOOL,
						       LPCSTR))(void *)
			GetProcAddress(k32, "CreateEventA");
		if (g_real_createevent)
			g_hooked_event += patch_iat(exe, (void *)g_real_createevent,
						    (void *)hook_createevent);
	}

	if (k32 && !g_real_terminate) {
		HMODULE crt = game_crt_module();
		g_real_terminate = (BOOL(WINAPI *)(HANDLE, UINT))(void *)GetProcAddress(
			k32, "TerminateProcess");
		g_real_exitprocess =
			(void(WINAPI *)(UINT))(void *)GetProcAddress(k32, "ExitProcess");
		if (g_real_terminate)
			patch_every_module((void *)g_real_terminate, (void *)hook_terminate);
		if (g_real_exitprocess)
			patch_every_module((void *)g_real_exitprocess, (void *)hook_exitprocess);
		if (crt) {
			g_real_exit = (void(__cdecl *)(int))(void *)GetProcAddress(crt, "exit");
			g_real_uexit = (void(__cdecl *)(int))(void *)GetProcAddress(crt, "_exit");
			g_real_amsg =
				(void(__cdecl *)(int))(void *)GetProcAddress(crt, "_amsg_exit");
			g_real_abort = (void(__cdecl *)(void))(void *)GetProcAddress(crt, "abort");
			/* Unity reaches these from UnityPlayer and mono, never
			 * from the executable, so patching only the exe's imports
			 * caught nothing. */
			if (g_real_exit)
				patch_every_module((void *)g_real_exit, (void *)hook_exit);
			if (g_real_uexit)
				patch_every_module((void *)g_real_uexit, (void *)hook_uexit);
			if (g_real_amsg)
				patch_every_module((void *)g_real_amsg, (void *)hook_amsg);
			if (g_real_abort)
				patch_every_module((void *)g_real_abort, (void *)hook_abort);
		}
	}

	if (k32) {
		g_real_qpc = (BOOL(WINAPI *)(LARGE_INTEGER *))(void *)GetProcAddress(
			k32, "QueryPerformanceCounter");
		g_real_tick = (DWORD(WINAPI *)(void))(void *)GetProcAddress(k32, "GetTickCount");
	}
	if (mm)
		g_real_tgt = (DWORD(WINAPI *)(void))(void *)GetProcAddress(mm, "timeGetTime");

	if (g_real_qpc)
		n += patch_iat(exe, (void *)g_real_qpc, (void *)hook_qpc);
	if (g_real_tick)
		n += patch_iat(exe, (void *)g_real_tick, (void *)hook_tick);
	if (g_real_tgt)
		n += patch_iat(exe, (void *)g_real_tgt, (void *)hook_tgt);
	g_hooked_time += n;
}

enum { RECLAIM_OFF = 0, RECLAIM_GROW, RECLAIM_ALL };

struct Slot {
	int valid;
	int nregs;
	int nthreads;
	unsigned long long bytes;
	HANDLE sect;
	int nids;
	DWORD ids[SS_MAX_THREADS];
	PVOID starts[SS_MAX_THREADS];
	int nfiles;
	int nevents;
	EventState events[SS_MAX_EVENTS];
	TimeBase clock;
	FileState files[SS_MAX_FILES];
	Region regs[SS_MAX_REGIONS];
	ThreadState threads[SS_MAX_THREADS];
};
typedef struct Slot Slot;

/* A section outside the restored set was never rewound, so its state is the
 * present one and correct by construction. Touching it would be inventing a
 * problem: ours, ntdll's, and every held module's sections are in that category,
 * and one of them is the lock the loader itself uses. */
static int cs_in_restored(Slot *s, void *p)
{
	uintptr_t a = (uintptr_t)p;
	int i;

	for (i = 0; i < s->nregs; i++)
		if (a >= (uintptr_t)s->regs[i].base &&
		    a < (uintptr_t)s->regs[i].base + (uintptr_t)s->regs[i].size)
			return 1;
	return 0;
}

/* The owner is coming back if its context is in the snapshot: it will resume
 * inside the section and leave it. Threads that exited between the save and the
 * restore are the dangerous case, and one has already been observed - a restore
 * that reported 52 threads against 55 at save. */
static int cs_owner_resumes(Slot *s, DWORD tid)
{
	int i;

	if (!tid)
		return 0;
	for (i = 0; i < s->nthreads; i++)
		if (s->threads[i].tid == tid)
			return 1;
	return 0;
}

/* Runs after the memory is back and the thread contexts are set, with every
 * thread still suspended, so nothing can be entering a section while this looks
 * at it. */
static void cs_reconcile(Slot *s)
{
	LONG i;

	if (!g_cs || !locks_mode()) {
		if (g_cs)
			ss_log("  locks: recording and repair both OFF by D3D9SW_LOCKS=0\n");
		return;
	}
	g_cs->held = 0;
	g_cs->reset = 0;
	g_cs->kept = 0;
	g_cs->unreadable = 0;
	g_cs->outside = 0;
	for (i = 0; i < SS_MAX_CS; i++) {
		CsRec *e = &g_cs->t[i];
		CRITICAL_SECTION *cs = e->cs;
		MEMORY_BASIC_INFORMATION mbi;
		DWORD owner;

		if (!cs || !e->live)
			continue;
		if (!cs_in_restored(s, cs)) {
			g_cs->outside++;
			continue;
		}
		/* The section's own memory can have gone away without
		 * DeleteCriticalSection ever being called - freed, or decommitted by
		 * the drift reclaim - and reading it would fault inside the restore. */
		if (!VirtualQuery(cs, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
			g_cs->unreadable++;
			continue;
		}
		owner = (DWORD)(DWORD_PTR)cs->OwningThread;
		if (!owner && cs->LockCount == -1)
			continue;
		g_cs->held++;
		if (cs_owner_resumes(s, owner)) {
			g_cs->kept++;
			continue;
		}
		if (!locks_mode())
			continue;
		/* Free, in the shape ntdll expects. LockSemaphore, DebugInfo and the
		 * spin count are left alone: they are correct, and DebugInfo is
		 * threaded onto a list whose head is in ntdll, which we hold - the
		 * exact structure that already cost a process once. */
		cs->LockCount = -1;
		cs->RecursionCount = 0;
		cs->OwningThread = NULL;
		g_cs->reset++;
	}
	ss_log("  locks: %ld tracked via %d import(s) and %d cached pointer(s), %ld came "
	       "back held, %ld reinitialised, %ld left for a thread that resumes inside "
	       "them, %ld not in the snapshot, %ld gone%s\n",
	       (long)g_cs->n, g_hooked_cs, g_hooked_data, (long)g_cs->held, (long)g_cs->reset,
	       (long)g_cs->kept, (long)g_cs->outside, (long)g_cs->unreadable,
	       locks_mode() ? "" : ", repair OFF by D3D9SW_LOCKS=0");
	if (!g_hooked_cs)
		ss_log("  WARNING: the critical-section hooks never installed, so nothing "
		       "above is trustworthy\n");
	if (g_cs->overflow)
		ss_log("  locks: %ld section(s) could not be recorded, the table is full\n",
		       (long)g_cs->overflow);
	if (g_cs->skipped)
		ss_log("  locks: %ld section(s) went unrecorded after the record lock refused "
		       "to be taken\n",
		       (long)g_cs->skipped);
}

enum { REQ_NONE = 0, REQ_SAVE, REQ_LOAD };

/* One allocation, excluded from the snapshot, holding every mutable thing this
 * file needs during an operation. Fixed-size arrays rather than heap so that
 * nothing allocates while the process is suspended. */
typedef struct Control {
	volatile LONG request;
	volatile LONG busy;
	volatile LONG result;
	volatile LONG slot;
	uintptr_t helper_lo, helper_hi;
	LONG nex, nex_fixed;
	uintptr_t ex_lo[SS_MAX_EXCL], ex_hi[SS_MAX_EXCL];
	DWORD ids[SS_MAX_THREADS];
	PVOID starts[SS_MAX_THREADS];
	HANDLE handles[SS_MAX_THREADS];
	int nids;
	DWORD req_tid;
	char fresh[SS_MAX_THREADS];
	char transient[SS_MAX_THREADS];
	Region guarded[1024];
	int nguarded;
	unsigned long long guard_bytes;
	int policy;
	int tmode;
	int rmode;
	int guard;
	int last_fresh;
	int listed;
	DWORD anchor_tick;
#define SS_FMT_SLOTS 64
	int nheaps, heaps_listed;
	HANDLE heap_h[SS_MAX_HEAPS];
	uintptr_t heap_lo[SS_MAX_HEAPS], heap_hi[SS_MAX_HEAPS];
	char heap_ours[SS_MAX_HEAPS];
	char heap_name[SS_MAX_HEAPS][32];
	int nmods;
	uintptr_t mod_lo[SS_MAX_MODS], mod_hi[SS_MAX_MODS];
	char mod_rewound[SS_MAX_MODS];
	char mod_name[SS_MAX_MODS][32];
	int nseg;
	uintptr_t seg_base[SS_MAX_SEGS], seg_size[SS_MAX_SEGS];
	HANDLE seg_owner[SS_MAX_SEGS];
	uintptr_t stk_lo[SS_MAX_THREADS], stk_hi[SS_MAX_THREADS];
	int nstk;
	Region scratch[8192];
	int nscratch;
	HANDLE log;
	double last_ms, last_mb;
	/* Measured by the helper and read by the requester, both from memory the
	 * snapshot excludes, because the requester's own stack comes back from the
	 * save and cannot time anything. done_req is which request actually
	 * finished, which is the only way a resumed thread can tell that it was
	 * restored rather than saved. */
	double helper_ms;
	volatile LONG done_req;

	/* The last formatting call each thread made, so a fault inside the C
	 * runtime's printf machinery can say what was being formatted and where.
	 *
	 * Every fault in the recurring signature lands inside that machinery -
	 * output_processor::process, state_case_type, type_case_integer<10>, and
	 * string_output_adapter::write_string, which is the code that writes into the
	 * destination. Mono's format strings are readable English, so the one thing
	 * that would turn this from a stack trace into a sentence is knowing which
	 * string it was.
	 *
	 * Pointers are stored, not text: three stores per call rather than a copy,
	 * because Mono may do this constantly and an instrument that costs real time
	 * changes what it measures. The text is read later, by the fault handler,
	 * after checking the page is readable. In the control block because it must
	 * survive a restore to describe what happened after one. */
	volatile LONG fmt_calls;
	struct {
		DWORD tid;
		const char *fmt;
		char *buf;
		size_t count;
		unsigned long long opts;
	} fmt[SS_FMT_SLOTS];

	Slot slots[SAVESTATE_SLOTS];
} Control;

static Control *g_ctl;

/* d3d9_sw.cfg, slurped once on first use.
 *
 * This used to live in the control block, which made it unreadable until the
 * first save: the control block is allocated by ensure_helper, and nothing
 * allocates it until the game asks for a savestate. Every knob read before
 * then - anything on the rendering path, for instance - therefore saw the
 * environment only and silently missed the file. The wrapper's own settings are
 * needed at swapchain creation, long before any save.
 *
 * Its own VirtualAlloc block instead, excluded from the snapshot alongside the
 * other bookkeeping. It must not be ordinary module data: a settings cache
 * there would be rewound by every restore and re-read mid-copy, which is the
 * trap verify_mode fell into. Read once and never touched inside the suspended
 * window. */
#define SS_CFG_MAX 4096
static char *g_cfg;
static int g_cfg_len;
static int g_cfg_tried;

/* Loads d3d9_sw.cfg from the working directory - the same place the log is
 * written, so the two always sit together. Format is NAME=value, one per line,
 * with # or ; starting a comment. Absent file is the normal case. Idempotent,
 * because it is now reached both lazily and from ensure_helper. */
static void cfg_load(void)
{
	HANDLE h;
	DWORD got = 0;

	if (g_cfg_tried)
		return;
	g_cfg_tried = 1;
	g_cfg = (char *)VirtualAlloc(NULL, SS_CFG_MAX, MEM_COMMIT | MEM_RESERVE,
				     PAGE_READWRITE);
	if (!g_cfg)
		return;
	h = CreateFileA("d3d9_sw.cfg", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;
	if (ReadFile(h, g_cfg, SS_CFG_MAX, &got, NULL))
		g_cfg_len = (int)got;
	CloseHandle(h);
}

unsigned savestate_getenv(const char *name, char *buf, unsigned cap)
{
	return (unsigned)ss_getenv(name, buf, (DWORD)cap);
}

/* Environment first, then the file. Returns the length written, 0 if unset. */
static DWORD ss_getenv(const char *name, char *buf, DWORD cap)
{
	DWORD n;
	int i, nlen;

	if (cap == 0)
		return 0;
	n = GetEnvironmentVariableA(name, buf, cap);
	if (n > 0 && n < cap)
		return n;
	/* Lazy, so a knob read before the first save still sees the file. */
	cfg_load();
	if (!g_cfg || g_cfg_len <= 0)
		return 0;
	for (nlen = 0; name[nlen]; nlen++)
		;
	i = 0;
	while (i < g_cfg_len) {
		int s = i, e, j, eq;

		while (i < g_cfg_len && g_cfg[i] != '\n')
			i++;
		e = i;
		if (i < g_cfg_len)
			i++;
		while (e > s && (g_cfg[e - 1] == '\r' || g_cfg[e - 1] == ' ' ||
				 g_cfg[e - 1] == '\t'))
			e--;
		while (s < e && (g_cfg[s] == ' ' || g_cfg[s] == '\t'))
			s++;
		if (s >= e || g_cfg[s] == '#' || g_cfg[s] == ';')
			continue;
		/* Whitespace is allowed between the name and the '=', because a config
		 * file people type by hand will have it. Without this, NAME = value
		 * silently did nothing while NAME=value worked - the worst kind of
		 * difference, since both look correct. */
		if (e - s <= nlen)
			continue;
		eq = s + nlen;
		while (eq < e && (g_cfg[eq] == ' ' || g_cfg[eq] == '\t'))
			eq++;
		if (eq >= e || g_cfg[eq] != '=')
			continue;
		for (j = 0; j < nlen; j++) {
			char a = g_cfg[s + j], b = name[j];

			if (a >= 'a' && a <= 'z')
				a = (char)(a - 32);
			if (b >= 'a' && b <= 'z')
				b = (char)(b - 32);
			if (a != b)
				break;
		}
		if (j != nlen)
			continue;
		{
			int vs = eq + 1;
			DWORD k = 0;

			while (vs < e && (g_cfg[vs] == ' ' || g_cfg[vs] == '\t'))
				vs++;
			while (vs < e && k < cap - 1)
				buf[k++] = g_cfg[vs++];
			buf[k] = 0;
			return k;
		}
	}
	return 0;
}
static HANDLE g_helper;

/* ---------------------------------------------------- formatting interception
 *
 * Hooked in ONE module, Mono's, and deliberately not everywhere. Every printf in
 * the process funnels through this function, our own logging included, so
 * patching it globally would either recurse or drown the log in our own output.
 * The question is about Mono, so only Mono is redirected.
 *
 * Records the destination and the size as well as the format, because the
 * standing bet on this crash is an integer overflow, and this is where such a
 * thing would be visible. __stdio_common_vsprintf takes a buffer count, and
 * plain sprintf legitimately passes (size_t)-1 for "unbounded" - so SIZE_MAX
 * here proves nothing on its own, while any *other* implausible count is
 * evidence. Logging the number is what separates those two, and guessing between
 * them is what this project keeps getting wrong. */
typedef int(__cdecl *StdioVsprintf)(unsigned long long opts, char *buf, size_t count,
				    const char *fmt, void *locale, va_list args);
static StdioVsprintf g_real_vsprintf;
static int g_fmt_said;

/* Checks the destination is writable before the runtime writes to it, so a bad
 * buffer is reported by name instead of arriving as an access violation inside
 * somebody else's code. Cheap: one VirtualQuery, and only the first page. */
static int fmt_dest_bad(char *buf)
{
	MEMORY_BASIC_INFORMATION mbi;

	if (!buf)
		return 1;
	if (VirtualQuery(buf, &mbi, sizeof(mbi)) != sizeof(mbi))
		return 1;
	if (mbi.State != MEM_COMMIT)
		return 1;
	if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
		return 1;
	return !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
				PAGE_EXECUTE_WRITECOPY));
}

static int __cdecl fmt_hook(unsigned long long opts, char *buf, size_t count, const char *fmt,
			    void *locale, va_list args)
{
	if (g_ctl) {
		DWORD tid = GetCurrentThreadId();
		unsigned slot = (unsigned)(tid % SS_FMT_SLOTS);

		g_ctl->fmt[slot].tid = tid;
		g_ctl->fmt[slot].fmt = fmt;
		g_ctl->fmt[slot].buf = buf;
		g_ctl->fmt[slot].count = count;
		g_ctl->fmt[slot].opts = opts;
		InterlockedIncrement(&g_ctl->fmt_calls);

		/* Reported before the call rather than after, because after may not
		 * happen. This is the line that would turn the recurring crash from a
		 * stack trace into a statement. */
		if (fmt_dest_bad(buf))
			ss_log("FORMAT: destination %p is not writable, count %llu, format "
			       "\"%.80s\" - this call would have faulted\n",
			       (void *)buf, (unsigned long long)count,
			       fmt && !fmt_dest_bad((char *)fmt) ? fmt : "(unreadable)");
	}
	return g_real_vsprintf(opts, buf, count, fmt, locale, args);
}

/* Prints what the faulting thread was last formatting. Called from the fault
 * handler, so it must assume every pointer it holds may be rubbish. */
static void fmt_report_for(DWORD tid)
{
	unsigned slot = (unsigned)(tid % SS_FMT_SLOTS);
	const char *f;

	if (!g_ctl || !g_ctl->fmt_calls || g_ctl->fmt[slot].tid != tid)
		return;
	f = g_ctl->fmt[slot].fmt;
	ss_raw("       last format on this thread: dest %p, count %llu, options %llx\n",
	       (void *)g_ctl->fmt[slot].buf, (unsigned long long)g_ctl->fmt[slot].count,
	       g_ctl->fmt[slot].opts);
	if (f && !fmt_dest_bad((char *)f))
		ss_raw("       format string: \"%.120s\"\n", f);
	else if (f)
		ss_raw("       format string at %p is unreadable\n", (void *)f);
}

static void fmt_hook_install(void)
{
	HMODULE mono = GetModuleHandleA("mono-2.0-bdwgc.dll");
	HMODULE crt = GetModuleHandleA("ucrtbase.dll");
	void *real;
	int n;

	if (g_real_vsprintf || !mono || !crt)
		return;
	real = (void *)GetProcAddress(crt, "__stdio_common_vsprintf");
	if (!real)
		real = (void *)GetProcAddress(crt, "_stdio_common_vsprintf");
	if (!real)
		return;
	g_real_vsprintf = (StdioVsprintf)real;
	/* Both, for the same reason the unwind hooks needed both: an import table
	 * entry is the normal case, a cached pointer in the module's own data is
	 * what Mono actually did last time. */
	n = patch_iat(mono, real, (void *)fmt_hook);
#if !defined(_M_IX86) && !defined(__i386__)
	n += patch_data_ptr(mono, real, (void *)fmt_hook);
#endif
	if (!n) {
		/* Not a hooking failure. Mono's import table names no C runtime at all -
		 * MSWSOCK, WS2_32, ole32, OLEAUT32, PSAPI, VERSION, ADVAPI32, WINMM,
		 * KERNEL32, USER32, SHELL32 and nothing else - so it cannot be reaching
		 * ucrtbase's printf through an import, and there is no site to redirect.
		 * Said once rather than every guard cycle, because the answer will not
		 * change while this module is loaded. */
		if (!g_fmt_said) {
			g_fmt_said = 1;
			ss_log("hooks: formatting NOT redirected - mono imports no C runtime, "
			       "so it is not the caller of the printf machinery\n");
		}
		g_real_vsprintf = NULL;
		return;
	}
	ss_log("hooks: formatting redirected in mono at %d site(s)\n", n);
}


/* Plain digits, no padding - the caller pads, so that one place handles width
 * for every conversion instead of each one doing it slightly differently. */
static char *ss_digits(char *p, char *end, unsigned long long v, unsigned base, int upper)
{
	const char *d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char t[24];
	int n = 0;

	do {
		t[n++] = d[v % base];
		v /= base;
	} while (v && n < (int)sizeof(t));
	while (n-- > 0 && p < end)
		*p++ = t[n];
	return p;
}

/* Exactly `digits` characters, zero-filled on the left. Only the fractional
 * part of a float needs this: 0 at two places is ".00", not ".0" and not "." */
static char *ss_frac(char *p, char *end, unsigned long long v, int digits)
{
	char t[24];
	int n = 0;

	while (n < digits && n < (int)sizeof(t)) {
		t[n++] = (char)('0' + (int)(v % 10));
		v /= 10;
	}
	while (n-- > 0 && p < end)
		*p++ = t[n];
	return p;
}

/* One CRT-free formatter, shared by the ordinary log and the fault log.
 *
 * Both writers already avoided stdio because a FILE and its lock live on the
 * CRT heap, which the restore rewinds. But ss_log still called vsnprintf, and
 * that is ucrtbase!__stdio_common_vsprintf - the formatting machinery, whose
 * locale and internal state ALSO live on the rewound heap. A hundred-run batch
 * put a number on what that costs: 30 of 49 faults had a real unwound ucrtbase
 * frame, and five of the seven most frequent fault sites were printf internals.
 * The majority of our crashes were our own logging dying on a thread that had
 * just been rewound.
 *
 * Unifying rather than duplicating, because the fault path's formatter had
 * three bugs of its own that this inventory turned up, all of them on lines the
 * fault log actually prints: it skipped the `l` modifiers and then read
 * `unsigned long`, which is 32 bits on Win64, so every %llu and %llx was cut in
 * half - the identical bug ss_num was written to fix, one level up. And %.120s
 * fell through to the default case and printed a bare '%'.
 *
 * Supports what the 105 call sites between them actually use, which was
 * measured rather than assumed: flags - + 0 and space, a numeric width, a
 * precision (fraction digits for floats, a truncation limit for strings), the
 * l/ll/h/z length modifiers, and d i u x X p s f %%. Anything else emits a '%'
 * so a mistake shows up in the log instead of being silently dropped.
 *
 * No CRT call anywhere in here, and no static storage, so it is safe on a
 * restored thread and inside the suspended window. */
static int ss_vfmt(char *buf, int cap, const char *fmt, va_list ap)
{
	char *p = buf, *end = buf + cap;

	while (*fmt && p < end) {
		char body[64];
		const char *s = NULL;
		int minus = 0, plus = 0, zero = 0, width = 0, prec = -1, lmod = 0;
		int neg = 0, upper = 0, isstr = 0, blen = -1;
		unsigned base = 10;
		unsigned long long uv = 0;

		if (*fmt != '%') {
			*p++ = *fmt++;
			continue;
		}
		fmt++;
		for (;; fmt++) {
			if (*fmt == '-')
				minus = 1;
			else if (*fmt == '+')
				plus = 1;
			else if (*fmt == '0')
				zero = 1;
			else if (*fmt != ' ')
				break;
		}
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			while (*fmt >= '0' && *fmt <= '9')
				prec = prec * 10 + (*fmt++ - '0');
		}
		while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 'j') {
			/* z and j jump straight to the wide read rather than counting
			 * as one l. size_t and intmax_t are 64 bits on x64, so a
			 * single increment would have read 32 and silently halved
			 * them - which is what the code did while the comment below
			 * claimed otherwise. No call site uses %zu today, so this was
			 * a landmine rather than a live bug, and it is the same
			 * mistake this formatter was written to fix. */
			if (*fmt == 'z' || *fmt == 'j')
				lmod = 2;
			else if (*fmt == 'l')
				lmod++;
			fmt++;
		}
		switch (*fmt++) {
		case 'p':
			/* Width and fill are forced: an address is only comparable
			 * against another address when both are the same shape. */
			uv = (unsigned long long)(uintptr_t)va_arg(ap, void *);
			base = 16;
			upper = 1;
			width = (int)(sizeof(void *) * 2);
			zero = 1;
			minus = 0;
			break;
		case 'X':
			upper = 1;
			/* fall through */
		case 'x':
			base = 16;
			uv = (lmod >= 2) ? va_arg(ap, unsigned long long)
					 : (unsigned long long)va_arg(ap, unsigned int);
			break;
		case 'u':
			uv = (lmod >= 2) ? va_arg(ap, unsigned long long)
					 : (unsigned long long)va_arg(ap, unsigned int);
			break;
		case 'i':
		case 'd': {
			/* `long` is 32 bits on Windows for both architectures, so a
			 * single `l` reads an int. Only `ll` (and %zu/%ju, folded in
			 * above) widens. */
			long long v = (lmod >= 2) ? va_arg(ap, long long)
						  : (long long)va_arg(ap, int);

			if (v < 0) {
				neg = 1;
				uv = (unsigned long long)(-v);
			} else {
				uv = (unsigned long long)v;
			}
			break;
		}
		case 'f': {
			double d = va_arg(ap, double);
			unsigned long long scale = 1, whole, frac;
			char *q = body;
			int k;

			if (prec < 0)
				prec = 6;
			if (prec > 9)
				prec = 9;
			if (d < 0.0) {
				neg = 1;
				d = -d;
			}
			for (k = 0; k < prec; k++)
				scale *= 10;
			/* Everything printed here is megabytes or milliseconds, so
			 * this cannot trip in practice - but a NaN or an infinity
			 * converts to a meaningless integer rather than failing, and
			 * a diagnostic that quietly prints a wrong number is worse
			 * than one that says it does not know. Written as !(d < lim)
			 * on purpose: a NaN compares false against everything. */
			if (!(d < 1.0e15)) {
				s = "?";
				isstr = 1;
				prec = -1;
				break;
			}
			{
				unsigned long long t =
					(unsigned long long)(d * (double)scale + 0.5);
				whole = t / scale;
				frac = t % scale;
			}
			q = ss_digits(q, body + sizeof(body), whole, 10, 0);
			if (prec > 0) {
				if (q < body + sizeof(body))
					*q++ = '.';
				q = ss_frac(q, body + sizeof(body), frac, prec);
			}
			blen = (int)(q - body);
			break;
		}
		case 's':
			s = va_arg(ap, const char *);
			if (!s)
				s = "(null)";
			isstr = 1;
			break;
		case '%':
			body[0] = '%';
			blen = 1;
			break;
		default:
			if (p < end)
				*p++ = '%';
			continue;
		}
		if (isstr) {
			blen = 0;
			while (s[blen] && (prec < 0 || blen < prec))
				blen++;
			zero = 0;
		} else if (blen < 0) {
			blen = (int)(ss_digits(body, body + sizeof(body), uv, base,
					       upper) - body);
		}
		{
			int total = blen + ((neg || plus) ? 1 : 0);
			int pad = width > total ? width - total : 0;
			int k;

			if (!minus && !zero)
				while (pad-- > 0 && p < end)
					*p++ = ' ';
			if (neg && p < end)
				*p++ = '-';
			else if (plus && p < end)
				*p++ = '+';
			/* Zero fill goes AFTER the sign, or -0012 becomes 00-12. */
			if (!minus && zero)
				while (pad-- > 0 && p < end)
					*p++ = '0';
			for (k = 0; k < blen && p < end; k++)
				*p++ = isstr ? s[k] : body[k];
			if (minus)
				while (pad-- > 0 && p < end)
					*p++ = ' ';
		}
	}
	return (int)(p - buf);
}

/* Bounded, NUL-terminating, CRT-free replacement for _snprintf. ss_vfmt does not
 * terminate (its callers write an exact byte count to a file), so that is done
 * here and the cap is reduced by one to make room for it. */
static int ss_fmt(char *buf, int cap, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (cap <= 0)
		return 0;
	va_start(ap, fmt);
	n = ss_vfmt(buf, cap - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		n = 0;
	buf[n] = 0;
	return n;
}

/* WriteFile rather than stdio: a FILE lives on the CRT heap, which the restore
 * rewinds. And now the formatting is ours too, for the same reason one level
 * deeper - see ss_vfmt. */
static void ss_log(const char *fmt, ...)
{
	char buf[512];
	int n;
	va_list ap;
	DWORD wrote;

	if (!g_ctl || g_ctl->log == INVALID_HANDLE_VALUE || !g_ctl->log)
		return;
	va_start(ap, fmt);
	n = ss_vfmt(buf, (int)sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0)
		WriteFile(g_ctl->log, buf, (DWORD)n, &wrote, NULL);
}

/* The fault path's writer. Identical to ss_log now that both share ss_vfmt, and
 * kept as a separate name because the call sites carry meaning: ss_raw marks the
 * places that run inside a fault handler, where the rule is no allocation, no
 * locks and no CRT.
 *
 * It used to have its own cut-down formatter, which was the whole point of it.
 * That formatter got %llu, %llx and %.120s wrong - all three appear in the fault
 * log - so the separation was costing correctness rather than buying safety. */
static void ss_raw(const char *fmt, ...)
{
	char buf[512];
	int n;
	va_list ap;
	DWORD wrote;

	if (!g_ctl || !g_ctl->log || g_ctl->log == INVALID_HANDLE_VALUE)
		return;
	va_start(ap, fmt);
	n = ss_vfmt(buf, (int)sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0)
		WriteFile(g_ctl->log, buf, (DWORD)n, &wrote, NULL);
}

static const char *ss_module(uintptr_t v, unsigned *off)
{
	int m;

	*off = 0;
	if (!g_ctl || g_ctl->nmods <= 0)
		return NULL;
	m = module_of(v);
	if (m < 0)
		return NULL;
	*off = (unsigned)(v - g_ctl->mod_lo[m]);
	return g_ctl->mod_name[m];
}

static void ss_exclude(void *p, size_t n)
{
	if (!g_ctl)
		return;
	if (g_ctl->nex >= SS_MAX_EXCL) {
		/* Dropping one silently would leave memory rewound that the rest
		 * of the run assumes is held, which is the failure this list
		 * exists to prevent. */
		static int said;
		if (!said) {
			said = 1;
			ss_log("  exclusions: list full at %d, %p not held\n", SS_MAX_EXCL, p);
		}
		return;
	}
	g_ctl->ex_lo[g_ctl->nex] = (uintptr_t)p;
	g_ctl->ex_hi[g_ctl->nex] = (uintptr_t)p + n;
	g_ctl->nex++;
}

static int region_excluded(uintptr_t base, uintptr_t size)
{
	uintptr_t end = base + size;
	LONG i;
	if (!g_ctl)
		return 0;
	if (end > g_ctl->helper_lo && base < g_ctl->helper_hi)
		return 1; /* the stack this code is running on */
	for (i = 0; i < g_ctl->nex; i++)
		if (end > g_ctl->ex_lo[i] && base < g_ctl->ex_hi[i])
			return 1;
	return 0;
}

/* Which side of the restore a register's target came from.
 *
 * The failures left are seams rather than corruption: a pointer restored out of
 * the snapshot aiming at memory deliberately left in the present, or the
 * reverse. A bare "the vtable pointer was null" cannot tell those apart, but the
 * provenance of the object address can, so every register pointing at mapped
 * memory gets named and placed. Allocates nothing and takes no lock, because the
 * fault it is describing can be inside the allocator with the heap lock held.
 *
 * 64-bit only, matching its one caller. */
/* Names an address if it is one WE resolved or installed.
 *
 * Twice now a pointer that should address data has instead addressed a module
 * image: rcx holding an ntdll code address at RtlEnterCriticalSection+0x4a where
 * a CRITICAL_SECTION * belongs, and a sprintf destination inside ucrtbase's
 * image. The obvious suspicion is our own hooking, because we deliberately write
 * function addresses into data slots all over the process - patch_iat and
 * patch_data_ptr do exactly that - and a data slot we should not have touched
 * would look precisely like this.
 *
 * That is a suspicion, not a finding, and this function exists so it stops being
 * either. If the garbage pointer is one of the addresses we deal in, our hooking
 * is implicated by name. If it is not, our hooking is cleared and the search
 * moves on, which given the record of theories here is the more likely and the
 * more useful outcome. */
static const char *ss_ours(uintptr_t v)
{
	struct {
		void *p;
		const char *what;
	} known[] = {
		{ (void *)g_real_cs_init, "the real InitializeCriticalSection, which we resolved" },
		{ (void *)g_real_cs_initsc, "the real InitializeCriticalSectionAndSpinCount" },
		{ (void *)g_real_cs_initex, "the real InitializeCriticalSectionEx" },
		{ (void *)g_real_cs_del, "the real DeleteCriticalSection, which we resolved" },
		{ (void *)hook_cs_init, "OUR InitializeCriticalSection hook" },
		{ (void *)hook_cs_initsc, "OUR AndSpinCount hook" },
		{ (void *)hook_cs_initex, "OUR Ex hook" },
		{ (void *)hook_cs_del, "OUR DeleteCriticalSection hook" },
#if !defined(_M_IX86) && !defined(__i386__)
		/* The growable unwind tables are an x64 mechanism; there is nothing to
		 * name on x86 and the hooks do not exist there. */
		{ (void *)g_real_fnadd, "the real RtlAddGrowableFunctionTable" },
		{ (void *)g_real_fngrow, "the real RtlGrowFunctionTable" },
		{ (void *)g_real_fndel, "the real RtlDeleteGrowableFunctionTable" },
		{ (void *)hook_fnadd, "OUR RtlAddGrowableFunctionTable hook" },
		{ (void *)hook_fngrow, "OUR RtlGrowFunctionTable hook" },
		{ (void *)hook_fndel, "OUR RtlDeleteGrowableFunctionTable hook" },
#endif
	};
	unsigned i;

	for (i = 0; i < sizeof(known) / sizeof(known[0]); i++)
		if (known[i].p && (uintptr_t)known[i].p == v)
			return known[i].what;
	return NULL;
}

#if !defined(_M_IX86) && !defined(__i386__)
static void ss_where_reg(const char *name, uintptr_t v)
{
	MEMORY_BASIC_INFORMATION mbi;
	const char *side = "in neither the save nor the held set";
	const char *heap;

	/* Below the lowest mappable address this is a small integer, not a pointer. */
	if (v < 0x10000)
		return;
	if (VirtualQuery((LPCVOID)v, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State == MEM_FREE)
		return;
	if (g_ctl) {
		int k, i, found = 0;

		for (k = 0; k < SAVESTATE_SLOTS && !found; k++) {
			const Slot *s = &g_ctl->slots[k];

			if (!s->valid)
				continue;
			for (i = 0; i < s->nregs; i++)
				if (v >= s->regs[i].base &&
				    v - s->regs[i].base < s->regs[i].size) {
					side = "restored from the save";
					found = 1;
					break;
				}
		}
	}
	/* Checked second because it is the narrower claim: an excluded range can sit
	 * inside a region the save also recorded, and being held is what matters. */
	if (region_excluded(v, 1))
		side = "held in the present";
	heap = ss_heap_of(v);
	ss_raw("       %s=%p %s%s, %s", name, (void *)v,
	       mbi.State == MEM_COMMIT ? "committed" : "reserved only", heap ? heap : "", side);
	/* The first word of an object is its vtable pointer, and that one word
	 * separates explanations this whole investigation has been guessing
	 * between. A plausible code address means the object is intact and the
	 * fault is elsewhere. An allocator fill pattern means it was freed or
	 * never initialised, which is use-after-free and names the seam. Zero
	 * means it was cleared. Guessing between those cost the dump analysis an
	 * evening, and it is eight bytes we can simply read. */
	if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
		unsigned long long w0 = *(const unsigned long long *)v;
		unsigned lo32 = (unsigned)(w0 & 0xFFFFFFFFu);
		const char *what = "";
		unsigned modoff = 0;

		if (!w0)
			what = " (zeroed)";
		else if (lo32 == 0xFEEEFEEEu)
			what = " (FEEEFEEE, freed by the CRT)";
		else if (lo32 == 0xBAADF00Du)
			what = " (BAADF00D, allocated but never written)";
		else if (lo32 == 0xCDCDCDCDu || lo32 == 0xDDDDDDDDu || lo32 == 0xABABABABu)
			what = " (debug heap fill)";
		else if (ss_module((uintptr_t)w0, &modoff))
			what = " (a module address, so plausibly a real vtable)";
		ss_raw(", first word %p%s", (void *)(uintptr_t)w0, what);
		{
			const char *mine = ss_ours((uintptr_t)w0);

			if (mine)
				ss_raw(" -- that word is %s", mine);
		}
	}
	{
		const char *mine = ss_ours(v);

		if (mine)
			ss_raw(" -- THIS REGISTER IS %s", mine);
	}
	ss_raw("\n");
}
#endif

/* Writable, committed, and actually part of the program's state. Image regions
 * carry the game's globals, so a naive "private memory only" filter would
 * silently miss them. Mapped views are skipped: they are usually shared with
 * another process, where a rewind has no meaning, and our own snapshot window
 * is one of them. */
static int region_wanted(const MEMORY_BASIC_INFORMATION *mbi)
{
	DWORD p = mbi->Protect;
	if (mbi->State != MEM_COMMIT)
		return 0;
	if (mbi->Type != MEM_PRIVATE && mbi->Type != MEM_IMAGE)
		return 0;
	if (p & (PAGE_GUARD | PAGE_NOACCESS))
		return 0; /* touching a guard page would arm a stack growth */
	if (!(p & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
		   PAGE_EXECUTE_WRITECOPY)))
		return 0;
	return !region_excluded((uintptr_t)mbi->BaseAddress, mbi->RegionSize);
}

typedef struct THREAD_BASIC_INFO {
	LONG ExitStatus;
	PVOID TebBaseAddress;
	PVOID UniqueProcess;
	PVOID UniqueThread;
	ULONG_PTR AffinityMask;
	LONG Priority;
	LONG BasePriority;
} THREAD_BASIC_INFO;

typedef LONG(NTAPI *PFN_NtQueryThread)(HANDLE, ULONG, PVOID, ULONG, PULONG);

typedef struct Slot Slot;

static void ss_log(const char *fmt, ...);
static int rewind_all_threads(void);
static int reclaim_tier(void);
static void guard_release(void);
static void do_reclaim(Slot *s, int tier);

static PFN_NtQueryThread query_thread(void)
{
	static PFN_NtQueryThread fn;
	if (!fn) {
		HMODULE nt = GetModuleHandleA("ntdll.dll");
		if (nt)
			fn = (PFN_NtQueryThread)(void *)GetProcAddress(
				nt, "NtQueryInformationThread");
	}
	return fn;
}

static PVOID teb_of(HANDLE thread)
{
	PFN_NtQueryThread fn = query_thread();
	THREAD_BASIC_INFO tbi;
	if (!fn)
		return NULL;
	memset(&tbi, 0, sizeof(tbi));
	if (fn(thread, 0 /* ThreadBasicInformation */, &tbi, sizeof(tbi), NULL) < 0)
		return NULL;
	return tbi.TebBaseAddress;
}

/* A thread's entry point identifies its role, which survives the thread itself.
 * Thread ids do not: a pool that retires one worker and starts another leaves
 * the process doing the same work under a different id, and matching on id
 * alone would call that a divergence when nothing meaningful changed. */
static PVOID start_of(HANDLE thread)
{
	PFN_NtQueryThread fn = query_thread();
	PVOID addr = NULL;
	if (!fn)
		return NULL;
	if (fn(thread, 9 /* ThreadQuerySetWin32StartAddress */, &addr, sizeof(addr), NULL) < 0)
		return NULL;
	return addr;
}

/* Everything the rewind must not touch, rebuilt per operation because the
 * thread set changes.
 *
 * The first restore attempt wrote all three of these back and killed the
 * process even though every region reported success, so the blast radius is
 * now deliberately narrow.
 *
 * TEBs are kernel-managed but user-visible. Rewinding one desynchronises the
 * kernel's idea of a thread from the thread's own, and on x86 it also drags
 * back the SEH chain head, which then points at stack frames that no longer
 * exist.
 *
 * Stacks other than the requester's belong to threads suspended at arbitrary
 * instructions. SetThreadContext does not reliably take on a thread parked
 * inside a syscall - the kernel reinstates its own trap frame when the wait
 * completes - so a rewound stack under a live context is a wild return waiting
 * to happen. Those threads keep their own stacks and are left running; the
 * game's actual state lives in globals and the heap, which are still rewound.
 *
 * Two classes of module image are left alone. System libraries hold loader and
 * runtime bookkeeping shared with the kernel, and rewinding a critical section
 * to "free" under a thread about to release it corrupts the lock for good.
 * Steam and its overlay run their own threads on their own schedule, so their
 * globals must not move either; splitting purely on the Windows directory
 * missed that and cost a process.
 *
 * Everything else that ships with the game is rewound, and MSVCR100 is the
 * reason the rule is not simply "executable only". It is the game's C runtime:
 * its data section holds the heap handle and pointers into the very heap being
 * restored, so excluding it would split the allocator's bookkeeping from the
 * memory it describes. The same argument covers this wrapper. */
static char g_steam_dir[MAX_PATH];
static UINT g_steam_len;
static char g_game_dir[MAX_PATH];
static UINT g_game_len;

static int module_excluded(const char *path, const char *windir, UINT wlen)
{
	const char *name = strrchr(path, '\\');
	name = name ? name + 1 : path;
	if (wlen && _strnicmp(path, windir, wlen) == 0)
		return 1;
	if (_strnicmp(name, "steam", 5) == 0 || _strnicmp(name, "gameoverlay", 11) == 0)
		return 1;
	/* The rest of the Steam client, which is not named after it.
	 *
	 * Matching "steam" and "gameoverlay" by filename catches the client and
	 * the overlay and misses the layer they are built on. vstdlib_s and
	 * tier0_s are Valve's base libraries - threading, allocation, timing -
	 * and steamclient.dll is nothing but a caller of them. Leaving the caller
	 * in the present while winding its foundation back is the same split that
	 * has been behind every failure here, and Windows named it directly: an
	 * access violation inside C:\Program Files (x86)\Steam\vstdlib_s.dll.
	 *
	 * Adding those two names to the list above would fix the crash we have
	 * seen and leave the next one to be found the same way. What actually
	 * defines this set is where the modules came from, so that is what gets
	 * tested.
	 *
	 * The game's directory has to be carved back out of that test, because a
	 * default install puts it at <Steam>\steamapps\common\<game> - underneath
	 * the very prefix this matches on. Without the exemption the rule reaches
	 * past the client and swallows everything shipped with the game, MSVCR100
	 * among it, which is the one exclusion the paragraph above this function
	 * says must never happen. It also cost every restore in a session: the
	 * game's own worker threads start inside the C runtime, an excluded image
	 * makes them read as operating-system threads, and their contexts were
	 * then left in the present while the memory around them went back.
	 *
	 * steam_api.dll ships beside the executable and stays excluded regardless,
	 * caught by name before this test is reached. */
	if (g_steam_len && _strnicmp(path, g_steam_dir, g_steam_len) == 0 &&
	    !(g_game_len && _strnicmp(path, g_game_dir, g_game_len) == 0))
		return 1;
	/* The device runtimes, wherever they were loaded from.
	 *
	 * Input and audio redistributables talk to drivers and to the operating
	 * system's own threads, not to the game's data, so they belong on the same
	 * side of the line as the copies of themselves that live in the Windows
	 * directory - which is where the rule had been catching them by accident.
	 * This game ships its own XInput next to the executable, so it fell
	 * through and got rewound instead, and the laptop died proving why: the
	 * runtime keeps per-thread data on a private heap, an operating system
	 * thread-pool worker had a block on it, and the rewind put that heap back
	 * to before the block existed. The pointer to it lives in the thread
	 * block, which we hold, so nothing noticed until the worker exited and its
	 * cleanup callback tried to free memory the heap no longer believed it had
	 * handed out.
	 *
	 * There is no rewinding our way out of that one. A thread we deliberately
	 * leave running in the present cannot be allowed to own memory we take
	 * back, and the runtime it allocated from has to stay with it. The game's
	 * own view of the controller is in the game's memory and still travels.
	 *
	 * d3dx9 is pointedly not on this list: it is a helper that operates on the
	 * game's device and its objects belong with the game. */
	if (_strnicmp(name, "xinput", 6) == 0 || _strnicmp(name, "dinput", 6) == 0 ||
	    _strnicmp(name, "xaudio", 6) == 0 || _strnicmp(name, "x3daudio", 8) == 0)
		return 1;
	return 0;
}

/* Where the Steam client was installed, learned from a module that can only
 * have been loaded out of it.
 *
 * Taken from the module snapshot the caller already holds rather than by asking
 * the loader, because this runs with the process suspended and a loader call
 * there can meet a lock a stopped thread is holding. steamclient.dll is the
 * witness of choice: it is always in the client root, whereas steam_api.dll
 * ships beside the game and would name the wrong directory entirely. */
static void find_steam_dir(HANDLE snap)
{
	MODULEENTRY32 me;

	if (g_steam_len)
		return;
	me.dwSize = sizeof(me);
	if (!Module32First(snap, &me))
		return;
	do {
		const char *name = strrchr(me.szExePath, '\\');
		char *slash;

		name = name ? name + 1 : me.szExePath;
		if (_stricmp(name, "steamclient.dll") != 0 &&
		    _stricmp(name, "steam.dll") != 0)
			continue;
		lstrcpynA(g_steam_dir, me.szExePath, sizeof(g_steam_dir));
		slash = strrchr(g_steam_dir, '\\');
		if (!slash)
			continue;
		slash[1] = 0;
		g_steam_len = (UINT)strlen(g_steam_dir);
		return;
	} while (Module32Next(snap, &me));
}

/* Where the game was started from, read off the same snapshot for the same
 * reason: GetModuleFileName goes through the loader, and this runs with every
 * other thread stopped. The executable's own entry names the directory that
 * everything shipping with the game was loaded out of. */
static void find_game_dir(HANDLE snap, HMODULE exe)
{
	MODULEENTRY32 me;

	if (g_game_len)
		return;
	me.dwSize = sizeof(me);
	if (!Module32First(snap, &me))
		return;
	do {
		char *slash;
		if ((HMODULE)me.modBaseAddr != exe)
			continue;
		lstrcpynA(g_game_dir, me.szExePath, sizeof(g_game_dir));
		slash = strrchr(g_game_dir, '\\');
		if (!slash)
			return;
		slash[1] = 0;
		g_game_len = (UINT)strlen(g_game_dir);
		return;
	} while (Module32Next(snap, &me));
}

/* Which library a thread belongs to, folded into a running tally. Attribution
 * matters because these threads keep their working state in the process heap,
 * which the rewind takes back underneath them - so knowing whether they belong
 * to the OS thread pool, to Steam, or to something we invited in decides whether
 * anything can be done about it. */
static void tally_owner(char names[8][64], int *counts, int *n, PVOID start)
{
	MEMORY_BASIC_INFORMATION mbi;
	char path[MAX_PATH] = "?";
	const char *base;
	int k;

	if (VirtualQuery(start, &mbi, sizeof(mbi)) == sizeof(mbi))
		GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, sizeof(path));
	base = strrchr(path, '\\');
	base = base ? base + 1 : path;
	for (k = 0; k < *n; k++) {
		if (lstrcmpiA(names[k], base) == 0) {
			counts[k]++;
			return;
		}
	}
	if (*n >= 8)
		return;
	lstrcpynA(names[*n], base, 64);
	counts[*n] = 1;
	(*n)++;
}

/* Heaps the game does not own.
 *
 * Leaving a module in the present only protects the half of its state that
 * lives in its own image. The other half is on a heap, and every heap in the
 * process was being rewound regardless of who allocated from it. A library we
 * deliberately refuse to move in time keeps a pointer in its untouched .data to
 * a block the rewind has just handed back to the free list, and reads it later
 * against a heap that no longer agrees the block exists.
 *
 * Windows Error Reporting named the mechanism outright: the silent deaths were
 * 0xC0000409 raised from ntdll with fail-fast code 3, a corrupt list entry.
 * That is the heap validating its own doubly-linked free list and finding links
 * that do not point back. It is a fail-fast, which is why nothing was ever
 * logged - it bypasses vectored handlers and every exit path we hook, and kills
 * the process where it stands. RPCRT4 raised its own fail-fast in the same
 * session, and MSCTF's access violation on the other machine is the gentler
 * version of the same story.
 *
 * So the line is not drawn around libraries, it is drawn around allocators. The
 * process default heap belongs to Windows: ntdll, RPC, COM and text services
 * all keep their lists in it, and rewinding those lists is what kills us. The C
 * runtime heaps are private to their runtime - MSVCR100's serves the game's
 * malloc and new, msvcrt's serves this wrapper - and those are the ones that
 * have to travel with the game.
 *
 * The cost of the trade is any game state that came from the process heap
 * directly rather than through the runtime, which for a title of this vintage
 * should be nothing, but is the thing to suspect if saves start restoring
 * subtly wrong instead of crashing. */
static uintptr_t alloc_span(uintptr_t base)
{
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t p = base, end = base;

	while (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == sizeof(mbi) &&
	       (uintptr_t)mbi.AllocationBase == base) {
		end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		if (end <= p)
			break;
		p = end;
	}
	return end - base;
}

#define SW_ALIGN 64

static HANDLE g_swheap;

HANDLE sw_heap(void)
{
	if (!g_swheap) {
		HANDLE h = HeapCreate(0, 1u << 20, 0);
		if (h && InterlockedCompareExchangePointer((PVOID *)&g_swheap, h, NULL))
			HeapDestroy(h); /* lost the race, someone else's is live */
	}
	return g_swheap;
}

void *sw_malloc(size_t n)
{
	unsigned char *raw, *p;
	if (n + SW_ALIGN < n)
		return NULL;
	raw = (unsigned char *)HeapAlloc(sw_heap(), 0, n + SW_ALIGN);
	if (!raw)
		return NULL;
	p = (unsigned char *)(((uintptr_t)raw + SW_ALIGN) & ~(uintptr_t)(SW_ALIGN - 1));
	((void **)p)[-1] = raw;
	return p;
}

void *sw_calloc(size_t count, size_t size)
{
	size_t n = count * size;
	void *p;
	if (count && n / count != size)
		return NULL;
	p = sw_malloc(n);
	if (p)
		memset(p, 0, n);
	return p;
}

void sw_free(void *p)
{
	if (p)
		HeapFree(sw_heap(), 0, ((void **)p)[-1]);
}

/* HeapSize reports the raw block, which is the request plus the alignment slack,
 * so subtracting the slack gives a length that is never longer than the old
 * block and never reads past it. */
void *sw_realloc(void *p, size_t n)
{
	size_t old;
	void *q;

	if (!p)
		return sw_malloc(n);
	if (!n) {
		sw_free(p);
		return NULL;
	}
	old = HeapSize(sw_heap(), 0, ((void **)p)[-1]);
	q = sw_malloc(n);
	if (!q)
		return NULL;
	old = (old == (size_t)-1 || old < SW_ALIGN) ? 0 : old - SW_ALIGN;
	memcpy(q, p, old < n ? old : n);
	sw_free(p);
	return q;
}

/* The handle of a C runtime's heap, asked of the runtime itself rather than
 * guessed. A runtime that is not loaded simply contributes nothing. */
static HANDLE crt_heap(const char *dll)
{
	HMODULE m = GetModuleHandleA(dll);
	intptr_t(__cdecl * get)(void);

	if (!m)
		return NULL;
	get = (intptr_t(__cdecl *)(void))(void *)GetProcAddress(m, "_get_heap_handle");
	return get ? (HANDLE)get() : NULL;
}

/* The D3D9 title's runtime is tried first so its partition is unchanged; a
 * Unity player links the UCRT instead and would otherwise leave the rewind with
 * no anchored game heap at all. */
static HANDLE game_crt_heap(void)
{
	size_t i;
	for (i = 0; i < sizeof(kCrtNames) / sizeof(kCrtNames[0]); i++) {
		HANDLE h = crt_heap(kCrtNames[i]);
		if (h) {
			/* Whether the runtime shares the process heap decides who
			 * really owns it. Windows keeps the dynamic function table
			 * and critical section lists there, and their heads live in
			 * ntdll's image, which is never rewound; rewinding the nodes
			 * alone is what raises FAST_FAIL_CORRUPT_LIST_ENTRY. */
			ss_log("    game runtime heap %p from %s%s\n", h, kCrtNames[i],
			       h == GetProcessHeap() ? ", which IS the process heap"
						     : ", separate from the process heap");
			return h;
		}
	}
	ss_log("    no game runtime heap found; relying on module votes\n");
	return NULL;
}

/* Every segment of every heap, not just the first.
 *
 * A heap handle is the address of its first segment, so GetProcessHeaps alone
 * finds one segment each and leaves the rest of a grown heap being rewound -
 * which is most of it, and enough to corrupt the same lists. The segments are
 * findable without asking the heap: each begins with a _HEAP_SEGMENT carrying a
 * fixed signature and a pointer back to its owner. Reading it is safe in a way
 * HeapWalk is not, since HeapWalk takes the heap's lock and we run with every
 * thread suspended, one of which may be holding it. */
static void *segment_owner(uintptr_t base, DWORD protect)
{
#if defined(_M_IX86) || defined(__i386__)
	const unsigned sig_at = 0x08, heap_at = 0x18;
#else
	const unsigned sig_at = 0x10, heap_at = 0x28;
#endif
	if (protect & (PAGE_NOACCESS | PAGE_GUARD))
		return NULL;
	if (*(const unsigned *)(base + sig_at) != 0xFFEEFFEEu)
		return NULL;
	return *(void *const *)(base + heap_at);
}

/* Which module a heap belongs to, decided by what its contents point at.
 *
 * There is no way to ask a heap who created it, and hooking HeapCreate is too
 * late for anything made before we load. But a heap full of C++ objects is a
 * heap full of vtable pointers, and those point straight into the image of
 * whoever allocated them. Counting them names the owner well enough to decide
 * which way it travels in time.
 *
 * This matters because the two halves of a library have to agree. Rewinding
 * d3dx9's code while leaving its heap in the present left an effect object
 * holding a state block we had already taken back, and it called through the
 * hole the next time it drew. The rule that follows is simply that a heap
 * rewinds when the module that owns it rewinds. */
static int module_of(uintptr_t v)
{
	int lo = 0, hi = g_ctl->nmods - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (v < g_ctl->mod_lo[mid])
			hi = mid - 1;
		else if (v >= g_ctl->mod_hi[mid])
			lo = mid + 1;
		else
			return mid;
	}
	return -1;
}

static void sort_modules(void)
{
	int i, j;
	for (i = 1; i < g_ctl->nmods; i++) {
		uintptr_t lo = g_ctl->mod_lo[i], hi = g_ctl->mod_hi[i];
		char rw = g_ctl->mod_rewound[i];
		char nm[32];
		memcpy(nm, g_ctl->mod_name[i], sizeof(nm));
		for (j = i; j > 0 && g_ctl->mod_lo[j - 1] > lo; j--) {
			g_ctl->mod_lo[j] = g_ctl->mod_lo[j - 1];
			g_ctl->mod_hi[j] = g_ctl->mod_hi[j - 1];
			g_ctl->mod_rewound[j] = g_ctl->mod_rewound[j - 1];
			memcpy(g_ctl->mod_name[j], g_ctl->mod_name[j - 1], sizeof(nm));
		}
		g_ctl->mod_lo[j] = lo;
		g_ctl->mod_hi[j] = hi;
		g_ctl->mod_rewound[j] = rw;
		memcpy(g_ctl->mod_name[j], nm, sizeof(nm));
	}
}

static int heap_claimed_by(HANDLE h, int *votes)
{
	MEMORY_BASIC_INFORMATION mbi;
	int counts[SS_MAX_MODS];
	int i, best = -1, bestn = 0;

	memset(counts, 0, sizeof(counts));
	for (i = 0; i < g_ctl->nseg; i++) {
		uintptr_t p, end;
		if (g_ctl->seg_owner[i] != h)
			continue;
		p = g_ctl->seg_base[i];
		end = p + g_ctl->seg_size[i];
		while (p < end && VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
			uintptr_t rend = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
			if (rend > end)
				rend = end;
			if (rend <= p)
				break;
			if (mbi.State == MEM_COMMIT &&
			    !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
				const uintptr_t *w = (const uintptr_t *)p;
				size_t k, n = (size_t)(rend - p) / sizeof(uintptr_t);
				for (k = 0; k < n; k++) {
					int m = module_of(w[k]);
					if (m >= 0)
						counts[m]++;
				}
			}
			p = rend;
		}
	}
	for (i = 0; i < g_ctl->nmods; i++)
		if (counts[i] > bestn) {
			bestn = counts[i];
			best = i;
		}
	*votes = bestn;
	return best;
}

/* The blocks a heap keeps outside its segments.
 *
 * A segment announces itself with a signature, so sweeping memory finds every
 * one. A block too large for a segment does not: the heap takes it straight
 * from the kernel and threads it onto a list in the heap header, leaving
 * nothing inside the block to say who owns it. This game's process heap had two
 * such blocks, and they were being rewound while the heap that indexes them
 * stayed in the present. That is a precise description of the fail-fast we kept
 * collecting, which arrived from the free path of an unrelated block on the
 * same heap and named heap corruption.
 *
 * Rather than hard-code where that list lives, which moves between Windows
 * builds, every candidate list head in the header is tried: two adjacent
 * pointers are a live list head when the entry they name points back at them.
 * Walking each one and holding whatever it reaches costs nothing when the list
 * turns out to be segments or free entries we already hold, and it catches the
 * one case that has no other tell. */
static int ss_readable(uintptr_t p, size_t n)
{
	MEMORY_BASIC_INFORMATION mbi;

	if (p < 0x10000 || VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) != sizeof(mbi))
		return 0;
	if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
		return 0;
	return p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

static int list_head_at(uintptr_t head, uintptr_t *first)
{
	uintptr_t flink;

	if (!ss_readable(head, 2 * sizeof(uintptr_t)))
		return 0;
	flink = *(const uintptr_t *)head;
	if (!flink || flink == head)
		return 0;
	if (!ss_readable(flink, 2 * sizeof(uintptr_t)))
		return 0;
	return *(const uintptr_t *)(flink + sizeof(uintptr_t)) == head ? (*first = flink, 1) : 0;
}

/* A pointer that names an allocation which names the heap back.
 *
 * The heap's front end - the low-fragmentation allocator - is neither a segment
 * nor a large block. It sits in an allocation of its own that only the heap
 * header points at, and it holds the busy/free state for every small block on
 * the heap. Rewinding it while the segments stay in the present is what the
 * fail-fast was reporting: the front end had been wound back to a moment when a
 * block was not yet handed out, so freeing it looked like freeing something
 * already free, which is the error code the debugger reads out of the dump.
 *
 * Finding it by structure offset would mean tracking a layout that changes
 * between Windows builds. The link is two-way instead, and that is checkable:
 * the header points at the allocation, and the allocation carries the heap's
 * own handle near its start. Requiring both directions is what keeps an
 * unrelated field that happens to hold a plausible address from dragging the
 * game's memory out of the rewind. */
static int points_back(uintptr_t at, uintptr_t want)
{
	unsigned i;

	if (!ss_readable(at, 0x100))
		return 0;
	for (i = 0; i < 0x100 / sizeof(uintptr_t); i++)
		if (((const uintptr_t *)at)[i] == want)
			return 1;
	return 0;
}

static uintptr_t heap_infra(uintptr_t base, HANDLE h, int *nheld, int depth)
{
	uintptr_t off, held = 0;
	uintptr_t seen_lo = 0, seen_hi = 0;

	for (off = 0; off + 2 * sizeof(uintptr_t) <= 0x400; off += sizeof(uintptr_t)) {
		uintptr_t head = base + off, p, first;
		int steps;

		if (!list_head_at(head, &first))
			continue;
		for (p = first, steps = 0; p && p != head && steps < 4096; steps++) {
			MEMORY_BASIC_INFORMATION mbi;
			uintptr_t next, ab, span;

			if (!ss_readable(p, 2 * sizeof(uintptr_t)))
				break;
			next = *(const uintptr_t *)p;
			/* Free and segment lists walk the same few allocations
			 * thousands of times; remembering the last one keeps this
			 * from becoming a hundred thousand kernel calls. */
			if (p >= seen_lo && p < seen_hi) {
				p = next;
				continue;
			}
			if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == sizeof(mbi) &&
			    mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) {
				ab = (uintptr_t)mbi.AllocationBase;
				span = alloc_span(ab);
				seen_lo = ab;
				seen_hi = ab + span;
				if (span && !region_excluded(ab, span)) {
					ss_exclude((void *)ab, (size_t)span);
					held += span;
					(*nheld)++;
				}
			}
			p = next;
		}
	}

	for (off = 0; off + sizeof(uintptr_t) <= 0x400; off += sizeof(uintptr_t)) {
		MEMORY_BASIC_INFORMATION mbi;
		uintptr_t v, span;

		if (!ss_readable(base + off, sizeof(uintptr_t)))
			continue;
		v = *(const uintptr_t *)(base + off);
		if (!v || v == base || v == (uintptr_t)h)
			continue;
		if (VirtualQuery((LPCVOID)v, &mbi, sizeof(mbi)) != sizeof(mbi))
			continue;
		if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE ||
		    (uintptr_t)mbi.AllocationBase != v)
			continue;
		if (!points_back(v, (uintptr_t)h))
			continue;
		span = alloc_span(v);
		if (!span || region_excluded(v, span))
			continue;
		ss_exclude((void *)v, (size_t)span);
		held += span;
		(*nheld)++;
		/* The front end keeps its own lists of zones allocated
		 * separately again, so what it points at travels with it. */
		if (depth > 0)
			held += heap_infra(v, h, nheld, depth - 1);
	}
	return held;
}

static uintptr_t heap_outliers(HANDLE h, int *nheld)
{
	return heap_infra((uintptr_t)h, h, nheld, 1);
}

/* A heap can only be rewound coherently if every thread that allocates from it
 * is rewound too, and the game's runtime heap fails that test: the OS worker
 * threads we deliberately leave in the present hold blocks from it, so rolling
 * its free lists back underneath them makes the owner's next free a double free.
 * That is the STATUS_HEAP_CORRUPTION fail-fast the thread audit predicts, and no
 * per-block exclusion avoids it, because preserving a block's contents does not
 * restore the metadata that marks it busy.
 *
 * Rewinding it stays the default, since leaving it behind is what left a tenth
 * of the process unowned. D3D9SW_REWIND_GAMEHEAP=0 takes the other side of that
 * trade: a few percent of stale state for a restore that cannot corrupt the
 * allocator. */
static int game_heap_rewound(void)
{
	char v[8];
	DWORD n = ss_getenv("D3D9SW_REWIND_GAMEHEAP", v, sizeof(v));
	return (n > 0 && n < sizeof(v) && v[0] == '0') ? 0 : 1;
}

/* Does the wrapper's own heap travel with the save?
 *
 * It always has, on the reasoning that our textures and targets describe the
 * same moment the game does, so both sides should move together. The census
 * priced that reasoning and it is bad. Our heap is 2585 MB of a 3137 MB
 * snapshot: five sixths of every save is our own pixels, and the game's actual
 * C-runtime heap is 321 MB of it.
 *
 * A second reason was claimed here and was wrong: that our own worker threads
 * keep allocating from this heap while its free lists are rewound underneath
 * them. They do not. request() calls swrast_pool_shutdown() before both the
 * save and the load, so the pool is stopped across the whole operation. The
 * size argument above is the only one that survives measurement.
 *
 * Leaving it in the present costs stale cache: textures uploaded after the save
 * stay uploaded and nothing references them. They remain valid memory, so the
 * pointers the game holds into our resources still resolve. That is a leak, not
 * a tear. Measured at 0: the save fell from 3137 MB to 482 MB and the restore
 * got proportionally faster, and the crash at UnityPlayer+54C1F7 was completely
 * unaffected - the same faulting instruction as two sessions with the opposite
 * heap policy. Whatever that crash is, it is not about which heap travels. */
static int sw_heap_rewound(void)
{
	char v[8];
	DWORD n = ss_getenv("D3D9SW_REWIND_SWHEAP", v, sizeof(v));
	return (n > 0 && n < sizeof(v) && v[0] == '0') ? 0 : 1;
}

/* Heaps whose contents Windows itself reaches from data we never rewind. The
 * list heads for ntdll's bookkeeping live in system module images, which are
 * excluded, while the nodes live in the heap; rewinding only the nodes leaves
 * the two disagreeing and the next walk hits FAST_FAIL_CORRUPT_LIST_ENTRY
 * (0xc0000409) inside ntdll.
 *
 * XInput's heaps were on this list and had to come off. Leaving them behind
 * stranded 32.4 MB, because heap_outliers follows a pointer out of their header
 * into a large region that is not theirs -- 16.2 MB apiece where mode 0 saw
 * 0.4 MB. Restoring on top of that gave a divide by zero inside ntdll's
 * allocator on two threads at once, a size field that came back zero. */
static const char *const kOsHeapOwners[] = { "ntdll.dll" };

static int os_owned_heap(const char *who)
{
	size_t i;
	for (i = 0; i < sizeof(kOsHeapOwners) / sizeof(kOsHeapOwners[0]); i++)
		if (lstrcmpiA(who, kOsHeapOwners[i]) == 0)
			return 1;
	return 0;
}

/* How much of the heap set travels with the save:
 *   0  partition by owner: only heaps belonging to modules we rewind
 *   1  every heap, so no allocator is half rolled back
 *   2  every heap except the ones Windows keeps its own lists in
 * Mode 0 leaves our own CRT heap in the present while our module data is
 * rewound, and mode 1 rolls ntdll's nodes back under un-rewound list heads.
 * Mode 2 is the seam between those two failures. */
static int heap_mode(void)
{
	char v[8];
	DWORD n = ss_getenv("D3D9SW_REWIND_ALLHEAPS", v, sizeof(v));
	if (n == 0 || n >= sizeof(v))
		return 0;
	return v[0] == '2' ? 2 : v[0] == '0' ? 0 : 1;
}

static void heaps_partition(void)
{
	HANDLE list[SS_MAX_HEAPS];
	MEMORY_BASIC_INFORMATION mbi;
	HANDLE game = game_crt_heap(), mine = sw_heap(), proc = GetProcessHeap();
	uintptr_t addr = 0, held = 0;
	DWORD n, i;
	int first = !g_ctl->heaps_listed, k, s, nheld = 0, nout = 0, base_heaps;
	int mode = heap_mode();

	g_ctl->nheaps = 0;
	g_ctl->nseg = 0;
	if (mode == 1)
		return;
	/* Neither runtime answering would leave nothing anchored, so fall back to
	 * the old behaviour rather than guess. */
	if (!game && !mine)
		return;
	n = GetProcessHeaps(SS_MAX_HEAPS, list);
	if (n > SS_MAX_HEAPS)
		n = SS_MAX_HEAPS;
	g_ctl->heaps_listed = 1;
	sort_modules();

	for (i = 0; i < n && g_ctl->nheaps < SS_MAX_HEAPS; i++) {
		k = g_ctl->nheaps++;
		g_ctl->heap_h[k] = list[i];
		g_ctl->heap_lo[k] = (uintptr_t)list[i];
		g_ctl->heap_hi[k] = (uintptr_t)list[i] + alloc_span((uintptr_t)list[i]);
		g_ctl->heap_ours[k] = 0;
	}
	base_heaps = g_ctl->nheaps;

	while (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
		uintptr_t base = (uintptr_t)mbi.BaseAddress;
		uintptr_t next = base + mbi.RegionSize;
		void *owner;

		if (next <= addr)
			break;
		addr = next;
		if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE ||
		    (uintptr_t)mbi.AllocationBase != base)
			continue;
		owner = segment_owner(base, mbi.Protect);
		if (!owner || g_ctl->nseg >= SS_MAX_SEGS)
			continue;
		s = g_ctl->nseg++;
		g_ctl->seg_base[s] = base;
		g_ctl->seg_size[s] = alloc_span(base);
		g_ctl->seg_owner[s] = (HANDLE)owner;
	}

	for (k = 0; k < base_heaps; k++) {
		HANDLE h = g_ctl->heap_h[k];
		const char *who;
		int votes = 0, m = -1;

		if (h == game) {
			g_ctl->heap_ours[k] = (char)game_heap_rewound();
			who = "the game's runtime";
		} else if (h == mine) {
			g_ctl->heap_ours[k] = (char)sw_heap_rewound();
			who = "this wrapper's own";
		} else if (h == proc) {
			/* Windows keeps its lists here whatever the vote says. */
			g_ctl->heap_ours[k] = 0;
			who = "the process heap";
		} else {
			m = heap_claimed_by(h, &votes);
			who = m >= 0 ? g_ctl->mod_name[m] : "no clear owner";
			/* Mode 2 keeps an unclaimed heap rather than stranding it,
			 * since the only heaps that must stay behind are the ones
			 * Windows reaches from data outside the save. */
			g_ctl->heap_ours[k] = mode == 2
						      ? (char)!os_owned_heap(who)
						      : (char)(m >= 0 ? g_ctl->mod_rewound[m] : 0);
		}
		lstrcpynA(g_ctl->heap_name[k], who, sizeof(g_ctl->heap_name[k]));
		if (first)
			ss_log("    heap %p %-24s %s (%d vote(s))\n", (void *)h, who,
			       g_ctl->heap_ours[k] ? "rewound" : "left in the present", votes);
	}

	for (s = 0; s < g_ctl->nseg; s++) {
		int owner = -1;
		for (k = 0; k < base_heaps; k++)
			if (g_ctl->heap_h[k] == g_ctl->seg_owner[s]) {
				owner = k;
				break;
			}
		if (owner < 0)
			continue;
		if (!g_ctl->heap_ours[owner]) {
			ss_exclude((void *)g_ctl->seg_base[s], (size_t)g_ctl->seg_size[s]);
			held += g_ctl->seg_size[s];
			nheld++;
		}
		/* Every segment is recorded, rewound or not, so that an address can
		 * be named and its side of the line known. Only the first segment of
		 * a heap is its handle, so without this the rest of a grown heap
		 * answers to nothing. */
		if (g_ctl->nheaps < SS_MAX_HEAPS &&
		    g_ctl->seg_base[s] != (uintptr_t)g_ctl->seg_owner[s]) {
			k = g_ctl->nheaps++;
			g_ctl->heap_h[k] = g_ctl->seg_owner[s];
			g_ctl->heap_lo[k] = g_ctl->seg_base[s];
			g_ctl->heap_hi[k] = g_ctl->seg_base[s] + g_ctl->seg_size[s];
			g_ctl->heap_ours[k] = g_ctl->heap_ours[owner];
			memcpy(g_ctl->heap_name[k], g_ctl->heap_name[owner],
			       sizeof(g_ctl->heap_name[k]));
		}
	}
	for (k = 0; k < base_heaps; k++) {
		int before = nout;
		uintptr_t got;

		if (g_ctl->heap_ours[k])
			continue;
		got = heap_outliers(g_ctl->heap_h[k], &nout);
		held += got;
		if (first && nout > before)
			ss_log("    heap %p keeps %d block(s) outside its segments, %.1f MB\n",
			       g_ctl->heap_h[k], nout - before,
			       (double)got / (1024.0 * 1024.0));
	}
	ss_log("  heaps: %lu total, %d of %d segment(s) plus %d outlying block(s), "
	       "%.1f MB left in the present\n",
	       (unsigned long)n, nheld, g_ctl->nseg, nout, (double)held / (1024.0 * 1024.0));
}

/* Segments discovered above are recorded too, so a fault report can say which
 * side of the line an address fell on and whether the partition held. */
/* Names the heap rather than describing it.
 *
 * This used to answer "in the game's heap" or "in a library heap we left alone",
 * and that phrasing cost three diagnostic cycles. Once it said "in the game's
 * heap" for an address the partition audit proved could not be there, which sent
 * me after a leak that did not exist. Once it said "a library heap we left
 * alone" for what may well have been our own, which is a different bug with a
 * different fix. Ten heaps go through this function and it was collapsing them
 * into two adjectives. */
static const char *ss_heap_of(uintptr_t at)
{
	static char buf[96];
	int i;

	if (!g_ctl)
		return NULL;
	for (i = 0; i < g_ctl->nheaps; i++)
		if (at >= g_ctl->heap_lo[i] && at < g_ctl->heap_hi[i]) {
			ss_fmt(buf, (int)sizeof(buf), ", in the %s heap at %p, which we %s",
				  g_ctl->heap_name[i], (void *)g_ctl->heap_lo[i],
				  g_ctl->heap_ours[i] ? "rewind" : "leave in the present");
			buf[sizeof(buf) - 1] = 0;
			return buf;
		}
	return NULL;
}

static int heap_index_of(uintptr_t at)
{
	int i;
	if (!at)
		return -1;
	for (i = 0; i < g_ctl->nheaps; i++)
		if (at >= g_ctl->heap_lo[i] && at < g_ctl->heap_hi[i])
			return i;
	return -1;
}

/* What is actually inside the heap, block by block.
 *
 * Everything else in this file treats the game's heap as anonymous pages: a few
 * gigabytes of bytes to copy and put back. But a Windows heap describes itself.
 * Every block carries a header giving its size and whether it is in use, and we
 * have been walking straight past all of it while inferring ownership from
 * vtable votes and pointers out of segment headers - an inference that has now
 * been wrong in both directions, over-claiming in the fault reports and
 * under-claiming in the partition.
 *
 * HeapWalk reads those headers exactly. The reason this file has avoided it is
 * written down two hundred lines up: it takes the heap's lock, and the snapshot
 * runs with every thread suspended, one of which may be holding it. That
 * objection is entirely about save time. A census is not a save. Run with the
 * process healthy and its threads live, taking the lock is ordinary, and the
 * one measurement we could never make becomes free.
 *
 * A census is worth taking twice and comparing, which is what the fingerprints
 * are for. Addresses cannot be compared across runs - the bases are randomised
 * - but the randomisation is a base plus a fixed offset, and we hold the bases,
 * so it cancels rather than needing to be inferred around. Below the bases
 * there is real entropy: the low-fragmentation heap deliberately shuffles which
 * slot in a size class a block lands in. That shuffle moves blocks; it does not
 * invent or resize them. So two fingerprints get taken, and the pair is the
 * measurement:
 *
 *   shape - the exact multiset of (size, in use), combined commutatively so
 *           order cannot affect it. Immune to the LFH shuffle.
 *   seq   - the same values in walk order. Sensitive to placement.
 *
 * shape equal with seq differing says the same allocations happened and only
 * their placement moved. shape differing says the content itself is different,
 * and the histogram says in which size classes. */
#define SS_CEN_BUCKETS 24

typedef struct HeapCensus {
	HANDLE h;
	unsigned long long busy_bytes, free_bytes, overhead_bytes;
	unsigned long long shape, seq;
	unsigned busy_n, free_n, region_n, uncommitted_n;
	unsigned busy_hist[SS_CEN_BUCKETS];
	unsigned free_hist[SS_CEN_BUCKETS];
	unsigned long long largest_free;
	int walked;
} HeapCensus;

static HeapCensus g_cen_base[SS_MAX_HEAPS];
static int g_cen_nbase;

/* splitmix64's finaliser. Any strong mix works; this one is short and has no
 * table. It matters only that near-identical inputs land far apart, so that
 * adding block sizes together cannot cancel two different heaps into agreement. */
static unsigned long long cen_mix(unsigned long long x)
{
	x += 0x9E3779B97F4A7C15ull;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
	return x ^ (x >> 31);
}

/* Size classes, not powers of two.
 *
 * A log2 histogram would put every small object in one bucket and report that
 * two very different heaps have the same shape. The front end allocates in
 * 16-byte steps, so the first sixteen buckets follow it exactly and only the
 * tail, where counts are low anyway, goes logarithmic. */
static int cen_bucket(unsigned long long n)
{
	int b;

	if (n < 256)
		return (int)(n >> 4);
	for (b = 16; b < SS_CEN_BUCKETS - 1; b++)
		if (n < (512ull << (b - 16)))
			return b;
	return SS_CEN_BUCKETS - 1;
}

static void cen_bucket_name(int b, char *out, size_t cap)
{
	if (b < 16)
		ss_fmt(out, (int)cap, "%d-%d", b * 16, b * 16 + 15);
	else if (b < SS_CEN_BUCKETS - 1)
		ss_fmt(out, (int)cap, "%lluK", (256ull << (b - 16)) / 1024 ? (256ull << (b - 16)) / 1024 : 1);
	else
		ss_fmt(out, (int)cap, ">=%lluK", (256ull << (SS_CEN_BUCKETS - 17)) / 1024);
	out[cap - 1] = 0;
}

/* The walk itself. Fills c and touches nothing else, so the heap's lock is held
 * for the walk alone and the logging happens after it is released - writing to
 * the log while holding another component's lock is how a diagnostic becomes the
 * bug it was added to find. */
static int cen_walk(HANDLE h, HeapCensus *c)
{
	PROCESS_HEAP_ENTRY e;
	int locked;

	memset(c, 0, sizeof(*c));
	c->h = h;
	locked = HeapLock(h) ? 1 : 0;
	memset(&e, 0, sizeof(e));
	while (HeapWalk(h, &e)) {
		unsigned long long n = (unsigned long long)e.cbData;
		int busy = (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) ? 1 : 0;

		if (e.wFlags & PROCESS_HEAP_REGION) {
			c->region_n++;
			continue;
		}
		if (e.wFlags & PROCESS_HEAP_UNCOMMITTED_RANGE) {
			c->uncommitted_n++;
			continue;
		}
		c->overhead_bytes += e.cbOverhead;
		/* Commutative on purpose: the sum is the same however the walk
		 * ordered the blocks, which is exactly the property that makes
		 * it survive the front end's slot shuffle. Addition rather than
		 * xor so that two identical blocks do not cancel each other. */
		c->shape += cen_mix(n * 2 + (unsigned)busy);
		c->seq = cen_mix(c->seq ^ (n * 2 + (unsigned)busy));
		if (busy) {
			c->busy_n++;
			c->busy_bytes += n;
			c->busy_hist[cen_bucket(n)]++;
		} else {
			c->free_n++;
			c->free_bytes += n;
			c->free_hist[cen_bucket(n)]++;
			if (n > c->largest_free)
				c->largest_free = n;
		}
	}
	if (locked)
		HeapUnlock(h);
	c->walked = (c->busy_n || c->free_n) ? 1 : 0;
	return c->walked;
}

static const char *cen_name(HANDLE h)
{
	int i;

	if (!g_ctl)
		return "";
	for (i = 0; i < g_ctl->nheaps; i++)
		if (g_ctl->heap_h[i] == h)
			return g_ctl->heap_name[i];
	return "";
}

/* Reported against the first census of the session, so the interesting question
 * - what did a scene transition actually do to the heap - is answered by taking
 * one census before it and one after, with no snapshot involved at all. */
static void cen_report_diff(const HeapCensus *now)
{
	int i, b;

	for (i = 0; i < g_cen_nbase; i++) {
		const HeapCensus *was = &g_cen_base[i];
		long long dn, db;

		if (was->h != now->h)
			continue;
		dn = (long long)now->busy_n - (long long)was->busy_n;
		db = (long long)now->busy_bytes - (long long)was->busy_bytes;
		ss_log("      vs baseline: busy %u -> %u (%+lld blocks, %+.1f MB), "
		       "free %u -> %u\n",
		       was->busy_n, now->busy_n, dn, (double)db / (1024.0 * 1024.0),
		       was->free_n, now->free_n);
		if (was->shape == now->shape) {
			ss_log("      SHAPE IDENTICAL%s: the same allocations exist; "
			       "%s\n",
			       was->seq == now->seq ? " AND IN THE SAME ORDER" : "",
			       was->seq == now->seq
				       ? "the heap is bit-for-bit the same arrangement"
				       : "only their placement moved, which is the front "
					 "end's shuffle and nothing else");
		} else {
			/* An exact multiset hash answers "identical or not", and the
			 * first run showed that is the wrong question: a heap 99.3%
			 * unchanged hashes as differently as one with nothing in
			 * common, so the useful signal was only visible in the
			 * per-class lines underneath. The L1 distance between the two
			 * histograms, over the larger population, says how far apart
			 * they actually are. */
			unsigned long long moved = 0, total = 0;
			int q;

			for (q = 0; q < SS_CEN_BUCKETS; q++) {
				long long d = (long long)now->busy_hist[q] -
					      (long long)was->busy_hist[q];

				moved += (unsigned long long)(d < 0 ? -d : d);
				total += was->busy_hist[q] > now->busy_hist[q] ? was->busy_hist[q]
									      : now->busy_hist[q];
			}
			ss_log("      shape %.2f%% unchanged by size class (%llu of %llu blocks "
			       "moved class)\n",
			       total ? 100.0 - (double)moved * 100.0 / (double)total : 0.0, moved,
			       total);
		}
		/* Only the classes that actually moved, because a full histogram
		 * every census would bury the few buckets carrying the change. */
		for (b = 0; b < SS_CEN_BUCKETS; b++) {
			long long d = (long long)now->busy_hist[b] - (long long)was->busy_hist[b];
			char nm[24];

			if (d > -8 && d < 8)
				continue;
			cen_bucket_name(b, nm, sizeof(nm));
			ss_log("        class %-10s busy %u -> %u (%+lld)\n", nm,
			       was->busy_hist[b], now->busy_hist[b], d);
		}
		return;
	}
}

static int heapcheck_mode(void)
{
	char v[16];
	DWORD n = ss_getenv("D3D9SW_HEAPCHECK", v, sizeof(v));

	return (n && n < sizeof(v)) ? atoi(v) : 1;
}

/* Asks every heap whether its own bookkeeping is self-consistent.
 *
 * Aimed at one specific failure. The current death is a write into ucrtbase's
 * read-only image reached from sprintf, with RtlAllocateHeap in the same call
 * chain and Mono as the caller, which reads as the allocator handing out
 * something that was never a heap block - a free list containing garbage. This
 * asks the allocator directly instead of inferring it from where the victim
 * happened to land, and it does so through HeapValidate, so it needs no
 * knowledge of the heap format and cannot rot when Windows changes it.
 *
 * MUST NOT run with threads suspended. HeapValidate takes each heap's lock, and
 * a lock held by a suspended thread cannot be released until we resume, so
 * calling it inside the suspended window is a guaranteed hang. That is not
 * speculation - it is exactly how the first version of the unwind-table
 * reconciliation froze the process, by allocating while suspended. So this runs
 * after the resume and before control returns to the caller, which costs a
 * little precision (other threads are briefly runnable, though they block on the
 * same lock while the walk proceeds) and buys the difference between a check and
 * a deadlock.
 *
 * Called at save time as well, and that call is the more important of the two. A
 * heap already inconsistent before the snapshot would make every post-restore
 * failure look like the restore's fault, and this project has already spent
 * three cycles on theories built from exactly that kind of missing control. */
/* Is this address inside a region that a valid slot would put back? The
 * question the whole project turns on, asked of one address. */
static int ss_in_restored(uintptr_t v)
{
	int k, i;

	if (!g_ctl)
		return 0;
	for (k = 0; k < SAVESTATE_SLOTS; k++) {
		const Slot *s = &g_ctl->slots[k];

		if (!s->valid)
			continue;
		for (i = 0; i < s->nregs; i++)
			if (v >= s->regs[i].base && v - s->regs[i].base < s->regs[i].size)
				return 1;
	}
	return 0;
}

/* Where a failed heap stops making sense.
 *
 * HeapValidate answers yes or no, and "no" about a multi-gigabyte heap is not a
 * lead. The walk narrows it to a block: HeapWalk stops at the first entry it
 * cannot parse, so the last address it did return is the last coherent block and
 * the damage is at or just past it.
 *
 * Reported together with whether that address sits inside a region this restore
 * wrote, which is the one thing worth knowing. If the rewind boundary is
 * innocent the bad block will be OUTSIDE the restored set, and that would be the
 * first evidence pointing away from it.
 *
 * Under the heap's own lock, like the census walk, logging only after releasing
 * it. The heap is already known bad, so the walk may abort early or return
 * nothing at all; both are reported rather than hidden, because "it stopped
 * immediately" says the damage is near the front.
 *
 * OFF BY DEFAULT, and the reason is that its first outing very probably killed
 * the process. The run that introduced it printed the "FAILED validation" line
 * and then NOTHING - no walk output and, decisively, not even the summary line
 * that had always followed - before a fault arrived on another thread inside
 * RtlpAllocateHeap. Taking a corrupt heap's lock and traversing its chains
 * while other threads are allocating from it is not a safe thing to do, and
 * this project has now converted a diagnostic into the failure twice. So the
 * summary is emitted BEFORE anything touches the bad heap, the walk is bounded
 * so a circular free list cannot spin forever, and it happens only when asked
 * for by name. */
static int heapwalk_mode(void)
{
	static int v = -1;

	if (v < 0) {
		char buf[16];

		v = ss_getenv("D3D9SW_HEAPWALK", buf, sizeof(buf)) && buf[0] != '0' ? 1 : 0;
	}
	return v;
}

static void heap_locate_bad(HANDLE h)
{
	PROCESS_HEAP_ENTRY e;
	uintptr_t last = 0;
	unsigned long long blocks = 0, bytes = 0;
	DWORD err;
	int locked;

	if (!heapwalk_mode()) {
		ss_log("      (set D3D9SW_HEAPWALK=1 to walk this heap and name the first "
		       "block it cannot parse - off by default because taking a corrupt "
		       "heap's lock and traversing it has killed this process before)\n");
		return;
	}
	/* Breadcrumb before, so the next run can tell "the walk found nothing"
	 * from "the walk never came back" without guessing from an absence. */
	ss_log("      walking heap %p to find the first unparseable block\n", (void *)h);
	locked = HeapLock(h) ? 1 : 0;
	memset(&e, 0, sizeof(e));
	SetLastError(0);
	/* Bounded. A corrupt heap can present a chain that loops, and an
	 * unbounded walk of it never returns - which on this path means the
	 * heap's lock is never released either. */
	while (blocks < 4000000 && HeapWalk(h, &e)) {
		if (e.wFlags & (PROCESS_HEAP_REGION | PROCESS_HEAP_UNCOMMITTED_RANGE))
			continue;
		blocks++;
		bytes += (unsigned long long)e.cbData;
		last = (uintptr_t)e.lpData;
	}
	err = GetLastError();
	if (locked)
		HeapUnlock(h);
	if (!blocks) {
		ss_log("      the walk could not parse a single block (error %lu), so the "
		       "damage is at the very front of this heap\n",
		       (unsigned long)err);
		return;
	}
	ss_log("      the walk parsed %llu block(s) / %.2f MB and then %s\n", blocks,
	       (double)bytes / (1024.0 * 1024.0),
	       err == ERROR_NO_MORE_ITEMS
		       ? "reached the end normally - so whatever HeapValidate objects to is "
			 "NOT in the block chain the walk follows"
		       : "STOPPED EARLY, so the damage is at or just past the last block");
	ss_log("      last coherent block at %p, which is %s\n", (void *)last,
	       ss_in_restored(last) ? "INSIDE a region this restore wrote"
				    : "OUTSIDE every restored region - the first evidence "
				      "that would point away from the rewind boundary");
}

static void heap_check(const char *when)
{
	HANDLE heaps[SS_MAX_HEAPS], failed[SS_MAX_HEAPS];
	DWORD n, i;
	int bad = 0;
	LARGE_INTEGER freq, t0, t1;

	if (!heapcheck_mode() || !g_ctl)
		return;
	n = GetProcessHeaps(SS_MAX_HEAPS, heaps);
	if (!n)
		return;
	if (n > SS_MAX_HEAPS)
		n = SS_MAX_HEAPS;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	for (i = 0; i < n; i++) {
		if (HeapValidate(heaps[i], 0, NULL))
			continue;
		if (bad < SS_MAX_HEAPS)
			failed[bad] = heaps[i];
		bad++;
		ss_log("  heap check: heap %p (%s) FAILED validation %s\n", (void *)heaps[i],
		       ss_heap_of((uintptr_t)heaps[i]), when);
	}
	QueryPerformanceCounter(&t1);
	ss_log("  heap check: %u heap(s) %s, %d failed, %.1f ms\n", (unsigned)n, when, bad,
	       (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart);
	/* Deliberately after the summary. The walk can be fatal, and a fatal
	 * diagnostic that also destroys the line saying how many heaps failed
	 * leaves the run less informative than having no diagnostic at all. */
	for (i = 0; i < (DWORD)bad && i < SS_MAX_HEAPS; i++)
		heap_locate_bad(failed[i]);
}

void savestate_census(void)
{
	HANDLE heaps[SS_MAX_HEAPS];
	DWORD n, i;
	int baseline;

	if (!g_ctl || !g_ctl->log || g_ctl->log == INVALID_HANDLE_VALUE)
		return;
	n = GetProcessHeaps(SS_MAX_HEAPS, heaps);
	if (!n)
		return;
	if (n > SS_MAX_HEAPS)
		n = SS_MAX_HEAPS;
	baseline = g_cen_nbase == 0;
	ss_log("census: %u heap(s), %s\n", (unsigned)n,
	       baseline ? "this one is the baseline" : "compared against the baseline");
	for (i = 0; i < n; i++) {
		HeapCensus c;

		if (!cen_walk(heaps[i], &c)) {
			ss_log("    heap %p %s refused the walk\n", (void *)heaps[i],
			       cen_name(heaps[i]));
			continue;
		}
		ss_log("    heap %p %-24s busy %u / %.1f MB, free %u / %.1f MB, "
		       "%u region(s), overhead %.1f MB, largest free %.1f KB\n",
		       (void *)heaps[i], cen_name(heaps[i]), c.busy_n,
		       (double)c.busy_bytes / (1024.0 * 1024.0), c.free_n,
		       (double)c.free_bytes / (1024.0 * 1024.0), c.region_n,
		       (double)c.overhead_bytes / (1024.0 * 1024.0),
		       (double)c.largest_free / 1024.0);
		ss_log("      shape=%016llX seq=%016llX\n", c.shape, c.seq);
		if (baseline) {
			if (g_cen_nbase < SS_MAX_HEAPS)
				g_cen_base[g_cen_nbase++] = c;
		} else {
			cen_report_diff(&c);
		}
	}
}

/* Threads left in the present that own memory we take back.
 *
 * Every failure this project has traced has been one relationship: something
 * still running holds a pointer into memory the rewind moved. The process
 * heap's front end, a recycled thread stack, XInput's per-thread runtime data -
 * all the same sentence with a different subject. Each one cost a crash, a
 * dump, and an evening, because the only symptom is a fail-fast somewhere
 * unrelated, minutes later, once the holder finally touches what it kept.
 *
 * The relationship is visible before it does any harm, though. A thread we
 * classify as the operating system's keeps its per-thread runtime state in its
 * thread block and in the storage vector that hangs off it, and we know which
 * heaps we intend to rewind. If any of those pointers lands in one, that is the
 * next crash, named in advance and while the process is still healthy.
 *
 * The search deliberately does not know any structure layouts. Per-thread
 * runtime state is reached through a chain - the thread block points at a
 * storage vector or a fiber-local table, and the blocks that matter hang off
 * that - and the shape of those tables differs between Windows versions, so
 * walking them by offset would work on the machine it was written on and go
 * quietly blind elsewhere. Following any pointer the thread block holds into a
 * small private allocation, and searching that too, finds the same blocks
 * without naming a single field. XInput's per-thread data, which cost us a
 * night, sits exactly one hop out.
 *
 * This only reports. A pointer here is not proof of a fault - a system thread
 * may legitimately be holding something the game handed it - so where it was
 * found and which heap owns it go in the line, and the judgement stays with
 * whoever reads it. */
static int audit_scan(const uintptr_t *w, size_t words, DWORD tid, const char *where,
		      int *found)
{
	size_t k;

	for (k = 0; k < words && *found < 12; k++) {
		int h = heap_index_of(w[k]);
		if (h < 0 || !g_ctl->heap_ours[h])
			continue;
		ss_log("  warning: system thread %lu holds %p %s, on %s's heap, "
		       "which we rewind\n",
		       (unsigned long)tid, (void *)w[k], where, g_ctl->heap_name[h]);
		(*found)++;
	}
	return *found;
}

static void audit_present_threads(void)
{
	int i, found = 0;

	for (i = 0; i < g_ctl->nids && found < 12; i++) {
		uintptr_t hops[16];
		unsigned char *teb;
		const uintptr_t *w;
		unsigned k;
		int nhops = 0, j;

		if (!g_ctl->transient[i] || !g_ctl->handles[i])
			continue;
		teb = (unsigned char *)teb_of(g_ctl->handles[i]);
		if (!teb)
			continue;

		w = (const uintptr_t *)teb;
		audit_scan(w, 0x1000 / sizeof(uintptr_t), g_ctl->ids[i], "in its thread block",
			   &found);

		/* Collected first, so the scans below are not walking a list that
		 * the scanning itself keeps extending. Anything large is skipped:
		 * per-thread runtime tables are small, and a sweep of the game's
		 * own buffers would report every pointer in them. */
		for (k = 0; k < 0x1000 / sizeof(uintptr_t) && nhops < 16; k++) {
			MEMORY_BASIC_INFORMATION mbi;
			uintptr_t ab, span;

			if (w[k] < 0x10000 ||
			    VirtualQuery((LPCVOID)w[k], &mbi, sizeof(mbi)) != sizeof(mbi))
				continue;
			if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE ||
			    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
				continue;
			ab = (uintptr_t)mbi.AllocationBase;
			span = alloc_span(ab);
			if (!span || span > 0x10000 || heap_index_of(ab) >= 0)
				continue;
			for (j = 0; j < nhops; j++)
				if (hops[j] == ab)
					break;
			if (j == nhops)
				hops[nhops++] = ab;
		}
		for (j = 0; j < nhops && found < 12; j++)
			audit_scan((const uintptr_t *)hops[j],
				   (size_t)alloc_span(hops[j]) / sizeof(uintptr_t),
				   g_ctl->ids[i], "in its per-thread runtime state", &found);
	}
	if (found >= 12)
		ss_log("  warning: more of these than are worth listing; the rest are "
		       "not shown\n");
}

/* What a reachability pass would have to hold.
 *
 * The game's runtime heap turned out to be the process heap. Windows keeps its
 * own bookkeeping there - dynamic function tables for jitted code, critical
 * section records, RPC bindings, thread pool timers - so the game's objects and
 * the operating system's share one allocator, and no whole-heap policy can
 * separate them. Rewinding that heap gave FAST_FAIL_CORRUPT_LIST_ENTRY inside
 * ntdll and an access violation on a thread pool timer during RPC teardown;
 * leaving it behind stranded the game's own objects and faulted Unity on a
 * zeroed vtable. Both sides of that choice are wrong.
 *
 * The line has to fall per block, and the honest way to place it is to ask what
 * the present can still reach. The images we leave behind are the roots: a
 * pointer in one of them landing in a heap we rewind names a block that has to
 * stay, and so does anything that block points at, transitively - hold a list's
 * head and rewind its second node and the links disagree just the same.
 *
 * This only measures. Acting on it needs holes inside a saved region, which the
 * exclusion list cannot express: region_excluded is all or nothing, so a single
 * held unit would drop the segment around it, and the list caps at SS_MAX_EXCL.
 * How large the closure is decides whether that is worth building, so it gets
 * counted before anything is held. */

/* The unit the closure is measured and held in.
 *
 * A page was the wrong unit and it is what made the first measurement useless:
 * holding one carries the five hundred words that merely surround the pointer
 * that justified holding it, each of which is then followed as if it mattered,
 * so the closure ran past 64 MB and was still growing. A heap block is the unit
 * the exclusion actually wants, but its bounds cannot be had from here, because
 * HeapWalk takes the heap's lock and this runs with every thread suspended, one
 * of which may be holding it.
 *
 * A granule sized like a list node approximates a block without asking the heap
 * anything. At 64 bytes a held unit carries eight words rather than five
 * hundred, which is where the spurious edges were coming from. It also covers
 * the part of a node that matters: a LIST_ENTRY keeps its forward and backward
 * links at offset zero, so the granule holding a node's start holds the links
 * whose disagreement is what ntdll fails on, even where the node is longer than
 * the granule. */
#define REACH_GRAN_DEFAULT 64
#define REACH_QUEUE_CAP (1u << 19)
#define REACH_HASH_CAP (1u << 20)

/* Off by default: it costs a noticeable slice of frozen process per save. */
static int reach_audit(void)
{
	char v[8];
	DWORD n = ss_getenv("D3D9SW_REACH_AUDIT", v, sizeof(v));
	return (n > 0 && n < sizeof(v) && v[0] != '0') ? 1 : 0;
}

static uintptr_t reach_gran(void)
{
	char v[16];
	DWORD n = ss_getenv("D3D9SW_REACH_GRAN", v, sizeof(v));
	uintptr_t w = 0;
	DWORD i;

	if (n == 0 || n >= sizeof(v))
		return REACH_GRAN_DEFAULT;
	for (i = 0; i < n && v[i] >= '0' && v[i] <= '9'; i++)
		w = w * 10 + (uintptr_t)(v[i] - '0');
	/* A granule below a pointer could not hold one, and a non-power-of-two
	 * would break the masking the walk indexes with. */
	if (w < sizeof(uintptr_t) || (w & (w - 1)) != 0 || w > 0x1000)
		return REACH_GRAN_DEFAULT;
	return w;
}

typedef struct ReachWalk {
	uintptr_t *queue;
	uintptr_t *seen;
	unsigned nq, head;
	uintptr_t gran, mask;
	uintptr_t lo_all, hi_all;
	unsigned nedges;
	int capped;
} ReachWalk;

static int reach_seen(ReachWalk *w, uintptr_t unit)
{
	unsigned h = (unsigned)((unit / w->gran) * 2654435761u) & (REACH_HASH_CAP - 1);
	/* Zero marks a free slot, and no heap unit sits at address zero. */
	while (w->seen[h]) {
		if (w->seen[h] == unit)
			return 1;
		h = (h + 1) & (REACH_HASH_CAP - 1);
	}
	w->seen[h] = unit;
	return 0;
}

/* Reads every aligned word in [base, end) and enqueues the granule around any
 * that lands in a heap being rewound. The caller guarantees the range is
 * committed and readable, which is what makes this cheap: the first pass asked
 * VirtualQuery per word and spent fifteen seconds doing it. */
static void reach_scan(ReachWalk *w, uintptr_t base, uintptr_t end)
{
	base = (base + sizeof(uintptr_t) - 1) & ~(uintptr_t)(sizeof(uintptr_t) - 1);
	for (; base + sizeof(uintptr_t) <= end; base += sizeof(uintptr_t)) {
		uintptr_t v = *(const uintptr_t *)base;
		uintptr_t unit;
		int h;

		if (v < w->lo_all || v >= w->hi_all)
			continue;
		h = heap_index_of(v);
		if (h < 0 || !g_ctl->heap_ours[h])
			continue;
		w->nedges++;
		unit = v & ~w->mask;
		if (reach_seen(w, unit))
			continue;
		if (w->nq >= REACH_QUEUE_CAP) {
			w->capped = 1;
			return;
		}
		w->queue[w->nq++] = unit;
	}
}

static void audit_system_reachable(void)
{
	ReachWalk w;
	unsigned nroots;
	int i;
	DWORD t0 = GetTickCount();

	if (!reach_audit() || !g_ctl->nheaps)
		return;

	memset(&w, 0, sizeof(w));
	w.gran = reach_gran();
	w.mask = w.gran - 1;
	w.lo_all = (uintptr_t)-1;
	/* One bound over every rewound heap turns the inner test into a pair of
	 * compares. Without it this is a hundred megabytes of image data times
	 * the heap count, which is slow enough to matter on every save. */
	for (i = 0; i < g_ctl->nheaps; i++) {
		if (!g_ctl->heap_ours[i])
			continue;
		if (g_ctl->heap_lo[i] < w.lo_all)
			w.lo_all = g_ctl->heap_lo[i];
		if (g_ctl->heap_hi[i] > w.hi_all)
			w.hi_all = g_ctl->heap_hi[i];
	}
	if (w.lo_all >= w.hi_all)
		return;

	/* A granule small enough to be useful needs far more slots than a page
	 * did, and as static arrays they would join the saved set and be copied
	 * on every save. Taken from the OS for the length of the walk instead. */
	w.queue = (uintptr_t *)VirtualAlloc(NULL, REACH_QUEUE_CAP * sizeof(uintptr_t),
					    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	w.seen = (uintptr_t *)VirtualAlloc(NULL, REACH_HASH_CAP * sizeof(uintptr_t),
					   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!w.queue || !w.seen) {
		ss_log("  reachability audit: no memory for the walk\n");
		if (w.queue)
			VirtualFree(w.queue, 0, MEM_RELEASE);
		if (w.seen)
			VirtualFree(w.seen, 0, MEM_RELEASE);
		return;
	}

	/* Roots: the writable data of every image left in the present. Read-only
	 * and code pages hold link-time constants, never heap addresses. */
	for (i = 0; i < g_ctl->nmods && !w.capped; i++) {
		uintptr_t p = g_ctl->mod_lo[i];
		MEMORY_BASIC_INFORMATION mbi;

		if (g_ctl->mod_rewound[i])
			continue;
		while (p < g_ctl->mod_hi[i] &&
		       VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
			uintptr_t base = (uintptr_t)mbi.BaseAddress;
			uintptr_t end = base + mbi.RegionSize;
			DWORD prot = mbi.Protect & 0xff;

			p = end;
			if (mbi.State != MEM_COMMIT ||
			    !(prot & (PAGE_READWRITE | PAGE_WRITECOPY |
				      PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
				continue;
			if (end > g_ctl->mod_hi[i])
				end = g_ctl->mod_hi[i];
			if (base < g_ctl->mod_lo[i])
				base = g_ctl->mod_lo[i];
			if (base < end)
				reach_scan(&w, base, end);
			if (w.capped)
				break;
		}
	}
	nroots = w.nedges;
	w.nedges = 0;

	/* Transitive closure over the held granules themselves: hold a list's
	 * head and rewind its second node and the links disagree just the same.
	 * A granule's address came from a pointer, so unlike the roots it has to
	 * be proved readable - but once per granule, not once per word. */
	while (w.head < w.nq && !w.capped) {
		uintptr_t unit = w.queue[w.head++];

		if (!ss_readable(unit, w.gran))
			continue;
		reach_scan(&w, unit, unit + w.gran);
	}

	ss_log("  reachable from the present: %u root pointer(s), %u granule(s) of %u B, "
	       "%.2f MB, %u interior edge(s), %lu ms%s\n",
	       nroots, w.nq, (unsigned)w.gran,
	       (double)w.nq * (double)w.gran / (1024.0 * 1024.0), w.nedges,
	       (unsigned long)(GetTickCount() - t0),
	       w.capped ? " (capped, closure is larger)" : "");

	VirtualFree(w.queue, 0, MEM_RELEASE);
	VirtualFree(w.seen, 0, MEM_RELEASE);
}

static void build_exclusions(void)
{
	MODULEENTRY32 me;
	HANDLE snap;
	HMODULE self = NULL, exe = GetModuleHandleA(NULL);
	char windir[MAX_PATH];
	UINT wlen;
	int i, nstack = 0, nteb = 0, nmod = 0, ntrans = 0;
	char onames[8][64];
	int ocounts[8], nown = 0;

	memset(g_ctl->transient, 0, sizeof(g_ctl->transient));
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCSTR)&build_exclusions, &self);

	g_ctl->nex = g_ctl->nex_fixed;
	g_ctl->nstk = 0;
	g_ctl->nmods = 0;

	/* Modules first: whether a thread belongs to the game or to the system is
	 * decided by whether its entry point lands in an excluded image, and the
	 * thread pass below needs that answer. */
	wlen = GetWindowsDirectoryA(windir, sizeof(windir));
	snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
	if (snap != INVALID_HANDLE_VALUE) {
		find_steam_dir(snap);
		find_game_dir(snap, exe);
		me.dwSize = sizeof(me);
		if (Module32First(snap, &me)) {
			/* Named once per session, and only for modules outside the
			 * Windows directory. Those are the game's own libraries plus
			 * whatever else has let itself in - overlays, capture hooks,
			 * input remappers, vendor utilities. A machine that cannot
			 * survive a restore while an identical one can is most easily
			 * explained by an extra participant, and until now the log
			 * only ever counted them. */
			int first = !g_ctl->listed;
			g_ctl->listed = 1;
			do {
				int is_self = (HMODULE)me.modBaseAddr == self;
				int mine = (HMODULE)me.modBaseAddr == exe || is_self;
				int ex = module_excluded(me.szExePath, windir, wlen);

				/* Our data section travels with our heap or not at all.
				 * It holds that heap's handle and pointers into it, so
				 * leaving the heap in the present while rewinding the
				 * bookkeeping that describes it is precisely the split
				 * the comment on module_excluded warns about - the one
				 * that made excluding MSVCR100 unsafe. Whichever way the
				 * knob goes, both halves go together. */
				if (is_self && !sw_heap_rewound()) {
					mine = 0;
					ex = 1;
				}
				if (first && !(wlen && _strnicmp(me.szExePath, windir,
								 wlen) == 0)) {
					const char *nm = strrchr(me.szExePath, '\\');
					/* The verdict below, not the rule that fed it.
					 * Printing ex alone reported the executable
					 * and this wrapper as held when the line
					 * beneath rewinds them both, and reading the
					 * log back cost an hour chasing a module that
					 * was never excluded in the first place. */
					ss_log("    module %-26s %8lu KB at %p, %s\n",
					       nm ? nm + 1 : me.szExePath,
					       (unsigned long)(me.modBaseSize / 1024),
					       (void *)me.modBaseAddr,
					       (mine || !ex) ? "rewound"
							     : "left in the present");
				}
				/* Kept so a heap can be attributed to whichever module
				 * its contents point back at. */
				if (g_ctl->nmods < SS_MAX_MODS) {
					const char *nm = strrchr(me.szExePath, '\\');
					int m = g_ctl->nmods++;
					g_ctl->mod_lo[m] = (uintptr_t)me.modBaseAddr;
					g_ctl->mod_hi[m] = (uintptr_t)me.modBaseAddr + me.modBaseSize;
					g_ctl->mod_rewound[m] = (char)(mine || !ex);
					lstrcpynA(g_ctl->mod_name[m], nm ? nm + 1 : me.szExePath,
						  sizeof(g_ctl->mod_name[m]));
				}
				if (mine || !ex)
					continue;
				ss_exclude(me.modBaseAddr, me.modBaseSize);
				nmod++;
			} while (Module32Next(snap, &me));
		}
		CloseHandle(snap);
	}

	heaps_partition();

	for (i = 0; i < g_ctl->nids; i++) {
		unsigned char *teb;
		if (!g_ctl->handles[i])
			continue;
		teb = (unsigned char *)teb_of(g_ctl->handles[i]);
		if (!teb)
			continue;
		ss_exclude(teb, 0x1000);
		if (nteb == 0) {
#if defined(_M_IX86) || defined(__i386__)
			void *peb = *(void **)(teb + 0x30);
#else
			void *peb = *(void **)(teb + 0x60);
#endif
			if (peb)
				ss_exclude(peb, 0x1000); /* loader and heap list */
		}
		nteb++;
		{
			MEMORY_BASIC_INFORMATION sm;
			uintptr_t hi = *(uintptr_t *)(teb + sizeof(void *) * 1);
			uintptr_t lo = *(uintptr_t *)(teb + sizeof(void *) * 2);
			uintptr_t foot;
			if (!lo || hi <= lo)
				continue;
			/* The thread block reports the lowest page the stack has
			 * grown onto so far, not how far it may grow. Below that sit
			 * the guard page and the rest of the reservation, and both
			 * belong to the kernel's accounting rather than to the
			 * program's state.
			 *
			 * Excluding only the committed part left that tail open, and
			 * a snapshot taken while the address was something else -
			 * stacks are recycled constantly, a thread that exits hands
			 * its range straight to the next one - restored old bytes
			 * over a live thread's guard page. Nothing complains at the
			 * time. The thread grows into what should have faulted the
			 * kernel into extending it, and the next return from a deep
			 * call comes back to a frame pointer of 0xFFFFFFFF. A COM
			 * worker sitting in a thirty second wait died exactly that
			 * way, half a minute after the restore that damaged it.
			 *
			 * The reservation base is the honest boundary, and asking
			 * the memory manager for it avoids depending on where the
			 * thread block keeps that field on a given Windows. */
			foot = lo;
			if (VirtualQuery((LPCVOID)lo, &sm, sizeof(sm)) == sizeof(sm) &&
			    sm.AllocationBase)
				foot = (uintptr_t)sm.AllocationBase;
			/* Recorded for every thread whatever the rewind mode, so
			 * reclamation can never hand back a stack that something is
			 * still standing on. */
			if (g_ctl->nstk < SS_MAX_THREADS) {
				g_ctl->stk_lo[g_ctl->nstk] = foot;
				g_ctl->stk_hi[g_ctl->nstk] = hi;
				g_ctl->nstk++;
			}
			/* A thread whose entry point sits in a system image belongs
			 * to the operating system, not the game: the thread pool
			 * retires and spawns these on its own schedule, which is
			 * every divergence the log has ever reported. Rewinding one
			 * means nothing, and treating its id as a change refuses
			 * restores over churn the game never saw. So it is left
			 * alone entirely - stack excluded, registers untouched, not
			 * counted as divergence. */
			if (region_excluded((uintptr_t)g_ctl->starts[i], 1)) {
				g_ctl->transient[i] = 1;
				ss_exclude((void *)foot, (size_t)(hi - foot));
				tally_owner(onames, ocounts, &nown, g_ctl->starts[i]);
				/* These keep running across the rewind with their
				 * registers untouched. That is survivable while one is
				 * parked in a wait, and not survivable while one is
				 * inside the allocator, because the heap it is halfway
				 * through is about to be replaced underneath it. */
				if (g_ctl->handles[i]) {
					CONTEXT c;
					memset(&c, 0, sizeof(c));
					c.ContextFlags = CONTEXT_CONTROL;
					if (GetThreadContext(g_ctl->handles[i], &c)) {
						MEMORY_BASIC_INFORMATION mbi;
						char nm[MAX_PATH] = "?";
#if defined(_M_IX86) || defined(__i386__)
						uintptr_t pc = (uintptr_t)c.Eip;
#else
						uintptr_t pc = (uintptr_t)c.Rip;
#endif
						if (park_is_new(pc)) {
							if (VirtualQuery((LPCVOID)pc, &mbi,
									 sizeof(mbi)) ==
							    sizeof(mbi))
								GetModuleFileNameA(
									(HMODULE)mbi.AllocationBase,
									nm, sizeof(nm));
							ss_log("    new park site: thread %lu at %p in %s\n",
							       (unsigned long)g_ctl->ids[i],
							       (void *)pc, nm);
						}
					}
				}
				ntrans++;
				continue;
			}
			/* A game thread's live frames do travel back with the rest
			 * of the program, but the unreached tail of its stack is
			 * held for the same reason as anyone else's. */
			if (foot < lo)
				ss_exclude((void *)foot, (size_t)(lo - foot));
			if (g_ctl->ids[i] == g_ctl->req_tid || rewind_all_threads())
				continue;
			ss_exclude((void *)lo, (size_t)(hi - lo));
			nstack++;
		}
	}

	ss_log("exclude: %d tebs, %d game stacks, %d system threads, %d library images, "
	       "%ld total\n",
	       nteb, nstack, ntrans, nmod, (long)g_ctl->nex);
	if (nown) {
		char line[512];
		int off = 0, k;
		/* ss_fmt, not snprintf: build_exclusions runs INSIDE the suspended
		 * window - the save's own phase timing proves it, reporting suspend
		 * and exclusions as consecutive costs - and the C runtime's
		 * formatting state lives on the heap this engine rewinds. Same class
		 * as the ss_log fix, and it survived that sweep only because a
		 * search for sprintf and _snprintf does not match a plain snprintf.
		 *
		 * Also fixes a truncation bug on the way past: snprintf returns the
		 * length it WOULD have written, which can exceed the buffer, so
		 * `off +=` could walk the cursor beyond `line` and hand the next
		 * call a negative size cast to size_t. ss_fmt returns what it
		 * actually wrote. */
		for (k = 0; k < nown && off < (int)sizeof(line) - 80; k++)
			off += ss_fmt(line + off, (int)sizeof(line) - off, "%s%s x%d",
				      k ? ", " : "", onames[k], ocounts[k]);
		ss_log("  those threads belong to: %s\n", line);
	}
	audit_present_threads();
	audit_system_reachable();
}

/* Puts a region back in place if the game released it since the save.
 *
 * MEM_COMMIT on its own only works on memory that is still reserved. Once a
 * range goes fully free it has to be reserved again first, and reservation
 * snaps to the 64 KB allocation granularity rather than the page-aligned base
 * a heap region reports - so the reserve covers the containing block and the
 * commit covers the exact bytes. Getting this wrong is silent: the region is
 * simply never written back, and the process resumes with most of its state
 * rewound and a few pieces still in the future. */
/* What to do with memory the process acquired after the snapshot.
 *
 * 0 leaves it, which is what every run so far has done. 1 decommits the part of
 * it that no heap claims, which is the honest thing: at snapshot time that
 * address space held nothing, so restoring it means giving it back.
 *
 * Decommit rather than release, because the point is to be told. Releasing lets
 * the address be handed out again and a stray use lands silently in someone
 * else's data; decommitting leaves the reservation, so the same stray use faults
 * at an address we can print and a module we can name. A fail-fast we cannot see
 * becomes an access violation we can. */
static int drift_mode(void)
{
	/* Read the same way as every other knob in this file. getenv answers from
	 * the runtime's own copy of the environment, which the config file does not
	 * reach, so a setting from there was invisible here. */
	char v[8];
	DWORD n = ss_getenv("D3D9SW_DRIFT", v, sizeof(v));

	if (n == 0 || n >= sizeof(v))
		return 0;
	return v[0] == '0' ? 0 : atoi(v);
}

static int ensure_committed(uintptr_t base, uintptr_t size, uintptr_t alloc_base,
			    uintptr_t alloc_end)
{
	MEMORY_BASIC_INFORMATION mbi;
	SYSTEM_INFO si;
	uintptr_t gran, page, p = base, end = base + size;

	GetSystemInfo(&si);
	gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 65536;
	page = si.dwPageSize ? si.dwPageSize : 4096;

	/* VirtualQuery answers for the run of pages that share attributes, not
	 * for the range it was asked about, so a region whose first page is
	 * committed can have a tail the game has since trimmed. Believing that
	 * first answer reported success, let the restore start copying, and
	 * faulted 14 MB into a 1440p surface on a write into the reserved part of
	 * the allocation - a crash in the copy rather than the silent gap this
	 * function was written to prevent. Each run is settled on its own terms. */
	while (p < end) {
		uintptr_t run;

		if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) != sizeof(mbi))
			return 0;
		run = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		if (run > end)
			run = end;
		if (run <= p)
			return 0;
		if (mbi.State == MEM_COMMIT) {
			p = run;
			continue;
		}
		/* Committing needs a reservation underneath it, and reservation
		 * snaps to the allocation granularity rather than the page-aligned
		 * base a region reports, so the reserve covers the containing
		 * block while the commit covers only the bytes wanted. */
		if (mbi.State == MEM_FREE) {
			/* Rounding the region's own base down to the granularity
			 * is wrong whenever the region is a slice of a larger
			 * allocation rather than the whole of one. A 1440p surface
			 * sat at ...524000, four pages above its allocation's base,
			 * so rounding down reached into whatever now owns the pages
			 * before it; the reserve failed and the commit failed
			 * behind it with ERROR_INVALID_ADDRESS, and the load was
			 * refused. The allocation the region was carved from is the
			 * reservation to recreate, and its recorded base is
			 * granular by construction. */
			uintptr_t rbase = (alloc_base ? alloc_base : p) & ~(gran - 1);
			uintptr_t rend = alloc_end > end ? alloc_end : end;

			rend = (rend + gran - 1) & ~(gran - 1);
			if (!VirtualAlloc((LPVOID)rbase, (SIZE_T)(rend - rbase), MEM_RESERVE,
					  PAGE_NOACCESS)) {
				/* Some of the allocation may already be back, and
				 * then only the free run itself is ours to take.
				 *
				 * Rounding this fallback's end up to the
				 * granularity as well made it ask for the very
				 * range that had just been refused, so it could
				 * only ever fail the same way - four small
				 * allocations round 0000020FC888xxxx refused a load
				 * with err=487 for exactly that reason. Only the
				 * base of a reservation has to be granular; the
				 * size is taken to the next page. Asking for the
				 * run alone can succeed where the containing block
				 * cannot, because the rest of that block is
				 * precisely what someone else now owns. */
				uintptr_t fbase = p & ~(gran - 1);
				uintptr_t fend = (run + page - 1) & ~(page - 1);

				if (!VirtualAlloc((LPVOID)fbase, (SIZE_T)(fend - fbase),
						  MEM_RESERVE, PAGE_NOACCESS))
					ss_log("  reserve %p+%lx and %p+%lx both refused, "
					       "err=%lu, state %lx\n",
					       (void *)rbase, (unsigned long)(rend - rbase),
					       (void *)fbase, (unsigned long)(fend - fbase),
					       GetLastError(), (unsigned long)mbi.State);
			}
		}
		if (!VirtualAlloc((LPVOID)p, (SIZE_T)(run - p), MEM_COMMIT, PAGE_READWRITE)) {
			ss_log("  commit %p+%lx refused, err=%lu, state was %lx\n", (void *)p,
			       (unsigned long)(run - p), GetLastError(),
			       (unsigned long)mbi.State);
			return 0;
		}
		p = run;
	}
	return 1;
}

/* A sliding window over a section that is far larger than anything a 32-bit
 * process could map in one piece. */
typedef struct Window {
	HANDLE sect;
	unsigned char *base;
	unsigned long long off, size;
	unsigned long long total;
} Window;

static void win_close(Window *w)
{
	if (w->base) {
		UnmapViewOfFile(w->base);
		w->base = NULL;
	}
	w->size = 0;
}

static int win_cover(Window *w, unsigned long long pos)
{
	unsigned long long start, want;
	DWORD gran = 65536;
	SYSTEM_INFO si;
	if (w->base && pos >= w->off && pos < w->off + w->size)
		return 1;
	win_close(w);
	GetSystemInfo(&si);
	if (si.dwAllocationGranularity)
		gran = si.dwAllocationGranularity;
	start = (pos / gran) * gran;
	want = w->total - start;
	if (want > SS_VIEW_BYTES)
		want = SS_VIEW_BYTES;
	w->base = (unsigned char *)MapViewOfFile(w->sect, FILE_MAP_ALL_ACCESS,
						 (DWORD)(start >> 32), (DWORD)start,
						 (SIZE_T)want);
	if (!w->base)
		return 0;
	w->off = start;
	w->size = want;
	return 1;
}

/* Answered once and cached, unlike every other knob in this file, because this
 * one is read inside the suspended window and the reading itself was showing up
 * in the results. GetEnvironmentVariableA converts through a scratch buffer on
 * the C runtime heap - which is the heap we rewind - so the first version of this
 * check reported a changed region at every save whose new contents were the
 * UTF-16 text "D3D9". The instrument was measuring its own footprint. */
static int verify_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[16];
		DWORD n = ss_getenv("D3D9SW_VERIFY", v, sizeof(v));

		cached = (n && n < sizeof(v)) ? atoi(v) : 1;
	}
	return cached;
}

/* Compares what is in the section against what is in memory, which is the one
 * question about the copy that has never been asked.
 *
 * OSFE moves about 465 MB out of memory per save across some 750 regions, and
 * the same 465 MB back per restore - roughly 930 MB a cycle, of which precisely
 * zero bytes have ever been checked. memcpy is not the suspect; memcpy is
 * correct. The two unverified assumptions around it are the suspects, and they
 * are assumptions rather than guarantees:
 *
 *   At save, that the source holds still while we read it. Every thread we know
 *   about is suspended, but a thread created between collect_threads() and
 *   suspend_all() is never suspended at all, and kernel-side writes - a
 *   completing overlapped read landing in a buffer, for one - do not stop
 *   because a user-mode thread did.
 *
 *   At restore, that nothing touches the destination while we write it. Same
 *   enumeration gap, except now we are the writer and 465 MB of someone else's
 *   memory is the target.
 *
 * If either fails, the result is a snapshot that is internally inconsistent at
 * byte granularity - exactly the shape the half-pointer faults have, a 64-bit
 * value with one half from one moment and one half from another - with no bad
 * arithmetic anywhere and nothing a heap walk could ever see.
 *
 * Reports the first differing offset and how many words differ, not just a
 * yes/no, because "one word in one region" and "half of everything" are
 * different bugs and the count is what separates them.
 *
 * Returns 1 identical, 0 differing, -1 could not be checked. */
static int win_cmp(Window *w, unsigned long long pos, const void *mem, size_t n,
		   unsigned long long *first_diff, unsigned long long *words,
		   unsigned long long *was, unsigned long long *now)
{
	unsigned long long done = 0;
	int same = 1;

	while (n) {
		const unsigned char *view, *m = (const unsigned char *)mem;
		size_t chunk, k;

		if (!win_cover(w, pos))
			return -1;
		view = w->base + (size_t)(pos - w->off);
		chunk = (size_t)(w->off + w->size - pos);
		if (chunk > n)
			chunk = n;
		/* memcmp first because it is vectorised and almost always says
		 * "identical", so the word-by-word walk below is paid for only by the
		 * regions that actually differ. */
		if (memcmp(view, m, chunk)) {
			for (k = 0; k + sizeof(unsigned long long) <= chunk;
			     k += sizeof(unsigned long long))
				if (*(const unsigned long long *)(view + k) !=
				    *(const unsigned long long *)(m + k)) {
					if (same) {
						*first_diff = done + k;
						/* The two values are the whole diagnosis. A
						 * count that stepped by one is a counter and
						 * nearly harmless; a pointer that moved is a
						 * live data structure being rebuilt while we
						 * photograph it. */
						*was = *(const unsigned long long *)(view + k);
						*now = *(const unsigned long long *)(m + k);
					}
					same = 0;
					(*words)++;
				}
			for (; k < chunk; k++)
				if (view[k] != m[k]) {
					if (same)
						*first_diff = done + k;
					same = 0;
					(*words)++;
				}
		}
		mem = m + chunk;
		pos += chunk;
		n -= chunk;
		done += chunk;
	}
	return same;
}

static int win_copy(Window *w, unsigned long long pos, void *mem, size_t n, int to_section)
{
	while (n) {
		size_t chunk;
		unsigned char *view;
		if (!win_cover(w, pos))
			return 0;
		view = w->base + (size_t)(pos - w->off);
		chunk = (size_t)(w->off + w->size - pos);
		if (chunk > n)
			chunk = n;
		if (to_section)
			memcpy(view, mem, chunk);
		else
			memcpy(mem, view, chunk);
		mem = (unsigned char *)mem + chunk;
		pos += chunk;
		n -= chunk;
	}
	return 1;
}

/* Every thread in this process except the helper, which is the caller. The
 * thread that requested the rewind is included on purpose: it is spinning at a
 * known instruction, so its context is exactly what a restore needs to resume
 * the frame that asked for the save. */
static void collect_threads(void)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	THREADENTRY32 te;
	DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
	g_ctl->nids = 0;
	if (snap == INVALID_HANDLE_VALUE)
		return;
	te.dwSize = sizeof(te);
	if (Thread32First(snap, &te)) {
		do {
			if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
				continue;
			if (g_ctl->nids >= SS_MAX_THREADS)
				break;
			g_ctl->ids[g_ctl->nids++] = te.th32ThreadID;
		} while (Thread32Next(snap, &te));
	}
	CloseHandle(snap);
}

static void suspend_all(void)
{
	int i;
	for (i = 0; i < g_ctl->nids; i++) {
		HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
					      THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
				      FALSE, g_ctl->ids[i]);
		if (h && SuspendThread(h) == (DWORD)-1) {
			CloseHandle(h);
			h = NULL;
		}
		g_ctl->handles[i] = h;
		g_ctl->starts[i] = h ? start_of(h) : NULL;
	}
}

static void resume_all(int hold_fresh)
{
	int i;
	for (i = 0; i < g_ctl->nids; i++) {
		if (!g_ctl->handles[i])
			continue;
		if (!(hold_fresh && g_ctl->fresh[i]))
			ResumeThread(g_ctl->handles[i]);
		CloseHandle(g_ctl->handles[i]);
		g_ctl->handles[i] = NULL;
	}
}

/* What to do about threads that did not exist when the save was taken. The
 * rewound memory has no record of them, so the game will neither talk to them
 * nor wait on them, but they are still running against state that has moved
 * under their feet. */
enum { POLICY_REFUSE = 0, POLICY_HOLD, POLICY_RUN };

/* Whether to rewind every thread or only the one that asked.
 *
 * Restoring one thread is the conservative choice, but it leaves the rest
 * executing against globals that moved under them, which shows up as a game
 * that redraws the restored frame and then fails to carry on from it. Rewinding
 * all of them is the semantically correct answer - the whole program goes back,
 * not a slice of it - and the reason it was abandoned earlier, that stacks were
 * being rewound alongside TEBs and library data, no longer holds.
 *
 * MEASURED, and it contradicts the paragraph above: rewinding all threads costs
 * 47% of runs (14 of 30 died), restoring only the requester costs 3% (1 of 30),
 * across the same 30-cycle harness workload. The reason it was abandoned did
 * still hold - just not for the stated reason. The mechanism is directly
 * counted at the context-collection loop below: 48 of 56 threads sampled at
 * save time were parked INSIDE a syscall, and SetThreadContext does not
 * reliably reposition such a thread, because the kernel reinstates its own trap
 * frame when the wait completes. Rewinding a stack whose registers then refuse
 * to move gives save-time frames under present-time registers.
 *
 * What this does NOT establish, because the harness cannot see it: the failure
 * mode this mode was introduced to fix. Workers here recompile in a loop and
 * hold nothing across a restore, so 'redraws the restored frame and then fails
 * to carry on' is invisible to them and remains a real risk in the game.
 * Mode 0 is proven to crash far less, not proven correct. */
static int rewind_all_threads(void)
{
	char v[16];
	DWORD n;
	if (g_ctl->tmode >= 0)
		return g_ctl->tmode;
	n = ss_getenv("D3D9SW_REWIND_THREADS", v, sizeof(v));
	/* Tested past the first letter, because "on" and "off" share it. The old
	 * test was `v[0] == 'o'`, so writing the setting out in full to be explicit -
	 * D3D9SW_REWIND_THREADS=on - selected OFF, the exact opposite, in silence.
	 * A knob whose two values begin with the same character cannot be read by
	 * its first character. */
	g_ctl->tmode = 1;
	if (n > 0 && n < sizeof(v)) {
		char c0 = (char)(v[0] >= 'A' && v[0] <= 'Z' ? v[0] + 32 : v[0]);
		char c1 = (char)(v[1] >= 'A' && v[1] <= 'Z' ? v[1] + 32 : v[1]);

		if (c0 == '0' || c0 == 'n' || c0 == 'f' || (c0 == 'o' && c1 == 'f'))
			g_ctl->tmode = 0;
	}
	return g_ctl->tmode;
}

static int policy(void)
{
	char v[16];
	DWORD n;
	if (g_ctl->policy >= 0)
		return g_ctl->policy;
	n = ss_getenv("D3D9SW_REWIND_NEWTHREADS", v, sizeof(v));
	g_ctl->policy = POLICY_REFUSE;
	if (n > 0 && n < sizeof(v)) {
		if (v[0] == 'h' || v[0] == 'H')
			g_ctl->policy = POLICY_HOLD;
		else if (v[0] == 'r' || v[0] == 'R')
			g_ctl->policy = POLICY_RUN;
	}
	return g_ctl->policy;
}

static void slot_release(Slot *s)
{
	if (s->sect) {
		CloseHandle(s->sect);
		s->sect = NULL;
	}
	s->valid = 0;
	s->nregs = 0;
	s->nthreads = 0;
	s->bytes = 0;
}

/* Is any thread executing inside the JIT?
 *
 * The snapshot suspends threads at whatever instruction they happen to be on,
 * which is fine for a structure that is either updated or not, and fatal for one
 * caught halfway. Mono's JIT-info table is the second kind: mono_jit_info_table_add
 * grows a chunk array and bumps a count, and a snapshot taken between those two
 * stores restores a table whose count promises more entries than the array holds.
 * jit_info_table_copy_and_split_chunk then walks count entries doing
 * "inc dword ptr [rax]" on each, reads uninitialised heap bytes as the last
 * chunk pointer, and writes through it.
 *
 * That is the crash at mono_2_0_bdwgc!jit_info_table_copy_and_split_chunk+0xa5,
 * where the address written was 5 GB past the top of all mapped memory - not a
 * stale pointer but garbage read as one, sourced from a region the fault triage
 * marked "restored from the save".
 *
 * No boundary can be moved to fix it, because both halves of the inconsistency
 * are on the same side. The only repair is not to take the picture at that
 * moment, and the JIT is bursty rather than continuous, so waiting works.
 *
 * Deliberately coarse: any thread anywhere inside the module, not just inside the
 * table functions. Naming individual functions means hard-coding offsets into a
 * shipped binary, and the whole module is a few hundred microseconds of a game
 * frame - cheap to wait out. */
static int in_the_jit(unsigned *who, uintptr_t *where)
{
	static uintptr_t lo, hi;
	int i;

	if (!hi) {
		HMODULE m = GetModuleHandleA("mono-2.0-bdwgc.dll");
		if (!m)
			m = GetModuleHandleA("mono-2.0-sgen.dll");
		if (!m)
			m = GetModuleHandleA("mono.dll");
		if (!m) {
			hi = 1; /* no Mono: never wait, and never look again */
			return 0;
		}
		{
			IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)m;
			IMAGE_NT_HEADERS *nt =
				(IMAGE_NT_HEADERS *)((unsigned char *)m + dos->e_lfanew);
			lo = (uintptr_t)m;
			hi = lo + nt->OptionalHeader.SizeOfImage;
		}
	}
	if (hi <= 1)
		return 0;
	for (i = 0; i < g_ctl->nids; i++) {
		CONTEXT c;
		uintptr_t pc;

		if (!g_ctl->handles[i])
			continue;
		memset(&c, 0, sizeof(c));
		c.ContextFlags = CONTEXT_CONTROL;
		if (!GetThreadContext(g_ctl->handles[i], &c))
			continue;
#if defined(_M_IX86) || defined(__i386__)
		pc = (uintptr_t)c.Eip;
#else
		pc = (uintptr_t)c.Rip;
#endif
		if (pc >= lo && pc < hi) {
			if (who)
				*who = g_ctl->ids[i];
			if (where)
				*where = pc;
			return 1;
		}
	}
	return 0;
}

/* How many times to let go and look again before saving anyway.
 *
 * Saving anyway rather than refusing, because a save that silently does not
 * happen is worse than one taken at a slightly bad moment: the player pressed a
 * key and has every right to expect a slot. The log says which it got. */
static int settle_tries(void)
{
	static int v = -1;
	if (v < 0) {
		char b[16];
		DWORD n = ss_getenv("D3D9SW_SETTLE", b, sizeof(b));
		v = (n && n < sizeof(b)) ? atoi(b) : 24;
	}
	return v;
}

static int do_save(int slotno)
{
	Slot *s = &g_ctl->slots[slotno];
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t addr = 0;
	unsigned long long total = 0, pos = 0;
	unsigned long long free_total = 0, free_largest = 0, used_total = 0, writable_total = 0;
	uintptr_t top = 0;
	Window w;
	int i, rc = 0;
	/* Phase attribution, so a save that gets slower is answered by reading a
	 * line rather than by experiment. Added while chasing an 18-second save that
	 * turned out not to exist - see request(), where a restored thread reports
	 * save-to-restore wall time as if it were a save duration. */
	LARGE_INTEGER pf, t0, t_susp, t_excl, t_rel, t_walk, t_copy;

	QueryPerformanceFrequency(&pf);
	QueryPerformanceCounter(&t0);
	collect_threads();
	suspend_all();
	/* Let go and look again while anything is inside the JIT. Threads must be
	 * suspended to read their contexts, so each attempt is a full suspend, and
	 * the release has to be real - a thread cannot leave the JIT while held. */
	{
		int tries = settle_tries(), n = 0;
		unsigned who = 0;
		uintptr_t where = 0;

		while (n < tries && in_the_jit(&who, &where)) {
			resume_all(0);
			Sleep(2);
			collect_threads();
			suspend_all();
			n++;
		}
		if (n && !in_the_jit(NULL, NULL))
			ss_log("  settled after %d attempt(s): no thread inside the JIT\n", n);
		else if (n)
			ss_log("  WARNING: still inside the JIT after %d attempt(s) (thread %u at "
			       "%p); saving anyway, and this snapshot may hold a half-updated "
			       "JIT-info table\n",
			       n, who, (void *)where);
	}
	QueryPerformanceCounter(&t_susp);
	build_exclusions();
	/* Every thread is stopped, so this is the registered set at the instant the
	 * snapshot describes. */
	fntab_note_save();
	/* Said out loud, because a fix that quietly does nothing is worse than no
	 * fix: it looks like the mechanism was wrong. */
	fntab_report();
	QueryPerformanceCounter(&t_excl);
	guard_release();
	slot_release(s);
	QueryPerformanceCounter(&t_rel);

	while (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
		uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		if (next <= addr)
			break;
		if (mbi.State == MEM_FREE) {
			free_total += mbi.RegionSize;
			if (mbi.RegionSize > free_largest)
				free_largest = mbi.RegionSize;
		} else {
			used_total += mbi.RegionSize;
			/* Writable state regardless of whether we choose to take it:
			 * the difference against what is captured is the part of the
			 * process a rewind knowingly leaves in the present. */
			if ((mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE) &&
			    !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
			    (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
					    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
				writable_total += mbi.RegionSize;
		}
		top = next;
		if (region_wanted(&mbi) && s->nregs < SS_MAX_REGIONS) {
			s->regs[s->nregs].base = (uintptr_t)mbi.BaseAddress;
			s->regs[s->nregs].size = mbi.RegionSize;
			s->regs[s->nregs].prot = mbi.Protect;
			s->regs[s->nregs].alloc_base = (uintptr_t)mbi.AllocationBase;
			s->regs[s->nregs].type = mbi.Type;
			total += mbi.RegionSize;
			s->nregs++;
		}
		addr = next;
	}

	/* Ceiling above 2 GB means the large-address-aware flag took effect. The
	 * largest free block is what any future allocation arena has to fit in. */
	ss_log("  address space: top %p, %.0f MB used, %.0f MB free, largest free block "
	       "%.0f MB\n",
	       (void *)top, (double)used_total / (1024.0 * 1024.0),
	       (double)free_total / (1024.0 * 1024.0),
	       (double)free_largest / (1024.0 * 1024.0));

	QueryPerformanceCounter(&t_walk);
	s->sect = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
				     (DWORD)(total >> 32), (DWORD)total, NULL);
	if (!s->sect) {
		ss_log("save: section %llu bytes failed, err=%lu\n", total, GetLastError());
		goto done;
	}

	memset(&w, 0, sizeof(w));
	w.sect = s->sect;
	w.total = total;
	for (i = 0; i < s->nregs; i++) {
		if (!win_copy(&w, pos, (void *)s->regs[i].base, s->regs[i].size, 1)) {
			ss_log("save: window failed at region %d, err=%lu\n", i, GetLastError());
			win_close(&w);
			slot_release(s);
			goto done;
		}
		pos += s->regs[i].size;
	}
	win_close(&w);
	/* Re-read while everything is still suspended. Nothing here allocates or
	 * takes a lock, which is the rule this window is governed by - the
	 * unwind-table reconciliation froze the process by breaking it. Two reads and
	 * a memcmp break nothing. */
	if (verify_mode()) {
		Window wv;
		unsigned long long vpos = 0, words = 0, tot_words = 0;
		uintptr_t v_at[8];
		unsigned long long v_was[8], v_now[8], v_words[8];
		int vi, differ = 0, unchecked = 0, named = 0;
		LARGE_INTEGER v0, v1, vf;

		QueryPerformanceFrequency(&vf);
		QueryPerformanceCounter(&v0);
		memset(&wv, 0, sizeof(wv));
		wv.sect = s->sect;
		wv.total = total;
		for (vi = 0; vi < s->nregs; vi++) {
			unsigned long long fd = 0, was = 0, now = 0;
			int r;

			words = 0;
			r = win_cmp(&wv, vpos, (const void *)s->regs[vi].base, s->regs[vi].size,
				    &fd, &words, &was, &now);
			if (r < 0) {
				unchecked++;
			} else if (!r) {
				differ++;
				tot_words += words;
				/* Recorded, not reported. Reporting means ss_log, which
				 * formats through the C runtime and calls ss_heap_of, whose
				 * static buffer lives in this module's data - both inside the
				 * snapshot. The first version logged from here and duly found
				 * a changed region every save containing the ASCII text ", in
				 * the", which was ss_heap_of's buffer being filled by the line
				 * describing the previous finding. Nothing between the copy
				 * and the last comparison may write to memory. */
				if (named < 8) {
					v_at[named] = s->regs[vi].base + fd;
					v_was[named] = was;
					v_now[named] = now;
					v_words[named] = words;
					named++;
				}
			}
			vpos += s->regs[vi].size;
		}
		win_close(&wv);
		/* Safe to write memory again from here: every comparison is done. */
		for (vi = 0; vi < named; vi++) {
			const char *mod;
			unsigned moff;

			mod = ss_module(v_at[vi], &moff);
			ss_log("  VERIFY: %p CHANGED while we copied it - snapshot has %llx, "
			       "memory now has %llx, %llu word(s) in this region, %s\n",
			       (void *)v_at[vi], v_was[vi], v_now[vi], v_words[vi],
			       mod ? mod : ss_heap_of(v_at[vi]));
			if (v_now[vi] > v_was[vi] && v_now[vi] - v_was[vi] < 0x10000)
				ss_log("      that is +%llu, so a counter or an index rather than "
				       "a pointer\n",
				       v_now[vi] - v_was[vi]);
		}
		/* Asks directly whether a thread exists that we never stopped. do_save
		 * enumerates threads and then suspends them, and a thread created between
		 * those two steps is never suspended at all - it keeps running while we
		 * photograph the memory it is writing. That was a candidate before this
		 * check existed; now that memory demonstrably changes under us, it is the
		 * first thing to rule in or out, and the enumeration is nearly free. */
		if (differ) {
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			THREADENTRY32 te;
			DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
			int strangers = 0;

			if (snap != INVALID_HANDLE_VALUE) {
				te.dwSize = sizeof(te);
				if (Thread32First(snap, &te))
					do {
						int k, known = 0;

						if (te.th32OwnerProcessID != pid ||
						    te.th32ThreadID == self)
							continue;
						for (k = 0; k < g_ctl->nids; k++)
							if (g_ctl->ids[k] == te.th32ThreadID)
								known = 1;
						if (known)
							continue;
						strangers++;
						ss_log("  VERIFY: thread %lu exists but was NEVER "
						       "SUSPENDED - it was created after the "
						       "enumeration\n",
						       (unsigned long)te.th32ThreadID);
					} while (Thread32Next(snap, &te));
				CloseHandle(snap);
			}
			if (!strangers)
				ss_log("  VERIFY: every thread in the process was suspended, so "
				       "the writer is NOT an unsuspended thread\n");
		}
		QueryPerformanceCounter(&v1);
		/* Printed every time, including when it is clean, because a check whose
		 * absence of output means "fine" is a check nobody can tell is running. */
		ss_log("  verify at save: %d region(s), %d CHANGED (%llu word(s)), %d "
		       "unchecked, %.1f ms%s\n",
		       s->nregs, differ, tot_words, unchecked,
		       (double)(v1.QuadPart - v0.QuadPart) * 1000.0 / (double)vf.QuadPart,
		       differ ? " <<< THE SNAPSHOT IS NOT A SINGLE MOMENT" : "");
	}
	QueryPerformanceCounter(&t_copy);
	{
		double q = (double)pf.QuadPart / 1000.0;

		ss_log("  save cost ms: suspend %.1f, exclusions %.1f, release %.1f, "
		       "walk %.1f, section+copy %.1f\n",
		       (double)(t_susp.QuadPart - t0.QuadPart) / q,
		       (double)(t_excl.QuadPart - t_susp.QuadPart) / q,
		       (double)(t_rel.QuadPart - t_excl.QuadPart) / q,
		       (double)(t_walk.QuadPart - t_rel.QuadPart) / q,
		       (double)(t_copy.QuadPart - t_walk.QuadPart) / q);
	}

	for (i = 0; i < g_ctl->nids; i++) {
		ThreadState *t;
		if (!g_ctl->handles[i] || s->nthreads >= SS_MAX_THREADS)
			continue;
		if (g_ctl->transient[i])
			continue;
		if (!rewind_all_threads() && g_ctl->ids[i] != g_ctl->req_tid)
			continue;
		t = &s->threads[s->nthreads];
		memset(&t->ctx, 0, sizeof(t->ctx));
		t->ctx.ContextFlags = CONTEXT_FULL;
		/* A failure here used to skip the thread in silence, and that is the
		 * worst possible outcome rather than a harmless one: the thread's
		 * STACK is still captured and will still be restored, while its
		 * registers are not, so it resumes with present-time registers over
		 * rewound stack memory and returns into a frame that no longer
		 * exists. Precisely the mismatch invariant F exists to prevent.
		 *
		 * Still skipped, because a context we could not read is not a
		 * context we can write - but no longer silently, so if it ever
		 * happens the log names the thread instead of leaving an
		 * unexplained death. */
		if (!GetThreadContext(g_ctl->handles[i], &t->ctx)) {
			ss_log("  WARNING: thread %u - could not read its context, so it "
			       "will resume with present-time registers over a rewound "
			       "stack (err %lu)\n",
			       (unsigned)g_ctl->ids[i], GetLastError());
			continue;
		}
		t->tid = g_ctl->ids[i];
		/* Where a thread is parked says whether the un-restorable sync
		 * state matters: an instruction pointer inside a system module
		 * means it is sitting in a wait we cannot reproduce.
		 *
		 * Named through ss_module rather than GetModuleFileNameA, which is
		 * what this used to call. That is a loader call, it reaches
		 * RtlAllocateHeap, and it ran here with every other thread frozen -
		 * so a thread holding the heap or loader lock would have deadlocked
		 * the save. ss_module answers from the module table already
		 * collected for this snapshot and touches nothing. The reason was
		 * written down at the top of this file for the fault path and simply
		 * never applied to the save path.
		 *
		 * The syscall test is the measurement this line was missing. x64
		 * enters the kernel through `syscall`, encoded 0F 05, and returns to
		 * the instruction after it - so a pc whose two preceding bytes are
		 * 0F 05 means this thread was inside a system call when we stopped
		 * it. That is exactly the state the exclusion comment warns about:
		 * "SetThreadContext does not reliably take on a thread parked inside
		 * a syscall - the kernel reinstates its own trap frame when the wait
		 * completes". Since worker stacks ARE rewound, such a thread can
		 * resume with save-time stack under present-time registers.
		 *
		 * ctx_verify cannot see this, because it reads the context back
		 * while the thread is still suspended and the kernel does its
		 * reinstating at resume. So counting the threads in the vulnerable
		 * state is the only cheap evidence available: if none are, the whole
		 * trap-frame story is dead. */
		{
#if defined(_M_IX86) || defined(__i386__)
			uintptr_t pc = (uintptr_t)t->ctx.Eip;
			const char *how = "";
#else
			uintptr_t pc = (uintptr_t)t->ctx.Rip;
			const unsigned char *b = (const unsigned char *)(pc - 2);
			const char *how = (pc > 2 && ss_readable(pc - 2, 2) &&
					   b[0] == 0x0F && b[1] == 0x05)
						  ? ", INSIDE A SYSCALL"
						  : "";
#endif
			unsigned off = 0;
			const char *name = ss_module(pc, &off);

			ss_log("  thread %lu parked at %p (%s+%X)%s\n",
			       (unsigned long)g_ctl->ids[i], (void *)pc,
			       name ? name : "?", off, how);
		}
		t->have_tls = 0;
		{
			unsigned char *teb = (unsigned char *)teb_of(g_ctl->handles[i]);
			if (teb) {
				memcpy(t->tls, teb + TEB_TLS_SLOTS, sizeof(t->tls));
				t->have_tls = 1;
			}
		}
		s->nthreads++;
	}

	/* Only the game's own threads are worth comparing against later; system
	 * pool workers come and go regardless of anything we do. */
	s->nids = 0;
	for (i = 0; i < g_ctl->nids; i++) {
		if (g_ctl->transient[i])
			continue;
		s->ids[s->nids] = g_ctl->ids[i];
		s->starts[s->nids] = g_ctl->starts[i];
		s->nids++;
	}
	ss_log("  coverage: %.1f MB captured of %.1f MB writable, %.1f MB left in the present "
	       "(%.2f%%)\n",
	       (double)total / (1024.0 * 1024.0), (double)writable_total / (1024.0 * 1024.0),
	       (double)(writable_total - total) / (1024.0 * 1024.0),
	       writable_total ? 100.0 * (double)(writable_total - total) / (double)writable_total
			      : 0.0);

	/* Does the save contradict the heap partition?
	 *
	 * Deciding a heap stays in the present is worth nothing if regions inside it
	 * are captured anyway: the restore then rewinds part of that heap and leaves
	 * the rest, which tears objects in half. A fault caught exactly this - an
	 * object reported both "in the game's heap", which we hold, and "restored
	 * from the save". Segment ownership is inferred by following pointers out of
	 * heap headers, so it can miss segments, and anything it misses ends up
	 * captured. This counts the disagreement instead of assuming there is none. */
	{
		unsigned long long bad_bytes = 0;
		int bad = 0, worst = -1;

		for (i = 0; i < s->nregs; i++) {
			int hi = heap_index_of(s->regs[i].base);

			if (hi < 0 || g_ctl->heap_ours[hi])
				continue;
			bad++;
			bad_bytes += s->regs[i].size;
			if (worst < 0)
				worst = i;
		}
		if (bad)
			ss_log("  PARTITION LEAK: %d region(s) / %.1f MB are inside a heap we "
			       "leave in the present but were captured anyway, first at %p "
			       "in %s. The restore will rewind part of that heap\n",
			       bad, (double)bad_bytes / (1024.0 * 1024.0),
			       (void *)s->regs[worst].base,
			       g_ctl->heap_name[heap_index_of(s->regs[worst].base)]);
		else
			ss_log("  partition: consistent, no captured region sits in a held "
			       "heap\n");
	}

	time_now(&s->clock);
	s->nevents = events_capture(s->events, SS_MAX_EVENTS);
	s->nfiles = for_each_file(s->files, SS_MAX_FILES);
	s->bytes = total;
	s->valid = 1;
	g_ctl->last_mb = (double)total / (1024.0 * 1024.0);
	ss_log("save: slot %d, %d regions, %.1f MB, %d threads, %d context(s), %d file(s)\n",
	       slotno, s->nregs, g_ctl->last_mb, g_ctl->nids, s->nthreads, s->nfiles);
	rc = 1;

done:
	resume_all(0);
	heap_check("before the save");
	return rc;
}

/* One CONTEXT field, named, so a mismatch can say WHICH register failed rather
 * than that something did. Worth the table: "the set did not take" and "rdi
 * specifically did not take" are different findings, and the second is the one
 * that matches the evidence. */
struct CtxField {
	const char *name;
	unsigned off;
};

#define SS_CTXF(f) { #f, (unsigned)offsetof(CONTEXT, f) }

static const struct CtxField g_ctx_fields[] = {
#if defined(_M_IX86) || defined(__i386__)
	SS_CTXF(Eip), SS_CTXF(Esp), SS_CTXF(Ebp), SS_CTXF(Eax), SS_CTXF(Ebx),
	SS_CTXF(Ecx), SS_CTXF(Edx), SS_CTXF(Esi), SS_CTXF(Edi),
#else
	SS_CTXF(Rip), SS_CTXF(Rsp), SS_CTXF(Rbp), SS_CTXF(Rax), SS_CTXF(Rbx),
	SS_CTXF(Rcx), SS_CTXF(Rdx), SS_CTXF(Rsi), SS_CTXF(Rdi), SS_CTXF(R8),
	SS_CTXF(R9), SS_CTXF(R10), SS_CTXF(R11), SS_CTXF(R12), SS_CTXF(R13),
	SS_CTXF(R14), SS_CTXF(R15),
#endif
};

#define SS_NCTXF (sizeof(g_ctx_fields) / sizeof(g_ctx_fields[0]))

/* Asks each thread whether the registers we just set are the registers it
 * actually has.
 *
 * This is the register-level counterpart of the copy verification, and it
 * exists for the same reason: SetThreadContext's result had never been checked,
 * let alone its effect. The comment on the exclusion policy has warned since it
 * was written that the call "does not reliably take on a thread parked inside a
 * syscall - the kernel reinstates its own trap frame when the wait completes",
 * and that warning was used to justify excluding stacks while the code went on
 * setting contexts anyway.
 *
 * If a set silently fails, the thread resumes with save-time MEMORY and
 * present-time REGISTERS. That mixture is the only mechanism proposed so far
 * that produces what the harness actually caught: rdi, a callee-saved register
 * masked to 0..63 two instructions after it is written, holding 0x0FFFFFFF at
 * an indexed load. It also accounts for the wider family of 64-bit values with
 * one half current and one half stale.
 *
 * Deliberately reports per-field rather than pass/fail, because "the set did
 * not take at all" and "the integer registers took and rip did not" point at
 * completely different fixes.
 *
 * Reads back with the same flags we set. A field that differs is not proof the
 * kernel refused it - a thread resumed between the set and the read would also
 * differ - but nothing is resumed until later in do_load, so within this window
 * a difference means the write did not land. */
static void ctx_verify(HANDLE h, const CONTEXT *want, unsigned tid, int *nbad,
		       unsigned *field_bad)
{
	CONTEXT got;
	unsigned f, bad = 0;

	memset(&got, 0, sizeof(got));
	got.ContextFlags = CONTEXT_FULL;
	if (!GetThreadContext(h, &got)) {
		ss_log("  WARNING: thread %u - could not read its context back, so "
		       "whether the registers took is unknown (err %lu)\n",
		       tid, GetLastError());
		(*nbad)++;
		return;
	}
	for (f = 0; f < SS_NCTXF; f++) {
		const unsigned char *a = (const unsigned char *)want + g_ctx_fields[f].off;
		const unsigned char *b = (const unsigned char *)&got + g_ctx_fields[f].off;

		if (memcmp(a, b, sizeof(void *)) == 0)
			continue;
		field_bad[f]++;
		/* First two per thread only. A thread whose whole context was
		 * refused would otherwise print seventeen lines and bury the
		 * interesting case, which is one or two fields differing. */
		if (bad < 2)
			ss_log("  thread %u: %s did NOT take - we set %p, it has %p\n",
			       tid, g_ctx_fields[f].name, *(void *const *)a,
			       *(void *const *)b);
		bad++;
	}
	if (bad)
		(*nbad)++;
}

static int do_load(int slotno)
{
	Slot *s = &g_ctl->slots[slotno];
	Window w;
	unsigned long long pos = 0;
	int i, j, restored = 0, skipped = 0, tls_done = 0;

	if (!s->valid)
		return 0;
	collect_threads();
	suspend_all();
	build_exclusions();
	/* Before any memory moves: see fntab_reconcile_down on why the order is not
	 * negotiable. */
	fntab_reconcile_down();

	/* Checked before a single byte is written, so a refusal leaves the
	 * process exactly as it was. A save and a restore taken in the same part
	 * of the game agree here; crossing a scene boundary does not, because
	 * loading spawns threads. */
	{
		int fresh = 0, recycled = 0, gone = 0;
		for (i = 0; i < g_ctl->nids; i++) {
			int known = 0, role = 0;
			if (g_ctl->transient[i])
				continue;
			for (j = 0; j < s->nids; j++) {
				if (s->ids[j] == g_ctl->ids[i])
					known = 1;
				else if (s->starts[j] && s->starts[j] == g_ctl->starts[i])
					role = 1;
			}
			g_ctl->fresh[i] = (char)!known;
			if (known)
				continue;
			fresh++;
			/* Same entry point under a new id means a pool retired a
			 * thread and started another: the process is doing the same
			 * work, and the saved context could be handed to this thread
			 * instead of being discarded. A genuinely new entry point is
			 * work that did not exist at all when the save was taken. */
			if (role)
				recycled++;
			ss_log("  thread %lu is newer than the save, entry %p%s\n",
			       (unsigned long)g_ctl->ids[i], g_ctl->starts[i],
			       role ? " (same role as a saved thread)" : " (new role)");
		}
		for (j = 0; j < s->nids; j++) {
			int live = 0;
			for (i = 0; i < g_ctl->nids; i++)
				if (g_ctl->ids[i] == s->ids[j])
					live = 1;
			if (!live) {
				gone++;
				ss_log("  saved thread %lu is gone, entry %p\n",
				       (unsigned long)s->ids[j], s->starts[j]);
			}
		}
		if (fresh || gone)
			ss_log("  divergence: %d newer (%d same role), %d gone\n", fresh,
			       recycled, gone);
		if (fresh && policy() == POLICY_REFUSE) {
			ss_log("load: refused, %d thread(s) newer than the save "
			       "(D3D9SW_REWIND_NEWTHREADS=hold or run to override)\n",
			       fresh);
			resume_all(0);
			return 0;
		}
		g_ctl->last_fresh = fresh;
	}

	/* Nothing is written until every region is known to be restorable. A
	 * partial rewind is worse than none: the process keeps running with most
	 * of its state in the past and the rest in the present, which is how a
	 * silent eight-region failure turned into a game that misbehaved without
	 * anything reporting an error. */
	for (i = 0; i < s->nregs; i++) {
		MEMORY_BASIC_INFORMATION mbi;
		uintptr_t base = s->regs[i].base, size = s->regs[i].size;

		/* The thread set can differ from the save, so re-test: what was
		 * ordinary memory then may be a live stack or TEB now. */
		if (region_excluded(base, size)) {
			ss_log("  region %p+%lx is excluded now but was saved\n", (void *)base,
			       (unsigned long)size);
			skipped++;
			continue;
		}
		/* Fitting is not the same as belonging. An address the game has
		 * since freed can be handed to an unrelated allocation of a
		 * convenient size, and writing the old contents there corrupts a
		 * live object instead of restoring a dead one. The allocation base
		 * and type say whether this is the same piece of memory or merely
		 * the same address. */
		if (VirtualQuery((LPCVOID)base, &mbi, sizeof(mbi)) == sizeof(mbi) &&
		    mbi.State == MEM_COMMIT) {
			if (mbi.Type != s->regs[i].type ||
			    (uintptr_t)mbi.AllocationBase != s->regs[i].alloc_base) {
				ss_log("  region %p+%lx now belongs elsewhere: "
				       "alloc %p vs %p, type %lx vs %lx\n",
				       (void *)base, (unsigned long)size, mbi.AllocationBase,
				       (void *)s->regs[i].alloc_base, (unsigned long)mbi.Type,
				       (unsigned long)s->regs[i].type);
				skipped++;
				continue;
			}
			/* Belongs to us and starts committed, but the run that
			 * VirtualQuery described may stop short of the region. Only a
			 * run that covers it needs nothing done. */
			if (mbi.RegionSize >= size)
				continue;
		}
		{
			/* The saved regions carved from one allocation give its
			 * extent, which is what has to be reserved again when the
			 * whole allocation has gone rather than just this slice. */
			uintptr_t abase = s->regs[i].alloc_base, aend = 0;
			int k;

			for (k = 0; k < s->nregs; k++)
				if (s->regs[k].alloc_base == abase &&
				    s->regs[k].base + s->regs[k].size > aend)
					aend = s->regs[k].base + s->regs[k].size;
			if (!ensure_committed(base, size, abase, aend)) {
				ss_log("  region %p+%lx cannot be recommitted, err=%lu, "
				       "alloc %p+%lx\n",
				       (void *)base, (unsigned long)size, GetLastError(),
				       (void *)abase, (unsigned long)(aend - abase));
				skipped++;
			}
		}
	}

	/* Memory the process has acquired since the save is left in place: the
	 * rewound allocator has no record of it, so it is leaked rather than
	 * reused. A large figure means the process has changed shape and the
	 * restore is on thin ice even when every saved region checks out. */
	{
#define D_TOP 12
		MEMORY_BASIC_INFORMATION mbi;
		uintptr_t addr = 0;
		unsigned long long extra = 0;
		unsigned long long d_exec = 0, d_priv = 0, d_image = 0;
		uintptr_t top_size[D_TOP] = { 0 }, top_base[D_TOP] = { 0 };
		DWORD top_prot[D_TOP] = { 0 }, top_type[D_TOP] = { 0 };
		uintptr_t cand_base[256], cand_size[256];
		int ncand = 0;
		int nextra = 0;
		while (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
			uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
			if (next <= addr)
				break;
			if (region_wanted(&mbi)) {
				int known = 0;
				for (j = 0; j < s->nregs; j++)
					if (s->regs[j].base == (uintptr_t)mbi.BaseAddress) {
						known = 1;
						break;
					}
				if (!known) {
					int slot;

					extra += mbi.RegionSize;
					nextra++;
					/* Executable drift is jitted code, and a
					 * heap's own growth has metadata that was
					 * restored with it. Neither is ours to take
					 * back. What is left is memory some allocator
					 * mapped directly, which is where the bulk of
					 * it turned out to be. */
					if (ncand < (int)(sizeof(cand_base) / sizeof(cand_base[0])) &&
					    mbi.Type == MEM_PRIVATE &&
					    !(mbi.Protect &
					      (PAGE_EXECUTE | PAGE_EXECUTE_READ |
					       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
					    heap_index_of((uintptr_t)mbi.BaseAddress) < 0) {
						cand_base[ncand] = (uintptr_t)mbi.BaseAddress;
						cand_size[ncand] = mbi.RegionSize;
						ncand++;
					}
					if (mbi.Protect &
					    (PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
						d_exec += mbi.RegionSize;
					else if (mbi.Type == MEM_IMAGE)
						d_image += mbi.RegionSize;
					else
						d_priv += mbi.RegionSize;
					/* Keep the biggest few so the shape can be
					 * read, not just the total. An insertion into
					 * a fixed table costs nothing next to the
					 * region scan already being done. */
					for (slot = 0; slot < D_TOP; slot++) {
						if (mbi.RegionSize > top_size[slot]) {
							int m;
							for (m = D_TOP - 1; m > slot; m--) {
								top_size[m] = top_size[m - 1];
								top_base[m] = top_base[m - 1];
								top_prot[m] = top_prot[m - 1];
								top_type[m] = top_type[m - 1];
							}
							top_size[slot] = mbi.RegionSize;
							top_base[slot] = (uintptr_t)mbi.BaseAddress;
							top_prot[slot] = mbi.Protect;
							top_type[slot] = mbi.Type;
							break;
						}
					}
				}
			}
			addr = next;
		}
		ss_log("  drift: %d regions / %.1f MB present now but not in the save, policy %d, "
		       "%d unclaimed\n",
		       nextra, (double)extra / (1024.0 * 1024.0), drift_mode(), ncand);
		/* Drift is memory the process acquired after the snapshot, and the
		 * restore leaves every byte of it alone. That is survivable when it
		 * is a few megabytes of pool growth, and is something else entirely
		 * after a scene reload: executable drift is freshly jitted code
		 * whose unwind tables Windows has registered in lists whose heads
		 * are in module data we never rewind, which is the shape of the
		 * fail-fast we keep dying to. */
		if (nextra) {
			int slot;

			ss_log("  drift by kind: %.1f MB executable, %.1f MB private, "
			       "%.1f MB image\n",
			       (double)d_exec / (1024.0 * 1024.0),
			       (double)d_priv / (1024.0 * 1024.0),
			       (double)d_image / (1024.0 * 1024.0));
			for (slot = 0; slot < D_TOP && top_size[slot]; slot++) {
				int hi = heap_index_of(top_base[slot]);

				ss_log("    drift %p+%lx prot %lx type %lx, %s\n",
				       (void *)top_base[slot], (unsigned long)top_size[slot],
				       (unsigned long)top_prot[slot],
				       (unsigned long)top_type[slot],
				       hi >= 0 ? g_ctl->heap_name[hi] : "no heap claims it");
			}
		}
		if (drift_mode() >= 1 && ncand) {
			unsigned long long freed = 0;
			int ok = 0, i2;

			g_dc_n = 0;
			for (i2 = 0; i2 < ncand; i2++) {
				if (VirtualFree((LPVOID)cand_base[i2], (SIZE_T)cand_size[i2],
						MEM_DECOMMIT)) {
					/* Remembered so that a later fault in one of
					 * these ranges convicts this decision outright
					 * instead of being reported as ordinary reserved
					 * address space, which is what a decommitted page
					 * otherwise looks like. */
					if (g_dc_n < (int)(sizeof(g_dc_base) / sizeof(g_dc_base[0]))) {
						g_dc_base[g_dc_n] = cand_base[i2];
						g_dc_size[g_dc_n] = cand_size[i2];
						g_dc_n++;
					}
					freed += cand_size[i2];
					ok++;
				} else {
					ss_log("    drift %p+%lx will not decommit, err=%lu\n",
					       (void *)cand_base[i2], (unsigned long)cand_size[i2],
					       GetLastError());
				}
			}
			ss_log("  drift: decommitted %d of %d unclaimed region(s), %.1f MB\n", ok,
			       ncand, (double)freed / (1024.0 * 1024.0));
		}
	}

	if (skipped) {
		ss_log("load: refused, %d of %d regions unrestorable\n", skipped, s->nregs);
		resume_all(0);
		return 0;
	}

	memset(&w, 0, sizeof(w));
	w.sect = s->sect;
	w.total = s->bytes;

	/* Checked inside this loop rather than after it, because the loop puts each
	 * region's saved protection back as it goes - a region that was PAGE_NOACCESS
	 * at save time would fault on being read a moment later. Here it is still
	 * PAGE_EXECUTE_READWRITE, so the comparison is free of that problem. */
	{
		Window wv;
		unsigned long long vwords = 0;
		int vdiffer = 0, vunchecked = 0, vnamed = 0, vmode = verify_mode();
		LARGE_INTEGER v0, v1, vf;

		QueryPerformanceFrequency(&vf);
		QueryPerformanceCounter(&v0);
		memset(&wv, 0, sizeof(wv));
		wv.sect = s->sect;
		wv.total = s->bytes;

		for (i = 0; i < s->nregs; i++) {
			void *base = (void *)s->regs[i].base;
			SIZE_T size = (SIZE_T)s->regs[i].size;
			unsigned long long here = pos;
			DWORD old;
			int writable =
				VirtualProtect(base, size, PAGE_EXECUTE_READWRITE, &old) != 0;
			if (win_copy(&w, pos, base, size, 0))
				restored++;
			else
				skipped++;
			if (vmode && writable) {
				unsigned long long fd = 0, words = 0, was = 0, now = 0;
				int r = win_cmp(&wv, here, base, size, &fd, &words, &was, &now);
				(void)was;
				(void)now;

				if (r < 0) {
					vunchecked++;
				} else if (!r) {
					vdiffer++;
					vwords += words;
					if (vnamed++ < 8)
						ss_log("  VERIFY: region %d at %p did NOT come "
						       "back as saved - first difference at "
						       "+%llx, %llu word(s), heap %s\n",
						       i, base, fd, words,
						       ss_heap_of(s->regs[i].base));
				}
			}
			/* Back to the protection the region had when it was saved, not the
			 * one it happened to have a moment ago. */
			if (writable)
				VirtualProtect(base, size, s->regs[i].prot, &old);
			pos += size;
		}
		win_close(&wv);
		QueryPerformanceCounter(&v1);
		if (vmode)
			ss_log("  verify at restore: %d region(s), %d WRONG (%llu word(s)), %d "
			       "unchecked, %.1f ms%s\n",
			       s->nregs, vdiffer, vwords, vunchecked,
			       (double)(v1.QuadPart - v0.QuadPart) * 1000.0 / (double)vf.QuadPart,
			       vdiffer ? " <<< THE RESTORE DID NOT TAKE" : "");
	}
	win_close(&w);
	/* Memory is back, so ntdll's list can be put back to match it. Unstick first
	 * and unconditionally: a lock the restore froze in the held state wedges the
	 * process whether or not the reconciliation itself is enabled. */
	fntab_unstick();
	cs_unstick();
	fntab_reconcile_up();
	/* Anything still held is now committed and part of the restored state. */
	g_ctl->nguarded = 0;
	g_ctl->guard_bytes = 0;

	/* After the contents are back, so the rewound allocator is in place before
	 * the memory it does not know about is taken away. */
	if (reclaim_tier() != RECLAIM_OFF)
		do_reclaim(s, reclaim_tier());

	{
		int seeked = 0, stale = 0;
		for (i = 0; i < s->nfiles; i++) {
			LARGE_INTEGER p;
			if (GetFileType(s->files[i].h) != FILE_TYPE_DISK ||
			    path_hash(s->files[i].h) != s->files[i].hash) {
				stale++;
				continue;
			}
			p.QuadPart = s->files[i].pos;
			if (SetFilePointerEx(s->files[i].h, p, NULL, FILE_BEGIN))
				seeked++;
		}
		if (stale)
			ss_log("  %d of %d saved file handles no longer match and were left "
			       "alone\n",
			       stale, s->nfiles);
		ss_log("  files: %d handle(s) seeked back\n", seeked);
	}

	{
	int ctx_set = 0, ctx_refused = 0, ctx_bad = 0;
	unsigned ctx_field_bad[SS_NCTXF];

	memset(ctx_field_bad, 0, sizeof(ctx_field_bad));
	for (i = 0; i < s->nthreads; i++) {
		for (j = 0; j < g_ctl->nids; j++) {
			if (g_ctl->ids[j] != s->threads[i].tid || !g_ctl->handles[j])
				continue;
			s->threads[i].ctx.ContextFlags = CONTEXT_FULL;
			/* Return value checked for the first time. A refusal here
			 * leaves the thread's registers entirely in the present
			 * over rewound memory, which is the worst of both. */
			if (!SetThreadContext(g_ctl->handles[j], &s->threads[i].ctx)) {
				ctx_refused++;
				ss_log("  WARNING: thread %u REFUSED the context we set "
				       "(err %lu) - it will resume with present-time "
				       "registers over rewound memory\n",
				       (unsigned)s->threads[i].tid, GetLastError());
			} else {
				ctx_set++;
				ctx_verify(g_ctl->handles[j], &s->threads[i].ctx,
					   (unsigned)s->threads[i].tid, &ctx_bad,
					   ctx_field_bad);
			}
			if (s->threads[i].have_tls) {
				unsigned char *teb =
					(unsigned char *)teb_of(g_ctl->handles[j]);
				if (teb) {
					memcpy(teb + TEB_TLS_SLOTS, s->threads[i].tls,
					       sizeof(s->threads[i].tls));
					tls_done++;
				}
			}
			break;
		}
	}
	if (ctx_refused || ctx_bad) {
		unsigned f;
		char which[256];
		size_t used = 0;

		which[0] = 0;
		for (f = 0; f < SS_NCTXF; f++) {
			int n;
			if (!ctx_field_bad[f])
				continue;
			n = ss_fmt(which + used, (int)(sizeof(which) - used), "%s%s x%u",
				      used ? ", " : "", g_ctx_fields[f].name,
				      ctx_field_bad[f]);
			if (n <= 0 || (size_t)n >= sizeof(which) - used)
				break;
			used += (size_t)n;
		}
		ss_log("  contexts: %d set, %d REFUSED, %d came back DIFFERENT from what "
		       "we set%s%s\n",
		       ctx_set, ctx_refused, ctx_bad, which[0] ? " - " : "", which);
	} else {
		ss_log("  contexts: %d set, all read back exactly as set\n", ctx_set);
	}
	}

	/* After the contexts, because whether a section may be left alone depends on
	 * whether its owner is one of the threads that just got one. */
	cs_reconcile(s);

	/* Reported before it is acted on. If this line always says none differ,
	 * the events never mattered and the whole question is closed by
	 * observation rather than argument; putting them back stays opt-in until
	 * it says otherwise. */
	{
		static int enabled = -1;
		int i, differ = 0, put = 0;
		if (enabled < 0) {
			char v[8];
			DWORD got = ss_getenv("D3D9SW_REWIND_EVENTS", v, sizeof(v));
			enabled = (got > 0 && got < sizeof(v) && v[0] == '1');
		}
		for (i = 0; i < s->nevents; i++) {
			DWORD flags;
			int now;
			if (!s->events[i].h || !GetHandleInformation(s->events[i].h, &flags))
				continue;
			now = event_probe(s->events[i].h);
			if (now == s->events[i].signalled)
				continue;
			differ++;
			if (!enabled)
				continue;
			if (s->events[i].signalled)
				SetEvent(s->events[i].h);
			else
				ResetEvent(s->events[i].h);
			put++;
		}
		ss_log("  events: %d tracked, %d differ from the save, %d put back\n", s->nevents,
		       differ, put);
	}

	time_rewind(&s->clock);
	g_frames_since_load = 0;
	/* Anchored at the first restore of the session and never moved again, so
	 * it measures how long the process has lasted since the rewind first
	 * touched it rather than how long since the most recent one. It lives in
	 * the control block because a static would be wound back to its
	 * pre-restore value by the second restore and re-anchor itself. */
	if (!g_ctl->anchor_tick)
		g_ctl->anchor_tick = GetTickCount();
	ss_log("  threads: %d context(s) restored, %d with TLS\n", s->nthreads, tls_done);
	ss_log("load: slot %d, %d restored, %d skipped, %d threads, %d newer than save%s\n",
	       slotno, restored, skipped, g_ctl->nids, g_ctl->last_fresh,
	       (g_ctl->last_fresh && policy() == POLICY_HOLD) ? " (held suspended)" : "");
	/* Breadcrumbs around the resume, because a silent death here is the oldest
	 * unexplained signature this project has and the log could not say which
	 * side of it the process was on.
	 *
	 * A run just ended with "load:" as its last line and NOTHING after -
	 * neither the resumed line nor the heap check, and no fault record in
	 * either log. That absence is itself the clue: a vectored handler cannot
	 * see a fail-fast, and heap corruption detected inside ntdll ends the
	 * process with one by design. So the two candidates are threads resuming
	 * onto rewound stacks, and the heap check being the first thing to touch a
	 * heap that is already broken. These two lines tell those apart for the
	 * cost of two writes. */
	ss_log("  resume: releasing %d thread(s)\n", g_ctl->nids);
	resume_all(policy() == POLICY_HOLD);
	ss_log("  resume: done, all threads runnable\n");
	heap_check("after the restore");
	/* The count answers a question the format strings cannot: whether Mono
	 * formats strings constantly during normal work, or only when something has
	 * gone wrong. If it is normally zero, the crash is on an error path and the
	 * printf fault is a symptom rather than the disease. */
	ss_log("  formatting: %ld call(s) through mono so far this session\n",
	       (long)(g_ctl ? g_ctl->fmt_calls : 0));
	return 1;
}

/* Giving back memory the process acquired after the save.
 *
 * Restoring contents is only half of a rewind; the address space has to match
 * too. Without this, an allocation made after the save survives it, and the
 * next time the game asks for memory the allocator - whose bookkeeping has been
 * rewound and knows nothing of that block - can hand out an address that is
 * still occupied.
 *
 * The hazard is that we deliberately do not rewind Steam, precisely because its
 * threads keep running, and running threads allocate. Freeing memory belonging
 * to them breaks the thing the exclusion was protecting. There is no reliable
 * way to attribute a page to its owner from the outside, so this is opt-in and
 * split by confidence:
 *
 *   grow - decommit only extensions of allocations that already existed at save
 *          time. These are almost always a heap growing, and the rewound heap
 *          header no longer describes the extra pages.
 *   all  - additionally release allocations that appeared wholesale. Higher
 *          yield, and the tier where a Steam allocation could be caught.
 *
 * Thread stacks and TEBs are never touched regardless of tier. */
static int reclaim_tier(void)
{
	char v[16];
	DWORD n;
	if (g_ctl->rmode >= 0)
		return g_ctl->rmode;
	n = ss_getenv("D3D9SW_REWIND_RECLAIM", v, sizeof(v));
	g_ctl->rmode = RECLAIM_OFF;
	if (n > 0 && n < sizeof(v)) {
		if (v[0] == 'g' || v[0] == 'G')
			g_ctl->rmode = RECLAIM_GROW;
		else if (v[0] == 'a' || v[0] == 'A' || v[0] == '1')
			g_ctl->rmode = RECLAIM_ALL;
	}
	return g_ctl->rmode;
}

static int in_live_stack(uintptr_t base, uintptr_t size)
{
	int i;
	for (i = 0; i < g_ctl->nstk; i++)
		if (base + size > g_ctl->stk_lo[i] && base < g_ctl->stk_hi[i])
			return 1;
	return 0;
}

static void do_reclaim(Slot *s, int tier)
{
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t addr = 0;
	unsigned long long freed = 0;
	int i, j, ngrow = 0, nwhole = 0;

	/* Collected in full before anything is freed: the walk would otherwise be
	 * enumerating a map that is changing underneath it. */
	g_ctl->nscratch = 0;
	while (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
		uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		uintptr_t base = (uintptr_t)mbi.BaseAddress;
		int known = 0, known_alloc = 0;
		if (next <= addr)
			break;
		addr = next;
		if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE)
			continue;
		if (region_excluded(base, mbi.RegionSize) || in_live_stack(base, mbi.RegionSize))
			continue;
		for (j = 0; j < s->nregs; j++) {
			if (s->regs[j].base == base)
				known = 1;
			if (s->regs[j].alloc_base == (uintptr_t)mbi.AllocationBase)
				known_alloc = 1;
		}
		if (known)
			continue;
		if (!known_alloc && tier < RECLAIM_ALL)
			continue;
		if (g_ctl->nscratch >= 8192)
			break;
		g_ctl->scratch[g_ctl->nscratch].base = base;
		g_ctl->scratch[g_ctl->nscratch].size = mbi.RegionSize;
		g_ctl->scratch[g_ctl->nscratch].alloc_base = (uintptr_t)mbi.AllocationBase;
		g_ctl->scratch[g_ctl->nscratch].type = known_alloc;
		g_ctl->nscratch++;
	}

	for (i = 0; i < g_ctl->nscratch; i++) {
		Region *r = &g_ctl->scratch[i];
		if (r->type) {
			if (VirtualFree((LPVOID)r->base, (SIZE_T)r->size, MEM_DECOMMIT)) {
				freed += r->size;
				ngrow++;
			}
		} else if (VirtualFree((LPVOID)r->alloc_base, 0, MEM_RELEASE)) {
			freed += r->size;
			nwhole++;
		}
	}

	ss_log("  reclaim(%s): %d grown regions decommitted, %d allocations released, "
	       "%.1f MB\n",
	       tier == RECLAIM_ALL ? "all" : "grow", ngrow, nwhole,
	       (double)freed / (1024.0 * 1024.0));
}

static DWORD WINAPI helper_main(LPVOID param)
{
	(void)param;
#if defined(_M_IX86) || defined(__i386__)
	g_ctl->helper_hi = (uintptr_t)__readfsdword(0x04);
	g_ctl->helper_lo = (uintptr_t)__readfsdword(0x08);
#else
	g_ctl->helper_hi = (uintptr_t)__readgsqword(0x08);
	g_ctl->helper_lo = (uintptr_t)__readgsqword(0x10);
#endif
	InterlockedExchange(&g_ctl->busy, 0);
	for (;;) {
		LONG req;
		LARGE_INTEGER h0, h1, hf;

		while ((req = InterlockedExchange(&g_ctl->request, REQ_NONE)) == REQ_NONE)
			Sleep(1);
		QueryPerformanceCounter(&h0);
		g_ctl->result = (req == REQ_SAVE) ? do_save((int)g_ctl->slot)
						  : do_load((int)g_ctl->slot);
		QueryPerformanceCounter(&h1);
		QueryPerformanceFrequency(&hf);
		/* This thread is not in the snapshot, so its clock readings survive a
		 * restore and are the only honest duration available. */
		g_ctl->helper_ms = hf.QuadPart ? (double)(h1.QuadPart - h0.QuadPart) * 1000.0 /
							 (double)hf.QuadPart
					       : 0.0;
		/* Both published before the release below, because the requester reads
		 * them the instant it sees busy fall. */
		InterlockedExchange(&g_ctl->done_req, req);
		/* Cleared last, and from memory outside the snapshot, so a restored
		 * thread sees the release rather than the value it was saved with. */
		InterlockedExchange(&g_ctl->busy, 0);
	}
}

static int ensure_helper(void)
{
	if (g_helper)
		return 1;
	g_ctl = (Control *)VirtualAlloc(NULL, sizeof(Control), MEM_COMMIT | MEM_RESERVE,
					PAGE_READWRITE);
	if (!g_ctl)
		return 0;
	memset(g_ctl, 0, sizeof(*g_ctl));
	g_ctl->busy = 1;
	g_ctl->policy = -1;
	g_ctl->tmode = -1;
	g_ctl->rmode = -1;
	g_ctl->guard = -1;
	savestate_hooks_install();
	ss_exclude(g_ctl, sizeof(Control));
	if (g_events)
		ss_exclude(g_events, sizeof(EventTrack));
	fntab_exclude();
	cs_exclude();
	/* Read-only after load, so rewinding it would put back identical bytes and
	 * be harmless - excluded anyway, because a settings store that the engine
	 * consults is bookkeeping and belongs with the rest of it. */
	if (g_cfg)
		ss_exclude(g_cfg, SS_CFG_MAX);
	/* Primed here so its cache is never first written inside the suspended
	 * window. Caching alone was not enough: the cache word lives in this
	 * module's data, which is rewound, so every restore put it back to -1 and the
	 * next save re-initialised it mid-copy - reporting itself as a changed region
	 * forever. A latch inside the snapshot is not a latch. */
	(void)verify_mode();
	/* Same reasoning, applied to the two ntdll entry points this engine calls
	 * with every other thread frozen. Both cache through GetProcAddress, and a
	 * cache that comes back empty from a restore sends the next call into the
	 * loader at the worst possible moment. */
	(void)query_file();
	(void)query_thread();
	g_ctl->nex_fixed = g_ctl->nex;
	/* Kept across launches, because the interesting session is always the one
	 * that just died and the next launch used to erase it. Two access
	 * violations were reported by Windows against builds whose logs, by the
	 * time they reached us, described a later and perfectly healthy run.
	 *
	 * Restores do not disturb the position: this handle is the one file
	 * for_each_file refuses to seek. */
	{
		/* Named after the host executable, because a single shared name has now
		 * destroyed evidence twice over. The test harness links this same engine,
		 * writes roughly 40 KB per session and can complete a hundred sessions in
		 * an afternoon, so it blew straight through the 8 MB cap below and
		 * truncated the file - taking every OSFE fault block with it. The
		 * comment on that cap said "a few dozen KB per session, so this is many
		 * months of play", which was true when the only host was a game someone
		 * plays for an hour.
		 *
		 * The point is not tidiness. The one measurement worth most right now is
		 * whether a fault shape seen in the harness also appears in the game, and
		 * that comparison needs both records to exist at the same time. */
		char name[MAX_PATH + 32], exe[MAX_PATH], *base = exe, *p;
		DWORD got = GetModuleFileNameA(NULL, exe, sizeof(exe));

		if (!got || got >= sizeof(exe))
			lstrcpynA(exe, "unknown", sizeof(exe));
		for (p = exe; *p; p++)
			if (*p == '\\' || *p == '/')
				base = p + 1;
		for (p = base; *p; p++)
			if (*p == '.') {
				*p = 0;
				break;
			}
		lstrcpynA(name, "d3d9_sw_savestate_", sizeof(name));
		lstrcatA(name, base);
		lstrcatA(name, ".txt");
		g_ctl->log = CreateFileA(name, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
					 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
	}
	g_logh = g_ctl->log;
	if (g_ctl->log != INVALID_HANDLE_VALUE) {
		LARGE_INTEGER sz, zero;
		zero.QuadPart = 0;
		/* A few dozen KB per session, so this is many months of play
		 * before it matters - but it is unattended on someone else's
		 * machine, so it does not get to grow without limit. */
		if (GetFileSizeEx(g_ctl->log, &sz) && sz.QuadPart > 8 * 1024 * 1024)
			SetEndOfFile(g_ctl->log);
		else
			SetFilePointerEx(g_ctl->log, zero, NULL, FILE_END);
	}
	cfg_load();
	{
		SYSTEMTIME lt;
		GetLocalTime(&lt);
		ss_log("\n===== session %04u-%02u-%02u %02u:%02u:%02u, pid %lu =====\n",
		       (unsigned)lt.wYear, (unsigned)lt.wMonth, (unsigned)lt.wDay,
		       (unsigned)lt.wHour, (unsigned)lt.wMinute, (unsigned)lt.wSecond,
		       (unsigned long)GetCurrentProcessId());
	}
	g_helper = CreateThread(NULL, 0, helper_main, NULL, 0, NULL);
	if (!g_helper) {
		VirtualFree(g_ctl, 0, MEM_RELEASE);
		g_ctl = NULL;
		return 0;
	}
	while (g_ctl->busy)
		YieldProcessor();
	ss_log("savestate ready, %s build, control %u KB at %p\n",
	       SW_VARIANT_STR(D3D9SW_VARIANT), (unsigned)(sizeof(Control) / 1024),
	       (void *)g_ctl);
	/* Every knob, and where its value came from.
	 *
	 * Seventeen settings could change what this engine does and not one of them
	 * had ever been written to the log, so the only way to find out whether a
	 * setting had arrived was to infer it from its side effects. An OSFE session
	 * meant to test D3D9SW_REWIND_THREADS=off ran the default instead and looked
	 * completely normal; it was caught only because that particular knob also
	 * moves the excluded-stack and restored-context counts, which most of these
	 * do not. A setting that cannot be confirmed is a setting that cannot be
	 * tested. */
	{
		static const char *const knobs[] = {
			"D3D9SW_REWIND_THREADS",  "D3D9SW_REWIND_NEWTHREADS",
			"D3D9SW_REWIND_CLOCK",	  "D3D9SW_REWIND_EVENTS",
			"D3D9SW_REWIND_GAMEHEAP", "D3D9SW_REWIND_SWHEAP",
			"D3D9SW_REWIND_ALLHEAPS", "D3D9SW_REWIND_RECLAIM",
			"D3D9SW_REWIND_GUARD",	  "D3D9SW_LOCKS",
			"D3D9SW_FNTAB",		  "D3D9SW_HEAPCHECK",
			"D3D9SW_VERIFY",	  "D3D9SW_DRIFT",
			"D3D9SW_SETTLE",	  "D3D9SW_REACH_AUDIT",
			"D3D9SW_REACH_GRAN"
		};
		int i, shown = 0;
		char v[64];

		ss_log("settings: d3d9_sw.cfg %s\n",
		       g_cfg_len > 0 ? "found" : "not present (environment only)");
		for (i = 0; i < (int)(sizeof(knobs) / sizeof(knobs[0])); i++) {
			char envv[64];
			int from_env = GetEnvironmentVariableA(knobs[i], envv, sizeof(envv)) > 0;

			if (!ss_getenv(knobs[i], v, sizeof(v)))
				continue;
			ss_log("  %s = %s (from the %s)\n", knobs[i], v,
			       from_env ? "environment" : "cfg file");
			shown++;
		}
		if (!shown)
			ss_log("  every setting is at its default\n");
		/* The one that decides whether thread stacks are rewound is printed as an
		 * effective value rather than a raw string, because it is the setting most
		 * likely to be under test and reading it wrong is how this line came to
		 * exist. */
		ss_log("  EFFECTIVE thread policy: %s\n",
		       rewind_all_threads()
			       ? "rewind ALL threads (stacks rewound, every context restored)"
			       : "rewind ONLY the requester (other stacks held, contexts left alone)");
	}
	/* The drift that breaks a restore across a scene reload is the collector
	 * mapping new heap sections after the snapshot was taken. Telling Boehm to
	 * take its heap up front stops that happening at all, but it reads this
	 * during runtime init, which is earlier than we load - so it has to come
	 * from the real environment, and a silent absence would look exactly like
	 * the setting not working. Reported so the two can be told apart. */
	{
		const char *ih = getenv("GC_INITIAL_HEAP_SIZE");
		const char *mh = getenv("GC_MAXIMUM_HEAP_SIZE");
		const char *dg = getenv("GC_DONT_GC");
		ss_log("  mono gc: initial=%s maximum=%s dont_gc=%s\n", ih ? ih : "(unset)",
		       mh ? mh : "(unset)", dg ? dg : "(unset)");
	}
	savestate_hooks_install();
	ss_log("hooks: %d clock import(s) redirected, %d event import(s), %ld event(s) seen\n",
	       g_hooked_time, g_hooked_event, g_events ? g_events->n : 0);
	/* Not reported here: Mono resolves its unwind-table calls lazily, and at
	 * start-up it has usually not done so yet. savestate_guard keeps trying and
	 * every save states where it got to. */
	return 1;
}

static int request(int req, int slot)
{
	if (slot < 0 || slot >= SAVESTATE_SLOTS)
		return 0;
	if (!ensure_helper())
		return 0;

	/* Cheap and idempotent, and it covers Mono having loaded after the helper
	 * was created - which is the normal order, since scripting starts after the
	 * graphics device that brought us in. */
	fntab_hook();

	/* The worker stacks would otherwise be snapshotted and then rewritten
	 * underneath threads that are still alive. */
	swrast_pool_shutdown();

	g_ctl->slot = slot;
	g_ctl->result = 0;
	g_ctl->req_tid = GetCurrentThreadId();
	InterlockedExchange(&g_ctl->busy, 1);
	InterlockedExchange(&g_ctl->request, req);

	/* A user-mode spin, not a kernel wait: this instruction is where a
	 * restored context resumes, and a thread parked inside a wait cannot be
	 * reliably resumed by SetThreadContext. */
	while (InterlockedCompareExchange(&g_ctl->busy, 0, 0))
		YieldProcessor();

	/* Execution reaching here does not mean the request above completed. A
	 * thread whose context was captured in that spin loop resumes at this exact
	 * instruction when a later restore puts it back, and every local it has -
	 * including req, and including any timestamp taken before the spin - comes
	 * back from the save with it. Such a thread believes it is finishing the
	 * save it was taking, and returns through savestate_save into the caller's
	 * save branch, which is why a working restore prints "saved".
	 *
	 * Timing it from this stack therefore measured save-to-restore wall time and
	 * reported it as a save duration: one 542 ms save and one restore 17.4
	 * seconds later were read as a 543 ms save followed by an 18-second one, and
	 * the resulting hunt for a nonexistent performance bug also produced a wrong
	 * crash diagnosis from the missing "restored" line. Both numbers now come
	 * from the helper's memory, which the snapshot excludes. */
	g_ctl->last_ms = g_ctl->helper_ms;
	return (int)g_ctl->result;
}

int savestate_save(int slot)
{
	return request(REQ_SAVE, slot);
}

int savestate_load(int slot)
{
	return request(REQ_LOAD, slot);
}

/* Hands back everything the guard is holding that has not been committed.
 *
 * Called before a new save, because reservations taken for the previous one are
 * dead weight the moment its region list is replaced. Without this the guard
 * ratchets: measured on a real session it took 226 MB in a single sweep and the
 * process never got it back, which in a 2 GB space is the difference between
 * working indefinitely and failing to allocate after a few restores. */
static void guard_release(void)
{
	MEMORY_BASIC_INFORMATION mbi;
	int i, freed = 0;
	unsigned long long bytes = 0;
	if (!g_ctl)
		return;
	for (i = 0; i < g_ctl->nguarded; i++) {
		void *base = (void *)g_ctl->guarded[i].base;
		if (VirtualQuery(base, &mbi, sizeof(mbi)) != sizeof(mbi) ||
		    mbi.State != MEM_RESERVE)
			continue; /* committed since, so it is in use now */
		if (VirtualFree(base, 0, MEM_RELEASE)) {
			bytes += g_ctl->guarded[i].size;
			freed++;
		}
	}
	if (freed)
		ss_log("  guard: released %d stale reservation(s), %.1f MB\n", freed,
		       (double)bytes / (1024.0 * 1024.0));
	g_ctl->nguarded = 0;
	g_ctl->guard_bytes = 0;
}

/* Keeps the addresses a live snapshot depends on out of circulation.
 *
 * Once the game frees a region that the snapshot needs, the address is fair
 * game for any allocator in the process, and in an address space that is
 * two-thirds full and fragmented it gets taken almost immediately. That is
 * where err=487 came from, and it is how a restore ends up writing old contents
 * over a live object that merely happens to fit.
 *
 * The airtight version hooks the free path, but the frees that matter happen
 * inside ntdll's heap rather than through any import we could redirect, and
 * patching ntdll means contesting bytes the Steam overlay also wants. Claiming
 * the addresses back a few times a second is most of the benefit for none of
 * that risk: the window in which something else could grab one is a handful of
 * frames wide.
 *
 * Off by default, because the address space it takes is not free. The ranges it
 * claims are ones the game has finished with and will not ask for again, so the
 * reservations stack on top of live memory rather than replacing it. With
 * roughly 700 MB of headroom that runs out fast, so what it holds is capped and
 * handed back whenever a new save supersedes the old region list. */
#define SS_GUARD_CAP (96ull * 1024ull * 1024ull)
void savestate_guard(void)
{
	static unsigned tick;
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t addr = 0, gran;
	SYSTEM_INFO si;
	Slot *s;
	int i, claimed = 0;
	unsigned long long bytes = 0;

	if (!g_ctl)
		return;
	g_frames_since_load++;
	if (g_ctl->busy || (tick++ & 3))
		return;
	/* Ahead of the guard's own knob, because this has nothing to do with
	 * guarding addresses and must happen whether or not that is enabled. */
	fntab_keep_hooked();
	cs_keep_hooked();
	/* Same reason as the other two are repeated: Mono is not necessarily loaded
	 * yet, and when it is, it may not have cached the pointer yet. Idempotent. */
	fmt_hook_install();
	if (g_ctl->guard == 0)
		return;
	if (g_ctl->guard < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_REWIND_GUARD", v, sizeof(v));
		g_ctl->guard = (n > 0 && n < sizeof(v) && v[0] == '1') ? 1 : 0;
		if (!g_ctl->guard)
			return;
	}
	s = &g_ctl->slots[0];
	if (!s->valid || g_ctl->guard_bytes >= SS_GUARD_CAP)
		return;

	GetSystemInfo(&si);
	gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 65536;

	while (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
		uintptr_t fb = (uintptr_t)mbi.BaseAddress, fe = fb + mbi.RegionSize;
		if (fe <= addr)
			break;
		addr = fe;
		if (mbi.State != MEM_FREE)
			continue;
		for (i = 0; i < s->nregs; i++) {
			uintptr_t lo = s->regs[i].base & ~(gran - 1);
			uintptr_t hi = (s->regs[i].base + s->regs[i].size + gran - 1) &
				       ~(gran - 1);
			if (lo < fb)
				lo = fb;
			if (hi > fe)
				hi = fe;
			if (hi <= lo)
				continue;
			if (g_ctl->nguarded >= 1024 ||
			    g_ctl->guard_bytes + (hi - lo) > SS_GUARD_CAP)
				return;
			if (VirtualAlloc((LPVOID)lo, (SIZE_T)(hi - lo), MEM_RESERVE,
					 PAGE_NOACCESS)) {
				g_ctl->guarded[g_ctl->nguarded].base = lo;
				g_ctl->guarded[g_ctl->nguarded].size = hi - lo;
				g_ctl->nguarded++;
				g_ctl->guard_bytes += hi - lo;
				claimed++;
				bytes += hi - lo;
				/* The map changed; the enclosing walk would now be
				 * describing memory that no longer looks like this. */
				addr = fb;
				break;
			}
		}
	}

	if (claimed)
		ss_log("guard: reclaimed %d range(s), %.1f MB of addresses the save needs\n",
		       claimed, (double)bytes / (1024.0 * 1024.0));
}

int savestate_slot_valid(int slot)
{
	if (slot < 0 || slot >= SAVESTATE_SLOTS || !g_ctl)
		return 0;
	return g_ctl->slots[slot].valid;
}

double savestate_last_ms(void)
{
	return g_ctl ? g_ctl->last_ms : 0.0;
}

double savestate_last_mb(void)
{
	return g_ctl ? g_ctl->last_mb : 0.0;
}

/* Whether the operation that actually completed was a restore.
 *
 * Callers cannot infer this from which function they called: a restored thread
 * returns through the save it was taking. See request(). */
int savestate_last_was_restore(void)
{
	return g_ctl ? (g_ctl->done_req == REQ_LOAD) : 0;
}

/* Wall time since the session's first restore, or negative if there has not
 * been one. Real time rather than the game's, which the rewind may have moved. */
double savestate_live_ms(void)
{
	if (!g_ctl || !g_ctl->anchor_tick)
		return -1.0;
	return (double)(GetTickCount() - g_ctl->anchor_tick);
}

void savestate_exclude(void *p, size_t bytes)
{
	if (!p || !bytes)
		return;
	/* The control block carries the exclusion list, so it has to exist before
	 * anything can be added to it. */
	if (!ensure_helper())
		return;
	ss_exclude(p, bytes);
	/* Latched as fixed, because build_exclusions resets the list to nex_fixed on
	 * every save. Added without this, a range would be held for one snapshot and
	 * quietly rewound by the next - which is worse than not holding it, since the
	 * first restore would appear to prove it worked. */
	g_ctl->nex_fixed = g_ctl->nex;
}
