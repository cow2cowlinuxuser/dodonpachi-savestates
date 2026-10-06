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

#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <tlhelp32.h>
#include <windows.h>
#include "logdir.h"

void phase_snapshot(const char *phase);

#define SS_MAX_REGIONS 65536
#define SS_MAX_THREADS 256
#define SS_MAX_EXCL 1024
/* One entry per heap AND one per heap segment, so this is not bounded by how
 * many heaps exist but by how far they have grown. At 64 the table filled on
 * this game - a single heap of 26 MB carries 79 segments - and the segments
 * past the cap were never registered. An unregistered segment answers to no
 * heap, so heap_rewound_at said no, so the region skipped the by-block path and
 * was restored wholesale: the allocator's own headers and free lists written
 * back from a snapshot, which is the one write we know kills the process. The
 * cap did not report itself, so this looked like block mode working. */
#define SS_MAX_HEAPS 512
#define SS_MAX_MODS 256
#define SS_MAX_SEGS 1024
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
#define TIB_STACK_BASE 0x04  /* NT_TIB.StackBase, high end */
#define TIB_STACK_LIMIT 0x08 /* NT_TIB.StackLimit, low committed end */
#else
#define TEB_TLS_SLOTS 0x1480
#define TIB_STACK_BASE 0x08
#define TIB_STACK_LIMIT 0x10
#endif
#define TLS_MINIMUM_SLOTS 64

typedef struct ThreadState {
	DWORD tid;
	/* What a cross-session transplant needs beyond the stack range: the head of
	 * the x86 SEH chain (it lives in the TEB, which is never captured), a mark
	 * saying these were recorded, and the build of this module that took the
	 * save. The saved stack returns into our own code, so a different build
	 * cannot be resumed through. Zero in slots older than the transplant. */
	DWORD seh_head;
	DWORD xs_mark;
	DWORD build_id;
	void *tls[TLS_MINIMUM_SLOTS];
	int have_tls;
	/* The thread's stack range at save (TEB StackBase/StackLimit). A saved SP and
	 * any pointer into this stack is stack_base-relative; cross-session the new
	 * thread's stack lands elsewhere, so the relocation pass shifts them by
	 * (new_stack_base - stack_base) when it adopts this thread. Zero if the TEB
	 * could not be read. */
	uintptr_t stack_base, stack_limit;
	/* Where the TEB was. A thread parked in ntdll keeps its own TEB's address
	 * in a register, and another process put that TEB somewhere else. */
	uintptr_t teb;
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
	/* Volume-relative, as file_name_of returns it, so a load in another
	 * process can open the same file again under the saved handle value. */
	char name[240];
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
static HANDLE g_logh SS_PRESENT;

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
		if (!file_name_of(h, out[found].name, sizeof(out[found].name)))
			out[found].name[0] = 0;
		found++;
	}
	return found;
}

/* Putting a file handle back under its saved value, for a load from another
 * process. The game's memory holds the handle as a number, and numbers are not
 * relocated: in the new process that number is another object or nothing, and a
 * stream reading through it feeds its decoder whatever that is. So the file is
 * opened again and the new handle is made to take the old value, which the
 * handle table allows because it hands a freed entry out again first.
 *
 * NtCreateFile on a path built here, because CreateFileW converts the name
 * through the process heap and this runs with every other thread suspended. */
typedef struct {
	USHORT Length, MaximumLength;
	PWSTR Buffer;
} SS_USTR;
typedef struct {
	ULONG Length;
	HANDLE RootDirectory;
	SS_USTR *ObjectName;
	ULONG Attributes;
	PVOID SecurityDescriptor, SecurityQualityOfService;
} SS_OBJATTR;
typedef LONG(NTAPI *PFN_NtCreateFile)(HANDLE *, ACCESS_MASK, SS_OBJATTR *, SS_IOSB *,
				      LARGE_INTEGER *, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);

static PFN_NtCreateFile create_file(void)
{
	static PFN_NtCreateFile fn;

	if (!fn) {
		HMODULE nt = GetModuleHandleA("ntdll.dll");
		if (nt)
			fn = (PFN_NtCreateFile)(void *)GetProcAddress(nt, "NtCreateFile");
	}
	return fn;
}

static uintptr_t peb_base(void);

/* The executable's path from the process parameters, read in place. */
static const WCHAR *exe_path(USHORT *chars)
{
	uintptr_t peb = peb_base(), pp;
	const SS_USTR *u;

	*chars = 0;
	if (!peb)
		return NULL;
#if defined(_M_IX86) || defined(__i386__)
	pp = *(const uintptr_t *)(peb + 0x10);
	u = pp ? (const SS_USTR *)(pp + 0x38) : NULL;
#else
	pp = *(const uintptr_t *)(peb + 0x20);
	u = pp ? (const SS_USTR *)(pp + 0x60) : NULL;
#endif
	if (!u || !u->Buffer || u->Length < 3 * sizeof(WCHAR))
		return NULL;
	*chars = (USHORT)(u->Length / sizeof(WCHAR));
	return u->Buffer;
}

/* Whether a volume-relative name lies in the game's own folder, which makes a
 * live handle to it the game's, and one the restore has just orphaned. */
static int in_game_dir(const char *name)
{
	USHORT n, i, dir = 0;
	const WCHAR *p = exe_path(&n);

	if (!p || p[1] != L':')
		return 0;
	for (i = 2; i < n; i++)
		if (p[i] == L'\\')
			dir = i;
	if (dir <= 2)
		return 0;
	for (i = 2; i <= dir; i++) {
		WCHAR a = p[i], b = (WCHAR)(unsigned char)name[i - 2];

		if (!b)
			return 0;
		if (a >= L'A' && a <= L'Z')
			a += 32;
		if (b >= L'A' && b <= L'Z')
			b += 32;
		if (a != b)
			return 0;
	}
	return 1;
}

static HANDLE file_open_ro(const char *name)
{
	PFN_NtCreateFile fn = create_file();
	WCHAR path[260 + 8];
	USHORT n, i, k = 0;
	const WCHAR *exe = exe_path(&n);
	SS_USTR us;
	SS_OBJATTR oa;
	SS_IOSB iosb;
	HANDLE h = NULL;

	if (!fn || !exe || exe[1] != L':' || !name[0])
		return NULL;
	path[k++] = L'\\';
	path[k++] = L'?';
	path[k++] = L'?';
	path[k++] = L'\\';
	path[k++] = exe[0];
	path[k++] = L':';
	for (i = 0; name[i] && k < 260 + 6; i++)
		path[k++] = (WCHAR)(unsigned char)name[i];
	if (name[i])
		return NULL;
	us.Length = us.MaximumLength = (USHORT)(k * sizeof(WCHAR));
	us.Buffer = path;
	memset(&oa, 0, sizeof(oa));
	oa.Length = sizeof(oa);
	oa.ObjectName = &us;
	oa.Attributes = 0x40; /* OBJ_CASE_INSENSITIVE */
	memset(&iosb, 0, sizeof(iosb));
	/* GENERIC_READ | SYNCHRONIZE, share everything, FILE_OPEN,
	 * FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE. */
	if (fn(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, 0,
	       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 1, 0x20 | 0x40, NULL,
	       0) < 0)
		return NULL;
	return h;
}

/* 1 when `want` now names the same object as `src`. The table hands out free
 * values in its own order, so duplicates are taken until one lands on `want`
 * and the misses are closed again. `src` is left open either way. */
static int handle_pin(HANDLE src, HANDLE want)
{
	static HANDLE spare[1024];
	int n = 0, got = 0, i;

	if (src == want)
		return 1;
	while (n < (int)(sizeof(spare) / sizeof(spare[0]))) {
		HANDLE d;

		if (!DuplicateHandle(GetCurrentProcess(), src, GetCurrentProcess(), &d, 0, FALSE,
				     DUPLICATE_SAME_ACCESS))
			break;
		if (d == want) {
			got = 1;
			break;
		}
		spare[n++] = d;
	}
	for (i = 0; i < n; i++)
		CloseHandle(spare[i]);
	return got;
}

/* 1 when f->h names f's file again, 0 with *why set when it could not. */
static int file_reinstate(const FileState *f, const char **why)
{
	HANDLE nh, want = f->h;
	DWORD flags;
	int got;

	if (!f->name[0]) {
		*why = "the save has no name for it";
		return 0;
	}
	if (GetHandleInformation(want, &flags)) {
		char live[MAX_PATH];

		if (GetFileType(want) != FILE_TYPE_DISK ||
		    !file_name_of(want, live, sizeof(live)) || !in_game_dir(live) ||
		    ours_by_name(want)) {
			*why = "its value is taken by something this process still uses";
			return 0;
		}
		CloseHandle(want);
	}
	nh = file_open_ro(f->name);
	if (!nh) {
		*why = "the file would not open";
		return 0;
	}
	if (nh == want)
		return 1;
	got = handle_pin(nh, want);
	CloseHandle(nh);
	if (!got)
		*why = "the handle table would not hand out that value";
	return got;
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

/* -------------------------------------------------- the window's focus, kept
 *
 * A crash report that says where the fault was but not what the process was
 * doing leaves the last step to be reconstructed from a second log file
 * afterwards, by hand, comparing file timestamps. That is how the Steam fault
 * was pinned down: the render log's last line was "window is now in the
 * background" and the savestate log was written 354 ms later, which turned an
 * intermittent crash into a sequence - restore, play four seconds, alt-tab,
 * die. Steam's threads sit on memory the restore rewound underneath them and
 * nothing makes them read it until a focus change wakes the overlay.
 *
 * Worth recording rather than inferring, because it is the difference between
 * a crash that looks random and one with a cause. A ring of the last few
 * transitions costs two words each and prints inside the fault path, which
 * must not allocate, take a lock or call the C runtime - so it is a fixed
 * array, a counter, and nothing else.
 */
#define SS_FOCUS_RING 8
static struct {
	DWORD tick;
	int state; /* 0 foreground, 1 background, 2 minimised */
} g_focus[SS_FOCUS_RING];
static volatile LONG g_focus_n;

void savestate_note_focus(int state)
{
	LONG i = InterlockedIncrement(&g_focus_n) - 1;

	g_focus[(unsigned)i & (SS_FOCUS_RING - 1)].tick = GetTickCount();
	g_focus[(unsigned)i & (SS_FOCUS_RING - 1)].state = state;
}

static void focus_report(void)
{
	DWORD now = GetTickCount();
	LONG n = g_focus_n, i;

	if (n <= 0)
		return;
	if (n > SS_FOCUS_RING)
		n = SS_FOCUS_RING;
	for (i = n - 1; i >= 0; i--) {
		LONG k = g_focus_n - 1 - (n - 1 - i);
		const char *w;

		if (k < 0)
			continue;
		switch (g_focus[(unsigned)k & (SS_FOCUS_RING - 1)].state) {
		case 2:
			w = "MINIMISED";
			break;
		case 1:
			w = "went to the BACKGROUND";
			break;
		default:
			w = "came to the foreground";
			break;
		}
		ss_raw("       focus: %s, %u ms before the fault\n", w,
		       (unsigned long)(now - g_focus[(unsigned)k & (SS_FOCUS_RING - 1)].tick));
	}
}
static void fmt_report_for(DWORD tid);
static const char *ss_module(uintptr_t v, unsigned *off);
static int region_excluded(uintptr_t base, uintptr_t size);
static void audit_untracked_regions(void);
/* Not architecture specific - it was only ever declared under the 64-bit guard
 * because the 64-bit fault path was written first. */
static void ss_where_reg(const char *name, uintptr_t v);

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
static void cfg_fault_note(void);
static int xs_spare_idx(DWORD tid);

/* Declared up here because the heap partitioning has to know whether block
 * restore is on to warn about a combination that silently does nothing. */
static int blk_mode(void);

/* Says what the last restore did with the block containing an address, so a
 * fault report can answer "did we write this, and who told us we could" without
 * a debugger. Defined next to the block machinery a long way below. */
static void blk_provenance(uintptr_t at);

/* Guard pages armed on the addresses a restore failed to hold.
 *
 * The clobber check says which words go back to their pre-restore values, but
 * says nothing about who put them there, and the candidates - a system thread
 * we do not rewind, our own wrapper, the game recomputing from the clock - all
 * look identical from the outside. A guard page settles it: the next thread to
 * touch the page traps, and the handler prints its instruction pointer and the
 * module it belongs to.
 *
 * One shot per page, because the kernel clears PAGE_GUARD when it delivers the
 * trap, so this costs a single fault per armed page and nothing thereafter.
 * Declared up here because the handler that reports the hits is defined a long
 * way above the restore code that arms them. */
#ifndef STATUS_GUARD_PAGE_VIOLATION
#define STATUS_GUARD_PAGE_VIOLATION ((DWORD)0x80000001L)
#endif
#define SS_CATCH_CAP 64
static uintptr_t g_catch_at[SS_CATCH_CAP];
static volatile LONG g_catch_n, g_catch_hits;

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

/* Defined with the heap checks, called from hooks install so the record exists
 * before the first snapshot can capture a null pointer to it. */
static void heap_seen_init(void);

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

#if defined(_M_IX86) || defined(__i386__)
static void ss_where_reg(const char *name, uintptr_t v);

/* Walk the saved-EBP chain, and read the callee-saved registers out of it.
 *
 * The stack scan below finds code addresses and calls them leads, which is
 * honest and nearly useless: it cannot tell a live frame from one that returned
 * an hour ago. On x86 with frame pointers there is a real chain sitting right
 * there - [ebp] is the caller's ebp and [ebp+4] is the return address - and it
 * costs two reads per frame to follow.
 *
 * The words just below each ebp matter more than the chain itself. A function
 * that sets up its frame and then pushes esi, edi and ebx leaves them at ebp-4,
 * ebp-8 and so on, which means a caller's object pointer is recoverable from a
 * crash three frames deeper. That is not hypothetical: rabiribi.exe+6E9F8 died
 * inside a memcpy whose destination came from a field of an object held in its
 * grandparent's esi, and the only reason we could not say which object was that
 * nothing was reading those slots. Each one goes through the same describer the
 * registers do, so the log says outright whether the value points at memory the
 * save restored, memory held in the present, or neither.
 *
 * Anything found this way is still a guess about which slot held which register
 * - that depends on the push order of a function we have not disassembled - so
 * the values are labelled by slot and left for a human to match up. */
static void ss_frames(const CONTEXT *c)
{
	uintptr_t fp = (uintptr_t)c->Ebp;
	int n;

	for (n = 0; n < 12 && fp; n++) {
		uintptr_t next, ret;
		const char *in;
		unsigned off = 0;
		int k;

		if (!ss_readable(fp, 2 * sizeof(uintptr_t)))
			break;
		next = ((const uintptr_t *)fp)[0];
		ret = ((const uintptr_t *)fp)[1];
		if (!ss_is_code(ret))
			break;
		in = ss_module(ret, &off);
		ss_raw("       frame %d: ebp %08lX, returns to %08lX in %s+%X\n", n,
		       (unsigned long)fp, (unsigned long)ret, in ? in : "unknown", off);
		for (k = 1; k <= 4; k++) {
			uintptr_t slot = fp - (uintptr_t)k * sizeof(uintptr_t);
			char label[32];

			if (!ss_readable(slot, sizeof(uintptr_t)))
				break;
			wsprintfA(label, "         saved [ebp-%X]", (unsigned)(k * 4));
			ss_where_reg(label, *(const uintptr_t *)slot);
		}
		/* Frames grow downwards, so the next ebp must be above this one, and
		 * a jump of more than a stack's worth means we are following data
		 * that happens to look like a chain. */
		if (next <= fp || next - fp > 0x10000)
			break;
		fp = next;
	}
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
	/* Before the scan, because it is the part that can be trusted. */
	ss_frames(c);
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

/* What a tracked handle names, so a load in another process can make the same
 * value name an equivalent object again. 0 is an event of unknown reset type. */
enum { HK_EVENT = 0, HK_EVENT_MANUAL, HK_EVENT_AUTO, HK_THREAD, HK_MUTEX };

typedef struct EventState {
	HANDLE h;
	int signalled;
	int kind;
	DWORD tid;
	uintptr_t site; /* the game's call to CreateEvent, for pairing by role */
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
	unsigned char kind[SS_MAX_EVENTS];
	DWORD tid[SS_MAX_EVENTS];
	uintptr_t site[SS_MAX_EVENTS];
	volatile LONG nfault;
	FaultRec fault[32];
	LONG npark;
	uintptr_t park[32];
	volatile LONG ndbg;
	char dbg[8][160];
} EventTrack;

static EventTrack *g_events SS_PRESENT;

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

static EventTrack *g_events SS_PRESENT;
static HANDLE(WINAPI *g_real_createevent)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR);
static int g_hooked_time, g_hooked_event;

static HANDLE(WINAPI *g_real_createeventw)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR);

static void event_note_at(HANDLE h, int kind, DWORD tid, uintptr_t site)
{
	if (h && g_events) {
		LONG i = InterlockedIncrement(&g_events->n) - 1;
		if (i >= 0 && i < SS_MAX_EVENTS) {
			g_events->kind[i] = (unsigned char)kind;
			g_events->tid[i] = tid;
			g_events->site[i] = site;
			g_events->h[i] = h;
		}
	}
}

static void event_note(HANDLE h, int kind, DWORD tid)
{
	event_note_at(h, kind, tid, 0);
}

static HANDLE WINAPI hook_createevent(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL init,
				      LPCSTR name)
{
	HANDLE h = g_real_createevent(sa, manual, init, name);

	event_note_at(h, manual ? HK_EVENT_MANUAL : HK_EVENT_AUTO, 0,
		      (uintptr_t)__builtin_return_address(0));
	return h;
}

static HANDLE WINAPI hook_createeventw(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL init,
				       LPCWSTR name)
{
	HANDLE h = g_real_createeventw(sa, manual, init, name);

	event_note_at(h, manual ? HK_EVENT_MANUAL : HK_EVENT_AUTO, 0,
		      (uintptr_t)__builtin_return_address(0));
	return h;
}

/* haydee.dll's HDMutex, which game.dll uses too: a global one guards every
 * component's reference count, so a room change waits on it, and a value from
 * another launch names nothing that will ever be released. */
static HANDLE WINAPI hook_createmutex(LPSECURITY_ATTRIBUTES sa, BOOL owned, LPCSTR name)
{
	HANDLE h = CreateMutexA(sa, owned, name);

	event_note_at(h, HK_MUTEX, 0, (uintptr_t)__builtin_return_address(0));
	return h;
}

/* The handle a game module keeps for its own thread - PhysX joins its workers
 * through it at exit, so in a restore from another process the saved value has
 * to name the live thread that took the saved one's place. */
static HANDLE(WINAPI *g_real_createthread)(LPSECURITY_ATTRIBUTES, SIZE_T,
					   LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);

static HANDLE WINAPI hook_createthread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack,
				       LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags,
				       LPDWORD id)
{
	DWORD tid = 0;
	HANDLE h = g_real_createthread(sa, stack, fn, arg, flags, &tid);

	if (id)
		*id = tid;
	event_note(h, HK_THREAD, tid);
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
		int j;

		if (!h || !GetHandleInformation(h, &flags))
			continue; /* closed since it was created */
		for (j = i + 1; j < cnt && g_events->h[j] != h; j++)
			;
		if (j < cnt)
			continue; /* the value was closed and handed out again */
		out[n].h = h;
		out[n].kind = g_events->kind[i];
		out[n].tid = g_events->tid[i];
		out[n].site = g_events->site[i];
		/* A zero wait on a mutex would take it. */
		out[n].signalled = out[n].kind == HK_THREAD || out[n].kind == HK_MUTEX
					   ? 0
					   : event_probe(h);
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

/* The same redirect, for the allocator hook in gameheap.c. It lives there rather
 * than here because it is a policy about where the game's memory goes, not part
 * of the rewind - but the patcher is here and there is no sense in two. */
int savestate_patch_iat(HMODULE mod, void *from, void *to)
{
	return patch_iat(mod, from, to);
}

/* The same redirect, found by name rather than by address.
 *
 * Matching on the address GetProcAddress returns works for an ordinary export
 * and fails for a forwarder. kernel32!HeapFree is one: its export entry is the
 * string "NTDLL.RtlFreeHeap", so GetProcAddress resolves through to ntdll and
 * hands back an address that need not be the one the loader wrote into this
 * executable's import slot. Our HeapFree patch found nothing for exactly that
 * reason, and reported an honest zero.
 *
 * The name is not ambiguous the way the address is, so walk the names. The
 * original thunk array keeps them after binding, which is what it is for; an
 * import bound by ordinal has no name and is skipped, and says so by matching
 * nothing rather than by matching the wrong thing.
 *
 * The address that was there is handed back so the caller has something to
 * forward to, which is the one thing the address-matching version got for free.
 */
int savestate_patch_iat_named(HMODULE mod, const char *dll, const char *fn, void *to,
			      void **prev)
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
		const char *name = (const char *)(base + imp->Name);
		IMAGE_THUNK_DATA *orig, *cur;

		if (dll && lstrcmpiA(name, dll))
			continue;
		if (!imp->OriginalFirstThunk)
			continue;
		orig = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
		cur = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
		for (; orig->u1.AddressOfData; orig++, cur++) {
			IMAGE_IMPORT_BY_NAME *by;
			DWORD old;

			if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG)
				continue;
			by = (IMAGE_IMPORT_BY_NAME *)(base + orig->u1.AddressOfData);
			if (lstrcmpA((const char *)by->Name, fn))
				continue;
			if (prev)
				*prev = (void *)cur->u1.Function;
			if (!VirtualProtect(cur, sizeof(void *), PAGE_READWRITE, &old))
				continue;
			cur->u1.Function = (ULONG_PTR)to;
			VirtualProtect(cur, sizeof(void *), old, &old);
			n++;
		}
	}
	return n;
}

/* A heap the game's allocations were redirected into, when they were.
 *
 * Set before the first save. It makes game_crt_heap answer with a heap that has
 * no other tenant, which is the whole point of the redirect: the classifier then
 * takes the same branch it takes for a game that linked MSVCR100, and the heap
 * is rewound whole instead of negotiated block by block. */
static HANDLE g_redirect_heap SS_PRESENT;

void savestate_game_heap(HANDLE h)
{
	g_redirect_heap = h;
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

static FnTabs *g_fntabs SS_PRESENT;
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
	/* When the section was (last) initialised, in g_cs->seq. A section born
	 * after the save the restore is going back to does not exist in that
	 * snapshot: its memory comes back holding whatever was there at the save -
	 * another object, or a heap's own bookkeeping - and "repairing" it as a
	 * lock writes a free section over that. Haydee's first room-to-room
	 * restore did it to 19 of them, and the game heap failed validation. */
	LONG born;
	/* When DeleteCriticalSection retired it, in the same sequence. Deleted
	 * after the save but born before it, it is a live lock again once the
	 * restore puts its memory back. */
	LONG died;
	/* The born of the section that last died at this address, so a restore
	 * that predates the newer one can tell the older one is back. */
	LONG prev_born;
	char live;
} CsRec;

typedef struct CsRecs {
	volatile LONG n;
	volatile LONG lock;
	volatile LONG seq;
	LONG unborn, revived, debug_cut, sem_cut;
	volatile LONG overflow, skipped;
	LONG held, reset, kept, unreadable, outside;
	CsRec t[SS_MAX_CS];
} CsRecs;

static CsRecs *g_cs SS_PRESENT;
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
			if (!e->live && e->born)
				e->prev_born = e->born;
			e->born = ++g_cs->seq;
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
		if (e) {
			e->live = 0;
			e->died = ++g_cs->seq;
		}
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

/* Hold the process still at the fault instead of letting it die.
 *
 * A crash currently costs a whole session and yields a minidump plus eight
 * lines of stack-scan guesses, because there are no symbols here and no unwind
 * tables worth trusting through a frame-pointer-free system DLL. Freezing turns
 * that into a live process stopped at the instruction, which a real debugger
 * can attach to and walk properly - true frames, the actual heap, memory that
 * can be read at leisure and re-read after a hypothesis changes.
 *
 * This is deliberately not the same as surviving the fault. Continuing past a
 * divide by zero inside the allocator leaves an allocator whose state is
 * already wrong handing out pointers to a game that keeps running, which is how
 * the heal guard turned one clean crash into an afternoon spent chasing module
 * divergence that was never there. Nothing continues here. The state below the
 * banner is the state at the instant of the fault, and it stays that way until
 * someone kills the process. */
static volatile LONG g_frozen;

/* Set while this thread is inside our own HeapWalk, so the handler can tell a
 * fault we caused from one the game suffered and unwind out of ntdll rather
 * than reporting a crash and freezing on it. */
static jmp_buf g_hw_jmp;
static volatile LONG g_hw_armed, g_hw_faults;
static DWORD g_hw_tid;

static int freeze_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[16];
		DWORD n = ss_getenv("D3D9SW_FREEZE", v, sizeof(v));

		if (!n || n >= sizeof(v))
			return 0; /* never remember a failed read */
		cached = atoi(v);
		if (cached < 0)
			cached = 0;
	}
	return cached;
}

static void ss_freeze_here(const char *why, uintptr_t pc, unsigned extra)
{
	DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
	int stopped = 0;
	HANDLE snap;

	if (!freeze_mode())
		return;
	/* A second thread faulting while the first holds the world just parks. It
	 * must not run on, and it must not race the banner. */
	if (InterlockedCompareExchange(&g_frozen, 1, 0) != 0)
		for (;;)
			Sleep(1000);

	snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap != INVALID_HANDLE_VALUE) {
		THREADENTRY32 te;

		te.dwSize = sizeof(te);
		if (Thread32First(snap, &te))
			do {
				HANDLE h;

				if (te.th32OwnerProcessID != pid || te.th32ThreadID == me)
					continue;
				h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
				if (!h)
					continue;
				if (SuspendThread(h) != (DWORD)-1)
					stopped++;
				CloseHandle(h);
			} while (Thread32Next(snap, &te));
		CloseHandle(snap);
	}
	ss_raw("\n"
	       "================================================================\n"
	       " FROZEN: %s at %p on thread %u", why, (void *)pc, (unsigned)me);
	if (extra)
		ss_raw(", code %08X", extra);
	ss_raw("\n"
	       " %d other thread(s) suspended. The process is alive and holding\n"
	       " still at the instant of the fault - nothing has continued, so\n"
	       " nothing has corrupted further.\n"
	       "\n"
	       "     cdb -p %u\n"
	       "\n"
	       " Kill the process when finished; it will not resume on its own.\n"
	       "================================================================\n",
	       stopped, (unsigned)pid);
	for (;;)
		Sleep(1000);
}

/* Fastfail cannot be caught, so stop it being raised.
 *
 * 0xC0000409 goes straight to process termination without consulting a vectored
 * handler, an SEH frame or the unhandled-exception filter, by design: it is how
 * Windows refuses to let a process whose invariants are broken talk its way
 * out. It is also the exact signal we most want, because the heap raises it the
 * moment it finds its own metadata inconsistent - which is the failure we have
 * been chasing all day and the one class we currently learn nothing from.
 *
 * It cannot be intercepted after the fact, so intercept it before. On x86 the
 * intrinsic compiles to "mov ecx, <code>" followed by "int 29h", and the two
 * bytes CD 29 can be replaced with CC 90 - a breakpoint and a nop, same length.
 * A breakpoint IS dispatched to a vectored handler, so the failure that would
 * have killed us silently arrives as a catchable exception with the reason
 * still sitting in ecx.
 *
 * The signature demands the preceding mov, because scanning executable bytes
 * for CD 29 without a disassembler will find it inside the middle of unrelated
 * instructions, and patching there would corrupt ntdll rather than instrument
 * it. Requiring B9 at -5 costs the rare non-constant call site and buys
 * certainty about every site it does take. */
/* Implemented in dsoundhook.c: the play cursor has to travel with the state. */
void dsh_install(void);
void dsh_save(void);
void dsh_restore(void);
void dsh_seek(void);
void dsh_quiet(void);
void dsh_mark_present(void);
void dsh_survey(void);
void ds_sw_report(void);
void xa2_sw_report(void);
void gameheap_report(void);
void gameheap_va_dump(unsigned n);
void gameheap_va_mark_restore(void);
LONG gameheap_res_seq(void);
void gameheap_res_restored(int slotno, LONG seq_at_save);
void gameheap_res_saved(void);
void gameheap_al_restored(void);
void gameheap_physx_quiet(const char *what, int slot);
void gameheap_physx_restored(void);
int gameheap_streams(uintptr_t *f, WCHAR (*path)[MAX_PATH], char (*mode)[8], long long *pos,
		     int max);
void *gameheap_stream_reopen(uintptr_t old, const WCHAR *path, const char *mode, long long pos);

/* Size and last write time of a file by path, 0 if it cannot be asked. */
static void ss_file_stamp(const WCHAR *path, unsigned long long *size, unsigned long long *mtime)
{
	WIN32_FILE_ATTRIBUTE_DATA a;

	*size = *mtime = 0;
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a))
		return;
	*size = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
	*mtime = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) |
		 a.ftLastWriteTime.dwLowDateTime;
}
static void fault_saved_note(uintptr_t at);
int gameheap_saved_block(const void *saved, void *live_head, size_t remain, size_t *n);
unsigned gameheap_busy_snapshot(void);
unsigned gameheap_busy_saved_count(void);
int gameheap_busy_saved_at(unsigned i, void **head, size_t *total);
void gameheap_busy_rewind(void);
/* 1 when `base` is a heap gameheap owns and has made self-contained (LFH off) for
 * D3D9SW_WHOLESALE, so its whole region may be restored byte-for-byte - freelist
 * and all - instead of block by block, which is the address-reuse cure. 0 unless
 * the knob is set. Defined in gameheap.c. */
int gameheap_owned_heap(uintptr_t base);
int gameheap_slot_heap(uintptr_t h);
int gameheap_import_mode(void);
HANDLE gameheap_rt_heap(void);
/* Owned-heap base anchors (arena, then LAA region) for relocatable cross-session
 * restore: writes up to `max` {base,size} pairs, returns the count. Defined in
 * gameheap.c. The exe and DLLs are anchored separately by the module table. */
int gameheap_anchor_ranges(uintptr_t *base, uintptr_t *size, int max);
int gameheap_pin_ranges(uintptr_t *base, uintptr_t *size, const char **what, int max);
int gameheap_va_origin_at(int i, uintptr_t *base, uintptr_t *size, const char **mod);
int gameheap_va_origin_count(void);
/* The renderer's own VirtualAlloc regions (ORIGIN d3d11.dll), recorded as they
 * are allocated and dropped as they are freed, for build_exclusions to hold in
 * the present. Empty unless D3D9SW_SWEXCLUDE armed the Nt hooks. */
int gameheap_own_va_ranges(uintptr_t *base, uintptr_t *size, int max);
void gameheap_heap_census(void);
/* Names the gameheap block an address belongs to from the tag table, which lives
 * in our image and survives a rewind - so a zeroed post-restore block can still
 * report the call site, size and ordinal it was born with. 1 = block start,
 * 2 = interior pointer, 0 = not ours. Defined in gameheap.c. */
int gameheap_whatis(const void *p, unsigned *site, unsigned *size, unsigned *ord,
		    uintptr_t *base);
void xa2_sw_pump(void);
void xa2_sw_park(void);
void xa2_sw_quiesce(void);
void xa2_sw_resume(void);
/* Bumps the audio restore-generation so voices that outlived this restore can be
 * told apart from ones the game builds afterwards; the generation valve refuses
 * callbacks for the former (the track-change crash). Defined in xa2_sw.c. */
void xa2_sw_restored(void);
void dsh_play(void);

/* x86 only, like the clamp that reads them: the only callers live under the same
 * 32-bit guard, and the x64 build compiles this file too - so without this the
 * counter and the knob are stranded as "unused" on every 64-bit target. */
#if defined(_M_IX86) || defined(__i386__)
static int g_runaway_hits;

/* On unless explicitly switched off: a negative copy length is a bug in every
 * program, so surviving it needs no per-game opt-in. */
static int runaway_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[16];
		DWORD n = ss_getenv("D3D9SW_RUNAWAY", v, sizeof(v));

		cached = (!n || n >= sizeof(v)) ? 1 : (atoi(v) ? 1 : 0);
	}
	return cached;
}
#endif

/* x86 only, like the clamp that calls it: the frame layout it walks is the
 * 32-bit calling convention's, and the x64 build compiles this file too.
 */
#if defined(_M_IX86) || defined(__i386__)
/* Name the code that asked for the copy, not the code performing it.
 *
 * rabiribi.exe+6E9F8 was read out of the running process and turned out to be
 * the C runtime's memcpy, linked in statically: three stack arguments, a test
 * of the count against zero, and a forward rep movs. Its neighbours settle it -
 * +6E980 is memcmp, returning 0 or 1 from a repe cmps, and +6EA00 is memmove,
 * the same copy plus the backwards std path for overlapping ranges. So every
 * memcpy in the program arrives at this one address, and reporting it says only
 * that a copy happened. The attribution to the Opus decoder was inferred from
 * this address and never actually supported by it.
 *
 * Recovering the caller is cheap here precisely because it is the CRT: the
 * function opens with push ebp / mov ebp,esp and ebp is still intact at the rep
 * movs, so the frame is exactly where the calling convention says.
 *
 * One wrinkle worth stating, because it looks like a bug otherwise. The count
 * slot at [ebp+10h] is reused: the prologue writes the source pointer over it
 * as a scratch spill. The original count survives at [ebp-8], and the two
 * pointer arguments are untouched at [ebp+8] and [ebp+0Ch]. Reading the count
 * from the argument slot would report a pointer as a length.
 *
 * Everything is probed before it is read. This runs inside a fault handler, and
 * a handler that faults tells us nothing at all. */
static void runaway_caller(const CONTEXT *c, uintptr_t pc)
{
	uintptr_t fp = (uintptr_t)c->Ebp;
	const char *names[3] = { "called from", "  which was called from",
				 "  which was called from" };
	uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
	int i;

	/* Only the copy inside the CRT's memcpy has the frame this reads.
	 *
	 * The first version applied the layout to whatever faulted, and at
	 * rabiribi+36913A - a rep movs written inline, in a function that does not
	 * keep ebp as a frame pointer - it duly reported memcpy(dst=00000015,
	 * src=0000001A, n=0). Those are not arguments, they are whatever happened
	 * to be at those offsets, and printing them as arguments is worse than
	 * printing nothing because they look like a finding.
	 *
	 * memcpy occupies +6E9D0 through +6E9FF, established by disassembling it.
	 * Anywhere else, fall back to the stack scan, which claims only that the
	 * addresses it prints are code and may be dead frames. */
	if (!base || pc < base + 0x6E9D0 || pc >= base + 0x6EA00) {
		ss_raw("  the copy is inline here rather than in the CRT's memcpy, so "
		       "there are no arguments to read. Scanning the stack instead:\n");
		ss_callers(c);
		return;
	}
	if (!ss_readable(fp, sizeof(uintptr_t) * 5)) {
		ss_raw("  (no frame at ebp=%08lX, so the caller cannot be named)\n",
		       (unsigned long)fp);
		return;
	}
	ss_raw("  memcpy(dst=%08lX, src=%08lX, n=%ld) - the arguments as the caller "
	       "passed them\n",
	       (unsigned long)*(const uintptr_t *)(fp + 8),
	       (unsigned long)*(const uintptr_t *)(fp + 12),
	       (long)*(const uintptr_t *)(fp - 8));

	/* Three frames out. The immediate caller is the one that computed the
	 * length; its caller is usually the one that owns the two pointers, which
	 * is the thing we actually want to identify. */
	for (i = 0; i < 3 && fp; i++) {
		uintptr_t ret, next;
		unsigned off = 0;
		const char *mod;

		if (!ss_readable(fp, sizeof(uintptr_t) * 2))
			break;
		next = *(const uintptr_t *)fp;
		ret = *(const uintptr_t *)(fp + 4);
		if (!ret)
			break;
		mod = ss_module(ret, &off);
		ss_raw("  %s %08lX (%s+%X)\n", names[i], (unsigned long)ret,
		       mod ? mod : "?", off);
		/* +6F533 is inside the buffered sample reader at +6F490, which keeps
		 * the stream object in esi (it reuses its own [ebp+8] as a counter),
		 * and memcpy's prologue pushes that esi to its [ebp-0Ch]. The refill
		 * (+6F280) takes the bytes left as [+34h] - (tell([+24h]) - [+30h])
		 * through the callback at [+0Ch]; a tell past the end is what drives
		 * [+454h] negative. */
		if (i == 0 && base && ret == base + 0x6F533 && g_runaway_hits <= 3 &&
		    ss_readable((uintptr_t)c->Ebp - 0xC, sizeof(uintptr_t))) {
			uintptr_t so = *(const uintptr_t *)((uintptr_t)c->Ebp - 0xC);
			const unsigned char *b = (const unsigned char *)so;

			if (ss_readable(so, 0x460)) {
				unsigned o1 = 0, o2 = 0;
				const char *m1 = ss_module(*(const uintptr_t *)(b + 0x0C), &o1);
				const char *m2 = ss_module(*(const uintptr_t *)(b + 0x14), &o2);

				ss_raw("  stream %08lX on thread %lu: type %lu, tell %08lX (%s+%X), "
				       "read %08lX (%s+%X), handle %08lX, start %08lX, size %08lX, "
				       "chunk %u, align %u, kind %u, buffer %08lX, used %ld, left %ld\n",
				       (unsigned long)so, (unsigned long)GetCurrentThreadId(),
				       (unsigned long)*(const DWORD *)(b + 0x08),
				       (unsigned long)*(const DWORD *)(b + 0x0C), m1 ? m1 : "?", o1,
				       (unsigned long)*(const DWORD *)(b + 0x14), m2 ? m2 : "?", o2,
				       (unsigned long)*(const DWORD *)(b + 0x24),
				       (unsigned long)*(const DWORD *)(b + 0x30),
				       (unsigned long)*(const DWORD *)(b + 0x34),
				       (unsigned)*(const DWORD *)(b + 0x4E),
				       (unsigned)*(const WORD *)(b + 0x62),
				       (unsigned)*(const WORD *)(b + 0x56),
				       (unsigned long)*(const DWORD *)(b + 0x44C),
				       (long)*(const DWORD *)(b + 0x458),
				       (long)*(const DWORD *)(b + 0x454));
				ss_raw("    the handle %s; the object is %s\n",
				       ss_readable(*(const DWORD *)(b + 0x24), 0x40)
					       ? "points at readable memory" : "is not a readable pointer",
				       savestate_addr_restored(b) == 1 ? "in memory the load wrote"
				       : savestate_addr_restored(b) == 0 ? "in saved memory the load LEFT in the present"
				       : "outside the save");
				if (ss_readable(*(const DWORD *)(b + 0x24), 0x40)) {
					const DWORD *h = (const DWORD *)(uintptr_t)*(const DWORD *)(b + 0x24);
					ss_raw("    handle words: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX "
					       "%08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX\n",
					       h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7],
					       h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15]);
				}
			}
		}
		if (next <= fp) /* frames grow one way; anything else is not a chain */
			break;
		fp = next;
	}
}
#endif

/* One byte, so that a decoder buffer with a negative count refills instead of
 * copying four gigabytes.
 *
 * Read out of the running process at rabiribi+6F490, which is the innermost of
 * three layers that move decoded audio out of a codec buffer. The object holds
 * its data at [esi+44Ch], how far it has been consumed at [esi+458h], and how
 * many bytes remain at [esi+454h]. Two instructions decide what happens when
 * that last field is wrong:
 *
 *     cmp   dword ptr [esi+454h],0
 *     jne   +6F50D                    ; nonzero, so skip the refill
 *     ...
 *     cmovg edi,eax                   ; signed min of requested and remaining
 *     call  memcpy
 *
 * The test asks whether the count is nonzero when it needed to ask whether it
 * is positive, and the min is signed. So a count of -3 skips the refill - it is
 * not zero, so the branch always takes - and then passes -3 to memcpy, where as
 * an unsigned length it is 4294967293. rep movs copied 92528 bytes over the
 * heap before it reached unmapped memory and faulted, which is why catching the
 * exception was never going to be enough: the damage is done by the time we are
 * called, and clamping ecx only spares the remainder.
 *
 * The two outer layers are not at fault and are left alone. +33DCE computes
 * remaining as total - position and clamps it with cmovle to zero; +36370 takes
 * an unsigned min with cmova. Only the innermost layer compares signed.
 *
 * jne becomes jg. A positive count still skips the refill, which is the normal
 * path and is untouched; a negative one now falls through to the decoder, which
 * resets the buffer. Nothing else changes, and the count is self-healing once a
 * copy is avoided - the sub that follows subtracts the same negative value and
 * brings it back to zero.
 *
 * Gated on the nine bytes being exactly what was disassembled. If the game is
 * ever patched and they are not, nothing is written. */
static int g_decpatch; /* 0 untried, 1 applied, -1 given up */
static unsigned g_dec_tries;
static int g_dec_said;

/* How many bytes of the image are mapped, from the headers rather than by
 * walking regions: a scan wants the whole image, and VirtualQuery would report
 * each section separately. */
static size_t pe_image_span(HMODULE m)
{
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)m;
	const IMAGE_NT_HEADERS *nt;

	if (!ss_readable((uintptr_t)dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return 0;
	nt = (const IMAGE_NT_HEADERS *)((const char *)m + dos->e_lfanew);
	if (!ss_readable((uintptr_t)nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
		return 0;
	return nt->OptionalHeader.SizeOfImage;
}

static void decoder_patch_once(void)
{
	static const unsigned char sig[] = { 0x83, 0xBE, 0x54, 0x04, 0x00,
					     0x00, 0x00, 0x75 };
	unsigned char *p;
	HMODULE exe;
	DWORD old;
	char v[16];
	DWORD n;

	if (g_decpatch)
		return;
	g_dec_tries++;
	/* Say something on the first call, unconditionally.
	 *
	 * Two builds in a row produced no decoder line at all, and because every
	 * bail-out was silent that was consistent with the check failing, the
	 * function never being reached, and the binary not being loaded - three
	 * very different problems that the log could not tell apart. One line that
	 * always prints costs nothing and removes the ambiguity. */
	if (!g_dec_said) {
		HMODULE m = GetModuleHandleA(NULL);
		const unsigned char *q = (const unsigned char *)m + 0x6F4C6;
		int ok = m && ss_readable((uintptr_t)q, 9);

		g_dec_said = 1;
		if (ok)
			ss_raw("decoder: exe at %p, +6F4C6 reads %02X %02X %02X %02X %02X "
			       "%02X %02X %02X %02X (expected 83 BE 54 04 00 00 00 75 3E)\n",
			       (void *)m, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7],
			       q[8]);
		else
			ss_raw("decoder: exe at %p, but +6F4C6 is not readable, so the "
			       "refill test cannot be examined there\n",
			       (void *)m);
	}
	n = ss_getenv("D3D9SW_DECPATCH", v, sizeof(v));
	if (n && n < sizeof(v) && !atoi(v)) {
		g_decpatch = -1;
		return;
	}
	exe = GetModuleHandleA(NULL);
	if (!exe) {
		g_decpatch = -1;
		return;
	}
	/* One game's instruction, so one game's executable.
	 *
	 * The hardcoded offset only ever holds for one build, so there is a
	 * fallback that scans the whole image for the same nine bytes and patches
	 * the single site it finds. Nothing above checked what image that was. The
	 * pattern is cmp dword [esi+0x454],0 followed by jne, which is ordinary
	 * enough that a 360 KB test harness contained exactly one - so the engine
	 * found it, called it the codec refill test, and turned a jne into a jg in
	 * a binary that has never decoded anything. It was caught because the
	 * harness crashed; in anything else it would be a rare wrong branch.
	 *
	 * Checked here rather than at the fallback because the fixed offset is a
	 * guess about a build, not a guarantee about a program, and it deserves the
	 * identity check just as much. */
	if (!savestate_host_is("rabiribi.exe")) {
		ss_raw("decoder: not patching - the refill test is a rabiribi.exe "
		       "instruction and this is not rabiribi.exe. The nine-byte "
		       "fallback would happily patch a lookalike in any image, which "
		       "is why the name is checked before the bytes\n");
		g_decpatch = -1;
		return;
	}
	/* The first attempt trusted rabiribi+6F4C6 alone and said nothing when it
	 * did not match, which is how a patch that never fired looked exactly like
	 * a patch that was never reached. So: try the offset the disassembly gave,
	 * fall back to searching the image for the same nine bytes, and report
	 * either way. A hardcoded offset is worth keeping as the fast path but is
	 * not worth trusting - it only holds for one build of the game. */
	p = (unsigned char *)exe + 0x6F4C6;
	/* Steam decrypts the image at launch, so early frames can still see the
	 * stub's ciphertext. Returning without a verdict means we are asked again
	 * next frame; giving up is reserved for a settled image that still does not
	 * match. */
	if (!ss_readable((uintptr_t)p, sizeof(sig) + 1))
		return;
	if (memcmp(p, sig, sizeof(sig)) != 0) {
		unsigned char *found = NULL;
		size_t span, i;
		int hits = 0;

		if (g_dec_tries < 60)
			return; /* still early; let the stub finish */
		span = pe_image_span(exe);
		for (i = 0; span && i + sizeof(sig) < span; i++) {
			unsigned char *q = (unsigned char *)exe + i;

			if (q[0] != 0x83 || memcmp(q, sig, sizeof(sig)) != 0)
				continue;
			if (!ss_readable((uintptr_t)q, sizeof(sig) + 1))
				continue;
			hits++;
			found = q;
		}
		if (hits != 1) {
			/* Zero means the code is not this build. More than one means
			 * the offset no longer identifies a single site, and guessing
			 * which to patch would be worse than not patching. */
			ss_raw("decoder: not patching - the refill test at rabiribi+6F4C6 "
			       "reads %02X %02X %02X %02X %02X %02X %02X %02X, not the "
			       "expected 83 BE 54 04 00 00 00 75, and a scan of the image "
			       "found %d other site(s) with that pattern\n",
			       p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], hits);
			g_decpatch = -1;
			return;
		}
		ss_raw("decoder: rabiribi+6F4C6 did not match, but the same nine bytes "
		       "were found once at +%X, so that is the refill test\n",
		       (unsigned)(found - (unsigned char *)exe));
		p = found;
	}
	if (!VirtualProtect(p + 7, 1, PAGE_EXECUTE_READWRITE, &old)) {
		ss_raw("decoder: found the refill test but could not make it writable\n");
		g_decpatch = -1;
		return;
	}
	p[7] = 0x7F; /* jne -> jg */
	VirtualProtect(p + 7, 1, old, &old);
	FlushInstructionCache(GetCurrentProcess(), p + 7, 1);
	g_decpatch = 1;
	ss_raw("decoder: rabiribi+6F4CD jne -> jg. A codec buffer whose remaining "
	       "count has gone negative now refills instead of handing the negative "
	       "to memcpy as a 4 GB length\n");
}

static uintptr_t g_nothr_at;
/* x86 only: every read/write of this counter sits under the 32-bit fault-fixup
 * guard, so on a 64-bit target it would otherwise be an unused static. */
#if defined(_M_IX86) || defined(__i386__)
static int g_nothr_hits;
#endif
static DWORD g_nothr_until;
static void nothread_arm(int on);

#define SS_FF_CAP 64
static uintptr_t g_ff_at[SS_FF_CAP];
static int g_ff_n;

static int ss_at_fastfail(uintptr_t pc)
{
	int i;

	for (i = 0; i < g_ff_n; i++)
		if (pc == g_ff_at[i] || pc == g_ff_at[i] + 1 || pc == g_ff_at[i] + 2)
			return 1;
	return 0;
}

static void fastfail_disarm(void)
{
#if defined(_M_IX86) || defined(__i386__)
	HMODULE nt = GetModuleHandleA("ntdll.dll");
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)nt;
	const IMAGE_NT_HEADERS *pe;
	const IMAGE_SECTION_HEADER *sec;
	int i, patched = 0, seen = 0;

	if (!nt || freeze_mode() < 2 || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return;
	pe = (const IMAGE_NT_HEADERS *)((const char *)nt + dos->e_lfanew);
	if (pe->Signature != IMAGE_NT_SIGNATURE)
		return;
	sec = IMAGE_FIRST_SECTION(pe);
	for (i = 0; i < pe->FileHeader.NumberOfSections; i++) {
		unsigned char *p, *end;

		if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
			continue;
		p = (unsigned char *)nt + sec[i].VirtualAddress;
		end = p + sec[i].Misc.VirtualSize;
		for (; p + 2 <= end; p++) {
			DWORD old;

			if (p[0] != 0xCD || p[1] != 0x29)
				continue;
			seen++;
			if ((uintptr_t)p - (uintptr_t)nt < 5 || p[-5] != 0xB9)
				continue; /* not a constant-code fastfail; leave it */
			if (g_ff_n >= SS_FF_CAP)
				break;
			if (!VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old))
				continue;
			p[0] = 0xCC; /* int 3, which a vectored handler does see */
			p[1] = 0x90;
			VirtualProtect(p, 2, old, &old);
			g_ff_at[g_ff_n++] = (uintptr_t)p;
			patched++;
		}
	}
	ss_raw("fastfail: %d of %d int 29h site(s) in ntdll turned into breakpoints, "
	       "so heap metadata failures freeze instead of terminating\n",
	       patched, seen);
#endif
}

/* The quiet exit, caught at the door.
 *
 * Ten restores held, no exception was raised, no heap failed, and the process
 * left anyway. That is not a crash, it is a decision, and the existing hooks
 * could not see it: they patch import tables for ExitProcess and
 * TerminateProcess, and the CRT reaches RtlExitUserProcess inside ntdll without
 * going through either.
 *
 * A breakpoint on the first byte of the real function catches every caller
 * regardless of how it got there, and since the point is to stop rather than to
 * continue, there is no trampoline to build and nothing to restore. The return
 * address is still sitting at the top of the stack when the byte executes, so
 * the handler can name who asked. */
static uintptr_t g_exit_at[4];
static const char *g_exit_name[4];
static int g_exit_n;

static const char *ss_at_exitpath(uintptr_t pc)
{
	int i;

	for (i = 0; i < g_exit_n; i++)
		if (pc == g_exit_at[i] || pc == g_exit_at[i] + 1)
			return g_exit_name[i];
	return NULL;
}

/* Milliseconds after a restore during which NtCreateThreadEx returns a failure
 * instead of making a thread. 0 is off. */
static int nothread_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[16];
		DWORD n = ss_getenv("D3D9SW_NOTHREAD", v, sizeof(v));

		if (!n || n >= sizeof(v))
			return 0;
		cached = atoi(v);
		if (cached < 0)
			cached = 0;
	}
	return cached;
}

static void nothread_arm(int on)
{
#if defined(_M_IX86) || defined(__i386__)
	static unsigned char saved;
	HMODULE nt;
	unsigned char *p;
	DWORD old;

	if (!nothread_mode() || (on && g_nothr_at) || (!on && !g_nothr_at))
		return;
	nt = GetModuleHandleA("ntdll.dll");
	p = nt ? (unsigned char *)(void *)GetProcAddress(nt, "NtCreateThreadEx") : NULL;
	if (!p || !VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old))
		return;
	if (on) {
		saved = *p;
		*p = 0xCC;
		g_nothr_at = (uintptr_t)p;
		g_nothr_hits = 0;
		ss_hook_note("byte", "NtCreateThreadEx", p, 1);
	} else {
		*p = saved;
		g_nothr_at = 0;
	}
	VirtualProtect(p, 1, old, &old);
	ss_log("NOTHREAD: thread creation %s at %p%s\n", on ? "BLOCKED" : "allowed again",
	       (void *)p,
	       on ? "" : " - see the refusals above for what asked while it was shut");
#else
	(void)on;
#endif
}

static void exitpath_arm(void)
{
	static const char *const names[] = { "RtlExitUserProcess", "NtTerminateProcess" };
	HMODULE nt = GetModuleHandleA("ntdll.dll");
	int i;

	if (!nt || freeze_mode() < 2)
		return;
	for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
		unsigned char *p = (unsigned char *)(void *)GetProcAddress(nt, names[i]);
		DWORD old;

		if (!p || g_exit_n >= (int)(sizeof(g_exit_at) / sizeof(g_exit_at[0])))
			continue;
		if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old))
			continue;
		g_exit_at[g_exit_n] = (uintptr_t)p;
		g_exit_name[g_exit_n] = names[i];
		g_exit_n++;
		*p = 0xCC;
		VirtualProtect(p, 1, old, &old);
		ss_hook_note("byte", names[i], p, 1);
		ss_raw("exitpath: %s at %p will freeze instead of ending the process\n",
		       names[i], (void *)p);
	}
}

/* Armed on the first save rather than at load.
 *
 * At DLL init the config is not readable yet - the D3D11 wrapper has not
 * finished seeding the environment and the working directory may not be the
 * game's - so freeze_mode() answered zero and none of this armed. That is the
 * same late-initialisation trap that hid HEAPBLOCKS for a day, so the arming
 * waits until a moment when the settings are known to be live. */
static void freeze_arm_once(void)
{
	static int done;

	if (done)
		return;
	done = 1;
	fastfail_disarm();
	exitpath_arm();
}

/* Counted here rather than in the control block, which is not in scope this
 * early in the file. This lives in our own image and our image rewinds with the
 * game, so a restore winds it back - it counts fixups since the last restore,
 * not since the session began, and the log says so. */
/* x86 only, like the LFH divide fixup that reads them (the frame it walks is the
 * 32-bit allocator's); the x64 build compiles this file too and would strand both
 * as unused. */
#if defined(_M_IX86) || defined(__i386__)
static unsigned long long g_div_fix;

/* Off makes the fault fatal again, which is the only way to tell whether the
 * fixup is carrying a session or merely hiding its ending. */
static int div_fixup(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_DIVFIX", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '0') ? 0 : 1;
	}
	return cached;
}
#endif

/* After a load, the first exceptions of any kind, said as they happen: the
 * dispatcher checks the SEH chain after the vectored handlers run, and a bad
 * chain is a fail-fast that no exit hook gets to print. */
static volatile LONG g_xlog_left;

static void xlog_chain(DWORD code, uintptr_t pc)
{
#if defined(_M_IX86) || defined(__i386__)
	NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
	uintptr_t base = (uintptr_t)tib->StackBase, lim = (uintptr_t)tib->StackLimit;
	uintptr_t r = (uintptr_t)tib->ExceptionList;
	unsigned off = 0;
	const char *in = ss_module(pc, &off);
	int n;

	ss_raw("xlog: thread %lu raised %08lX at %08lX (%s+%X), stack %08lX..%08lX, "
	       "seh head %08lX\n",
	       (unsigned long)GetCurrentThreadId(), (unsigned long)code, (unsigned long)pc,
	       in ? in : "?", off, (unsigned long)lim, (unsigned long)base, (unsigned long)r);
	for (n = 0; n < 24 && r != (uintptr_t)-1; n++) {
		uintptr_t next, h;
		const char *why = NULL;

		if (r < lim || r + 8 > base)
			why = "outside this thread's stack";
		else if (r & 3)
			why = "misaligned";
		else if (!ss_readable(r, 8))
			why = "unreadable";
		if (why) {
			ss_raw("xlog:   record %08lX %s - THE CHAIN BREAKS HERE\n",
			       (unsigned long)r, why);
			return;
		}
		next = ((uintptr_t *)r)[0];
		h = ((uintptr_t *)r)[1];
		off = 0;
		in = ss_module(h, &off);
		ss_raw("xlog:   record %08lX next %08lX handler %08lX (%s+%X)%s\n",
		       (unsigned long)r, (unsigned long)next, (unsigned long)h, in ? in : "?",
		       off, (h >= lim && h < base) ? " - handler on the stack" : "");
		if (next != (uintptr_t)-1 && next <= r) {
			ss_raw("xlog:   next is not above this record - THE CHAIN BREAKS HERE\n");
			return;
		}
		r = next;
	}
	ss_raw("xlog:   chain %s after %d record(s)\n",
	       r == (uintptr_t)-1 ? "ends normally" : "still going", n);
#else
	(void)code;
	(void)pc;
#endif
}

/* A fault inside the handler re-enters it, and on that path the handler has
 * looped until the stack ran out, hiding the fault that started it. */
static _Thread_local int g_in_fault;
static LONG ss_fault_body(EXCEPTION_POINTERS *ep);

static LONG CALLBACK ss_fault_log(EXCEPTION_POINTERS *ep)
{
	LONG r;

	if (g_in_fault)
		return EXCEPTION_CONTINUE_SEARCH;
	g_in_fault = 1;
	r = ss_fault_body(ep);
	g_in_fault = 0;
	return r;
}

static LONG ss_fault_body(EXCEPTION_POINTERS *ep)
{
	DWORD code = ep->ExceptionRecord->ExceptionCode;
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t pc;
	int held;

	pc = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;

	/* Before anything else, because this one is not a crash report - it is our
	 * own heap walk failing, and the only correct response is to leave. */
	if (code == EXCEPTION_ACCESS_VIOLATION && g_hw_armed &&
	    GetCurrentThreadId() == g_hw_tid) {
		unsigned moff = 0;
		const char *mod = ss_module(pc, &moff);

		g_hw_armed = 0;
		InterlockedIncrement(&g_hw_faults);
		ss_raw("heapwalk: FAULTED at %p (%s+%X) inside our own HeapWalk - the "
		       "allocator's LFH chain is inconsistent. Abandoning the walk "
		       "rather than taking the process down with it.\n",
		       (void *)pc, mod ? mod : "?", moff);
		g_in_fault = 0;
		longjmp(g_hw_jmp, 1);
	}

	/* A breakpoint at a site we patched is not a breakpoint. It is the fastfail
	 * that would otherwise have taken the process out with 0xC0000409 before
	 * any handler ran, and ecx still holds the reason ntdll gave. */
	/* A block copy asking for two gigabytes is a length that came out negative.
	 *
	 * The game streams audio by subtracting a play cursor from a write cursor
	 * and copying the difference. DirectSound is excluded from the rewind and
	 * has to be - it talks to the sound card, and the card keeps playing while
	 * we are stopped - so after a restore the hardware's cursor is in the
	 * present and the game's is not. Subtract them in the wrong order and the
	 * difference is negative, which as an unsigned count is 4.29 billion bytes.
	 *
	 * Caught at rabiribi.exe+36913A with ecx = FFEAAD9C, one restore after the
	 * clock was first wound back. Winding the clock back did not create the
	 * disagreement; it widened one that every restore has had, until the
	 * subtraction crossed zero.
	 *
	 * These addresses name the copy, not the caller. +6E9F8 was read out of the
	 * running process and is the statically linked CRT memcpy, which every copy
	 * in the program goes through, so it identifies nobody - see runaway_caller,
	 * which walks the frame to answer the question this address only appears to.
	 *
	 * Clamping the count abandons the rest of that copy, which costs a click or
	 * a moment of silence. There is a proper fix - proxy dsound and own the
	 * cursors - and this is not it. But no correct code copies two gigabytes in
	 * one instruction, so refusing to is right on its own terms, and it is not
	 * specific to this game or this address the way the DDPR overrun guard was.
	 */
#if defined(_M_IX86) || defined(__i386__)
	if (code == EXCEPTION_ACCESS_VIOLATION && runaway_mode()) {
		const unsigned char *ins = (const unsigned char *)pc;
		CONTEXT *c = ep->ContextRecord;

		/* Probe before reading, because the one case that most needs a fault
		 * report is the one where this read is itself illegal.
		 *
		 * A call through a null pointer arrives here with pc at zero, and this
		 * test then reads address zero looking for a rep prefix and faults
		 * inside the handler. What reached the log was "fault at d3d11.dll
		 * +1AEF7 reading 00000000" - our own logger, with the exception code
		 * still sitting in esi and edi - and not one word about the null call
		 * that started it. The report named the reporter.
		 *
		 * Two bytes is what the test below reads. */
		if (ss_readable(pc, 2) && ins[0] == 0xF3 &&
		    (ins[1] == 0xA4 || ins[1] == 0xA5 || ins[1] == 0xAA || ins[1] == 0xAB) &&
		    (c->Ecx & 0x80000000u)) {
			unsigned moff = 0;
			const char *mod = ss_module(pc, &moff);

			g_runaway_hits++;
			if (g_runaway_hits <= 20 || !(g_runaway_hits % 100)) {
				ss_raw("runaway: %s of %ld byte(s) at %p (%s+%X) - a negative "
				       "length, so the copy is abandoned rather than run. "
				       "Hit %d this session\n",
				       ins[1] == 0xA4 || ins[1] == 0xA5 ? "block copy"
									: "block fill",
				       (long)c->Ecx, (void *)pc, mod ? mod : "?", moff,
				       g_runaway_hits);
				/* A length this shape is a subtraction of two positions
				 * that drifted apart, and the interesting question is
				 * which side of the rewind each one lives on. The
				 * magnitude grows every restore - -128819, then -147107,
				 * then -200339 - which is what a pointer we wind back and
				 * a pointer we leave running look like from a distance.
				 * Saying where they point turns that from a guess into a
				 * reading. */
				ss_where_reg("edi (destination)", (uintptr_t)c->Edi);
				ss_where_reg("esi (source)", (uintptr_t)c->Esi);
				blk_provenance((uintptr_t)c->Edi);
				blk_provenance((uintptr_t)c->Esi);
				runaway_caller(c, pc);
			}
			/* No fault_gate_leave here: this runs before the gate is
			 * entered, and the gate's counter is not ours to touch. */
			c->Ecx = 0; /* rep with zero count completes and moves on */
			return EXCEPTION_CONTINUE_EXECUTION;
		}
	}
#endif
	/* Thread creation refused, by request, to prove what depends on it.
	 *
	 * Every new thread allocates its TLS from the process heap before it runs a
	 * single instruction of its own, which is why LdrpAllocateTls keeps turning
	 * up at the bottom of these crashes. Turning creation into a failure return
	 * for a window after each restore answers a question logging cannot: what
	 * the game does when it asks for a thread and does not get one.
	 *
	 * The syscall stub is entered, so the return address is still on top of the
	 * stack and none of the eleven arguments have been touched. Faking the
	 * return means putting the caller's address into eip, popping the arguments
	 * the way __stdcall would have, and handing back a status. */
#if defined(_M_IX86) || defined(__i386__)
	if (code == EXCEPTION_BREAKPOINT && g_nothr_at && pc == g_nothr_at) {
		CONTEXT *c = ep->ContextRecord;
		uintptr_t *sp = (uintptr_t *)(uintptr_t)c->Esp;
		uintptr_t caller = sp[0];
		unsigned moff = 0;
		const char *mod = ss_module(caller, &moff);

		g_nothr_hits++;
		ss_raw("NOTHREAD: refused thread creation #%d, asked for by %p (%s+%X)\n",
		       g_nothr_hits, (void *)caller, mod ? mod : "?", moff);
		c->Eip = (DWORD)caller;
		c->Esp += 4 + 11 * 4;
		c->Eax = 0xC0000022; /* STATUS_ACCESS_DENIED */
		return EXCEPTION_CONTINUE_EXECUTION;
	}
#endif
	if (code == EXCEPTION_BREAKPOINT) {
		const char *door = ss_at_exitpath(pc);
		unsigned sub = 0;

#if defined(_M_IX86) || defined(__i386__)
		sub = (unsigned)ep->ContextRecord->Ecx;
#endif
		if (door) {
			uintptr_t caller = 0;
			unsigned moff = 0;
			const char *mod;

#if defined(_M_IX86) || defined(__i386__)
			/* Entry byte, so the return address is still on top. */
			caller = *(const uintptr_t *)(uintptr_t)ep->ContextRecord->Esp;
#endif
			mod = ss_module(caller, &moff);
			ss_raw("exitpath: %s called from %p (%s+%X) - the process was "
			       "leaving deliberately, not crashing\n",
			       door, (void *)caller, mod ? mod : "?", moff);
			ss_callers(ep->ContextRecord);
			ss_freeze_here(door, pc, 0);
			return EXCEPTION_CONTINUE_SEARCH;
		}
		if (ss_at_fastfail(pc)) {
			ss_raw("fastfail: ntdll raised failure code %u at %p - this would "
			       "have killed the process silently, with no handler "
			       "consulted\n",
			       sub, (void *)pc);
			ss_freeze_here("fastfail - ntdll found its own structures corrupt",
				       pc, sub);
			return EXCEPTION_CONTINUE_SEARCH;
		}
	}

	/* Ahead of everything else, and returning rather than falling through: a
	 * guard page we armed ourselves is not a fault, it is the answer to a
	 * question we asked, and it must not reach the reporting below that treats
	 * an access violation as a crash. */
	if (code == STATUS_GUARD_PAGE_VIOLATION && g_catch_n &&
	    ep->ExceptionRecord->NumberParameters >= 2) {
		uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
		int rw = (int)ep->ExceptionRecord->ExceptionInformation[0];
		LONG k, n = g_catch_n;

		for (k = 0; k < n && k < SS_CATCH_CAP; k++) {
			unsigned moff;
			const char *mod;

			if ((at & ~(uintptr_t)0xFFF) != g_catch_at[k])
				continue;
			if (InterlockedIncrement(&g_catch_hits) <= 40) {
				mod = ss_module(pc, &moff);
				ss_raw("catch: thread %lu %s %p from %p (%s+%x)\n",
				       (unsigned long)GetCurrentThreadId(),
				       rw ? "WROTE" : "read", (void *)at, (void *)pc,
				       mod ? mod : "no module", moff);
			}
			return EXCEPTION_CONTINUE_EXECUTION;
		}
	}

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

	if (g_xlog_left > 0 && InterlockedDecrement(&g_xlog_left) >= 0)
		xlog_chain(code, pc);

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
	cfg_fault_note();
	focus_report();
	gameheap_va_dump(200);
	/* What the instruction was reaching for.
	 *
	 * rabiribi.exe+6E9F8 has now killed the process twice, once immediately
	 * after a restore that crossed a room boundary. The obvious next move is to
	 * disassemble it, and that move is not available: the on-disk .text is
	 * encrypted behind a Steam .bind stub and only exists in plaintext in the
	 * memory of a process that is, by the time we care, dead.
	 *
	 * So describe the crash from the inside instead. An access violation
	 * already carries the two facts a disassembly would have been used to
	 * recover - which direction the access went and what address it named - and
	 * VirtualQuery turns the second into a verdict: a write into PAGE_READONLY
	 * is a pointer that should have been to a buffer and is instead to a
	 * literal; a MEM_FREE target is a pointer to something that was reissued;
	 * committed and writable means the pointer was fine and its contents were
	 * not. Those are different bugs and we have been guessing between them.
	 *
	 * The bytes at the faulting address are dumped too, because the plaintext
	 * instruction is right there under our own hand and nowhere else. */
	if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) {
		ULONG_PTR what = ep->ExceptionRecord->NumberParameters > 0
					 ? ep->ExceptionRecord->ExceptionInformation[0]
					 : 2;
		uintptr_t at = ep->ExceptionRecord->NumberParameters > 1
				       ? (uintptr_t)ep->ExceptionRecord
						 ->ExceptionInformation[1]
				       : 0;
		MEMORY_BASIC_INFORMATION mbi;
		unsigned off = 0;
		const char *in;

		in = ss_module(at, &off);
		ss_raw("       reaching: %s %08lX%s%s%s\n",
		       what == 1 ? "WROTE to" : what == 8 ? "EXECUTED" : "read from",
		       (unsigned long)at, in ? " in " : "", in ? in : "",
		       in ? "" : " (no module)");
		fault_saved_note(at);
		if (VirtualQuery((LPCVOID)at, &mbi, sizeof(mbi)) == sizeof(mbi))
			ss_raw("       that page: base %08lX, %lu bytes, state %s, "
			       "protect %08lX, type %s\n",
			       (unsigned long)(ULONG_PTR)mbi.BaseAddress,
			       (unsigned long)mbi.RegionSize,
			       mbi.State == MEM_COMMIT	 ? "committed"
			       : mbi.State == MEM_RESERVE ? "RESERVED, not committed"
							  : "FREE - nothing is there",
			       (unsigned long)(mbi.State == MEM_COMMIT ? mbi.Protect
								       : 0),
			       mbi.Type == MEM_IMAGE   ? "image"
			       : mbi.Type == MEM_MAPPED ? "mapped"
			       : mbi.Type == MEM_PRIVATE ? "private"
							 : "none");
		else
			ss_raw("       that page: VirtualQuery would not describe it at "
			       "all, so the address is not in this address space\n");
		if (ss_readable(pc, 16)) {
			const unsigned char *q = (const unsigned char *)pc;

			ss_raw("       the instruction, in plaintext because we are inside "
			       "the process: %02X %02X %02X %02X %02X %02X %02X %02X "
			       "%02X %02X %02X %02X %02X %02X %02X %02X\n",
			       q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8],
			       q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
		}
#if defined(_M_IX86) || defined(__i386__)
		ss_raw("       registers: eax %08lX ebx %08lX ecx %08lX edx %08lX "
		       "esi %08lX edi %08lX ebp %08lX esp %08lX\n",
		       (unsigned long)ep->ContextRecord->Eax,
		       (unsigned long)ep->ContextRecord->Ebx,
		       (unsigned long)ep->ContextRecord->Ecx,
		       (unsigned long)ep->ContextRecord->Edx,
		       (unsigned long)ep->ContextRecord->Esi,
		       (unsigned long)ep->ContextRecord->Edi,
		       (unsigned long)ep->ContextRecord->Ebp,
		       (unsigned long)ep->ContextRecord->Esp);
#endif
	}
	/* The one arithmetic fault we can name exactly.
	 *
	 * ntdll's RtlpSubSegmentInitialize ends with `div ebx` where ebx is the
	 * LFH subsegment's BlockCount. The dividend there is a zero-extended byte
	 * from a random table, so a quotient overflow is impossible and a zero
	 * divisor is the only way that instruction can fault. BlockCount reaches
	 * zero on exactly one path: the UserBlocks region was too small to hold
	 * its bitmap header plus a single block, so the block-laying loop never
	 * ran.
	 *
	 * That region is described by a _HEAP_USERDATA_HEADER popped off an SList
	 * whose head lives in LFH bookkeeping. Two different corruptions produce
	 * the same crash, and only the header's signature tells them apart: if it
	 * is not F0E0D0C0 the list head pointed at memory that is not a UserBlocks
	 * header at all, and if it is correct but the size is tiny then the header
	 * itself was rewound out from under a list head that was not.
	 *
	 * Offsets verified by disassembly against ntdll 10.0.26100.8246 and
	 * checked twice over: the frame size implied by the prologue matches the
	 * ebp-esp of a real fault, and the function writes the F0E0D0C0 signature
	 * itself. Guarded on the exact RVA so a different build simply prints
	 * nothing rather than inventing a reading. */
#if defined(_M_IX86) || defined(__i386__)
	if (code == 0xC0000094u) {
		unsigned off = 0;
		const char *in = ss_module(pc, &off);

		if (in && lstrcmpiA(in, "ntdll.dll") == 0 && off == 0x43E11) {
			uintptr_t fp = (uintptr_t)ep->ContextRecord->Ebp;

			if (ss_readable(fp + 8, 3 * sizeof(uintptr_t))) {
				uintptr_t ub = *(const uintptr_t *)(fp + 8);
				uintptr_t blk = *(const uintptr_t *)(fp + 0xC);
				uintptr_t usable = *(const uintptr_t *)(fp + 0x10);

				ss_raw("       LFH: BlockCount was zero. UserBlocks %08lX, "
				       "block size %lu, usable %lu\n",
				       (unsigned long)ub, (unsigned long)blk,
				       (unsigned long)usable);
				if (ss_readable(ub + 0xC, sizeof(unsigned))) {
					unsigned sig = *(const unsigned *)(ub + 0xC);
					unsigned char si =
						*(const unsigned char *)(ub + 8);
					unsigned short pad =
						*(const unsigned short *)(ub + 0xA);

					ss_raw("       LFH: signature %08lX (%s), "
					       "SizeIndex %u, PaddingBytes %u - %s\n",
					       (unsigned long)sig,
					       sig == 0xF0E0D0C0u ? "correct"
							  : "WRONG",
					       (unsigned)si, (unsigned)pad,
					       sig == 0xF0E0D0C0u
						       ? "a real header, so the "
							 "header itself was rewound "
							 "under a list head that "
							 "was not"
						       : "NOT a UserBlocks header, "
							 "so the list head pointed "
							 "at the wrong memory");
					/* Same triage the register dump uses, so the
					 * header is placed on the same map as
					 * everything else in the report: which
					 * heap, and which side of the restore. */
					ss_where_reg("       UserBlocks", ub);
				} else {
					ss_raw("       LFH: UserBlocks %08lX is not "
					       "readable, so the list head pointed "
					       "at memory that is gone\n",
					       (unsigned long)ub);
				}
			}
			/* Supply the value the divide was reaching for.
			 *
			 * BlockCount is not a number anyone chose - ntdll derives
			 * it from how many blocks fit in the UserBlocks region,
			 * and both operands of that division are in the frame. So
			 * rather than invent a constant, recompute what it was
			 * trying to compute and only fall back to one block if
			 * that is degenerate too.
			 *
			 * The dividend is a zero-extended byte, so with any
			 * divisor of one or more the quotient cannot overflow and
			 * re-fault. edx is forced to zero for the same reason:
			 * it is the high half of the dividend and the disassembly
			 * says it should already be zero.
			 *
			 * This does not repair the state that produced the zero.
			 * It buys a session that keeps running and keeps
			 * reporting, instead of one that ends here. */
			if (div_fixup()) {
				uintptr_t fix = 1;

				if (ss_readable(fp + 0xC, 2 * sizeof(uintptr_t))) {
					uintptr_t blk = *(const uintptr_t *)(fp + 0xC);
					uintptr_t usable =
						*(const uintptr_t *)(fp + 0x10);

					if (blk && usable >= blk)
						fix = usable / blk;
				}
				g_div_fix++;
				ss_raw("       LFH: divisor set to %lu and execution "
				       "resumed - the allocator carries on with a "
				       "recomputed BlockCount (%llu since the last "
				       "restore)\n",
				       (unsigned long)fix, g_div_fix);
				ep->ContextRecord->Ebx = (DWORD)fix;
				ep->ContextRecord->Edx = 0;
				return EXCEPTION_CONTINUE_EXECUTION;
			}
		}
	}
#endif
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
		/* A fault whose own pc is unreadable is a call through a bad pointer,
		 * not a bad access by the code at pc - there is no code at pc. Saying so
		 * costs one line and points at a different culprit: the caller, whose
		 * return address is still on the stack, rather than the address the
		 * report leads with. */
		if (!ss_readable(pc, 1)) {
			uintptr_t ra = 0;
#if defined(_M_IX86) || defined(__i386__)
			uintptr_t sp = (uintptr_t)ep->ContextRecord->Esp;
#else
			uintptr_t sp = (uintptr_t)ep->ContextRecord->Rsp;
#endif

			if (ss_readable(sp, sizeof(ra)))
				ra = *(uintptr_t *)sp;
			ss_raw("       VERDICT: there is no code at %p, so this is a call "
			       "through a bad function pointer, not a bad access. The "
			       "caller pushed a return address of %p\n",
			       (void *)pc, (void *)ra);
			if (ra) {
				unsigned roff = 0;
				const char *rm = ss_module(ra, &roff);

				if (rm)
					ss_raw("       the call was made from %s+%X\n", rm, roff);
			}
		}
		{
			uintptr_t db, dz;

			if (decommitted_drift(at, &db, &dz))
				ss_raw("       VERDICT: that address is inside %p+%lx, which the "
				       "last restore decommitted as unclaimed drift. Something "
				       "in the present still held it\n",
				       (void *)db, (unsigned long)dz);
		}
		blk_provenance(at);
	}
#if defined(_M_IX86) || defined(__i386__)
	/* The 32-bit half of the register dump, which never existed.
	 *
	 * Every crash in this game is 32-bit, and this whole family of them reads a
	 * small constant - 4, 0x20, 0xE - which is a null pointer plus a field
	 * offset. So the faulting address names no block and provenance has nothing
	 * to say about it, which is exactly what happened on the run that prompted
	 * this: a null deref inside RtlpAllocateHeap, reached from the game's own
	 * malloc, and not one word about which heap object was involved.
	 *
	 * The pointer that would name a block is the one the code was holding, and
	 * that is sitting in a register. Dumping them was under the 64-bit guard
	 * for no better reason than that the 64-bit path was written first, so
	 * eight registers of evidence have been discarded on every fault this game
	 * has ever produced. */
	if (ep->ContextRecord) {
		static const char *const nm[8] = { "eax", "ecx", "edx", "ebx",
						   "esp", "ebp", "esi", "edi" };
		const CONTEXT *c = ep->ContextRecord;
		const DWORD gpr[8] = { c->Eax, c->Ecx, c->Edx, c->Ebx,
				       c->Esp, c->Ebp, c->Esi, c->Edi };
		int i;

		for (i = 0; i < 8; i++) {
			ss_where_reg(nm[i], (uintptr_t)gpr[i]);
			blk_provenance((uintptr_t)gpr[i]);
		}
	}
#endif
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

		for (i = 0; i < 16; i++) {
			ss_where_reg(nm[i], (uintptr_t)gpr[i]);
			blk_provenance((uintptr_t)gpr[i]);
		}

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
	/* Everything above is what we could learn without a debugger. Freezing is
	 * what lets a debugger learn the rest, so it goes last - the log is already
	 * written by the time the world stops. Never returns when it takes. */
	if (code == EXCEPTION_ACCESS_VIOLATION || code == 0xC0000094 /* div by zero */ ||
	    code == 0xC000001D /* illegal instruction */ ||
	    code == 0xC0000096 /* privileged instruction */ ||
	    code == EXCEPTION_INT_OVERFLOW || code == EXCEPTION_STACK_OVERFLOW)
		ss_freeze_here("fault", pc, code);
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
static void(__cdecl *g_real_abort)(void);

/* Sticky until the op returns: who is in savestate, and which stage. The
 * ExitProcess hook reads these because "chose to leave" previously named
 * only the CRT thunk, not the thread or whether copy was in progress. */
static const char *g_ss_op;
static DWORD g_ss_op_tid;

static void ss_op_set(const char *op)
{
	g_ss_op = op;
	g_ss_op_tid = GetCurrentThreadId();
}

static void ss_name_leave_tid(DWORD tid);
static int foreign_seen(void);

static void WINAPI hook_exitprocess(UINT code)
{
	void *bt[12];
	USHORT n, k;
	DWORD tid = GetCurrentThreadId();
	void *caller = __builtin_return_address(0);
	uintptr_t c = (uintptr_t)caller;

	exit_report("ExitProcess", caller);
	ss_log("      tid %lu code %u ss_op=%s ss_op_tid=%lu%s\n", (unsigned long)tid,
	       (unsigned)code, g_ss_op ? g_ss_op : "(none)", (unsigned long)g_ss_op_tid,
	       (g_ss_op_tid && tid == g_ss_op_tid) ? " - this IS the savestate thread"
						   : " - a present peer (not the savestate thread)");
	if (g_real_uexit) {
		uintptr_t e = (uintptr_t)(void *)g_real_uexit;
		if (c >= e && c < e + 128)
			ss_log("      caller sits inside CRT _exit (+%X) - intra-module "
			       "teardown; IAT exit/_exit would not see it\n",
			       (unsigned)(c - e));
	}
	if (g_real_exit) {
		uintptr_t e = (uintptr_t)(void *)g_real_exit;
		if (c >= e && c < e + 128)
			ss_log("      caller sits inside CRT exit (+%X)\n", (unsigned)(c - e));
	}
	if (g_real_abort) {
		uintptr_t e = (uintptr_t)(void *)g_real_abort;
		if (c >= e && c < e + 64)
			ss_log("      caller sits inside CRT abort\n");
	}
	if (code == 3)
		ss_log("      exit code 3 is the CRT abort/_exit convention "
		       "(invalid_parameter, abort, or _invoke_watson)\n");
	ss_name_leave_tid(tid);
	n = CaptureStackBackTrace(0, 12, bt, NULL);
	for (k = 0; k < n; k++) {
		unsigned off = 0;
		const char *mod = ss_module((uintptr_t)bt[k], &off);
		ss_log("      leave stack[%u] %p %s+%X\n", (unsigned)k, bt[k],
		       mod ? mod : "?", off);
	}
	/* The game's own shutdown has already run by now. What ExitProcess still
	 * does is detach every DLL and tear down the loader, walking lists the
	 * restore brought in from the launch that saved. */
	if (foreign_seen() && g_real_terminate) {
		ss_log("      after a load from another process: ending here, no DLL "
		       "detach\n");
		g_real_terminate(GetCurrentProcess(), code);
	}
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

/* Addresses handed back to the allocator while a snapshot is live.
 *
 * The restore identifies a block by address and size: if something is still busy
 * at the same place and the same length, we assume it is the same allocation and
 * write the saved bytes into it. That assumption has never been tested, and it
 * is exactly the assumption a heap breaks by design - allocators prefer to reuse
 * the most recently freed block of a given size, so free-then-reallocate at the
 * same address is the normal behaviour of a heap under churn.
 *
 * When it happens we write the old owner's bytes over the new owner's object,
 * and every check we have reports success: the bytes we intended did land, the
 * heap still validates, and the block really is busy at the right address. This
 * table is the only thing that can see it. A matched block whose address appears
 * here was freed and re-handed-out between the save and the restore, and putting
 * it back is not a restore, it is corruption.
 *
 * Open addressing with an atomic claim, because frees arrive on every thread in
 * the process and a lock here would serialise the game's allocator through our
 * diagnostic. */
#define SS_FREED_SLOTS 65536 /* power of two, masked below */

typedef struct {
	volatile LONG used;
	volatile LONG overflow;
	volatile LONG slot[SS_FREED_SLOTS];
} FreedSet;

static FreedSet *g_freed SS_PRESENT;
static volatile LONG g_freed_arm;

static unsigned freed_hash(uintptr_t a)
{
	/* Shifted first: heap blocks are at least 8-byte aligned, so the low bits
	 * are constant and hashing them wastes most of the table. */
	return (unsigned)(((a >> 3) * 2654435761u) & (SS_FREED_SLOTS - 1));
}

static void freed_note(uintptr_t a)
{
	unsigned i = freed_hash(a), n;

	for (n = 0; n < 64; n++) {
		unsigned k = (i + n) & (SS_FREED_SLOTS - 1);
		LONG cur = g_freed->slot[k];

		if (cur == (LONG)a)
			return;
		if (!cur && InterlockedCompareExchange(&g_freed->slot[k], (LONG)a, 0) == 0) {
			InterlockedIncrement(&g_freed->used);
			return;
		}
	}
	/* Sixty-four probes deep means the table is saturated. Recorded rather than
	 * silently dropped, because an overflowing table under-reports and would
	 * make the theory look weaker than it is. */
	InterlockedIncrement(&g_freed->overflow);
}

static int freed_has(uintptr_t a)
{
	unsigned i = freed_hash(a), n;

	if (!g_freed)
		return 0;
	for (n = 0; n < 64; n++) {
		unsigned k = (i + n) & (SS_FREED_SLOTS - 1);
		LONG cur = g_freed->slot[k];

		if (cur == (LONG)a)
			return 1;
		if (!cur)
			return 0;
	}
	return 0;
}

static BOOL(WINAPI *g_real_heapfree)(HANDLE, DWORD, LPVOID);
static LPVOID(WINAPI *g_real_heaprealloc)(HANDLE, DWORD, LPVOID, SIZE_T);
/* The same two calls one layer down.
 *
 * Hooking only kernel32's HeapFree saw sixty frees across a whole room
 * transition, which is not a small number so much as an obviously wrong one. A C
 * runtime that was compiled against ntdll - which is most of them, and certainly
 * anything statically linked - calls RtlFreeHeap through its own import table
 * and never touches kernel32's thunk at all. Patching both catches each caller
 * once: kernel32's HeapFree reaches RtlFreeHeap by an internal call, not through
 * any import table we have patched, so nothing is counted twice. */
static BOOLEAN(NTAPI *g_real_rtlfree)(PVOID, ULONG, PVOID);
static PVOID(NTAPI *g_real_rtlrealloc)(PVOID, ULONG, PVOID, SIZE_T);

static BOOLEAN NTAPI hook_rtlfree(PVOID h, ULONG flags, PVOID p)
{
	if (g_freed_arm && p && g_freed)
		freed_note((uintptr_t)p);
	return g_real_rtlfree(h, flags, p);
}

static PVOID NTAPI hook_rtlrealloc(PVOID h, ULONG flags, PVOID p, SIZE_T n)
{
	PVOID r = g_real_rtlrealloc(h, flags, p, n);

	if (g_freed_arm && p && r != p && g_freed)
		freed_note((uintptr_t)p);
	return r;
}

static BOOL WINAPI hook_heapfree(HANDLE h, DWORD flags, LPVOID p)
{
	if (g_freed_arm && p && g_freed)
		freed_note((uintptr_t)p);
	return g_real_heapfree(h, flags, p);
}

static LPVOID WINAPI hook_heaprealloc(HANDLE h, DWORD flags, LPVOID p, SIZE_T n)
{
	LPVOID r = g_real_heaprealloc(h, flags, p, n);

	/* Only when the block actually moved. A realloc that grows in place keeps
	 * the same allocation and is not a recycle. */
	if (g_freed_arm && p && r != p && g_freed)
		freed_note((uintptr_t)p);
	return r;
}

/* What we have our hands on, recorded where it is installed.
 *
 * Written as the specification for a shape recorder. Before anything can replay
 * the game's behaviour into a harness, we have to be able to say precisely which
 * operations we are in a position to observe - and that list has been scattered
 * across IAT swaps, vtable swaps and raw byte patches in five files with no
 * single place that knows all of them. Asking "what do we touch" required
 * reading the source, which is exactly the sort of question that should be
 * answerable from a log.
 *
 * Kept in the data section rather than the held arena on purpose. Every entry is
 * written before the first snapshot and never changes afterwards, so rewinding
 * it is a no-op, and this has to work at hooks-install time when there is no
 * control block to register an exclusion in. */
#define SS_HOOKS_MAX 64

typedef struct {
	const char *kind; /* iat, byte, vtable - how the redirection is done */
	const char *what; /* the symbol or slot */
	void *at;         /* where, when there is a single site */
	int sites;        /* how many places were patched, for module-wide sweeps */
} SsHook;

static SsHook g_hook[SS_HOOKS_MAX];
static int g_hook_n;

void ss_hook_note(const char *kind, const char *what, void *at, int sites)
{
	int i;

	/* Idempotent, because hooks_install runs more than once by design and a
	 * second pass finding nothing new should not lengthen the list. */
	for (i = 0; i < g_hook_n; i++)
		if (g_hook[i].what == what || (g_hook[i].at && g_hook[i].at == at)) {
			if (sites > g_hook[i].sites)
				g_hook[i].sites = sites;
			return;
		}
	if (g_hook_n >= SS_HOOKS_MAX)
		return;
	g_hook[g_hook_n].kind = kind;
	g_hook[g_hook_n].what = what;
	g_hook[g_hook_n].at = at;
	g_hook[g_hook_n].sites = sites;
	g_hook_n++;
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
	static int veh;

	if (!g_events)
		g_events = (EventTrack *)VirtualAlloc(NULL, sizeof(EventTrack),
						      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!veh) {
		veh = 1;
		AddVectoredExceptionHandler(1, ss_fault_log);
	/* The fastfail and exit-path patches are NOT armed here. The handler has to
	 * exist before they can fire, which is true from this point on, but the
	 * config does not exist yet - so arming happens on the first save, where
	 * the settings are known to be readable. */
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
		if (g_real_createevent) {
			g_hooked_event += patch_iat(exe, (void *)g_real_createevent,
						    (void *)hook_createevent);
			ss_hook_note("iat", "CreateEventA", (void *)g_real_createevent,
				     g_hooked_event);
		}
	}

	/* Every module's imports, not just the executable's. The game's runtime
	 * frees through its own copy of the thunk, and msvcrt frees through
	 * another - both land here, which is the coverage that matters. Calls made
	 * straight to RtlFreeHeap inside ntdll are not caught, so the count this
	 * feeds is a floor rather than a total. */
	if (k32 && !g_real_heapfree) {
		g_real_heapfree = (BOOL(WINAPI *)(HANDLE, DWORD, LPVOID))(void *)GetProcAddress(
			k32, "HeapFree");
		g_real_heaprealloc = (LPVOID(WINAPI *)(HANDLE, DWORD, LPVOID, SIZE_T))(void *)
			GetProcAddress(k32, "HeapReAlloc");
		if (g_real_heapfree)
			ss_hook_note("iat", "HeapFree", (void *)g_real_heapfree,
				     patch_every_module((void *)g_real_heapfree,
							(void *)hook_heapfree));
		if (g_real_heaprealloc)
			ss_hook_note("iat", "HeapReAlloc", (void *)g_real_heaprealloc,
				     patch_every_module((void *)g_real_heaprealloc,
							(void *)hook_heaprealloc));
	}

	if (!g_real_rtlfree) {
		HMODULE nt = GetModuleHandleA("ntdll.dll");

		if (nt) {
			g_real_rtlfree = (BOOLEAN(NTAPI *)(PVOID, ULONG, PVOID))(void *)
				GetProcAddress(nt, "RtlFreeHeap");
			g_real_rtlrealloc = (PVOID(NTAPI *)(PVOID, ULONG, PVOID, SIZE_T))(
				void *)GetProcAddress(nt, "RtlReAllocateHeap");
			if (g_real_rtlfree)
				ss_hook_note("iat", "RtlFreeHeap", (void *)g_real_rtlfree,
					     patch_every_module((void *)g_real_rtlfree,
								(void *)hook_rtlfree));
			if (g_real_rtlrealloc)
				ss_hook_note("iat", "RtlReAllocateHeap",
					     (void *)g_real_rtlrealloc,
					     patch_every_module((void *)g_real_rtlrealloc,
								(void *)hook_rtlrealloc));
		}
	}

	if (k32 && !g_real_terminate) {
		HMODULE crt = game_crt_module();
		g_real_terminate = (BOOL(WINAPI *)(HANDLE, UINT))(void *)GetProcAddress(
			k32, "TerminateProcess");
		g_real_exitprocess =
			(void(WINAPI *)(UINT))(void *)GetProcAddress(k32, "ExitProcess");
		if (g_real_terminate)
			ss_hook_note("iat", "TerminateProcess", (void *)g_real_terminate,
				     patch_every_module((void *)g_real_terminate,
							(void *)hook_terminate));
		if (g_real_exitprocess)
			ss_hook_note("iat", "ExitProcess", (void *)g_real_exitprocess,
				     patch_every_module((void *)g_real_exitprocess,
							(void *)hook_exitprocess));
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
				ss_hook_note("iat", "exit", (void *)g_real_exit,
					     patch_every_module((void *)g_real_exit,
								(void *)hook_exit));
			if (g_real_uexit)
				ss_hook_note("iat", "_exit", (void *)g_real_uexit,
					     patch_every_module((void *)g_real_uexit,
								(void *)hook_uexit));
			if (g_real_amsg)
				ss_hook_note("iat", "_amsg_exit", (void *)g_real_amsg,
					     patch_every_module((void *)g_real_amsg,
								(void *)hook_amsg));
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

/* The clock and event hooks for one of the game's own modules, by import name.
 *
 * savestate_hooks_install patches the executable, which is the game in
 * Rabi-Ribi and a launcher stub in Haydee: there the game is haydee.dll and
 * game.dll, which create their worker events and read the clock themselves, so
 * "events: 0 tracked" and a clock that never rewound were both true no matter
 * what the knobs said. gameheap's import mode already visits every game module
 * as it loads, before its initialisers run, so it asks here for what to put in
 * each slot. Resolved lazily because that can be at DLL attach, long before
 * savestate_hooks_install. Rewinding stays behind the same knobs. */
void *savestate_game_import_hook(const char *fn)
{
	HMODULE k32 = GetModuleHandleA("kernel32.dll");

	if (!g_events)
		g_events = (EventTrack *)VirtualAlloc(NULL, sizeof(EventTrack),
						      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!lstrcmpA(fn, "QueryPerformanceCounter")) {
		if (!g_real_qpc && k32)
			g_real_qpc = (BOOL(WINAPI *)(LARGE_INTEGER *))(void *)GetProcAddress(
				k32, "QueryPerformanceCounter");
		return g_real_qpc ? (void *)hook_qpc : NULL;
	}
	if (!lstrcmpA(fn, "GetTickCount")) {
		if (!g_real_tick && k32)
			g_real_tick = (DWORD(WINAPI *)(void))(void *)GetProcAddress(
				k32, "GetTickCount");
		return g_real_tick ? (void *)hook_tick : NULL;
	}
	if (!lstrcmpA(fn, "timeGetTime")) {
		HMODULE mm = GetModuleHandleA("winmm.dll");

		if (!g_real_tgt && mm)
			g_real_tgt = (DWORD(WINAPI *)(void))(void *)GetProcAddress(mm,
										  "timeGetTime");
		return g_real_tgt ? (void *)hook_tgt : NULL;
	}
	if (!g_events)
		return NULL;
	if (!lstrcmpA(fn, "CreateEventA")) {
		if (!g_real_createevent && k32)
			g_real_createevent = (HANDLE(WINAPI *)(LPSECURITY_ATTRIBUTES, BOOL,
							       BOOL, LPCSTR))(void *)
				GetProcAddress(k32, "CreateEventA");
		return g_real_createevent ? (void *)hook_createevent : NULL;
	}
	if (!lstrcmpA(fn, "CreateEventW")) {
		if (!g_real_createeventw && k32)
			g_real_createeventw = (HANDLE(WINAPI *)(LPSECURITY_ATTRIBUTES, BOOL,
								BOOL, LPCWSTR))(void *)
				GetProcAddress(k32, "CreateEventW");
		return g_real_createeventw ? (void *)hook_createeventw : NULL;
	}
	if (!lstrcmpA(fn, "CreateMutexA"))
		return (void *)hook_createmutex;
	if (!lstrcmpA(fn, "CreateThread")) {
		if (!g_real_createthread && k32)
			g_real_createthread = (HANDLE(WINAPI *)(LPSECURITY_ATTRIBUTES, SIZE_T,
								LPTHREAD_START_ROUTINE, LPVOID,
								DWORD, LPDWORD))(void *)
				GetProcAddress(k32, "CreateThread");
		return g_real_createthread ? (void *)hook_createthread : NULL;
	}
	return NULL;
}

enum { RECLAIM_OFF = 0, RECLAIM_GROW, RECLAIM_ALL };

/* Bumped whenever Slot's layout or meaning changes. A .meta file is a raw Slot,
 * so an old one is refused by size already; the number says why. */
#define SS_SLOT_FMT 12
#define SS_MAX_WND 16
#define SS_BND_N 5
#define SS_BND_W 8
#define SS_MAX_STREAMS 32

/* See savestate_follow_save_range. Static storage in our image, which is held
 * in the present, so it survives every restore. */
#define SS_MAX_FOLLOW 4
static uintptr_t g_follow_lo[SS_MAX_FOLLOW], g_follow_hi[SS_MAX_FOLLOW];
static int g_nfollow;

/* Which regions of the last loaded slot were actually written, for
 * savestate_addr_restored. Asked after the load, so the load must not rewind them. */
static unsigned char g_reg_wr[65536] SS_PRESENT;
static const struct Slot *g_last_loaded SS_PRESENT;

#if defined(_M_IX86) || defined(__i386__)
static int follows_save(uintptr_t p)
{
	int i;

	for (i = 0; i < g_nfollow; i++)
		if (p >= g_follow_lo[i] && p < g_follow_hi[i])
			return 1;
	return 0;
}
#endif

struct Slot {
	int valid;
	unsigned fmt;
	/* Boundary samples: rewound words pointing into a heap left in the
	 * present, with the bytes they pointed at. A cross-launch load looks for
	 * those bytes again to measure where each target went. */
	int nbnd;
	struct {
		uintptr_t holder, target, heap;
		char heap_name[32];
		uintptr_t fp[SS_BND_W];
		unsigned ptrmask; /* bit w: fp[w] was a non-image pointer, ignored on match */
	} bnd[SS_BND_N];
	/* Who took this save. A load classifies itself against it - same process,
	 * same boot in a new process, or a new boot - and in the same process any
	 * module base that looks moved is an identity bug, not a move. */
	DWORD save_pid;
	unsigned long long save_created; /* process creation, FILETIME units */
	unsigned long long save_boot;    /* system boot, FILETIME units */
	LONG cs_seq; /* critical sections initialised up to here exist in this save */
	LONG gh_seq; /* game reservations made up to here exist in this save */
	uintptr_t save_peb;
	uintptr_t save_ctl;
	/* The cookie EncodePointer used in the saving process. */
	ULONG save_ptr_cookie;
	/* The process's top-level windows. A handle is not a pointer, so nothing
	 * else relocates the game's copy of it, and a message loop filtered on the
	 * old one never sees this launch's window. */
	int nwnd;
	uintptr_t wnd[SS_MAX_WND];
	char wnd_cls[SS_MAX_WND][64];
	/* The window's own DC where its class is CS_OWNDC, else 0. A GL game keeps
	 * that DC for life and hands it to SwapBuffers, and it is a number like the
	 * window handle: from another launch it names another DC or none. */
	uintptr_t wnd_dc[SS_MAX_WND];
	/* The witness snapshot: where the player was when THIS slot was saved, so a
	 * restore of this slot checks against its own world rather than whatever was
	 * saved most recently. Persistent per-slot. The transient wit_reg/wit_blk/
	 * wit_why stay in Control because they describe the operation in flight, not
	 * the stored save; do_load copies these five back into the active witness
	 * before room_check so every g_ctl->wit_* reader sees the right save. */
	uintptr_t wit_ent;
	float wit_x, wit_y;
	unsigned wit_map;
	int wit_valid;
	int nregs;
	int nthreads;
	unsigned long long bytes;
	HANDLE sect;
	int nids;
	DWORD ids[SS_MAX_THREADS];
	PVOID starts[SS_MAX_THREADS];
	int nfiles;
	int nevents;
	/* Which modules were mapped, and where. The thread set is already checked
	 * across a restore; the module set was not, and it diverges for the same
	 * reason - Steam's helpers load and unload on their own schedule. We rewind
	 * the game's memory but the loader's list lives in ntdll, which stays in the
	 * present, so the two can end up disagreeing about which modules exist. */
	int nmods;
	uintptr_t mod_lo[SS_MAX_MODS], mod_hi[SS_MAX_MODS];
	char mod_name[SS_MAX_MODS][32];
	char mod_path[SS_MAX_MODS][MAX_PATH];
	DWORD mod_stamp[SS_MAX_MODS], mod_isize[SS_MAX_MODS];
	uintptr_t mod_cookie[SS_MAX_MODS];
	/* Base anchors for a relocatable (cross-session) restore, alongside the
	 * module set above. These are the non-module owned heaps (arena, LAA region)
	 * whose block offsets are deterministic, so a saved pointer base+offset can
	 * become new_base+offset when the arena lands elsewhere next session. */
	int nanchor;
	uintptr_t anchor_base[4], anchor_size[4];
	EventState events[SS_MAX_EVENTS];
	TimeBase clock;
	FileState files[SS_MAX_FILES];
	/* The game's open C streams, from gameheap's fopen hooks. Size and write
	 * time because the game is modded by swapping files: a stream reopened by
	 * path in another launch may not be the file it was reading. */
	int nstreams;
	uintptr_t stream_f[SS_MAX_STREAMS];
	WCHAR stream_path[SS_MAX_STREAMS][MAX_PATH];
	char stream_mode[SS_MAX_STREAMS][8];
	long long stream_pos[SS_MAX_STREAMS];
	unsigned long long stream_size[SS_MAX_STREAMS], stream_mtime[SS_MAX_STREAMS];
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
static DWORD xs_new_tid(DWORD old_tid);
static int xs_foreign_load(void);

static int cs_owner_resumes(Slot *s, DWORD tid)
{
	int i;

	if (!tid)
		return 0;
	/* From another process a saved id names nobody here, whatever the slot's
	 * thread list says; only a transplanted owner comes back. */
	if (xs_foreign_load())
		return 0;
	for (i = 0; i < s->nthreads; i++)
		if (s->threads[i].tid == tid)
			return 1;
	return 0;
}

/* A section's DebugInfo is a record threaded onto ntdll's list of every
 * section in the process. The list and its records stay in the present (ntdll
 * and the process heap); the section is rewound. A section deleted after the
 * save comes back pointing at a record ntdll has since unlinked and freed - and
 * often handed to another section - so deleting it again unlinks a stranger
 * and corrupts the list, and the next InitializeCriticalSection fast-fails.
 * Returns 1 when the pointer was not this section's record on the list and
 * was replaced by -1, ntdll's own "no record". */
static int cs_debug_fix(CRITICAL_SECTION *cs)
{
	PRTL_CRITICAL_SECTION_DEBUG d = cs->DebugInfo;
	LIST_ENTRY *me, *f, *b;

	if (!d || d == (PRTL_CRITICAL_SECTION_DEBUG)(LONG_PTR)-1)
		return 0;
	if (ss_readable((uintptr_t)d, sizeof(*d)) && d->CriticalSection == cs) {
		me = &d->ProcessLocksList;
		f = me->Flink;
		b = me->Blink;
		if (ss_readable((uintptr_t)f, sizeof(*f)) && ss_readable((uintptr_t)b, sizeof(*b)) &&
		    f->Blink == me && b->Flink == me)
			return 0;
	}
	cs->DebugInfo = (PRTL_CRITICAL_SECTION_DEBUG)(LONG_PTR)-1;
	return 1;
}

#define SS_OWN_CS 16
static CRITICAL_SECTION *g_own_cs[SS_OWN_CS] SS_PRESENT;
static int g_own_cs_n SS_PRESENT;

void savestate_own_cs(CRITICAL_SECTION *cs)
{
	int i;

	for (i = 0; i < g_own_cs_n; i++)
		if (g_own_cs[i] == cs)
			return;
	if (g_own_cs_n < SS_OWN_CS)
		g_own_cs[g_own_cs_n++] = cs;
}

/* Our own locks after a load from another launch. Rewound with everything
 * else, so their held state matches the threads; the debug link and the wait
 * event are the old process's. An owner that did not come back as a live
 * thread here is gone, so the lock is freed. */
static void cs_own_fix(Slot *s)
{
	int i, debug = 0, sem = 0, freed = 0;

	if (!xs_foreign_load())
		return;
	for (i = 0; i < g_own_cs_n; i++) {
		CRITICAL_SECTION *cs = g_own_cs[i];
		DWORD owner = (DWORD)(DWORD_PTR)cs->OwningThread;

		debug += cs_debug_fix(cs);
		if (cs->LockSemaphore && cs->LockSemaphore != INVALID_HANDLE_VALUE) {
			cs->LockSemaphore = NULL;
			sem++;
		}
		if (!owner && cs->LockCount == -1)
			continue;
		if (xs_new_tid(owner)) {
			cs->OwningThread = (HANDLE)(DWORD_PTR)xs_new_tid(owner);
			continue;
		}
		(void)s;
		cs->LockCount = -1;
		cs->RecursionCount = 0;
		cs->OwningThread = NULL;
		freed++;
	}
	ss_log("  locks: %d of this DLL's own section(s) checked - %d debug link(s) cut, %d "
	       "wait event(s) cleared, %d freed from an owner that did not come back\n",
	       g_own_cs_n, debug, sem, freed);
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
	g_cs->unborn = 0;
	g_cs->revived = 0;
	g_cs->debug_cut = 0;
	g_cs->sem_cut = 0;
	for (i = 0; i < SS_MAX_CS; i++) {
		CsRec *e = &g_cs->t[i];
		CRITICAL_SECTION *cs = e->cs;
		MEMORY_BASIC_INFORMATION mbi;
		DWORD owner;

		if (!cs)
			continue;
		if (!e->live) {
			if (s->cs_seq && e->born <= s->cs_seq && e->died > s->cs_seq &&
			    cs_in_restored(s, cs)) {
				e->live = 1;
				g_cs->revived++;
			} else {
				continue;
			}
		}
		/* Not a lock in the memory we just wrote, so not ours to touch, and
		 * no longer a lock at all: the timeline that created it is gone. Left
		 * live, a later save would count it as a real section and its restore
		 * would "repair" whatever object sits there now - Haydee's second
		 * save-and-restore wrote 13 free sections over its game heap that way.
		 * If the game initialises it again, cs_note records it afresh. */
		if (s->cs_seq && e->born > s->cs_seq && cs_in_restored(s, cs)) {
			if (e->prev_born && e->prev_born <= s->cs_seq && e->died > s->cs_seq) {
				e->born = e->prev_born;
				e->prev_born = 0;
				g_cs->revived++;
			} else {
				e->live = 0;
				g_cs->unborn++;
				continue;
			}
		}
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
		g_cs->debug_cut += cs_debug_fix(cs);
		/* The wait event is a handle, and from another process it names
		 * nothing here - the first contended wait raises INVALID_HANDLE inside
		 * ntdll. NULL makes ntdll create one again when it is next needed. */
		if (xs_foreign_load() && cs->LockSemaphore) {
			cs->LockSemaphore = NULL;
			g_cs->sem_cut++;
		}
		owner = (DWORD)(DWORD_PTR)cs->OwningThread;
		if (!owner && cs->LockCount == -1)
			continue;
		g_cs->held++;
		/* The owner's context was transplanted onto a live thread, which will
		 * leave the section under its own id. Re-entry compares the id, so it
		 * has to be the live one. */
		if (xs_new_tid(owner)) {
			cs->OwningThread = (HANDLE)(DWORD_PTR)xs_new_tid(owner);
			g_cs->kept++;
			continue;
		}
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
	if (g_cs->unborn)
		ss_log("  locks: %ld section(s) created after this save left alone and dropped - their "
		       "memory now holds what was there at the save, not a lock\n",
		       (long)g_cs->unborn);
	if (g_cs->revived)
		ss_log("  locks: %ld section(s) deleted after this save are live again - the "
		       "restore put them back\n",
		       (long)g_cs->revived);
	if (g_cs->debug_cut)
		ss_log("  locks: %ld section(s) came back pointing at a debug record ntdll no "
		       "longer has on its list for them - set to none, so deleting them cannot "
		       "unlink someone else's\n",
		       (long)g_cs->debug_cut);
	if (g_cs->sem_cut)
		ss_log("  locks: %ld section(s) came back with another process's wait event - "
		       "cleared, so ntdll makes a fresh one on the next contended wait\n",
		       (long)g_cs->sem_cut);
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
/* Reasons a saved block goes unwritten, in the order the restore tests them.
 * WIT_UNSEEN is the absence of a verdict: the allocation never reached the loop
 * at all, which means the block map did not match it. */
enum {
	WIT_UNSEEN,
	WIT_WRITTEN,
	WIT_HOMELESS,
	WIT_HELD,
	WIT_VTAB,
	WIT_NOTOURS,
	WIT_VETOED,
	WIT_RECYCLED,
	WIT_COPYFAIL
};

/* One saved thread handed to a live one by a cross-session transplant. The live
 * part of the saved stack, [old_sp, old_hi), now sits at [old_sp + delta,
 * old_hi + delta) on the live thread's own stack. */
typedef struct XsMap {
	DWORD old_tid, new_tid;
	uintptr_t old_sp, old_hi;
	intptr_t delta;
	/* What the live thread is meant to resume with, kept so the check before
	 * the release can compare against it and put it back. state: 0 never set,
	 * 1 set, 2 refused. live_seh is the TEB's own chain head from before the
	 * transplant, which no snapshot holds, for the rollback to put back. */
	int state;
	DWORD live_seh;
	uintptr_t old_teb, new_teb;
	CONTEXT want;
} XsMap;

/* A hidden fifth slot for the rollback point taken before a load from another
 * process. Never written to a slot file and never reachable from a hotkey. */
#define SS_SCRATCH SAVESTATE_SLOTS

typedef struct Control {
	volatile LONG request;
	volatile LONG busy;
	volatile LONG result;
	volatile LONG slot;
	uintptr_t helper_lo, helper_hi;
	LONG nex, nex_fixed;
	uintptr_t ex_lo[SS_MAX_EXCL], ex_hi[SS_MAX_EXCL];
	const char *ex_why[SS_MAX_EXCL];
	void *kept[SS_KEPT];
	DWORD ids[SS_MAX_THREADS];
	PVOID starts[SS_MAX_THREADS];
	HANDLE handles[SS_MAX_THREADS];
	int nids;
	DWORD req_tid;
	/* Set by the transplant for the load in flight, cleared at the start of
	 * every load. */
	int nxs;
	XsMap xs[SS_MAX_THREADS];
	/* Saved handle values another object holds here, and the stand-in the
	 * game's calls on them are steered to. Replaced by each foreign load. */
	volatile LONG nhx;
	uintptr_t hx_old[SS_MAX_EVENTS];
	HANDLE hx_new[SS_MAX_EVENTS];
	/* Saved threads paired with a live one but left running as they are. */
	int nkeep;
	DWORD keep_old[SS_MAX_THREADS], keep_new[SS_MAX_THREADS];
	uintptr_t xs_peb_old, xs_peb_new;
	uintptr_t xs_ctl_old;
	/* This process's windows, listed as a load begins. user32 cannot be asked
	 * after the write: it faults on the 64-bit side, past every handler. */
	int nlwnd;
	uintptr_t lwnd[SS_MAX_WND];
	char lwnd_cls[SS_MAX_WND][64];
	/* Rollback. rollback_ready: a scratch save exists for the load in flight.
	 * held: threads a failed load left suspended, released by the scratch
	 * load once it has taken its own suspension. */
	int rollback_ready;
	int held_n;
	HANDLE held[SS_MAX_THREADS];
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
	int last_gone;
	int last_recycled;
	int listed;

	/* Soak driver.
	 *
	 * Here rather than in ordinary statics because the wrapper's own image
	 * rewinds with the game, so a trial counter kept in .data would be wound
	 * back by the very restore it is trying to count and the ladder would
	 * repeat one rung forever. Three separate bugs in this project came from
	 * putting bookkeeping somewhere that rewinds; Control is excluded, so it
	 * is the one safe home. */
	int soak_state;
	int soak_frame;	  /* frames elapsed in the current phase */
	int soak_trial;
	int soak_dwell;	  /* frames between this trial's save and its restore */
	int soak_settle;  /* frames to let the game breathe before a save */
	int soak_watch;	  /* frames to watch after a restore before calling it alive */
	int soak_max;	  /* stop once the dwell would exceed this */
	int soak_ladder;  /* dwell multiplier per rung */
	int soak_survived[24]; /* the curve, so a truncated log still tells the story */
	int soak_dwells[24];
	int soak_rows;

	/* heap_infra confiscating memory that belongs to a heap we rewind */
	int infra_clash;
	int infra_kept;
	uintptr_t infra_first_at;
	HANDLE infra_first_heap;
	unsigned long long infra_clash_bytes;
	/* Distinct spans, because refusing to exclude one means region_excluded
	 * never marks it, so the next list head over finds it again and the count
	 * triples. The first run reported "3 span(s), 3.00 MB" for what was one
	 * megabyte seen three times, which is the kind of inflation that turns a
	 * measurement into a talking point. */
	uintptr_t infra_seen[16];
	int infra_nseen;

	/* the small savestate: a span of the player entity, not just x and y */
	unsigned char pos_buf[0x400];
	int pos_span;
	unsigned pos_map;
	int pos_valid;
	uintptr_t pos_ent;
	unsigned watch_map;
	int watch_seen;
	unsigned key_down[8];
	int key_seeded;
	unsigned long long diff_same, diff_wrote;
	uintptr_t wit_ent;
	float wit_x, wit_y;
	unsigned wit_map;
	int wit_valid;
	int wit_reg;
	uintptr_t wit_blk;
	/* See g_wit_why for what each of these reads as.
	 *
	 * Which rule decided her block, rather than only whether one did. "Not in
	 * the block map" covered five different outcomes with one sentence, and
	 * they call for opposite fixes: a block nobody enumerated is an
	 * enumeration problem, a block the ownership closure declined is a filter
	 * problem, and a block a running thread was pointing at is neither. */
	int wit_why;
	/* Carried across the restore by hand, for state the snapshot does not
	 * cover. Kept apart from pos_buf so that F11 and F5 cannot overwrite
	 * each other's mark. */
	/* What the last save actually captured. The object map needs to answer
	 * "will a restore reach this address", and the only truthful source for
	 * that is the region list the save was built from - asking which heap an
	 * address belongs to answers a different question and gets it wrong,
	 * because a heap's later segments are nowhere near its handle. */
	uintptr_t cap_base[1024];
	uintptr_t cap_size[1024];
	int cap_n;
	/* The process heap roster as it stood in the present, sampled before the
	 * restore writes anything. */
	unsigned roster_n;
	uintptr_t roster_arr;
	uintptr_t roster_ent[64];
	int roster_valid;
	unsigned char carry_buf[0x400];
	int carry_span;
	int carry_valid;
	uintptr_t carry_ent;
	unsigned carry_map;
	DWORD anchor_tick;
	DWORD load_tick; /* GetTickCount at the most recent restore, for elapsed-since */
#define SS_FMT_SLOTS 64
	int nheaps, heaps_listed;
	HANDLE heap_h[SS_MAX_HEAPS];
	int seg_full;
	uintptr_t heap_lo[SS_MAX_HEAPS], heap_hi[SS_MAX_HEAPS];
	char heap_ours[SS_MAX_HEAPS];
	char heap_name[SS_MAX_HEAPS][32];
	int nmods;
	uintptr_t mod_lo[SS_MAX_MODS], mod_hi[SS_MAX_MODS];
	char mod_rewound[SS_MAX_MODS];
	char mod_name[SS_MAX_MODS][32];
	/* Identity, because a base name is not one: our pinned d3d11.dll and
	 * dxgi.dll share their names with the system copies the GPU probe loads.
	 * Path plus the PE header's link stamp and image size tells them apart,
	 * and tells a rebuilt module from the one a slot was saved against. */
	char mod_path[SS_MAX_MODS][MAX_PATH];
	DWORD mod_stamp[SS_MAX_MODS], mod_isize[SS_MAX_MODS];
	/* Where the current load stands relative to its save. Here and not in a
	 * static, because the restore winds our own image back to the save and a
	 * static set before the copy reads as its save-time value after it. */
	int load_prov;
	/* Set by any load from another process and never cleared: a later
	 * same-process load can land on a save taken after it. */
	int foreign_seen;
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

	Slot slots[SAVESTATE_SLOTS + 1];
	/* Settings that differ between the loaded save and this launch, one line,
	 * so every fault report after the load says it first. */
	char cfg_diff[512];
	/* The stall watchdog's thread, kept out of every save. */
	DWORD wd_tid;
	/* Stand-in threads of ours, started before a load freezes anything, one
	 * per saved game thread with no live thread in its role. A load moves the
	 * saved thread onto one; a stand-in still idle is kept out of saves and
	 * told to exit when the load is over. 0 once taken or released. */
	DWORD spare_tid[100];
	volatile LONG spare_ready[100];
	/* Stand-ins the current load took, ended if it rolls back: the scratch
	 * save it returns to predates them. */
	DWORD spare_taken[100];
	int nspare_taken;
	/* What the slot's manifest says the load will find: rewound threads with
	 * no live thread in their role. -1 when the slot has no manifest. */
	int mf_short;
} Control;

static Control *g_ctl SS_PRESENT;

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
/* Four kilobytes was enough when the file was a list of settings. It is a list
 * of settings and the reasoning behind each one, which is the right way to keep
 * them, and it crossed the line at 4259 bytes - so D3D9SW_GAMEHEAP=1, the last
 * line in the file, was read as unset and an entire feature sat switched off
 * while the file plainly asked for it. The read was silently short and nothing
 * anywhere said so. */
#define SS_CFG_MAX (32u * 1024u)
static char *g_cfg SS_PRESENT;
static int g_cfg_len;
static int g_cfg_tried;
/* Bytes of the file that did not fit. Reported at the session header rather than
 * here, because cfg_load runs long before there is a log to write to. */
static unsigned g_cfg_over;

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
	/* Reused rather than reallocated, because this is now reached more than
	 * once: the watch knob clears g_cfg_tried to re-read the file mid-session,
	 * and a fresh reservation per reload would leak one buffer each time. */
	if (!g_cfg)
		g_cfg = (char *)VirtualAlloc(NULL, SS_CFG_MAX, MEM_COMMIT | MEM_RESERVE,
					     PAGE_READWRITE);
	if (!g_cfg)
		return;
	h = CreateFileA("d3d9_sw.cfg", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;
	if (ReadFile(h, g_cfg, SS_CFG_MAX, &got, NULL)) {
		DWORD hi = 0, size = GetFileSize(h, &hi);

		g_cfg_len = (int)got;
		if (hi || size > got)
			g_cfg_over = hi ? 0xFFFFFFFFu : size - got;
	}
	CloseHandle(h);
}

unsigned savestate_getenv(const char *name, char *buf, unsigned cap)
{
	return (unsigned)ss_getenv(name, buf, (DWORD)cap);
}

/* True when the main executable has this file name.
 *
 * Everything in this wrapper that came out of a disassembly is a fact about one
 * binary, and until a harness existed nothing checked that the binary was the
 * one in question. Two places were doing it: the decoder patch, which finds its
 * instruction by scanning for nine bytes and will match a coincidence, and the
 * dsound cursor report, which reads exe+0x50E18C outright. Both were pointed at
 * a 363 KB test executable and both misbehaved - the first rewrote a branch, the
 * second faulted reading a megabyte past the end of the image.
 *
 * Deliberately compares the base name only. The game is installed wherever Steam
 * put it, and a full-path comparison would be a second thing to get wrong. */
int savestate_host_is(const char *exe_name)
{
	char path[MAX_PATH];
	HMODULE exe = GetModuleHandleA(NULL);
	DWORD len = exe ? GetModuleFileNameA(exe, path, sizeof(path)) : 0;
	const char *base = path;
	DWORD i;

	if (!len || len >= sizeof(path))
		return 0;
	for (i = 0; i < len; i++)
		if (path[i] == '\\' || path[i] == '/')
			base = path + i + 1;
	return lstrcmpiA(base, exe_name) == 0;
}

/* One line into the engine's log from outside this file. The dsound hook needs
 * to say what it did, and its findings belong beside the restore they explain
 * rather than in a log of their own. */
void savestate_log_line(const char *s)
{
	ss_log("%s", s);
}

/* Every environment lookup answered once and then remembered.
 *
 * GetEnvironmentVariableA allocates. It converts the name through
 * RtlAnsiStringToUnicodeString, which calls RtlAllocateHeap on the process
 * heap - the same heap we lock, walk, freeze the process around and then write
 * saved blocks into. Reading a knob inside that window means mutating the thing
 * we are in the middle of restoring, and it is not theoretical: a restore died
 * in FAST_FAIL_CORRUPT_LIST_ENTRY inside RtlpLowFragHeapAllocateFromZone with
 * our own thread on the stack, four frames under GetEnvironmentVariableA,
 * because the low fragmentation heap's zone list was half-updated on a thread
 * we had suspended. HeapLock does not serialise that path - the LFH fast path
 * is lock-free - so freezing threads cannot make it safe. Not allocating can.
 *
 * cfg_load already got this right and says so in its own comment; this is the
 * one call that was still reaching outside. The file half needs no allocation
 * at all: it parses a buffer we own, so it stays available in the window.
 *
 * A name never looked up before the freeze falls through to the file rather
 * than reaching for the environment, which is the safe way to be wrong - the
 * file is the route that actually gets used, and the environment is documented
 * above as the one that cannot be relied on. */
#define SS_ENV_CAP 128

static struct {
	char name[48];
	char val[32];
	DWORD len;
} g_env_memo[SS_ENV_CAP];
static int g_env_memo_n;
static int g_env_sealed;
static unsigned g_seal_tick;

/* Take the whole environment once, then never ask the OS again.
 *
 * Memoising per name was not enough. It only covered the window where threads
 * were frozen, and the call that actually killed a session was an ordinary
 * knob read on a wrapper worker thread during play: GetEnvironmentVariableA ->
 * RtlAnsiStringToUnicodeString -> RtlAllocateHeap, landing in the low
 * fragmentation front end of the process heap. That is the one heap we lock,
 * walk, snapshot and write saved blocks into, and it is the only one of the
 * eight that ever breaks.
 *
 * Whether our allocations are what break it is still unproven. This exists so
 * the question can be settled rather than argued: after the seal the wrapper
 * makes no heap allocation of its own on that heap for a knob read, so if it
 * still goes inconsistent, it was never us.
 *
 * Enumerating beats a name list because the list would have to be maintained.
 * D3D11SW_* knobs reach us only by the D3D11 wrapper pushing its config into
 * the environment, and a name missing from a hand-written list would read as
 * unset forever - which is the exact failure that cost three runs earlier
 * today, rebuilt in a new place. */
static void env_seal(void)
{
	LPCH blk, p;
	int took = 0;

	if (g_env_sealed)
		return;
	blk = GetEnvironmentStringsA();
	if (blk) {
		for (p = blk; *p; p += lstrlenA(p) + 1) {
			const char *eq;
			int nlen, vlen, k;

			if (!(p[0] == 'D' && p[1] == '3' && p[2] == 'D'))
				continue;
			for (eq = p; *eq && *eq != '='; eq++)
				;
			if (*eq != '=')
				continue;
			nlen = (int)(eq - p);
			vlen = lstrlenA(eq + 1);
			if (nlen <= 0 || nlen >= (int)sizeof(g_env_memo[0].name) ||
			    vlen <= 0 || vlen >= (int)sizeof(g_env_memo[0].val) ||
			    g_env_memo_n >= SS_ENV_CAP)
				continue;
			/* Already answered once; that answer stands. */
			for (k = 0; k < g_env_memo_n; k++)
				if (!lstrcmpiA(g_env_memo[k].name, p))
					break;
			if (k < g_env_memo_n)
				continue;
			k = g_env_memo_n++;
			lstrcpynA(g_env_memo[k].name, p, nlen + 1);
			memcpy(g_env_memo[k].val, eq + 1, vlen + 1);
			g_env_memo[k].len = vlen;
			took++;
		}
		FreeEnvironmentStringsA(blk);
	}
	g_env_sealed = 1;
	ss_log("env: sealed with %d D3D knob(s) held in %d memo slot(s). No knob read "
	       "will touch the process heap again this session\n",
	       took, g_env_memo_n);
}
static int g_env_frozen;

static DWORD env_once(const char *name, char *buf, DWORD cap)
{
	DWORD n;
	int i;

	for (i = 0; i < g_env_memo_n; i++) {
		if (lstrcmpiA(g_env_memo[i].name, name) != 0)
			continue;
		if (!g_env_memo[i].len || g_env_memo[i].len >= cap)
			return 0;
		memcpy(buf, g_env_memo[i].val, g_env_memo[i].len + 1);
		return g_env_memo[i].len;
	}
	if (g_env_frozen || g_env_sealed)
		return 0;
	n = GetEnvironmentVariableA(name, buf, cap);
	/* Only answers are remembered, never absences.
	 *
	 * Caching "unset" looks like the same optimisation and is a different bug
	 * entirely, because the environment here is not static. The D3D11 wrapper
	 * reads d3d11_sw.cfg at startup and pushes every key into the process
	 * environment with SetEnvironmentVariableA, and the savestate engine's own
	 * file fallback names d3d9_sw.cfg, which does not exist beside this game.
	 * So every knob arrives late and by this route. Remembering a miss from
	 * before that push made the answer permanent, and D3D9SW_HEAPBLOCKS read as
	 * unset for the rest of the process.
	 *
	 * It cost three runs that looked like results: block restore was off for
	 * all of them, the logs said "0 by block" in a line nobody was reading, and
	 * a five-restore run and a one-restore run got compared as if they were
	 * testing the thing that had been switched off. A miss costs one more call
	 * next time, which is the correct price. */
	if (n > 0 && n < cap && n < sizeof(g_env_memo[0].val) &&
	    g_env_memo_n < SS_ENV_CAP) {
		int k = g_env_memo_n++;

		lstrcpynA(g_env_memo[k].name, name, sizeof(g_env_memo[k].name));
		memcpy(g_env_memo[k].val, buf, n + 1);
		g_env_memo[k].len = n;
	}
	return n;
}

/* The d3d9_sw.cfg half on its own.
 *
 * Split out of ss_getenv because the session header has to be able to ask what
 * the file says even when the environment has already answered. Without that, a
 * knob the file sets and the environment overrides looks exactly like a knob the
 * file never mentions, and the override cannot be reported - only inferred from
 * behaviour, months later, by someone reading a log tail. */
static DWORD cfg_lookup(const char *name, char *buf, DWORD cap)
{
	int i, nlen;

	if (cap == 0)
		return 0;
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

/* Environment first, then the file. Returns the length written, 0 if unset. */
static DWORD ss_getenv(const char *name, char *buf, DWORD cap)
{
	DWORD n;

	if (cap == 0)
		return 0;
	n = env_once(name, buf, cap);
	if (n > 0 && n < cap)
		return n;
	return cfg_lookup(name, buf, cap);
}

/* Every knob the engine reads, in one place, so the freeze can warm all of them
 * and the session header can print all of them. */
static const char *const g_knobs[] = {
	"D3D9SW_REWIND_THREADS",  "D3D9SW_REWIND_NEWTHREADS",
	"D3D9SW_REWIND_CLOCK",	  "D3D9SW_REWIND_EVENTS",
	"D3D9SW_REWIND_GAMEHEAP", "D3D9SW_REWIND_SWHEAP",
	"D3D9SW_REWIND_ALLHEAPS", "D3D9SW_REWIND_RECLAIM",
	"D3D9SW_REWIND_GUARD",	  "D3D9SW_LOCKS",
	"D3D9SW_FNTAB",		  "D3D9SW_HEAPCHECK",
	"D3D9SW_VERIFY",	  "D3D9SW_DRIFT",
	"D3D9SW_INVENTORY",
	"D3D9SW_SETTLE",	  "D3D9SW_REACH_AUDIT",
	"D3D9SW_REACH_GRAN",	  "D3D9SW_HEAPBLOCKS",
	"D3D9SW_BLKOWNER",	  "D3D9SW_SYSROOT",
	"D3D9SW_CLOBBER",	  "D3D9SW_CATCH",
	"D3D9SW_SKIPREG",	  "D3D9SW_SCRIBBLE",
	"D3D9SW_DERIVED",	  "D3D9SW_FREEZE",
	"D3D9SW_PROBE",           "D3D9SW_WATCH",
	"D3D9SW_NOTHREAD",        "D3D9SW_RUNAWAY",
	"D3D9SW_REWIND_TEXTINPUT", "D3D9SW_HELDVETO",
	"D3D9SW_VTABVETO",	  "D3D9SW_CROSSWORLD",
	"D3D9SW_SAVE_AT",
	"D3D9SW_DSSEEK",
	"D3D9SW_DECPATCH",	  "D3D9SW_LFHHOLD",
	"D3D9SW_RECYCLED",	  "D3D9SW_GAMEHEAP",
	"D3D9SW_DIFFWRITE",	  "D3D9SW_POS_SPAN",
	"D3D9SW_SLOTFILE",
	"D3D9SW_WATCH_AT",
	"D3D9SW_EXCLSKIP",
	"D3D9SW_WATCH_TRACE",
	"D3D9SW_DELTA",		  "D3D9SW_DELTA_KB",
	"D3D9SW_OBJWATCH",
	/* Read only inside a load, when the environment is frozen: without a warm
	 * memo it reads as unset in any host that does not seal the whole block. */
	"D3D9SW_RELOC",
	/* The game heap's own knobs. They reach the environment through gh_knob,
	 * which calls savestate_getenv like everything else, so leaving them out of
	 * this list did not stop them working - it stopped them being memoised, and
	 * the first read of one is on a free path long after the environment is
	 * declared closed. Seal them here with the rest. */
	"D3D9SW_GHTRACE",	  "D3D9SW_GHPIN",
	"D3D9SW_GHLEDGER",
	"D3D9SW_GHPLACE",
	"D3D9SW_MERGE",
	"D3D9SW_MERGE_TAKE",
	"D3D9SW_MERGE_SITES",
	"D3D9SW_GHSW_OWN",	  "D3D9SW_MERGE_CHECK",
	"D3D9SW_MERGE_RELINK",	  "D3D9SW_MERGE_RELOC",
	"D3D9SW_MERGE_REFOCUS",
	"D3D9SW_GHPIN_MB",	  "D3D9SW_GHBIG_MB",
	"D3D9SW_GHPEEK",	  "D3D9SW_GHVORBIS",
	"D3D9SW_ENTS",		  "D3D9SW_CLOCKPROBE",
	"D3D9SW_KEY_EVERY",	  "D3D9SW_KEY_HOLD",
	"D3D9SW_KEY_FROM",	  "D3D9SW_KEY_VK",
	"D3D9SW_QUIT_AT",	  "D3D9SW_XINPUT",
	"D3D9SW_LOAD_AT",	  "D3D9SW_SAVE_AFTER_LOAD",
	"D3D9SW_REC_GATE", "D3D9SW_LOSE_DEVICE", "D3D9SW_LOAD_AFTER_SAVE", "D3D9SW_MERGE_THREADS",
	"D3D9SW_SAVE_VK",	  "D3D9SW_LOAD_VK",
	"D3D9SW_SLOT_VK",	  "D3D9SW_SWARENA",
	"D3D9SW_SWARENA_MB",	  "D3D9SW_SWARENA_SWEEP",
	"D3D9SW_TEXPROBE",	  "D3D9SW_TEXPACK",
	"D3D9SW_TEXPACK_BASE",	  "D3D9SW_TEXPACK_MB"
};

/* Read every knob into the memo before the environment is closed for business.
 *
 * The freeze exists so that no lookup allocates while the threads are stopped,
 * because GetEnvironmentVariable goes to the process heap and doing that inside
 * a half-updated low-fragmentation zone is a fastfail. Answering nothing was
 * meant to be the safe response to a lookup during that window. It is not: it
 * changes what the engine decides. blk_mode is consulted during the save, gets
 * an empty answer from the environment, falls through to the config file, and
 * concludes block restore is off - while the environment it was not allowed to
 * read says it is on.
 *
 * That cost most of a day. Three runs were logged, compared and reasoned about
 * as measurements of block restore while block restore was switched off, and
 * the switch was thrown by a performance fix in a different function.
 *
 * So warm the cache instead of blinding the reader. Every name is looked up
 * once here, outside the window, and the memo answers identically inside it. */
static void env_prewarm(void)
{
	char v[64];
	int i;

	for (i = 0; i < (int)(sizeof(g_knobs) / sizeof(g_knobs[0])); i++)
		ss_getenv(g_knobs[i], v, sizeof(v));
}

static HANDLE g_helper SS_PRESENT;

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

/* Who held it back, recorded alongside what was held.
 *
 * The list has always known which ranges are excluded and never which rule put
 * them there, which was fine while the answer was "stacks and images". It stopped
 * being fine when the coverage check found a 233 MB region the game writes to
 * every frame, reported it as excluded by us, and left no way to find out which
 * of a dozen call sites made that decision. An exclusion is a deliberate act and
 * it should sign its name. */
static const char *g_ex_why = "unattributed";

static void ss_exclude_as(const char *why, void *p, size_t n)
{
	const char *prev = g_ex_why;

	g_ex_why = why;
	ss_exclude(p, n);
	g_ex_why = prev;
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
	g_ctl->ex_why[g_ctl->nex] = g_ex_why;
	g_ctl->nex++;
}

/* The tag of the first exclusion covering this range, for reporting. Returns a
 * fixed string rather than an index because the caller only ever wants to print
 * it, and the range may be covered by more than one entry. */
static const char *region_why(uintptr_t base, uintptr_t size)
{
	uintptr_t end = base + size;
	int i;

	if (!g_ctl)
		return "no control block";
	for (i = 0; i < g_ctl->nex; i++)
		if (base < g_ctl->ex_hi[i] && end > g_ctl->ex_lo[i])
			return g_ctl->ex_why[i] ? g_ctl->ex_why[i] : "unattributed";
	return "not excluded - some other filter dropped it";
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

/* A short hex + ascii dump of a block, for the fault reporter: the bytes that
 * are actually on the page right now, so the read the game just made is in
 * plaintext instead of guessed from a single word. Guarded row by row with
 * ss_readable so a block ending at a page boundary cannot fault the handler. */
static void ss_dump_bytes(uintptr_t base, unsigned n)
{
	unsigned off;

	if (n > 64)
		n = 64; /* a header and the first few fields is enough to read intent */
	for (off = 0; off < n; off += 16) {
		unsigned row = (n - off < 16) ? (n - off) : 16, k;
		const unsigned char *r = (const unsigned char *)(base + off);
		char line[128], *o = line;

		if (!ss_readable(base + off, row))
			break;
		o += wsprintfA(o, "         +%02X ", off);
		for (k = 0; k < row; k++)
			o += wsprintfA(o, "%02X ", r[k]);
		for (; k < 16; k++) {
			*o++ = ' ';
			*o++ = ' ';
			*o++ = ' ';
		}
		*o++ = '|';
		for (k = 0; k < row; k++)
			*o++ = (r[k] >= 32 && r[k] < 127) ? (char)r[k] : '.';
		*o++ = '|';
		*o = 0;
		ss_raw("%s\n", line);
	}
}

/* Whether a save holds memory from the allocation at alloc_base: slot slotno,
 * or any valid slot when slotno is negative. Asked by gameheap's reservation
 * tracking, so a block the game releases is kept reserved while a restore may
 * still need its address. */
int savestate_alloc_saved(uintptr_t alloc_base, int slotno)
{
	int k, i;

	if (!g_ctl)
		return 0;
	for (k = 0; k < SAVESTATE_SLOTS; k++) {
		const Slot *s = &g_ctl->slots[k];

		if ((slotno >= 0 && k != slotno) || !s->valid)
			continue;
		for (i = 0; i < s->nregs; i++)
			if (s->regs[i].alloc_base == alloc_base)
				return 1;
	}
	return 0;
}

int savestate_any_valid(void)
{
	int k;

	if (!g_ctl)
		return 0;
	for (k = 0; k < SAVESTATE_SLOTS; k++)
		if (g_ctl->slots[k].valid)
			return 1;
	return 0;
}

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
		/* Pointer sized, not always eight bytes. A vtable pointer is four
		 * bytes on x86, and reading eight at the tail of a committed region
		 * whose next page is not committed faults inside the fault handler. */
		uintptr_t w0 = *(const uintptr_t *)v;
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
	/* Birth certificate from the gameheap tag table. The tag lives in our image,
	 * not the block, so a block the restore rewound to zeroes can still name the
	 * call site, size and ordinal it was allocated with - the one thing its own
	 * blanked contents can no longer say. Then dump those contents, so the object
	 * the game just faulted reading is on the page in full rather than one word. */
	{
		unsigned bsite = 0, bsize = 0, bord = 0;
		uintptr_t bbase = 0;
		int w = gameheap_whatis((const void *)v, &bsite, &bsize, &bord, &bbase);

		ss_raw("\n");
		if (w) {
			ss_raw("         born: rabiribi.exe+%X, %u byte(s), ordinal %u%s\n",
			       bsite, bsize, bord,
			       w == 2 ? " (the address is inside this block, not its head)"
				      : "");
			ss_dump_bytes(bbase, bsize);
		}
		return;
	}
}

/* Writable, committed, and actually part of the program's state. Image regions
 * carry the game's globals, so a naive "private memory only" filter would
 * silently miss them. Mapped views are skipped: they are usually shared with
 * another process, where a rewind has no meaning, and our own snapshot window
 * is one of them. */
/* Stands the adapter down around the snapshot. Registered by whichever front
 * end owns a device; null everywhere else, which is how the targets with no
 * hardware backend avoid linking one. */
static void (*g_gpu_park_fn)(int on);

void savestate_set_gpu_park(void (*fn)(int on))
{
	g_gpu_park_fn = fn;
}

/* Device mappings passed over, counted so the decision is visible rather than
 * silent. Reset each save: the question is what this snapshot skipped. */
static unsigned g_dev_regions;
static unsigned long long g_dev_bytes;

/* Memory a device owns rather than memory this process owns. Write-combining and
 * non-cached exist for mappings whose reads must not be cached, which describes
 * an adapter's aperture and nothing an allocator hands out.
 *
 * Shared by everything that walks the address space, because the two callers
 * want it for different reasons and both are right. The snapshot skips these
 * because reading one mid-freeze is a fault no handler can service. The
 * coverage audit skips them because they are unreadably slow - uncached, no
 * prefetch, across the bus - and because fingerprinting them is meaningless
 * anyway: the adapter rewrites them on its own schedule, so they would report
 * "changed" every time and mean nothing by it. */
static int region_is_device(DWORD prot)
{
	return (prot & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) != 0;
}

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
	/* Private executable pages are runtime-generated code - thunks and
	 * trampolines - and they are not ours to move through time.
	 *
	 * The crash: a fault executing 07196BD8, reached from an indirect call
	 * inside CoreUIComponents.dll through a pointer in that DLL's own data.
	 * System modules are deliberately left in the present, so the pointer
	 * survived the restore pointing at a thunk the restore had rewound out
	 * of existence. A module in the present must not be made to point into
	 * rewound memory, and the way to keep that promise is to leave the code
	 * it calls alone.
	 *
	 * Costs the game nothing: it is native compiled code with no JIT, so
	 * nothing it needs to rewind lives on an executable private page.
	 * Scoped to MEM_PRIVATE so image sections, including the game's own
	 * globals and anything Steam's DRM decrypted in place, are untouched. */
	if (mbi->Type == MEM_PRIVATE &&
	    (p & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
		  PAGE_EXECUTE_WRITECOPY)))
		return 0;
	/* Device memory, not process memory.
	 *
	 * A display driver maps video memory into the process write-combined, and
	 * the kernel's video memory manager can repoint those mappings at any
	 * moment - suspending threads holds ours, not the GPU. Reading one during
	 * the copy is a fault no handler can service, which is how a save with a
	 * live device died inside win_copy having logged nothing at all: the
	 * driver's heaps were already left in the present and its threads already
	 * excluded, so the region walk was the only way in left.
	 *
	 * Nothing the game owns is ever mapped like this. Write-combining and
	 * non-cached exist for memory whose reads must not be cached, which
	 * describes an aperture and nothing an allocator hands out. Leaving them
	 * out costs the snapshot nothing it should have had: the contents belong
	 * to the adapter, and the adapter does not rewind - the same bargain the
	 * texture arena already runs under. */
	if (region_is_device(p)) {
		g_dev_regions++;
		g_dev_bytes += mbi->RegionSize;
		return 0;
	}
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
typedef LONG(NTAPI *PFN_NtQuerySys)(ULONG, PVOID, ULONG, PULONG);

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

#define XS_MARK 0x31535858u /* "XXS1" */

/* Which build of this module is running, from its code bytes rather than the
 * link stamp, which this toolchain leaves at zero. A saved stack returns through
 * our own frames, so resuming it under a different build jumps into the middle
 * of unrelated instructions. Read through __ImageBase and never cached: the
 * loader lock may be held by a suspended thread, and a cached value in this
 * module's data could itself have come back from another process. */
extern IMAGE_DOS_HEADER __ImageBase;

/* Hashed from the file on disk: the code in memory has been relocated, so the
 * same build loaded at another base would hash as a different one. */
DWORD ss_build_id(void)
{
	static DWORD cached;
	const unsigned char *img = (const unsigned char *)&__ImageBase;
	const IMAGE_NT_HEADERS *nt =
		(const IMAGE_NT_HEADERS *)(img + __ImageBase.e_lfanew);
	const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
	DWORD h = 2166136261u;
	WORD k;
	WCHAR path[MAX_PATH];
	HANDLE f;

	if (cached)
		return cached;
	if (GetModuleFileNameW((HMODULE)&__ImageBase, path, MAX_PATH) &&
	    (f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
			     OPEN_EXISTING, 0, NULL)) != INVALID_HANDLE_VALUE) {
		unsigned char buf[65536];
		DWORD got, i;

		while (ReadFile(f, buf, sizeof(buf), &got, NULL) && got)
			for (i = 0; i < got; i++)
				h = (h ^ buf[i]) * 16777619u;
		CloseHandle(f);
		cached = h ? h : 1;
		return cached;
	}
	h = (h ^ nt->OptionalHeader.SizeOfImage) * 16777619u;
	for (k = 0; k < nt->FileHeader.NumberOfSections; k++) {
		const DWORD *p = (const DWORD *)(img + sec[k].VirtualAddress);
		DWORD n = sec[k].Misc.VirtualSize / sizeof(DWORD), i;

		if (!(sec[k].Characteristics & IMAGE_SCN_CNT_CODE))
			continue;
		for (i = 0; i < n; i++)
			h = (h ^ p[i]) * 16777619u;
	}
	return h ? h : 1;
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

/* Wine and Proton: ntdll exports this, native Windows does not. */
static int ss_under_wine(void)
{
	static int cached = -1;
	HMODULE ntdll;

	if (cached >= 0)
		return cached;
	ntdll = GetModuleHandleA("ntdll.dll");
	cached = (ntdll && GetProcAddress(ntdll, "wine_get_version")) ? 1 : 0;
	return cached;
}

/* A log line that has to survive the next wineserver call hanging.
 *
 * WriteFile of the savestate log has been observed to complete under Proton
 * (the dsound-quiet line is on disk) and then the helper never writes again.
 * Flush so the breadcrumb is not sitting in a wineserver write when we freeze. */
static void ss_phase(const char *fmt, ...)
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
	if (n > 0) {
		WriteFile(g_ctl->log, buf, (DWORD)n, &wrote, NULL);
		FlushFileBuffers(g_ctl->log);
	}
}

/* KTHREAD_STATE as NtQuerySystemInformation reports it. Ready/Running are
 * executing user-mode code (or about to); Waiting is a wineserver wait. */
enum {
	SS_TS_READY = 1,
	SS_TS_RUNNING = 2,
	SS_TS_WAITING = 5,
	SS_TS_DEFERRED_READY = 7
};

static int wine_state_runnable(unsigned state)
{
	return state == SS_TS_READY || state == SS_TS_RUNNING ||
	       state == SS_TS_DEFERRED_READY;
}

/* SystemProcessInformation layout moves between Wine builds. CLIENT_ID is
 * two HANDLEs (pid, tid) and ThreadState sits 20 bytes after UniqueProcess, so
 * scanning the blob for those pairs does not depend on the header size. A
 * missed match leaves the slot at 0, which means "do not freeze the mixer" -
 * the restore-crashes path, not the helper-hangs path. */
static void wine_sample_states(unsigned *states, int n)
{
	static BYTE buf[1 << 18];
	static PFN_NtQuerySys fn;
	ULONG got = 0, off;
	DWORD pid = GetCurrentProcessId();
	int i;

	for (i = 0; i < n; i++)
		states[i] = 0;
	if (!fn) {
		HMODULE nt = GetModuleHandleA("ntdll.dll");
		if (nt)
			fn = (PFN_NtQuerySys)(void *)GetProcAddress(nt, "NtQuerySystemInformation");
	}
	if (!fn || n <= 0)
		return;
	if (fn(5 /* SystemProcessInformation */, buf, sizeof(buf), &got) < 0)
		return;
	if (got < 24)
		return;
	for (off = 0; off + 24 <= got; off += 4) {
		DWORD p = *(DWORD *)(buf + off);
		DWORD t, st;

		if (p != pid)
			continue;
		t = *(DWORD *)(buf + off + 4);
		st = *(DWORD *)(buf + off + 20);
		if (st > 7)
			continue;
		for (i = 0; i < n; i++) {
			if (g_ctl->ids[i] == t)
				states[i] = st;
		}
	}
}

static int wine_start_in(PVOID start, HMODULE exe, HMODULE self)
{
	HMODULE m = NULL;

	if (!start)
		return 0;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCSTR)start, &m) ||
	    !m)
		return 0;
	return m == exe || m == self;
}

static int wine_audio_start(PVOID start)
{
	HMODULE m = NULL;
	char path[MAX_PATH];
	const char *name;

	if (!start)
		return 0;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCSTR)start, &m) ||
	    !m)
		return 0;
	if (!GetModuleFileNameA(m, path, sizeof(path)))
		return 0;
	name = strrchr(path, '\\');
	name = name ? name + 1 : path;
	return !_stricmp(name, "dsound.dll") || !_stricmp(name, "mmdevapi.dll") ||
	       !_stricmp(name, "audioses.dll") || !_stricmp(name, "audioeng.dll") ||
	       !_stricmp(name, "avrt.dll") || !_stricmp(name, "wdmaud.drv") ||
	       !_stricmp(name, "winmm.dll") || !_stricmp(name, "winepulse.drv") ||
	       !_stricmp(name, "winealsa.drv") || !_stricmp(name, "wineoss.drv") ||
	       !_stricmp(name, "winecoreaudio.drv") || !_stricmp(name, "winepulse.so") ||
	       !_strnicmp(name, "winepulse", 9) || !_strnicmp(name, "winealsa", 8);
}

/* Under Wine, SuspendThread of a thread that is in a wineserver request waits
 * for that request to finish. wineserver is single-threaded, so freezing the
 * threads it is serving deadlocks the helper's own next server call - which is
 * how a save dies after dsh_quiet with the helper in readv and the requester
 * spinning on busy. Native Windows has no such server, and freezing everyone
 * is still correct there.
 *
 * Game and wrapper threads freeze as before. Pulse/ALSA/PipeWire mixer workers
 * (dsound, mmdevapi, winepulse, ...) freeze only while they are Running/Ready:
 * that is user-mode mixing, and SuspendThread is then a local pause. Waiting
 * means they are already in the server; leave them, the way dinput is left,
 * so the server can still answer VirtualQuery. Unknown start in the Windows
 * directory: do not freeze. Pulse stays up the whole time - we never unload
 * the mixer, we only pause the threads that would write while we copy. */
static int wine_freeze_this(PVOID start, unsigned state)
{
	HMODULE exe, self = NULL;
	char path[MAX_PATH], windir[MAX_PATH];
	UINT wlen;
	const char *name;
	HMODULE m = NULL;

	if (!start)
		return 0;
	exe = GetModuleHandleA(NULL);
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCSTR)&wine_freeze_this, &self);
	if (wine_start_in(start, exe, self))
		return 1;
	if (wine_audio_start(start))
		return wine_state_runnable(state);
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCSTR)start, &m) ||
	    !m)
		return 0;
	if (!GetModuleFileNameA(m, path, sizeof(path)))
		return 0;
	wlen = GetWindowsDirectoryA(windir, sizeof(windir));
	if (wlen && _strnicmp(path, windir, wlen) == 0)
		return 0;
	name = strrchr(path, '\\');
	name = name ? name + 1 : path;
	/* Steam overlay, Wine Steam IPC (lsteamclient — prefix "steam" misses it),
	 * dinput/win32u waiters, and DXVK/winevulkan workers. Parking any of
	 * those is "Steam is not responding" or a silent leave. The Present
	 * thread is frozen only because request() already has it in a user-mode
	 * spin; do not SuspendThread a presenter that is still in Present. */
	if (_strnicmp(name, "steam", 5) == 0 || _strnicmp(name, "gameoverlay", 11) == 0 ||
	    !_stricmp(name, "lsteamclient.dll") || _strnicmp(name, "dinput", 6) == 0 ||
	    !_stricmp(name, "win32u.dll") || !_stricmp(name, "winevulkan.dll") ||
	    _strnicmp(name, "dxvk", 4) == 0)
		return 0;
	return 1;
}

/* waveOutPause and DXGI Present both wait on wineserver. The helper is about
 * to freeze threads; if it parks first, it never gets that far. Stopping the
 * game's DirectSound buffers (dsh_quiet) is a short server call and has been
 * observed to return. Standing the device down entirely is not - Pulse, ALSA
 * and PipeWire stay loaded, which is what a native Linux build has to survive
 * too. */
static void park_audio_gpu(void)
{
	if (ss_under_wine()) {
		/* waveOutPause / DXGI Present wait on wineserver. Our mix thread
		 * lives in this DLL and can idle in user-mode without that. Pulse
		 * stays loaded. */
		ss_phase("  wine: xa2 user-mode quiesce only; skipping waveOutPause/"
			 "Present (wineserver). mixer device stays loaded\n");
		xa2_sw_quiesce();
		return;
	}
	xa2_sw_park();
	if (g_gpu_park_fn)
		g_gpu_park_fn(1);
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
	/* Everything above this line is a deny-list grown one crash at a time, and
	 * every entry was added after a session died proving it belonged there.
	 * This is the same rule stated the other way round: what we rewind is what
	 * shipped with the game, and anything else stays in the present.
	 *
	 * The module that forced it was ddxx_MesHoooooook.dll, which is DxLib's
	 * message-hook helper. The engine writes it into %TEMP% at startup and
	 * loads it from there, so it matched no pattern above - not the Windows
	 * directory, not the Steam tree, not a device runtime name - and we rewound
	 * it like the game's own code.
	 *
	 * It holds a heap handle in its globals. A restore put that handle back to
	 * a value that no longer named a live heap, and the next thread the process
	 * created ran the DLL's DllMain on DLL_THREAD_ATTACH, which allocates. The
	 * frozen process caught it exactly there: RtlAllocateHeap comparing the
	 * signature at [esi+8] of heap 02300000, an address that appears nowhere in
	 * the eight heaps the process actually had. It faults at zero frames after
	 * the restore, every time, because thread creation is what triggers it.
	 *
	 * The same DLL is the one that held the loader lock in the deadlock, and it
	 * fills the stack-scan guesses in nearly every crash report we have. It was
	 * never the game's state to rewind.
	 *
	 * Stated positively the rule needs no maintenance: a module we did not
	 * install cannot have its OS handles wound backwards, whether or not we
	 * have met it yet. */
	if (g_game_len && _strnicmp(path, g_game_dir, g_game_len) != 0)
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

static HANDLE g_swheap SS_PRESENT;

HANDLE sw_heap(void)
{
	if (!g_swheap) {
		HANDLE h = HeapCreate(0, 1u << 20, 0);
		if (h && InterlockedCompareExchangePointer((PVOID *)&g_swheap, h, NULL))
			HeapDestroy(h); /* lost the race, someone else's is live */
	}
	return g_swheap;
}

void *gameheap_sw_alloc(size_t n, size_t a, const void *caller);
int gameheap_sw_owns(void *raw);
void gameheap_sw_free(void *raw);
size_t gameheap_sw_size(void *raw);

static void *sw_malloc_at(size_t n, const void *caller)
{
	unsigned char *raw, *p;
	if (n + SW_ALIGN < n)
		return NULL;
	/* In the game heap's import mode our blocks go where the game's do: a heap
	 * with no low-fragmentation front end, which rewinds whole. */
	p = (unsigned char *)gameheap_sw_alloc(n, SW_ALIGN, caller);
	if (p)
		return p;
	raw = (unsigned char *)HeapAlloc(sw_heap(), 0, n + SW_ALIGN);
	if (!raw)
		return NULL;
	p = (unsigned char *)(((uintptr_t)raw + SW_ALIGN) & ~(uintptr_t)(SW_ALIGN - 1));
	((void **)p)[-1] = raw;
	return p;
}

void *sw_malloc(size_t n)
{
	return sw_malloc_at(n, __builtin_return_address(0));
}

void *sw_calloc(size_t count, size_t size)
{
	size_t n = count * size;
	void *p;
	if (count && n / count != size)
		return NULL;
	p = sw_malloc_at(n, __builtin_return_address(0));
	if (p)
		memset(p, 0, n);
	return p;
}

void *gameheap_swa_alloc(size_t n);
int gameheap_swa_free(void *p);
size_t gameheap_swa_size(void *p);

/* DEFAULT-pool memory: the game remakes it after every load, so it goes where
 * no save carries it when the arena is on. */
void *sw_calloc_present(size_t count, size_t size)
{
	size_t n = count * size;
	void *p;

	if (count && n / count != size)
		return NULL;
	p = gameheap_swa_alloc(n);
	return p ? p : sw_calloc(count, size);
}

void sw_free(void *p)
{
	void *raw;

	if (!p || gameheap_swa_free(p))
		return;
	raw = ((void **)p)[-1];
	if (gameheap_sw_owns(raw))
		gameheap_sw_free(raw);
	else
		HeapFree(sw_heap(), 0, raw);
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
	if ((old = gameheap_swa_size(p)) != 0) {
		q = sw_malloc(n);
		if (!q)
			return NULL;
		memcpy(q, p, old < n ? old : n);
		sw_free(p);
		return q;
	}
	if (gameheap_sw_owns(((void **)p)[-1])) {
		unsigned char *raw = (unsigned char *)((void **)p)[-1];

		old = gameheap_sw_size(raw);
		old = old ? old - (size_t)((unsigned char *)p - raw) : 0;
		q = sw_malloc(n);
		if (!q)
			return NULL;
		memcpy(q, p, old < n ? old : n);
		sw_free(p);
		return q;
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

/* Does an address range fall inside a module the roster says we are holding?
 *
 * The module roster and the captured region list are built by different code
 * and have never been checked against each other. If a region we write back
 * lies inside a held module, one of the two is lying about what is ours, and
 * the restore is reaching into a peer that is still running. */
static int held_mod_at(uintptr_t lo, uintptr_t hi)
{
	int k;

	if (!g_ctl)
		return -1;
	for (k = 0; k < g_ctl->nmods; k++)
		if (!g_ctl->mod_rewound[k] && lo < g_ctl->mod_hi[k] &&
		    hi > g_ctl->mod_lo[k])
			return k;
	return -1;
}

/* Does this executable name a C runtime in its imports at all?
 *
 * Asking a loaded runtime for its heap only answers for a game that calls that
 * runtime. Rabi-Ribi does not: it links the UCRT statically, so its malloc is a
 * private function in its own .text and the ucrtbase.dll in the process belongs
 * to Windows, which loads it for itself in every modern process. We asked
 * ucrtbase, it truthfully said "the process heap", and we wrote that down as the
 * game's runtime heap. The heap was right and the sentence was wrong, and the
 * wrong half is the one a reader builds a theory on.
 *
 * An executable with no CRT import has a static one, and a static UCRT takes
 * GetProcessHeap() for its own - which the game's own image confirms from the
 * other side, since it contains no call to HeapCreate anywhere. */
static const char *exe_crt_import(void)
{
	static const char *const kCrtPrefix[] = { "ucrtbase", "msvcr", "msvcrt",
						  "api-ms-win-crt" };
	HMODULE m = GetModuleHandleA(NULL);
	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)m;
	IMAGE_NT_HEADERS *nt;
	IMAGE_IMPORT_DESCRIPTOR *imp;
	DWORD rva;

	if (!m || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return NULL;
	nt = (IMAGE_NT_HEADERS *)((char *)m + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return NULL;
	rva = nt->OptionalHeader
		      .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
		      .VirtualAddress;
	if (!rva)
		return NULL;
	imp = (IMAGE_IMPORT_DESCRIPTOR *)((char *)m + rva);
	for (; imp->Name; imp++) {
		const char *n = (const char *)m + imp->Name;
		size_t k;

		for (k = 0; k < sizeof(kCrtPrefix) / sizeof(kCrtPrefix[0]); k++) {
			const char *a = n, *b = kCrtPrefix[k];

			while (*b && *a) {
				char ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;

				if (ca != *b)
					break;
				a++;
				b++;
			}
			if (!*b)
				return n;
		}
	}
	return NULL;
}

/* The D3D9 title's runtime is tried first so its partition is unchanged; a
 * Unity player links the UCRT instead and would otherwise leave the rewind with
 * no anchored game heap at all. */
static HANDLE game_crt_heap(void)
{
	size_t i;

	/* Ahead of asking the runtimes, because when the redirect is on this is
	 * where the game's allocations actually are. Asking ucrtbase would
	 * answer with the process heap and send us back down the shared-heap
	 * path we installed the redirect to leave. */
	if (g_redirect_heap)
		return g_redirect_heap;
	/* Before any runtime is asked, because a loaded runtime will answer for
	 * itself whether or not this game has ever called it. */
	if (!exe_crt_import()) {
		HANDLE h = GetProcessHeap();

		ss_log("    game runtime heap %p: this executable imports no C "
		       "runtime, so it links one statically and its allocator is "
		       "its own code. The heap is the process heap, shared with "
		       "Windows - not because a runtime DLL said so, but because a "
		       "static runtime takes GetProcessHeap() and this image never "
		       "calls HeapCreate\n",
		       h);
		return h;
	}
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

/* The link stamp and image size from a mapped module's own PE header. Read from
 * memory rather than asked of the loader: this runs while threads are frozen, and
 * GetModuleFileName takes the loader lock a frozen thread may hold. */
static void mod_pe_id(uintptr_t base, DWORD *stamp, DWORD *isize)
{
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
	const IMAGE_NT_HEADERS *nt;

	*stamp = 0;
	*isize = 0;
	if (!ss_readable(base, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return;
	nt = (const IMAGE_NT_HEADERS *)(base + (uintptr_t)dos->e_lfanew);
	if (!ss_readable((uintptr_t)nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
		return;
	*stamp = nt->FileHeader.TimeDateStamp;
	*isize = nt->OptionalHeader.SizeOfImage;
}

/* The module's /GS cookie as it stands, or 0 if it has none. Every process
 * draws a fresh one, and a frame that stored it stored it XORed with its own
 * frame pointer - so a stack moved to another process fails the check twice
 * over unless both halves are put right. */
static uintptr_t mod_gs_cookie_at(uintptr_t base);

static uintptr_t mod_gs_cookie(uintptr_t base)
{
	uintptr_t at = mod_gs_cookie_at(base);

	return at ? *(const uintptr_t *)at : 0;
}

/* Where the module keeps its /GS cookie, or 0. */
static uintptr_t mod_gs_cookie_at(uintptr_t base)
{
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
	const IMAGE_NT_HEADERS *nt;
	const IMAGE_DATA_DIRECTORY *dd;
	const IMAGE_LOAD_CONFIG_DIRECTORY *lc;
	uintptr_t at;

	if (!ss_readable(base, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return 0;
	nt = (const IMAGE_NT_HEADERS *)(base + (uintptr_t)dos->e_lfanew);
	if (!ss_readable((uintptr_t)nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
		return 0;
	if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG)
		return 0;
	dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
	if (!dd->VirtualAddress ||
	    dd->Size < FIELD_OFFSET(IMAGE_LOAD_CONFIG_DIRECTORY, SecurityCookie) + sizeof(lc->SecurityCookie))
		return 0;
	lc = (const IMAGE_LOAD_CONFIG_DIRECTORY *)(base + dd->VirtualAddress);
	if (!ss_readable((uintptr_t)lc, FIELD_OFFSET(IMAGE_LOAD_CONFIG_DIRECTORY, SecurityCookie) +
						sizeof(lc->SecurityCookie)))
		return 0;
	at = (uintptr_t)lc->SecurityCookie;
	if (!at || !ss_readable(at, sizeof(uintptr_t)))
		return 0;
	return at;
}

/* One module list entry from a toolhelp record. The toolhelp snapshot already
 * carries the full path, so identity costs no loader call. */
static void mod_record(int m, const MODULEENTRY32 *me, char rewound)
{
	const char *nm = strrchr(me->szExePath, '\\');

	g_ctl->mod_lo[m] = (uintptr_t)me->modBaseAddr;
	g_ctl->mod_hi[m] = (uintptr_t)me->modBaseAddr + me->modBaseSize;
	g_ctl->mod_rewound[m] = rewound;
	lstrcpynA(g_ctl->mod_name[m], nm ? nm + 1 : me->szExePath,
		  sizeof(g_ctl->mod_name[m]));
	lstrcpynA(g_ctl->mod_path[m], me->szExePath, sizeof(g_ctl->mod_path[m]));
	mod_pe_id(g_ctl->mod_lo[m], &g_ctl->mod_stamp[m], &g_ctl->mod_isize[m]);
}

/* This process and this boot, in FILETIME units. Boot is derived from the wall
 * clock minus uptime, so it drifts with clock adjustments; compare with slack. */
typedef struct {
	uintptr_t *h;
	char (*cls)[64];
	int n, max;
} WndList;

static BOOL CALLBACK wnd_list_one(HWND w, LPARAM lp)
{
	WndList *l = (WndList *)lp;
	DWORD pid = 0;

	GetWindowThreadProcessId(w, &pid);
	if (pid != GetCurrentProcessId())
		return TRUE;
	if (l->n >= l->max)
		return FALSE;
	l->h[l->n] = (uintptr_t)w;
	if (!GetClassNameA(w, l->cls[l->n], 64))
		l->cls[l->n][0] = 0;
	l->n++;
	return TRUE;
}

/* This process's top-level windows in Z order, with their class names. */
static int wnd_list(uintptr_t *h, char (*cls)[64], int max)
{
	WndList l;

	l.h = h;
	l.cls = cls;
	l.n = 0;
	l.max = max;
	EnumWindows(wnd_list_one, (LPARAM)&l);
	return l.n;
}

static uintptr_t wnd_own_dc(uintptr_t w)
{
	HWND h = (HWND)w;
	HDC dc;

	if (!(GetClassLongPtrA(h, GCL_STYLE) & CS_OWNDC))
		return 0;
	dc = GetDC(h);
	if (dc)
		ReleaseDC(h, dc);
	return (uintptr_t)dc;
}

static void proc_identity(DWORD *pid, unsigned long long *created,
			  unsigned long long *boot)
{
	FILETIME c, e, k, u, now;

	*pid = GetCurrentProcessId();
	*created = 0;
	if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
		*created = ((unsigned long long)c.dwHighDateTime << 32) | c.dwLowDateTime;
	GetSystemTimeAsFileTime(&now);
	*boot = (((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime) -
		GetTickCount64() * 10000ull;
}

enum { PROV_UNKNOWN, PROV_SAME_PROCESS, PROV_SAME_BOOT, PROV_NEW_BOOT };

static const char *prov_name(int p)
{
	return p == PROV_SAME_PROCESS ? "same process"
	       : p == PROV_SAME_BOOT  ? "same boot, new process"
	       : p == PROV_NEW_BOOT   ? "new boot"
				      : "unknown (slot predates provenance)";
}

/* Where this load stands relative to the save. The engine's own restore never
 * branches on it: it is the check that what the load computed is possible. What
 * does branch is the bookkeeping that only means something inside one process -
 * see savestate_last_load_foreign. */
static int slot_provenance(const Slot *s)
{
	DWORD pid;
	unsigned long long created, boot, gap;

	if (!s->save_pid)
		return PROV_UNKNOWN;
	proc_identity(&pid, &created, &boot);
	if (pid == s->save_pid && created == s->save_created)
		return PROV_SAME_PROCESS;
	gap = boot > s->save_boot ? boot - s->save_boot : s->save_boot - boot;
	return gap < 30ull * 10000000ull ? PROV_SAME_BOOT : PROV_NEW_BOOT;
}

/* Under the Windows directory, so a system copy rather than one of ours or the
 * game's that happens to share its name. */
static int mod_is_system(int m)
{
	static char windir[MAX_PATH];
	static UINT n;

	if (!n) {
		n = GetWindowsDirectoryA(windir, sizeof(windir));
		if (n >= sizeof(windir))
			n = 0;
	}
	return n && _strnicmp(g_ctl->mod_path[m], windir, n) == 0;
}

static void sort_modules(void)
{
	int i, j;
	static char path[MAX_PATH]; /* only ever called from one thread at a time */

	for (i = 1; i < g_ctl->nmods; i++) {
		uintptr_t lo = g_ctl->mod_lo[i], hi = g_ctl->mod_hi[i];
		char rw = g_ctl->mod_rewound[i];
		DWORD st = g_ctl->mod_stamp[i], is = g_ctl->mod_isize[i];
		char nm[32];
		memcpy(nm, g_ctl->mod_name[i], sizeof(nm));
		memcpy(path, g_ctl->mod_path[i], sizeof(path));
		for (j = i; j > 0 && g_ctl->mod_lo[j - 1] > lo; j--) {
			g_ctl->mod_lo[j] = g_ctl->mod_lo[j - 1];
			g_ctl->mod_hi[j] = g_ctl->mod_hi[j - 1];
			g_ctl->mod_rewound[j] = g_ctl->mod_rewound[j - 1];
			g_ctl->mod_stamp[j] = g_ctl->mod_stamp[j - 1];
			g_ctl->mod_isize[j] = g_ctl->mod_isize[j - 1];
			memcpy(g_ctl->mod_name[j], g_ctl->mod_name[j - 1], sizeof(nm));
			memcpy(g_ctl->mod_path[j], g_ctl->mod_path[j - 1], sizeof(path));
		}
		g_ctl->mod_lo[j] = lo;
		g_ctl->mod_hi[j] = hi;
		g_ctl->mod_rewound[j] = rw;
		g_ctl->mod_stamp[j] = st;
		g_ctl->mod_isize[j] = is;
		memcpy(g_ctl->mod_name[j], nm, sizeof(nm));
		memcpy(g_ctl->mod_path[j], path, sizeof(path));
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

/* Is this list node really a heap's large-block entry, or a field that happened
 * to look like one?
 *
 * points_back is the wrong test here and saying so cost a run. It looks for the
 * heap's handle, which the low-fragmentation front end does carry - but a large
 * block is fronted by a _HEAP_VIRTUAL_ALLOC_ENTRY, which carries list links and
 * sizes and no handle at all. Every candidate failed it, genuine ones included,
 * so it separated nothing.
 *
 * Nor does anything else that can be read from out here. Two further tests were
 * tried and both measured something other than the block. The list node's
 * offset inside its allocation measures the walk order, because a block on
 * several candidate lists is attributed to whichever head reaches it first: the
 * same 128 MB allocation reported +0 on one run and +4000 on the next. The
 * entry's reserve size against the span measures a number that moves - the
 * field reads 8002000 while the span has come out at 128.01, 128.02 and 128.05
 * MB on three consecutive runs. Each wrong answer released a real block along
 * with its entry, and the restore faulted.
 *
 * The reserve field is still worth printing, so the log carries the evidence
 * for whoever tries a third time with better information.
 */
#define HEAP_VA_ENTRY_RESERVE 0x14u

/* Would excluding this span take memory away from a heap we rewind?
 *
 * heap_infra runs for heaps left in the present and holds back whatever their
 * headers reach, so the front end of a held heap does not get wound back
 * underneath its own segments. That part is right. What it never checked is who
 * owns the far end, and the two crash logs say that matters: heap 03B70000 in
 * one session and 03C10000 in the next were both classified "REWOUND with the
 * game" and both then excluded as "private memory reached from a held heap
 * header", so the game's own state was confiscated by a walk over somebody
 * else's lists. The object that faulted was sitting in exactly that span,
 * holding pointers into a sibling heap that did rewind.
 *
 * Two of our own decisions disagreeing about one heap is not a judgement call,
 * it is a bug, and the game cannot survive either answer being applied to half
 * of its object graph. */
static int overlaps_rewound_heap(uintptr_t lo, uintptr_t hi, HANDLE *who)
{
	int s, k;

	if (!g_ctl)
		return 0;
	for (s = 0; s < g_ctl->nseg; s++) {
		uintptr_t sb = g_ctl->seg_base[s];
		uintptr_t se = sb + g_ctl->seg_size[s];

		if (hi <= sb || lo >= se)
			continue;
		for (k = 0; k < g_ctl->nheaps; k++) {
			if (g_ctl->heap_h[k] != g_ctl->seg_owner[s])
				continue;
			if (!g_ctl->heap_ours[k])
				break;
			if (who)
				*who = g_ctl->seg_owner[s];
			return 1;
		}
	}
	return 0;
}

/* 1 protects rewinding heaps from the walk, 0 restores the old behaviour.
 *
 * A knob because the old behaviour is what every log in this project was
 * recorded under, and a change that silently invalidates the archive is worse
 * than one that can be switched off and compared against. */
static int infra_guard(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_INFRA_GUARD", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '0') ? 0 : 1;
	}
	return cached;
}

/* Report the clash and say whether the span was kept. Returns 1 when the caller
 * should skip the exclusion. */
static int infra_clash(uintptr_t ab, uintptr_t span, const char *where)
{
	HANDLE who = NULL;

	int i;

	if (!overlaps_rewound_heap(ab, ab + span, &who))
		return 0;
	for (i = 0; i < g_ctl->infra_nseen; i++)
		if (g_ctl->infra_seen[i] == ab)
			return 1; /* already counted, still keep it with the game */
	if (g_ctl->infra_nseen < (int)(sizeof(g_ctl->infra_seen) /
				       sizeof(g_ctl->infra_seen[0])))
		g_ctl->infra_seen[g_ctl->infra_nseen++] = ab;
	g_ctl->infra_clash++;
	g_ctl->infra_clash_bytes += span;
	if (!g_ctl->infra_first_at) {
		g_ctl->infra_first_at = ab;
		g_ctl->infra_first_heap = who;
	}
	/* Capped: one clash is the finding, a thousand identical lines is not. */
	if (g_ctl->infra_clash <= 8)
		ss_log("    CLASH: %s would hold %08lX (%.2f MB) in the present, but "
		       "it belongs to heap %p which we rewind. %s\n",
		       where, (unsigned long)ab,
		       (double)span / (1024.0 * 1024.0), who,
		       infra_guard() ? "Kept with the game."
				     : "Held anyway - INFRA_GUARD is off.");
	if (!infra_guard())
		return 0;
	g_ctl->infra_kept++;
	return 1;
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
				if (span && !region_excluded(ab, span) &&
				    !infra_clash(ab, span, "the header list walk")) {
					/* Everything this walk reaches is held, which is
					 * where two attempts to be cleverer ended up.
					 *
					 * Both tried to tell a large block whose payload is
					 * the game's from a piece of the heap's own
					 * machinery, and both read something that is not a
					 * property of the block. Testing where the list
					 * node sat inside the allocation tested the walk
					 * order: a block reachable from several candidate
					 * heads is attributed to whichever arrives first,
					 * and the same 128 MB allocation reported +0 on one
					 * run and +4000 on the next. Testing the entry's
					 * reserve size against the span tested a number
					 * that moves: the reserve field reads 8002000 while
					 * the span has measured 128.01, 128.02 and 128.05
					 * MB across three runs. Each misclassification
					 * released a genuine block with its entry and the
					 * restore faulted.
					 *
					 * The framing was wrong as well as the tests. This
					 * walk only ever reaches nodes on lists anchored in
					 * this heap's header, so everything it finds really
					 * is on one of the heap's lists; the front end and
					 * segment entries legitimately sit at non-zero
					 * offsets. "Not at the start" never meant "not the
					 * heap's".
					 *
					 * Reported, because the sizes are worth seeing and
					 * the 128 MB block is still the largest changing
					 * thing missing from every save. Answering that
					 * wants evidence about what is in it, not another
					 * guess at Windows' internals. */
					if (span >= (1u << 20))
						ss_log("    infra: holding %08lX (%.2f MB) off heap "
						       "%p's header list, reserve field %lX\n",
						       (unsigned long)ab,
						       (double)span / (1024.0 * 1024.0), (void *)h,
						       ss_readable(ab + HEAP_VA_ENTRY_RESERVE,
								   sizeof(unsigned))
							       ? *(const unsigned *)(ab +
										     HEAP_VA_ENTRY_RESERVE)
							       : 0u);
					ss_exclude_as("private memory reached from a held heap header", (void *)ab, (size_t)span);
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
		/* Checked here too, though this site already demands points_back
		 * and so is far less likely to reach a stranger. If it ever does,
		 * the same reasoning applies and the log should say so. */
		if (infra_clash(v, span, "the two-way header scan"))
			continue;
		ss_exclude_as("private allocation pointed to by a heap we hold", (void *)v, (size_t)span);
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
/* -1 when the setting is absent, so a caller can tell "unset" from an explicit
 * choice. The plain flag readers cannot: they fold both into their default. */
static int ss_tristate(const char *name)
{
	char v[8];
	DWORD n = ss_getenv(name, v, sizeof(v));

	if (!n || n >= sizeof(v))
		return -1;
	return v[0] == '0' ? 0 : 1;
}

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
/* The text input stack was added after it crashed in exactly the way this list
 * describes: C0000005 at textinputframework.dll+E0A20 with MSCTF.dll frames
 * beneath it, 388 frames after a restore, on a session whose eight restores were
 * all inside one room. Its heap was being rewound - the partition dump said
 * "heap 03E80000 textinputframework.dll rewound (193 vote(s))" - while the
 * objects in it are Windows' own, reached from thread local storage and from
 * COM apartment state that no snapshot of ours touches.
 *
 * It fails on focus changes rather than continuously because that is when MSCTF
 * runs. A game window losing focus is enough to make Windows walk structures we
 * wound back, which is why this looked like the game refusing to run off-screen.
 *
 * ntdll must stay first: the knob below trims the list back to it. */
static const char *const kOsHeapOwners[] = { "ntdll.dll", "textinputframework.dll",
					     "MSCTF.dll", "inputhost.dll" };

static int os_owned_heap(const char *who)
{
	size_t i, n = sizeof(kOsHeapOwners) / sizeof(kOsHeapOwners[0]);

	/* D3D9SW_REWIND_TEXTINPUT=1 hands the text input heaps back to the rewind,
	 * so the trade can be measured from the cfg instead of rebuilt. */
	if (ss_tristate("D3D9SW_REWIND_TEXTINPUT") == 1)
		n = 1;
	for (i = 0; i < n; i++)
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

/* Asset heaps flagged by the pre-suspend census (option 3, D3D9SW_ASSETEXCL).
 * These are the game's own large pure-data HeapCreate heaps that grow on a room
 * change - regenerable assets, not state. heaps_partition holds them in the
 * present so the game reloads them, rather than rewinding bytes that cross-session
 * arrive at a base Windows chose differently this launch. Handles mean nothing in
 * another process, so the list lives only for the duration of one save and is
 * cleared by asset_heaps_reset() before the census refills it. */
static HANDLE g_asset_heaps[SS_MAX_HEAPS];
static int g_n_asset_heaps;

static void asset_heaps_reset(void)
{
	g_n_asset_heaps = 0;
}

void savestate_note_asset_heap(void *h)
{
	int i;

	if (!h || g_n_asset_heaps >= SS_MAX_HEAPS)
		return;
	for (i = 0; i < g_n_asset_heaps; i++)
		if (g_asset_heaps[i] == (HANDLE)h)
			return; /* already noted this save */
	g_asset_heaps[g_n_asset_heaps++] = (HANDLE)h;
}

static int is_asset_heap(HANDLE h)
{
	int i;

	for (i = 0; i < g_n_asset_heaps; i++)
		if (g_asset_heaps[i] == h)
			return 1;
	return 0;
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
	g_ctl->infra_clash = 0;
	g_ctl->infra_kept = 0;
	g_ctl->infra_first_at = 0;
	g_ctl->infra_first_heap = NULL;
	g_ctl->infra_clash_bytes = 0;
	g_ctl->infra_nseen = 0;
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
		if (!owner || g_ctl->nseg >= SS_MAX_SEGS) {
			if (owner && g_ctl->nseg >= SS_MAX_SEGS && !g_ctl->seg_full++)
				ss_log("    WARNING: the segment list is full at %d, so some "
				       "heap memory is invisible to every decision below\n",
				       SS_MAX_SEGS);
			continue;
		}
		s = g_ctl->nseg++;
		g_ctl->seg_base[s] = base;
		g_ctl->seg_size[s] = alloc_span(base);
		g_ctl->seg_owner[s] = (HANDLE)owner;
	}

	for (k = 0; k < base_heaps; k++) {
		HANDLE h = g_ctl->heap_h[k];
		const char *who;
		int votes = 0, m = -1;

		/* The process heap is tested before the other two because it can BE
		 * one of them, and when it is, leaving it in the present has to win.
		 *
		 * A CRT that creates its own heap - MSVCR100, as DDPR uses - makes
		 * these distinct handles and the ordering never mattered. ucrtbase
		 * allocates from the process heap directly, so for a game linked
		 * against it the first test matched and rewound the very heap the
		 * branch below exists to protect. Windows keeps its own lists there
		 * and does not rewind with us, so the restore put back heap metadata
		 * that no longer described the blocks the system had allocated since
		 * the save. It survived two restores and failed the third, ending in
		 * the game dereferencing a freed list node. */
		if (h == proc && h != game) {
			/* Windows keeps its lists here whatever the vote says. */
			g_ctl->heap_ours[k] = 0;
			who = "the process heap";
		} else if (h == proc) {
			/* The game's runtime heap IS the process heap, which happens
			 * with ucrtbase and does not with MSVCR100. Both answers are
			 * wrong in different ways and there is no third option while
			 * the game and Windows share one heap:
			 *
			 * Rewinding it puts back heap metadata that no longer
			 * describes what Windows allocated since the save, and a
			 * later walk trips over its own free list.
			 *
			 * Leaving it in the present keeps Windows consistent but
			 * strands the game's own allocations in the future, so the
			 * restore returns holding pointers to data that moved on.
			 *
			 * Measured, rewinding lasts longer: it survived two reloads
			 * and failed the third, where holding it back failed the
			 * first with the game dereferencing what turned out to be
			 * ASCII text. So rewinding stays the default, and this is the
			 * knob for testing the other side of it. */
			int forced = ss_tristate("D3D9SW_REWIND_GAMEHEAP");

			g_ctl->heap_ours[k] = forced < 0 ? 1 : (char)forced;
			who = "the game's runtime, and the process heap";
			/* These two settings cancel each other and the result looks
			 * like a working restore that does nothing. Block restore
			 * draws its bytes from the captured region containing each
			 * block; holding the heap back excludes those segments from
			 * the snapshot, so every block comes back homeless and is
			 * skipped. Measured on this game: 14279 of 14472 blocks
			 * dropped and 10 KB written, with no error anywhere. */
			if (first && blk_mode() && !g_ctl->heap_ours[k])
				ss_log("    WARNING: D3D9SW_HEAPBLOCKS=1 with "
				       "D3D9SW_REWIND_GAMEHEAP=0 does nothing. Block restore "
				       "needs this heap CAPTURED to have bytes to put back - "
				       "set D3D9SW_REWIND_GAMEHEAP=1, which with heap blocks "
				       "on captures the heap but still writes only the busy "
				       "blocks and leaves the allocator's metadata alone\n");
			if (first)
				ss_log("    NOTE: this game's runtime allocates from the "
				       "process heap, so one heap serves both it and Windows. "
				       "Rewinding it (currently %s) risks the lists Windows "
				       "keeps alongside; holding it back strands the game's "
				       "own state. Set D3D9SW_REWIND_GAMEHEAP=0 to try the "
				       "other trade\n",
				       g_ctl->heap_ours[k] ? "on" : "off");
		} else if (h == game || (h && h == gameheap_rt_heap())) {
			g_ctl->heap_ours[k] = (char)game_heap_rewound();
			who = "the game's runtime";
		} else if (h == mine) {
			g_ctl->heap_ours[k] = (char)sw_heap_rewound();
			who = "this wrapper's own";
		} else if (gameheap_slot_heap((uintptr_t)h)) {
			/* The vote named steamclient.dll for DDPR's D3DX heap at
			 * 0D000000 on one machine; held back, a cross-launch load
			 * paired the restored game with this launch's effects. */
			g_ctl->heap_ours[k] = 1;
			who = "a pinned heap slot";
		} else {
			m = heap_claimed_by(h, &votes);
			who = m >= 0 ? g_ctl->mod_name[m] : "no clear owner";
			/* Wine has no 0xFFEEFFEE HEAP_SEGMENT, so votes stay 0 and
			 * ALLHEAPS=2 rewinds the heap. linux 82152 lived when those
			 * unnamed heaps stayed in the present; linux 26458/31417/
			 * 33277 died vkEndCommandBuffer after rewinding 001D0000
			 * under tid 524's 001DCA88. Hold them. Handle-page exclude
			 * for every held Wine heap is below, after heap_ours is
			 * final - not only unnamed. */
			if (ss_under_wine() && m < 0)
				g_ctl->heap_ours[k] = 0;
			/* With the game's allocator redirected by import, the game
			 * has exactly two heaps and both are named above; none of its
			 * modules imports HeapCreate. Anything else was created by a
			 * module we hold, whatever the vote says. Haydee's evidence:
			 * a 15 MB heap voted to launcher.exe was rewound under
			 * wrap_oal.dll's mixer thread, which resumed into a string in
			 * dsound's data section. */
			else if (gameheap_import_mode() && !gameheap_rt_heap()) {
				g_ctl->heap_ours[k] = 0;
				who = m >= 0 ? g_ctl->mod_name[m] : "no clear owner";
			} else
			/* Mode 2 keeps an unclaimed heap rather than stranding it,
			 * since the only heaps that must stay behind are the ones
			 * Windows reaches from data outside the save. */
			/* A heap belonging to a module we hold in the present goes
			 * with its module.
			 *
			 * os_owned_heap is a list of four names, written when the
			 * only offenders anyone had seen were ntdll's and the text
			 * input stack's. XAudio2 does not appear on it, so launching
			 * the game with -xaudio2 produced this: heap 07810000 claimed
			 * by xaudio2_9.DLL with 19669 votes, 7.06 MB over four
			 * segments, REWOUND with the game - while xaudio2_9.DLL ran
			 * thirty-two threads that never stopped. The game died on
			 * every restore, and it was not subtle about why.
			 *
			 * The list was never the rule, only the instances of it. The
			 * rule is that a module and its allocations belong to the same
			 * era: rewinding one while holding the other is the split this
			 * whole file exists to avoid, and we were manufacturing it for
			 * any audio backend nobody had happened to name yet. */
			g_ctl->heap_ours[k] =
				mode == 2
					? (char)!(os_owned_heap(who) ||
						  (m >= 0 && !g_ctl->mod_rewound[m]))
					: (char)(m >= 0 ? g_ctl->mod_rewound[m] : 0);
		}
		/* One switch over every Windows heap, because the write set says this
		 * game does not keep its state in them.
		 *
		 * Measured across a save and a reload: 16.0 MB changed, of which
		 * 15.81 MB sat in private regions belonging to no heap and 0.17 MB
		 * was spread over every NT heap put together - the process heap
		 * itself rounding to 0.00. The game runs its own allocator on
		 * VirtualAlloc and uses HeapAlloc for almost nothing that moves. So
		 * the trade the branch above agonises over is worth a fraction of a
		 * percent of the restore, while getting it wrong costs an ntdll fault
		 * walking rewound free lists or a length computed between two moments
		 * in time. Setting this to 0 gives up that fraction on purpose and
		 * hands every heap back to the present. */
		if (ss_tristate("D3D9SW_REWIND_HEAPS") == 0) {
			static int said_heaps;

			g_ctl->heap_ours[k] = 0;
			if (first && !said_heaps++)
				ss_log("    NOTE: D3D9SW_REWIND_HEAPS=0, so every Windows heap "
				       "below is left in the present regardless of who owns "
				       "it. The write set put 0.17 MB of change in all heaps "
				       "combined against 15.81 MB outside them, which is the "
				       "whole argument for doing this\n");
		}
		/* Option 3 (D3D9SW_ASSETEXCL): a heap the pre-suspend census judged to
		 * be regenerable assets is held in the present regardless of the vote
		 * above, so the game reloads it instead of the restore rewinding bytes to
		 * a base that moved between launches. Applied after every other rule so it
		 * wins, and only ever FLIPS a rewound heap to held - never the reverse. */
		if (is_asset_heap(h) && g_ctl->heap_ours[k]) {
			g_ctl->heap_ours[k] = 0;
			if (first)
				ss_log("    heap %p held in present as REGENERABLE ASSETS "
				       "(D3D9SW_ASSETEXCL) - the game reloads it\n",
				       (void *)h);
		}
		/* Wine has no HEAP_SEGMENT, so the segment-exclude loop below never
		 * sees handle pages. linux 82fd84a held unnamed heaps only
		 * (001DCA88 warning gone) then winevulkan+23434 walked 0015D390
		 * on process heap 00150000: PARTITION LEAK 1.1 MB still captured
		 * the process-heap and wrapper-heap handle pages. 82152 lived
		 * when every held Wine heap's handle page stayed in the present.
		 * Not the header-list walk (process-heap growth); that killed
		 * load 1 in rabiribi.exe. Engine payload only. */
		if (ss_under_wine() && !g_ctl->heap_ours[k])
			ss_exclude_as("Wine heap left in the present",
				      (void *)g_ctl->heap_lo[k],
				      (size_t)(g_ctl->heap_hi[k] - g_ctl->heap_lo[k]));
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
			ss_exclude_as("segment of a heap left in the present", (void *)g_ctl->seg_base[s], (size_t)g_ctl->seg_size[s]);
			held += g_ctl->seg_size[s];
			nheld++;
		}
		/* Every segment is recorded, rewound or not, so that an address can
		 * be named and its side of the line known. Only the first segment of
		 * a heap is its handle, so without this the rest of a grown heap
		 * answers to nothing. */
		/* Loud, because the silent version of this cost us a day. A segment
		 * that does not fit here is a segment the restore will write
		 * wholesale. */
		if (g_ctl->nheaps >= SS_MAX_HEAPS && first)
			ss_log("    WARNING: the heap table is full at %d entries, so "
			       "segments past this point answer to no heap and will be "
			       "restored WHOLESALE, metadata included, instead of by "
			       "block\n",
			       SS_MAX_HEAPS);
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
		/* Wine's heap header is not a Windows HEAP. Walking the first
		 * 0x400 bytes as LIST_ENTRY follows random pointers into the game
		 * runtime and holds tens of megabytes that a silent VM (no Pulse
		 * mixer, nothing to walk) never held. The restore then rewinds
		 * the rest of that heap and the two disagree. Mixer heaps are
		 * already left in the present by owner; skipping the walk keeps
		 * Pulse loaded without tearing the game's allocator in half. */
		if (ss_under_wine()) {
			static int said_wine_infra;

			if (first && !said_wine_infra++)
				ss_log("    NOTE: Wine heap headers are not Windows HEAP, so the "
				       "header-list walk that holds outlying blocks is skipped\n");
			continue;
		}
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
	/* Printed every save, including when it is zero. A contradiction that only
	 * appears in the log when it happens is one nobody can prove the absence
	 * of, and the whole point of this line is to be comparable run to run. */
	if (g_ctl->infra_clash)
		ss_log("  heap contradiction: %d span(s), %.2f MB, belong to heaps we "
		       "rewind AND were reachable from a heap we hold. %d kept with "
		       "the game, first at %08lX on heap %p. This is the split that "
		       "killed three sessions at 0 frames\n",
		       g_ctl->infra_clash,
		       (double)g_ctl->infra_clash_bytes / (1024.0 * 1024.0),
		       g_ctl->infra_kept, (unsigned long)g_ctl->infra_first_at,
		       g_ctl->infra_first_heap);
	else
		ss_log("  heap contradiction: none - no span belongs to a heap we "
		       "rewind while also being reachable from one we hold\n");
	if (first)
		audit_untracked_regions();
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

/* Large private memory that belongs to no heap we found.
 *
 * The write that killed a restore copied into 133E0000 from 12890000. The first
 * is a later segment of a heap we rewind - which is why ss_heap_of could name it
 * even though the census only prints base heaps - and the second is in no heap
 * range at all, so the fault report had nothing to say about it. Whether that
 * source came back with the save or stayed in the present is what decides
 * whether the two ends of a copy agree, and no log line answered it.
 *
 * So answer it. Every private region big enough to matter, with the capture
 * decision spelled out rather than inferred. Diagnostic only, and off unless
 * asked, because it walks the whole address space inside the suspend. */
static void audit_untracked_regions(void)
{
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t p = 0x10000, total = 0, hidden = 0;
	int listed = 0, n = 0;
	char v[8];

	if (ss_getenv("D3D9SW_REGION_AUDIT", v, sizeof(v)) == 0 || v[0] == '0')
		return;
	ss_log("  region audit: committed private memory in no heap we enumerated\n");
	while (p < (uintptr_t)0x7FFF0000 &&
	       VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
		uintptr_t base = (uintptr_t)mbi.BaseAddress;
		uintptr_t size = (uintptr_t)mbi.RegionSize;
		int wanted;

		if (size == 0)
			break;
		p = base + size;
		if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE ||
		    heap_index_of(base) >= 0)
			continue;
		n++;
		total += size;
		wanted = region_wanted(&mbi);
		if (!wanted)
			hidden += size;
		if (size < 1024 * 1024)
			continue;
		if (listed++ < 32)
			ss_log("    %p +%.1f MB prot %lx: %s%s\n", (void *)base,
			       (double)size / (1024.0 * 1024.0),
			       (unsigned long)mbi.Protect,
			       wanted ? "captured, so it rewinds with the save"
				      : "NOT captured, so it stays in the present",
			       region_excluded(base, size) ? " (held back by name)" : "");
	}
	/* The split is the number to read. Anything left in the present that the
	 * game also writes to is a candidate for the next mismatched length. */
	ss_log("  region audit: %d region(s), %.1f MB total, %.1f MB of it staying in "
	       "the present\n",
	       n, (double)total / (1024.0 * 1024.0), (double)hidden / (1024.0 * 1024.0));
}

/* Restoring what the game allocated without restoring the allocator.
 *
 * The run that finally worked got two clean restores and then hung, with the
 * process heap failing HeapValidate and a thread parked in RtlFreeHeap walking
 * a free list that no longer described the heap. The write set says how small
 * the problem is: about 10 KB of that heap changes between a save and a reload,
 * and those 10 KB are two things mixed together. Some of it is the game's own
 * objects - the entity state that makes a restore visible at all, without which
 * the camera snaps back and is dragged forward again on the next frame. The
 * rest is the allocator's bookkeeping, updated by every system allocation since
 * the save, and writing that back is what corrupts the heap.
 *
 * Pages cannot tell those apart. A Windows heap can: every block carries a
 * header giving its size and whether it is in use. Walking the heap at the save
 * and again at the restore gives two block maps, and a block busy in both at
 * the same address and the same size is the game's data sitting exactly where
 * it sat. Those ranges are written back. Everything else in the heap - segment
 * headers, free lists, the heap's own lock - stays in the present, which is the
 * only state consistent with what Windows has done since.
 *
 * A block that was freed after the save is deliberately not restored. Its
 * memory belongs to the allocator now, and putting the old contents back would
 * be writing into someone else's allocation to satisfy a pointer that is
 * already dangling.
 *
 * Both walks run before the threads are suspended. That is the constraint the
 * census records: HeapWalk takes the heap's lock, and taking a lock whose
 * holder is frozen is a deadlock rather than a measurement. */
typedef struct HeapBlk {
	uintptr_t base;
	unsigned long size;
	/* Which locked heap the walk found it in. Carried so that when a heap
	 * starts faulting we can name the blocks we wrote into that heap rather
	 * than every block we wrote anywhere. */
	unsigned short heap;
} HeapBlk;

#define SS_BLK_CAP (1024u * 1024u) /* 8 MB per list. Every heap gets walked, not
				    * just the rewound ones, and this game's
				    * biggest heap alone votes 590k times. */

static HeapBlk *g_blk_save SS_PRESENT, *g_blk_now SS_PRESENT;
static unsigned long long *g_reg_off SS_PRESENT;
static unsigned g_blk_save_n, g_blk_now_n, g_blk_match_n;
static int g_blk_ready;

/* Off by default. MEASURED on the PE32 harness, 12 runs each, after the
 * audit_scan overrun that had been polluting every earlier comparison was
 * fixed: off fails 1 of 12, on fails 6 of 12, and half of those are 0xC0000409
 * fastfails - ntdll finding its own free list corrupt.
 *
 * The flaw looks fundamental rather than incidental. blk_match pairs a saved
 * block with a current one when the address and the size agree, and treats that
 * as identity. It is not identity: the allocator recycles addresses, so a block
 * freed and handed to something else of the same size passes the test, and the
 * restore then writes the game's stale bytes over whatever now lives there -
 * including ntdll's own allocations, which is precisely the pause-menu crash.
 * Making this safe needs a real identity test, not a tighter filter. */
static int blk_mode(void)
{
	char v[8];
	DWORD n = ss_getenv("D3D9SW_HEAPBLOCKS", v, sizeof(v));

	return (n > 0 && n < sizeof(v) && v[0] == '1') ? 1 : 0;
}

/* Held outside the snapshot, or the block map would itself be rewound halfway
 * through being used to rewind something else. */
static void *blk_arena(size_t bytes)
{
	void *p = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

	if (p)
		savestate_exclude(p, bytes);
	return p;
}

static int blk_alloc(void)
{
	if (!g_blk_save)
		g_blk_save = (HeapBlk *)blk_arena(SS_BLK_CAP * sizeof(HeapBlk));
	if (!g_blk_now)
		g_blk_now = (HeapBlk *)blk_arena(SS_BLK_CAP * sizeof(HeapBlk));
	if (!g_reg_off)
		g_reg_off = (unsigned long long *)blk_arena(SS_MAX_REGIONS *
							   sizeof(unsigned long long));
	return g_blk_save && g_blk_now && g_reg_off;
}

/* The heaps this thread has locked, held across the freeze and released by
 * resume_all. Releasing from resume_all rather than from the callers is
 * deliberate: every exit path already funnels through it, and a leaked
 * all-heaps lock is the one bug here that cannot be recovered from - the
 * process would wedge on its next allocation with no fault to report. */
static HANDLE g_blk_locked[SS_MAX_HEAPS];
static unsigned g_blk_locked_n;

static void blk_unlock_all(void);

/* The loader lock, taken before the heap locks and released after them.
 *
 * Windows' order is loader then heap, and it is not negotiable because the
 * loader takes it on our behalf: LdrpInitializeThread holds the loader lock
 * while it runs every module's DllMain on a starting thread, and this process
 * has a DllMain that allocates - ddxx_MesHoooooook, on every thread attach. We
 * took the two the other way round and the process wedged exactly as that
 * recipe promises. Our thread held all eight heap locks and sat in
 * CreateToolhelp32Snapshot waiting for the loader lock; a thread that had just
 * started held the loader lock and sat in RtlAllocateHeap waiting for a heap
 * lock. 32 game workers and both thread pools were frozen behind them.
 *
 * Both snapshots inside the window want this lock - the thread one in
 * collect_threads and the module one in build_exclusions, each by way of
 * RtlQueryProcessDebugInformation. Owning it first turns those into recursive
 * acquisitions that cost nothing instead of waits on somebody else.
 *
 * The side effect is worth more than the deadlock it fixes. While we hold this,
 * no thread can get through LdrpInitializeThread and no DLL can load, so the
 * window is closed to exactly the events that were killing this game hundreds
 * of frames after a restore looked clean.
 *
 * Try rather than block, because a thread already stuck in a DllMain of its own
 * would take us down with it. Giving up on a save is recoverable; hanging the
 * game is what we are here to stop. */
#define SS_LDR_TRY 0x02
#define SS_LDR_GOT 0x01

typedef LONG(WINAPI *PFN_LdrLock)(ULONG, ULONG *, ULONG_PTR *);
typedef LONG(WINAPI *PFN_LdrUnlock)(ULONG, ULONG_PTR);

static PFN_LdrLock g_ldr_lock_fn;
static PFN_LdrUnlock g_ldr_unlock_fn;
static ULONG_PTR g_ldr_cookie;
static int g_ldr_held;

static int ldr_lock(void)
{
	int tries;

	if (g_ldr_held)
		return 1;
	/* Both resolved here, where nothing is held yet. Looking one up later
	 * would mean calling into the loader from inside the window. */
	if (!g_ldr_lock_fn || !g_ldr_unlock_fn) {
		HMODULE nt = GetModuleHandleA("ntdll.dll");

		if (!nt)
			return 0;
		g_ldr_lock_fn = (PFN_LdrLock)GetProcAddress(nt, "LdrLockLoaderLock");
		g_ldr_unlock_fn =
			(PFN_LdrUnlock)GetProcAddress(nt, "LdrUnlockLoaderLock");
		if (!g_ldr_lock_fn || !g_ldr_unlock_fn)
			return 0;
	}
	for (tries = 0; tries < 400; tries++) {
		ULONG disp = 0;

		g_ldr_cookie = 0;
		if (g_ldr_lock_fn(SS_LDR_TRY, &disp, &g_ldr_cookie) >= 0 &&
		    disp == SS_LDR_GOT) {
			g_ldr_held = 1;
			return 1;
		}
		Sleep(1);
	}
	return 0;
}

static void ldr_unlock(void)
{
	if (!g_ldr_held)
		return;
	g_ldr_held = 0;
	if (g_ldr_unlock_fn)
		g_ldr_unlock_fn(0, g_ldr_cookie);
	g_ldr_cookie = 0;
}

/* The module snapshot, taken before any lock is held rather than in the middle
 * of build_exclusions. The loader lock above makes this safe either way, but
 * it is the one call in the window with no business being there, and the less
 * loader work we do while holding eight heap locks the smaller the blast radius
 * if some future path forgets the ordering. */
static HANDLE g_mod_snap SS_PRESENT;

static void modsnap_take(void)
{
	if (g_mod_snap)
		return;
	g_mod_snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
	if (g_mod_snap == INVALID_HANDLE_VALUE)
		g_mod_snap = NULL;
}

static HANDLE modsnap_use(void)
{
	HANDLE h = g_mod_snap;

	g_mod_snap = NULL;
	return h ? h : CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
}

/* Take every heap lock while the process is still running, so that the walk
 * that happens later - after suspend_all - is looking at a heap nobody is in
 * the middle of modifying.
 *
 * This is the ordering the first version got backwards. It walked before the
 * freeze, which meant the block map described one instant and the page copy
 * that followed described another; a block freed in between was recorded as
 * busy and its free-list metadata got captured and later written back as if it
 * were the game's data. It also meant walking against 41 live threads, which
 * the PE32 harness eventually caught in the act: blk_collect sat inside
 * HeapWalk holding a heap lock while four other threads piled up behind the
 * same critical section, at which point the run neither finished nor died.
 *
 * Locking first inverts both problems. A thread that wants to allocate blocks
 * on the lock instead of racing us, and it blocks outside the heap structures,
 * which is exactly where we want it when it gets frozen a moment later. */
static int blk_lock_all(void)
{
	HANDLE list[SS_MAX_HEAPS];
	DWORD nh, k;

	blk_unlock_all();
	/* Both before a single heap lock: the snapshot so it is not loader work
	 * done under them, and the loader lock so our order matches Windows'. */
	modsnap_take();
	/* From here to resume_all, no knob read may touch the process heap - but
	 * every one of them must still give the answer it would have given out
	 * here, so warm them all first. */
	env_prewarm();
	g_env_frozen = 1;
	if (!ldr_lock()) {
		ss_log("  warning: could not take the loader lock, so the heap locks "
		       "are not safe to take either - no block map this time\n");
		return 0;
	}
	/* GetProcessHeaps does not return how many handles it wrote. When the
	 * buffer is too small it returns how many heaps the process has, which is
	 * larger, and looping to that count walks off the end of a stack array
	 * and calls HeapLock on whatever was next on the stack. Caught by the
	 * PE32 harness on the very first save, twice, having also shown up once
	 * at 64 bits as a stack-overrun fastfail rather than an access
	 * violation. */
	nh = GetProcessHeaps(SS_MAX_HEAPS, list);
	if (nh > SS_MAX_HEAPS)
		nh = SS_MAX_HEAPS;
	for (k = 0; k < nh; k++) {
		if (!list[k] || !HeapLock(list[k]))
			continue;
		g_blk_locked[g_blk_locked_n++] = list[k];
	}
	return (int)g_blk_locked_n;
}

static void blk_unlock_all(void)
{
	while (g_blk_locked_n)
		HeapUnlock(g_blk_locked[--g_blk_locked_n]);
	/* Released in the reverse of the order they were taken. */
	ldr_unlock();
}

/* Every busy block in every heap this thread holds the lock on. Must run with
 * the locks held and the other threads frozen; see blk_lock_all.
 *
 * Not filtered to the heaps being rewound, because it cannot be:
 * build_exclusions - which decides who is rewound - has not run yet. Filtering
 * happens for free at the other end instead. A block in a heap left in the
 * present sits in no captured region, so blk_region_of refuses it and nothing
 * is written. */
static unsigned blk_walk_locked(HeapBlk *out, unsigned cap, int *hit_cap)
{
	unsigned n = 0, k;

	*hit_cap = 0;
	for (k = 0; k < g_blk_locked_n; k++) {
		PROCESS_HEAP_ENTRY e;

		/* HeapWalk can fault, and when it does it is our fault.
		 *
		 * A frozen process gave the first real stack this project has had, and
		 * every frame under ntdll in it was ours: RtlpGetFirstBlockAddress
		 * faulting while decoding an LFH header, called from RtlpWalkLFHBlock,
		 * called from HeapWalk, called from here. The game was not crashing.
		 * The block machinery was walking a heap whose LFH chain a previous
		 * restore had made inconsistent, and taking the process down with it.
		 *
		 * Whether that inconsistency is ours to fix is a separate question, and
		 * a real one. This is not the fix - it is the guarantee that the
		 * diagnostic can never be the thing that kills the run. A heap we
		 * cannot walk contributes no blocks, the restore falls back to whole
		 * regions for it, and the log says so. */
		if (setjmp(g_hw_jmp)) {
			ss_log("  heap blocks: heap %u of %u could not be walked - its "
			       "allocator structures faulted under us, so it contributes "
			       "no blocks to this restore\n",
			       k + 1, g_blk_locked_n);
			continue;
		}
		g_hw_tid = GetCurrentThreadId();
		g_hw_armed = 1;
		memset(&e, 0, sizeof(e));
		while (HeapWalk(g_blk_locked[k], &e)) {
			if (e.wFlags & (PROCESS_HEAP_REGION |
					PROCESS_HEAP_UNCOMMITTED_RANGE))
				continue;
			if (!(e.wFlags & PROCESS_HEAP_ENTRY_BUSY))
				continue;
			if (!e.lpData || !e.cbData)
				continue;
			if (n >= cap) {
				*hit_cap = 1;
				break;
			}
			out[n].base = (uintptr_t)e.lpData;
			out[n].size = (unsigned long)e.cbData;
			out[n].heap = (unsigned short)k;
			n++;
		}
		g_hw_armed = 0;
	}
	g_hw_armed = 0;
	return n;
}

/* Walk a heap purely to find out whether it can still be walked.
 *
 * Run twice around the block writes, inside one restore with every thread still
 * frozen. Nothing else in the process has executed between the two calls, so a
 * heap that answers before and faults after was broken by us, in the window we
 * control, and the blocks we wrote into it are a complete list of suspects.
 *
 * Until now the damage only surfaced on the NEXT restore, minutes of gameplay
 * later, with no way to tie it to anything - which is how the block identity
 * problem stayed a hypothesis all day instead of becoming a measurement. */
static int blk_walk_probe(HANDLE h)
{
	PROCESS_HEAP_ENTRY e;
	unsigned n = 0;

	if (setjmp(g_hw_jmp))
		return -1;
	g_hw_tid = GetCurrentThreadId();
	g_hw_armed = 1;
	memset(&e, 0, sizeof(e));
	while (HeapWalk(h, &e))
		if (++n > 4000000u)
			break;
	g_hw_armed = 0;
	return (int)n;
}

/* How many frames between walkability checks of the process heap. 0 is off.
 *
 * The corruption is silent. Nothing faults at the moment it happens; we only
 * find out at the next restore, which can be minutes of play later, and that
 * window contains everything the game did and everything we did with no way to
 * tell them apart. Checking on a timer shrinks the window to one interval, and
 * freezing on the first failure means the process is still standing there with
 * every thread's stack intact when it is caught. */
static int watch_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[16];
		DWORD n = ss_getenv("D3D9SW_WATCH", v, sizeof(v));

		if (!n || n >= sizeof(v))
			return 0;
		cached = atoi(v);
		if (cached < 0)
			cached = 0;
	}
	return cached;
}

static int g_watch_ok = 1;
static unsigned g_watch_since;

static void heap_watch_tick(void)
{
	static unsigned t;
	HANDLE h = GetProcessHeap();
	int every = watch_mode(), n;

	g_watch_since++;
	/* Open and shut the thread gate on the frame clock, so the block lasts a
	 * wall-clock window after the restore rather than a count of calls. */
	if (g_nothr_until) {
		if (!g_nothr_at)
			nothread_arm(1);
		if (GetTickCount() >= g_nothr_until) {
			nothread_arm(0);
			g_nothr_until = 0;
		}
	}
	if (!every || !g_watch_ok)
		return;
	if (++t % (unsigned)every)
		return;
	if (!HeapLock(h))
		return;
	n = blk_walk_probe(h);
	HeapUnlock(h);
	if (n >= 0)
		return;
	g_watch_ok = 0;
	ss_log("WATCH: the process heap walked clean %d frame(s) ago and cannot be "
	       "walked now. No save or restore ran in that interval unless the log "
	       "above says so, which would make this ordinary play breaking it\n",
	       every);
	ss_freeze_here("heap watch - the process heap's front end just went inconsistent",
		       0, 0);
}

static int g_probe_before[64];
/* Tentative definition; the real one sits with the rest of the block-provenance
 * arrays further down, next to the code that allocates it. */
static unsigned char *g_blk_wrote SS_PRESENT;

static int probe_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[16];
		DWORD n = ss_getenv("D3D9SW_PROBE", v, sizeof(v));

		if (!n || n >= sizeof(v))
			return 0;
		cached = atoi(v) ? 1 : 0;
	}
	return cached;
}

static void blk_probe_before(void)
{
	unsigned k;

	if (!probe_mode())
		return;
	for (k = 0; k < g_blk_locked_n && k < 64; k++)
		g_probe_before[k] = blk_walk_probe(g_blk_locked[k]);
}

/* The verdict. Anything named here was written by us, into a heap that could be
 * walked moments earlier and cannot be walked now, with no other thread having
 * run in between. */
static void blk_probe_after(void)
{
	unsigned k, i;

	if (!probe_mode())
		return;
	for (k = 0; k < g_blk_locked_n && k < 64; k++) {
		int now = blk_walk_probe(g_blk_locked[k]);
		unsigned wrote = 0, listed = 0;

		if (g_probe_before[k] < 0) {
			ss_log("  PROBE: heap %u could not be walked even before this "
			       "restore - the damage predates it\n",
			       k + 1);
			continue;
		}
		if (now >= 0)
			continue;
		for (i = 0; i < g_blk_match_n; i++)
			if (g_blk_wrote && g_blk_wrote[i] && g_blk_save[i].heap == k)
				wrote++;
		ss_log("  PROBE: heap %u walked %d entries BEFORE our writes and FAULTS "
		       "after them. We wrote %u block(s) into it during this restore, "
		       "and nothing else ran in between - one of these did it\n",
		       k + 1, g_probe_before[k], wrote);
		for (i = 0; i < g_blk_match_n && listed < 24; i++) {
			unsigned moff;
			const char *mod;

			if (!g_blk_wrote || !g_blk_wrote[i] || g_blk_save[i].heap != k)
				continue;
			mod = ss_module(g_blk_save[i].base, &moff);
			ss_log("    PROBE: wrote %p + %lu byte(s)%s%s\n",
			       (void *)g_blk_save[i].base, g_blk_save[i].size,
			       mod ? ", " : "", mod ? mod : "");
			listed++;
		}
		if (wrote > listed)
			ss_log("    PROBE: %u more not shown\n", wrote - listed);
	}
}

/* Iterative quicksort: the lists reach six figures, this runs on the restore
 * path, and neither deep recursion nor a library sort that might take the heap
 * lock belongs here. */
static void blk_sort(HeapBlk *a, int n)
{
	struct {
		int lo, hi;
	} st[64];
	int sp = 0;

	if (n < 2)
		return;
	st[sp].lo = 0;
	st[sp].hi = n - 1;
	sp++;
	while (sp > 0) {
		int lo, hi;

		sp--;
		lo = st[sp].lo;
		hi = st[sp].hi;
		while (lo < hi) {
			uintptr_t piv = a[lo + ((hi - lo) >> 1)].base;
			int i = lo, j = hi;

			while (i <= j) {
				while (a[i].base < piv)
					i++;
				while (a[j].base > piv)
					j--;
				if (i <= j) {
					HeapBlk t = a[i];

					a[i] = a[j];
					a[j] = t;
					i++;
					j--;
				}
			}
			/* Push the smaller side, iterate the larger, so the stack
			 * stays logarithmic whatever the input looks like. */
			if (j - lo < hi - i) {
				if (lo < j && sp < 64) {
					st[sp].lo = lo;
					st[sp].hi = j;
					sp++;
				}
				lo = i;
			} else {
				if (i < hi && sp < 64) {
					st[sp].lo = i;
					st[sp].hi = hi;
					sp++;
				}
				hi = j;
			}
		}
	}
}

/* Both lists sorted, so one merge finds every block that is busy at the same
 * address and the same size in each. Compacted into the save list in place:
 * the match count never overtakes the read cursor. */
static unsigned blk_match(void)
{
	unsigned i = 0, j = 0, m = 0;

	while (i < g_blk_save_n && j < g_blk_now_n) {
		if (g_blk_save[i].base < g_blk_now[j].base) {
			i++;
		} else if (g_blk_save[i].base > g_blk_now[j].base) {
			j++;
		} else {
			if (g_blk_save[i].size == g_blk_now[j].size)
				g_blk_save[m++] = g_blk_save[i];
			i++;
			j++;
		}
	}
	return m;
}

static int heap_rewound_at(uintptr_t a)
{
	int k = heap_index_of(a);

	return k >= 0 && g_ctl->heap_ours[k];
}

/* Is this address in a heap the system allocates from as well as the game?
 *
 * Only the process heap qualifies here, and only because this game's runtime
 * happens to be ucrtbase, which does not create a private heap. The game's
 * other heaps are its alone and every block in them is safe to put back. */
static int heap_is_shared(uintptr_t a)
{
	int k = heap_index_of(a);

	return k >= 0 && g_ctl->heap_h[k] == GetProcessHeap();
}

/* Does this block look like it belongs to the game rather than to Windows?
 *
 * Restoring block contents fixed the allocator but not the tenants. The process
 * heap serves ntdll as well as the game, and a block busy at the same address
 * and size in both walks can just as easily be one of ntdll's - a work item, a
 * string, a list node - whose contents have moved on for reasons that have
 * nothing to do with the save. Putting one of those back rewinds live system
 * state, which is how a restore that validated cleanly still died in the pause
 * menu reading 0x20 off a null pointer inside ntdll.
 *
 * The test is the one the heap census already uses for whole heaps, applied a
 * block at a time: a pointer into a module that travels with the save. A C++
 * object's vtable is such a pointer, and so is any function pointer or
 * back-reference the game stores. Only the head of the block is scanned,
 * because a vtable lives at offset zero and scanning megabytes to find a
 * pointer that is probably in the first few words is not worth the restore
 * time.
 *
 * This errs towards leaving blocks alone. A game block wrongly skipped costs
 * some state that does not come back; a system block wrongly restored costs
 * the process. */
static int blk_owner_of(uintptr_t b, unsigned long n)
{
	const uintptr_t *w = (const uintptr_t *)b;
	unsigned long i, lim = n / sizeof(uintptr_t);
	int saw_game = 0, saw_sys = 0;

	if (lim > 16)
		lim = 16;
	if (!lim || IsBadReadPtr(w, lim * sizeof(uintptr_t)))
		return 0;
	for (i = 0; i < lim; i++) {
		int m;

		if (w[i] < 0x10000)
			continue;
		m = module_of(w[i]);
		if (m < 0)
			continue;
		if (g_ctl->mod_rewound[m])
			saw_game = 1;
		else
			saw_sys = 1;
	}
	/* A block naming a module that stays in the present is Windows' business
	 * whatever else it holds; the reverse is not true, because plenty of the
	 * game's own blocks are plain bytes naming nothing at all. */
	if (saw_sys && !saw_game)
		return -1;
	return saw_game ? 1 : 0;
}

/* 0 restores every matched block. 1 skips only blocks that point at a module
 * left in the present, which is the shape of ntdll's own allocations. 2 demands
 * positive proof of the game and skips everything else.
 *
 * 2 was the first attempt and it was far too strict: it turned away 3228 blocks
 * a reload and halved what came back, because a string or a float array carries
 * no pointer to prove itself with. 1 inverts the question - convict rather than
 * acquit - and only the blocks that actively look like Windows' are held back. */
static int blk_owner_filter(void)
{
	char v[8];
	DWORD n = ss_getenv("D3D9SW_BLKOWNER", v, sizeof(v));

	if (n == 0 || n >= sizeof(v))
		return 1;
	return v[0] - '0';
}

/* Whether a block that was freed and handed out again since the save is still
 * written back.
 *
 * Address and size identity is what the block map matches on, and a free in
 * between breaks the only thing that identity was standing in for. The bytes in
 * the snapshot describe an object that no longer exists; the allocator has since
 * given that address to something else, and on the low fragmentation heap that
 * something else is as likely to be the allocator's own subsegment bookkeeping
 * as another game object.
 *
 * The free set already found these and the count was already logged - we were
 * writing them anyway. Every session that reported "none of them an address we
 * restored" survived its restores. The first session to report five of them
 * died in RtlpLowFragHeapAllocFromContext reading through 28DC, zero frames
 * later.
 *
 * There is no version of writing these that is defensible: at best the address
 * now holds a different object of the same size, and we overwrite it with
 * another object's fields. Set D3D9SW_RECYCLED=1 to go back to writing them. */
static int recycled_write(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_RECYCLED", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '1') ? 1 : 0;
	}
	return cached;
}

/* Ownership by reachability, which is the thing the guess above approximates.
 *
 * A block belongs to the game if the game can reach it. Start from everything
 * we capture that is not a heap - the game's image, its arenas, its thread
 * stacks - and follow every pointer that lands in the shared heap, then follow
 * the pointers inside those blocks, and so on. What the closure reaches is the
 * game's object graph. What it does not reach is somebody else's.
 *
 * This replaces sniffing a block's first few words for something module-shaped,
 * which cannot work here and did not: at BLKOWNER=1 it waved through a GDI font
 * cache block and ntdll's corruption analyzer wedged the process inside the heap
 * lock, and at BLKOWNER=2 it rejected 3686 blocks and lost the state we were
 * trying to restore. The signal is not in the block's contents, it is in who
 * points at it.
 *
 * Heaps are deliberately not roots. Following pointers out of arbitrary heap
 * memory would let one Windows block vouch for the next and the closure would
 * swallow the whole heap. Only blocks already proven to be the game's get their
 * contents scanned. */
#define SS_BLK_QCAP (1u << 18)

static unsigned char *g_blk_own SS_PRESENT;
static unsigned char *g_blk_sys SS_PRESENT;
static unsigned *g_blk_queue SS_PRESENT;

/* The derivation behind the verdict, kept rather than discarded.
 *
 * The walk below already computes all of this and used to throw it away, which
 * is why every crash so far has cost a debugger session: we could say a block
 * was reachable but not from where, through what, or who else could reach it.
 * A fault would land in ntdll or gdi32 and the only honest answer was a theory.
 *
 * Four arrays turn that into an answer. parent is the block whose contents held
 * the pointer, so following it back spells out the chain a claim came down.
 * root is where the chain started - a module's data, a thread stack, one of the
 * game's arenas - which is the part that says whether a claim is real ownership
 * or a stale pointer in a dead stack frame. depth is how far from that root, and
 * high numbers on a block that Windows also claims are exactly the shape of a
 * closure that has wandered somewhere it should not be. wrote is the fact that
 * matters most and was never recorded at all: whether this restore actually put
 * bytes there. */
static unsigned *g_blk_parent SS_PRESENT;
static uintptr_t *g_blk_root SS_PRESENT;
static unsigned char *g_blk_depth SS_PRESENT;
static unsigned char *g_blk_wrote SS_PRESENT;

#define SS_BLK_NOPARENT ((unsigned)-1)

typedef struct {
	unsigned char *own;
	unsigned *qn;
	uintptr_t lo, hi;
	int exact_only;
	int track; /* only the game's walk decides writes, so only it is recorded */
	uintptr_t root;
	unsigned parent;
	unsigned char depth;
} BlkWalk;

static int region_excluded(uintptr_t base, uintptr_t size);

static int blk_own_alloc(void)
{
	if (!g_blk_own)
		g_blk_own = (unsigned char *)blk_arena(SS_BLK_CAP);
	if (!g_blk_sys)
		g_blk_sys = (unsigned char *)blk_arena(SS_BLK_CAP);
	if (!g_blk_wrote)
		g_blk_wrote = (unsigned char *)blk_arena(SS_BLK_CAP);
	if (!g_blk_depth)
		g_blk_depth = (unsigned char *)blk_arena(SS_BLK_CAP);
	if (!g_blk_parent)
		g_blk_parent = (unsigned *)blk_arena(SS_BLK_CAP * sizeof(unsigned));
	if (!g_blk_root)
		g_blk_root = (uintptr_t *)blk_arena(SS_BLK_CAP * sizeof(uintptr_t));
	if (!g_blk_queue)
		g_blk_queue = (unsigned *)blk_arena(SS_BLK_QCAP * sizeof(unsigned));
	return g_blk_own && g_blk_sys && g_blk_wrote && g_blk_depth && g_blk_parent &&
	       g_blk_root && g_blk_queue;
}

/* The matched list is sorted by address and its blocks do not overlap, so the
 * block containing an address is one binary search rather than a scan of
 * fourteen thousand entries per candidate pointer. */
static int blk_find(uintptr_t v)
{
	unsigned lo = 0, hi = g_blk_match_n;

	while (lo < hi) {
		unsigned mid = lo + (hi - lo) / 2;

		if (v < g_blk_save[mid].base)
			hi = mid;
		else if (v >= g_blk_save[mid].base + g_blk_save[mid].size)
			lo = mid + 1;
		else
			return (int)mid;
	}
	return -1;
}

/* One bounds check before the binary search, because this runs over every
 * aligned word of everything we capture - tens of millions of them - and all
 * but a handful land nowhere near the shared heap. */
static unsigned g_reach_exact, g_reach_inner;

/* Exact means the word equals the block's user pointer, which is what malloc
 * returned and therefore what a real reference to the object looks like.
 * Anything else is a word that merely lands somewhere inside the block.
 *
 * The distinction decides correctness, not tidiness. This scans every aligned
 * word of everything we capture - tens of millions - against an 8 MB window, so
 * any float, length or hash that happens to fall in range adopts a block. At
 * BLKOWNER=3, which accepted interior hits, 73% of blocks came back "reachable"
 * and ntdll died in RtlpLocalInfoAllocFromCache reading a null - an LFH
 * structure claimed by a coincidence and then overwritten. Mode 4 takes only
 * exact base pointers. It can miss a block the game holds solely by an interior
 * pointer, which costs state; adopting one of ntdll's costs the process. */
static void blk_scan(const uintptr_t *w, size_t words, BlkWalk *k)
{
	size_t i;

	for (i = 0; i < words; i++) {
		uintptr_t v = w[i];
		int bi;

		if (v < k->lo || v >= k->hi)
			continue;
		bi = blk_find(v);
		if (bi < 0)
			continue;
		if (v == g_blk_save[bi].base)
			g_reach_exact++;
		else if (k->exact_only)
			continue;
		else
			g_reach_inner++;
		if (k->own[bi])
			continue;
		k->own[bi] = 1;
		if (k->track) {
			g_blk_parent[bi] = k->parent;
			g_blk_root[bi] = k->root;
			g_blk_depth[bi] = k->depth;
		}
		if (*k->qn < SS_BLK_QCAP)
			g_blk_queue[(*k->qn)++] = (unsigned)bi;
	}
}

/* A root's address range is not necessarily one uniform mapping: a module's
 * data can straddle pages the loader has left read-only, and .mrdata spends
 * most of its life that way. Query as we go so the walk reads what is there
 * and steps over what is not, rather than faulting halfway down a section. */
static void blk_scan_range(uintptr_t base, uintptr_t size, BlkWalk *k)
{
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t p = base, end = base + size;

	while (p < end && VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
		uintptr_t a = p;
		uintptr_t re = (uintptr_t)mbi.BaseAddress + (uintptr_t)mbi.RegionSize;
		uintptr_t b = re < end ? re : end;

		if (!mbi.RegionSize)
			break;
		p = re;
		if (mbi.State != MEM_COMMIT || b <= a)
			continue;
		if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
			continue;
		if (!(mbi.Protect &
		      (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
		       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
			continue;
		blk_scan((const uintptr_t *)a, (b - a) / sizeof(uintptr_t), k);
	}
}

static void blk_reach_mark(void)
{
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t p = 0x10000, lo_all = (uintptr_t)-1, hi_all = 0;
	unsigned qh = 0, qn = 0, marked = 0, i;
	HANDLE proc = GetProcessHeap();
	int nroot = 0;
	LARGE_INTEGER t0, t1, pf;
	BlkWalk k;
	int kk;

	int exact_only = blk_owner_filter() >= 4;

	if (!g_blk_match_n || !blk_own_alloc())
		return;
	QueryPerformanceFrequency(&pf);
	QueryPerformanceCounter(&t0);
	memset(g_blk_own, 0, g_blk_match_n);
	g_reach_exact = 0;
	g_reach_inner = 0;

	memset(&k, 0, sizeof(k));
	k.own = g_blk_own;
	k.qn = &qn;
	k.exact_only = exact_only;
	k.track = 1;

	/* Only the shared heap needs deciding; the game's private heaps have no
	 * other tenant and every block in them is already safe to put back. */
	for (kk = 0; kk < g_ctl->nheaps; kk++) {
		if (g_ctl->heap_h[kk] != proc)
			continue;
		if (g_ctl->heap_lo[kk] < lo_all)
			lo_all = g_ctl->heap_lo[kk];
		if (g_ctl->heap_hi[kk] > hi_all)
			hi_all = g_ctl->heap_hi[kk];
	}
	if (lo_all >= hi_all)
		return;
	k.lo = lo_all;
	k.hi = hi_all;

	while (p < (uintptr_t)0x7FFF0000 &&
	       VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
		uintptr_t base = (uintptr_t)mbi.BaseAddress;
		uintptr_t size = (uintptr_t)mbi.RegionSize;

		p = base + size;
		if (!size)
			break;
		if (mbi.State != MEM_COMMIT)
			continue;
		if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
			continue;
		if (!(mbi.Protect &
		      (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
		       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
			continue;
		/* Nor is the adapter's memory, for both of the reasons that keep it
		 * out of the snapshot and the coverage pass. It cannot hold a game
		 * pointer - it holds texels, written by us and rearranged by the
		 * driver - and reading it is uncached and across the bus, which is
		 * ruinous for a scan that touches every word. Sixty-six such regions
		 * appearing alongside the hardware backend took this pass from
		 * 181.7 ms to 4928.0 ms; nothing about the algorithm changed. */
		if (region_is_device(mbi.Protect))
			continue;
		/* Heaps are not roots, and neither is anything we leave in the
		 * present - system images and our own memory. What is left is the
		 * game: its image, its arenas, its stacks. */
		if (heap_index_of(base) >= 0)
			continue;
		if (region_excluded(base, size))
			continue;
		nroot++;
		k.root = base;
		k.parent = SS_BLK_NOPARENT;
		k.depth = 0;
		blk_scan((const uintptr_t *)base, size / sizeof(uintptr_t), &k);
	}

	while (qh < qn) {
		unsigned bi = g_blk_queue[qh++];

		/* The chain carries forward: whatever vouched for this block vouches
		 * for what it points at, one hop further out. */
		k.root = g_blk_root[bi];
		k.parent = bi;
		k.depth = (unsigned char)(g_blk_depth[bi] < 254 ? g_blk_depth[bi] + 1 : 255);
		blk_scan((const uintptr_t *)g_blk_save[bi].base,
			 g_blk_save[bi].size / sizeof(uintptr_t), &k);
	}
	for (i = 0; i < g_blk_match_n; i++)
		if (g_blk_own[i])
			marked++;

	QueryPerformanceCounter(&t1);
	/* The two hit counts are the diagnosis. Exact hits are real references;
	 * interior hits are mostly words that happen to land in the heap's address
	 * range. If interior dwarfs exact, the loose mode was measuring noise. */
	ss_log("  reach: %u of %u matched block(s) reachable from the game, over %d "
	       "root region(s), %u exact + %u interior hit(s), %s, %.1f ms%s\n",
	       marked, g_blk_match_n, nroot, g_reach_exact, g_reach_inner,
	       exact_only ? "exact only" : "interior allowed",
	       (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)pf.QuadPart,
	       qn >= SS_BLK_QCAP ? " <<< QUEUE FULL, closure is incomplete" : "");
}

#if defined(_M_IX86) || defined(__i386__)
#define TEB_PEB 0x30
#define TEB_ACTCTX_SP 0x1A8
#define PEB_BYTES 0x480
#else
#define TEB_PEB 0x60
#define TEB_ACTCTX_SP 0x2C8
#define PEB_BYTES 0x7C8
#endif

/* The writable data of one module we leave in the present. .data holds its
 * globals; on current Windows the loader keeps its module index and hash table
 * in .mrdata, which carries the write bit in the section header but sits
 * read-only at runtime between updates - so the header is what to test, not the
 * page protection. */
static void blk_sys_mod(uintptr_t mbase, uintptr_t mhi, BlkWalk *k)
{
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)mbase;
	const IMAGE_NT_HEADERS *nt;
	const IMAGE_SECTION_HEADER *sec;
	unsigned i;

	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return;
	nt = (const IMAGE_NT_HEADERS *)(mbase + (uintptr_t)dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return;
	sec = IMAGE_FIRST_SECTION(nt);
	for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
		uintptr_t b = mbase + sec->VirtualAddress;
		uintptr_t n = sec->Misc.VirtualSize;

		if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE))
			continue;
		if (!n || b < mbase || b + n > mhi)
			continue;
		k->root = b;
		blk_scan_range(b, n, k);
	}
}

/* The same closure run from the other side of the process, and a veto.
 *
 * Everything we leave in the present - ntdll, the rest of Windows, Steam, the
 * overlay - keeps its own objects on the same shared heap the game uses, and
 * the loader is the one that matters: PEB_LDR_DATA, an LDR_DATA_TABLE_ENTRY per
 * module, the name buffers, the dependency-graph nodes and the base-address
 * index all live there. Putting any of those back tells the loader a lie about
 * which DLLs exist, and it collects at the next DLL load rather than at the
 * restore - LdrpInitializeThread on a thread that started afterwards, or
 * RtlpLocateActivationContextSection resolving a redirect. Both of those killed
 * this game, several hundred frames after a restore that looked clean.
 *
 * They reach the game's side of the closure by accident. Thread stacks are
 * roots, and a stack that has ever been through LoadLibrary or GetModuleHandle
 * still has loader pointers in dead frames, so the walk above adopts them on an
 * exact base match like any other object. Excluding the TEB and the PEB does
 * not help: that protects the pointers while the structures they name sit in
 * the heap we write.
 *
 * Deny wins, because the two mistakes are not the same size. Refusing a block
 * the game owned costs some state and the next restore tries again. Putting
 * back a block the loader owned costs the process.
 *
 * This is deliberately a closure and not a list of structures. Walking
 * PEB->Ldr field by field means encoding a layout that changes between Windows
 * releases, and a pointer missed there - the DDAG node, the index tree - reads
 * as a fixed bug right up until the next crash. Following words that happen to
 * equal a block's base needs to know nothing about any of it, and picks up
 * whatever Microsoft adds next. */
/* Which of the modules we leave in the present get a vote.
 *
 * 1, the default, is ntdll alone. 2 is all of them, which sounds like the safer
 * setting and is the opposite: the game's runtime heap is the process heap and
 * it belongs to ucrtbase, so the CRT's globals reach every object the game has
 * ever malloc'd. At 2 the system closure contested 7920 of the 8326 blocks the
 * game's closure had claimed - 95 percent - and the restore wrote 246 blocks
 * where the run before it wrote 1481. That is not a veto, it is turning block
 * restore off by another name, and the restore after it did not survive.
 *
 * A transitive closure only stays useful while its roots stay honest. ntdll is
 * the loader and it owns the activation-context tables, which is the whole of
 * what we caught corrupting; the CRT is the game's allocator wearing a Windows
 * filename. 0 marks nothing, for measuring against. */
static int sys_root_mode(void)
{
	char v[8];
	DWORD n = ss_getenv("D3D9SW_SYSROOT", v, sizeof(v));

	if (n == 0 || n >= sizeof(v))
		return 1;
	return v[0] - '0';
}

/* Which modules in the present get a vote, at modes 1 and 3.
 *
 * Mode 3 exists because a fault named the gap between the other two. dsound.dll
 * died at +61926 reading offset 0x10 from 07141684, a block on the game's
 * runtime heap that we had just wound back and whose provenance recorded "Game
 * reach yes, Windows reach yes". dsound has to stay in the present - it talks to
 * the sound card, which does not rewind - and it holds pointers into memory we
 * do. At mode 1 it has no vote, so nothing protects what it is still reading.
 *
 * Mode 2 would protect it and everything else besides, which the notes above
 * record as ruinous: the game's runtime heap is the process heap and belongs to
 * ucrtbase, so the CRT's globals reach every object the game ever allocated, and
 * the system closure contested 95 percent of the game's blocks.
 *
 * So the audio stack by name, and nothing else. Naming modules is exactly what
 * the closure was designed to avoid, and it is still the right instinct - but
 * the reason to avoid it was that the loader's internals change between Windows
 * releases, and these are public DLL names rather than private structures. The
 * layer below dsound is included because a buffer's memory is reached through
 * the mixer, and CoTaskMem allocations made on its behalf live on the process
 * heap like everything else. */
static int sys_root_wanted(const char *name, int mode)
{
	static const char *audio[] = { "dsound.dll",   "audioses.dll", "mmdevapi.dll",
				       "audioeng.dll", "wdmaud.drv",   "ksuser.dll",
				       "avrt.dll",     "xaudio2_7.dll" };
	unsigned i;

	if (lstrcmpiA(name, "ntdll.dll") == 0)
		return 1;
	if (mode != 3)
		return 0;
	for (i = 0; i < sizeof(audio) / sizeof(audio[0]); i++)
		if (lstrcmpiA(name, audio[i]) == 0)
			return 1;
	return 0;
}

static void blk_sys_mark(void)
{
	uintptr_t lo_all = (uintptr_t)-1, hi_all = 0;
	unsigned qh = 0, qn = 0, marked = 0, both = 0, i;
	HANDLE proc = GetProcessHeap();
	int k, nroot = 0, nteb = 0, nteb_sys = 0, nteb_hits = 0;
	LARGE_INTEGER t0, t1, pf;
	BlkWalk w;

	int exact_only = blk_owner_filter() >= 4;
	int mode = sys_root_mode();

	if (!g_blk_match_n || !blk_own_alloc())
		return;
	QueryPerformanceFrequency(&pf);
	QueryPerformanceCounter(&t0);
	memset(g_blk_sys, 0, g_blk_match_n);
	if (mode <= 0) {
		ss_log("  system reach: off\n");
		return;
	}
	g_reach_exact = 0;
	g_reach_inner = 0;

	for (k = 0; k < g_ctl->nheaps; k++) {
		if (g_ctl->heap_h[k] != proc)
			continue;
		if (g_ctl->heap_lo[k] < lo_all)
			lo_all = g_ctl->heap_lo[k];
		if (g_ctl->heap_hi[k] > hi_all)
			hi_all = g_ctl->heap_hi[k];
	}
	if (lo_all >= hi_all)
		return;
	/* Not tracked: provenance records who let a block be written, and nothing
	 * this walk marks is written. */
	memset(&w, 0, sizeof(w));
	w.own = g_blk_sys;
	w.qn = &qn;
	w.lo = lo_all;
	w.hi = hi_all;
	w.exact_only = exact_only;
	w.parent = SS_BLK_NOPARENT;

	/* One rule for every tenant: if we decided not to rewind a module, its
	 * globals are where that module keeps the handles to its heap objects.
	 * That covers the loader, the activation-context tables, Steam and the
	 * overlay without naming any of them, and it stays covered when the next
	 * uninvited DLL shows up. */
	for (k = 0; k < g_ctl->nmods; k++) {
		if (g_ctl->mod_rewound[k])
			continue;
		/* The allowlist names system DLLs, and a name alone would also match
		 * one of ours - dsound.dll among them - so the path has to agree. */
		if (mode < 2 && !(sys_root_wanted(g_ctl->mod_name[k], mode) && mod_is_system(k)))
			continue;
		blk_sys_mod(g_ctl->mod_lo[k], g_ctl->mod_hi[k], &w);
		nroot++;
	}

	/* Each heap's own header, because without it the chain from ntdll's
	 * globals to the allocator's private structures dies at the first step.
	 * RtlpProcessHeaps points at the _HEAP structures and those live at the
	 * heap base as metadata, not as busy blocks, so HeapWalk never reports one
	 * and blk_find refuses the pointer - the walk stops before it can follow
	 * FrontEndHeap.
	 *
	 * What is on the far side of that hop is the low fragmentation heap, and
	 * its bookkeeping is allocated out of the heap like anything else. That is
	 * why HeapWalk hands it to us as an ordinary busy block, why the game's
	 * closure can adopt one through a stale pointer, and why writing a saved
	 * copy over it kills the next allocation in
	 * RtlpLocalInfoAllocFromCache with a null segment info. "Allocator
	 * metadata untouched" was always true and never covered this: the free
	 * list headers between blocks are fine, it is the allocator's own objects
	 * inside blocks that were not. */
	for (k = 0; k < g_ctl->nheaps; k++) {
		if (!g_ctl->heap_h[k])
			continue;
		w.root = (uintptr_t)g_ctl->heap_h[k];
		blk_scan_range((uintptr_t)g_ctl->heap_h[k], 0x1000, &w);
		nroot++;
	}

	for (i = 0; i < (unsigned)g_ctl->nids; i++) {
		unsigned char *teb;

		if (!g_ctl->handles[i])
			continue;
		teb = (unsigned char *)teb_of(g_ctl->handles[i]);
		if (!teb)
			continue;
		if (!nteb) {
			void *peb = *(void **)(teb + TEB_PEB);

			/* Reaches Ldr, ProcessParameters and the process-default
			 * activation context in one step. The structure only - the
			 * rest of its page is not ours to interpret. */
			if (peb) {
				w.root = (uintptr_t)peb;
				blk_scan_range((uintptr_t)peb, PEB_BYTES, &w);
				nroot++;
			}
		}
		/* Class B: system thread TEBs as veto roots (restore_invariants.md
		 * invariant 6).
		 *
		 * audit_present_threads reported ~8 blocks on the game's rewound
		 * heap held by system thread TEBs every session, but the report
		 * was advisory only. Those blocks were still written back, and the
		 * system threads that held them - which keep running forward - used
		 * the restored contents as present-time state. The crash histogram
		 * shows workers dying 0 frames after restore with wild pointers
		 * in CRT and ntdll paths on those same threads.
		 *
		 * Only the blocks a TEB word points at directly. No closure from
		 * their contents, and no hop into the small private allocations the
		 * TEB reaches.
		 *
		 * That wider walk is what this used to do, and it was measured
		 * wrong: feeding the TEB page into the closure took system-owned
		 * from 3317 blocks to 7483, because every TEB pointer that lands in
		 * the heap fans out through the whole object graph and drags the
		 * game's own state out of the restore with it. The advisory count
		 * was always about eight, and eight is the honest number - that is
		 * how many blocks the thread can reach without going through
		 * something else, and reaching through something else is the
		 * game's business, not Windows'.
		 *
		 * Over-vetoing does not announce itself. It looks like a restore
		 * that ran clean and a game that quietly did not come back.
		 *
		 * Game threads still get only the activation-context pointer, which
		 * is the one TEB field pointing at a heap block their side needs.
		 * (Ported from rabiribi-savestate PR 1, which measured it.) */
		if (g_ctl->transient[i]) {
			const uintptr_t *tw = (const uintptr_t *)teb;
			unsigned tk;

			nteb_sys++;
			for (tk = 0; tk < 0x1000 / sizeof(uintptr_t); tk++) {
				uintptr_t v = tw[tk];
				int bi;

				if (v < lo_all || v >= hi_all)
					continue;
				bi = blk_find(v);
				if (bi < 0)
					continue;
				if (exact_only && v != g_blk_save[bi].base)
					continue;
				if (!g_blk_sys[bi]) {
					g_blk_sys[bi] = 1;
					nteb_hits++;
				}
			}
			nroot++;
		} else {
			/* ActivationContextStackPointer. That stack is a heap block
			 * and the TEB is the only thing pointing at it, so without
			 * this word the closure never finds it - and the TEB is
			 * excluded, so nothing else is going to. */
			w.root = (uintptr_t)(teb + TEB_ACTCTX_SP);
			blk_scan_range((uintptr_t)(teb + TEB_ACTCTX_SP),
				       sizeof(void *), &w);
		}
		nteb++;
	}
	nroot += nteb;

	while (qh < qn) {
		unsigned bi = g_blk_queue[qh++];

		w.parent = bi;
		blk_scan((const uintptr_t *)g_blk_save[bi].base,
			 g_blk_save[bi].size / sizeof(uintptr_t), &w);
	}
	for (i = 0; i < g_blk_match_n; i++) {
		if (!g_blk_sys[i])
			continue;
		marked++;
		if (g_blk_own[i])
			both++;
	}

	QueryPerformanceCounter(&t1);
	/* Contested is the number to read. It is how many blocks the game's
	 * closure had claimed and this one takes back, which is the size of the
	 * hole the loader crashes were coming through. Zero means they were not
	 * coming from here and the next theory is somewhere else. */
	ss_log("  system reach: %u of %u matched block(s) reachable from %s, %u "
	       "contested, over %d root(s), %u exact hit(s), %.1f ms. TEB veto: %d "
	       "system thread(s) scanned, %d block(s) directly held%s\n",
	       marked, g_blk_match_n,
	       mode == 3  ? "ntdll and the audio stack"
	       : mode < 2 ? "ntdll"
			  : "everything left in the present",
	       both, nroot, g_reach_exact,
	       (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)pf.QuadPart,
	       nteb_sys, nteb_hits,
	       qn >= SS_BLK_QCAP ? " <<< QUEUE FULL, closure is incomplete" : "");
}

/* What the restore did with the block containing an address, and why.
 *
 * This is the answer to every fault report we have written by hand. Until now a
 * crash gave an address and a module, and turning that into "did we touch it"
 * meant a debugger, a symbol download and a chain of inference that was a
 * theory at best. Three tenants killed this process in one night - ntdll's low
 * fragmentation heap, our own allocator use, then GDI - and each one was found
 * by crashing into it and reasoning backwards.
 *
 * The walk knows all of this at the moment it decides. Printing it here costs
 * one binary search and converts a crash from an invitation to guess into a
 * statement: this block, written or not, claimed by this root, reached through
 * this chain, and contested or not by Windows.
 *
 * The chain is the part worth reading. A block held at depth 1 from the game's
 * image is the game's beyond argument. The same block at depth 9 from a thread
 * stack, with Windows also claiming it, is a stale pointer in a dead frame
 * dragging somebody else's object into the save - which is the exact shape we
 * spent the night failing to see. */
static void blk_provenance(uintptr_t at)
{
	unsigned bi, hop;
	int found;

	if (!g_blk_match_n || !g_blk_save || !g_blk_wrote)
		return;
	found = blk_find(at);
	if (found < 0) {
		if (heap_is_shared(at))
			ss_raw("       provenance: on a shared heap but inside no block we "
			       "had a map for, so this restore did not write it\n");
		return;
	}
	bi = (unsigned)found;
	ss_raw("       provenance: block %p+%lx (offset %lx), %s by the last restore. "
	       "Game reach %s, Windows reach %s\n",
	       (void *)g_blk_save[bi].base, (unsigned long)g_blk_save[bi].size,
	       (unsigned long)(at - g_blk_save[bi].base),
	       g_blk_wrote[bi] ? "WRITTEN" : "not written",
	       g_blk_own && g_blk_own[bi] ? "yes" : "no",
	       g_blk_sys && g_blk_sys[bi] ? "yes" : "no");
	if (!g_blk_own || !g_blk_own[bi])
		return;
	{
		unsigned off = 0;
		uintptr_t r = g_blk_root[bi];
		const char *m = ss_module(r, &off);

		ss_raw("       claimed at depth %u from root %p%s%s\n", g_blk_depth[bi],
		       (void *)r, m ? " in " : " (not an image - a stack, arena or the "
					       "game's own data)",
		       m ? m : "");
	}
	/* Backwards along the pointers that vouched, nearest first. Capped because
	 * a deep chain says what it needs to in the first few hops. */
	for (hop = 0; hop < 6; hop++) {
		unsigned p = g_blk_parent[bi];

		if (p == SS_BLK_NOPARENT || p >= g_blk_match_n)
			break;
		ss_raw("         via %p+%lx%s\n", (void *)g_blk_save[p].base,
		       (unsigned long)g_blk_save[p].size,
		       g_blk_sys && g_blk_sys[p] ? "  <- Windows reaches this one too"
						 : "");
		bi = p;
	}
}

/* Which captured region wholly contains this block. Regions come from an
 * ascending VirtualQuery walk, so a binary search is available. */
static int blk_region_of(Slot *s, uintptr_t b, unsigned long n)
{
	int lo = 0, hi = s->nregs - 1;

	while (lo <= hi) {
		int mid = lo + ((hi - lo) >> 1);
		uintptr_t rb = s->regs[mid].base, re = rb + s->regs[mid].size;

		if (b < rb)
			hi = mid - 1;
		else if (b >= re)
			lo = mid + 1;
		else
			return (b + n <= re) ? mid : -1;
	}
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

/* Every heap this process has ever had, which is not the same question as
 * GetProcessHeaps answers.
 *
 * The check below used to enumerate the live list and validate what it found,
 * and it was blind to the one failure that kills us. On 2026-09-09 the harness
 * created a heap after a snapshot, restored, and Windows dropped that heap from
 * the process list; the very next line of the log read "4 heap(s) after the
 * restore, 0 failed" and the process then died of heap corruption. Nothing had
 * failed validation because the broken heap was never enumerated. A heap that
 * has been forgotten cannot fail a test it is not given.
 *
 * Comparing the live list against the snapshot's would not have caught it
 * either: the heap postdates the snapshot, so it is absent from both. Only a
 * record that accumulates - every heap seen at any point, never pruned - can
 * notice that something which existed a moment ago does not now.
 *
 * Held outside the snapshot, or the record would be rewound alongside the
 * registration it exists to outlive. */
#define SS_SEEN_HEAPS 256

/* Count and contents together in held memory, which the first version of this
 * got wrong in a way worth recording.
 *
 * The array was held and the count was an ordinary static. On a restore the
 * count rewound to its value at save time while the array kept the entry added
 * after it, so the record of the fifth heap was still there and the code was
 * told there were only four. The check then found nothing missing and printed
 * the same reassuring line it printed before any of this was written.
 *
 * That is the identical mistake the rasterizer made with its bin arrays and the
 * one the wrapper's data section is carefully arranged to avoid: half the state
 * in the present, half in the past. A diagnostic straddling the rewind boundary
 * reports on a world that never existed. */
typedef struct {
	int n;
	HANDLE h[SS_SEEN_HEAPS];
} SeenHeaps;

static SeenHeaps *g_seen SS_PRESENT;

static void heap_seen_init(void)
{
	if (!g_seen)
		g_seen = (SeenHeaps *)blk_arena(sizeof(SeenHeaps));
}

static int seen_has(HANDLE h)
{
	int i;

	for (i = 0; i < g_seen->n; i++)
		if (g_seen->h[i] == h)
			return 1;
	return 0;
}

/* A forgotten heap's handle usually still points at committed memory - that is
 * exactly the trap, since the code holding it has no way to tell. Asked before
 * HeapValidate, which would be walking structures Windows has disowned. */
static int heap_handle_live(HANDLE h)
{
	MEMORY_BASIC_INFORMATION mbi;

	return h && VirtualQuery((LPCVOID)h, &mbi, sizeof(mbi)) == sizeof(mbi) &&
	       mbi.State == MEM_COMMIT;
}

static void heap_forgotten_check(const HANDLE *live, DWORD nlive, const char *when)
{
	int i, lost = 0;
	DWORD k;

	heap_seen_init();
	if (!g_seen)
		return;
	/* Losses first, then additions, so a heap that appears and disappears
	 * within one interval is still reported rather than cancelling out. */
	for (i = 0; i < g_seen->n; i++) {
		int still = 0;

		for (k = 0; k < nlive; k++)
			if (live[k] == g_seen->h[i]) {
				still = 1;
				break;
			}
		if (still)
			continue;
		lost++;
		ss_log("  heap check: heap %p is GONE from the process heap list %s, but "
		       "its handle still points at %s. Anything still holding it will "
		       "allocate into a heap Windows has disowned\n",
		       (void *)g_seen->h[i], when,
		       heap_handle_live(g_seen->h[i]) ? "committed memory"
						      : "memory that is no longer committed");
	}
	if (lost)
		ss_log("  heap check: %d forgotten heap(s). This is not a validation "
		       "failure - a forgotten heap is never enumerated, so the count "
		       "above cannot see it\n",
		       lost);

	for (k = 0; k < nlive && g_seen->n < SS_SEEN_HEAPS; k++) {
		if (seen_has(live[k]))
			continue;
		g_seen->h[g_seen->n++] = live[k];
		/* A heap created after the snapshot is the one at risk, because the
		 * restore rewinds a heap list that predates it. Reported once per heap
		 * without needing a flag: this branch is inside the loop that adds to
		 * the record, and a heap is only ever added once. */
		if (g_ctl && g_ctl->nheaps) {
			int in_snap = 0, j;

			for (j = 0; j < g_ctl->nheaps; j++)
				if (g_ctl->heap_h[j] == live[k])
					in_snap = 1;
			if (!in_snap)
				ss_log("  heap check: heap %p exists now but is in no "
				       "snapshot, so a restore will rewind a heap list "
				       "that predates it\n",
				       (void *)live[k]);
		}
	}
}

/* Everything we are in a position to see, said once.
 *
 * This is the specification a shape recorder would be written against, and it
 * exists because the question "what do we actually touch" could until now only
 * be answered by reading five files. Four sections, in the order that matters to
 * a replay: what we have hooked, which heaps exist and what we do with each,
 * which modules rewind and which are left in the present, and how much memory we
 * hold outside the snapshot.
 *
 * The point is not completeness for its own sake. Every failure diagnosed today
 * was an object split across the rewind boundary - a pointer in the present to
 * memory in the past, or the reverse - and the boundary is drawn precisely by
 * the heap and module columns below. A reader who wants to know whether a given
 * object can survive a restore can now answer it from one log entry. */
/* Where the rewound world calls out to the present one.
 *
 * Every import thunk in a module that rewinds, whose target lands in a module
 * that does not, is a place where present-time code runs on behalf of past
 * state. That is tonight's entire failure class expressed as a number, and the
 * import table is the closest thing to a call graph available without tracing:
 * a static list of who calls whom across the boundary we draw.
 *
 * It also finds co-tenants. Anything that patched the game's imports to install
 * itself - a translation shim, an overlay, us - shows up as edges pointing into
 * it, which is how we learn that something else is holding the same table. A
 * name in this list that does not belong to Windows is worth a second look. */
static void iat_cross(void)
{
	static int cross[SS_MAX_MODS]; /* 1 KB, and the stack here is not ours */
	int m, k, total = 0;

	for (k = 0; k < g_ctl->nmods; k++)
		cross[k] = 0;

	for (m = 0; m < g_ctl->nmods; m++) {
		unsigned char *base;
		IMAGE_DOS_HEADER *dos;
		IMAGE_NT_HEADERS *nt;
		IMAGE_IMPORT_DESCRIPTOR *imp;
		DWORD rva;

		if (!g_ctl->mod_rewound[m])
			continue;
		base = (unsigned char *)g_ctl->mod_lo[m];
		dos = (IMAGE_DOS_HEADER *)base;
		/* Probed rather than trusted at every step. This walks headers of
		 * modules we did not build, one of which has already been patched
		 * by at least two other parties. */
		if (!ss_readable((uintptr_t)base, sizeof(*dos)) ||
		    dos->e_magic != IMAGE_DOS_SIGNATURE)
			continue;
		nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
		if (!ss_readable((uintptr_t)nt, sizeof(*nt)) ||
		    nt->Signature != IMAGE_NT_SIGNATURE)
			continue;
		rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
			      .VirtualAddress;
		if (!rva)
			continue;
		for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva);
		     ss_readable((uintptr_t)imp, sizeof(*imp)) && imp->Name; imp++) {
			void **thunk = (void **)(base + imp->FirstThunk);

			for (; ss_readable((uintptr_t)thunk, sizeof(*thunk)) && *thunk;
			     thunk++) {
				k = module_of((uintptr_t)*thunk);
				if (k < 0 || g_ctl->mod_rewound[k])
					continue;
				cross[k]++;
				total++;
			}
		}
	}

	ss_log("  crossings: %d import thunk(s) in rewound modules resolve into code "
	       "left in the present\n",
	       total);
	/* Selection sort by repeatedly taking the largest, because the list is
	 * short and printing it in descending order is the whole point: the top
	 * few names are the ones whose objects have to survive a rewind. */
	for (;;) {
		int best = -1;

		for (k = 0; k < g_ctl->nmods; k++)
			if (cross[k] > 0 && (best < 0 || cross[k] > cross[best]))
				best = k;
		if (best < 0)
			break;
		ss_log("    %5d -> %s\n", cross[best], g_ctl->mod_name[best]);
		cross[best] = 0;
	}
}

/* The crossings the import table cannot see.
 *
 * iat_cross reads a static list, and a static list only knows about functions a
 * module declared at link time. Everything reached through GetProcAddress or a
 * COM vtable is invisible to it - which is why the first run counted 274 edges
 * and not one of them pointed at dsound.dll, the module we actually crash in.
 * DxLib resolves DirectSound at runtime and then calls through vtables, so the
 * entire audio coupling leaves no trace in an import walk.
 *
 * So do it the other way round: sweep the memory that rewinds, and count every
 * stored word that lands inside a module that does not. Each one is a pointer
 * the game will still be holding after a restore, aimed at code whose own state
 * moved on without it. Function pointers, vtable pointers and cached interface
 * pointers all look the same from here, which is the point.
 *
 * Pointers into the whole image rather than only its executable sections. A COM
 * object's vtable lives in the DLL's read-only data, not its code, and testing
 * for executability would discard exactly the case worth counting. */
/* A committed-memory map, built once so the sweep does not have to ask Windows
 * about fourteen million words one at a time. */
#define SS_REGS 8192
#define SS_WRITABLE                                                                        \
	(PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)

typedef struct {
	uintptr_t lo, hi;
	uintptr_t alloc; /* the reservation this page belongs to, for grouping */
	DWORD prot;
} SsReg;

static SsReg *g_reg SS_PRESENT;
static int g_nreg;

static void reg_map_build(void)
{
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t p = 0x10000;

	g_nreg = 0;
	g_reg = (SsReg *)VirtualAlloc(NULL, SS_REGS * sizeof(SsReg), MEM_COMMIT | MEM_RESERVE,
				      PAGE_READWRITE);
	if (!g_reg)
		return;
	while (g_nreg < SS_REGS && VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
		uintptr_t base = (uintptr_t)mbi.BaseAddress;

		if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
			g_reg[g_nreg].lo = base;
			g_reg[g_nreg].hi = base + mbi.RegionSize;
			g_reg[g_nreg].alloc = (uintptr_t)mbi.AllocationBase;
			g_reg[g_nreg].prot = mbi.Protect;
			g_nreg++;
		}
		p = base + mbi.RegionSize;
		if (p <= base)
			break;
	}
}

static void reg_map_free(void)
{
	if (g_reg)
		VirtualFree(g_reg, 0, MEM_RELEASE);
	g_reg = NULL;
	g_nreg = 0;
}

/* Returns the protection of the page holding v, or 0 if v does not point at
 * committed memory - which is the cheapest possible test for "this word is not
 * a pointer at all". */
static int reg_of(uintptr_t v)
{
	int lo = 0, hi = g_nreg - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;

		if (v < g_reg[mid].lo)
			hi = mid - 1;
		else if (v >= g_reg[mid].hi)
			lo = mid + 1;
		else
			return mid;
	}
	return -1;
}

/* Where the memory we are not capturing actually is.
 *
 * The bucket counts say how much of the game's state points somewhere a restore
 * will not fix, but not which somewhere, and a number that large is useless
 * without a name attached. Grouping by allocation base turns it into a short
 * list of reservations that can be looked up, sized, and either brought into the
 * snapshot or ruled out deliberately. */
#define SS_PRIV_TOP 96

typedef struct {
	uintptr_t alloc;
	int hits;
} PrivHit;

static PrivHit g_priv[SS_PRIV_TOP];
static int g_npriv;

static void priv_note(uintptr_t alloc)
{
	int i, worst = 0;

	for (i = 0; i < g_npriv; i++)
		if (g_priv[i].alloc == alloc) {
			g_priv[i].hits++;
			return;
		}
	if (g_npriv < SS_PRIV_TOP) {
		g_priv[g_npriv].alloc = alloc;
		g_priv[g_npriv].hits = 1;
		g_npriv++;
		return;
	}
	/* Full: evict the least-referenced entry. The list is for finding the big
	 * reservations, and anything that loses a race with a single hit was never
	 * going to be one of them. */
	for (i = 1; i < g_npriv; i++)
		if (g_priv[i].hits < g_priv[worst].hits)
			worst = i;
	if (g_priv[worst].hits <= 1) {
		g_priv[worst].alloc = alloc;
		g_priv[worst].hits = 1;
	}
}

static int looks_like_vtable(uintptr_t v, int m)
{
	const uintptr_t *t = (const uintptr_t *)v;
	int i;

	/* A vtable is an array of pointers into the module that defines it, and
	 * four in a row is enough: the odds of texture data producing four
	 * consecutive words that all land inside one particular image are
	 * negligible, while every COM interface worth finding has far more.
	 *
	 * The readability probe covers all four at once. It is the expensive part
	 * of this test, so it runs on the sixty thousand candidates rather than
	 * the fourteen million words they were drawn from. */
	if (v & (sizeof(uintptr_t) - 1))
		return 0;
	if (!ss_readable(v, 4 * sizeof(uintptr_t)))
		return 0;
	for (i = 0; i < 4; i++)
		if (module_of(t[i]) != m)
			return 0;
	return 1;
}

/* Rewound heap memory to scan, by index: the segments of rewound Windows heaps,
 * then one more - the pinned game heap when our own allocator serves it, which
 * makes it no Windows heap and so absent from the segment list. */
static int rewound_span(int sg, uintptr_t *lo, uintptr_t *hi)
{
	uintptr_t b[4], z[4];
	int k, owner = -1;

	if (sg < g_ctl->nseg) {
		for (k = 0; k < g_ctl->nheaps; k++)
			if (g_ctl->heap_h[k] == g_ctl->seg_owner[sg]) {
				owner = k;
				break;
			}
		if (owner < 0 || !g_ctl->heap_ours[owner])
			return 0;
		*lo = g_ctl->seg_base[sg];
		*hi = *lo + g_ctl->seg_size[sg];
		return 1;
	}
	if (gameheap_anchor_ranges(b, z, 4) < 1)
		return 0;
	for (k = 0; k < g_ctl->nseg; k++)
		if (g_ctl->seg_base[k] == b[0])
			return 0;
	*lo = b[0];
	*hi = b[0] + z[0];
	return 1;
}

static void ptr_cross(Slot *s)
{
	static int cross[SS_MAX_MODS], vt[SS_MAX_MODS];
	int k, sg, total = 0, vtotal = 0;
	int n_moves = 0, n_frozen = 0, n_heldheap = 0, n_moddata = 0, n_private = 0;
	double mb = 0;
	LARGE_INTEGER t0, t1, freq;

	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	reg_map_build();
	g_npriv = 0;
	for (k = 0; k < g_ctl->nmods; k++)
		cross[k] = vt[k] = 0;

	for (sg = 0; sg <= g_ctl->nseg; sg++) {
		uintptr_t p, end;

		if (!rewound_span(sg, &p, &end))
			continue;

		/* Walked by region rather than read straight through. A heap
		 * segment is reserved as a unit and committed in pieces, so the
		 * uncommitted tail is a fault waiting for anyone who assumes the
		 * whole span is there. */
		while (p < end) {
			MEMORY_BASIC_INFORMATION mbi;
			uintptr_t stop, q;

			if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) != sizeof(mbi))
				break;
			stop = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
			if (stop > end)
				stop = end;
			if (mbi.State != MEM_COMMIT ||
			    (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
				p = stop;
				continue;
			}
			mb += (double)(stop - p);
			for (q = p & ~(uintptr_t)(sizeof(uintptr_t) - 1); q + sizeof(uintptr_t) <= stop;
			     q += sizeof(uintptr_t)) {
				uintptr_t v = *(uintptr_t *)q;
				int m;

				DWORD prot;
				int h, r;

				if (v < 0x10000)
					continue;
				r = reg_of(v);
				if (r < 0)
					continue; /* points at nothing: not a pointer */
				prot = g_reg[r].prot;
				total++;

				h = heap_index_of(v);
				m = module_of(v);
				/* Asked of the snapshot directly, rather than inferred
				 * from which heap or module the target sits in.
				 *
				 * The inferred version reported 1.85 million pointers
				 * into "held private memory belonging to no heap or
				 * module" - sixty per cent of everything - and almost all
				 * of it was the game's own memory. DxLib keeps its bulk
				 * state in large private VirtualAlloc regions that belong
				 * to no heap and no module, so a test phrased in terms of
				 * heaps and modules cannot see that we capture them, and
				 * files the game's own data under held. The captured
				 * region list knows the answer without having to guess.
				 */
				if (blk_region_of(s, v, 1) >= 0) {
					n_moves++;
					continue;
				}
				/* Immutable in the present, so a restore cannot make it
				 * disagree with the pointer. Code, read-only data and
				 * every static vtable land here, which is why the
				 * section-based reading of this census was measuring the
				 * safest category and calling it a risk. */
				if (!(prot & SS_WRITABLE)) {
					n_frozen++;
					if (m >= 0 && looks_like_vtable(v, m)) {
						vt[m]++;
						vtotal++;
					}
					continue;
				}
				if (h >= 0)
					n_heldheap++;
				else if (m >= 0)
					n_moddata++;
				else {
					n_private++;
					priv_note(g_reg[r].alloc);
				}
				if (m >= 0)
					cross[m]++;
			}
			p = stop;
		}
	}

	QueryPerformanceCounter(&t1);
	/* Sorted by whether a restore can make the target disagree with the pointer,
	 * which is the only property that decides whether a crossing is a hazard.
	 * Where the target sits in a PE image is a proxy for that at best: .rdata is
	 * usually read-only and .data usually is not, but the page protection says
	 * so directly and says it for heap objects too, which have no sections. */
	ss_log("  pointer targets: %d pointer-shaped word(s) in rewound memory, %.1f MB "
	       "scanned, %.1f ms\n",
	       total, mb / (1024.0 * 1024.0),
	       (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart);
	ss_log("    %8d  rewind with us, so pointer and target stay in step\n", n_moves);
	ss_log("    %8d  held but immutable - code, read-only data, %d static vtable(s). "
	       "Identical after a restore, so harmless\n",
	       n_frozen, vtotal);
	ss_log("    %8d  HELD and WRITABLE, in a heap left in the present: objects owned "
	       "by present-time modules, which moved on while we went back\n",
	       n_heldheap);
	ss_log("    %8d  HELD and WRITABLE, in a module's data section\n", n_moddata);
	ss_log("    %8d  HELD and WRITABLE, private memory belonging to no heap or module\n",
	       n_private);
	ss_log("  the last three lines are the hazard set: %d word(s). Writable is a "
	       "capability, not proof of a write, so this is an upper bound\n",
	       n_heldheap + n_moddata + n_private);

	/* Named, because "863196 words point at memory we do not restore" is not a
	 * finding until someone can say which memory. */
	if (n_private) {
		int shown;

		ss_log("  the uncaptured reservations those private pointers land in:\n");
		for (shown = 0; shown < 12; shown++) {
			int best = -1, i;
			double sz = 0;
			DWORD prot = 0;

			for (i = 0; i < g_npriv; i++)
				if (g_priv[i].hits > 0 &&
				    (best < 0 || g_priv[i].hits > g_priv[best].hits))
					best = i;
			if (best < 0)
				break;
			for (i = 0; i < g_nreg; i++)
				if (g_reg[i].alloc == g_priv[best].alloc) {
					sz += (double)(g_reg[i].hi - g_reg[i].lo);
					prot = g_reg[i].prot;
				}
			ss_log("    %p  %8d hit(s), %8.2f MB committed, prot %lX\n",
			       (void *)g_priv[best].alloc, g_priv[best].hits,
			       sz / (1024.0 * 1024.0), (unsigned long)prot);
			g_priv[best].hits = 0;
		}
	}
	reg_map_free();
	/* Sorted by hits per megabyte of image, not by hits.
	 *
	 * The raw count is confounded by how big a target each module is: 56 MB of
	 * texture and audio data contains a lot of words that happen to fall in the
	 * numeric range of a large DLL, so steamclient and DbgHelp rose to the top
	 * of the first listing purely by being enormous. If the density is flat
	 * across modules the whole column is that kind of coincidence; a module
	 * that stands out is being genuinely referenced.
	 *
	 * The vtable count is the one that means something on its own. A pointer
	 * into a held image is harmless in itself - the image does not move and its
	 * contents do not change - but a vtable pointer sitting in a rewound heap
	 * says an object belonging to that module is living on memory we rewind,
	 * which is the split that has been killing us. */
	ss_log("    %-28s %8s %9s %8s\n", "module", "hits", "per MB", "vtables");
	for (;;) {
		int best = -1;
		double bd = 0, d;

		for (k = 0; k < g_ctl->nmods; k++) {
			if (cross[k] <= 0)
				continue;
			d = (double)cross[k] /
			    ((double)(g_ctl->mod_hi[k] - g_ctl->mod_lo[k]) / (1024.0 * 1024.0));
			if (best < 0 || d > bd) {
				best = k;
				bd = d;
			}
		}
		if (best < 0)
			break;
		ss_log("    %-28s %8d %9.0f %8d\n", g_ctl->mod_name[best], cross[best], bd,
		       vt[best]);
		cross[best] = 0;
	}
}

/* What changed that we do not put back.
 *
 * Every check in this engine compares captured memory against the snapshot, so
 * every check is blind to the same thing: memory outside the captured set. The
 * pointer census can say that 863196 words point at memory a restore will not
 * fix, but not which memory, and not whether the game actually writes to it.
 *
 * So hash every committed region at save, re-hash after restore, and report the
 * ones that changed and are not ours to put back. A region that changed and is
 * captured is normal - that is the write set. A region that changed and is NOT
 * captured is the game carrying state forward through a restore, which is the
 * one failure no amount of pointer classification can find.
 *
 * The record lives in the arena rather than our data section because our data
 * section rewinds now: a table saved before the restore and read after it would
 * revert to the values it held at the snapshot, and quietly compare the save
 * against itself. */
#define SS_COV_MAX 6144

typedef struct {
	uintptr_t base, size;
	unsigned long long hash;
	DWORD prot, type;
} CovReg;

typedef struct {
	int n, truncated;
	CovReg r[SS_COV_MAX];
} Coverage;

static Coverage *g_cov SS_PRESENT;

static int coverage_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_COVERAGE", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] != '0') ? 1 : 0;
	}
	return cached;
}

/* FNV-1a over words rather than bytes. Eight times fewer rounds on a few
 * hundred megabytes is the difference between a diagnostic and a stall, and the
 * tail past the last whole word cannot change without some word changing too. */
static unsigned long long cov_hash(uintptr_t p, uintptr_t n)
{
	const unsigned long long *q = (const unsigned long long *)p;
	unsigned long long h = 1469598103934665603ULL;
	uintptr_t i, w = n / sizeof(*q);

	for (i = 0; i < w; i++)
		h = (h ^ q[i]) * 1099511628211ULL;
	return h;
}

/* Called before the snapshot is taken, not when the record is first needed.
 *
 * The arena the record lives in is excluded, but the pointer to it is not: it is
 * an ordinary global in a data section that rewinds. Allocate it during the save
 * and the snapshot captures a null, so every restore reverts the pointer and the
 * check silently does nothing - which is exactly how the forgotten-heap record
 * failed earlier tonight. A held allocation still needs a held-looking pointer,
 * and the only way to get one is to exist before the photograph. */
static void coverage_init(void)
{
	if (coverage_mode() && !g_cov)
		g_cov = (Coverage *)blk_arena(sizeof(Coverage));
}

/* Same reason coverage_init exists, and the same trap: the table is held but the
 * pointer to it is not, so it has to be non-null before the snapshot copies it.
 * Disarmed while it is being cleared, so a free landing mid-wipe is dropped
 * rather than recorded into a slot that is about to be zeroed. */
static void freed_arm(void)
{
	int i;

	if (!g_freed) {
		g_freed = (FreedSet *)blk_arena(sizeof(FreedSet));
		if (!g_freed)
			return;
	}
	InterlockedExchange(&g_freed_arm, 0);
	for (i = 0; i < SS_FREED_SLOTS; i++)
		g_freed->slot[i] = 0;
	InterlockedExchange(&g_freed->used, 0);
	InterlockedExchange(&g_freed->overflow, 0);
	InterlockedExchange(&g_freed_arm, 1);
}

static void coverage_take(void)
{
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t p = 0x10000;
	LARGE_INTEGER t0, t1, freq;
	double mb = 0;

	if (!coverage_mode())
		return;
	coverage_init();
	if (!g_cov)
		return;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	g_cov->n = 0;
	g_cov->truncated = 0;
	while (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
		uintptr_t base = (uintptr_t)mbi.BaseAddress;

		/* The adapter's mappings are left out here for the same reason the
		 * snapshot leaves them out, and the cost of not doing so was
		 * startling: this pass ran 1185.8 MB in 212.7 ms before a hardware
		 * backend existed and 1020.0 MB in 4962.8 ms after - less memory,
		 * twenty-three times slower. The difference was 124 MB of
		 * write-combined aperture being hashed at about 26 MB/s, which is
		 * simply what an uncached read across the bus costs. It was ninety
		 * percent of every save. */
		if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
		    !region_is_device(mbi.Protect)) {
			if (g_cov->n >= SS_COV_MAX) {
				g_cov->truncated = 1;
				break;
			}
			g_cov->r[g_cov->n].base = base;
			g_cov->r[g_cov->n].size = mbi.RegionSize;
			g_cov->r[g_cov->n].prot = mbi.Protect;
			g_cov->r[g_cov->n].type = mbi.Type;
			g_cov->r[g_cov->n].hash = cov_hash(base, mbi.RegionSize);
			g_cov->n++;
			mb += (double)mbi.RegionSize;
		}
		p = base + mbi.RegionSize;
		if (p <= base)
			break;
	}
	QueryPerformanceCounter(&t1);
	ss_log("  coverage: %d committed region(s) fingerprinted, %.1f MB, %.1f ms%s\n",
	       g_cov->n, mb / (1024.0 * 1024.0),
	       (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart,
	       g_cov->truncated ? " (TRUNCATED - raise SS_COV_MAX)" : "");
}

/* The first SS_BND_N rewound heap words, in segment order, whose targets lie in
 * a heap left in the present - the same test ptr_cross counts as HELD and
 * WRITABLE, in a heap. Targets closer than 256 bytes to an earlier sample are
 * skipped so five samples are five objects, not one object five times. */
static void bnd_sample(Slot *s)
{
	int sg, n = 0;

	s->nbnd = 0;
	if (!g_ctl)
		return;
	reg_map_build();
	if (!g_reg)
		return;
	for (sg = 0; sg <= g_ctl->nseg && n < SS_BND_N; sg++) {
		uintptr_t p, end;

		if (!rewound_span(sg, &p, &end))
			continue;
		while (p < end && n < SS_BND_N) {
			MEMORY_BASIC_INFORMATION mbi;
			uintptr_t stop, q;

			if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) != sizeof(mbi))
				break;
			stop = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
			if (stop > end)
				stop = end;
			if (mbi.State != MEM_COMMIT ||
			    (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
				p = stop;
				continue;
			}
			for (q = p & ~(uintptr_t)3; q + 4 <= stop && n < SS_BND_N; q += 4) {
				uintptr_t v = *(uintptr_t *)q;
				int r, h, j, w;

				if (v < 0x10000 || (v & 3))
					continue;
				r = reg_of(v);
				if (r < 0 || !(g_reg[r].prot & SS_WRITABLE))
					continue;
				h = heap_index_of(v);
				if (h < 0 || blk_region_of(s, v, 1) >= 0)
					continue;
				if (reg_of(v + SS_BND_W * 4 - 1) < 0)
					continue;
				for (j = 0; j < n; j++)
					if (v - s->bnd[j].target + 256 < 512)
						break;
				if (j < n)
					continue;
				s->bnd[n].holder = q;
				s->bnd[n].target = v;
				s->bnd[n].heap = (uintptr_t)g_ctl->heap_h[h];
				memcpy(s->bnd[n].heap_name, g_ctl->heap_name[h],
				       sizeof(s->bnd[n].heap_name));
				s->bnd[n].heap_name[sizeof(s->bnd[n].heap_name) - 1] = 0;
				memcpy(s->bnd[n].fp, (const void *)v, sizeof(s->bnd[n].fp));
				s->bnd[n].ptrmask = 0;
				for (w = 0; w < SS_BND_W; w++)
					if (s->bnd[n].fp[w] >= 0x10000 && reg_of(s->bnd[n].fp[w]) >= 0 &&
					    module_of(s->bnd[n].fp[w]) < 0)
						s->bnd[n].ptrmask |= 1u << w;
				ss_log("  BOUNDARY sample %d: holder %08lX -> target %08lX in heap "
				       "%08lX (%s), +%lX into it, pointer words mask %02X\n",
				       n + 1, (unsigned long)q, (unsigned long)v,
				       (unsigned long)s->bnd[n].heap, s->bnd[n].heap_name,
				       (unsigned long)(v - s->bnd[n].heap), s->bnd[n].ptrmask);
				n++;
			}
			p = stop;
		}
	}
	s->nbnd = n;
	reg_map_free();
	if (n < SS_BND_N)
		ss_log("  BOUNDARY: only %d sample(s) found\n", n);
}

/* Where did each sampled target go in this launch? The saved bytes are looked
 * for in every private committed region outside our pinned spans, skipping
 * the words that were pointers when saved. "actual" is the match nearest the
 * saved address. */
static void bnd_report(Slot *s)
{
	static unsigned char buf[0x10000 + 64];
	uintptr_t pb[4], ps[4], best[SS_BND_N], p = 0x10000;
	const char *pw[4];
	int np, i, hits[SS_BND_N], anchor[SS_BND_N], uniq = 0, many = 0, none = 0;

	if (!s->nbnd) {
		ss_log("\n##### BOUNDARY: this slot carries no samples #####\n");
		return;
	}
	np = gameheap_pin_ranges(pb, ps, pw, 4);
	for (i = 0; i < s->nbnd; i++) {
		int w;

		hits[i] = 0;
		best[i] = 0;
		anchor[i] = -1;
		for (w = 0; w < SS_BND_W && anchor[i] < 0; w++)
			if (!(s->bnd[i].ptrmask & (1u << w)) && s->bnd[i].fp[w])
				anchor[i] = w;
	}
	for (;;) {
		MEMORY_BASIC_INFORMATION mbi;
		uintptr_t base, stop, a;
		int k, skip = 0;

		if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) != sizeof(mbi))
			break;
		base = (uintptr_t)mbi.BaseAddress;
		stop = base + mbi.RegionSize;
		if (stop <= base)
			break;
		for (k = 0; k < np; k++)
			if (base < pb[k] + ps[k] && stop > pb[k])
				skip = 1;
		if (skip || mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE ||
		    (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
			p = stop;
			continue;
		}
		for (a = base; a < stop; a += 0x10000) {
			SIZE_T want = stop - a, got = 0, q;

			if (want > sizeof(buf))
				want = sizeof(buf);
			if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, buf, want, &got))
				continue;
			for (q = 0; q + SS_BND_W * 4 <= got && q < 0x10000; q += 4) {
				const uintptr_t *c = (const uintptr_t *)(buf + q);
				uintptr_t at = a + q;

				if (at >= (uintptr_t)s && at < (uintptr_t)(s + 1))
					continue;
				for (i = 0; i < s->nbnd; i++) {
					int w;

					if (anchor[i] < 0 || c[anchor[i]] != s->bnd[i].fp[anchor[i]])
						continue;
					for (w = 0; w < SS_BND_W; w++)
						if (!(s->bnd[i].ptrmask & (1u << w)) &&
						    c[w] != s->bnd[i].fp[w])
							break;
					if (w < SS_BND_W)
						continue;
					hits[i]++;
					if (!best[i] ||
					    (at > s->bnd[i].target ? at - s->bnd[i].target
								   : s->bnd[i].target - at) <
						(best[i] > s->bnd[i].target ? best[i] - s->bnd[i].target
									    : s->bnd[i].target - best[i]))
						best[i] = at;
				}
			}
		}
		p = stop;
	}

	ss_log("\n################################################################\n");
	for (i = 0; i < s->nbnd; i++) {
		uintptr_t t = s->bnd[i].target;

		if (anchor[i] < 0) {
			ss_log("##### BOUNDARY %d/%d  heap: %08lX  wants: %08lX  actual: ????????  "
			       "offset: n/a  (all saved words were pointers or zero)\n",
			       i + 1, s->nbnd, (unsigned long)s->bnd[i].heap, (unsigned long)t);
			ss_log("      saved in %s, +%lX into it; words %08lX %08lX %08lX %08lX "
			       "%08lX %08lX %08lX %08lX; holder %08lX\n",
			       s->bnd[i].heap_name, (unsigned long)(t - s->bnd[i].heap),
			       (unsigned long)s->bnd[i].fp[0], (unsigned long)s->bnd[i].fp[1],
			       (unsigned long)s->bnd[i].fp[2], (unsigned long)s->bnd[i].fp[3],
			       (unsigned long)s->bnd[i].fp[4], (unsigned long)s->bnd[i].fp[5],
			       (unsigned long)s->bnd[i].fp[6], (unsigned long)s->bnd[i].fp[7],
			       (unsigned long)s->bnd[i].holder);
			none++;
			continue;
		}
		if (!hits[i]) {
			ss_log("##### BOUNDARY %d/%d  heap: %08lX  wants: %08lX  actual: --------  "
			       "offset: n/a  (0 matches)\n",
			       i + 1, s->nbnd, (unsigned long)s->bnd[i].heap, (unsigned long)t);
			ss_log("      saved in %s, +%lX into it; words %08lX %08lX %08lX %08lX "
			       "%08lX %08lX %08lX %08lX, mask %02X; holder %08lX\n",
			       s->bnd[i].heap_name, (unsigned long)(t - s->bnd[i].heap),
			       (unsigned long)s->bnd[i].fp[0], (unsigned long)s->bnd[i].fp[1],
			       (unsigned long)s->bnd[i].fp[2], (unsigned long)s->bnd[i].fp[3],
			       (unsigned long)s->bnd[i].fp[4], (unsigned long)s->bnd[i].fp[5],
			       (unsigned long)s->bnd[i].fp[6], (unsigned long)s->bnd[i].fp[7],
			       s->bnd[i].ptrmask, (unsigned long)s->bnd[i].holder);
			none++;
			continue;
		}
		{
			int h = heap_index_of(best[i]);
			long d = (long)(best[i] - t);

			ss_log("##### BOUNDARY %d/%d  heap: %08lX  wants: %08lX  actual: %08lX  "
			       "offset: %s%lX  (%d match%s)\n",
			       i + 1, s->nbnd, (unsigned long)s->bnd[i].heap, (unsigned long)t,
			       (unsigned long)best[i], d < 0 ? "-" : "+",
			       (unsigned long)(d < 0 ? -d : d), hits[i], hits[i] == 1 ? "" : "es");
			ss_log("      saved in %s, +%lX into it; now in %s %08lX, +%lX into it; "
			       "holder %08lX\n",
			       s->bnd[i].heap_name, (unsigned long)(t - s->bnd[i].heap),
			       h >= 0 ? g_ctl->heap_name[h] : "no heap",
			       h >= 0 ? (unsigned long)(uintptr_t)g_ctl->heap_h[h] : 0ul,
			       h >= 0 ? (unsigned long)(best[i] - (uintptr_t)g_ctl->heap_h[h]) : 0ul,
			       (unsigned long)s->bnd[i].holder);
			if (hits[i] == 1)
				uniq++;
			else
				many++;
		}
	}
	ss_log("##### BOUNDARY ACCURACY: %d of %d found exactly once, %d ambiguous, %d "
	       "not found\n",
	       uniq, s->nbnd, many, none);
	/* The holder's own address in this launch, before the restore. Where the
	 * holder is the same structure in both launches - a heap header at a
	 * pinned address - this is where the pointer should go, no search needed. */
	for (i = 0; i < s->nbnd; i++) {
		uintptr_t v = 0, t = s->bnd[i].target;
		SIZE_T got = 0;
		long d;

		if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)s->bnd[i].holder, &v,
				       sizeof(v), &got) ||
		    got != sizeof(v)) {
			ss_log("##### LIVE %d/%d  holder: %08lX  wants: %08lX  live: unmapped\n",
			       i + 1, s->nbnd, (unsigned long)s->bnd[i].holder, (unsigned long)t);
			continue;
		}
		d = (long)(v - t);
		ss_log("##### LIVE %d/%d  holder: %08lX  wants: %08lX  live: %08lX  offset: %s%lX"
		       "  %s\n",
		       i + 1, s->nbnd, (unsigned long)s->bnd[i].holder, (unsigned long)t,
		       (unsigned long)v, d < 0 ? "-" : "+", (unsigned long)(d < 0 ? -d : d),
		       !hits[i]           ? "(search found nothing to compare)"
		       : v == best[i]     ? "(AGREES with the search)"
		       : hits[i] > 1      ? "(differs from the nearest of the search's matches)"
					  : "(DIFFERS from the search)");
	}
	ss_log("################################################################\n\n");
}

static void coverage_check(Slot *s)
{
	int i, changed = 0, uncap = 0, vanished = 0, shown;
	double uncap_mb = 0;
	LARGE_INTEGER t0, t1, freq;
	static int flag[SS_COV_MAX];

	if (!coverage_mode() || !g_cov || !g_cov->n)
		return;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	for (i = 0; i < g_cov->n; i++) {
		MEMORY_BASIC_INFORMATION mbi;

		flag[i] = 0;
		/* Re-queried rather than assumed still there. A region that was
		 * decommitted between the save and now cannot be compared, and
		 * counting it as changed would be a lie with the same shape as the
		 * finding we are looking for. */
		if (VirtualQuery((LPCVOID)g_cov->r[i].base, &mbi, sizeof(mbi)) != sizeof(mbi) ||
		    mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
		    (uintptr_t)mbi.BaseAddress != g_cov->r[i].base ||
		    mbi.RegionSize != g_cov->r[i].size) {
			vanished++;
			continue;
		}
		if (cov_hash(g_cov->r[i].base, g_cov->r[i].size) == g_cov->r[i].hash)
			continue;
		changed++;
		if (blk_region_of(s, g_cov->r[i].base, 1) >= 0)
			continue; /* captured: this is the write set, and expected */
		flag[i] = 1;
		uncap++;
		uncap_mb += (double)g_cov->r[i].size;
	}
	QueryPerformanceCounter(&t1);

	ss_log("  coverage: %d of %d region(s) differ from the save, %d of those are NOT "
	       "captured (%.2f MB), %d could not be compared, %.1f ms\n",
	       changed, g_cov->n, uncap, uncap_mb / (1024.0 * 1024.0), vanished,
	       (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart);
	if (!uncap)
		return;
	ss_log("  coverage: the game changed these and a restore does not put them back:\n");
	for (shown = 0; shown < 16; shown++) {
		int best = -1;

		for (i = 0; i < g_cov->n; i++)
			if (flag[i] && (best < 0 || g_cov->r[i].size > g_cov->r[best].size))
				best = i;
		if (best < 0)
			break;
		/* Why we skipped it, not just that we did. region_wanted rejects on
		 * type, on protection and on the exclusion list, and those have
		 * completely different answers: a mapped file cannot simply be added
		 * to the snapshot, while an over-eager exclusion can be removed. */
		ss_log("    %p  %9.2f MB  prot %lX  type %s  %s\n",
		       (void *)g_cov->r[best].base,
		       (double)g_cov->r[best].size / (1024.0 * 1024.0),
		       (unsigned long)g_cov->r[best].prot,
		       g_cov->r[best].type == MEM_PRIVATE  ? "PRIVATE"
		       : g_cov->r[best].type == MEM_MAPPED ? "MAPPED "
		       : g_cov->r[best].type == MEM_IMAGE  ? "IMAGE  "
							   : "?      ",
		       region_why(g_cov->r[best].base, g_cov->r[best].size));
		flag[best] = 0;
	}
}

static void ss_inventory(Slot *s)
{
	static int told;
	HANDLE live[SS_MAX_HEAPS];
	DWORD nlive, i;
	int k;

	char v[8];

	if (told || !g_ctl)
		return;
	told = 1;
	if (!ss_getenv("D3D9SW_INVENTORY", v, sizeof(v)) || v[0] == '0')
		return;

	ss_log("\n===== inventory: what this wrapper can observe =====\n");

	ss_log("  hooks: %d installed\n", g_hook_n);
	for (k = 0; k < g_hook_n; k++) {
		/* Zero is printed, not hidden. An import sweep that patched nothing
		 * looks exactly like a healthy hook if the count is only shown when
		 * it is interesting, and a hook that catches nothing is the quietest
		 * way this wrapper can be wrong. */
		if (strcmp(g_hook[k].kind, "iat") == 0)
			ss_log("    %-6s %-38s at %p, %d import site(s)%s\n", g_hook[k].kind,
			       g_hook[k].what, g_hook[k].at, g_hook[k].sites,
			       g_hook[k].sites ? "" : "  <- PATCHED NOTHING");
		else
			ss_log("    %-6s %-38s at %p\n", g_hook[k].kind, g_hook[k].what,
			       g_hook[k].at);
	}

	nlive = GetProcessHeaps(SS_MAX_HEAPS, live);
	if (nlive > SS_MAX_HEAPS)
		nlive = SS_MAX_HEAPS;
	ss_log("  heaps: %u live\n", (unsigned)nlive);
	for (i = 0; i < nlive; i++) {
		int segs = 0, ours = 0;
		double bytes = 0;
		const char *name = "";

		/* Summed rather than looked up. A heap with more than one segment
		 * holds a separate control entry per segment under the same handle,
		 * so taking the first match reports one segment and calls it the
		 * heap - which is how this line first claimed the process heap was
		 * a megabyte when its own census had found ninety. */
		for (k = 0; k < g_ctl->nheaps; k++) {
			if (g_ctl->heap_h[k] != live[i])
				continue;
			segs++;
			bytes += (double)(g_ctl->heap_hi[k] - g_ctl->heap_lo[k]);
			ours = g_ctl->heap_ours[k];
			if (g_ctl->heap_name[k][0])
				name = g_ctl->heap_name[k];
		}
		if (!segs) {
			ss_log("    %p  NOT in the snapshot - a restore will rewind a "
			       "heap list that predates it\n",
			       (void *)live[i]);
			continue;
		}
		ss_log("    %p  %-21s %2d segment(s), %7.2f MB%s%s\n", (void *)live[i],
		       ours ? "REWOUND with the game" : "left in the present", segs,
		       bytes / (1024.0 * 1024.0), name[0] ? "  " : "", name);
	}

	/* The tenancy list. A module left in the present whose objects live on a
	 * rewound heap is the split that produced the dsound and overlay crashes,
	 * so this column and the one above are meant to be read together. */
	{
		int rew = 0, held = 0;

		for (k = 0; k < g_ctl->nmods; k++)
			g_ctl->mod_rewound[k] ? rew++ : held++;
		ss_log("  modules: %d rewound with the game, %d left in the present\n", rew,
		       held);
		/* The short list first, because it is the one worth checking. Anything
		 * of ours that rewinds while the rest of our own wrapper is held is a
		 * split we inflicted on ourselves, and reading it off the absence of a
		 * name from a list of seventy-six is not reading it at all. */
		for (k = 0; k < g_ctl->nmods; k++)
			if (g_ctl->mod_rewound[k])
				ss_log("    REWOUND: %s\n", g_ctl->mod_name[k]);
		for (k = 0; k < g_ctl->nmods; k++)
			if (!g_ctl->mod_rewound[k])
				ss_log("    held: %s\n", g_ctl->mod_name[k]);
	}

	iat_cross();
	ptr_cross(s);

	ss_log("  threads: %d tracked at the last snapshot\n", g_ctl->nids);
	ss_log("  excluded: %d region(s) held outside every snapshot\n", g_ctl->nex);
	ss_log("===== end inventory =====\n\n");
}

/* linux 61185: restore 1 and 2 lived, then HeapValidate of held process
 * heap 00150000 FAILED after restore 2. Load 3 never logged load: slot —
 * do_load's first act is heap_check("as the restore begins"), which takes
 * that heap's lock while Present is in request() and DXVK may be in
 * RtlAllocateHeap. Wine heap is not Windows HEAP. Do not HEAPWALK it.
 * Do not hold process-heap growth. Skip HeapValidate for heaps we leave
 * in the present so the next KEY_2 can start. */
static int wine_heap_held(HANDLE h)
{
	int k;

	if (!ss_under_wine() || !h)
		return 0;
	if (h == GetProcessHeap())
		return 1;
	if (!g_ctl || g_ctl->nheaps <= 0)
		return 0;
	for (k = 0; k < g_ctl->nheaps && k < SS_MAX_HEAPS; k++)
		if (g_ctl->heap_h[k] == h)
			return !g_ctl->heap_ours[k];
	return 0;
}

static void heap_check(const char *when)
{
	HANDLE heaps[SS_MAX_HEAPS], failed[SS_MAX_HEAPS];
	DWORD n, i;
	int bad = 0, skipped = 0;
	LARGE_INTEGER freq, t0, t1;

	if (!heapcheck_mode() || !g_ctl)
		return;
	n = GetProcessHeaps(SS_MAX_HEAPS, heaps);
	if (!n)
		return;
	if (n > SS_MAX_HEAPS)
		n = SS_MAX_HEAPS;
	heap_forgotten_check(heaps, n, when);
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	for (i = 0; i < n; i++) {
		if (wine_heap_held(heaps[i])) {
			skipped++;
			continue;
		}
		if (HeapValidate(heaps[i], 0, NULL))
			continue;
		if (bad < SS_MAX_HEAPS)
			failed[bad] = heaps[i];
		bad++;
		ss_log("  heap check: heap %p (%s) FAILED validation %s\n", (void *)heaps[i],
		       ss_heap_of((uintptr_t)heaps[i]), when);
	}
	QueryPerformanceCounter(&t1);
	/* The ever-seen count is printed even though nothing consumes it, because
	 * without it a quiet log is ambiguous. This check failed three times in the
	 * harness by keeping an empty record and finding nothing missing in it, and
	 * each time the log looked exactly like a healthy run. A number that tracks
	 * the live count upward is the evidence that silence means "nothing was
	 * lost" rather than "there was nothing to lose". */
	ss_log("  heap check: %u heap(s) %s, %d failed, %d skipped (held Wine), %d ever seen, %.1f ms\n",
	       (unsigned)n, when, bad, skipped, g_seen ? g_seen->n : -1,
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

/* The committed, readable part of an allocation, and nothing else.
 *
 * AllocationBase spans the whole reservation, while the MEM_COMMIT test that
 * qualified the pointer only vouched for the page it happened to land in. A
 * 64 KB private allocation with 0xC000 committed therefore got scanned to
 * 0x10000 and faulted on the first page that was never backed. The PE32 harness
 * hit this in roughly a fifth of runs with the block pass switched off, which
 * is what made it look like a savestate problem for so long: it fires from
 * build_exclusions, so both saving and restoring can trip it, and it has
 * nothing to do with what is being saved. */
static void audit_scan_alloc(uintptr_t ab, DWORD tid, const char *where, int *found)
{
	uintptr_t end = ab + alloc_span(ab), p = ab;

	while (p < end && *found < 12) {
		MEMORY_BASIC_INFORMATION mbi;
		uintptr_t stop;

		if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) != sizeof(mbi))
			break;
		stop = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		if (stop > end)
			stop = end;
		if (stop <= p)
			break;
		if (mbi.State == MEM_COMMIT &&
		    !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
			audit_scan((const uintptr_t *)p,
				   (size_t)(stop - p) / sizeof(uintptr_t), tid, where,
				   found);
		p = stop;
	}
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
			audit_scan_alloc(hops[j], g_ctl->ids[i],
					 "in its per-thread runtime state", &found);
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
	/* Which rewound heap each edge lands in. The totals say a hazard
	 * exists; naming the heap is what says whether reclassifying one
	 * would remove it, or whether the module has to be handled directly. */
	unsigned heap_hits[SS_MAX_HEAPS];
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
		if (h < SS_MAX_HEAPS)
			w->heap_hits[h]++;
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
		unsigned before[SS_MAX_HEAPS];
		unsigned mod_edges = w.nedges;
		int hk;

		if (g_ctl->mod_rewound[i])
			continue;
		for (hk = 0; hk < SS_MAX_HEAPS; hk++)
			before[hk] = w.heap_hits[hk];
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
		/* One line per module that is left in the present yet holds a
		 * pointer into memory the restore will rewind. Every crash of
		 * this shape so far - the CoreUIComponents thunk, dsound freeing
		 * 80000000 - is a module that would have appeared here. */
		mod_edges = w.nedges - mod_edges;
		if (mod_edges) {
			char line[400];
			int off = 0;

			for (hk = 0; hk < SS_MAX_HEAPS && hk < g_ctl->nheaps &&
				     off < (int)sizeof(line) - 90;
			     hk++) {
				unsigned d = w.heap_hits[hk] - before[hk];

				if (!d)
					continue;
				off += ss_fmt(line + off, (int)sizeof(line) - off,
					      "%s%p %s x%u", off ? ", " : "",
					      (void *)g_ctl->heap_lo[hk], g_ctl->heap_name[hk],
					      d);
			}
			ss_log("    HAZARD %-26s %5u pointer(s) into rewound heaps: %s\n",
			       g_ctl->mod_name[i], mod_edges, off ? line : "(non-heap)");
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

/* Set by suspend_all. build_exclusions and the settle scanners run after
 * that and must not GetThreadContext a thread we left running: under Wine
 * that is a SIGUSR1 round-trip, and NtSetContextThread ORs bit 2 into a
 * live unix-call frame. The dispatcher then reloads eax and mmdevapi's
 * audio_client_main asserts on a pulse_main_loop that returned success. */
static char g_did_suspend[SS_MAX_THREADS];

/* Clears that bit on threads we left running, and can punch the frame's
 * page out of the copy. Does not SuspendThread and does not GetThreadContext. */
static void unix_frame_shield(const char *when, int hold_pages);
static void unix_frame_heal(void);

/* Set by do_load for a load from another process, and only for that call.
 *
 * A 32-bit thread under WOW64 has a second, 64-bit TEB two pages below its own
 * and a 64-bit stack that neither TEB field of ours describes. The kernel side
 * of every wait lives there, including the copy of the thread's 32-bit
 * registers that SetThreadContext edits for a thread parked in a syscall. In
 * one process those come back consistent with the thread that owns them. From
 * another they are a stranger's: the restore wrote the old process's 64-bit
 * frames under live threads, and the pointer shift then moved registers the
 * transplant had just set a second time. */
static int g_excl_wow64;

static int wow64_exclude(unsigned char *teb, int *nstk)
{
#if defined(_M_IX86) || defined(__i386__)
	unsigned char *t64 = teb - 0x2000;
	MEMORY_BASIC_INFORMATION m;
	DWORD base;

	if (!ss_readable((uintptr_t)t64, 0x40) ||
	    *(const DWORD *)(t64 + 0x30) != (DWORD)(uintptr_t)t64 ||
	    *(const DWORD *)(t64 + 0x34))
		return 0;
	ss_exclude_as("64-bit thread environment block", t64, 0x2000);
	base = *(const DWORD *)(t64 + 0x08);
	if (!base || *(const DWORD *)(t64 + 0x0C))
		return 1;
	if (VirtualQuery((LPCVOID)(uintptr_t)(base - 1), &m, sizeof(m)) == sizeof(m) &&
	    m.AllocationBase && (uintptr_t)m.AllocationBase < base) {
		ss_exclude_as("64-bit thread stack", m.AllocationBase,
			      (size_t)(base - (uintptr_t)m.AllocationBase));
		(*nstk)++;
	}
	return 1;
#else
	(void)teb;
	(void)nstk;
	return 0;
#endif
}

/* The 64-bit half of WOW64 keeps its own process heap, below 4 GB and invisible
 * to GetProcessHeaps. Every system call that thunks a structure allocates from
 * it, so a restore from another process that writes the old launch's copy
 * under the live one leaves wow64 with a corrupted heap: the next
 * CreateToolhelp32Snapshot died inside NtQuerySystemInformation, then raised
 * c0000374. Same footing as the 64-bit stacks: kept only on a foreign load. */
static void wow64_alloc_exclude(uintptr_t p)
{
	MEMORY_BASIC_INFORMATION m;
	uintptr_t a, end;

	if (VirtualQuery((LPCVOID)p, &m, sizeof(m)) != sizeof(m) || !m.AllocationBase)
		return;
	a = end = (uintptr_t)m.AllocationBase;
	while (VirtualQuery((LPCVOID)end, &m, sizeof(m)) == sizeof(m) &&
	       (uintptr_t)m.AllocationBase == a)
		end += m.RegionSize;
	ss_exclude_as("64-bit process heap", (void *)a, end - a);
}

static int wow64_heaps_exclude(void)
{
#if defined(_M_IX86) || defined(__i386__)
	typedef LONG(NTAPI * QIP64)(HANDLE, ULONG, void *, ULONG, void *);
	typedef LONG(NTAPI * RVM64)(HANDLE, ULONGLONG, void *, ULONGLONG, ULONGLONG *);
	static QIP64 qip;
	static RVM64 rvm;
	ULONGLONG heaps[32];
	struct {
		LONG status;
		ULONG pad;
		ULONGLONG peb, affinity;
		LONG prio;
		ULONG pad2;
		ULONGLONG pid, ppid;
	} pbi;
	const unsigned char *peb;
	HANDLE self;
	int i, n, done = 0;

	if (!qip)
		qip = (QIP64)GetProcAddress(GetModuleHandleA("ntdll.dll"),
					    "NtWow64QueryInformationProcess64");
	memset(&pbi, 0, sizeof(pbi));
	if (!qip || qip(GetCurrentProcess(), 0, &pbi, sizeof(pbi), NULL) < 0 ||
	    !pbi.peb || pbi.peb >> 32 || !ss_readable((uintptr_t)pbi.peb, 0x100)) {
		ss_log("  wow64: no 64-bit PEB (query %p, status %lX, peb %llX)\n",
		       (void *)qip, pbi.status, pbi.peb);
		return 0;
	}
	peb = (const unsigned char *)(uintptr_t)pbi.peb;
	n = (int)*(const ULONG *)(peb + 0xE8);
	if (n > 32)
		n = 32;
	/* The list itself sits in the 64-bit ntdll's data, above 4 GB. */
	if (!rvm)
		rvm = (RVM64)GetProcAddress(GetModuleHandleA("ntdll.dll"),
					    "NtWow64ReadVirtualMemory64");
	/* The 64-bit read refuses the GetCurrentProcess() pseudo-handle. */
	self = OpenProcess(PROCESS_VM_READ, FALSE, GetCurrentProcessId());
	if (!rvm || !self || rvm(self, *(const ULONGLONG *)(peb + 0xF0), heaps,
				 (ULONGLONG)n * 8, NULL) < 0) {
		ss_log("  wow64: 64-bit heap list unreadable, only the process heap is kept\n");
		heaps[0] = *(const ULONGLONG *)(peb + 0x30);
		n = 1;
	}
	if (self)
		CloseHandle(self);
	/* A heap grows by new segments wherever the OS puts them, and its big
	 * blocks are allocations of their own: both lists have to be walked, or
	 * the restore still writes the old launch's copy under the live ones. The
	 * offsets are the 64-bit _HEAP's (SegmentList 0x120, VirtualAllocdBlocks
	 * 0x110, Signature 0x98), unchanged from Windows 8 to 11. */
	for (i = 0; i < n; i++) {
		ULONGLONG h = heaps[i];
		int l;

		if (!h || h >> 32 || !ss_readable((uintptr_t)h, 0x130) ||
		    *(const DWORD *)(uintptr_t)(h + 0x98) != 0xEEFFEEFFu)
			continue;
		wow64_alloc_exclude((uintptr_t)h);
		for (l = 0; l < 2; l++) {
			ULONGLONG head = h + (l ? 0x110 : 0x120), f;
			int k = 0;

			for (f = *(const ULONGLONG *)(uintptr_t)head;
			     f != head && !(f >> 32) && k < 256 && ss_readable((uintptr_t)f, 16);
			     f = *(const ULONGLONG *)(uintptr_t)f, k++)
				wow64_alloc_exclude((uintptr_t)f);
		}
		done++;
	}
	return done;
#else
	return 0;
#endif
}

static void build_exclusions(void)
{
	MODULEENTRY32 me;
	HANDLE snap;
	HMODULE self = NULL, exe = GetModuleHandleA(NULL);
	char windir[MAX_PATH];
	UINT wlen;
	int i, nstack = 0, nteb = 0, nmod = 0, ntrans = 0, nteb64 = 0, nstk64 = 0;
	char onames[8][64];
	int ocounts[8], nown = 0;
	/* The same tally for the threads we rewind, so the report describes the
	 * whole thread set rather than only the exempt half. */
	char rnames[8][64];
	int rcounts[8], nrewound = 0;

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
	snap = modsnap_use();
	if (snap && snap != INVALID_HANDLE_VALUE) {
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
				if (g_ctl->nmods < SS_MAX_MODS)
					mod_record(g_ctl->nmods++, &me, (char)(mine || !ex));
				if (mine || !ex)
					continue;
				ss_exclude_as("library image", me.modBaseAddr, me.modBaseSize);
				nmod++;
			} while (Module32Next(snap, &me));
		}
		CloseHandle(snap);
	}

	heaps_partition();

	/* This thread does the copy and is never suspended, so its 64-bit stack
	 * moves under every snapshot - the VERIFY tear on each save - and a restore
	 * would write the old frames beneath its own live syscalls. Never ours to
	 * keep, in either direction. */
	nteb64 += wow64_exclude((unsigned char *)NtCurrentTeb(), &nstk64);
	if (g_excl_wow64)
		ss_log("  wow64: %d 64-bit heap(s) kept in the present\n",
		       wow64_heaps_exclude());

	/* A reservation made by a module left in the present (Steam's tier0 takes
	 * 32 MB at a different address every launch) is that module's, not the
	 * game's: rewinding it is wrong in one session and refuses the load in the
	 * next. Needs D3D9SW_NTLOG for the origins; D3D9SW_PRESENTVA=0 turns it off. */
	{
		char pv[8];
		int nv = gameheap_va_origin_count(), vi, nheld = 0;
		uintptr_t held = 0;

		if (!(ss_getenv("D3D9SW_PRESENTVA", pv, sizeof(pv)) > 0 && pv[0] == '0'))
			for (vi = 0; vi < nv; vi++) {
				uintptr_t vb, vs;
				const char *vm;
				MEMORY_BASIC_INFORMATION vq;
				int k;

				if (!gameheap_va_origin_at(vi, &vb, &vs, &vm))
					continue;
				for (k = 0; k < g_ctl->nmods; k++)
					if (!lstrcmpiA(g_ctl->mod_name[k], vm))
						break;
				if (k == g_ctl->nmods || g_ctl->mod_rewound[k])
					continue;
				if (VirtualQuery((LPCVOID)vb, &vq, sizeof(vq)) != sizeof(vq) ||
				    vq.State == MEM_FREE || (uintptr_t)vq.AllocationBase != vb)
					continue;
				ss_exclude_as("reserved by a module left in the present",
					      (void *)vb, vs);
				nheld++;
				held += vs;
			}
		if (nheld)
			ss_log("  present modules' reservations: %d held out, %lu KB\n", nheld,
			       (unsigned long)(held >> 10));
	}

	for (i = 0; i < g_ctl->nids; i++) {
		unsigned char *teb;
		if (!g_ctl->handles[i])
			continue;
		teb = (unsigned char *)teb_of(g_ctl->handles[i]);
		if (!teb)
			continue;
		ss_exclude_as("thread environment block", teb, 0x1000);
		if (g_excl_wow64)
			nteb64 += wow64_exclude(teb, &nstk64);
		if (nteb == 0) {
#if defined(_M_IX86) || defined(__i386__)
			void *peb = *(void **)(teb + 0x30);
#else
			void *peb = *(void **)(teb + 0x60);
#endif
			if (peb)
				ss_exclude_as("process environment block", peb, 0x1000);
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
			/* The other side of the same question, counted.
			 *
			 * This test has only ever reported who we leave alone, which
			 * reads as complete until something we rewound turns out to
			 * have needed leaving alone too. A Steam worker faulted 226
			 * frames after a restore, doing a rep movsb into a read-only
			 * page of steamclient.dll's own image - a destination computed
			 * from a stack and a working buffer we had just rewound
			 * underneath it, while steamclient.dll itself was held in the
			 * present and the Steam client it talks to lives in another
			 * process entirely. It is the same shape as the display driver,
			 * which is why amdxx32 has sixteen threads on the exempt list.
			 *
			 * Steam should have been caught by the very test above, because
			 * its image is held and therefore excluded. It was not, and the
			 * reason is not visible from outside: the exempt tally names
			 * ntdll, inputhost, dinput, wdmaud, d3d11, amdxx32 and amdihk32
			 * and simply does not mention the thread that crashed. Naming
			 * the rewound side says whether its start address lands in a
			 * module we did not expect, in no module at all, or in
			 * steamclient after all - and those three want different
			 * fixes. Guessing between them is how the last two heap
			 * classifiers went wrong. */
			/* The stall watchdog is ours and starts in our image, which
			 * rewinds, so its start address alone counted it as a game
			 * thread: saved, and then unmatched in the next launch. */
			int keep_out = region_excluded((uintptr_t)g_ctl->starts[i], 1) ||
				       (g_ctl->wd_tid && g_ctl->ids[i] == g_ctl->wd_tid) ||
				       xs_spare_idx(g_ctl->ids[i]) >= 0;

			if (!keep_out)
				tally_owner(rnames, rcounts, &nrewound, g_ctl->starts[i]);
			if (keep_out) {
				g_ctl->transient[i] = 1;
				ss_exclude_as("thread stack", (void *)foot, (size_t)(hi - foot));
				tally_owner(onames, ocounts, &nown, g_ctl->starts[i]);
				/* These keep running across the rewind with their
				 * registers untouched. That is survivable while one is
				 * parked in a wait, and not survivable while one is
				 * inside the allocator, because the heap it is halfway
				 * through is about to be replaced underneath it.
				 *
				 * Read the park site only when we already suspended the
				 * thread. GetThreadContext on one we left running is the
				 * SIGUSR1 round-trip that clobbers a unix call's return. */
				if (g_ctl->handles[i] && g_did_suspend[i]) {
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
				ss_exclude_as("thread stack", (void *)foot, (size_t)(lo - foot));
			/* From another launch a live stack's address held something
			 * else in the save. The transplant writes paired stacks itself;
			 * an unpaired worker restored over came back with a broken SEH
			 * chain and fail-fasted on its next exception. */
			if (!g_excl_wow64 &&
			    (g_ctl->ids[i] == g_ctl->req_tid || rewind_all_threads()))
				continue;
			ss_exclude_as("thread stack", (void *)lo, (size_t)(hi - lo));
			nstack++;
		}
	}

	/* Our own renderer's VirtualAlloc regions - textures, surfaces, framebuffers,
	 * GPU-present staging - are ours and regenerable: the game re-creates its
	 * resources and gpu_dxgi_reconcile re-uploads the framebuffer on restore. They
	 * originate in d3d11.dll, land OS-placed, and so drift across a session, which
	 * is the bulk of what a cross-session restore cannot put back. Holding them in
	 * the present keeps them out of the snapshot entirely; the arena is already
	 * excluded above by the wrapper's own module handling, this covers everything
	 * else. gameheap records them as it sees d3d11.dll allocate, and drops them as
	 * it frees, so this list is the live set at the instant of the save. */
	{
		uintptr_t ob[256], os[256];
		int i, non = gameheap_own_va_ranges(ob, os, 256);

		for (i = 0; i < non; i++)
			ss_exclude_as("renderer VirtualAlloc, regenerated on restore",
				      (void *)ob[i], (size_t)os[i]);
		if (non)
			ss_log("exclude: %d renderer VirtualAlloc region(s) held in the "
			       "present - ours, the game rebuilds them\n",
			       non);
	}

	ss_log("exclude: %d tebs, %d game stacks, %d system threads, %d library images, "
	       "%ld total\n",
	       nteb, nstack, ntrans, nmod, (long)g_ctl->nex);
	if (g_excl_wow64)
		ss_log("exclude: load from another process - %d 64-bit TEB(s) and %d 64-bit "
		       "stack(s) of live threads held in the present\n",
		       nteb64, nstk64);
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
	if (nrewound) {
		char line[512];
		int off = 0, k;

		for (k = 0; k < nrewound && off < (int)sizeof(line) - 80; k++)
			off += ss_fmt(line + off, (int)sizeof(line) - off, "%s%s x%d",
				      k ? ", " : "", rnames[k], rcounts[k]);
		ss_log("  rewound threads belong to: %s\n", line);
	}
	/* Before the snapshot is taken, and again before a restore writes.
	 * A frame captured with bit 2 set is written back under the live call. */
	unix_frame_shield("exclusions", 0);
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

/* Reservations ensure_committed made for the load under way. A refused load
 * releases them: left behind, they sit at the saved addresses, and a module the
 * game loads later - DDPR's dat12.bin at its fixed base - is pushed elsewhere,
 * which refuses every load after it. */
#define SS_RSV_CAP 1024
static uintptr_t g_rsv[SS_RSV_CAP];
static int g_rsv_n;

static void rsv_note(uintptr_t base)
{
	if (g_rsv_n < SS_RSV_CAP)
		g_rsv[g_rsv_n++] = base;
}

static void rsv_release(void)
{
	int i, n = 0;

	for (i = 0; i < g_rsv_n; i++)
		n += VirtualFree((LPVOID)g_rsv[i], 0, MEM_RELEASE) != 0;
	if (g_rsv_n)
		ss_log("  released %d of %d reservation(s) the refused load made at saved "
		       "addresses\n",
		       n, g_rsv_n);
	g_rsv_n = 0;
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
			if (VirtualAlloc((LPVOID)rbase, (SIZE_T)(rend - rbase), MEM_RESERVE,
					 PAGE_NOACCESS)) {
				rsv_note(rbase);
			} else {
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

				if (VirtualAlloc((LPVOID)fbase, (SIZE_T)(fend - fbase),
						 MEM_RESERVE, PAGE_NOACCESS))
					rsv_note(fbase);
				else
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
/* One known-truth byte range, watched across a save and restore.
 *
 * The savestate reports success - hundreds of regions restored, threads and
 * contexts back - while the player visibly does not return to where she was.
 * Those two facts can only both be true if the thing that defines where she is
 * never travelled. F11 already proved that entity x and y are authoritative:
 * write those eight bytes and she moves and stays moved. So they are the ideal
 * witness. If they come back, the restore reached the player and the problem
 * is elsewhere; if they do not, the snapshot does not contain her and every
 * other question is premature.
 *
 * Declared here, defined beside the rest of the Rabi-Ribi code at the bottom,
 * because it needs both the Slot layout and the entity lookup. */
static void witness_save(Slot *s);
static void witness_load(void);
static int room_check(void);
static void carry_save(void);
static void carry_load(void);
static void roster_save(void);
static uintptr_t peb_base(void);
static void roster_load(void);
static void cap_record(Slot *s);

/* D3D9SW_DIFFWRITE=0 goes back to the wholesale copy, so the two can be run
 * against each other on the same save without rebuilding. */
static int diff_write(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_DIFFWRITE", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '0') ? 0 : 1;
	}
	return cached;
}

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

/* Phase 1 bank window. A contiguous reservation held from setup through the whole
 * session, released ONLY during a frozen checkpoint so the restore's views have a
 * guaranteed contiguous hole to map into, then re-held for gameplay. Reserving it
 * EARLY - before the game fragments the 2 GB - is what makes it available later,
 * once the largest free block has shrunk below a view. Multiple concurrent windows
 * (region, block, verify) all land in the freed span, so this is a block, not one
 * fixed view. The whole point: the number and size of save slots is bounded by
 * disk, not by finding a big hole at restore time. */
#define SS_BANK_RESERVE (64u * 1024u * 1024u)
static void *g_bank_reserve;
static int g_bank_released;

static void bank_reserve_init(void)
{
	if (g_bank_reserve || g_bank_released)
		return;
	g_bank_reserve = VirtualAlloc(NULL, SS_BANK_RESERVE, MEM_RESERVE, PAGE_READWRITE);
	ss_log("bank: %s a %u MB contiguous reserve for checkpoint views%s\n",
	       g_bank_reserve ? "held" : "COULD NOT hold", SS_BANK_RESERVE >> 20,
	       g_bank_reserve
		       ? " - restores get a guaranteed hole even when the address "
			 "space is fragmented"
		       : "; restores fall back to scavenging holes (the view retry)");
}

/* Free the reserve so the frozen checkpoint's views can land in the contiguous
 * span. Only ever called inside the freeze, where nothing runs to grab it. */
static void bank_release(void)
{
	if (g_bank_reserve) {
		VirtualFree(g_bank_reserve, 0, MEM_RELEASE);
		g_bank_reserve = NULL;
		g_bank_released = 1;
	}
}

/* Re-hold the span for gameplay so it is there for the next checkpoint. If the
 * game fragmented the space while we were mapped, this may land elsewhere - still
 * a 64 MB contiguous block, which is all the next restore needs. */
static void bank_reacquire(void)
{
	if (g_bank_released) {
		g_bank_reserve = VirtualAlloc(NULL, SS_BANK_RESERVE, MEM_RESERVE,
					      PAGE_READWRITE);
		g_bank_released = 0;
	}
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
	/* A 32-bit process holding the game, the arena and several ~780 MB save
	 * sections has no 32 MB CONTIGUOUS hole to map a view into, even with plenty
	 * of total address space free - MapViewOfFile returns ERROR_NOT_ENOUGH_MEMORY.
	 * Falling the whole restore over a fragmented address space leaves the game
	 * half-written; a smaller view almost always fits, at the cost of more slides.
	 * Halve down to the allocation granularity before giving up. */
	while (!w->base && want > gran) {
		want /= 2;
		if (want < gran)
			want = gran;
		w->base = (unsigned char *)MapViewOfFile(w->sect, FILE_MAP_ALL_ACCESS,
							 (DWORD)(start >> 32),
							 (DWORD)start, (SIZE_T)want);
	}
	if (!w->base) {
		/* Capped diagnostic: pos past total is an accounting mismatch; pos
		 * within total after shrinking to the granularity is genuine address-
		 * space exhaustion, not fragmentation. */
		static int said;

		if (said++ < 6)
			ss_log("  win_cover: MapViewOfFile failed even at %llu bytes - pos "
			       "%llu, start %llu, total %llu, sect %p, error %lu%s\n",
			       want, pos, start, w->total, (void *)w->sect,
			       (unsigned long)GetLastError(),
			       pos >= w->total ? " (pos is PAST total - region list outruns "
						 "the captured bytes)"
					       : " (address space is genuinely exhausted)");
		return 0;
	}
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

		cached = (n && n < sizeof(v)) ? atoi(v) : 0;
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

/* Cross-launch only: a saved word that pointed at an object the snapshot never
 * held - DirectInput's device, OpenAL's context, anything a system or library
 * module allocated in the process heap - leads nowhere in this launch. The
 * game built the same object here, and the word being overwritten is where it
 * keeps it, so that word is left as it is. Both sides must look like pointers
 * into unrestored, non-module memory; a pointer into a module is reloc's job,
 * one into restored memory is the snapshot's own. */
#define SS_KEEP_MAX 8192
static int g_keep_on;
static uintptr_t g_keep_rlo[SS_KEEP_MAX], g_keep_rhi[SS_KEEP_MAX];
static int g_keep_nr;
static const Slot *g_keep_s;
static unsigned long g_keep_n, g_keep_first_at, g_keep_first_was, g_keep_first_now;
static uintptr_t g_keep_qlo, g_keep_qhi;
static int g_keep_qok;

static int keep_in_restored(uintptr_t v)
{
	int lo = 0, hi = g_keep_nr - 1;

	while (lo <= hi) {
		int m = (lo + hi) / 2;

		if (v < g_keep_rlo[m])
			hi = m - 1;
		else if (v >= g_keep_rhi[m])
			lo = m + 1;
		else
			return 1;
	}
	return 0;
}

static int keep_live_word(uintptr_t was, uintptr_t now)
{
	MEMORY_BASIC_INFORMATION mbi;
	int k;

	if (was == now || ((was | now) & 3) || was < 0x10000 || now < 0x10000)
		return 0;
	if (keep_in_restored(was) || keep_in_restored(now))
		return 0;
	/* Values a later pass translates exactly: keeping the live word here would
	 * hide the saved one from it. */
	for (k = 0; k < g_keep_s->nstreams; k++)
		if (was == g_keep_s->stream_f[k])
			return 0;
	for (k = 0; k < g_keep_s->nwnd; k++)
		if (was == g_keep_s->wnd[k] || (g_keep_s->wnd_dc[k] && was == g_keep_s->wnd_dc[k]))
			return 0;
	for (k = 0; k < g_keep_s->nmods; k++)
		if (was >= g_keep_s->mod_lo[k] && was < g_keep_s->mod_hi[k])
			return 0;
	for (k = 0; k < g_ctl->nmods; k++)
		if (now >= g_ctl->mod_lo[k] && now < g_ctl->mod_hi[k])
			return 0;
	if (now < g_keep_qlo || now >= g_keep_qhi) {
		if (VirtualQuery((LPCVOID)now, &mbi, sizeof(mbi)) != sizeof(mbi))
			return 0;
		g_keep_qlo = (uintptr_t)mbi.BaseAddress;
		g_keep_qhi = g_keep_qlo + mbi.RegionSize;
		/* Committed and not part of an allocation the snapshot rewinds. A heap
		 * that grew past its saved extent is still the game's heap, and the
		 * restore is about to take that growth away. */
		g_keep_qok = mbi.State == MEM_COMMIT &&
			     !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
		for (k = 0; g_keep_qok && k < g_keep_s->nregs; k++)
			if (g_keep_s->regs[k].alloc_base == (uintptr_t)mbi.AllocationBase)
				g_keep_qok = 0;
	}
	return g_keep_qok;
}

static void keep_begin(const Slot *s)
{
	char v[8];
	DWORD n = ss_getenv("D3D9SW_XS_KEEPLIVE", v, sizeof(v));
	int i, j;

	g_keep_on = 0;
	g_keep_n = 0;
	g_keep_first_at = 0;
	g_keep_qlo = g_keep_qhi = 0;
	if (!xs_foreign_load() || (n > 0 && n < sizeof(v) && v[0] == '0'))
		return;
	if (!diff_write()) {
		ss_log("  keep-live: off - it rides on the differing-bytes copy, and "
		       "D3D9SW_DIFFWRITE=0 turned that off\n");
		return;
	}
	g_keep_nr = 0;
	for (i = 0; i < s->nregs && g_keep_nr < SS_KEEP_MAX; i++) {
		uintptr_t lo = s->regs[i].base, hi = lo + s->regs[i].size;

		for (j = g_keep_nr; j > 0 && g_keep_rlo[j - 1] > lo; j--) {
			g_keep_rlo[j] = g_keep_rlo[j - 1];
			g_keep_rhi[j] = g_keep_rhi[j - 1];
		}
		g_keep_rlo[j] = lo;
		g_keep_rhi[j] = hi;
		g_keep_nr++;
	}
	g_keep_s = s;
	g_keep_on = 1;
}

/* EncodePointer's per-process secret: x86 stores ror(p ^ c, c & 31). */
static ULONG ptr_cookie(void)
{
	static LONG(NTAPI * q)(HANDLE, int, PVOID, ULONG, PULONG);
	ULONG c = 0;

	if (!q)
		q = (LONG(NTAPI *)(HANDLE, int, PVOID, ULONG, PULONG))(void *)GetProcAddress(
			GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
	if (!q || q(GetCurrentProcess(), 36 /* ProcessCookie */, &c, sizeof(c), NULL) < 0)
		return 0;
	return c;
}

static uintptr_t ptr_enc(ULONG c, uintptr_t p)
{
	unsigned r = c & 31;
	uint32_t v = (uint32_t)p ^ c;

	return r ? (v >> r) | (v << (32 - r)) : v;
}

static uintptr_t ptr_dec(ULONG c, uintptr_t e)
{
	unsigned r = c & 31;
	uint32_t v = (uint32_t)e;

	return (r ? (v << r) | (v >> (32 - r)) : v) ^ c;
}

/* One walk of the address space up front: a VirtualQuery per candidate word
 * cost 19 s on a WOW64 load. */
#define PK_MAX 16384
static struct {
	uintptr_t lo, hi;
	DWORD type;
	int exec;
} g_pk[PK_MAX];
static int g_pk_n;

static void ptr_kind_map(void)
{
	MEMORY_BASIC_INFORMATION m;
	uintptr_t p = 0x10000;

	g_pk_n = 0;
	while (g_pk_n < PK_MAX && VirtualQuery((LPCVOID)p, &m, sizeof(m)) == sizeof(m)) {
		uintptr_t e = (uintptr_t)m.BaseAddress + m.RegionSize;

		if (m.State == MEM_COMMIT && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
			g_pk[g_pk_n].lo = (uintptr_t)m.BaseAddress;
			g_pk[g_pk_n].hi = e;
			g_pk[g_pk_n].type = m.Type;
			g_pk[g_pk_n].exec = !!(m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
							     PAGE_EXECUTE_READWRITE |
							     PAGE_EXECUTE_WRITECOPY));
			g_pk_n++;
		}
		if (e <= p)
			break;
		p = e;
	}
}

static int ptr_kind_at(uintptr_t p)
{
	int lo = 0, hi = g_pk_n - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;

		if (p < g_pk[mid].lo)
			hi = mid - 1;
		else if (p >= g_pk[mid].hi)
			lo = mid + 1;
		else
			return mid;
	}
	return -1;
}

static int ptr_is_kind(uintptr_t p, DWORD type, int exec)
{
	int k = ptr_kind_at(p);

	return k >= 0 && g_pk[k].type == type && (!exec || g_pk[k].exec);
}

/* A module's C runtime keeps its atexit table as two encoded pointers in the
 * module's data, the table's start and end, and the table holds encoded
 * function pointers. Restored from another launch they decode with this
 * launch's cookie into garbage, and the module's unload frees the garbage. The
 * test for a pair: both decode with the saving launch's cookie into one private
 * block, the lower one aligned as a heap block, and every entry between them
 * decodes to code.
 * An empty table has nothing to vouch for it - two equal words that decode
 * somewhere plausible are common (a pointer repeated in an array), and taking
 * them rewrote 500 words of a real save - so it is left alone. Encoded NULL is
 * one fixed value per cookie and is taken wherever it appears. */
static void encoded_ptrs_reencode(const Slot *s)
{
	ULONG oc = s->save_ptr_cookie, nc = ptr_cookie();
	uintptr_t onull, nnull;
	int i, nulls = 0, tables = 0, entries = 0;
	DWORD t0;

	if (!oc || !nc || oc == nc)
		return;
	if ((uintptr_t)EncodePointer((PVOID)0x12345678) != ptr_enc(nc, 0x12345678)) {
		ss_log("  encoded pointers: EncodePointer does not match the x86 formula here - "
		       "left as restored\n");
		return;
	}
	onull = ptr_enc(oc, 0);
	nnull = ptr_enc(nc, 0);
	t0 = GetTickCount();
	ptr_kind_map();
	for (i = 0; i < s->nregs; i++) {
		const Region *r = &s->regs[i];
		uintptr_t a, e;

		if (r->type != MEM_IMAGE ||
		    !(r->prot & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
				 PAGE_EXECUTE_WRITECOPY)))
			continue;
		e = r->base + r->size;
		for (a = r->base; a + 4 <= e; a += 4) {
			uintptr_t w, d, b;

			/* The section comes back as several protection runs, and
			 * ss_readable only vouches for one; skip a run it refuses. */
			if (!(a & 0xFFF) && !ss_readable(a, 0x1000)) {
				a += 0x1000 - 4;
				continue;
			}
			w = *(uintptr_t *)a;

			if (w == onull) {
				*(uintptr_t *)a = nnull;
				nulls++;
				continue;
			}
			d = ptr_dec(oc, w);
			if ((d & 3) || !ptr_is_kind(d, MEM_PRIVATE, 0))
				continue;
			for (b = a + 4; b < a + 64 && b + 4 <= e; b += 4) {
				uintptr_t d2, p, lo, hi;
				int ok = 1;

				if (!(b & 0xFFF) && !ss_readable(b, 4))
					break;
				d2 = ptr_dec(oc, *(uintptr_t *)b);
				/* Either word may be the start: MSVCR100's DLL runtime
				 * keeps the end first. */
				lo = d < d2 ? d : d2;
				hi = d < d2 ? d2 : d;
				if (hi == lo || (lo & 7) || hi - lo > 0x10000 || ((hi - lo) & 3))
					continue;
				{
					int k = ptr_kind_at(lo);

					if (k < 0 || hi > g_pk[k].hi)
						continue;
				}
				for (p = lo; p < hi && ok; p += 4)
					ok = ptr_is_kind(ptr_dec(oc, *(uintptr_t *)p), MEM_IMAGE, 1);
				if (!ok)
					continue;
				for (p = lo; p < hi; p += 4) {
					*(uintptr_t *)p = ptr_enc(nc, ptr_dec(oc, *(uintptr_t *)p));
					entries++;
				}
				*(uintptr_t *)a = ptr_enc(nc, d);
				*(uintptr_t *)b = ptr_enc(nc, d2);
				if (tables < 8) {
					unsigned off = 0;
					const char *mod = ss_module(a, &off);

					ss_log("    atexit table: %s+%X and +%X, %lu entr(ies) at %08lX\n",
					       mod ? mod : "?", off, off + (unsigned)(b - a),
					       (unsigned long)((hi - lo) / 4), (unsigned long)lo);
				}
				tables++;
				break;
			}
		}
	}
	ss_log("  encoded pointers: saving launch's cookie %08lX, this one's %08lX - %d encoded "
	       "NULL(s), %d atexit table(s) with %d entr(ies) re-encoded, %lu ms\n",
	       (unsigned long)oc, (unsigned long)nc, nulls, tables, entries,
	       (unsigned long)(GetTickCount() - t0));
}

/* Every word the restore wrote that points into memory it left in the present.
 *
 * Restored memory carries the saving launch's references. Where they land in
 * memory that travelled with them they are fine; where they land in memory this
 * launch kept - its heaps, audio objects, Steam's sections, a DLL now at another
 * address - they name whatever this launch put there. Each crash after a cross
 * load has been one of these, found one at a time. This lists them all at once,
 * grouped by holder allocation and target allocation. Pointers into a module
 * that sits at the same base in both launches are not counted. A word that is
 * not a pointer but happens to land in kept memory is counted too, so a group of
 * one is a weak hint and a group of hundreds is a seam. D3D9SW_XREFS=1. */
int gameheap_va_origin(uintptr_t base, char *out, int cap);

/* Whether a live allocation is the past's to define at a load: a segment of a
 * heap we rewind, or reserved by a module we rewind. */
static int relayout_rewound(uintptr_t base)
{
	char o[64], *plus;
	int h = heap_index_of(base), k;

	if (h >= 0)
		return g_ctl->heap_ours[h];
	if (!gameheap_va_origin(base, o, sizeof(o)) || !(plus = strchr(o, '+')))
		return 0;
	*plus = 0;
	for (k = 0; k < g_ctl->nmods; k++)
		if (!lstrcmpiA(g_ctl->mod_name[k], o))
			return g_ctl->mod_rewound[k];
	return 0;
}

#define XR_GROUPS 4096
static struct {
	uintptr_t holder, target, first_at, first_val;
	unsigned n;
} g_xr[XR_GROUPS];
#define XR_HASH 8192
static unsigned short g_xr_idx[XR_HASH]; /* group + 1, 0 = empty */
DWORD WINAPI K32GetMappedFileNameA(HANDLE, LPVOID, LPSTR, DWORD);

static void xr_label(uintptr_t base, char *out, int cap)
{
	MEMORY_BASIC_INFORMATION m;
	char o[64] = "";
	unsigned off = 0;
	const char *mod = ss_module(base, &off);
	HANDLE heaps[64];
	DWORD nh = GetProcessHeaps(64, heaps), k;

	if (mod) {
		wsprintfA(out, "image %.31s", mod);
		return;
	}
	for (k = 0; k < nh && k < 64; k++)
		if ((uintptr_t)heaps[k] == base) {
			wsprintfA(out, "heap %08lX", (unsigned long)base);
			return;
		}
	if (VirtualQuery((LPCVOID)base, &m, sizeof(m)) == sizeof(m) && m.Type == MEM_MAPPED) {
		char f[MAX_PATH];

		if (K32GetMappedFileNameA(GetCurrentProcess(), (LPVOID)base, f, sizeof(f))) {
			const char *b = f, *q;

			for (q = f; *q; q++)
				if (*q == '\\')
					b = q + 1;
			wsprintfA(out, "file %.40s", b);
		} else
			lstrcpynA(out, "section", cap);
		return;
	}
	if (gameheap_va_origin(base, o, sizeof(o)))
		wsprintfA(out, "private from %.40s", o);
	else
		lstrcpynA(out, "private", cap);
}

/* Read before the load: knob reads are frozen while one runs, and a plain static
 * would be wound back to whatever the saving launch had. */
static int g_xrefs_on SS_PRESENT = -1;

static int xrefs_on(void)
{
	if (g_xrefs_on < 0) {
		char v[8];
		DWORD got = ss_getenv("D3D9SW_XREFS", v, sizeof(v));

		g_xrefs_on = got > 0 && got < sizeof(v) && v[0] == '1';
	}
	return g_xrefs_on;
}

static void xrefs_census(const Slot *s)
{
	DWORD t0 = GetTickCount();
	MEMORY_BASIC_INFORMATION m;
	uintptr_t p;
	unsigned long long words = 0, hits = 0;
	int i, ng = 0, lost = 0;

	unsigned char *kept;
	uintptr_t qlo = 1, qhi = 0, qbase = 0;
	int timed_out = 0;

	if (g_xrefs_on != 1)
		return;
	/* One byte per 4 KB page below 2 GB: 1 = committed now, not written by the
	 * restore, and not inside a module at the same base as in the save. */
	kept = VirtualAlloc(NULL, 0x80000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!kept)
		return;
	memset(g_xr_idx, 0, sizeof(g_xr_idx));
	for (p = 0x10000; p < 0x80000000u &&
			  VirtualQuery((LPCVOID)p, &m, sizeof(m)) == sizeof(m);) {
		uintptr_t e = (uintptr_t)m.BaseAddress + m.RegionSize, q;
		int same = 0;

		if (m.State == MEM_COMMIT && m.Type == MEM_IMAGE) {
			unsigned off;
			const char *now = ss_module(p, &off);
			int k;

			for (k = 0; now && k < s->nmods; k++)
				if (s->mod_lo[k] == p - off && !lstrcmpiA(now, s->mod_name[k])) {
					same = 1;
					break;
				}
		}
		if (m.State == MEM_COMMIT && !same)
			for (q = p; q < e && q < 0x80000000u; q += 0x1000)
				kept[q >> 12] = 1;
		if (e <= p)
			break;
		p = e;
	}
	for (i = 0; i < s->nregs; i++) {
		uintptr_t a = s->regs[i].base, e = a + s->regs[i].size;

		for (; a < e && a < 0x80000000u; a += 0x1000)
			kept[a >> 12] = 0;
	}
	for (i = 0; i < s->nregs && !timed_out; i++) {
		const Region *r = &s->regs[i];
		uintptr_t a, e = r->base + r->size;

		for (a = r->base; a + 4 <= e; a += 4) {
			uintptr_t w, tb;
			int k;

			if (!(a & 0xFFF)) {
				if (!(a & 0xFFFFF) && GetTickCount() - t0 > 15000) {
					timed_out = 1;
					break;
				}
				if (!ss_readable(a, 0x1000)) {
					a += 0x1000 - 4;
					continue;
				}
			}
			w = *(uintptr_t *)a;
			words++;
			/* 00xx00yy is two UTF-16 characters or a pair of small
			 * numbers far more often than an address in low memory. */
			if (w < 0x10000 || w >= 0x80000000u || !(w & 0xFF00FF00u) ||
			    !kept[w >> 12])
				continue;
			if (w < qlo || w >= qhi) {
				if (VirtualQuery((LPCVOID)w, &m, sizeof(m)) != sizeof(m))
					continue;
				qlo = (uintptr_t)m.BaseAddress;
				qhi = qlo + m.RegionSize;
				qbase = (uintptr_t)m.AllocationBase;
			}
			tb = qbase;
			hits++;
			{
				unsigned hsh = (unsigned)((r->alloc_base >> 12) * 2654435761u ^
							  (tb >> 12) * 40503u) &
					       (XR_HASH - 1);

				while (g_xr_idx[hsh] &&
				       !(g_xr[g_xr_idx[hsh] - 1].holder == r->alloc_base &&
					 g_xr[g_xr_idx[hsh] - 1].target == tb))
					hsh = (hsh + 1) & (XR_HASH - 1);
				if (g_xr_idx[hsh])
					k = g_xr_idx[hsh] - 1;
				else if (ng == XR_GROUPS) {
					lost++;
					continue;
				} else {
					k = ng;
					g_xr_idx[hsh] = (unsigned short)(ng + 1);
				}
			}
			if (k == ng) {
				g_xr[k].holder = r->alloc_base;
				g_xr[k].target = tb;
				g_xr[k].first_at = a;
				g_xr[k].first_val = w;
				g_xr[k].n = 0;
				ng++;
			}
			g_xr[k].n++;
		}
	}
	VirtualFree(kept, 0, MEM_RELEASE);
	ss_log("  xrefs: %llu restored word(s) scanned, %llu point into memory this launch kept, "
	       "%d holder->target group(s)%s%s, %lu ms\n",
	       words, hits, ng, lost ? " (TABLE FULL, some not grouped)" : "",
	       timed_out ? " (STOPPED at 15 s, partial)" : "",
	       (unsigned long)(GetTickCount() - t0));
	for (i = 0; i < 60 && i < ng; i++) {
		int k, best = i;
		char hl[80], tl[80];
		unsigned hoff = 0;
		const char *hm;

		for (k = i + 1; k < ng; k++)
			if (g_xr[k].n > g_xr[best].n)
				best = k;
		if (best != i) {
			uintptr_t t[5];

			t[0] = g_xr[i].holder, t[1] = g_xr[i].target, t[2] = g_xr[i].first_at,
			t[3] = g_xr[i].first_val, t[4] = g_xr[i].n;
			g_xr[i] = g_xr[best];
			g_xr[best].holder = t[0], g_xr[best].target = t[1],
			g_xr[best].first_at = t[2], g_xr[best].first_val = t[3],
			g_xr[best].n = (unsigned)t[4];
		}
		xr_label(g_xr[i].holder, hl, sizeof(hl));
		xr_label(g_xr[i].target, tl, sizeof(tl));
		hm = ss_module(g_xr[i].first_at, &hoff);
		ss_log("    %6u  %08lX %s  ->  %08lX %s   first %08lX%s%s%s = %08lX\n", g_xr[i].n,
		       (unsigned long)g_xr[i].holder, hl, (unsigned long)g_xr[i].target, tl,
		       (unsigned long)g_xr[i].first_at, hm ? " (" : "", hm ? hm : "",
		       hm ? ")" : "", (unsigned long)g_xr[i].first_val);
	}
}

/* An NT heap's CommitRoutine (+0xCC in _HEAP, x86) is stored encoded with the
 * process cookie. A heap header restored from another launch decodes it with
 * this launch's cookie into garbage, and the next RtlpExtendHeap calls it. The
 * routine itself is the same either way, so this launch's encoding goes back. */
#define SS_HEAP_SIG_OFF 0x60
#define SS_HEAP_COMMIT_OFF 0xCC
static uintptr_t g_hcr_at[64], g_hcr_live[64];
static int g_hcr_n;

static void heap_cookie_hold(const Slot *s)
{
	int i;

	g_hcr_n = 0;
	if (!xs_foreign_load())
		return;
	for (i = 0; i < s->nregs && g_hcr_n < 64; i++) {
		uintptr_t b = s->regs[i].base;

		if (s->regs[i].size < 0x1000 || !ss_readable(b, 0x100) ||
		    *(const DWORD *)(b + SS_HEAP_SIG_OFF) != 0xEEFFEEFFu)
			continue;
		g_hcr_at[g_hcr_n] = b + SS_HEAP_COMMIT_OFF;
		g_hcr_live[g_hcr_n] = *(const uintptr_t *)(b + SS_HEAP_COMMIT_OFF);
		g_hcr_n++;
	}
}

static void heap_cookie_put(void)
{
	int i;

	for (i = 0; i < g_hcr_n; i++) {
		DWORD old;

		if (!VirtualProtect((void *)g_hcr_at[i], sizeof(uintptr_t), PAGE_READWRITE, &old))
			continue;
		*(uintptr_t *)g_hcr_at[i] = g_hcr_live[i];
		VirtualProtect((void *)g_hcr_at[i], sizeof(uintptr_t), old, &old);
	}
	if (g_hcr_n)
		ss_log("  heaps: %d restored heap header(s) given back this launch's encoded "
		       "CommitRoutine\n",
		       g_hcr_n);
}

static void keep_end(void)
{
	if (!g_keep_on)
		return;
	g_keep_on = 0;
	ss_log("  keep-live: %lu word(s) left holding this launch's pointer, because the "
	       "saved one led to an object the snapshot does not carry. First at %08lX, "
	       "saved %08lX, kept %08lX. Verify counts each of them as WRONG\n",
	       g_keep_n, g_keep_first_at, g_keep_first_was, g_keep_first_now);
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
		if (to_section) {
			memcpy(view, mem, chunk);
		} else if (!diff_write()) {
			memcpy(mem, view, chunk);
		} else {
			/* Write only what actually differs.
			 *
			 * Measured on this game: two censuses thirty seconds apart
			 * found 5 MB of 62 MB of heap content changed, and six of nine
			 * heaps were bit-for-bit identical. So the overwhelming
			 * majority of a restore is writing bytes that already hold the
			 * value being written.
			 *
			 * Those writes are not free. Every one is a chance to put a
			 * saved pointer back over a live one - the present/rewound
			 * split that has killed every session at zero frames. A byte
			 * that has not changed since the save cannot need restoring,
			 * and skipping it cannot break anything that was not already
			 * broken.
			 *
			 * The result is byte-for-byte identical to the wholesale copy.
			 * This is purely about which writes are issued. */
			size_t done = 0;

			while (done < chunk) {
				size_t page = chunk - done;

				if (page > 4096)
					page = 4096;
				if (memcmp((unsigned char *)mem + done, view + done,
					   page)) {
					uintptr_t *dst = (uintptr_t *)((unsigned char *)mem + done);
					const uintptr_t *src = (const uintptr_t *)(view + done);

					if (g_keep_on && !((uintptr_t)dst & 3) && !(page & 3)) {
						size_t k;

						for (k = 0; k < page / sizeof(uintptr_t); k++) {
							if (dst[k] == src[k])
								continue;
							if (keep_live_word(src[k], dst[k])) {
								if (!g_keep_n++) {
									g_keep_first_at = (uintptr_t)&dst[k];
									g_keep_first_was = src[k];
									g_keep_first_now = dst[k];
								}
								continue;
							}
							dst[k] = src[k];
						}
					} else
						memcpy((unsigned char *)mem + done,
						       view + done, page);
					if (g_ctl)
						g_ctl->diff_wrote += page;
				} else if (g_ctl) {
					g_ctl->diff_same += page;
				}
				done += page;
			}
		}
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

static void ss_name_leave_tid(DWORD tid)
{
	int i;

	if (!g_ctl) {
		ss_log("      no control block; cannot name the leave thread\n");
		return;
	}
	for (i = 0; i < g_ctl->nids; i++) {
		unsigned off = 0;
		const char *mod;
		if (g_ctl->ids[i] != tid)
			continue;
		mod = g_ctl->starts[i] ? ss_module((uintptr_t)g_ctl->starts[i], &off) : NULL;
		ss_log("      leave tid start %p (%s+%X) freeze=%s\n", g_ctl->starts[i],
		       mod ? mod : "?", off,
		       g_did_suspend[i] ? "WAS FROZEN (should not run)" : "LEFT RUNNING");
		return;
	}
	ss_log("      leave tid is not in the freeze roster (requester or born after "
	       "collect)\n");
}

static void suspend_all(void)
{
	int i, froze = 0, skipped = 0, audio_wait = 0, known = 0;
	int wine = ss_under_wine();
	unsigned state[SS_MAX_THREADS];
	/* Also set in blk_lock_all, which runs earlier when block mode is on.
	 * Repeated here so the paths that freeze without taking the heap locks
	 * are covered too - a suspended process is no safer to allocate against
	 * than a locked heap, and for the LFH it is the same hazard. */
	env_prewarm();
	g_env_frozen = 1;
	memset(g_did_suspend, 0, sizeof(g_did_suspend));
	if (wine)
		wine_sample_states(state, g_ctl->nids);
	for (i = 0; i < g_ctl->nids; i++) {
		HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
					      THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
				      FALSE, g_ctl->ids[i]);
		PVOID start = h ? start_of(h) : NULL;
		int freeze = h && (!wine || wine_freeze_this(start, wine ? state[i] : 0));

		if (wine && state[i])
			known++;
		if (h && freeze && SuspendThread(h) == (DWORD)-1) {
			CloseHandle(h);
			h = NULL;
			freeze = 0;
		}
		g_ctl->handles[i] = h;
		g_ctl->starts[i] = start;
		g_did_suspend[i] = (char)(h && freeze);
		if (g_did_suspend[i])
			froze++;
		else if (h) {
			skipped++;
			if (wine && wine_audio_start(start))
				audio_wait++;
		}
	}
	/* Mixer threads that were Waiting can leave the server now (game threads
	 * are already frozen; dinput is still running). Catch them in user-mode
	 * so dsound.dll is not walking rewound objects during the copy. Pulse
	 * itself is never torn down. */
	if (wine && audio_wait) {
		int pass;

		for (pass = 0; pass < 16; pass++) {
			int still = 0;

			Sleep(1);
			wine_sample_states(state, g_ctl->nids);
			for (i = 0; i < g_ctl->nids; i++) {
				PVOID start = g_ctl->starts[i];

				if (g_did_suspend[i] || !g_ctl->handles[i] || !wine_audio_start(start))
					continue;
				if (!wine_state_runnable(state[i])) {
					still++;
					continue;
				}
				if (SuspendThread(g_ctl->handles[i]) == (DWORD)-1)
					continue;
				g_did_suspend[i] = 1;
				froze++;
				skipped--;
			}
			audio_wait = still;
			if (!still)
				break;
		}
	}
	if (wine)
		ss_phase("  wine: froze %d game/mixer thread(s), left %d wineserver-side "
			 "thread(s) running (%d audio still waiting, %d/%d states known)\n",
			 froze, skipped, audio_wait, known, g_ctl->nids);
}

static void resume_all(int hold_fresh)
{
	int i;

	/* The copy can write a saved eax back into a frame that was not
	 * inside a call. Clear it again before any resumed thread reaches
	 * pulse_stop. */
	unix_frame_heal();
	for (i = 0; i < g_ctl->nids; i++) {
		if (!g_ctl->handles[i])
			continue;
		if (g_did_suspend[i] && !(hold_fresh && g_ctl->fresh[i]))
			ResumeThread(g_ctl->handles[i]);
		CloseHandle(g_ctl->handles[i]);
		g_ctl->handles[i] = NULL;
		g_did_suspend[i] = 0;
	}
	/* After the threads are running again, so that anything queued behind a
	 * heap lock wakes into a process that can actually service it. */
	blk_unlock_all();
	g_env_frozen = 0;
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

/* -------------------------------------------------- a slot that outlives us
 *
 * D3D9SW_SLOTFILE=1 backs the snapshot with a file instead of the pagefile and
 * writes the slot's description beside it, so a save taken in one session can
 * be handed to the next.
 *
 * Deliberately the smallest thing that answers the question. Slot is plain
 * data - region base/size pairs, thread contexts, file states, module bases -
 * with one field that cannot travel, the section handle, so persisting it is a
 * write and a read rather than a serialiser.
 *
 * What it cannot do is make the restore correct across a relaunch, and that is
 * the point of running it. The engine leaves 16.4 MB of heap, every system
 * thread and all 91 module images in the present because they cannot be
 * rewound; today that is safe because the present is continuous with the save.
 * A second session is a different present. If this works at all it says the
 * snapshot carries more of the game than we thought; if it fails it says where
 * the continuity is load-bearing, and either answer is worth one run.
 */
static int slotfile_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_SLOTFILE", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '1') ? 1 : 0;
	}
	return cached;
}

static void slotfile_path(char *buf, int cap, int slotno, const char *ext)
{
	char leaf[64];

	wsprintfA(leaf, "d3d9sw_slot%d.%s", slotno, ext);
	if (!GetFullPathNameA(leaf, (DWORD)cap, buf, NULL))
		lstrcpynA(buf, leaf, cap);
}

/* The section, backed by a file this time. Returns NULL to let the caller fall
 * back to the pagefile, because a slot we cannot persist is still a slot. */
static HANDLE slotfile_section(int slotno, unsigned long long total)
{
	char path[MAX_PATH];
	HANDLE f, sect;

	slotfile_path(path, sizeof(path), slotno, "bin");
	f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		ss_log("  slotfile: cannot create %s, err=%lu - falling back to the "
		       "pagefile\n",
		       path, GetLastError());
		return NULL;
	}
	sect = CreateFileMappingA(f, NULL, PAGE_READWRITE, (DWORD)(total >> 32),
				  (DWORD)total, NULL);
	if (!sect)
		ss_log("  slotfile: mapping %s at %llu bytes failed, err=%lu\n", path, total,
		       GetLastError());
	else
		ss_log("  slotfile: snapshot is backed by %s, %.1f MB\n", path,
		       (double)total / (1024.0 * 1024.0));
	/* The mapping keeps the file alive on its own. */
	CloseHandle(f);
	return sect;
}

/* -------------------------------------------------- watching one address
 *
 * D3D9SW_WATCH_AT=<hex> names an address to report a pair of floats from, at
 * the save and again once the restore has settled. Two readings answer the only
 * question that matters about a value that visibly does not come back:
 *
 *   same at both        restored, and it stayed restored
 *   differs afterwards  restored and then overwritten, which is the game
 *                       re-deriving it from something we did not rewind
 *   not in the snapshot  never captured, and the coverage list says where
 *
 * The player has had this since the beginning - see witness_save - and it is
 * how we know a restore reached her. This is the same instrument pointed
 * wherever the evidence says to point it. Ribbon was found at 31876868 by
 * searching a dump for a float pair near the player's position; she is in the
 * snapshot, so the interesting question is now the second line above.
 *
 * A pair of floats because that is how this game stores a position, and because
 * a single word that happens to match proves much less than two that do.
 */
/* Whether a region that is a live stack or TEB now, but was ordinary memory at
 * the save, aborts the restore or is simply left alone. On by default; see the
 * reasoning at the test site. */
static int exclskip_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_EXCLSKIP", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '0') ? 0 : 1;
	}
	return cached;
}

/* Damn-the-torpedoes restore. Off by default.
 *
 * The refuse that guards a partial rewind is exactly right when the aim is a
 * process that keeps running correctly - a restore that puts back most of the
 * state and not the rest is worse than none. This knob is for the opposite aim:
 * force every saved block back over whatever now occupies its address and watch
 * what the game does with it. Regions that "belong elsewhere" or will not
 * recommit no longer refuse the load; they are written where they can be and
 * left in the present where they cannot, and the process is allowed to run on
 * the result. The one thing still protected is what cannot be written without
 * killing the restore itself: the running restore thread's stack and any peer
 * still live are held back by win_copy_except_live, and every engine structure
 * was excluded at the save so it was never in the region set to begin with. */
static int exclforce_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_EXCLFORCE", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '1') ? 1 : 0;
	}
	return cached;
}

/* Data-only restore: rewind the world, leave the runners. When set, do_load does
 * NOT apply any saved thread context - no thread is wound back to the save, they
 * all keep their current execution and read the rewound memory. Cross-session this
 * is already what happens (the saved TIDs are gone, so nothing matches), and the
 * thread_harness proved fresh threads pick up a rewound world with no adoption.
 * The knob forces it IN-SESSION too, where the memory rewind is complete, so the
 * model can be A/B'd against the resume-through-the-save hijack on clean memory.
 * RESULT (2026-09-26): even in-session on complete memory the game freezes and
 * dies quickly under this - the runners genuinely need their contexts. A game
 * thread's execution is welded to the memory it was executing over; rewind the
 * world without rewinding the thread and its next instruction operates on data
 * that jumped, so it faults at once. Data-only cannot carry the game. The harness
 * loop was too simple to model this. Kept for the record. Off by default. */
static int dataonly_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_DATAONLY", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '1') ? 1 : 0;
	}
	return cached;
}

/* Stack-address probe. Data-only is dead (see above): the game needs its threads
 * wound back, contexts AND the stacks those contexts point into. In-session that
 * is free - the saved threads still exist at the same stack addresses, so we
 * rewind the same memory. Cross-session the saved threads are gone and adoption
 * would mean: reserve the saved stack's address, restore its bytes, and point a
 * fresh thread's context at it. That is only possible if the saved stack address
 * is still FREE in the new process. thread_harness answered "occupied", but it
 * respawned in the SAME process, where the OS handed the fresh threads the exact
 * addresses the dead ones freed - a same-process artifact. A real cross-session
 * restore is a NEW process, and the log shows thread stacks land at different
 * addresses across launches (req thread 00AF1000 one run, 00CF1000 another). So
 * the saved-stack addresses may well be free here. This probe measures it for the
 * real game at real restore time: for every saved thread stack, VirtualQuery the
 * range now and report free / reserved / committed. If most are free, adoption
 * has a door the harness closed prematurely. On by default; it only reads. */
static int stackprobe_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_STACKPROBE", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '0') ? 0 : 1;
	}
	return cached;
}

static void stackprobe_report(const Slot *s)
{
	int i, with_stack = 0, freed = 0, reserved = 0, committed = 0, ours = 0;
	uintptr_t req_lo = 0, req_hi = 0;
	DWORD req_tid = g_ctl ? g_ctl->req_tid : 0;

	if (!stackprobe_mode() || !s)
		return;

	for (i = 0; i < s->nthreads; i++) {
		MEMORY_BASIC_INFORMATION mbi;
		uintptr_t lo = s->threads[i].stack_limit;   /* low committed end */
		uintptr_t hi = s->threads[i].stack_base;     /* high end */
		const char *state = "?";
		int is_req = (s->threads[i].tid == req_tid);

		if (!hi)
			continue;
		with_stack++;
		if (is_req) { req_lo = lo; req_hi = hi; }
		if (VirtualQuery((LPCVOID)lo, &mbi, sizeof(mbi)) != sizeof(mbi)) {
			ss_log("  stackprobe: saved tid %u stack %08lX..%08lX - "
			       "VirtualQuery failed (err %lu)\n",
			       (unsigned)s->threads[i].tid, (unsigned long)lo,
			       (unsigned long)hi, GetLastError());
			continue;
		}
		if (mbi.State == MEM_FREE) { state = "FREE"; freed++; }
		else if (mbi.State == MEM_RESERVE) { state = "RESERVED"; reserved++; }
		else { state = "COMMITTED"; committed++; }
		if (mbi.State != MEM_FREE && mbi.Type == MEM_PRIVATE &&
		    (uintptr_t)mbi.AllocationBase != lo)
			ours++;
		/* Only the requesting thread and a handful of others are worth a line;
		 * the summary carries the rest. */
		if (is_req || i < 6 || mbi.State == MEM_FREE)
			ss_log("  stackprobe: saved tid %u%s stack %08lX..%08lX now %s "
			       "(base %08lX size %luK type %s)\n",
			       (unsigned)s->threads[i].tid, is_req ? " [REQ]" : "",
			       (unsigned long)lo, (unsigned long)hi, state,
			       (unsigned long)(uintptr_t)mbi.AllocationBase,
			       (unsigned long)(mbi.RegionSize >> 10),
			       mbi.Type == MEM_PRIVATE ? "private" :
			       mbi.Type == MEM_MAPPED ? "mapped" :
			       mbi.Type == MEM_IMAGE ? "image" : "free");
	}
	ss_log("  stackprobe: %d saved stack(s): %d FREE, %d reserved, %d committed "
	       "(%d re-tenanted) - adoption %s (req thread %08lX..%08lX)\n",
	       with_stack, freed, reserved, committed, ours,
	       freed == with_stack ? "OPEN: every saved stack address is free" :
	       freed > 0 ? "PARTIAL: some saved stack addresses are free" :
	       "CLOSED: no saved stack address is free",
	       (unsigned long)req_lo, (unsigned long)req_hi);
}

/* On by default. A saved region that is a live file mapping at restore time
 * cannot be written back - a section view is not a memcpy target - so holding it
 * out is the only correct move, not a policy choice; the game re-maps the asset
 * and reads from the mapping. This exists as a knob only so the old behaviour
 * (count it as unrestorable and refuse) can be restored for comparison. */
static int mapskip_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_MAPSKIP", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '0') ? 0 : 1;
	}
	return cached;
}

#define SS_WATCH_MAX 8
static uintptr_t g_watch_at[SS_WATCH_MAX];
static int g_watch_at_n = -1;

/* A list rather than one address, because a search never returns one candidate.
 * Looking for Ribbon turned up three sites holding the same X to a decimal
 * place and three different Y values - one hovering character recorded in
 * several places, not three characters - and there is no way to tell from
 * outside which of them the game reads. Watching all of them costs a line each
 * and settles it in one run instead of three. */
static void watch_at_parse(int live)
{
	char v[128];
	DWORD n, i;
	uintptr_t a = 0;
	int have = 0;
	int rel = 0;

	/* Re-read on request, because the address cannot be known before the
	 * session that contains it. Finding it means taking a save and searching
	 * the dump, and by then this process has long since started; a knob fixed
	 * at startup could only ever carry an address from a previous run, and
	 * the game's allocations do not survive a relaunch. So the reading that
	 * matters takes the knob fresh.
	 *
	 * Only ever from outside the suspended window. ss_getenv can fall back to
	 * reading the cfg file, and a file read between suspend_all and
	 * resume_all is how this engine deadlocks itself - which is what the warm
	 * list above exists to prevent. */
	if (g_watch_at_n >= 0 && !live)
		return;
	g_watch_at_n = 0;
	if (live) {
		/* Re-read the file, not the copy of it taken at startup.
		 *
		 * Everything else in this engine wants the config frozen: knobs are
		 * warmed into a memo before the threads stop so that no lookup
		 * allocates inside the window, and cfg_load slurps the file once.
		 * That is right for every other knob and wrong for this one, because
		 * this one carries an address that cannot exist until the session is
		 * already running - you take a save, search the dump, and only then
		 * know what to type. A value fixed at launch can only ever be an
		 * address from a previous launch, and the game's allocations do not
		 * survive a relaunch.
		 *
		 * Safe here and nowhere else: this call site runs 100 ms after the
		 * restore, on the render thread, with every game thread already
		 * resumed. */
		g_cfg_tried = 0;
		g_cfg_len = 0;
		g_cfg_over = 0;
		cfg_load();
	}
	/* cfg_lookup rather than ss_getenv on the live path, because the memo in
	 * env_once would answer with the startup value and never reach the file. */
	n = live ? cfg_lookup("D3D9SW_WATCH_AT", v, sizeof(v))
		 : ss_getenv("D3D9SW_WATCH_AT", v, sizeof(v));
	if (n == 0 || n >= sizeof(v))
		return;
	/* One past the end, so a trailing address is flushed by the same branch
	 * that handles a separator rather than by a copy of it afterwards. */
	for (i = 0; i <= n; i++) {
		char c = (i < n) ? v[i] : ',';
		int d = -1;

		/* A leading + means "this far past the player's entity", which is the
		 * only form that survives a relaunch.
		 *
		 * The arena moves every session - three runs put its base at
		 * 0E34F000, 0E03F000 and 0E1AF000 - but the entities inside it do
		 * not. Two independent saves put the player at base+4C4 and Ribbon at
		 * base+2A738, so she is +2A280 from the player's x field in both, to
		 * the byte. The engine already records the player's entity address at
		 * every save for the witness, so a relative address resolves itself
		 * and an absolute one has to be looked up and retyped between the
		 * save and the restore - which is a step that has now been got wrong
		 * three times in a row, twice by pasting the same three constants. */
		if (c == '+' && !have) {
			rel = 1;
			continue;
		}
		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (c >= 'a' && c <= 'f')
			d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			d = c - 'A' + 10;
		if (d >= 0) {
			a = (a << 4) | (uintptr_t)d;
			have = 1;
			continue;
		}
		if (have && rel) {
			if (g_ctl && g_ctl->wit_valid && g_ctl->wit_ent) {
				if (g_watch_at_n < SS_WATCH_MAX)
					g_watch_at[g_watch_at_n++] = g_ctl->wit_ent + a;
			} else {
				ss_log("  watch +%lX: no player entity recorded yet, so a "
				       "relative address cannot be resolved\n",
				       (unsigned long)a);
			}
		} else if (have && a && g_watch_at_n < SS_WATCH_MAX) {
			g_watch_at[g_watch_at_n++] = a;
		}
		a = 0;
		have = 0;
		rel = 0;
	}
}

/* -------------------------------------------------- the watch, frame by frame
 *
 * D3D9SW_WATCH_TRACE=N logs the watched addresses on each of the next N frames
 * after a save and after a restore.
 *
 * Two readings a hundred milliseconds apart cannot tell a value that was
 * restored wrongly from one that was restored rightly and then moved, and the
 * gap involved here is small: Ribbon read 16018.03 at a save and 16013.65 after
 * the restore settled, four units, which is either a defect or an idle
 * animation. A trace decides it by looking at the shape rather than the
 * endpoints - an animation oscillates around a centre, a botched restore sits
 * wherever it landed, and a follower converges on the player.
 *
 * There is a detail in the two-sample data that a trace also settles. Her Y has
 * read 4596F2AD - the same bits - in every sample across four sessions, while X
 * moves. If she is visibly bobbing, then the bob is computed per frame for
 * drawing and never stored, and the stored Y is not what the eye is following.
 * That matters, because the field we watch has to be the field that decides
 * where she is.
 */
static int trace_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[16];
		DWORD n = ss_getenv("D3D9SW_WATCH_TRACE", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v)) ? atoi(v) : 0;
		if (cached < 0)
			cached = 0;
	}
	return cached;
}

static volatile LONG g_trace_left;
static const char *g_trace_why = "";
static int g_trace_frame;

static void watch_trace_arm(const char *why)
{
	if (!trace_mode() || g_watch_at_n <= 0)
		return;
	g_trace_why = why;
	g_trace_frame = 0;
	g_trace_left = trace_mode();
}

void savestate_watch_tick(void)
{
	int k;

	if (g_trace_left <= 0)
		return;
	g_trace_left--;
	g_trace_frame++;
	for (k = 0; k < g_watch_at_n; k++) {
		uintptr_t a = g_watch_at[k];
		float f[2];
		unsigned w[2];

		if (!ss_readable(a, sizeof(f)))
			continue;
		memcpy(f, (const void *)a, sizeof(f));
		memcpy(w, f, sizeof(w));
		/* Wall-clock since the last restore, so "frames" cannot be mistaken for
		 * time: an unthrottled present loop over dead game logic ticks the frame
		 * counter fast, and only a real clock tells that apart from play. */
		ss_log("  trace %s +%d %lums %08lX: x %d.%02d y %d.%02d (%08lX %08lX)\n",
		       g_trace_why, g_trace_frame,
		       (g_ctl && g_ctl->load_tick) ?
			       (unsigned long)(GetTickCount() - g_ctl->load_tick) : 0ul,
		       (unsigned long)a, (int)f[0],
		       (int)((f[0] < 0 ? -f[0] : f[0]) * 100.0f) % 100, (int)f[1],
		       (int)((f[1] < 0 ? -f[1] : f[1]) * 100.0f) % 100,
		       (unsigned long)w[0], (unsigned long)w[1]);
	}
}

/* ------------------------------------------------------- the entity array
 *
 * D3D9SW_ENTS=1 reports the game's entity table by walking it, rather than by
 * searching memory for numbers that look like coordinates.
 *
 * The layout comes from the Rabi-Ribi speedrunning community's LiveSplit
 * autosplitter, which has carried per-version offsets for years. For v1.65 a
 * pointer at image + 0x940EE0 leads to the entity array, and an entity's x and
 * y sit at +0x0C and +0x10. That matches, exactly, the two floats found here by
 * scanning for a coordinate pair - the same +0x0C and +0x10 off the same
 * structure, arrived at from the opposite direction.
 *
 * The stride was left as a TODO in that file, so it is derived here instead.
 * Two artbook timers in the same table are declared at +0x1310 and +0xB2FC,
 * being the same field on two different entities; the gap between them is
 * 0x9FEC, which 0x6F4 divides exactly 23 times. The check that settles it is
 * independent: the first allocation this game ever makes is 176,220 bytes, and
 * 176,220 is 1780 x 99 exactly. So the array is 99 slots of 0x6F4, and the
 * block this engine has been tracking at heap offset +0x7F4B8 since the
 * beginning is the entity table itself.
 *
 * Worth having because searching for coordinates finds coordinate-shaped bytes.
 * An earlier hunt for Ribbon turned up four candidates, three of them in a
 * DxLib arena that cannot hold entities at all. Walking the array cannot return
 * a false positive: slot 3 is slot 3.
 */
#define SS_ENT_PTR 0x940EE0u  /* image-relative, v1.65 */
#define SS_ENT_STRIDE 0x6F4u  /* derived, see above */
#define SS_ENT_COUNT 99	      /* 176220 / 1780, the whole allocation */
#define SS_ENT_X 0x0Cu
#define SS_ENT_Y 0x10u
#define SS_ENT_V165_IMAGE 0x010CE000u

/* Asked before every read here, because the table pointer is null until the
 * game builds it and this runs at save time, which can be before that. */
static int ents_readable(const void *p, size_t n)
{
	MEMORY_BASIC_INFORMATION mbi;

	if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)))
		return 0;
	if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
		return 0;
	return (uintptr_t)p + n <=
	       (uintptr_t)mbi.BaseAddress + (uintptr_t)mbi.RegionSize;
}

static int ents_mode(void)
{
	char v[8];
	DWORD n = ss_getenv("D3D9SW_ENTS", v, sizeof(v));

	return n && v[0] == '1';
}

/* The offsets are version-specific and wrong on any other build, so the version
 * is checked rather than assumed. Reading a stale pointer would produce
 * confident nonsense, which is worse than saying nothing. */
static int ents_version_ok(uintptr_t base)
{
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
	const IMAGE_NT_HEADERS32 *nt;

	if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return 0;
	nt = (const IMAGE_NT_HEADERS32 *)(base + (uintptr_t)dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return 0;
	return nt->OptionalHeader.SizeOfImage == SS_ENT_V165_IMAGE;
}

static uintptr_t ents_table(void)
{
	uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
	uintptr_t arr;

	if (!ents_version_ok(base)) {
		ss_log("entities: this is not the v1.65 build the offsets are for, "
		       "so the table is not being read\n");
		return 0;
	}
	if (!ents_readable((const void *)(base + SS_ENT_PTR), sizeof(void *)))
		return 0;
	arr = *(const uintptr_t *)(base + SS_ENT_PTR);
	if (!arr || !ents_readable((const void *)arr, SS_ENT_STRIDE * SS_ENT_COUNT)) {
		ss_log("entities: the table pointer at image+%lX reads %p, which is "
		       "not a table yet\n",
		       (unsigned long)SS_ENT_PTR, (void *)arr);
		return 0;
	}
	return arr;
}

static void ents_report(const char *when)
{
	uintptr_t arr;
	int i, shown = 0;

	if (!ents_mode())
		return;
	arr = ents_table();
	if (!arr)
		return;
	ss_log("entities %s: table at %p, %d slot(s) of %lu bytes\n", when,
	       (void *)arr, SS_ENT_COUNT, (unsigned long)SS_ENT_STRIDE);
	for (i = 0; i < SS_ENT_COUNT; i++) {
		const char *e = (const char *)(arr + (uintptr_t)i * SS_ENT_STRIDE);
		float x = *(const float *)(e + SS_ENT_X);
		float y = *(const float *)(e + SS_ENT_Y);

		/* Empty slots read as zero, and the map is never that large, so
		 * this prints what is on the field rather than all 99 rows. */
		if (x == 0.0f && y == 0.0f)
			continue;
		if (x < -100000.0f || x > 100000.0f || y < -100000.0f ||
		    y > 100000.0f)
			continue;
		ss_log("  slot %-3d at %p  x %.2f  y %.2f%s\n", i, (const void *)e,
		       x, y, i == 0 ? "   <- the player" : "");
		shown++;
	}
	ss_log("entities: %d slot(s) occupied of %d\n", shown, SS_ENT_COUNT);
}

/* Where she ends up is a consequence; which word moved her is the cause. The
 * slot is 1780 bytes and only two of them have ever been read here, so keep a
 * copy of the whole thing at the save and let a later pass name every field
 * that came back different. A follower chasing a stale target has that target
 * written down somewhere, and if it is inside her own slot this finds it. */
static unsigned char g_ent_save[2][SS_ENT_STRIDE];
static int g_ent_have;
static const int g_ent_which[2] = { 0, 97 };	/* the player, and Ribbon */

static void ents_snap(void)
{
	uintptr_t arr;
	int k;

	if (!ents_mode())
		return;
	arr = ents_table();
	if (!arr)
		return;
	for (k = 0; k < 2; k++)
		memcpy(g_ent_save[k],
		       (const void *)(arr + (uintptr_t)g_ent_which[k] * SS_ENT_STRIDE),
		       SS_ENT_STRIDE);
	g_ent_have = 1;
}

/* Most of a 1780 byte slot is not a float, so printing one for every word would
 * bury the answer in denormals. Hex is always shown; the float only when it
 * could plausibly be a coordinate. */
static int ents_sane(float f)
{
	return f == f && f > -1.0e9f && f < 1.0e9f;
}

static void ents_diff(const char *when)
{
	uintptr_t arr;
	int k;

	if (!ents_mode() || !g_ent_have)
		return;
	arr = ents_table();
	if (!arr)
		return;
	for (k = 0; k < 2; k++) {
		const unsigned char *now = (const unsigned char *)
			(arr + (uintptr_t)g_ent_which[k] * SS_ENT_STRIDE);
		const unsigned char *then = g_ent_save[k];
		unsigned off;
		int moved = 0;

		ss_log("slot %d %s: word(s) that differ from the save\n",
		       g_ent_which[k], when);
		for (off = 0; off + 4 <= SS_ENT_STRIDE; off += 4) {
			unsigned a, b;
			float fa, fb;

			memcpy(&a, then + off, 4);
			memcpy(&b, now + off, 4);
			if (a == b)
				continue;
			memcpy(&fa, &a, 4);
			memcpy(&fb, &b, 4);
			moved++;
			if (ents_sane(fa) && ents_sane(fb))
				ss_log("  +%03lX  was %08lX %.2f  now %08lX %.2f\n",
				       (unsigned long)off, (unsigned long)a, (double)fa,
				       (unsigned long)b, (double)fb);
			else
				ss_log("  +%03lX  was %08lX  now %08lX\n",
				       (unsigned long)off, (unsigned long)a,
				       (unsigned long)b);
		}
		if (!moved)
			ss_log("  none, the slot is word-for-word the save\n");
	}
}

static void watch_at_report(const char *when, int live)
{
	int k;

	watch_at_parse(live);
	for (k = 0; k < g_watch_at_n; k++) {
		uintptr_t a = g_watch_at[k];
		float f[2];
		unsigned w[2];

		if (!ss_readable(a, sizeof(f))) {
			ss_log("  watch %08lX: not readable %s\n", (unsigned long)a, when);
			continue;
		}
		memcpy(f, (const void *)a, sizeof(f));
		memcpy(w, f, sizeof(w));
		/* The raw words as well as the decimals. Two positions that print the
		 * same to two places are not necessarily the same value, and "did this
		 * come back exactly" is the entire question being asked here. */
		ss_log("  watch %08lX: x %d.%02d  y %d.%02d  (%08lX %08lX) %s\n",
		       (unsigned long)a, (int)f[0],
		       (int)((f[0] < 0 ? -f[0] : f[0]) * 100.0f) % 100, (int)f[1],
		       (int)((f[1] < 0 ? -f[1] : f[1]) * 100.0f) % 100,
		       (unsigned long)w[0], (unsigned long)w[1], when);
	}
}

/* A plain-text map from addresses to offsets in the .bin.
 *
 * The description file has all of this already, but only as a Slot, and any
 * reader of that would have to reproduce a C layout with 65536-entry arrays to
 * find a single region. This costs one small file and makes the snapshot a flat
 * seekable address space that anything can walk - which is what turns a dump
 * into evidence rather than 388 MB of opaque bytes.
 *
 * The player's position goes in the header because it is the one value in the
 * snapshot whose correct answer is known independently, so it is the way to
 * check a reader works before trusting anything it says about a value that is
 * not known.
 */
static void slotfile_write_index(int slotno, const Slot *s)
{
	char path[MAX_PATH], line[160];
	HANDLE f;
	DWORD wrote = 0;
	unsigned long long off = 0;
	int i, n;

	slotfile_path(path, sizeof(path), slotno, "regions");
	f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
			NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	/* wsprintfA is the Win32 formatter, not the CRT's: it has no ll modifier,
	 * and "%012llX" silently emits the literal text lX. Everything here is cast
	 * to 32 bits instead, which is exact rather than a compromise - the total is
	 * the sum of regions in a 32-bit address space and cannot reach 4 GB. */
	n = wsprintfA(line, "# slot %d  %d region(s)  %lu byte(s)\r\n", slotno, s->nregs,
		      (unsigned long)s->bytes);
	WriteFile(f, line, (DWORD)n, &wrote, NULL);
	if (g_ctl && g_ctl->wit_valid) {
		/* The raw bits as well as the rounded value. A reader searching for
		 * the position needs the exact float, and the decimal here is an int
		 * cast: the player at "x 16080" is at some 16080.something, so a
		 * search for 16080.0 finds nothing and looks like a broken index
		 * rather than a truncated number. */
		unsigned long xb, yb;

		memcpy(&xb, &g_ctl->wit_x, sizeof(xb));
		memcpy(&yb, &g_ctl->wit_y, sizeof(yb));
		n = wsprintfA(line,
			      "# player entity %08lX  x %d  y %d  world %u  xbits %08lX  "
			      "ybits %08lX\r\n",
			      (unsigned long)g_ctl->wit_ent, (int)g_ctl->wit_x,
			      (int)g_ctl->wit_y, g_ctl->wit_map, xb, yb);
		WriteFile(f, line, (DWORD)n, &wrote, NULL);
	}
	/* Module ranges, so an offline reader can tell a pointer into code or a
	 * vtable from a pointer into the heap and from plain data - the three shift
	 * differently across a relaunch, and separating them is the whole point of
	 * comparing two saves. Emitted before the region table so a parser has them
	 * in hand by the time it reads the bytes. */
	for (i = 0; i < s->nmods; i++) {
		char mline[MAX_PATH + 96];

		/* Name first, as savediff parses it; identity after. */
		n = wsprintfA(mline, "# module %08lX %08lX %s stamp %08lX size %08lX %s\r\n",
			      (unsigned long)s->mod_lo[i], (unsigned long)s->mod_hi[i],
			      s->mod_name[i], (unsigned long)s->mod_stamp[i],
			      (unsigned long)s->mod_isize[i], s->mod_path[i]);
		WriteFile(f, mline, (DWORD)n, &wrote, NULL);
	}
	/* Bands: address ranges a saved word can point INTO that are not captured
	 * regions and not modules - the held heaps left in the present, and the
	 * per-thread stacks, TEBs and PEB from the exclusion list. Without these an
	 * offline reader cannot tell a pointer into the process heap from a data
	 * word, so every such pointer reads as a content difference when it is only
	 * an address that moved. Recording them lets the reader mask all pointer
	 * classes and measure the divergence that is genuinely content. */
	if (g_ctl) {
		int b;

		for (b = 0; b < g_ctl->nheaps; b++) {
			n = wsprintfA(line, "# band %08lX %08lX heap\r\n",
				      (unsigned long)g_ctl->heap_lo[b],
				      (unsigned long)g_ctl->heap_hi[b]);
			WriteFile(f, line, (DWORD)n, &wrote, NULL);
		}
		for (b = 0; b < g_ctl->nex; b++) {
			n = wsprintfA(line, "# band %08lX %08lX excl\r\n",
				      (unsigned long)g_ctl->ex_lo[b],
				      (unsigned long)g_ctl->ex_hi[b]);
			WriteFile(f, line, (DWORD)n, &wrote, NULL);
		}
	}
	n = wsprintfA(line, "# base size offset prot\r\n");
	WriteFile(f, line, (DWORD)n, &wrote, NULL);
	for (i = 0; i < s->nregs; i++) {
		n = wsprintfA(line, "%08lX %08lX %08lX %lX\r\n",
			      (unsigned long)s->regs[i].base, (unsigned long)s->regs[i].size,
			      (unsigned long)off, (unsigned long)s->regs[i].prot);
		WriteFile(f, line, (DWORD)n, &wrote, NULL);
		off += s->regs[i].size;
	}
	CloseHandle(f);
	ss_log("  slotfile: %s written - the .bin is addressable from it\n", path);
}

/* Settings beside each save.
 *
 * A load from another launch runs the old launch's memory under this launch's
 * settings, and then rewinds most of our cached knob values to the save's -
 * the config buffer itself included. A setting changed in between (PhysX off
 * at the save and on at the load, or a rebuilt wrapper) gives a crash that
 * looks like the game's. So every save writes d3d9sw_slotN.cfg with each
 * setting in effect and the wrapper's file identity, and every load compares
 * it with the present before a byte moves. Names come from the memoised
 * environment and the cfg file; nothing here asks the OS for the environment,
 * because the save runs with the game frozen. */
#define SS_CFGSNAP_MAX 8192

static int cfg_text_get(const char *t, int n, const char *name, char *val, int cap)
{
	int i = 0, nlen = lstrlenA(name);

	while (i < n) {
		int s = i, e, k;

		while (i < n && t[i] != '\n')
			i++;
		e = i;
		if (i < n)
			i++;
		while (e > s && (t[e - 1] == '\r' || t[e - 1] == ' ' || t[e - 1] == '\t'))
			e--;
		while (s < e && (t[s] == ' ' || t[s] == '\t'))
			s++;
		if (e - s <= nlen || t[s] == '#' || t[s] == ';')
			continue;
		for (k = 0; k < nlen; k++)
			if ((t[s + k] | 0x20) != (name[k] | 0x20))
				break;
		if (k < nlen)
			continue;
		k = s + nlen;
		while (k < e && (t[k] == ' ' || t[k] == '\t'))
			k++;
		if (k >= e || t[k] != '=')
			continue;
		k++;
		if (val) {
			int m = e - k < cap - 1 ? e - k : cap - 1;

			memcpy(val, t + k, (size_t)m);
			val[m] = 0;
		}
		return 1;
	}
	return 0;
}

static void cfg_snap_add(char *out, int *n, const char *name)
{
	char v[64], line[160];
	DWORD k;
	int len;

	if (!name[0] || cfg_text_get(out, *n, name, NULL, 0))
		return;
	k = ss_getenv(name, v, sizeof(v));
	if (k >= sizeof(v))
		k = 0;
	v[k] = 0;
	len = wsprintfA(line, "%s=%s\r\n", name, v);
	if (*n + len >= SS_CFGSNAP_MAX)
		return;
	memcpy(out + *n, line, (size_t)len);
	*n += len;
}

static int cfg_snapshot(char *out)
{
	char path[MAX_PATH], name[48];
	WIN32_FILE_ATTRIBUTE_DATA fa;
	MEMORY_BASIC_INFORMATION mbi;
	int n = 0, i = 0;

	if (VirtualQuery((LPCVOID)cfg_snapshot, &mbi, sizeof(mbi)) &&
	    GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, sizeof(path)) &&
	    GetFileAttributesExA(path, GetFileExInfoStandard, &fa))
		n = wsprintfA(out, "WRAPPER_BUILD=%lu-%08lX%08lX\r\n", fa.nFileSizeLow,
			      fa.ftLastWriteTime.dwHighDateTime,
			      fa.ftLastWriteTime.dwLowDateTime);
	for (i = 0; i < g_env_memo_n; i++)
		cfg_snap_add(out, &n, g_env_memo[i].name);
	cfg_load();
	i = 0;
	while (g_cfg && i < g_cfg_len) {
		int s, k = 0;

		while (i < g_cfg_len && (g_cfg[i] == ' ' || g_cfg[i] == '\t' ||
					 g_cfg[i] == '\r' || g_cfg[i] == '\n'))
			i++;
		s = i;
		while (i < g_cfg_len && g_cfg[i] != '\n')
			i++;
		if (s >= i || g_cfg[s] == '#' || g_cfg[s] == ';')
			continue;
		while (s + k < i && k < (int)sizeof(name) - 1 && g_cfg[s + k] != '=' &&
		       g_cfg[s + k] != ' ' && g_cfg[s + k] != '\t')
			name[k] = g_cfg[s + k], k++;
		name[k] = 0;
		cfg_snap_add(out, &n, name);
	}
	out[n] = 0;
	return n;
}

static void cfg_write_for_slot(int slotno)
{
	static char snap[SS_CFGSNAP_MAX + 1];
	char path[MAX_PATH];
	HANDLE f;
	DWORD wrote = 0;
	int n = cfg_snapshot(snap);

	slotfile_path(path, sizeof(path), slotno, "cfg");
	f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
			NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	WriteFile(f, snap, (DWORD)n, &wrote, NULL);
	CloseHandle(f);
}

static void cfg_diff_note(const char *name, const char *was, const char *now)
{
	char item[160];
	int have = lstrlenA(g_ctl->cfg_diff), len;

	ss_log("  config: %s was \"%s\" at the save, \"%s\" now\n", name, was, now);
	len = wsprintfA(item, "%s%s %s->%s", have ? ", " : "", name, was, now);
	if (have + len < (int)sizeof(g_ctl->cfg_diff))
		memcpy(g_ctl->cfg_diff + have, item, (size_t)len + 1);
}

/* Before the restore: afterwards our own copy of the settings is the save's. */
static void cfg_compare_with_slot(int slotno)
{
	static char saved[SS_CFGSNAP_MAX + 1], now[SS_CFGSNAP_MAX + 1];
	char path[MAX_PATH], name[48], a[64], b[64];
	HANDLE f;
	DWORD got = 0;
	int ns, nn, i = 0, diffs = 0;

	g_ctl->cfg_diff[0] = 0;
	slotfile_path(path, sizeof(path), slotno, "cfg");
	f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		ss_log("  config: no settings were recorded with this save (older build) - "
		       "cannot tell whether they changed\n");
		return;
	}
	ReadFile(f, saved, SS_CFGSNAP_MAX, &got, NULL);
	CloseHandle(f);
	ns = (int)got;
	saved[ns] = 0;
	nn = cfg_snapshot(now);
	/* Every name on either side, saved side first. */
	for (int pass = 0; pass < 2; pass++) {
		const char *t = pass ? now : saved;
		int n = pass ? nn : ns;

		for (i = 0; i < n;) {
			int s = i, k = 0;

			while (i < n && t[i] != '\n')
				i++;
			if (i < n)
				i++;
			while (s + k < i && k < (int)sizeof(name) - 1 && t[s + k] != '=')
				name[k] = t[s + k], k++;
			name[k] = 0;
			if (!k || (pass && cfg_text_get(saved, ns, name, NULL, 0)))
				continue;
			if (!cfg_text_get(saved, ns, name, a, sizeof(a)))
				lstrcpyA(a, "(unset)");
			if (!cfg_text_get(now, nn, name, b, sizeof(b)))
				lstrcpyA(b, "(unset)");
			if (lstrcmpA(a, b)) {
				cfg_diff_note(name, a, b);
				diffs++;
			}
		}
	}
	if (diffs)
		ss_log("  config: %d setting(s) differ from the save. Our own cached copies "
		       "rewind to the save's values, so the wrapper now runs a mix of "
		       "both - suspect this before the game\n",
		       diffs);
	else
		ss_log("  config: every setting matches the save, and so does the wrapper "
		       "build\n");
}

static void cfg_fault_note(void)
{
	if (g_ctl && g_ctl->cfg_diff[0])
		ss_raw("       settings differ from the loaded save: %s\n", g_ctl->cfg_diff);
}

/* The slot manifest, d3d9sw_slotN.json: JSON Lines, one object per line with a
 * "k" kind, describing the save in words that survive a relaunch - addresses
 * inside a module are written as module+offset. Written beside .meta; for now a
 * load only compares it with what it works out for itself and logs where the
 * two disagree (docs/slot-manifest-proposal.md, phase 1). Fixed buffers, no
 * heap, because the save writes it with the game frozen. */
static char g_mf_buf[16384];
static int g_mf_n, g_mf_lines;
static HANDLE g_mf_f SS_PRESENT;

static void mf_flush(void)
{
	DWORD wrote = 0;

	if (g_mf_n && g_mf_f != INVALID_HANDLE_VALUE)
		WriteFile(g_mf_f, g_mf_buf, (DWORD)g_mf_n, &wrote, NULL);
	g_mf_n = 0;
}

static void mf_put(const char *fmt, ...)
{
	char line[1100];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = wvsprintfA(line, fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	if (g_mf_n + n + 2 > (int)sizeof(g_mf_buf))
		mf_flush();
	memcpy(g_mf_buf + g_mf_n, line, (size_t)n);
	g_mf_n += n;
	g_mf_buf[g_mf_n++] = '\r';
	g_mf_buf[g_mf_n++] = '\n';
	g_mf_lines++;
}

static const char *mf_esc(const char *src, char *dst, int cap)
{
	int n = 0;

	for (; *src && n < cap - 3; src++) {
		if (*src == '\\' || *src == '"')
			dst[n++] = '\\';
		dst[n++] = (unsigned char)*src < ' ' ? '?' : *src;
	}
	dst[n] = 0;
	return dst;
}

static const char *mf_base(const char *path)
{
	const char *b = path;

	for (; *path; path++)
		if (*path == '\\' || *path == '/')
			b = path + 1;
	return b;
}

/* Module+offset when the address is inside a saved module, else the address. */
static const char *mf_key(const Slot *s, uintptr_t a, char *out)
{
	int i;

	for (i = 0; i < s->nmods; i++)
		if (a >= s->mod_lo[i] && a < s->mod_hi[i]) {
			wsprintfA(out, "%s+%lX", s->mod_name[i], (unsigned long)(a - s->mod_lo[i]));
			return out;
		}
	wsprintfA(out, "%08lX", (unsigned long)a);
	return out;
}

/* The same key for an address in this process, from the loader's own list. */
static const char *mf_live_key(uintptr_t a, char *out)
{
	HMODULE m = NULL;
	char path[MAX_PATH];

	if (a && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				    (LPCSTR)a, &m) &&
	    m && GetModuleFileNameA(m, path, sizeof(path))) {
		wsprintfA(out, "%s+%lX", mf_base(path), (unsigned long)(a - (uintptr_t)m));
		return out;
	}
	wsprintfA(out, "%08lX", (unsigned long)a);
	return out;
}

static void mf_self_name(char *out, int cap)
{
	HMODULE m = NULL;
	char path[MAX_PATH];

	out[0] = 0;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			       (LPCSTR)mf_self_name, &m) &&
	    GetModuleFileNameA(m, path, sizeof(path)))
		lstrcpynA(out, mf_base(path), cap);
}

static const ThreadState *mf_ctx_of(const Slot *s, DWORD tid)
{
	int i;

	for (i = 0; i < s->nthreads; i++)
		if (s->threads[i].tid == tid)
			return &s->threads[i];
	return NULL;
}

static const char *mf_region_kind(const Slot *s, const Region *r)
{
	uintptr_t a = r->base;
	int i;

	for (i = 0; i < s->nmods; i++)
		if (a >= s->mod_lo[i] && a < s->mod_hi[i])
			return "image";
	for (i = 0; i < s->nthreads; i++)
		if (s->threads[i].stack_base && a >= s->threads[i].stack_limit - 0x100000 &&
		    a < s->threads[i].stack_base)
			return "stack";
	if (g_ctl)
		for (i = 0; i < g_ctl->nheaps; i++)
			if (a >= g_ctl->heap_lo[i] && a < g_ctl->heap_hi[i])
				return "heap";
	return "private";
}

/* Expectations: what a save assumes about the launch that will load it. The
 * save writes each fact as an "expect" line; a load works the same facts out
 * for itself and logs every one - the same, or different and handled, or
 * different and not handled - before a byte is written. Nothing is refused. */
static unsigned g_xp_pk_n SS_PRESENT;
static unsigned long long g_xp_pk_bytes SS_PRESENT;

void savestate_note_texpack(unsigned contents, unsigned long long bytes)
{
	g_xp_pk_n = contents;
	g_xp_pk_bytes = bytes;
}

static DWORD *xs_seh_last(DWORD head, uintptr_t lo, uintptr_t hi, intptr_t delta);

static void xp_mod(char *val, const char *name)
{
	HMODULE m = GetModuleHandleA(name);
	const IMAGE_NT_HEADERS *nt =
		m ? (const IMAGE_NT_HEADERS *)((const char *)m + ((const IMAGE_DOS_HEADER *)m)->e_lfanew)
		  : NULL;

	if (nt)
		wsprintfA(val, "stamp %08lX size %lX", (unsigned long)nt->FileHeader.TimeDateStamp,
			  (unsigned long)nt->OptionalHeader.SizeOfImage);
	else
		lstrcpyA(val, "not loaded");
}

/* The main thread's stack top, and the handler its SEH chain ends at. */
static void xp_main(uintptr_t *top, DWORD *fin)
{
	HANDLE h = g_ctl && g_ctl->req_tid
			   ? OpenThread(THREAD_QUERY_INFORMATION, FALSE, g_ctl->req_tid)
			   : NULL;
	unsigned char *teb = h ? (unsigned char *)teb_of(h) : NULL;
	DWORD *last;

	*top = 0;
	*fin = 0;
	if (teb) {
		*top = *(const uintptr_t *)(teb + TIB_STACK_BASE);
		last = xs_seh_last(*(const DWORD *)teb, *(const uintptr_t *)(teb + TIB_STACK_LIMIT),
				   *top, 0);
		*fin = last ? last[1] : 0;
	}
	if (h)
		CloseHandle(h);
}

enum { XP_INFO, XP_HANDLED, XP_NOT };

/* Fact i of this launch: its name, value, and what a load does when they
 * differ. 0 past the last. */
static int xp_fact(int i, char *name, char *val, int *kind, const char **what)
{
	static const char *const sysmods[] = { "ntdll.dll", "KERNELBASE.dll", "kernel32.dll",
					       "user32.dll" };
	char exe[MAX_PATH];
	uintptr_t top;
	DWORD fin;

	*kind = XP_NOT;
	*what = "";
	switch (i) {
	case 0:
		lstrcpyA(name, "wrapper build");
		wsprintfA(val, "%08lX", (unsigned long)ss_build_id());
		*what = "our own saved state is skipped, and saved frames in our code return "
			"into the other build's instructions";
		return 1;
	case 1:
		lstrcpyA(name, "game");
		GetModuleFileNameA(NULL, exe, sizeof(exe));
		xp_mod(val, NULL);
		wsprintfA(val + lstrlenA(val), " %s", mf_base(exe));
		*what = "a different game build";
		return 1;
	case 2:
		lstrcpyA(name, "game base");
		wsprintfA(val, "%08lX", (unsigned long)GetModuleHandleA(NULL));
		*kind = XP_HANDLED;
		*what = "pointers into it are shifted and thread start addresses translated";
		return 1;
	case 3:
	case 4:
	case 5:
	case 6:
		wsprintfA(name, "%s build", sysmods[i - 3]);
		xp_mod(val, sysmods[i - 3]);
		*what = "saved frames that return into it land in different code, and pointers "
			"into it are not shifted";
		return 1;
	case 7: {
		typedef LONG(WINAPI * RGV)(OSVERSIONINFOW *);
		RGV rgv = (RGV)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
		OSVERSIONINFOW v;

		memset(&v, 0, sizeof(v));
		v.dwOSVersionInfoSize = sizeof(v);
		if (rgv)
			rgv(&v);
		lstrcpyA(name, "windows");
		wsprintfA(val, "%lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
		*kind = XP_INFO;
		return 1;
	}
	case 8: {
		DWORD n = 64;

		lstrcpyA(name, "machine");
		if (!GetComputerNameA(val, &n))
			lstrcpyA(val, "?");
		*kind = XP_INFO;
		return 1;
	}
	case 9:
		lstrcpyA(name, "main stack top");
		xp_main(&top, &fin);
		wsprintfA(val, "%08lX", (unsigned long)top);
		*kind = XP_HANDLED;
		*what = "the moved stack's frame links and stack pointers are shifted";
		return 1;
	case 10:
		lstrcpyA(name, "SEH final handler");
		xp_main(&top, &fin);
		wsprintfA(val, "%08lX", (unsigned long)fin);
		*kind = XP_HANDLED;
		*what = "rewritten at the end of the moved stack's chain";
		return 1;
	case 11:
		lstrcpyA(name, "texture pack");
		if (g_xp_pk_n)
			wsprintfA(val, "%u contents, %lu KB", g_xp_pk_n,
				  (unsigned long)(g_xp_pk_bytes >> 10));
		else
			lstrcpyA(val, "none open");
		*what = "textures the save names but this pack does not hold paint magenta";
		return 1;
	}
	return 0;
}

static void mf_write(int slotno, const Slot *s)
{
	static char snap[SS_CFGSNAP_MAX + 1];
	char path[MAX_PATH], k1[64], e1[600], e2[600], self[64];
	LARGE_INTEGER t0, t1, f;
	unsigned long long off = 0;
	int i, j, n, nctx = 0;

	QueryPerformanceCounter(&t0);
	slotfile_path(path, sizeof(path), slotno, "json");
	g_mf_f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
			     NULL);
	if (g_mf_f == INVALID_HANDLE_VALUE)
		return;
	g_mf_n = g_mf_lines = 0;
	n = cfg_snapshot(snap);
	mf_self_name(self, sizeof(self));
	for (i = 0; i < s->nids; i++)
		if (mf_ctx_of(s, s->ids[i]))
			nctx++;
	{
		char build[64] = "", exe[MAX_PATH] = "";

		cfg_text_get(snap, n, "WRAPPER_BUILD", build, sizeof(build));
		GetModuleFileNameA(NULL, exe, sizeof(exe));
		mf_put("{\"k\":\"header\",\"v\":1,\"slot\":%d,\"game\":\"%s\",\"wrapper\":\"%s\","
		       "\"wrapper_build\":\"%s\",\"pid\":%lu,\"created\":\"%08lX%08lX\","
		       "\"boot\":\"%08lX%08lX\",\"regions\":%d,\"bytes\":%lu,\"threads\":%d,"
		       "\"rewound\":%d,\"modules\":%d}",
		       slotno, mf_esc(mf_base(exe), e1, sizeof(e1)), mf_esc(self, e2, sizeof(e2)),
		       build, (unsigned long)s->save_pid,
		       (unsigned long)(s->save_created >> 32), (unsigned long)s->save_created,
		       (unsigned long)(s->save_boot >> 32), (unsigned long)s->save_boot,
		       s->nregs, (unsigned long)s->bytes, s->nids, nctx, s->nmods);
	}
	{
		char xn[48], xv[128];
		const char *what;
		int kind;

		for (j = 0; xp_fact(j, xn, xv, &kind, &what); j++)
			mf_put("{\"k\":\"expect\",\"name\":\"%s\",\"val\":\"%s\"}", xn,
			       mf_esc(xv, e1, 200));
	}
	for (i = 0; i < n;) {
		int b = i, e;
		char name[48], val[64];

		while (i < n && snap[i] != '\n')
			i++;
		e = i;
		if (i < n)
			i++;
		for (j = 0; b + j < e && j < (int)sizeof(name) - 1 && snap[b + j] != '='; j++)
			name[j] = snap[b + j];
		name[j] = 0;
		if (!j || !lstrcmpA(name, "WRAPPER_BUILD") ||
		    !cfg_text_get(snap, n, name, val, sizeof(val)))
			continue;
		mf_put("{\"k\":\"cfg\",\"name\":\"%s\",\"val\":\"%s\"}", mf_esc(name, e1, 100),
		       mf_esc(val, e2, 140));
	}
	for (i = 0; i < s->nmods; i++)
		mf_put("{\"k\":\"module\",\"name\":\"%s\",\"base\":\"%08lX\",\"size\":\"%lX\","
		       "\"stamp\":\"%08lX\",\"path\":\"%s\"}",
		       mf_esc(s->mod_name[i], e1, 80), (unsigned long)s->mod_lo[i],
		       (unsigned long)(s->mod_hi[i] - s->mod_lo[i]), (unsigned long)s->mod_stamp[i],
		       mf_esc(s->mod_path[i], e2, sizeof(e2)));
	for (i = 0; i < s->nids; i++) {
		const ThreadState *t = mf_ctx_of(s, s->ids[i]);
		int ord = 0;

		for (j = 0; j < i; j++)
			if (s->starts[j] == s->starts[i])
				ord++;
		mf_key(s, (uintptr_t)s->starts[i], k1);
		if (t)
			mf_put("{\"k\":\"thread\",\"tid\":%lu,\"entry\":\"%s\",\"role\":\"%s#%d\","
			       "\"rewound\":1,\"stack\":[\"%08lX\",\"%08lX\"],\"teb\":\"%08lX\"}",
			       (unsigned long)s->ids[i], mf_esc(k1, e1, 80), e1, ord,
			       (unsigned long)t->stack_limit, (unsigned long)t->stack_base,
			       (unsigned long)t->teb);
		else
			mf_put("{\"k\":\"thread\",\"tid\":%lu,\"entry\":\"%s\",\"role\":\"%s#%d\","
			       "\"rewound\":0}",
			       (unsigned long)s->ids[i], mf_esc(k1, e1, 80), e1, ord);
	}
	for (i = 0; i < s->nregs; i++) {
		const Region *r = &s->regs[i];

		mf_put("{\"k\":\"region\",\"base\":\"%08lX\",\"size\":\"%lX\",\"off\":\"%lX\","
		       "\"prot\":\"%lX\",\"kind\":\"%s\",\"in\":\"%s\"}",
		       (unsigned long)r->base, (unsigned long)r->size, (unsigned long)off,
		       (unsigned long)r->prot, mf_region_kind(s, r), mf_key(s, r->base, k1));
		off += r->size;
	}
	mf_put("{\"k\":\"end\",\"lines\":%d}", g_mf_lines + 1);
	mf_flush();
	CloseHandle(g_mf_f);
	g_mf_f = INVALID_HANDLE_VALUE;
	QueryPerformanceCounter(&t1);
	QueryPerformanceFrequency(&f);
	ss_log("  slotfile: %s written, %d line(s), %d ms\n", path, g_mf_lines,
	       (int)((t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart));
}

/* A string field of one manifest line, unescaped. */
static int mf_str(const char *l, int len, const char *key, char *out, int cap)
{
	char pat[40];
	int pl = wsprintfA(pat, "\"%s\":\"", key), i, n = 0;

	for (i = 0; i + pl <= len; i++)
		if (!memcmp(l + i, pat, (size_t)pl))
			break;
	if (i + pl > len)
		return 0;
	for (i += pl; i < len && l[i] != '"' && n < cap - 1; i++) {
		if (l[i] == '\\' && i + 1 < len)
			i++;
		out[n++] = l[i];
	}
	out[n] = 0;
	return 1;
}

static long mf_num(const char *l, int len, const char *key, long dflt)
{
	char pat[40];
	int pl = wsprintfA(pat, "\"%s\":", key), i;
	long v = 0, neg = 0;

	for (i = 0; i + pl <= len; i++)
		if (!memcmp(l + i, pat, (size_t)pl))
			break;
	if (i + pl >= len)
		return dflt;
	i += pl;
	if (l[i] == '-')
		neg = 1, i++;
	if (i >= len || l[i] < '0' || l[i] > '9')
		return dflt;
	while (i < len && l[i] >= '0' && l[i] <= '9')
		v = v * 10 + (l[i++] - '0');
	return neg ? -v : v;
}

#define SS_MF_ROLES 128

/* Before the restore, like the config comparison: reads the slot's manifest
 * and checks it against the .meta the load is about to use and against the
 * threads and modules of this process. Logs only. */
static int reloc_live_of(const Slot *s, int i);

static void xp_check(int slotno, const Slot *s)
{
	char path[MAX_PATH], w[16], name[48], sv[200], xn[48], xv[128];
	const char *what;
	HANDLE fh;
	DWORD size, got = 0;
	char *buf;
	int i, j, kind, seen = 0, same = 0, handled = 0, nothandled = 0;
	int ident = 0, rebuilt = 0, absent = 0;

	slotfile_path(path, sizeof(path), slotno, "json");
	fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
			 FILE_ATTRIBUTE_NORMAL, NULL);
	if (fh == INVALID_HANDLE_VALUE)
		return;
	size = GetFileSize(fh, NULL);
	buf = size && size != INVALID_FILE_SIZE
		      ? (char *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
		      : NULL;
	if (!buf || !ReadFile(fh, buf, size, &got, NULL) || got != size) {
		CloseHandle(fh);
		if (buf)
			VirtualFree(buf, 0, MEM_RELEASE);
		return;
	}
	CloseHandle(fh);
	for (i = 0; i < (int)size;) {
		const char *l = buf + i;
		int len = 0;

		while (i < (int)size && buf[i] != '\n')
			i++, len++;
		if (i < (int)size)
			i++;
		if (!mf_str(l, len, "k", w, sizeof(w)) || lstrcmpA(w, "expect") ||
		    !mf_str(l, len, "name", name, sizeof(name)) ||
		    !mf_str(l, len, "val", sv, sizeof(sv)))
			continue;
		if (!seen++)
			ss_log("  expect: what this save assumes, against this launch\n");
		for (j = 0; xp_fact(j, xn, xv, &kind, &what); j++)
			if (!lstrcmpA(xn, name))
				break;
		if (!xn[0] || lstrcmpA(xn, name)) {
			ss_log("  expect: %-18s %s - this build does not know the fact\n", name, sv);
			continue;
		}
		if (!lstrcmpA(sv, xv)) {
			same++;
			ss_log("  expect: %-18s %s - same\n", name, sv);
			continue;
		}
		if (!lstrcmpA(name, "texture pack")) {
			unsigned a = (unsigned)strtoul(sv, NULL, 10), b = (unsigned)strtoul(xv, NULL, 10);

			if (b >= a && a) {
				kind = XP_HANDLED;
				what = "this pack holds at least as many contents";
			}
		}
		if (kind == XP_INFO)
			ss_log("  expect: %-18s saved %s, here %s\n", name, sv, xv);
		else
			ss_log("  expect: %-18s saved %s, here %s - %s: %s\n", name, sv, xv,
			       kind == XP_HANDLED ? "handled" : "NOT HANDLED", what);
		if (kind == XP_HANDLED)
			handled++;
		else if (kind == XP_NOT)
			nothandled++;
	}
	VirtualFree(buf, 0, MEM_RELEASE);
	if (!seen) {
		ss_log("  expect: none with this save (older build)\n");
		return;
	}
	for (i = 0; i < s->nmods; i++) {
		if (reloc_live_of(s, i) >= 0)
			ident++;
		else if (GetModuleHandleA(s->mod_name[i]))
			rebuilt++;
		else
			absent++;
	}
	ss_log("  expect: modules            %d saved - %d the same build here, %d a different "
	       "build%s, %d not loaded\n",
	       s->nmods, ident, rebuilt, rebuilt ? " (NOT HANDLED: frames into them return into "
					    "different code)" : "",
	       absent);
	if (rebuilt)
		nothandled++;
	ss_log("  expect: %d fact(s) the same, %d different and handled, %d different and NOT "
	       "handled%s\n",
	       same, handled, nothandled,
	       nothandled ? " - if this load fails, start with those" : "");
}

static void mf_check(int slotno, const Slot *s)
{
	static char roles[SS_MF_ROLES][64];
	static int saved_n[SS_MF_ROLES], live_n[SS_MF_ROLES];
	char path[MAX_PATH], self[64], v[600], w[64];
	LARGE_INTEGER t0, t1, f;
	HANDLE fh, snap;
	DWORD size, got = 0, me = GetCurrentThreadId(), pid = GetCurrentProcessId();
	char *buf;
	int nroles = 0, i, k, lines = 0, end = 0, bad = 0, nshort = 0, said = 0;
	int mods = 0, moved = 0, gone = 0, rebuilt = 0;

	g_ctl->mf_short = -1;
	QueryPerformanceCounter(&t0);
	slotfile_path(path, sizeof(path), slotno, "json");
	fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
			 FILE_ATTRIBUTE_NORMAL, NULL);
	if (fh == INVALID_HANDLE_VALUE) {
		ss_log("  manifest: none with this save (older build)\n");
		return;
	}
	size = GetFileSize(fh, NULL);
	buf = size && size != INVALID_FILE_SIZE
		      ? (char *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
		      : NULL;
	if (!buf || !ReadFile(fh, buf, size, &got, NULL) || got != size) {
		CloseHandle(fh);
		if (buf)
			VirtualFree(buf, 0, MEM_RELEASE);
		ss_log("  manifest: %s could not be read\n", path);
		return;
	}
	CloseHandle(fh);
	mf_self_name(self, sizeof(self));
	for (i = 0; i < (int)size;) {
		const char *l = buf + i;
		int len = 0;

		while (i < (int)size && buf[i] != '\n')
			i++, len++;
		if (i < (int)size)
			i++;
		if (len < 8)
			continue;
		lines++;
		if (!mf_str(l, len, "k", w, sizeof(w)))
			continue;
		if (!lstrcmpA(w, "header")) {
			long r = mf_num(l, len, "regions", -1), t = mf_num(l, len, "threads", -1);
			long b = mf_num(l, len, "bytes", -1);

			if (r != s->nregs || t != s->nids || (unsigned long)b != (unsigned long)s->bytes) {
				ss_log("  manifest: DISAGREES with .meta - %ld region(s), %ld "
				       "thread(s), %ld byte(s) against %d, %d, %lu\n",
				       r, t, b, s->nregs, s->nids, (unsigned long)s->bytes);
				bad++;
			}
		} else if (!lstrcmpA(w, "end")) {
			end = 1;
		} else if (!lstrcmpA(w, "module")) {
			char name[64], base[16], stamp[16];
			HMODULE m;

			mods++;
			if (!mf_str(l, len, "name", name, sizeof(name)) ||
			    !mf_str(l, len, "base", base, sizeof(base)) ||
			    !mf_str(l, len, "stamp", stamp, sizeof(stamp)))
				continue;
			m = GetModuleHandleA(name);
			if (!m) {
				gone++;
				continue;
			}
			wsprintfA(v, "%08lX", (unsigned long)(uintptr_t)m);
			if (lstrcmpA(v, base))
				moved++;
			{
				IMAGE_DOS_HEADER *dh = (IMAGE_DOS_HEADER *)m;
				IMAGE_NT_HEADERS *nh = (IMAGE_NT_HEADERS *)((char *)m + dh->e_lfanew);

				wsprintfA(v, "%08lX", (unsigned long)nh->FileHeader.TimeDateStamp);
				if (lstrcmpA(v, stamp)) {
					rebuilt++;
					if (said++ < 8)
						ss_log("  manifest: %s is a different build now "
						       "(stamp %s at the save, %s now)\n",
						       name, stamp, v);
				}
			}
		} else if (!lstrcmpA(w, "thread") && mf_num(l, len, "rewound", 0) == 1) {
			char *hash;

			if (!mf_str(l, len, "entry", v, sizeof(v)))
				continue;
			hash = v;
			{
				int sl = lstrlenA(self);
				char pre[64];

				lstrcpynA(pre, v, sl + 1 < (int)sizeof(pre) ? sl + 1 : (int)sizeof(pre));
				if (sl && v[sl] == '+' && !lstrcmpiA(pre, self))
					continue;
			}
			for (k = 0; k < nroles; k++)
				if (!lstrcmpiA(roles[k], hash))
					break;
			if (k == nroles) {
				if (nroles == SS_MF_ROLES)
					continue;
				lstrcpynA(roles[nroles], hash, sizeof(roles[0]));
				saved_n[nroles] = live_n[nroles] = 0;
				nroles++;
			}
			saved_n[k]++;
		}
	}
	VirtualFree(buf, 0, MEM_RELEASE);
	if (!end) {
		ss_log("  manifest: %s ends early (%d line(s)) - written by a save that did "
		       "not finish\n",
		       path, lines);
		bad++;
	}
	snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap != INVALID_HANDLE_VALUE) {
		THREADENTRY32 te;

		te.dwSize = sizeof(te);
		if (Thread32First(snap, &te))
			do {
				HANDLE h;

				if (te.th32OwnerProcessID != pid || te.th32ThreadID == me ||
				    te.th32ThreadID == g_ctl->wd_tid || xs_spare_idx(te.th32ThreadID) >= 0)
					continue;
				h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
				if (!h)
					continue;
				mf_live_key((uintptr_t)start_of(h), v);
				CloseHandle(h);
				for (k = 0; k < nroles; k++)
					if (!lstrcmpiA(roles[k], v)) {
						live_n[k]++;
						break;
					}
			} while (Thread32Next(snap, &te));
		CloseHandle(snap);
	}
	for (k = 0; k < nroles; k++)
		if (live_n[k] < saved_n[k]) {
			nshort += saved_n[k] - live_n[k];
			ss_log("  manifest: role %s - %d rewound at the save, %d live now\n",
			       roles[k], saved_n[k], live_n[k]);
		}
	g_ctl->mf_short = nshort;
	QueryPerformanceCounter(&t1);
	QueryPerformanceFrequency(&f);
	ss_log("  manifest: %d line(s), %s; %d module(s): %d moved, %d not loaded, %d a "
	       "different build; %d rewound role(s), %d thread(s) with no live thread in "
	       "their role; %d ms\n",
	       lines, bad ? "DISAGREES with the save it sits beside" : "agrees with .meta",
	       mods, moved, gone, rebuilt, nroles, nshort,
	       (int)((t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart));
}

/* Never a local copy of a Slot. SS_MAX_REGIONS is 65536 and every one of 256
 * thread slots carries a CONTEXT, so the struct runs to several megabytes and a
 * stack copy of it is an immediate C00000FD - which is exactly how the first
 * version of this ended, freezing the process on the first F5. The handle is
 * nulled in place and put back instead. */
static void slotfile_write_meta(int slotno, Slot *s)
{
	char path[MAX_PATH];
	HANDLE f, keep;
	DWORD wrote = 0;

	slotfile_path(path, sizeof(path), slotno, "meta");
	f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
			NULL);
	if (f == INVALID_HANDLE_VALUE) {
		ss_log("  slotfile: cannot write %s, err=%lu\n", path, GetLastError());
		return;
	}
	/* Meaningless in another process, and leaving a stale value in it is how a
	 * hydrated slot would close somebody else's handle. */
	keep = s->sect;
	s->sect = NULL;
	WriteFile(f, s, (DWORD)sizeof(*s), &wrote, NULL);
	s->sect = keep;
	CloseHandle(f);
	ss_log("  slotfile: %s written, %d region(s), %d thread(s), %.1f MB of bytes "
	       "alongside\n",
	       path, s->nregs, s->nthreads, (double)s->bytes / (1024.0 * 1024.0));
	slotfile_write_index(slotno, s);
	cfg_write_for_slot(slotno);
	mf_write(slotno, s);
}

/* Fills an empty slot from the pair of files, so Shift+F5 in a fresh session
 * has something to restore. */
static int slotfile_read(int slotno, Slot *s)
{
	char path[MAX_PATH];
	HANDLE f;
	DWORD got = 0;
	HANDLE sect;

	slotfile_path(path, sizeof(path), slotno, "meta");
	f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	/* Straight into the slot, for the same reason the writer does not take a
	 * copy: this struct is far too large to sit on a stack. Safe only because
	 * the caller checked the slot was empty, and any failure below leaves it
	 * marked invalid. */
	s->valid = 0;
	if (!ReadFile(f, s, (DWORD)sizeof(*s), &got, NULL) || got != sizeof(*s)) {
		ss_log("  slotfile: %s is %lu bytes, expected %u - ignoring it (written by "
		       "a build with a different slot layout)\n",
		       path, got, (unsigned)sizeof(*s));
		CloseHandle(f);
		return 0;
	}
	CloseHandle(f);
	if (s->fmt != SS_SLOT_FMT) {
		ss_log("  slotfile: %s is slot format %u, this build reads %u - ignoring it\n",
		       path, s->fmt, (unsigned)SS_SLOT_FMT);
		s->valid = 0;
		return 0;
	}

	slotfile_path(path, sizeof(path), slotno, "bin");
	f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		ss_log("  slotfile: %s missing, err=%lu - the description is no use "
		       "without the bytes\n",
		       path, GetLastError());
		return 0;
	}
	sect = CreateFileMappingA(f, NULL, PAGE_READWRITE, 0, 0, NULL);
	CloseHandle(f);
	if (!sect) {
		ss_log("  slotfile: cannot map %s, err=%lu\n", path, GetLastError());
		return 0;
	}
	s->sect = sect;
	s->valid = 1;
	ss_log("  slotfile: loaded a slot taken in another session - %d region(s), %d "
	       "thread(s), %.1f MB. Nothing about this restore is expected to be "
	       "safe; the run is the experiment\n",
	       s->nregs, s->nthreads, (double)s->bytes / (1024.0 * 1024.0));
	return 1;
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

		if (!g_ctl->handles[i] || !g_did_suspend[i])
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

static int alloc_settle(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_ALLOCSETTLE", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '0') ? 0 : 1;
	}
	return cached;
}

/* Is any thread running ntdll code that is not a parked system call?
 *
 * Heap locks do not answer this. blk_lock_all holds every heap's lock while the
 * block map is walked, which excludes the classic allocator paths - but the low
 * fragmentation heap deliberately does not take that lock. Its fast path runs on
 * interlocked SLists precisely so that it never has to, so HeapLock succeeds
 * while a thread is halfway through an LFH allocation, and the snapshot is taken
 * anyway.
 *
 * That is the fault at 0D2AF674: thread 4488 was suspended inside the LFH path
 * with ntdll+90CF5 and ntdll+43ACF beneath it. Its stack was held in the present
 * because it is a system thread we exclude, and while it stood still we wrote 92
 * MB of block contents underneath it. On resume its locals still pointed where
 * they had pointed, but the bytes there had been replaced with bytes from the
 * past, so the target it computed was not a function and it jumped into its own
 * stack - the instruction pointer landed 348 bytes above the stack pointer, in
 * the same allocation.
 *
 * Nothing can be restored differently to fix that. Both halves of the
 * inconsistency are legitimate: the stack must stay in the present because it
 * has kernel-side state we cannot rewind, and the heap must go back because that
 * is the whole point. As with the JIT, the only repair is not to take the
 * picture at that moment.
 *
 * The test has to be coarse, because the functions involved are internal and
 * naming them means hard-coding offsets into whichever ntdll this machine
 * shipped. But the whole module is too coarse the other way: threads park in
 * ntdll for their entire lives waiting on handles, and a loop that waits for
 * those would never settle. So the parked ones are subtracted by address. Every
 * Nt* system call stub lives in one contiguous block, so a few exported ones
 * give its bounds, and a thread sitting in any wait is inside it. A thread in
 * ntdll but outside it is running real code, which for our purposes is close
 * enough to "might be in the allocator" - the false positives are things like
 * critical section entry, which are over in microseconds and cost us one more
 * turn of the loop. */
static uintptr_t g_ntd_lo, g_ntd_hi, g_stub_lo, g_stub_hi;

/* Deliberately separate from the test below, and called with the process still
 * running.
 *
 * GetProcAddress takes the loader lock and can allocate, and the test it feeds
 * is only ever asked its question with every thread suspended. Doing the setup
 * there too would mean reaching for the loader lock at the one moment nobody can
 * hand it over, which is the shape of half the deadlocks this file already
 * guards against. It costs nothing to look the addresses up early, so it happens
 * early. */
static void alloc_ranges_init(void)
{
	static uintptr_t lo, hi, stub_lo, stub_hi;

	if (!hi) {
		HMODULE m = GetModuleHandleA("ntdll.dll");
		if (!m) {
			hi = 1; /* cannot happen, but never look again if it does */
			goto publish;
		}
		{
			IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)m;
			IMAGE_NT_HEADERS *nt =
				(IMAGE_NT_HEADERS *)((unsigned char *)m + dos->e_lfanew);
			lo = (uintptr_t)m;
			hi = lo + nt->OptionalHeader.SizeOfImage;
		}
		{
			/* Spread deliberately across the alphabet, because the stubs are
			 * laid out in system call number order and a handful of names from
			 * one letter would bound only a slice of the block. */
			static const char *const nm[] = {
				"NtWaitForSingleObject", "NtWaitForMultipleObjects",
				"NtDelayExecution", "NtRemoveIoCompletion",
				"NtReadFile", "NtWriteFile", "NtDeviceIoControlFile",
				"NtRequestWaitReplyPort", "NtAlpcSendWaitReceivePort",
				"NtWaitForWorkViaWorkerFactory", "NtSetEvent",
				"NtQueryObject", "NtClose", "NtCreateFile",
				"NtQueryInformationThread", "NtReleaseMutant",
			};
			unsigned k;

			for (k = 0; k < sizeof(nm) / sizeof(nm[0]); k++) {
				uintptr_t a = (uintptr_t)GetProcAddress(m, nm[k]);
				if (!a)
					continue;
				if (!stub_lo || a < stub_lo)
					stub_lo = a;
				if (a + 32 > stub_hi)
					stub_hi = a + 32;
			}
			if (!stub_lo) {
				/* No stubs found means no way to tell a parked thread from a
				 * working one, and a settle loop that cannot tell would burn
				 * every attempt and warn on every save. Better to say so once
				 * and stay out of the way. */
				hi = 1;
				ss_log("  allocator settle: OFF - could not locate ntdll's "
				       "system call stubs, so a parked thread cannot be told "
				       "from one inside the heap\n");
				goto publish;
			}
		}
	}
publish:
	g_ntd_lo = lo;
	g_ntd_hi = hi;
	g_stub_lo = stub_lo;
	g_stub_hi = stub_hi;
}

static int in_the_allocator(unsigned *who, uintptr_t *where)
{
	uintptr_t lo = g_ntd_lo, hi = g_ntd_hi;
	uintptr_t stub_lo = g_stub_lo, stub_hi = g_stub_hi;
	int i;

	if (hi <= 1)
		return 0;
	for (i = 0; i < g_ctl->nids; i++) {
		CONTEXT c;
		uintptr_t pc;

		if (!g_ctl->handles[i] || !g_did_suspend[i])
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
		if (pc >= lo && pc < hi && !(pc >= stub_lo && pc < stub_hi)) {
			if (who)
				*who = g_ctl->ids[i];
			if (where)
				*where = pc - lo; /* an RVA, so the log can be compared across runs */
			return 1;
		}
	}
	return 0;
}

/* What the threads we are NOT rewinding are currently holding.
 *
 * Park settled the question the other diagnostics could not: fifty-four threads
 * stopped for a hundred seconds and the game carried on, so being suspended is
 * harmless. What is not harmless is being suspended and then resumed into
 * memory that changed while you were still holding a pointer into it.
 *
 * That is a question with an answer. Every thread is stopped and its registers
 * and stack are readable, so before a single block is written we can ask which
 * of them a foreign thread is actually looking at, and decline those. It needs
 * no type information and relocates nothing - the test is only "is some thread
 * that keeps running pointed at this exact range", which is decidable.
 *
 * Only threads marked transient are consulted. A game thread's pointers are
 * expected to point at restored memory; that is the entire point. It is the
 * ones that keep their registers and keep running that must not be lied to.
 *
 * The live stack is scanned rather than the whole reservation. Anything below
 * the stack pointer is dead frames, and the words above it are what the thread
 * will return through. Bounded, because a scan proportional to the reservation
 * would cost more than the restore. */
#define HELD_CAP 262144
#define HELD_STACK_WORDS 8192

static uintptr_t *g_held SS_PRESENT;
static int g_held_n;
static int g_held_full;
static int g_held_threads;

/* Is this block somebody else's object, judged by the vtable at its head?
 *
 * Cheap and exact where it fires: read the first word, ask which module it
 * lands in, and if that module is one we hold in the present then the object is
 * that module's and rewinding its contents is a lie told to code that never
 * went back. Anything else - a word that is not a pointer, a pointer into a
 * module we do rewind, a pointer into the heap - falls through and is judged by
 * the ownership tests as before. */
/* Which module's objects, tallied per restore. A count alone cannot tell
 * "DirectSound's buffers" from "half the game", and that is the only question
 * worth asking about a rule that withholds memory from the rewind. */
static int g_vtab_bymod[SS_MAX_MODS];

static int vtab_on(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_VTABVETO", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '0') ? 0 : 1;
	}
	return cached;
}

static int foreign_object(uintptr_t b, unsigned long n, int *who)
{
	uintptr_t w, f;
	int m;

	if (!vtab_on() || n < sizeof(uintptr_t) || !g_ctl)
		return 0;
	if (!ss_readable(b, sizeof(uintptr_t)))
		return 0;
	w = *(const uintptr_t *)b;
	if (w < 0x10000)
		return 0;
	m = module_of(w);
	if (m < 0 || g_ctl->mod_rewound[m])
		return 0;
	/* Confirm it is a vtable rather than merely a pointer into a module.
	 *
	 * The first version asked only whether the head word landed in a module we
	 * hold, and that caught 10220 blocks averaging 82 bytes apiece - small
	 * objects whose first field happens to be a pointer to a DLL's data, which
	 * is an ordinary thing for a C++ program to contain. Not rewinding ten
	 * thousand of the game's own objects to protect a few hundred of
	 * DirectSound's is a bad trade in the direction that does not announce
	 * itself.
	 *
	 * A vtable is a table of function pointers, so read the first slot. If it
	 * is executable and in the same module as the table, this is an object
	 * with virtual methods belonging to that module. A pointer to a locale
	 * struct or a string constant will not survive both tests. */
	if (!ss_readable(w, sizeof(uintptr_t)))
		return 0;
	f = *(const uintptr_t *)w;
	if (!ss_is_code(f) || module_of(f) != m)
		return 0;
	if (who)
		*who = m;
	return 1;
}

static int held_on(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_HELDVETO", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '0') ? 0 : 1;
	}
	return cached;
}

static int held_cmp(const void *a, const void *b)
{
	uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
	return x < y ? -1 : x > y ? 1 : 0;
}

static void held_add(uintptr_t v)
{
	/* A value only matters if it could name a heap block. Anything below the
	 * first user page is a small integer wearing a pointer's clothes, and there
	 * are a great many of those on a stack. */
	if (v < 0x10000)
		return;
	if (g_held_n >= HELD_CAP) {
		g_held_full = 1;
		return;
	}
	g_held[g_held_n++] = v;
}

static void held_build(void)
{
	int i;

	g_held_n = 0;
	g_held_full = 0;
	g_held_threads = 0;
	if (!held_on() || !g_ctl)
		return;
	if (!g_held) {
		g_held = (uintptr_t *)blk_arena(HELD_CAP * sizeof(uintptr_t));
		if (!g_held)
			return;
	}
	for (i = 0; i < g_ctl->nids; i++) {
		CONTEXT c;
		uintptr_t sp, top;
		MEMORY_BASIC_INFORMATION mbi;
		int w;

		if (!g_ctl->transient[i] || !g_ctl->handles[i] || !g_did_suspend[i])
			continue;
		memset(&c, 0, sizeof(c));
		c.ContextFlags = CONTEXT_FULL;
		if (!GetThreadContext(g_ctl->handles[i], &c))
			continue;
		g_held_threads++;
#if defined(_M_IX86) || defined(__i386__)
		held_add((uintptr_t)c.Eax);
		held_add((uintptr_t)c.Ebx);
		held_add((uintptr_t)c.Ecx);
		held_add((uintptr_t)c.Edx);
		held_add((uintptr_t)c.Esi);
		held_add((uintptr_t)c.Edi);
		held_add((uintptr_t)c.Ebp);
		sp = (uintptr_t)c.Esp;
#else
		held_add((uintptr_t)c.Rax);
		held_add((uintptr_t)c.Rbx);
		held_add((uintptr_t)c.Rcx);
		held_add((uintptr_t)c.Rdx);
		held_add((uintptr_t)c.Rsi);
		held_add((uintptr_t)c.Rdi);
		held_add((uintptr_t)c.Rbp);
		held_add((uintptr_t)c.R8);
		held_add((uintptr_t)c.R9);
		held_add((uintptr_t)c.R10);
		held_add((uintptr_t)c.R11);
		held_add((uintptr_t)c.R12);
		held_add((uintptr_t)c.R13);
		held_add((uintptr_t)c.R14);
		held_add((uintptr_t)c.R15);
		sp = (uintptr_t)c.Rsp;
#endif
		if (!sp)
			continue;
		/* The end of this stack's committed range, so the scan stops at the
		 * top of the thread rather than walking into whatever follows it. */
		top = sp + HELD_STACK_WORDS * sizeof(uintptr_t);
		if (VirtualQuery((LPCVOID)sp, &mbi, sizeof(mbi)) == sizeof(mbi)) {
			uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;

			if (end < top)
				top = end;
		}
		sp = (sp + sizeof(uintptr_t) - 1) & ~(uintptr_t)(sizeof(uintptr_t) - 1);
		for (w = 0; sp + sizeof(uintptr_t) <= top; sp += sizeof(uintptr_t), w++) {
			if (!ss_readable(sp, sizeof(uintptr_t)))
				break;
			held_add(*(const uintptr_t *)sp);
		}
	}
	if (g_held_n > 1)
		qsort(g_held, (size_t)g_held_n, sizeof(uintptr_t), held_cmp);
	if (g_held_full)
		ss_log("  held: the table filled at %d, so some of what the surviving "
		       "threads point at is invisible and those blocks will be written "
		       "anyway\n", HELD_CAP);
}

/* Is any surviving thread pointed into [base, base+size)? */
static int held_hit(uintptr_t base, unsigned long size)
{
	int lo = 0, hi = g_held_n;

	if (!g_held_n)
		return 0;
	while (lo < hi) {
		int mid = lo + (hi - lo) / 2;

		if (g_held[mid] < base)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < g_held_n && g_held[lo] < base + size;
}

static int settle_tries(void);
static int ensure_helper(void);

/* The freeze on its own, with nothing in the middle.
 *
 * Every failure so far has been read as a restore failure, but a restore is
 * three things at once: the process is held still, memory is copied, and memory
 * is written back. Only the last two have ever been varied. Nobody has asked
 * whether a game of this vintage survives simply being stopped for a third of a
 * second and started again, and if the answer is no then every conclusion drawn
 * from a save-and-restore has been measuring the wrong one of the three.
 *
 * Deliberately the same shape as a save: the audio is silenced first, the same
 * settle loops run, the same threads are collected and suspended, and the same
 * resume and audio restart happen at the end. The only difference is that the
 * middle is a sleep. If parking is survivable and restoring is not, the copy or
 * the write-back is to blame. If parking alone kills it, nothing further up the
 * stack matters until that is fixed.
 *
 * Runs on the calling thread, like a save and unlike a restore, because a park
 * returns to its caller - there is no rewind to carry it away. */
int savestate_park(int ms)
{
	LARGE_INTEGER pf, t0, t1, t2;
	int held;

	/* Park is the first thing here that runs before any save, so it is also the
	 * first to discover that the control block does not exist until one
	 * happens. Six parks were requested and six returned instantly on the
	 * strength of a bare "if (!g_ctl) return 0" - the key was right, the wiring
	 * was right, and nothing ran. The block carries the thread list and the log
	 * handle, so it has to exist before anything can be held still or said. */
	if (!g_ctl && !ensure_helper())
		return 0;
	if (!g_ctl)
		return 0;
	if (ms <= 0)
		ms = 1000;
	QueryPerformanceFrequency(&pf);
	ss_log("park: holding the whole process still for %d ms, touching no memory\n", ms);
	dsh_save();
	dsh_quiet();
	park_audio_gpu();
	QueryPerformanceCounter(&t0);
	if (alloc_settle())
		alloc_ranges_init();
	collect_threads();
	suspend_all();
	/* Skipped under Wine: resume+resuspend would freeze the wineserver
	 * waiters that suspend_all just left running on purpose. */
	if (!ss_under_wine()) {
		int tries = settle_tries(), n = 0;
		unsigned who = 0, awho = 0;
		uintptr_t where = 0, arva = 0;
		int jit, alloc;

		for (;;) {
			jit = in_the_jit(&who, &where);
			alloc = alloc_settle() && in_the_allocator(&awho, &arva);
			if ((!jit && !alloc) || n >= tries)
				break;
			resume_all(0);
			Sleep(2);
			collect_threads();
			suspend_all();
			n++;
		}
		if (jit)
			ss_log("  park: still inside the JIT after %d attempt(s) (thread %u)\n",
			       n, who);
		if (alloc)
			ss_log("  park: thread %u still running ntdll at +%lX after %d "
			       "attempt(s)\n", awho, (unsigned long)arva, n);
	}
	held = g_ctl->nids;
	QueryPerformanceCounter(&t1);
	/* The whole experiment. Sleep rather than a spin, because a spinning
	 * thread is one more thread doing something and the point is that nothing
	 * is. */
	Sleep((DWORD)ms);
	resume_all(0);
	dsh_play();
	xa2_sw_resume();
	QueryPerformanceCounter(&t2);
	ss_log("park: %d thread(s) were held for %.0f ms (%.1f ms to freeze them, %.1f ms "
	       "total). No memory was read or written. If the game is still playing, the "
	       "freeze is not what kills restores\n",
	       held, (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)pf.QuadPart,
	       (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)pf.QuadPart,
	       (double)(t2.QuadPart - t0.QuadPart) * 1000.0 / (double)pf.QuadPart);
	heap_check("after a park with no restore");
	return 1;
}

/* Write the game's own image out as it exists in memory, for a disassembler.
 *
 * rabiribi.exe ships with its .text encrypted behind a Steam .bind stub. The
 * measurement is unambiguous: 3.7 MB of supposed x86 code at 8.00 bits of
 * entropy per byte and not one `push ebp; mov ebp,esp` in the whole section,
 * where a real image that size would have thousands. .rdata and .data are
 * plaintext, which is why the string that named DxSound.cpp could be read off
 * disk, but every function body a static tool shows is decryption noise.
 *
 * We are on the other side of that. The stub decrypts into memory and we are in
 * the memory, so the only thing standing between us and a readable disassembly
 * is writing the bytes down.
 *
 * Three fixups make the result loadable rather than merely present. The section
 * headers get PointerToRawData set to VirtualAddress, because on disk a section
 * is packed to FileAlignment and in memory it is spread to SectionAlignment, and
 * a loader told otherwise reads every section from the wrong place. FileAlignment
 * is raised to match SectionAlignment so that remains self-consistent. And
 * ImageBase is rewritten to wherever ASLR actually put us, so that the absolute
 * addresses baked into the code - which is all of them, this being a 32-bit image
 * whose relocations were already applied - agree with where the disassembler
 * thinks it is looking.
 *
 * The sidecar exists because a dumped IAT holds resolved addresses rather than
 * names, so every call through it disassembles as an indirect jump to a bare
 * number. The import table usually survives and is walked when it does; the
 * module list is written unconditionally, because it is what lets any raw
 * pointer anywhere in the dump be attributed even when the table does not. */
int savestate_dump_image(void)
{
	const unsigned char *base = (const unsigned char *)GetModuleHandleA(NULL);
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
	const IMAGE_NT_HEADERS *nt;
	unsigned char *buf;
	IMAGE_NT_HEADERS *out;
	IMAGE_SECTION_HEADER *sec;
	DWORD size, align, off, wrote = 0;
	int i, holes = 0;
	char exe[MAX_PATH], path[MAX_PATH + 64], *leaf;
	HANDLE h;

	if (!base || !ss_readable((uintptr_t)base, sizeof(*dos)) ||
	    dos->e_magic != IMAGE_DOS_SIGNATURE) {
		ss_log("dump: the main module does not start with a DOS header, so "
		       "there is nothing here to write out\n");
		return 0;
	}
	nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
	if (!ss_readable((uintptr_t)nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) {
		ss_log("dump: no PE header at e_lfanew, refusing to guess\n");
		return 0;
	}
	size = nt->OptionalHeader.SizeOfImage;
	align = nt->OptionalHeader.SectionAlignment;
	buf = (unsigned char *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
					    PAGE_READWRITE);
	if (!buf) {
		ss_log("dump: could not reserve %lu bytes to copy the image into\n",
		       (unsigned long)size);
		return 0;
	}
	/* Page at a time, because a hole is expected rather than exceptional: an
	 * image has reserved-but-uncommitted tails and guard pages, and one
	 * unreadable page must cost that page rather than the whole dump. */
	for (off = 0; off < size; off += 0x1000) {
		DWORD n = size - off < 0x1000 ? size - off : 0x1000;

		if (ss_readable((uintptr_t)base + off, n))
			memcpy(buf + off, base + off, n);
		else
			holes++;
	}
	out = (IMAGE_NT_HEADERS *)(buf + dos->e_lfanew);
	out->OptionalHeader.ImageBase = (ULONG_PTR)base;
	out->OptionalHeader.FileAlignment = align;
	sec = IMAGE_FIRST_SECTION(out);
	for (i = 0; i < out->FileHeader.NumberOfSections; i++) {
		DWORD vsz = sec[i].Misc.VirtualSize;

		sec[i].PointerToRawData = sec[i].VirtualAddress;
		sec[i].SizeOfRawData = (vsz + align - 1) & ~(align - 1);
	}
	if (!GetModuleFileNameA(NULL, exe, sizeof(exe)))
		lstrcpynA(exe, "image", sizeof(exe));
	leaf = exe;
	for (i = 0; exe[i]; i++)
		if (exe[i] == '\\' || exe[i] == '/')
			leaf = exe + i + 1;
	wsprintfA(path, "%s.dump_%08lX.exe", leaf, (unsigned long)(ULONG_PTR)base);
	h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		ss_log("dump: could not create %s (error %lu)\n", path,
		       (unsigned long)GetLastError());
		VirtualFree(buf, 0, MEM_RELEASE);
		return 0;
	}
	WriteFile(h, buf, size, &wrote, NULL);
	CloseHandle(h);
	ss_log("dump: wrote %s - %lu bytes from base %08lX, %d page(s) unreadable and "
	       "left as zero. Load it in Ghidra as a PE; it is already based at "
	       "%08lX so the addresses in this log line up with it directly\n",
	       path, (unsigned long)wrote, (unsigned long)(ULONG_PTR)base, holes,
	       (unsigned long)(ULONG_PTR)base);

	wsprintfA(path, "%s.dump_%08lX.txt", leaf, (unsigned long)(ULONG_PTR)base);
	h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, NULL);
	if (h != INVALID_HANDLE_VALUE) {
		char line[512];
		HANDLE snap;
		const IMAGE_DATA_DIRECTORY *dd;

#define DUMP_PUT(s) WriteFile(h, (s), lstrlenA(s), &wrote, NULL)
		wsprintfA(line, "image %s based at %08lX, %lu bytes\r\n\r\n", leaf,
			  (unsigned long)(ULONG_PTR)base, (unsigned long)size);
		DUMP_PUT(line);
		DUMP_PUT("loaded modules - any raw pointer in the dump falls in one of "
			 "these\r\n");
		snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
		if (snap != INVALID_HANDLE_VALUE) {
			MODULEENTRY32 me;

			me.dwSize = sizeof(me);
			if (Module32First(snap, &me)) {
				do {
					wsprintfA(line, "  %08lX..%08lX  %s\r\n",
						  (unsigned long)(ULONG_PTR)me.modBaseAddr,
						  (unsigned long)((ULONG_PTR)me.modBaseAddr +
								  me.modBaseSize),
						  me.szModule);
					DUMP_PUT(line);
				} while (Module32Next(snap, &me));
			}
			CloseHandle(snap);
		}
		dd = &nt->OptionalHeader
			     .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		DUMP_PUT("\r\nimport table - IAT slot, what it resolved to, and the "
			 "name if the descriptors survived\r\n");
		if (dd->VirtualAddress && dd->Size) {
			const IMAGE_IMPORT_DESCRIPTOR *imp =
				(const IMAGE_IMPORT_DESCRIPTOR *)(base + dd->VirtualAddress);

			while (ss_readable((uintptr_t)imp, sizeof(*imp)) && imp->Name) {
				const char *dll = (const char *)(base + imp->Name);
				const ULONG_PTR *iat =
					(const ULONG_PTR *)(base + imp->FirstThunk);
				const ULONG_PTR *int_ =
					imp->OriginalFirstThunk
						? (const ULONG_PTR *)(base +
								      imp->OriginalFirstThunk)
						: NULL;
				int k;

				if (!ss_readable((uintptr_t)dll, 1))
					break;
				wsprintfA(line, "\r\n  from %s\r\n", dll);
				DUMP_PUT(line);
				for (k = 0; k < 4096; k++) {
					unsigned moff = 0;
					const char *in;
					char nm[128];

					if (!ss_readable((uintptr_t)(iat + k),
							 sizeof(*iat)) ||
					    !iat[k])
						break;
					nm[0] = 0;
					if (int_ &&
					    ss_readable((uintptr_t)(int_ + k),
							sizeof(*int_)) &&
					    int_[k]) {
						if (int_[k] & IMAGE_ORDINAL_FLAG)
							wsprintfA(nm, "ordinal %lu",
								  (unsigned long)(int_[k] &
										  0xFFFF));
						else if (ss_readable((uintptr_t)base +
									     int_[k] + 2,
								     1))
							lstrcpynA(nm,
								  (const char *)(base +
										 int_[k] +
										 2),
								  sizeof(nm));
					}
					in = ss_module((uintptr_t)iat[k], &moff);
					wsprintfA(line,
						  "    +%08lX  ->  %08lX  %s+%X  %s\r\n",
						  (unsigned long)((const unsigned char *)(iat +
											  k) -
								  base),
						  (unsigned long)iat[k],
						  in ? in : "unknown", moff, nm);
					DUMP_PUT(line);
				}
				imp++;
			}
		} else {
			DUMP_PUT("  the import directory is empty - the stub tore it down "
				 "after loading, so use the module list above to "
				 "attribute call targets by hand\r\n");
		}
#undef DUMP_PUT
		CloseHandle(h);
		ss_log("dump: wrote %s alongside it, naming the imports and every "
		       "loaded module\n",
		       path);
	}
	VirtualFree(buf, 0, MEM_RELEASE);
	return 1;
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

/* D3D9SW_DELTA: how much of a snapshot is actually new since the last one.
 *
 * Eight regions carry ninety percent of the 390 MB, and all eight look like
 * asset arenas that ought to sit still while the player walks around a room.
 * If that holds, a snapshot could store and write back only what moved, and
 * the value of doing so is not speed - a second is already acceptable - but
 * that a restore which writes ten megabytes has far fewer ways to clobber
 * something that should have stayed in the present than one which writes
 * three hundred and ninety.
 *
 * Ought to holds is not a foundation, so this measures it. Per-chunk hashes
 * of the previous save are kept and compared against the current ones, which
 * costs one extra read of the snapshot and no memory beyond our own BSS - the
 * copy window forbids allocating or taking a lock, and a diagnostic is not
 * worth breaking that rule for. */
#define SS_DELTA_MAX 65536u

typedef struct {
	uintptr_t at;
	unsigned hash;
} DeltaChunk;

static DeltaChunk g_dl_a[SS_DELTA_MAX], g_dl_b[SS_DELTA_MAX];
static DeltaChunk *g_dl_prev = g_dl_a, *g_dl_cur = g_dl_b;
static unsigned g_dl_nprev, g_dl_ncur;
static int g_dl_have;
/* Held out of the stack deliberately: one per region at the region cap is half
 * a megabyte, and a save runs on whichever thread pressed the key. A stack copy
 * of a structure this size is what crashed the slotfile work with C00000FD. */
static unsigned long long g_dl_moved[SS_MAX_REGIONS];

static int delta_mode(void)
{
	static int v = -1;
	if (v < 0) {
		char b[8];
		DWORD n = ss_getenv("D3D9SW_DELTA", b, sizeof(b));
		v = (n && b[0] == '1') ? 1 : 0;
	}
	return v;
}

/* Smaller chunks measure more finely and cost more to track. 64 KB is the
 * granularity a delta engine would plausibly use anyway, so measuring at any
 * other size would answer a question we are not asking. */
static unsigned delta_chunk(void)
{
	static unsigned v = 0;
	if (!v) {
		char b[16];
		DWORD n = ss_getenv("D3D9SW_DELTA_KB", b, sizeof(b));
		int kb = (n && n < sizeof(b)) ? atoi(b) : 64;
		if (kb < 4)
			kb = 4;
		v = (unsigned)kb * 1024u;
	}
	return v;
}

static unsigned delta_hash(const void *p, size_t n)
{
	const unsigned *w = (const unsigned *)p;
	const unsigned char *tail;
	unsigned h = 2166136261u;
	size_t i, nw = n / 4;

	for (i = 0; i < nw; i++) {
		h ^= w[i];
		h *= 16777619u;
	}
	tail = (const unsigned char *)p + nw * 4;
	for (i = nw * 4; i < n; i++) {
		h ^= *tail++;
		h *= 16777619u;
	}
	return h;
}

/* 1 the chunk is byte-identical, 0 it changed, -1 this address was not in the
 * previous save. The third case is its own answer: memory that came into
 * existence between the two saves is not a delta engine's problem, it is a
 * region-set change, and lumping it in with changed bytes would overstate how
 * much a delta would have to write. */
static int delta_seen(uintptr_t at, unsigned h)
{
	int lo = 0, hi = (int)g_dl_nprev - 1;

	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		if (g_dl_prev[mid].at == at)
			return g_dl_prev[mid].hash == h;
		if (g_dl_prev[mid].at < at)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

static void delta_scan(Slot *s)
{
	unsigned chunk = delta_chunk();
	unsigned long long same = 0, moved = 0, fresh = 0;
	unsigned long long *r_moved = g_dl_moved;
	DeltaChunk *swap;
	LARGE_INTEGER f, t0, t1;
	int i, listed = 0;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t0);
	g_dl_ncur = 0;

	for (i = 0; i < s->nregs; i++) {
		uintptr_t at = s->regs[i].base, end = at + s->regs[i].size;

		r_moved[i] = 0;
		for (; at < end; at += chunk) {
			size_t n = (size_t)((end - at < chunk) ? end - at : chunk);
			unsigned h = delta_hash((const void *)at, n);
			int was = g_dl_have ? delta_seen(at, h) : -1;

			if (was == 1) {
				same += n;
			} else if (was == 0) {
				moved += n;
				r_moved[i] += n;
			} else {
				fresh += n;
			}
			if (g_dl_ncur < SS_DELTA_MAX) {
				g_dl_cur[g_dl_ncur].at = at;
				g_dl_cur[g_dl_ncur].hash = h;
				g_dl_ncur++;
			}
		}
	}
	QueryPerformanceCounter(&t1);

	if (!g_dl_have) {
		ss_log("  delta: first save, %u chunk(s) of %u KB fingerprinted in %.0f ms "
		       "- the next save is the one that reports\n",
		       g_dl_ncur, chunk / 1024u,
		       1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart);
	} else {
		unsigned long long tot = same + moved + fresh;

		ss_log("  delta: %.1f MB in %u chunk(s) of %u KB - %.1f MB changed since the "
		       "previous save (%.1f%%), %.1f MB identical, %.1f MB not in it. "
		       "Fingerprinting took %.0f ms\n",
		       (double)tot / (1024.0 * 1024.0), g_dl_ncur, chunk / 1024u,
		       (double)moved / (1024.0 * 1024.0),
		       tot ? 100.0 * (double)moved / (double)tot : 0.0,
		       (double)same / (1024.0 * 1024.0), (double)fresh / (1024.0 * 1024.0),
		       1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart);

		/* Named per region, because the whole question is whether the churn
		 * is spread everywhere or concentrated in the small regions. A total
		 * cannot tell those apart and they call for different engines. */
		while (listed < 12) {
			int best = -1;

			for (i = 0; i < s->nregs; i++)
				if (r_moved[i] && (best < 0 || r_moved[i] > r_moved[best]))
					best = i;
			if (best < 0)
				break;
			ss_log("  delta:   %08lX  %8.2f MB held, %8.2f MB changed (%.1f%%)\n",
			       (unsigned long)s->regs[best].base,
			       (double)s->regs[best].size / (1024.0 * 1024.0),
			       (double)r_moved[best] / (1024.0 * 1024.0),
			       100.0 * (double)r_moved[best] / (double)s->regs[best].size);
			r_moved[best] = 0;
			listed++;
		}
	}

	swap = g_dl_prev;
	g_dl_prev = g_dl_cur;
	g_dl_cur = swap;
	g_dl_nprev = g_dl_ncur;
	g_dl_have = 1;
}

/* Fill the module name table at the very top of a save, before anything in the
 * opening steps can fault.
 *
 * build_exclusions rebuilds this properly once the threads are frozen, but it
 * does not run until well after dsh_quiet and the audio/gpu park, and those
 * steps run with every other thread still live. A background thread that faults
 * in that window hits ss_module with nmods==0, so the whole report comes out as
 * "unknown+0" and there is no way to tell ntdll from our own d3d11 from the
 * graphics stack. Priming the table here - a fresh snapshot, before the freeze,
 * so it is safe - means such a fault resolves to real names. The mod_rewound
 * flags are left zeroed; only the name and range matter for the report, and
 * build_exclusions overwrites them with the real verdict a moment later. */
static void ss_prime_module_table(void)
{
	HANDLE snap;
	MODULEENTRY32 me;
	HMODULE exe = GetModuleHandleA(NULL);
	char pv[8];

	if (!g_ctl)
		return;
	/* Opt-out for A/B testing. This walks the module list at the top of a save,
	 * before the freeze, purely so a fault's addresses resolve to names. It is
	 * the only thing added to the save path, so if a save is misbehaving it is
	 * the first suspect to rule out: D3D9SW_PRIMEMODS=0 skips it (faults then
	 * report unknown+0 again, but the save runs exactly as it did before). */
	if (ss_getenv("D3D9SW_PRIMEMODS", pv, sizeof(pv)) > 0 && pv[0] == '0')
		return;
	snap = modsnap_use();
	if (!snap || snap == INVALID_HANDLE_VALUE)
		return;
	find_steam_dir(snap);
	find_game_dir(snap, exe);
	me.dwSize = sizeof(me);
	g_ctl->nmods = 0;
	if (Module32First(snap, &me)) {
		do {
			if (g_ctl->nmods >= SS_MAX_MODS)
				break;
			mod_record(g_ctl->nmods++, &me, 0);
		} while (Module32Next(snap, &me));
	}
	CloseHandle(snap);
	sort_modules();
}

/* Who owns the memory a save captures, and who owns what it leaves behind.
 *
 * The cross-session question is not "how much travels" but "of the part that
 * does not come back, is it ours or the game's." Our renderer's regenerable
 * scratch is fine to lose; the game's audio state is not - and the two have been
 * indistinguishable inside a single coverage total, which is why a restore can
 * bring the room back while the music dies referencing buffers that no longer
 * exist. This buckets every committed writable region by owner and by whether
 * the snapshot took it, so the next cross-session restore's unrestorable set can
 * be read against a known composition.
 *
 * It also settles the 0x20000000 question directly. Two subsystems each log that
 * base as theirs - the gameheap pin (committed, read-write) and the renderer
 * arena (a 256 MB PAGE_NOACCESS reserve) - which cannot both hold it. So here we
 * ask the OS what is actually there while every thread is stopped: if both are
 * present, the window shows ~256 MB reserved-noaccess alongside the committed-RW
 * heap; if one lost the base, its bytes are simply not in the window. Reads only
 * - no allocation, no lock - so it is safe from inside the suspended save. */
enum { OWN_GH = 0, OWN_OURS_MOD, OWN_GAME_EXE, OWN_SYS_MOD, OWN_GAME_PRIV, OWN_N };

static int attrib_is_ours_mod(const char *n)
{
	static const char *const ours[] = {
		"d3d11.dll", "dxgi.dll", "xaudio2_9.dll", "xinput1_4.dll",
		"dsound.dll", "opengl32.dll", "d3d9.dll", "d3d9_sw.dll",
		"d3d9_sw_ref.dll"
	};
	unsigned i;

	for (i = 0; i < sizeof(ours) / sizeof(ours[0]); i++)
		if (!lstrcmpiA(n, ours[i]))
			return 1;
	return 0;
}

static void save_attribution(const Slot *s)
{
	char v[8];
	DWORD gn = ss_getenv("D3D9SW_ATTRIB", v, sizeof(v));
	uintptr_t gh_base[4], gh_size[4];
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t a;
	int ngh, i;

	if (gn && v[0] == '0')
		return; /* opt-out; on by default because it only reads and only at a save */

	ngh = gameheap_anchor_ranges(gh_base, gh_size, 4);

	/* --- base truth at the contested 0x20000000 --- */
	{
		uintptr_t used = 0, committed = 0, reserved = 0, noaccess = 0, rw = 0;
		uintptr_t first_alloc = 0;

		for (a = 0x20000000u; a < 0x40000000u &&
		     VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) == sizeof(mbi);) {
			uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;

			if (next <= a)
				break;
			a = next;
			if (mbi.State == MEM_FREE)
				continue;
			if (!first_alloc && (uintptr_t)mbi.BaseAddress == 0x20000000u)
				first_alloc = (uintptr_t)mbi.AllocationBase;
			used += mbi.RegionSize;
			if (mbi.State == MEM_COMMIT) {
				committed += mbi.RegionSize;
				if (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
						   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
					rw += mbi.RegionSize;
			} else if (mbi.State == MEM_RESERVE) {
				reserved += mbi.RegionSize;
			}
			if (mbi.Protect & PAGE_NOACCESS)
				noaccess += mbi.RegionSize;
		}
		ss_log("  base-truth: [20000000,40000000) alloc-base at 20000000 is %08lX; "
		       "%.0f MB used = %.0f committed (%.0f MB RW) + %.0f reserved; %.0f MB "
		       "PAGE_NOACCESS\n",
		       (unsigned long)first_alloc, (double)used / 1048576.0,
		       (double)committed / 1048576.0, (double)rw / 1048576.0,
		       (double)reserved / 1048576.0, (double)noaccess / 1048576.0);
		for (i = 0; i < ngh; i++)
			ss_log("  base-truth: gameheap anchor %d claims %08lX + %.0f MB "
			       "(committed-RW is the heap; the 256 MB NOACCESS reserve above "
			       "would be the renderer arena sharing the base)\n",
			       i, (unsigned long)gh_base[i], (double)gh_size[i] / 1048576.0);
	}

	/* --- who owns what the snapshot took, versus what it leaves in the present --- */
	{
		double cap_kb[OWN_N] = { 0 }, left_kb[OWN_N] = { 0 };
		static const char *const label[OWN_N] = {
			"gameheap (ours)", "our shim modules", "game exe",
			"system modules", "game/other private"
		};

		for (a = 0; VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) == sizeof(mbi);) {
			uintptr_t base = (uintptr_t)mbi.BaseAddress, next = base + mbi.RegionSize;
			int own = OWN_GAME_PRIV, captured = 0, j;
			double kb;

			if (next <= a)
				break;
			a = next;
			if (mbi.State != MEM_COMMIT)
				continue;
			if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
				continue;
			if (!(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
					     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
				continue;
			kb = (double)mbi.RegionSize / 1024.0;

			for (j = 0; j < ngh; j++)
				if (base >= gh_base[j] && base < gh_base[j] + gh_size[j]) {
					own = OWN_GH;
					break;
				}
			/* g_ctl's module census, not the slot's: the slot's copy is filled
			 * later in do_save (after this runs), so reading it here would put
			 * every module region in the private bucket - which is exactly the
			 * 0.0 rows the first run showed. */
			if (own == OWN_GAME_PRIV && g_ctl)
				for (j = 0; j < g_ctl->nmods; j++)
					if (base >= g_ctl->mod_lo[j] && base < g_ctl->mod_hi[j]) {
						/* By name AND outside the Windows directory:
						 * the system d3d11/dxgi/dsound share our names. */
						own = attrib_is_ours_mod(g_ctl->mod_name[j]) &&
						      !mod_is_system(j) ? OWN_OURS_MOD
						    : savestate_host_is(g_ctl->mod_name[j]) ? OWN_GAME_EXE
						    : OWN_SYS_MOD;
						break;
					}
			for (j = 0; j < s->nregs; j++)
				if (base >= s->regs[j].base &&
				    base < s->regs[j].base + s->regs[j].size) {
					captured = 1;
					break;
				}
			if (captured)
				cap_kb[own] += kb;
			else
				left_kb[own] += kb;
		}
		ss_log("  attribution (committed writable, MB captured / left in the present):\n");
		for (i = 0; i < OWN_N; i++)
			ss_log("      %-20s %8.1f / %8.1f\n",
			       label[i], cap_kb[i] / 1024.0, left_kb[i] / 1024.0);
		ss_log("  attribution: \"left in the present\" for our own rows is regenerable "
		       "scratch; for the game rows it is state a cross-session restore drops - "
		       "the audio among it is why the music goes stale\n");
	}
}

/* Free-address-space histogram (D3D9SW_FREEMAP=1). Walks the whole user address
 * space and reports the MEM_FREE regions: a size histogram, the total, and the
 * largest few holes WITH their base addresses. Run across several sessions, the
 * bases of the big holes reveal which are DETERMINISTIC (same address every
 * launch) - the proof of where a stable aperture can be planted, and how much
 * room is left for a resident window without starving the game. Read-only; walks
 * inside the save's quiet window like the census. 2 GB space, so every size fits
 * unsigned long as KB/MB. */
static void freemap_report(void)
{
	char v[8];
	DWORD got = ss_getenv("D3D9SW_FREEMAP", v, sizeof(v));
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t addr = 0;
	unsigned long total_mb = 0, hist[6];
	int nfree = 0, ntop = 0, i;
	struct { uintptr_t base; SIZE_T size; } top[8];

	if (!(got > 0 && got < sizeof(v) && v[0] == '1'))
		return;
	memset(hist, 0, sizeof(hist));
	memset(top, 0, sizeof(top));
	while (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
		uintptr_t base = (uintptr_t)mbi.BaseAddress;
		uintptr_t next = base + mbi.RegionSize;

		if (next <= addr)
			break;
		addr = next;
		if (mbi.State != MEM_FREE)
			continue;
		{
			SIZE_T sz = mbi.RegionSize;

			nfree++;
			total_mb += (unsigned long)(sz >> 20);
			if (sz < 65536ul) hist[0]++;
			else if (sz < 1048576ul) hist[1]++;
			else if (sz < 16ul * 1048576) hist[2]++;
			else if (sz < 64ul * 1048576) hist[3]++;
			else if (sz < 256ul * 1048576) hist[4]++;
			else hist[5]++;
			/* Keep the 8 largest, sorted descending, by insertion. */
			if (ntop < 8 || sz > top[7].size) {
				int j = ntop < 8 ? ntop++ : 7;

				top[j].base = base;
				top[j].size = sz;
				for (; j > 0 && top[j].size > top[j - 1].size; j--) {
					uintptr_t tb = top[j].base;
					SIZE_T ts = top[j].size;

					top[j].base = top[j - 1].base;
					top[j].size = top[j - 1].size;
					top[j - 1].base = tb;
					top[j - 1].size = ts;
				}
			}
		}
	}
	ss_log("freemap: %d free region(s), %lu MB total; hist <64K:%lu <1M:%lu "
	       "<16M:%lu <64M:%lu <256M:%lu >=256M:%lu\n",
	       nfree, total_mb, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5]);
	ss_log("freemap: largest holes (base, size) - compare bases across sessions "
	       "for a deterministic aperture site:\n");
	for (i = 0; i < ntop; i++)
		ss_log("    %08lX  %lu MB\n", (unsigned long)top[i].base,
		       (unsigned long)(top[i].size >> 20));
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
	DWORD build;
	int i, rc = 0;
	/* Phase attribution, so a save that gets slower is answered by reading a
	 * line rather than by experiment. Added while chasing an 18-second save that
	 * turned out not to exist - see request(), where a restored thread reports
	 * save-to-restore wall time as if it were a save duration. */
	LARGE_INTEGER pf, t0, t_susp, t_excl, t_rel, t_walk, t_copy;
	/* The phases above start at suspend and stop at the copy, which left most of
	 * this function unmeasured - and that is where a five-second save turned out
	 * to be hiding while every phase read faster than it ever had. */
	LARGE_INTEGER t_begin;

	QueryPerformanceFrequency(&pf);
	QueryPerformanceCounter(&t_begin);
	ss_op_set("save");

	/* Before anything else in the save can fault, so the report is legible if it
	 * does. build_exclusions rebuilds this after the freeze; this is only for the
	 * window before it. */
	ss_prime_module_table();

	/* Here rather than at hooks install, where the arena cannot be taken yet:
	 * blk_arena registers an exclusion, and there is no control block to
	 * register it in until the first save. This is still early enough, because
	 * what matters is only that the pointer be non-null before the .data holding
	 * it is captured a few hundred lines below. */
	heap_seen_init();
	coverage_init();
	if (!g_freed)
		g_freed = (FreedSet *)blk_arena(sizeof(FreedSet));
	freeze_arm_once();
	/* Reported from here because this is a log point that demonstrably works.
	 *
	 * Three builds in a row produced no decoder line, and the reason was never
	 * the patch: it was that nothing logged from the per-frame guard has ever
	 * reached this file - dsh_install's message has been absent from all 38
	 * sessions while dsh_save's, written by the same function, appears. So the
	 * question "did the patch match" was being asked in a place that could not
	 * answer. Saying it at save time costs one line and cannot be swallowed. */
	ss_raw("decoder: guard ran %u time(s) before this save, patch %s\n", g_dec_tries,
	       g_decpatch > 0	 ? "APPLIED"
	       : g_decpatch < 0	 ? "given up on"
				 : "still trying");
	dsh_save();
	/* Beside dsh_save, because these answer the same question for whichever
	 * audio stand-in is serving. They were in savestate_object_report, which
	 * this config gates off, so a whole working session produced no census at
	 * all - and the census is the one piece of evidence the run was for. */
	ds_sw_report();
	xa2_sw_report();
	gameheap_report();
	/* Cleared before the census refills it, so option 3's asset-heap list reflects
	 * only this save. */
	asset_heaps_reset();
	/* Census every process heap while all threads are still live, so HeapLock can
	 * serialise the walk (a suspended lock-holder would hang it). This is the
	 * measurement that sizes the "own the allocator boundary" plan. Read-only,
	 * gated D3D9SW_HEAPCENSUS. */
	gameheap_heap_census();
	/* The free-space histogram beside the census: what the game and our own
	 * reservations leave unclaimed, and where the big holes sit. Run across
	 * sessions it says which holes are deterministic enough to plant an aperture
	 * in. Gated D3D9SW_FREEMAP. */
	freemap_report();
	/* Before suspend_all, because the point is to have nothing playing for the
	 * whole window rather than merely for the copy. */
	dsh_quiet();
	ss_phase("save: after dsh_quiet%s\n", ss_under_wine() ? " (wine/proton)" : "");
	park_audio_gpu();
	ss_phase("save: after audio/gpu park\n");
	QueryPerformanceFrequency(&pf);
	QueryPerformanceCounter(&t0);
	g_blk_save_n = 0;
	/* Before collect_threads, because everything past this point runs with the
	 * process held still and this reaches for the loader lock. */
	if (alloc_settle())
		alloc_ranges_init();
	collect_threads();
	ss_phase("save: collected %d thread(s), suspending\n", g_ctl->nids);
	suspend_all();
	ss_phase("save: suspend_all returned\n");
	/* Let go and look again while anything is inside the JIT. Threads must be
	 * suspended to read their contexts, so each attempt is a full suspend, and
	 * the release has to be real - a thread cannot leave the JIT while held.
	 *
	 * Skipped under Wine: the loop resume+resuspends every thread, including
	 * the wineserver waiters that suspend_all just left running on purpose. */
	if (!ss_under_wine()) {
		int tries = settle_tries(), n = 0;
		unsigned who = 0, awho = 0;
		uintptr_t where = 0, arva = 0;
		int jit, alloc;

		for (;;) {
			jit = in_the_jit(&who, &where);
			alloc = alloc_settle() && in_the_allocator(&awho, &arva);
			if ((!jit && !alloc) || n >= tries)
				break;
			resume_all(0);
			Sleep(2);
			collect_threads();
			suspend_all();
			n++;
		}
		if (n && !jit && !alloc)
			ss_log("  settled after %d attempt(s): nobody inside the JIT or "
			       "running ntdll\n", n);
		if (jit)
			ss_log("  WARNING: still inside the JIT after %d attempt(s) (thread %u at "
			       "%p); saving anyway, and this snapshot may hold a half-updated "
			       "JIT-info table\n",
			       n, who, (void *)where);
		if (alloc)
			ss_log("  WARNING: thread %u is still running ntdll at +%lX after %d "
			       "attempt(s); saving anyway, and if it is inside the heap its "
			       "stack will resume against block contents from the past\n",
			       awho, (unsigned long)arva, n);
	}
	/* Heap locks are taken after settling rather than before it, because a
	 * thread that needs to allocate its way out of the JIT cannot do that
	 * against locks we are holding, and the settle loop would then spend all
	 * 24 attempts waiting for something it had itself made impossible. The
	 * cost is one extra release-and-refreeze.
	 *
	 * Wine's ntdll heap is not laid out with Windows segment headers, so
	 * HeapWalk / the LFH walker either finds nothing or never returns. The
	 * restore then has to put back whole regions anyway. */
	if (!ss_under_wine() && blk_mode() && blk_alloc()) {
		int capped = 0;

		resume_all(0);
		blk_lock_all();
		collect_threads();
		suspend_all();
		/* Settle again, because the release just above reopened the window the
		 * first loop closed. Fewer attempts than the first pass: the heap locks
		 * are held now, so a thread that wants the classic allocator will park
		 * on the lock - which reads as a system call stub and therefore as
		 * settled - and the only thing still able to make progress is the LFH
		 * fast path, which is what we are waiting for anyway. */
		if (alloc_settle()) {
			unsigned awho = 0;
			uintptr_t arva = 0;
			int n = 0;

			while (n < 8 && in_the_allocator(&awho, &arva)) {
				resume_all(0);
				Sleep(1);
				collect_threads();
				suspend_all();
				n++;
			}
			if (in_the_allocator(&awho, &arva))
				ss_log("  WARNING: thread %u is running ntdll at +%lX with the heap "
				       "locks held; the block map is being walked out from under "
				       "it\n", awho, (unsigned long)arva);
			else if (n)
				ss_log("  settled again after %d attempt(s) once the heaps were "
				       "locked\n", n);
		}
		g_blk_save_n = blk_walk_locked(g_blk_save, SS_BLK_CAP, &capped);
		blk_sort(g_blk_save, (int)g_blk_save_n);
		if (capped)
			ss_log("  heap blocks: hit the %u block ceiling at save - the "
			       "map is incomplete and fewer blocks will come back\n",
			       SS_BLK_CAP);
		ss_log("  heap blocks: mapped %u busy block(s) across %u locked heap(s)\n",
		       g_blk_save_n, g_blk_locked_n);
	} else {
		unsigned nbusy = 0;

		/* Said out loud because the quiet version of this cost three runs.
		 * With block mode off the restore puts back whole regions, which is a
		 * different experiment from the one the config describes, and the only
		 * evidence was a zero in the middle of the load line.
		 *
		 * Wine has no HEAP_SEGMENT, so HeapWalk is not used. The redirected
		 * game heap still has a live set: we intercepted every malloc. The
		 * snapshot is taken here, after suspend_all, so it describes the
		 * same instant the page copy is about to capture. */
		if (ss_under_wine() && blk_mode())
			nbusy = gameheap_busy_snapshot();
		ss_log("  heap blocks: OFF for this save - D3D9SW_HEAPBLOCKS reads as %d, "
		       "so Windows HeapWalk is not used%s\n",
		       blk_mode(),
		       ss_under_wine()
			       ? " (Wine heap is not Windows HEAP; redirected game "
				 "heap restores from the busy-block sidetable, not "
				 "HeapWalk)"
			       : "");
		if (ss_under_wine() && blk_mode())
			ss_log("  wine gameheap: sidetable snapshot %u busy block(s) "
			       "(no HeapWalk)\n",
			       nbusy);
	}
	QueryPerformanceCounter(&t_susp);
	/* Its cache lives in our rewound data, and filled in for the first time
	 * mid-copy it reads as a torn snapshot. */
	(void)delta_mode();
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

	g_dev_regions = 0;
	g_dev_bytes = 0;
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

	/* Ownership breakdown of what was just chosen versus left behind, and the
	 * direct read of who actually holds 0x20000000. Still inside the suspended
	 * window, reads only. */
	save_attribution(s);

	/* Ceiling above 2 GB means the large-address-aware flag took effect. The
	 * largest free block is what any future allocation arena has to fit in. */
	if (g_dev_regions)
		ss_log("  device memory: %u region(s), %.1f MB, passed over - write-combined "
		       "or non-cached, so they are the adapter's and not ours to snapshot\n",
		       g_dev_regions, (double)g_dev_bytes / (1024.0 * 1024.0));
	ss_log("  address space: top %p, %.0f MB used, %.0f MB free, largest free block "
	       "%.0f MB\n",
	       (void *)top, (double)used_total / (1024.0 * 1024.0),
	       (double)free_total / (1024.0 * 1024.0),
	       (double)free_largest / (1024.0 * 1024.0));

	/* D3D9SW_SAVE_NOCOPY: everything a save does except the one thing that
	 * touches the game's pages. Threads are collected and suspended, the
	 * exclusions are built, the walk runs and the regions are chosen - and
	 * then it resumes without reading any of them. If the process survives
	 * this and dies with the knob off, the copy is the killer and nothing
	 * before it is; if it dies either way, the copy is innocent. One run
	 * answers a question that would otherwise take several. */
		{
		char nc[8];
		DWORD ncn = ss_getenv("D3D9SW_SAVE_NOCOPY", nc, sizeof(nc));

		if (ncn && nc[0] == '1') {
		ss_log("  save: NOCOPY - %d region(s), %.1f MB chosen and deliberately "
		       "NOT read. This proves only whether the copy is what kills the "
		       "process; no snapshot is kept.\n",
		       s->nregs, (double)total / (1024.0 * 1024.0));
			slot_release(s);
			rc = 0;
			goto done;
		}
	}
	QueryPerformanceCounter(&t_walk);
	s->sect = (slotfile_mode() && slotno != SS_SCRATCH) ? slotfile_section(slotno, total)
							     : NULL;
	if (!s->sect)
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
	/* After the copy, so a fault while hashing cannot cost the snapshot, and
	 * still inside the suspended window so the bytes hashed are the bytes
	 * saved rather than whatever the game reached next. */
	if (delta_mode())
		delta_scan(s);
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

		ss_log("  save cost ms: prologue %.1f, suspend %.1f, exclusions %.1f, "
		       "release %.1f, walk %.1f, section+copy %.1f\n",
		       (double)(t0.QuadPart - t_begin.QuadPart) / q,
		       (double)(t_susp.QuadPart - t0.QuadPart) / q,
		       (double)(t_excl.QuadPart - t_susp.QuadPart) / q,
		       (double)(t_rel.QuadPart - t_excl.QuadPart) / q,
		       (double)(t_walk.QuadPart - t_rel.QuadPart) / q,
		       (double)(t_copy.QuadPart - t_walk.QuadPart) / q);
	}

	build = ss_build_id();
	for (i = 0; i < g_ctl->nids; i++) {
		ThreadState *t;
		if (!g_ctl->handles[i] || s->nthreads >= SS_MAX_THREADS)
			continue;
		if (g_ctl->transient[i] || !g_did_suspend[i])
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
		{
			/* The stack range, for a cross-session restore to relocate this
			 * thread's saved SP and stack pointers when it adopts the thread. */
			unsigned char *teb = (unsigned char *)teb_of(g_ctl->handles[i]);

			t->stack_base = teb ? *(const uintptr_t *)(teb + TIB_STACK_BASE) : 0;
			t->stack_limit = teb ? *(const uintptr_t *)(teb + TIB_STACK_LIMIT) : 0;
			t->seh_head = teb ? *(const DWORD *)teb : 0;
			t->xs_mark = teb ? XS_MARK : 0;
			t->build_id = build;
			t->teb = (uintptr_t)teb;
		}
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

	/* Copied from the census build_exclusions just took, so the save's view of
	 * the module set is the same one its exclusions were computed from. */
	s->nmods = g_ctl->nmods < SS_MAX_MODS ? g_ctl->nmods : SS_MAX_MODS;
	for (i = 0; i < s->nmods; i++) {
		s->mod_lo[i] = g_ctl->mod_lo[i];
		s->mod_hi[i] = g_ctl->mod_hi[i];
		memcpy(s->mod_name[i], g_ctl->mod_name[i], sizeof(s->mod_name[i]));
		memcpy(s->mod_path[i], g_ctl->mod_path[i], sizeof(s->mod_path[i]));
		s->mod_stamp[i] = g_ctl->mod_stamp[i];
		s->mod_isize[i] = g_ctl->mod_isize[i];
		s->mod_cookie[i] = mod_gs_cookie(s->mod_lo[i]);
	}
	s->fmt = SS_SLOT_FMT;
	proc_identity(&s->save_pid, &s->save_created, &s->save_boot);
	s->save_peb = peb_base();
	s->save_ctl = (uintptr_t)g_ctl;
	s->save_ptr_cookie = ptr_cookie();
	s->nwnd = wnd_list(s->wnd, s->wnd_cls, SS_MAX_WND);
	for (i = 0; i < s->nwnd; i++)
		s->wnd_dc[i] = wnd_own_dc(s->wnd[i]);
	/* The non-module owned-heap anchors (arena, LAA region). With the modules
	 * above, this is the base set a cross-session restore relocates against. */
	s->nanchor = gameheap_anchor_ranges(s->anchor_base, s->anchor_size,
					    (int)(sizeof(s->anchor_base) /
						  sizeof(s->anchor_base[0])));
	for (i = 0; i < s->nanchor; i++)
		ss_log("  anchor: owned heap %d at %08lX, %lu KB - recorded for a "
		       "relocatable restore\n",
		       i, (unsigned long)s->anchor_base[i],
		       (unsigned long)(s->anchor_size[i] >> 10));
	{
		/* The per-thread stack anchors, recorded above in the thread loop. The
		 * requesting thread is the one a cross-session restore would adopt, so
		 * name its stack for visibility. */
		int j, with_stack = 0;
		uintptr_t rb = 0, rl = 0;

		for (j = 0; j < s->nthreads; j++) {
			if (s->threads[j].stack_base)
				with_stack++;
			if (s->threads[j].tid == g_ctl->req_tid) {
				rb = s->threads[j].stack_base;
				rl = s->threads[j].stack_limit;
			}
		}
		ss_log("  anchor: %d of %d thread stack(s) recorded; requesting thread %u "
		       "stack %08lX..%08lX - the thread a cross-session restore adopts\n",
		       with_stack, s->nthreads, (unsigned)g_ctl->req_tid,
		       (unsigned long)rl, (unsigned long)rb);
	}
	time_now(&s->clock);
	s->nevents = events_capture(s->events, SS_MAX_EVENTS);
	s->nfiles = for_each_file(s->files, SS_MAX_FILES);
	s->nstreams = gameheap_streams(s->stream_f, s->stream_path, s->stream_mode, s->stream_pos,
				       SS_MAX_STREAMS);
	for (i = 0; i < s->nstreams; i++)
		ss_file_stamp(s->stream_path[i], &s->stream_size[i], &s->stream_mtime[i]);
	s->bytes = total;
	s->cs_seq = g_cs ? g_cs->seq : 0;
	s->gh_seq = gameheap_res_seq();
	s->valid = 1;
	g_ctl->last_mb = (double)total / (1024.0 * 1024.0);
	ss_log("save: slot %d, %d regions, %.1f MB, %d threads, %d context(s), %d file(s), "
	       "%d stream(s)\n",
	       slotno, s->nregs, g_ctl->last_mb, g_ctl->nids, s->nthreads, s->nfiles,
	       s->nstreams);
	for (i = 0; i < s->nstreams; i++) {
		char nm[MAX_PATH];

		if (!WideCharToMultiByte(CP_ACP, 0, s->stream_path[i], -1, nm, sizeof(nm), NULL,
					 NULL))
			nm[0] = 0;
		ss_log("  stream %08lX \"%s\" at %llu: %s\n", (unsigned long)s->stream_f[i],
		       s->stream_mode[i], (unsigned long long)s->stream_pos[i], nm);
	}
	gameheap_res_saved();
	witness_save(s);
	cap_record(s);
	watch_at_report("at the save", 0);
	ents_report("at the save");
	ents_snap();
	watch_trace_arm("save");
	carry_save();
	/* Last, because everything above fills fields the description has to carry. */
	if (slotfile_mode() && slotno != SS_SCRATCH) {
		bnd_sample(s);
		slotfile_write_meta(slotno, s);
	}
	rc = 1;

done:
	/* Timed individually, because between them these account for whatever the
	 * phases above do not, and three of them walk the whole process. Reporting
	 * a lump sum here would only move the mystery rather than answer it. */
	{
		LARGE_INTEGER e0, e1, e2, e3, e4, e5;
		double q = (double)pf.QuadPart / 1000.0;

		QueryPerformanceCounter(&e0);
		resume_all(0);
		dsh_play();
		xa2_sw_resume();
		QueryPerformanceCounter(&e1);
		/* After the first save rather than at install, because half of what it
		 * reports - the heap partition, the module tenancy, the exclusion
		 * count - does not exist until a snapshot has been built. */
		ss_inventory(s);
		QueryPerformanceCounter(&e2);
		/* Last thing before the save returns, so the fingerprints describe the
		 * process as the snapshot leaves it rather than as it was mid-copy. */
		coverage_take();
		QueryPerformanceCounter(&e3);
		/* Armed as the save finishes, so the window it records is exactly the
		 * window between this snapshot and whatever restores it. */
		freed_arm();
		QueryPerformanceCounter(&e4);
		heap_check("before the save");
		QueryPerformanceCounter(&e5);
		ss_log("  save cost ms: resume %.1f, inventory %.1f, coverage %.1f, "
		       "freed_arm %.1f, heap_check %.1f | whole save %.1f\n",
		       (double)(e1.QuadPart - e0.QuadPart) / q,
		       (double)(e2.QuadPart - e1.QuadPart) / q,
		       (double)(e3.QuadPart - e2.QuadPart) / q,
		       (double)(e4.QuadPart - e3.QuadPart) / q,
		       (double)(e5.QuadPart - e4.QuadPart) / q,
		       (double)(e5.QuadPart - t_begin.QuadPart) / q);
	}
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

/* What the surviving threads overwrite once they are let go.
 *
 * Both existing verify passes run inside the freeze: one checks that the
 * snapshot held still while it was read, the other that the write-back landed.
 * Between them they prove the copy is correct, and the copy has never been the
 * problem. What has never been measured is the first few frames after
 * resume_all, when 41 threads that were not rewound start working against
 * memory that was - and put their own idea of the world back over ours.
 *
 * That is the difference between a restore that fails and a restore that is
 * undone, and from the outside the two look identical: the frame comes back and
 * then the game carries on from where it was. This names which regions get
 * taken back, so the argument stops being about which memory ought to matter.
 *
 * Set D3D9SW_CLOBBER to the number of milliseconds to wait before looking.
 * Small values catch the fastest writer, larger ones catch more of them. */
static int g_clob_slot = -1;
static long long g_clob_due;
static char *g_clob_ok SS_PRESENT;
static unsigned long long *g_clob_off SS_PRESENT, *g_clob_pre SS_PRESENT;

/* Eight bytes out of the snapshot at a given offset, for quoting alongside what
 * memory holds now. */
static int win_u64(Window *w, unsigned long long pos, unsigned long long *out)
{
	if (pos + sizeof(*out) > w->total || !win_cover(w, pos))
		return 0;
	if (pos + sizeof(*out) > w->off + w->size)
		return 0;
	memcpy(out, w->base + (size_t)(pos - w->off), sizeof(*out));
	return 1;
}

/* Whatever reverted on the last restore, remembered so the next one can be told
 * to leave it alone without anyone reading an address out of a log.
 *
 * Region bases move every launch, so naming one by hand means taking a
 * baseline, reading the hex, editing the config and restoring again, all inside
 * one session, because relaunching invalidates the number. D3D9SW_SKIPREG=auto
 * closes that loop: restore once to learn what reverts, restore again to learn
 * whether holding it back changes anything. */
#define SS_POKE_CAP 16
static uintptr_t g_revert_at[SS_POKE_CAP];
static int g_revert_n, g_skip_auto;

/* Derived state: the shelf rather than the stock.
 *
 * A restore puts every captured word back, which quietly assumes every word is
 * a fact. Most are. Some are a cached opinion about other facts - free-list
 * heads, pool counts, handle-table sentinels, ring cursors - and the game
 * recomputes those from the things they describe. Putting one back is worse
 * than leaving it alone, because the value we paint in disagrees with the state
 * it claims to summarise and the game acts on the summary. DxLib's SubHandle
 * zeroing a pool sentinel is this exactly: the shelf reads empty, so the game
 * makes more stock on top of the stock that is already there.
 *
 * The two kinds are separable by observation rather than by analysis, which
 * matters because analysis has now failed at it three times - reachability, the
 * ntdll veto and content sniffing are all attempts to decide ownership of a
 * word by looking at it. Three values name a word instead: what it held before
 * the restore, what we wrote, and what it holds a few hundred ms later. A word
 * that has gone back to its pre-restore value in spite of what we wrote was
 * recomputed by the game from state it trusts more than ours. That is derived,
 * and the next restore leaves it in the present.
 *
 * This buys a pre-image, so it is budgeted. A 32-bit process already carrying
 * 1.5 GB cannot hold a second copy of 279 MB, and it does not need to: the
 * regions worth watching are the ones that reverted last time, so those are
 * captured first and the rest fill whatever budget is left.
 *
 * The mask only ever grows within a session. A word held back keeps its live
 * value, so it trivially still looks reverted on the next check and stays in
 * the set - self-sustaining by design, since a shelf does not stop being a
 * shelf. The dedupe after each learn is what stops that from growing without
 * bound. */
#define SS_DER_CAP (1u << 18) /* 262144 words, so up to 2 MB of held-back state */

static int poke_hit(const uintptr_t *list, int n, uintptr_t base);

static unsigned char *g_der_pre SS_PRESENT; /* live bytes, taken before the restore paints */
static size_t g_der_cap, g_der_used;
static unsigned long long *g_der_at SS_PRESENT, *g_der_len SS_PRESENT;
static unsigned char *g_der_plan SS_PRESENT;
static uintptr_t *g_der_mask SS_PRESENT; /* kept sorted, so a region's words are one run */
static unsigned g_der_n;
static int g_der_regs, g_der_held, g_der_new, g_der_covered;

/* Megabytes of pre-image to spend. 0 is off, which is the default: this holds a
 * second copy of whatever it watches, and the address space it comes out of is
 * the same one the game is already close to exhausting. */
static int der_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[16];
		DWORD n = ss_getenv("D3D9SW_DERIVED", v, sizeof(v));

		/* A failed read is not remembered. The first call lands inside the
		 * restore, with every thread frozen and the environment lookup
		 * deliberately answering nothing, so caching the zero it returns
		 * there pins the feature off for the life of the process no matter
		 * what the config says. */
		if (!n || n >= sizeof(v))
			return 0;
		cached = atoi(v);
		if (cached < 0)
			cached = 0;
		if (cached > 192)
			cached = 192;
	}
	return cached;
}

static int der_alloc(int nregs)
{
	if (!der_mode())
		return 0;
	if (!g_der_pre) {
		g_der_cap = (size_t)der_mode() << 20;
		g_der_pre = (unsigned char *)blk_arena(g_der_cap);
		if (!g_der_pre) {
			g_der_cap = 0;
			return 0;
		}
	}
	if (!g_der_mask) {
		g_der_mask = (uintptr_t *)blk_arena(SS_DER_CAP * sizeof(*g_der_mask));
		if (!g_der_mask)
			return 0;
	}
	if (g_der_regs < nregs) {
		g_der_at = (unsigned long long *)blk_arena((size_t)nregs * sizeof(*g_der_at));
		g_der_len = (unsigned long long *)blk_arena((size_t)nregs * sizeof(*g_der_len));
		g_der_plan = (unsigned char *)blk_arena((size_t)nregs);
		if (!g_der_at || !g_der_len || !g_der_plan) {
			g_der_regs = 0;
			return 0;
		}
		g_der_regs = nregs;
	}
	return 1;
}

/* Which regions get a pre-image, decided before the write loop so the budget
 * can go to the regions that earned it rather than to whichever happen to come
 * first in the slot. */
static void der_plan_regions(Slot *s)
{
	size_t left = g_der_cap;
	int i, pass;

	g_der_used = 0;
	g_der_held = 0;
	g_der_new = 0;
	g_der_covered = 0;
	for (i = 0; i < s->nregs; i++) {
		g_der_at[i] = ~0ull;
		g_der_plan[i] = 0;
	}
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < s->nregs; i++) {
			size_t size = (size_t)s->regs[i].size;

			if (g_der_plan[i] || size > left)
				continue;
			if (pass == 0 && !poke_hit(g_revert_at, g_revert_n, s->regs[i].base))
				continue;
			g_der_plan[i] = 1;
			left -= size;
		}
}

static void der_capture(int i, const void *base, size_t size)
{
	if (!g_der_pre || i >= g_der_regs || !g_der_plan[i])
		return;
	if (size > g_der_cap - g_der_used)
		return;
	g_der_at[i] = g_der_used;
	g_der_len[i] = size;
	memcpy(g_der_pre + g_der_used, base, size);
	g_der_used += size;
	g_der_covered++;
}

/* Put the derived words back to what the game had, immediately after the region
 * was painted. Cheaper than it looks: the mask is sorted, so this is a binary
 * search and a short walk rather than a lookup per word. */
static void der_apply(int i, unsigned char *base, size_t size)
{
	uintptr_t b = (uintptr_t)base, e = b + size;
	unsigned lo = 0, hi = g_der_n, k;

	if (!g_der_n || i >= g_der_regs || g_der_at[i] == ~0ull)
		return;
	while (lo < hi) {
		unsigned m = lo + (hi - lo) / 2;

		if (g_der_mask[m] < b)
			lo = m + 1;
		else
			hi = m;
	}
	for (k = lo; k < g_der_n && g_der_mask[k] < e; k++) {
		size_t o = (size_t)(g_der_mask[k] - b);

		if (o + sizeof(unsigned long long) > size)
			continue;
		memcpy(base + o, g_der_pre + (size_t)g_der_at[i] + o,
		       sizeof(unsigned long long));
		g_der_held++;
	}
}

static void der_sort(uintptr_t *a, unsigned n)
{
	unsigned gap;

	/* Shell sort: no stdlib here, and the array arrives as a handful of
	 * already-sorted runs, which is the case this handles well. */
	for (gap = n / 2; gap > 0; gap /= 2) {
		unsigned i;

		for (i = gap; i < n; i++) {
			uintptr_t v = a[i];
			unsigned j = i;

			while (j >= gap && a[j - gap] > v) {
				a[j] = a[j - gap];
				j -= gap;
			}
			a[j] = v;
		}
	}
}

/* The three-way read that names a word. Run once per restore, a few hundred ms
 * after it, from the same check that reports what came back. */
static void der_learn(Slot *s, Window *w)
{
	unsigned long long pos = 0;
	unsigned before = g_der_n, keep, i;
	int r;

	if (!g_der_pre || !g_der_mask)
		return;
	for (r = 0; r < s->nregs; r++) {
		unsigned long long here = pos;
		size_t size = (size_t)s->regs[r].size, done = 0;
		const unsigned char *pre, *mem = (const unsigned char *)s->regs[r].base;

		pos += s->regs[r].size;
		if (r >= g_der_regs || g_der_at[r] == ~0ull)
			continue;
		if (!g_clob_ok || !g_clob_ok[r])
			continue;
		{
			MEMORY_BASIC_INFORMATION mbi;

			if (VirtualQuery((LPCVOID)s->regs[r].base, &mbi, sizeof(mbi)) !=
				    sizeof(mbi) ||
			    mbi.State != MEM_COMMIT ||
			    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
			    (uintptr_t)mbi.BaseAddress + mbi.RegionSize <
				    s->regs[r].base + s->regs[r].size)
				continue;
		}
		pre = g_der_pre + (size_t)g_der_at[r];
		while (done < size) {
			const unsigned char *view;
			size_t chunk, k;

			if (!win_cover(w, here + done))
				break;
			view = w->base + (size_t)(here + done - w->off);
			chunk = (size_t)(w->off + w->size - (here + done));
			if (chunk > size - done)
				chunk = size - done;
			for (k = 0; k + sizeof(unsigned long long) <= chunk;
			     k += sizeof(unsigned long long)) {
				unsigned long long cur, pv, sv;

				memcpy(&cur, mem + done + k, sizeof(cur));
				memcpy(&pv, pre + done + k, sizeof(pv));
				memcpy(&sv, view + k, sizeof(sv));
				if (cur != pv || pv == sv)
					continue; /* a fact, or one we never changed */
				if (g_der_n < SS_DER_CAP)
					g_der_mask[g_der_n++] =
						(uintptr_t)(s->regs[r].base + done + k);
			}
			done += chunk;
		}
	}
	if (!g_der_n)
		return;
	der_sort(g_der_mask, g_der_n);
	/* Held-back words re-qualify every time by construction, so without this
	 * the mask would grow by its own size on each restore. */
	for (i = 1, keep = 1; i < g_der_n; i++)
		if (g_der_mask[i] != g_der_mask[i - 1])
			g_der_mask[keep++] = g_der_mask[i];
	g_der_n = keep;
	g_der_new = (int)g_der_n - (int)before;

	/* Where the shelf turned out to be.
	 *
	 * This is the question that decides whether the idea is safe at all. A
	 * mask sitting in the game's own image and statics is masking the game's
	 * summaries of its own state, which is the whole point. A mask sitting in
	 * heap regions is masking the allocator's bookkeeping, and holding one
	 * live pointer back inside an otherwise rewound free list is how a heap
	 * fails validation. The two are indistinguishable in the totals and
	 * obvious the moment they are attributed. */
	{
		int r, shown = 0;

		for (r = 0; r < s->nregs && shown < 10; r++) {
			uintptr_t b = s->regs[r].base, e = b + s->regs[r].size;
			unsigned lo = 0, hi = g_der_n, m, c = 0;
			unsigned moff;
			const char *mod;

			while (lo < hi) {
				m = lo + (hi - lo) / 2;
				if (g_der_mask[m] < b)
					lo = m + 1;
				else
					hi = m;
			}
			for (m = lo; m < g_der_n && g_der_mask[m] < e; m++)
				c++;
			if (!c)
				continue;
			mod = ss_module(b, &moff);
			ss_log("    derived: region %d at %p holds %u masked word(s), %s\n",
			       r, (void *)b, c, mod ? mod : ss_heap_of(b));
			shown++;
		}
	}
}

static int clobber_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[16];
		DWORD n = ss_getenv("D3D9SW_CLOBBER", v, sizeof(v));

		cached = (n && n < sizeof(v)) ? atoi(v) : 0;
	}
	return cached;
}

/* Outside the snapshot, like every other piece of our own bookkeeping: this
 * records what a restore did, so a later restore must not be able to rewind
 * it out from under the check. */
static char *clobber_marks(void)
{
	if (!g_clob_ok)
		g_clob_ok = (char *)blk_arena(SS_MAX_REGIONS);
	if (!g_clob_off)
		g_clob_off = (unsigned long long *)blk_arena(SS_MAX_REGIONS *
							    sizeof(unsigned long long));
	if (!g_clob_pre)
		g_clob_pre = (unsigned long long *)blk_arena(SS_MAX_REGIONS *
							    sizeof(unsigned long long));
	return (g_clob_ok && g_clob_off && g_clob_pre) ? g_clob_ok : NULL;
}

static void clobber_arm(int slotno)
{
	LARGE_INTEGER pf, now;
	int ms = clobber_mode();

	if (ms <= 0 || !clobber_marks())
		return;
	QueryPerformanceFrequency(&pf);
	QueryPerformanceCounter(&now);
	g_clob_slot = slotno;
	g_clob_due = now.QuadPart + (pf.QuadPart * ms) / 1000;
}

/* Deliberately does not suspend anything. Freezing the process to ask what the
 * running process is doing would answer a different question, and the threads
 * we care about are exactly the ones a freeze would stop. A word that changes
 * underneath this walk is the phenomenon, not an error in measuring it. */
static void clobber_check(void)
{
	Slot *s;
	Window w;
	unsigned long long pos = 0, worst_words = 0;
	int i, named = 0, hit = 0, worst = -1, reverts = 0;
	const char *marks = g_clob_ok;

	if (g_clob_slot < 0 || !g_ctl)
		return;
	s = &g_ctl->slots[g_clob_slot];
	g_clob_slot = -1;
	if (!s->valid || !marks)
		return;

	memset(&w, 0, sizeof(w));
	w.sect = s->sect;
	w.total = s->bytes;
	ss_log("clobber: %d ms after the restore, checking what came back\n",
	       clobber_mode());
	watch_at_report("after the restore settled", 1);
	ents_report("after the restore settled");
	ents_diff("after the restore settled");
	if (nothread_mode())
		g_nothr_until = GetTickCount() + (DWORD)nothread_mode();
	der_learn(s, &w);
	if (der_mode())
		ss_log("  derived: %u word(s) in the mask (%d new), %d held back during "
		       "this restore, pre-image %.1f MB over %d region(s)\n",
		       g_der_n, g_der_new, g_der_held,
		       (double)g_der_used / (1024.0 * 1024.0), g_der_covered);
	/* Each check replaces the previous verdict rather than adding to it, so
	 * SKIPREG=auto always acts on the most recent restore. */
	g_revert_n = 0;
	for (i = 0; i < s->nregs; i++) {
		unsigned long long fd = 0, words = 0, was = 0, now = 0;
		unsigned long long here = pos;

		pos += s->regs[i].size;
		if (!marks[i])
			continue;
		/* The region was readable when we wrote it, which says nothing about
		 * now: the game has had a few frames to free it or protect it, and a
		 * diagnostic that faults while asking its question is worse than no
		 * diagnostic. Checked per region rather than per page, so a region
		 * that has been partly decommitted is skipped whole. */
		{
			MEMORY_BASIC_INFORMATION mbi;

			if (VirtualQuery((LPCVOID)s->regs[i].base, &mbi, sizeof(mbi)) !=
				    sizeof(mbi) ||
			    mbi.State != MEM_COMMIT ||
			    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
			    (uintptr_t)mbi.BaseAddress + mbi.RegionSize <
				    s->regs[i].base + s->regs[i].size)
				continue;
		}
		if (win_cmp(&w, here, (const void *)s->regs[i].base,
			    (size_t)s->regs[i].size, &fd, &words, &was, &now) != 0)
			continue;
		hit++;
		if (words > worst_words) {
			worst_words = words;
			worst = i;
		}
		/* The verdict, at the one offset we have a before-value for. Equal to
		 * what was there before the restore means the region reverted: the
		 * rewind was undone rather than carried forward. Anything else is the
		 * game running on from the state we gave it, which is what it is
		 * supposed to do and is not a fault at all. */
		if (g_clob_off && g_clob_off[i] != ~0ull &&
		    g_clob_off[i] + sizeof(unsigned long long) <= s->regs[i].size) {
			unsigned long long at = s->regs[i].base + g_clob_off[i];
			unsigned long long cur = *(const unsigned long long *)(uintptr_t)at;
			unsigned long long saved = 0;
			int reverted = (cur == g_clob_pre[i]);

			if (reverted) {
				reverts++;
				if (g_revert_n < SS_POKE_CAP)
					g_revert_at[g_revert_n++] = s->regs[i].base;
			}
			win_u64(&w, here + g_clob_off[i], &saved);
			if (named++ < 14) {
				unsigned moff;
				const char *mod = ss_module(s->regs[i].base, &moff);

				/* Module before heap. This used to print the heap alone,
				 * so every region inside the game's own 17 MB image
				 * reported as (null) and looked anonymous - which is
				 * most of the interesting ones. */
				ss_log("  clobber: region %d at %p +%llx: was %llx before the "
				       "restore, we wrote %llx, now %llx - %s (%llu word(s) "
				       "differ, %s)\n",
				       i, (void *)s->regs[i].base, g_clob_off[i],
				       g_clob_pre[i], saved, cur,
				       reverted ? "REVERTED" : "moved on", words,
				       mod ? mod : ss_heap_of(s->regs[i].base));
			}
		} else if (named++ < 14) {
			unsigned moff;
			const char *mod = ss_module(s->regs[i].base, &moff);

			ss_log("  clobber: region %d at %p, %llu word(s) differ, first at "
			       "+%llx - we wrote %llx, now %llx, %s (no before-value)\n",
			       i, (void *)s->regs[i].base, words, fd, was, now,
			       mod ? mod : ss_heap_of(s->regs[i].base));
		}
	}
	win_close(&w);
	if (!hit)
		ss_log("  clobber: nothing. Every region we restored still holds what "
		       "we put there, so what the restore fails to bring back is "
		       "state we never captured\n");
	else
		ss_log("  clobber: %d of %d restored region(s) differ, %d REVERTED to "
		       "what they held before the restore, worst is region %d with "
		       "%llu word(s)\n",
		       hit, s->nregs, reverts, worst, worst_words);
}

/* Deliberate damage, by hand, as an experiment.
 *
 * Two ways to interfere with a named region, both taking a comma-separated list
 * of region base addresses in hex:
 *
 *   D3D9SW_SKIPREG   restore everything except these. The region keeps whatever
 *                    the running game had in it, which is a value the game
 *                    itself produced and is therefore self-consistent. If the
 *                    symptom changes, that region carries the state; if nothing
 *                    changes, it does not, and it can be struck off.
 *
 *   D3D9SW_SCRIBBLE  restore, then fill with 0xCD. Blunt: a region of pointers
 *                    will fault on the first dereference, which proves only
 *                    that they were pointers. Useful for confirming that a
 *                    region is read at all, and for little else.
 *
 * Skip is the sharper of the two precisely because it leaves the process in a
 * state it could have reached on its own. */
static uintptr_t g_skip_list[SS_POKE_CAP], g_scrib_list[SS_POKE_CAP];
static int g_skip_n = -1, g_scrib_n = -1;

static int poke_parse(const char *name, uintptr_t *out, int cap)
{
	char v[256];
	DWORD n = ss_getenv(name, v, sizeof(v));
	const char *p = v;
	int k = 0;

	if (!n || n >= sizeof(v))
		return 0;
	while (*p && k < cap) {
		unsigned long long acc = 0;
		int any = 0;

		while (*p == ' ' || *p == ',' || *p == '\t')
			p++;
		if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
			p += 2;
		for (;; p++) {
			int d;

			if (*p >= '0' && *p <= '9')
				d = *p - '0';
			else if (*p >= 'a' && *p <= 'f')
				d = *p - 'a' + 10;
			else if (*p >= 'A' && *p <= 'F')
				d = *p - 'A' + 10;
			else
				break;
			acc = acc * 16 + (unsigned)d;
			any = 1;
		}
		if (!any)
			break;
		out[k++] = (uintptr_t)acc;
	}
	return k;
}

static int poke_hit(const uintptr_t *list, int n, uintptr_t base)
{
	int k;

	for (k = 0; k < n; k++)
		if (list[k] == base)
			return 1;
	return 0;
}

/* Re-read on every restore rather than cached like the other knobs, and
 * deliberately so. Region addresses move between launches, so the experiment is
 * always "restore once, read which region reverted, name it, restore again" -
 * and having to relaunch in the middle of that would lose the very state being
 * investigated. Editing the config file mid-session now takes effect on the
 * next restore. Costs one small file read per restore, next to a 200 ms copy. */
static void poke_init(void)
{
	char v[256];
	DWORD n = ss_getenv("D3D9SW_SKIPREG", v, sizeof(v));

	g_skip_auto = (n && n < sizeof(v) &&
		       (v[0] == 'a' || v[0] == 'A')) ? 1 : 0;
	g_skip_n = g_skip_auto ? 0 : poke_parse("D3D9SW_SKIPREG", g_skip_list, SS_POKE_CAP);
	g_scrib_n = poke_parse("D3D9SW_SCRIBBLE", g_scrib_list, SS_POKE_CAP);
	if (g_skip_auto)
		ss_log("  by hand: auto - leaving in the present the %d region(s) that "
		       "reverted after the last restore\n",
		       g_revert_n);
	else if (g_skip_n || g_scrib_n)
		ss_log("  by hand: %d region(s) to leave in the present, %d to scribble\n",
		       g_skip_n, g_scrib_n);
}

static int lfh_hold(void);

/* Regions that are the allocator's own bookkeeping, found by asking the heaps.
 *
 * The low fragmentation heap keeps some of its state outside the segments it
 * manages, in ordinary private allocations of its own. Region 9 at 01E90000 was
 * one: 116 KB of committed memory sitting just below heap 01F50000, holding
 * that heap's handle at offset 0xC. It is not inside any heap segment, so the
 * block-level restore never applied to it, and we put stale contents back over
 * live allocator state on every single restore.
 *
 * That one region explains the whole day. HeapWalk faulting inside
 * RtlpWalkLFHBlock, RtlpLocalInfoAllocFromCache reading through a zeroed
 * pointer, RtlpSubSegmentInitialize dying while a new thread allocated its TLS,
 * and only ever on the process heap because it is by far the heaviest LFH user.
 * HeapValidate passed throughout and was telling the truth - the classic block
 * chain really was intact, and the damage was entirely in the front end.
 *
 * The clobber watch had been reporting it as REVERTED for hours.
 *
 * Finding them by asking each heap which addresses it points at needs no
 * internal structure offsets and so does not depend on the Windows build. Heap
 * segments are deliberately exempt: those are handled block by block, and
 * marking them here would switch heap restore off altogether. */
static uintptr_t g_lfh_at[SS_POKE_CAP];
static char g_lfh_keep[SS_POKE_CAP];
static int g_lfh_n;
static int lfh_hold(void);

/* Holding a heap's bookkeeping is right when its blocks stay in the present or
 * come back block by block under live metadata. A rewound heap with block
 * restore off has every segment written back whole, and then holding the
 * bookkeeping is the split: present metadata over past blocks, which divides
 * by a zero BlockCount in ntdll (DDPR, 2026-10-02). */
static int lfh_keep_for(int k)
{
	int h = lfh_hold();

	if (h != 2)
		return h;
	return !g_ctl->heap_ours[k] || blk_mode();
}

static void lfh_find_regions(Slot *s)
{
	int k, i;

	g_lfh_n = 0;
	for (k = 0; k < g_ctl->nheaps; k++) {
		const uintptr_t *hdr = (const uintptr_t *)g_ctl->heap_h[k];
		MEMORY_BASIC_INFORMATION mbi;
		unsigned w, words = 0x1000 / sizeof(uintptr_t);

		if (!hdr || VirtualQuery(hdr, &mbi, sizeof(mbi)) != sizeof(mbi) ||
		    mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
			continue;
		if ((uintptr_t)mbi.BaseAddress + mbi.RegionSize < (uintptr_t)hdr + 0x1000)
			continue;
		for (w = 0; w < words; w++) {
			uintptr_t p = hdr[w];

			if (p < 0x10000)
				continue;
			for (i = 0; i < s->nregs; i++) {
				uintptr_t b = s->regs[i].base;

				if (p < b || p >= b + s->regs[i].size)
					continue;
				if (heap_index_of(b) >= 0)
					break; /* a segment; block restore owns it */
				/* Being pointed at by a heap header is necessary and
				 * nowhere near sufficient. The header carries the
				 * heap's virtual-alloc list, which names every large
				 * block the heap handed to the game through
				 * VirtualAlloc, so this test on its own claimed the
				 * game's asset memory: 70 MB at 18A80000 and 86 MB at
				 * 135E0000 among fifty-six regions. The game then
				 * faulted in its own code reading state we had stopped
				 * putting back.
				 *
				 * Allocator bookkeeping is small and names its heap
				 * near its start. A buffer the heap merely handed out
				 * does neither. The size ceiling is empirical rather
				 * than documented, so it is logged when it decides. */
				if (s->regs[i].size > (2u << 20))
					break;
				/* The saved address, read live. From another process it
				 * can be reserved and nothing more. */
				if (!ss_readable(b, 64 * sizeof(uintptr_t)))
					break;
				{
					const uintptr_t *hw = (const uintptr_t *)b;
					unsigned q, back = 0;

					for (q = 0; q < 64 && !back; q++)
						if (hw[q] == (uintptr_t)g_ctl->heap_h[k])
							back = 1;
					if (!back)
						break;
				}
				if (poke_hit(g_lfh_at, g_lfh_n, b) || g_lfh_n >= SS_POKE_CAP)
					break;
				g_lfh_keep[g_lfh_n] = (char)lfh_keep_for(k);
				g_lfh_at[g_lfh_n++] = b;
				/* Says what will happen, not what used to happen. This
				 * line claimed "left in the present" on a run where
				 * LFHHOLD=0 was restoring them, because it prints at
				 * discovery and the decision is taken later. */
				ss_log("  LFH: region %d at %p is bookkeeping for heap %p and "
				       "sits outside its segments - %s, %llu bytes\n",
				       i, (void *)b, (void *)g_ctl->heap_h[k],
				       g_lfh_keep[g_lfh_n - 1]
					       ? "left in the present"
					       : "restored with its heap, whose segments "
						 "all come back whole",
				       (unsigned long long)s->regs[i].size);
				break;
			}
		}
	}
}

/* Whether the LFH's own bookkeeping stays in the present.
 *
 * Holding it was the right call while heaps were partitioned by owner, because
 * then the segments it describes were half held anyway. With ALLHEAPS=2 every
 * segment travels, and holding the bookkeeping creates the split on its own:
 * present-time metadata describing past-time blocks. The allocator derives
 * bucket indices and block counts by dividing by fields in that metadata, so
 * the failure mode is not a bad pointer but arithmetic on a zero - which is
 * exactly the C0000094 in ntdll that replaced the access violation the moment
 * the segment split was closed.
 *
 * 1 always holds, 0 never does, unset (2) decides per heap in lfh_keep_for. */
static int lfh_hold(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_LFHHOLD", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v)) ? (v[0] == '0' ? 0 : 1) : 2;
	}
	return cached;
}

static int poke_skipped(uintptr_t base)
{
	int i;

	for (i = 0; i < g_lfh_n; i++)
		if (g_lfh_at[i] == base && g_lfh_keep[i])
			return 1;
	if (g_skip_auto)
		return poke_hit(g_revert_at, g_revert_n, base);
	return poke_hit(g_skip_list, g_skip_n, base);
}

/* Pages a LEFT RUNNING thread currently holds a pointer into, on a heap we
 * would otherwise memcpy. mmdevapi tid 524 ExitProcess(3) during load-copy
 * was that memcpy: its TLS still pointed at gameheap 0CE20000 while we
 * rewrote the region wholesale. Do not SuspendThread those peers (wineserver).
 * Leave the pages they hold in the present. */
#define SS_LIVE_PAGES 256
static uintptr_t g_live_page[SS_LIVE_PAGES];
static int g_live_page_n;
static unsigned long long g_live_skip_bytes;
/* Set per region by the write loop when the region overlaps an exclusion that
 * appeared since the save - a live stack, TEB, or 64-bit thread block now
 * sitting where ordinary memory was. Those pages keep their live bytes. */
static int g_copy_excl;
static unsigned long long g_excl_skip_bytes;

static int live_page_held(uintptr_t a)
{
	uintptr_t pg = a & ~(uintptr_t)0xFFF;
	int i;

	for (i = 0; i < g_live_page_n; i++)
		if (g_live_page[i] == pg)
			return 1;
	return 0;
}

static int live_page_in_span(uintptr_t base, size_t n)
{
	int i;

	for (i = 0; i < g_live_page_n; i++)
		if (g_live_page[i] >= base && g_live_page[i] < base + n)
			return 1;
	return 0;
}

static void live_page_note(uintptr_t a)
{
	uintptr_t pg = a & ~(uintptr_t)0xFFF;

	if (!pg || live_page_held(pg) || g_live_page_n >= SS_LIVE_PAGES)
		return;
	g_live_page[g_live_page_n++] = pg;
}

static void live_page_scan(const uintptr_t *w, size_t words)
{
	size_t k;

	for (k = 0; k < words; k++) {
		int h = heap_index_of(w[k]);

		if (h < 0 || !g_ctl->heap_ours[h])
			continue;
		live_page_note(w[k]);
	}
}

/* i386 Wine keeps the unix syscall frame at TEB+0x218. The first dword is
 * the restore flags. Bit 2 (orl $2 in signal_set_full_context) makes
 * __wine_unix_call_dispatcher reload eax from frame+0x1c and throw away
 * the unix function's 0. mmdevapi then asserts. The audio thread is left
 * running, so this only reads the TEB and clears that one bit. */
static void unix_frame_shield(const char *when, int hold_pages)
{
#if defined(_M_IX86) || defined(__i386__)
	int i, nframe = 0, nclear = 0;

	if (!ss_under_wine() || !g_ctl)
		return;
	for (i = 0; i < g_ctl->nids; i++) {
		unsigned char *teb;
		uintptr_t frame;
		DWORD flags;
		MEMORY_BASIC_INFORMATION mbi;
		int h;

		if (g_did_suspend[i] || !g_ctl->handles[i])
			continue;
		teb = (unsigned char *)teb_of(g_ctl->handles[i]);
		if (!teb)
			continue;
		frame = *(uintptr_t *)(teb + 0x218);
		if (frame < 0x10000 || (frame & 3))
			continue;
		if (VirtualQuery((LPCVOID)frame, &mbi, sizeof(mbi)) != sizeof(mbi))
			continue;
		if (mbi.State != MEM_COMMIT ||
		    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD | PAGE_READONLY |
				    PAGE_EXECUTE | PAGE_EXECUTE_READ)))
			continue;
		flags = *(DWORD *)frame;
		/* 0x8000 is the in-call mark the dispatcher writes. Bit 2 is
		 * the poison. Anything else is not this frame. */
		if (!(flags & 0x8002))
			continue;
		nframe++;
		h = heap_index_of(frame);
		if (flags & 2) {
			InterlockedAnd((LONG volatile *)frame, ~(LONG)2);
			nclear++;
		}
		/* pulse_main_loop returns only 0. The dword at frame+0x1c is not
		 * that return: it is eax saved by an earlier context round-trip
		 * (0xD on the title's audio thread). The epilogue reloads it when
		 * bit 2 is set, and mmdevapi asserts on any nonzero status. Zero
		 * the saved status so the reload is success. */
		{
			DWORD saved = *(DWORD *)(frame + 0x1c);

			if (saved) {
				*(volatile DWORD *)(frame + 0x1c) = 0;
				nclear++;
			}
			if (hold_pages)
				live_page_note(frame);
			ss_log("  unix frame: tid %lu at %p flags %08lx%s%s eax %08lx%s (%s)\n",
			       (unsigned long)g_ctl->ids[i], (void *)frame,
			       (unsigned long)flags,
			       (flags & 2) ? " bit2 cleared" : " bit2 clear",
			       h >= 0 ? " INSIDE a rewound heap" : "",
			       (unsigned long)saved, saved ? " cleared" : "", when);
		}
	}
	if (!nframe)
		ss_log("  unix frame: no in-call frame on a left-running thread (%s)\n",
		       when);
	else if (!nclear && !hold_pages)
		ss_log("  unix frame: %d in-call frame(s), bit 2 already clear (%s)\n",
		       nframe, when);
#else
	(void)when;
	(void)hold_pages;
#endif
}

/* Same clear as the shield, without a line per region. Called around each
 * VirtualProtect so a SIGUSR1 that rewrote the saved status is put back
 * before the unix call can return through it. */
static void unix_frame_heal(void)
{
#if defined(_M_IX86) || defined(__i386__)
	int i;

	if (!ss_under_wine() || !g_ctl)
		return;
	for (i = 0; i < g_ctl->nids; i++) {
		unsigned char *teb;
		uintptr_t frame;
		DWORD flags;
		MEMORY_BASIC_INFORMATION mbi;

		if (g_did_suspend[i] || !g_ctl->handles[i])
			continue;
		teb = (unsigned char *)teb_of(g_ctl->handles[i]);
		if (!teb)
			continue;
		frame = *(uintptr_t *)(teb + 0x218);
		if (frame < 0x10000 || (frame & 3))
			continue;
		if (VirtualQuery((LPCVOID)frame, &mbi, sizeof(mbi)) != sizeof(mbi))
			continue;
		if (mbi.State != MEM_COMMIT ||
		    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD | PAGE_READONLY |
				    PAGE_EXECUTE | PAGE_EXECUTE_READ)))
			continue;
		flags = *(DWORD *)frame;
		/* Not only threads inside a call. winmm's device thread was idle
		 * during the copy, then client_Stop -> pulse_stop asserted once
		 * resume let it run. Call entry writes 0x8000 over the flags and
		 * leaves the saved eax, so a stale frame+0x1c is what bit 2
		 * reloads. Zero it on every left-running frame. */
		if (flags & 2)
			InterlockedAnd((LONG volatile *)frame, ~(LONG)2);
		if (*(DWORD *)(frame + 0x1c))
			*(volatile DWORD *)(frame + 0x1c) = 0;
	}
#else
#endif
}

/* True when a left-running unix frame lies in [base, base+size). The copy
 * must not VirtualProtect that span: mprotect of the live frame is a
 * SIGUSR1, and the epilogue then returns the saved eax. */
static int unix_frame_in_span(uintptr_t base, size_t size)
{
#if defined(_M_IX86) || defined(__i386__)
	int i;

	if (!ss_under_wine() || !g_ctl || !size)
		return 0;
	for (i = 0; i < g_ctl->nids; i++) {
		unsigned char *teb;
		uintptr_t frame;

		if (g_did_suspend[i] || !g_ctl->handles[i])
			continue;
		teb = (unsigned char *)teb_of(g_ctl->handles[i]);
		if (!teb)
			continue;
		frame = *(uintptr_t *)(teb + 0x218);
		if (frame < base || frame >= base + size)
			continue;
		/* A left-running thread keeps this pointer after the call returns.
		 * Only the in-call mark means the frame is live. Skipping every
		 * stale slot left three other frame allocations unrestored. */
		{
			MEMORY_BASIC_INFORMATION mbi;
			DWORD flags;

			if (VirtualQuery((LPCVOID)frame, &mbi, sizeof(mbi)) != sizeof(mbi))
				continue;
			if (mbi.State != MEM_COMMIT ||
			    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD | PAGE_READONLY |
				    PAGE_EXECUTE | PAGE_EXECUTE_READ)))
				continue;
			flags = *(DWORD *)frame;
			if (!(flags & 0x8000))
				continue;
		}
		return 1;
	}
#else
	(void)base;
	(void)size;
#endif
	return 0;
}

static void live_peer_pages_collect(void)
{
	int i, named = 0;

	g_live_page_n = 0;
	g_live_skip_bytes = 0;
	if (!g_ctl)
		return;
	for (i = 0; i < g_ctl->nids; i++) {
		unsigned char *teb;
		const uintptr_t *w;
		uintptr_t hops[16];
		unsigned k;
		int nhops = 0, j;

		if (g_did_suspend[i] || !g_ctl->handles[i])
			continue;
		teb = (unsigned char *)teb_of(g_ctl->handles[i]);
		if (!teb)
			continue;
		w = (const uintptr_t *)teb;
		live_page_scan(w, 0x1000 / sizeof(uintptr_t));
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
		for (j = 0; j < nhops; j++) {
			uintptr_t end = hops[j] + alloc_span(hops[j]), p = hops[j];

			while (p < end) {
				MEMORY_BASIC_INFORMATION mbi;
				uintptr_t stop;

				if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) != sizeof(mbi))
					break;
				stop = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
				if (stop > end)
					stop = end;
				if (stop <= p)
					break;
				if (mbi.State == MEM_COMMIT &&
				    !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
					live_page_scan((const uintptr_t *)p,
						       (size_t)(stop - p) / sizeof(uintptr_t));
				p = stop;
			}
		}
		/* The TEB hop list is small private tables. The live stack is
		 * where a mixer actually keeps the pointers it is using. Do not
		 * GetThreadContext (wineserver). NT_TIB StackBase/StackLimit
		 * are user-readable. */
		{
			uintptr_t stack_base = w[1], stack_limit = w[2], p;

			if (stack_limit && stack_base > stack_limit &&
			    stack_base - stack_limit <= 0x200000) {
				p = stack_limit;
				while (p < stack_base) {
					MEMORY_BASIC_INFORMATION mbi;
					uintptr_t stop;

					if (VirtualQuery((LPCVOID)p, &mbi,
							 sizeof(mbi)) != sizeof(mbi))
						break;
					stop = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
					if (stop > stack_base)
						stop = stack_base;
					if (stop <= p)
						break;
					if (mbi.State == MEM_COMMIT &&
					    !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
						live_page_scan((const uintptr_t *)p,
							       (size_t)(stop - p) /
								       sizeof(uintptr_t));
					p = stop;
				}
			}
		}
		if (named++ < 8) {
			unsigned moff = 0;
			const char *mname = g_ctl->starts[i]
						? ss_module((uintptr_t)g_ctl->starts[i], &moff)
						: NULL;

			ss_log("  live-peer: tid %lu LEFT RUNNING, start %s - scanning TLS "
			       "for pointers into heaps we rewind\n",
			       (unsigned long)g_ctl->ids[i], mname ? mname : "?");
		}
	}
	if (g_live_page_n)
		ss_log("  live-peer hold: %d page(s) (%u KB) on rewound heaps, left in "
		       "the present so LEFT RUNNING TLS does not watch a memcpy\n",
		       g_live_page_n, (unsigned)(g_live_page_n * 4));
	else
		ss_log("  live-peer hold: no TLS pointers into rewound heaps from LEFT "
		       "RUNNING threads\n");
	/* After the TLS scan, so the frame page is not wiped with that list. */
	unix_frame_shield("before-copy", 1);
}

static int win_read(Window *w, unsigned long long pos, void *dst, size_t n)
{
	unsigned char *d = dst;

	while (n) {
		size_t chunk;
		unsigned char *view;

		if (!win_cover(w, pos))
			return 0;
		view = w->base + (size_t)(pos - w->off);
		chunk = (size_t)(w->off + w->size - pos);
		if (chunk > n)
			chunk = n;
		memcpy(d, view, chunk);
		d += chunk;
		pos += chunk;
		n -= chunk;
	}
	return 1;
}

/* Copy saved bytes into live memory, but do not rewrite a page a LEFT RUNNING
 * peer currently holds. pos still advances across a hole so the slot mapping
 * stays aligned. */
static int page_committed(uintptr_t p)
{
	MEMORY_BASIC_INFORMATION mbi;

	return VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == sizeof(mbi) &&
	       mbi.State == MEM_COMMIT;
}

/* The .sspres section of this image (SS_PRESENT globals), found once. */
static uintptr_t g_pres_lo SS_PRESENT, g_pres_hi SS_PRESENT;
static int g_pres_found SS_PRESENT;
static unsigned long long g_pres_skip_bytes;

static int page_present_only(uintptr_t a)
{
	if (!g_pres_found) {
		const IMAGE_DOS_HEADER *dh = (const IMAGE_DOS_HEADER *)&__ImageBase;
		const IMAGE_NT_HEADERS *nh = (const IMAGE_NT_HEADERS *)((const char *)dh + dh->e_lfanew);
		const IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nh);
		int i;

		for (i = 0; i < nh->FileHeader.NumberOfSections; i++)
			if (!memcmp(sh[i].Name, ".sspres", 7)) {
				g_pres_lo = (uintptr_t)dh + sh[i].VirtualAddress;
				g_pres_hi = (g_pres_lo + sh[i].Misc.VirtualSize + 0xFFF) & ~(uintptr_t)0xFFF;
			}
		g_pres_found = 1;
	}
	return a >= g_pres_lo && a < g_pres_hi;
}

static int win_copy_except_live(Window *w, unsigned long long pos, void *mem, size_t n)
{
	unsigned char *p = mem;
	size_t done = 0;

	while (done < n) {
		size_t page = 0x1000 - (((uintptr_t)p + done) & 0xFFF);

		if (page > n - done)
			page = n - done;
		if (page_present_only((uintptr_t)p + done)) {
			g_pres_skip_bytes += page;
		} else if (g_copy_excl && region_excluded((uintptr_t)p + done, page)) {
			g_excl_skip_bytes += page;
		} else if (g_copy_excl && !page_committed((uintptr_t)p + done)) {
			/* The check pass never recommits a partly excluded region, and
			 * the rest of it may not exist in this process. */
			g_excl_skip_bytes += page;
		} else if (live_page_held((uintptr_t)p + done)) {
			g_live_skip_bytes += page;
		} else if (!win_copy(w, pos + done, p + done, page, 0)) {
			return 0;
		}
		done += page;
	}
	return 1;
}

/* Wine HEAPBLOCKS for the redirected game heap: put back busy GhHead blocks
 * from the save, leave allocator lists (and any live-peer pages) in the present.
 * Never falls back to a wholesale memcpy - that is the named leave.
 *
 * Preferred source is the sidetable snapshot taken after suspend: that is the
 * exact live set, with no HeapWalk and no scan of heap bytes. The GhHead image
 * scan remains for a slot that has no snapshot (cross-session, or the table
 * was never reserved). */
static int wine_gameheap_copy(Window *w, unsigned long long pos, void *live, size_t n)
{
	size_t off = 0;
	int nblk = 0;
	unsigned long long wrote = 0;
	unsigned i, nbusy = gameheap_busy_saved_count();

	if (nbusy) {
		/* The copy below can restore a saved flags word. Clear bit 2
		 * again first, and skip any block that lands on the live frame. */
		unix_frame_shield("gameheap", 1);
		for (i = 0; i < nbusy; i++) {
			void *head = NULL;
			size_t blk = 0;

			if (!gameheap_busy_saved_at(i, &head, &blk))
				continue;
			if ((uintptr_t)head < (uintptr_t)live)
				continue;
			off = (uintptr_t)head - (uintptr_t)live;
			if (off >= n || blk > n - off)
				continue;
			if (!win_copy_except_live(w, pos + off, head, blk))
				return -1;
			nblk++;
			wrote += blk;
		}
		ss_log("  wine gameheap: %d of %u sidetable block(s) copied, %llu KB "
		       "written, %llu KB left as lists/live-peer pages (no HeapWalk, "
		       "no wholesale memcpy)\n",
		       nblk, nbusy, wrote >> 10,
		       (unsigned long long)(n - (size_t)wrote) >> 10);
		return nblk;
	}

	while (off + 8 <= n) {
		unsigned char head[16];
		size_t pull = sizeof(head), blk = 0;

		if (pull > n - off)
			pull = n - off;
		if (!win_read(w, pos + off, head, pull))
			return -1;
		if (!gameheap_saved_block(head, (char *)live + off, n - off, &blk)) {
			off += 8;
			continue;
		}
		if (!win_copy_except_live(w, pos + off, (char *)live + off, blk))
			return -1;
		nblk++;
		wrote += blk;
		off += (blk + 7u) & ~(size_t)7u;
	}
	ss_log("  wine gameheap: %d busy block(s) copied by GhHead scan (no "
	       "sidetable snapshot), %llu KB written, %llu KB left as "
	       "lists/live-peer pages (no wholesale memcpy)\n",
	       nblk, wrote >> 10, (unsigned long long)(n - (size_t)wrote) >> 10);
	return nblk;
}

static int catch_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[16];
		DWORD n = ss_getenv("D3D9SW_CATCH", v, sizeof(v));

		cached = (n && n < sizeof(v)) ? atoi(v) : 0;
	}
	return cached;
}

/* Armed while the threads are still frozen, so the very first access after the
 * restore is the one that gets caught rather than the tenth. The offsets come
 * from the same pass that records the pre-restore values: they are the places
 * memory had already drifted from the snapshot, which is exactly where a
 * restore that does not hold shows up. */
static void catch_arm(Slot *s)
{
	int i, cap = catch_mode(), armed = 0, nstk = 0, skipped = 0;
	uintptr_t stk_lo[64], stk_hi[64];
	SYSTEM_INFO si;
	uintptr_t page;

	g_catch_n = 0;
	g_catch_hits = 0;
	if (cap <= 0 || !g_clob_off || !g_clob_ok)
		return;
	if (cap > SS_CATCH_CAP)
		cap = SS_CATCH_CAP;
	GetSystemInfo(&si);
	page = si.dwPageSize ? si.dwPageSize : 4096;

	/* Every thread stack has drifted, because its thread was running, so
	 * arming there is guaranteed to catch something and guaranteed to teach
	 * nothing: the hit is just the thread resuming and using its own stack.
	 * The first run proved it, spending 28 of 48 pages on a perfect
	 * arithmetic sequence of per-thread blocks, one hit each, all from the
	 * same instruction. Collected from the TEBs while the handles are still
	 * open, which is only true before resume_all. */
	for (i = 0; i < g_ctl->nids && nstk < 64; i++) {
		MEMORY_BASIC_INFORMATION mbi;
		unsigned char *teb;
		uintptr_t limit, base;

		if (!g_ctl->handles[i])
			continue;
		teb = (unsigned char *)teb_of(g_ctl->handles[i]);
		if (!teb)
			continue;
		/* NT_TIB: ExceptionList, StackBase, StackLimit - so the third slot
		 * at both widths. The limit is inside the committed part; the
		 * reservation around it is the whole stack. */
		limit = *(uintptr_t *)(teb + 2 * sizeof(uintptr_t));
		if (!limit || VirtualQuery((LPCVOID)limit, &mbi, sizeof(mbi)) != sizeof(mbi))
			continue;
		base = (uintptr_t)mbi.AllocationBase;
		if (!base)
			continue;
		stk_lo[nstk] = base;
		stk_hi[nstk] = base + alloc_span(base);
		nstk++;
	}

	for (i = 0; i < s->nregs && armed < cap; i++) {
		uintptr_t at;
		DWORD old;
		int k, is_stack = 0;

		if (!g_clob_ok[i] || g_clob_off[i] == ~0ull)
			continue;
		at = (s->regs[i].base + (uintptr_t)g_clob_off[i]) & ~(page - 1);
		for (k = 0; k < nstk; k++)
			if (at >= stk_lo[k] && at < stk_hi[k]) {
				is_stack = 1;
				break;
			}
		if (is_stack) {
			skipped++;
			continue;
		}
		if (!VirtualProtect((LPVOID)at, page, s->regs[i].prot | PAGE_GUARD, &old))
			continue;
		g_catch_at[armed++] = at;
	}
	g_catch_n = armed;
	if (armed)
		ss_log("  catch: %d guard page(s) armed on addresses that had drifted, "
		       "%d skipped as thread stacks (%d stack(s) known)\n",
		       armed, skipped, nstk);
}

static void clobber_tick(void)
{
	LARGE_INTEGER now;

	if (g_clob_slot < 0)
		return;
	QueryPerformanceCounter(&now);
	if (now.QuadPart >= g_clob_due)
		clobber_check();
}

/* -------------------------------------------------- relocatable restore (Option B)
 *
 * Cross-session, a saved snapshot's absolute addresses are wrong wherever the
 * thing they point at loaded elsewhere this run - the exe most of all (ASLR).
 * Each saved anchor (module by name, arena by range) carries its save-time base;
 * here we find its NEW base and shift every pointer-shaped word in the restored
 * memory that falls in a MOVED range by that range's delta. The arena is pinned
 * at a fixed base so its delta is zero and it needs nothing. Heuristic - a
 * non-pointer word that happens to land in a moved range is shifted too - but the
 * surface is tiny (essentially the exe) so the collateral is in dead data, which
 * the cross-session bar accepts. D3D9SW_RELOC=1 shifts, =2 only reports what it
 * would shift. The same path runs for every load: deltas come from matching each
 * saved module to a live one by identity, so a same-process load finds every
 * delta zero by construction, and the provenance tripwire in reloc_apply says so
 * loudly if it ever does not. This is the memory-content half; the
 * adopted thread's SP and context come with the adoption piece. */
typedef struct {
	uintptr_t lo, hi;
	intptr_t delta;
	int mod;             /* index into the slot's module list, -1 for a stack */
	int write;           /* 0: count what would be shifted, touch nothing */
	unsigned long hits;
	uintptr_t sample_at; /* first word matched, for the log */
	uintptr_t sample_val;
} RelocEnt;

/* The live module that IS saved module i: same path, same link stamp, same image
 * size. -1 when none is loaded - absent this session, or a different build.
 *
 * A base name does not identify a module. Our d3d11.dll and dxgi.dll are pinned
 * at 60000000/61000000 while the GPU probe loads the system copies as well, so
 * the list holds each name twice and GetModuleHandleA only ever answered with
 * ours. The system copy then looked moved by -227 MB in a same-session restore
 * and 3527 words in its range were shifted - real pointers into it and plain
 * data that happened to fall there, audio among it - until DxLib found a block's
 * guard words broken and stopped the game on purpose. */
static int reloc_live_of(const Slot *s, int i)
{
	int j;

	if (!s->mod_path[i][0])
		return -1;
	for (j = 0; j < g_ctl->nmods; j++)
		if (g_ctl->mod_stamp[j] == s->mod_stamp[i] &&
		    g_ctl->mod_isize[j] == s->mod_isize[i] &&
		    lstrcmpiA(g_ctl->mod_path[j], s->mod_path[i]) == 0)
			return j;
	return -1;
}

/* A saved code address where that module sits now. A thread's start address
 * names its role, and the exe itself can load elsewhere on another boot. */
static PVOID start_now(const Slot *s, PVOID start)
{
	uintptr_t a = (uintptr_t)start;
	int i, j;

	for (i = 0; i < s->nmods; i++)
		if (a >= s->mod_lo[i] && a < s->mod_hi[i]) {
			j = reloc_live_of(s, i);
			return j < 0 ? start : (PVOID)(a - s->mod_lo[i] + g_ctl->mod_lo[j]);
		}
	return start;
}

static int reloc_build(const Slot *s, RelocEnt *map, int max)
{
	int i, n = 0, missing = 0;

	for (i = 0; i < s->nmods && n < max; i++) {
		int j = reloc_live_of(s, i);
		intptr_t d;

		if (j < 0) {
			/* Named so a rebuilt module reads as what it is. Pointers into
			 * it are left as they were: shifting toward a different build's
			 * layout would be a guess. */
			if (missing++ < 8)
				ss_log("  reloc: %-20s saved at %08lX (stamp %08lX) has no "
				       "identical module loaded now - absent or a different "
				       "build, not shifted\n",
				       s->mod_name[i], (unsigned long)s->mod_lo[i],
				       (unsigned long)s->mod_stamp[i]);
			continue;
		}
		d = (intptr_t)(g_ctl->mod_lo[j] - s->mod_lo[i]);
		if (!d)
			continue;
		map[n].lo = s->mod_lo[i];
		map[n].hi = s->mod_hi[i];
		map[n].delta = d;
		map[n].mod = i;
		map[n].write = 1;
		map[n].hits = 0;
		map[n].sample_at = 0;
		map[n].sample_val = 0;
		ss_log("  reloc: %-20s was %08lX now %08lX, delta %+ld (%lu KB range) - %s\n",
		       s->mod_name[i], (unsigned long)s->mod_lo[i],
		       (unsigned long)g_ctl->mod_lo[j], (long)d,
		       (unsigned long)((s->mod_hi[i] - s->mod_lo[i]) >> 10), s->mod_path[i]);
		n++;
	}
	if (missing > 8)
		ss_log("  reloc: %d saved module(s) in all have no identical module loaded "
		       "now\n", missing);
	return n;
}

/* Our own image, never shifted. The map sits in it - a restored region - and
 * holds the very values it matches: scanned, it rewrote its own ranges and the
 * next word matched garbage, 60 million device contexts replaced in one load.
 * The rest of our data is bookkeeping (journal homes, hook trampolines, the
 * cookie list) that the same pass bent out of shape. */
static uintptr_t g_reloc_self_lo, g_reloc_self_hi;

/* The map as disjoint sorted segments, each naming the entry that wins there
 * (the lowest index, as the linear walk had it), and a table by the top 16 bits
 * of a 32-bit value giving the first segment touching that 64 KB. Nearly every
 * word of ~2 GB of restored memory is rejected by one load from the table; the
 * linear walk over every entry for every word took 17 s. */
#define RX_MAX (2 * (SS_MAX_MODS + 2 * SS_MAX_THREADS + 2 + 2 * SS_MAX_WND + SS_MAX_STREAMS + \
		     SS_MAX_THREADS + 2))
static uintptr_t g_rx_lo[RX_MAX], g_rx_hi[RX_MAX];
static int g_rx_j[RX_MAX], g_rx_n, g_rx_ok;
static int g_rx_first[65536];
static const RelocEnt *g_rx_map;
static int g_rx_mapn = -1;

static void reloc_index(const RelocEnt *map, int n)
{
	static uintptr_t bnd[RX_MAX];
	int nb = 0, i, k, j;

	g_rx_map = map;
	g_rx_mapn = n;
	g_rx_n = 0;
	g_rx_ok = 1;
	for (i = 0; i < n; i++) {
		if ((unsigned long long)map[i].hi > 0x100000000ull || map[i].hi <= map[i].lo ||
		    nb + 2 > RX_MAX) {
			g_rx_ok = 0;
			return;
		}
		bnd[nb++] = map[i].lo;
		bnd[nb++] = map[i].hi;
	}
	for (i = 1; i < nb; i++) {
		uintptr_t v = bnd[i];

		for (k = i - 1; k >= 0 && bnd[k] > v; k--)
			bnd[k + 1] = bnd[k];
		bnd[k + 1] = v;
	}
	for (i = 0; i + 1 < nb; i++) {
		uintptr_t lo = bnd[i], hi = bnd[i + 1];

		if (lo == hi)
			continue;
		for (j = 0; j < n; j++)
			if (map[j].lo <= lo && lo < map[j].hi)
				break;
		if (j == n)
			continue;
		if (g_rx_n && g_rx_j[g_rx_n - 1] == j && g_rx_hi[g_rx_n - 1] == lo) {
			g_rx_hi[g_rx_n - 1] = hi;
			continue;
		}
		g_rx_lo[g_rx_n] = lo;
		g_rx_hi[g_rx_n] = hi;
		g_rx_j[g_rx_n++] = j;
	}
	for (i = 0; i < 65536; i++)
		g_rx_first[i] = -1;
	for (i = g_rx_n - 1; i >= 0; i--) {
		unsigned b0 = (unsigned)(g_rx_lo[i] >> 16), b1 = (unsigned)((g_rx_hi[i] - 1) >> 16), b;

		for (b = b0; b <= b1; b++)
			g_rx_first[b] = i;
	}
	/* Every edge is where an off-by-one would show: the index and the linear
	 * walk must name the same entry on both sides of each. */
	for (i = 0; i < nb; i++) {
		int d;

		for (d = -1; d <= 0; d++) {
			uintptr_t v = bnd[i] + (uintptr_t)d;
			int lin = -1, idx = -1, sg;

			for (j = 0; j < n; j++)
				if (v >= map[j].lo && v < map[j].hi) {
					lin = j;
					break;
				}
			sg = g_rx_first[(unsigned)(v >> 16) & 0xFFFFu];
			if (sg >= 0) {
				while (sg < g_rx_n && g_rx_hi[sg] <= v)
					sg++;
				if (sg < g_rx_n && g_rx_lo[sg] <= v)
					idx = g_rx_j[sg];
			}
			if (lin != idx) {
				ss_log("  reloc: INDEX DISAGREES at %08lX (linear %d, index %d) - "
				       "falling back to the linear walk\n",
				       (unsigned long)v, lin, idx);
				g_rx_ok = 0;
				return;
			}
		}
	}
}

static unsigned long reloc_scan(uintptr_t base, size_t size, RelocEnt *map, int n)
{
	uintptr_t end = base + size, a = base;
	unsigned long shifted = 0;
	MEMORY_BASIC_INFORMATION mbi;

	if (map != g_rx_map || n != g_rx_mapn)
		reloc_index(map, n);

	/* Walk only what this process actually has committed under the saved
	 * extent, run by run, rather than trusting the saved size and striding to
	 * its end. A region can be back only in part: EXCLFORCE writes the pages
	 * whose address is committed and leaves holes where it is not, and a region
	 * that "belongs elsewhere" now begins inside a shorter live allocation. The
	 * old scan checked one readable first word and then strode the whole size -
	 * it faulted ~26 MB into a 32 MB region when the live allocation ended and
	 * the next page was free. Each committed, writable run is scanned on its
	 * own; free, reserved and no-access runs are stepped over, never read. */
	while (a < end && VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) == sizeof(mbi)) {
		uintptr_t run = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		int usable = mbi.State == MEM_COMMIT &&
			     !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
			     (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
					     PAGE_EXECUTE_READWRITE |
					     PAGE_EXECUTE_WRITECOPY)) != 0;

		if (run > end)
			run = end;
		if (run <= a)
			break;
		if (usable) {
			uintptr_t *p = (uintptr_t *)a;
			size_t words = (size_t)(run - a) / sizeof(uintptr_t), w;
			int j;

			for (w = 0; w < words; w++) {
				uintptr_t val = p[w];

				if (g_rx_ok) {
					int sg;

					if ((unsigned long long)val >= 0x100000000ull ||
					    (sg = g_rx_first[(unsigned)(val >> 16)]) < 0)
						continue;
					while (sg < g_rx_n && g_rx_hi[sg] <= val)
						sg++;
					if (sg == g_rx_n || g_rx_lo[sg] > val)
						continue;
					if ((uintptr_t)&p[w] >= g_reloc_self_lo &&
					    (uintptr_t)&p[w] < g_reloc_self_hi)
						continue;
					j = g_rx_j[sg];
					if (!map[j].hits++) {
						map[j].sample_at = (uintptr_t)&p[w];
						map[j].sample_val = val;
					}
					if (map[j].write)
						p[w] = (uintptr_t)((intptr_t)val + map[j].delta);
					shifted++;
					continue;
				}
				if ((uintptr_t)&p[w] >= g_reloc_self_lo &&
				    (uintptr_t)&p[w] < g_reloc_self_hi)
					continue;
				for (j = 0; j < n; j++)
					if (val >= map[j].lo && val < map[j].hi) {
						if (!map[j].hits++) {
							map[j].sample_at = (uintptr_t)&p[w];
							map[j].sample_val = val;
						}
						if (map[j].write)
							p[w] = (uintptr_t)((intptr_t)val +
									   map[j].delta);
						shifted++;
						break;
					}
			}
		}
		a = run;
	}
	return shifted;
}

/* Saved region i is the committed top of a reservation nothing else of it was
 * saved from - the shape of a thread stack - and no thread the save rewinds was
 * standing on it. System threads' stacks are excluded at the save and the game
 * threads' stacks are in s->threads, so this is a thread that had already gone,
 * or one we never listed. No restored register points into it, and once the
 * process has freed it and handed the addresses out again it cannot be put
 * back: Haydee's thread pool did that after one room change and three restores
 * in a row were refused over 32 KB nobody would read. */
static int g_dead_reg[256], g_ndead_reg;

static int dead_reg_has(int i)
{
	int k;

	for (k = 0; k < g_ndead_reg; k++)
		if (g_dead_reg[k] == i)
			return 1;
	return 0;
}

/* D3D9SW_SKIP_SMALL=<KB>: a saved region no bigger than this that cannot go
 * back is left out instead of refusing the load. A gamble - whatever pointed
 * into it now points at someone else's memory, and the fault log will say so. */
static int small_skip(const Slot *s, int i)
{
	char v[16];
	unsigned kb = 0, k;

	if (ss_getenv("D3D9SW_SKIP_SMALL", v, sizeof(v)) <= 0)
		return 0;
	for (k = 0; v[k] >= '0' && v[k] <= '9'; k++)
		kb = kb * 10u + (unsigned)(v[k] - '0');
	if (!kb || s->regs[i].size > (uintptr_t)kb << 10 ||
	    g_ndead_reg >= (int)(sizeof(g_dead_reg) / sizeof(g_dead_reg[0])))
		return 0;
	g_dead_reg[g_ndead_reg++] = i;
	return 1;
}

static int dead_stack_region(const Slot *s, int i, uintptr_t abase, uintptr_t aend)
{
	uintptr_t base = s->regs[i].base, lo = base;
	int k;

	if (base + s->regs[i].size != aend || aend - abase > (16u << 20))
		return 0;
	for (k = 0; k < s->nregs; k++)
		if (s->regs[k].alloc_base == abase && s->regs[k].base < lo)
			lo = s->regs[k].base;
	if (lo - abase < 0x10000)
		return 0;
	for (k = 0; k < s->nthreads; k++) {
		uintptr_t tb = s->threads[k].stack_base, tl = s->threads[k].stack_limit;

		if (tb && tb > abase && tl < aend)
			return 0;
	}
	if (g_ndead_reg >= (int)(sizeof(g_dead_reg) / sizeof(g_dead_reg[0])))
		return 0;
	g_dead_reg[g_ndead_reg++] = i;
	return 1;
}

/* Saved region i lies on a saved thread's stack, and the load is a transplant:
 * that thread's bytes go to the live thread standing in for it, read from the
 * slot, so the old addresses need nothing written. In another process they are
 * often some other allocation by now, and refusing over them protects nothing.
 * Returns the saved tid, or 0. */
static int transplant_mode(void);

static DWORD xs_stack_region(const Slot *s, int i)
{
	uintptr_t base = s->regs[i].base, end = base + s->regs[i].size;
	int k;

	if (!xs_foreign_load() || !transplant_mode() ||
	    g_ndead_reg >= (int)(sizeof(g_dead_reg) / sizeof(g_dead_reg[0])))
		return 0;
	for (k = 0; k < s->nthreads; k++) {
		uintptr_t tb = s->threads[k].stack_base, tl = s->threads[k].stack_limit;

		if (tb && base < tb && end > tl - 0x10000) {
			g_dead_reg[g_ndead_reg++] = i;
			return s->threads[k].tid;
		}
	}
	return 0;
}

/* A transplanted stack's new home is a live thread's stack, which the restore
 * skipped. A saved region can still share its allocation base - the old thread's
 * stack was reserved at the same address - and scanning it both as a restored
 * region and as a new stack would shift its words twice. */
static int xs_on_new_stack(uintptr_t base, uintptr_t size)
{
	int k;

	for (k = 0; k < g_ctl->nxs; k++) {
		uintptr_t lo = (uintptr_t)((intptr_t)g_ctl->xs[k].old_sp + g_ctl->xs[k].delta);
		uintptr_t hi = (uintptr_t)((intptr_t)g_ctl->xs[k].old_hi + g_ctl->xs[k].delta);

		if (base < hi && base + size > lo)
			return 1;
	}
	return 0;
}

static void reloc_apply(const Slot *s)
{
	static RelocEnt map[SS_MAX_MODS + 2 * SS_MAX_THREADS + 2 + 2 * SS_MAX_WND + SS_MAX_STREAMS];
	char v[8];
	unsigned got = ss_getenv("D3D9SW_RELOC", v, sizeof(v));
	unsigned long total = 0, stack_hits = 0, wnd_hits = 0;
	int n = 0, nmod, i, k, scanned = 0, write, nw0, nw1;
	int want = got > 0 && got < sizeof(v) && (v[0] == '1' || v[0] == '2');

	g_rx_map = NULL;
	g_rx_mapn = -1;
	{
		IMAGE_NT_HEADERS *nh =
			(IMAGE_NT_HEADERS *)((unsigned char *)&__ImageBase + __ImageBase.e_lfanew);

		g_reloc_self_lo = (uintptr_t)&__ImageBase;
		g_reloc_self_hi = g_reloc_self_lo + nh->OptionalHeader.SizeOfImage;
	}
	/* The stacks are shifted whatever the knob says: a transplanted thread
	 * with its frame pointers still aimed at the old stack returns into
	 * nothing, so there is no report-only version of that worth having. */
	if (!want && !g_ctl->nxs)
		return;
	write = want && v[0] == '1';
	nmod = want ? reloc_build(s, map, SS_MAX_MODS) : 0;
	if (want && !nmod)
		ss_log("  reloc: no module base moved since the save - nothing to shift "
		       "(%s)\n", prov_name(g_ctl->load_prov));
	/* Nothing can have moved inside one process. A delta here means identity
	 * matched the wrong module, so the shift is clamped to zero and the restore
	 * carries on unrelocated, which is what a same-process restore needs. The
	 * scan still runs, read-only, so the log shows what would have been hit. */
	if (nmod && g_ctl->load_prov == PROV_SAME_PROCESS && write) {
		ss_log("  reloc: TRIPWIRE - %d module(s) look moved but this load is in the "
		       "process that saved. That is an identity bug, not a move: shifting "
		       "NOTHING, and reporting what would have been shifted\n", nmod);
		write = 0;
	}
	for (i = 0; i < nmod; i++)
		map[i].write = write;
	n = nmod;
	/* One pass over modules and stacks together. Two passes would let a word
	 * shifted out of an old stack land in a moved module's old range and be
	 * shifted again. */
	for (k = 0; k < g_ctl->nxs; k++, n++) {
		map[n].lo = g_ctl->xs[k].old_sp;
		map[n].hi = g_ctl->xs[k].old_hi;
		map[n].delta = g_ctl->xs[k].delta;
		map[n].mod = -1;
		map[n].write = 1;
		map[n].hits = 0;
		map[n].sample_at = 0;
		map[n].sample_val = 0;
	}
	nw0 = n;
	if (xs_foreign_load()) {
		const uintptr_t *lw = g_ctl->lwnd;
		char(*lcls)[64] = g_ctl->lwnd_cls;
		int nl = g_ctl->nlwnd, used[SS_MAX_WND] = { 0 }, a, b;

		/* Paired by class, then by order among windows of one class. */
		for (a = 0; a < s->nwnd && a < SS_MAX_WND; a++) {
			for (b = 0; b < nl; b++)
				if (!used[b] && !strcmp(s->wnd_cls[a], lcls[b]))
					break;
			if (b == nl) {
				ss_log("  window: saved %08lX (%s) has no live window of that class\n",
				       (unsigned long)s->wnd[a], s->wnd_cls[a]);
				continue;
			}
			used[b] = 1;
			ss_log("  window: saved %08lX (%s) paired with live %08lX\n",
			       (unsigned long)s->wnd[a], s->wnd_cls[a], (unsigned long)lw[b]);
			if (s->wnd_dc[a]) {
				uintptr_t ldc = wnd_own_dc(lw[b]);

				if (ldc && ldc != s->wnd_dc[a]) {
					map[n].lo = s->wnd_dc[a];
					map[n].hi = s->wnd_dc[a] + 1;
					map[n].delta = (intptr_t)ldc - (intptr_t)s->wnd_dc[a];
					map[n].mod = -5;
					map[n].write = 1;
					map[n].hits = 0;
					map[n].sample_at = 0;
					map[n].sample_val = 0;
					n++;
				}
			}
			if (lw[b] == s->wnd[a])
				continue;
			map[n].lo = s->wnd[a];
			map[n].hi = s->wnd[a] + 1;
			map[n].delta = (intptr_t)lw[b] - (intptr_t)s->wnd[a];
			map[n].mod = -3;
			map[n].write = 1;
			map[n].hits = 0;
			map[n].sample_at = 0;
			map[n].sample_val = 0;
			n++;
		}
		if (!s->nwnd)
			ss_log("  window: the save recorded no windows (older slot), so window "
			       "handles in it are not remapped\n");
		for (a = 0; a < s->nstreams; a++) {
			unsigned long long sz, mt;
			char nm[MAX_PATH];
			void *f;

			if (!WideCharToMultiByte(CP_ACP, 0, s->stream_path[a], -1, nm, sizeof(nm),
						 NULL, NULL))
				nm[0] = 0;
			ss_file_stamp(s->stream_path[a], &sz, &mt);
			if (sz != s->stream_size[a] || mt != s->stream_mtime[a])
				ss_log("  stream: %s CHANGED since the save (%llu -> %llu bytes) - "
				       "swapped or rewritten; reopened anyway so the game can close "
				       "it, but what it reads from here on is not what it was "
				       "reading\n",
				       nm, s->stream_size[a], sz);
			f = gameheap_stream_reopen(s->stream_f[a], s->stream_path[a], s->stream_mode[a],
						   s->stream_pos[a]);
			if (!f) {
				ss_log("  stream: %s would not open again - the game's pointer to "
				       "it leads nowhere\n",
				       nm);
				continue;
			}
			map[n].lo = s->stream_f[a];
			map[n].hi = s->stream_f[a] + 1;
			map[n].delta = (intptr_t)f - (intptr_t)s->stream_f[a];
			map[n].mod = -4;
			map[n].write = 1;
			map[n].hits = 0;
			map[n].sample_at = 0;
			map[n].sample_val = 0;
			n++;
		}
	}
	nw1 = n;
	if (!n)
		return;
	/* Only writable data holds relocatable pointers; code is read-only and its
	 * own fixups were done by the loader. Scan the regions we just restored. */
	for (i = 0; i < s->nregs; i++) {
		DWORD prot = s->regs[i].prot;
		int writable = (prot & (PAGE_READWRITE | PAGE_WRITECOPY |
					PAGE_EXECUTE_READWRITE |
					PAGE_EXECUTE_WRITECOPY)) != 0;
		MEMORY_BASIC_INFORMATION mbi;

		if (!writable)
			continue;
		/* Only a region that came back as its own allocation holds the
		 * snapshot's pointers. One that "belongs elsewhere" now is a
		 * different live allocation at the same address - shifting words in
		 * it that happen to fall in a module's range would corrupt a
		 * stranger, not relocate our data - so it is left alone. The scan
		 * itself is bounded to committed runs, but this keeps it off memory
		 * that was never ours to walk in the first place. */
		if (VirtualQuery((LPCVOID)s->regs[i].base, &mbi, sizeof(mbi)) != sizeof(mbi) ||
		    mbi.State != MEM_COMMIT ||
		    (uintptr_t)mbi.AllocationBase != s->regs[i].alloc_base)
			continue;
		if (g_ctl->nxs && xs_on_new_stack(s->regs[i].base, s->regs[i].size))
			continue;
		/* Held out of a load from another process, so what is there is the
		 * live thread's own - a 64-bit stack carrying the registers the
		 * transplant just set, among others - and not the snapshot's. */
		if (xs_foreign_load() && region_excluded(s->regs[i].base, s->regs[i].size))
			continue;
		total += reloc_scan(s->regs[i].base, (size_t)s->regs[i].size, map, n);
		scanned++;
	}
	/* The transplanted stacks hold the densest set of both kinds: frame links
	 * into themselves, and return addresses into whatever module moved. They
	 * also hold what ntdll keeps in callee-saved registers across a wait - its
	 * own TEB and the PEB - which point at the old process's copies. Those two
	 * are shifted on the moved stacks only: the ranges are small and low, and
	 * restored data has no business naming a TEB. */
	{
		int nstk = n;

		for (k = 0; k < g_ctl->nxs; k++) {
			if (!g_ctl->xs[k].old_teb)
				continue;
			map[n].lo = g_ctl->xs[k].old_teb - 0x2000;
			map[n].hi = g_ctl->xs[k].old_teb + 0x1000;
			map[n].delta = (intptr_t)g_ctl->xs[k].new_teb - (intptr_t)g_ctl->xs[k].old_teb;
			map[n].mod = -2;
			map[n].write = 1;
			map[n].hits = 0;
			map[n].sample_at = 0;
			map[n].sample_val = 0;
			n++;
		}
		if (g_ctl->nxs && g_ctl->xs_peb_old) {
			map[n].lo = g_ctl->xs_peb_old;
			map[n].hi = g_ctl->xs_peb_old + 0x1000;
			map[n].delta = (intptr_t)g_ctl->xs_peb_new - (intptr_t)g_ctl->xs_peb_old;
			map[n].mod = -2;
			map[n].write = 1;
			map[n].hits = 0;
			map[n].sample_at = 0;
			map[n].sample_val = 0;
			n++;
		}
		if (g_ctl->nxs && g_ctl->xs_ctl_old) {
			map[n].lo = g_ctl->xs_ctl_old;
			map[n].hi = g_ctl->xs_ctl_old + sizeof(Control);
			map[n].delta = (intptr_t)g_ctl - (intptr_t)g_ctl->xs_ctl_old;
			map[n].mod = -2;
			map[n].write = 1;
			map[n].hits = 0;
			map[n].sample_at = 0;
			map[n].sample_val = 0;
			n++;
		}
		for (k = 0; k < g_ctl->nxs; k++) {
			uintptr_t lo = (uintptr_t)((intptr_t)g_ctl->xs[k].old_sp +
						   g_ctl->xs[k].delta);

			total += reloc_scan(lo, g_ctl->xs[k].old_hi - g_ctl->xs[k].old_sp, map, n);
		}
		if (n > nstk) {
			unsigned long th = 0;

			for (i = nstk; i < n; i++)
				th += map[i].hits;
			total -= th;
			ss_log("  reloc: %lu word(s) on the moved stacks pointed at the old "
			       "process's TEBs or PEB and were moved to this one's (%d range(s))\n",
			       th, n - nstk);
		}
		n = nstk;
	}
	for (i = 0; i < nmod; i++)
		ss_log("  reloc: %-20s %lu word(s) in its old range%s, first at %p = %08lX\n",
		       s->mod_name[map[i].mod], map[i].hits, write ? " shifted" : "",
		       (void *)map[i].sample_at, (unsigned long)map[i].sample_val);
	for (i = nmod; i < n; i++)
		if (i >= nw0 && i < nw1)
			wnd_hits += map[i].hits;
		else
			stack_hits += map[i].hits;
	total -= wnd_hits;
	for (i = nw0; i < nw1; i++)
		ss_log("  %s: %08lX -> %08lX, %lu copy(ies) of the old %s replaced, first "
		       "at %p\n",
		       map[i].mod == -4 ? "stream" : map[i].mod == -5 ? "dc" : "window",
		       (unsigned long)map[i].lo, (unsigned long)(map[i].lo + map[i].delta),
		       map[i].hits,
		       map[i].mod == -4 ? "FILE pointer"
		       : map[i].mod == -5 ? "device context"
					  : "handle",
		       (void *)map[i].sample_at);
	if (nmod)
		ss_log("  reloc: %lu pointer(s) %s across %d moved module(s), %d region(s) "
		       "scanned - %s\n",
		       total - stack_hits,
		       write ? "shifted" : "WOULD be shifted (nothing written)", nmod,
		       scanned, prov_name(g_ctl->load_prov));
	if (g_ctl->nxs) {
		ss_log("  reloc: %lu pointer(s) into %d transplanted stack(s) shifted, in "
		       "those stacks and in %d restored region(s)\n",
		       stack_hits, g_ctl->nxs, scanned);
		for (i = nmod; i < nw0 && i < nmod + 8; i++)
			ss_log("    stack %08lX..%08lX %+ld: %lu word(s), first at %p = %08lX\n",
			       (unsigned long)map[i].lo, (unsigned long)map[i].hi,
			       (long)map[i].delta, map[i].hits, (void *)map[i].sample_at,
			       (unsigned long)map[i].sample_val);
	}
}

/* Cross-session thread transplant.
 *
 * A slot from another process names threads that no longer exist, so the
 * context loop matches none of them and every live thread runs on over a world
 * rewound underneath it - data-only, which is known to kill the game within a
 * frame. Their stacks cannot simply be put back where they were either: the
 * addresses are other threads' stacks now.
 *
 * So each saved thread is handed to the live thread doing the same work, told
 * apart by entry point and then by order among threads sharing one. The live
 * part of the saved stack goes onto the live thread's stack at the same
 * distance from the top, the registers follow, and every word that pointed into
 * the old stack - frame links, SEH records, pointers to locals, from the stack
 * or from restored memory - is shifted by the difference in reloc_apply.
 *
 * x86 only. The SEH chain head is an x86 TEB field, and the 64-bit build has no
 * cross-session target that needs this yet. On by default for loads from
 * another process; D3D9SW_TRANSPLANT=0 turns it off. */
static int transplant_mode(void)
{
#if defined(_M_IX86) || defined(__i386__)
	char v[8];
	DWORD n = ss_getenv("D3D9SW_TRANSPLANT", v, sizeof(v));

	return (n > 0 && n < sizeof(v) && v[0] == '0') ? 0 : 1;
#else
	return 0;
#endif
}

static int xs_foreign_load(void)
{
	return g_ctl && g_ctl->load_prov != PROV_SAME_PROCESS;
}

static DWORD xs_new_tid(DWORD old_tid)
{
	int k;

	if (!g_ctl || !old_tid)
		return 0;
	for (k = 0; k < g_ctl->nxs; k++)
		if (g_ctl->xs[k].old_tid == old_tid)
			return g_ctl->xs[k].new_tid;
	for (k = 0; k < g_ctl->nkeep; k++)
		if (g_ctl->keep_old[k] == old_tid)
			return g_ctl->keep_new[k];
	return 0;
}

#define SS_SPARE_CAP ((int)(sizeof(g_ctl->spare_tid) / sizeof(g_ctl->spare_tid[0])))

/* Exits once its slot no longer names it. A stand-in that a load takes never
 * gets back here: its registers become the saved thread's. */
static DWORD WINAPI xs_spare_main(LPVOID p)
{
	int k = (int)(uintptr_t)p;
	DWORD me = GetCurrentThreadId();

	InterlockedExchange(&g_ctl->spare_ready[k], 1);
	while (g_ctl->spare_tid[k] == me)
		Sleep(100);
	return 0;
}

static int xs_spare_idx(DWORD tid)
{
	int k;

	if (!g_ctl || !tid)
		return -1;
	for (k = 0; k < SS_SPARE_CAP; k++)
		if (g_ctl->spare_tid[k] == tid)
			return k;
	return -1;
}

/* A saved start address inside our own image: a watchdog or spare from the
 * saving launch, whose frames return into that launch's build of us. */
static int xs_in_self(uintptr_t a)
{
	MEMORY_BASIC_INFORMATION mbi;
	const unsigned char *b;
	const IMAGE_NT_HEADERS *nt;

	if (!VirtualQuery((LPCVOID)xs_spare_main, &mbi, sizeof(mbi)))
		return 0;
	b = (const unsigned char *)mbi.AllocationBase;
	nt = (const IMAGE_NT_HEADERS *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
	return a >= (uintptr_t)b && a < (uintptr_t)b + nt->OptionalHeader.SizeOfImage;
}

/* Before the freeze, so each new thread finishes the loader's thread attach
 * while nothing that could hold the loader lock is suspended. Counts, role by
 * role (entry point), the saved threads no live thread can take, and starts
 * exactly that many. Returns 0 to refuse the load: more than the cap means a
 * misread census, not a game. */
static int xs_spares_topup(const Slot *s)
{
	static PVOID live[SS_MAX_THREADS];
	static char taken[SS_MAX_THREADS];
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	THREADENTRY32 te;
	DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId(), t0;
	int nlive = 0, need = 0, idle = 0, made = 0, i, j, k;

	if (snap == INVALID_HANDLE_VALUE)
		return 1;
	te.dwSize = sizeof(te);
	if (Thread32First(snap, &te)) {
		do {
			HANDLE h;

			if (te.th32OwnerProcessID != pid || te.th32ThreadID == self ||
			    te.th32ThreadID == g_ctl->wd_tid || xs_spare_idx(te.th32ThreadID) >= 0 ||
			    nlive >= SS_MAX_THREADS)
				continue;
			h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
			if (!h)
				continue;
			live[nlive++] = start_of(h);
			CloseHandle(h);
		} while (Thread32Next(snap, &te));
	}
	CloseHandle(snap);
	memset(taken, 0, sizeof(taken));
	for (i = 0; i < s->nthreads; i++) {
		PVOID start = NULL;

		for (k = 0; k < s->nids; k++)
			if (s->ids[k] == s->threads[i].tid)
				start = start_now(s, s->starts[k]);
		if (!start || xs_in_self((uintptr_t)start))
			continue;
		for (j = 0; j < nlive; j++)
			if (!taken[j] && live[j] == start)
				break;
		if (j < nlive)
			taken[j] = 1;
		else
			need++;
	}
	for (k = 0; k < SS_SPARE_CAP; k++)
		if (g_ctl->spare_tid[k])
			idle++;
	if (g_ctl->mf_short >= 0 && g_ctl->mf_short != need)
		ss_log("  manifest: predicted %d rewound thread(s) with no live thread in their "
		       "role, the stand-in count finds %d - the two disagree\n",
		       g_ctl->mf_short, need);
	if (need <= idle)
		return 1;
	if (need > SS_SPARE_CAP) {
		ss_log("  threads: %d saved thread(s) have no live thread in their role, over "
		       "the cap of %d stand-ins - refusing the load\n",
		       need, SS_SPARE_CAP);
		return 0;
	}
	for (k = 0; k < SS_SPARE_CAP && idle + made < need; k++) {
		DWORD tid = 0;
		HANDLE h;

		if (g_ctl->spare_tid[k])
			continue;
		g_ctl->spare_ready[k] = 0;
		h = CreateThread(NULL, 1u << 20, xs_spare_main, (LPVOID)(uintptr_t)k,
				 CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
		if (!h)
			break;
		g_ctl->spare_tid[k] = tid;
		ResumeThread(h);
		CloseHandle(h);
		made++;
	}
	t0 = GetTickCount();
	for (k = 0; k < SS_SPARE_CAP; k++)
		while (g_ctl->spare_tid[k] && !g_ctl->spare_ready[k] && GetTickCount() - t0 < 2000)
			Sleep(1);
	ss_log("  threads: %d saved thread(s) have no live thread in their role - started "
	       "%d stand-in(s), %d already idle\n",
	       need, made, idle);
	return 1;
}

/* After every load, whatever its outcome: stand-ins nobody took exit. */
static void xs_spares_release(void)
{
	int k, n = 0;

	for (k = 0; k < SS_SPARE_CAP; k++)
		if (g_ctl->spare_tid[k]) {
			g_ctl->spare_tid[k] = 0;
			n++;
		}
	if (n)
		ss_log("  threads: %d unused stand-in(s) told to exit\n", n);
}

/* PhysX's task workers are transplanted like any other thread. Left live they
 * carry this launch's per-thread state into the restored scene, and the first
 * room change tore down a list twice. D3D9SW_PHYSX_WORKERS=keep leaves the live
 * ones where they are, with their events paired by role in xs_handles_pin. */
static int xs_keep_live(const char *owner)
{
	static int keep = -1;

	if (keep < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_PHYSX_WORKERS", v, sizeof(v));

		keep = n > 0 && n < sizeof(v) && v[0] == 'k';
	}
	return keep && !_strnicmp(owner, "PhysX", 5);
}

/* 0 for a manual-reset event, 1 for auto-reset, -1 when `h` is not an event. */
static int event_type(HANDLE h)
{
	static LONG(NTAPI * q)(HANDLE, int, PVOID, ULONG, PULONG);
	struct {
		int type;
		LONG state;
	} info;

	if (!q) {
		HMODULE nt = GetModuleHandleA("ntdll.dll");

		q = (LONG(NTAPI *)(HANDLE, int, PVOID, ULONG, PULONG))(void *)(
			nt ? GetProcAddress(nt, "NtQueryEvent") : NULL);
		if (!q)
			return -1;
	}
	if (q(h, 0, &info, sizeof(info), NULL) < 0)
		return -1;
	return info.type;
}

/* The game's events and thread handles, under the values the restored memory
 * holds. Those values came from the other process's handle table; here they
 * name nothing, or this launch's copy of the same object, or something else
 * entirely - and a PhysX worker waiting on one of them never wakes, which is
 * the hang at exit. Each value is made to name an object of the same kind: the
 * event already there when it is one this launch's game made with the same
 * reset type, otherwise a fresh one in the saved state, and for a thread the
 * live thread that took the saved one's place. A value held by anything the
 * game did not create is left alone. */
HANDLE savestate_hx(HANDLE h)
{
	LONG k, n = g_ctl ? g_ctl->nhx : 0;

	for (k = 0; k < n; k++)
		if (g_ctl->hx_old[k] == (uintptr_t)h)
			return g_ctl->hx_new[k];
	return h;
}

/* The game closed a translated value: close its stand-in, not the stranger. */
int savestate_hx_close(HANDLE h)
{
	LONG k, n = g_ctl ? g_ctl->nhx : 0;

	for (k = 0; k < n; k++)
		if (g_ctl->hx_old[k] == (uintptr_t)h) {
			HANDLE w = g_ctl->hx_new[k];

			g_ctl->hx_old[k] = 0;
			CloseHandle(w);
			return 1;
		}
	return 0;
}

/* The live event in the same role as saved event i: made by the same CreateEvent
 * call, the same number of times before it. Both launches build PhysX's task
 * dispatcher the same way, so its workers - left running in the present - and
 * the restored main thread then wait and signal one object. */
static HANDLE g_twin[SS_MAX_EVENTS] SS_PRESENT;

static HANDLE live_twin(const Slot *s, int i)
{
	const EventState *e = &s->events[i];
	int k, ord = 0, seen = 0, cnt;
	DWORD flags;

	if (e->kind == HK_THREAD || e->kind == HK_MUTEX || !e->site || !g_events)
		return NULL;
	for (k = 0; k < i; k++)
		if (s->events[k].site == e->site && s->events[k].kind == e->kind)
			ord++;
	cnt = (int)g_events->n;
	if (cnt > SS_MAX_EVENTS)
		cnt = SS_MAX_EVENTS;
	for (k = 0; k < cnt; k++) {
		HANDLE h = g_events->h[k];
		int j;

		if (!h || g_events->site[k] != e->site || g_events->kind[k] != e->kind ||
		    !GetHandleInformation(h, &flags))
			continue;
		for (j = k + 1; j < cnt && g_events->h[j] != h; j++)
			;
		if (j < cnt)
			continue;
		if (seen++ == ord)
			return h;
	}
	return NULL;
}

/* A new object of the saved one's kind: the live thread standing in for a
 * saved thread, an unowned mutex, or an event in its saved state. */
static HANDLE xs_fresh(const EventState *e, DWORD want_tid)
{
	if (e->kind == HK_THREAD)
		return OpenThread(THREAD_ALL_ACCESS, FALSE, want_tid);
	if (e->kind == HK_MUTEX)
		return CreateMutexA(NULL, FALSE, NULL);
	return CreateEventA(NULL, e->kind != HK_EVENT_AUTO, e->signalled, NULL);
}

static int is_twin(const Slot *s, HANDLE v)
{
	int i;

	for (i = 0; i < s->nevents && i < SS_MAX_EVENTS; i++)
		if (g_twin[i] == v)
			return 1;
	return 0;
}

static void xs_handles_pin(const Slot *s)
{
	int i, kept = 0, made = 0, clash = 0, fail = 0, nothr = 0, said = 0, steered = 0;
	int twins = 0;

	for (i = 0; i < g_ctl->nhx; i++)
		if (g_ctl->hx_old[i])
			CloseHandle(g_ctl->hx_new[i]);
	g_ctl->nhx = 0;
	for (i = 0; i < s->nevents && i < SS_MAX_EVENTS; i++)
		g_twin[i] = live_twin(s, i);
	for (i = 0; i < s->nevents; i++) {
		const EventState *e = &s->events[i];
		HANDLE v = e->h, src = NULL;
		DWORD flags, want_tid = 0;
		const char *why = NULL;
		int thr = e->kind == HK_THREAD, j;

		if (!v)
			continue;
		if (!thr && i < SS_MAX_EVENTS && g_twin[i]) {
			HANDLE t = g_twin[i], d = NULL;

			if (t == v) {
				kept++;
				twins++;
				continue;
			}
			if (!DuplicateHandle(GetCurrentProcess(), t, GetCurrentProcess(), &d, 0,
					     FALSE, DUPLICATE_SAME_ACCESS)) {
				fail++;
				why = "its live twin could not be duplicated";
				goto report;
			}
			twins++;
			if (!GetHandleInformation(v, &flags) && handle_pin(d, v)) {
				if (d != v)
					CloseHandle(d);
				event_note(v, e->kind, 0);
				made++;
				continue;
			}
			if (g_ctl->nhx < SS_MAX_EVENTS) {
				g_ctl->hx_new[g_ctl->nhx] = d;
				g_ctl->hx_old[g_ctl->nhx] = (uintptr_t)v;
				InterlockedIncrement(&g_ctl->nhx);
				steered++;
				continue;
			}
			CloseHandle(d);
			fail++;
			why = "the steering table is full";
			goto report;
		}
		if (thr && !(want_tid = xs_new_tid(e->tid))) {
			nothr++;
			why = "its thread has no live stand-in";
			goto report;
		}
		if (GetHandleInformation(v, &flags)) {
			for (j = (int)g_events->n - 1; j >= 0; j--)
				if (j < SS_MAX_EVENTS && g_events->h[j] == v)
					break;
			if (thr ? GetThreadId(v) == want_tid
				: (j >= 0 && !is_twin(s, v) && e->kind != HK_MUTEX &&
				   event_type(v) == (e->kind == HK_EVENT_AUTO))) {
				kept++;
				continue;
			}
			/* A live mutex may be owned by a thread the restore just handed
			 * a saved stack, which will never release it: a mutex is always
			 * a fresh one, and a held value is never closed. */
			if (j < 0 || is_twin(s, v) || e->kind == HK_MUTEX) {
				HANDLE w = xs_fresh(e, want_tid);

				clash++;
				if (w && g_ctl->nhx < SS_MAX_EVENTS) {
					g_ctl->hx_new[g_ctl->nhx] = w;
					g_ctl->hx_old[g_ctl->nhx] = (uintptr_t)v;
					InterlockedIncrement(&g_ctl->nhx);
					steered++;
					continue;
				}
				if (w)
					CloseHandle(w);
				why = "the value names something the game did not create here";
				goto report;
			}
			/* This launch's own, and no restored memory names it now. */
			CloseHandle(v);
		}
		src = xs_fresh(e, want_tid);
		if (!src) {
			fail++;
			why = "the object to give it could not be made";
			goto report;
		}
		if (!handle_pin(src, v)) {
			CloseHandle(src);
			fail++;
			why = "the handle table would not hand out that value";
			goto report;
		}
		if (src != v)
			CloseHandle(src);
		event_note(v, e->kind, want_tid);
		made++;
		continue;
report:
		if (said++ < 8)
			ss_log("  handles: %s %p (saved tid %lu): not pinned - %s\n",
			       thr ? "thread" : e->kind == HK_MUTEX ? "mutex" : "event", v,
			       (unsigned long)e->tid, why);
	}
	ss_log("  handles: %d saved event/thread value(s) - %d already right here, %d made "
	       "to name a fresh object, %d held by something else, %d failed, %d thread(s) "
	       "with no live stand-in; %d steered to a stand-in in the game's calls; %d "
	       "paired with this launch's event in the same role\n",
	       s->nevents, kept, made, clash, fail, nothr, steered, twins);
}

/* Why this slot cannot be transplanted, or NULL if it can. Asked before a byte
 * is written, so a refusal leaves the game running as it was. */
static const char *xs_unready(const Slot *s)
{
	DWORD build = ss_build_id();
	int i;

	for (i = 0; i < s->nthreads; i++) {
		if (!s->threads[i].stack_base)
			continue;
		if (s->threads[i].xs_mark != XS_MARK)
			return "the slot predates the thread transplant - save again with "
			       "this build";
		if (s->threads[i].build_id != build)
			return "the slot was saved by a different build of this DLL, and its "
			       "stacks return into that build's code - save again with this "
			       "build";
	}
	return NULL;
}

#if defined(_M_IX86) || defined(__i386__)
/* Saved bytes for [a, a+n) out of the slot, across however many captured
 * regions the range spans. 0 if any part of it was not captured. */
static int xs_fetch(const Slot *s, Window *w, uintptr_t a, void *dst, size_t n)
{
	unsigned char *d = (unsigned char *)dst;

	while (n) {
		unsigned long long off = 0;
		size_t take = 0;
		int i, found = 0;

		for (i = 0; i < s->nregs; i++) {
			uintptr_t b = s->regs[i].base, z = s->regs[i].size;

			if (a >= b && a < b + z) {
				off += a - b;
				take = (size_t)(b + z - a);
				found = 1;
				break;
			}
			off += z;
		}
		if (!found)
			return 0;
		if (take > n)
			take = n;
		if (d && !win_read(w, off, d, take))
			return 0;
		if (d)
			d += take;
		a += take;
		n -= take;
	}
	return 1;
}

static void xs_shift_reg(DWORD *r)
{
	int k;

	for (k = 0; k < g_ctl->nxs; k++)
		if (*r >= g_ctl->xs[k].old_sp && *r < g_ctl->xs[k].old_hi) {
			*r = (DWORD)((intptr_t)*r + g_ctl->xs[k].delta);
			return;
		}
	for (k = 0; k < g_ctl->nxs; k++) {
		const XsMap *x = &g_ctl->xs[k];

		if (x->old_teb && *r >= x->old_teb - 0x2000 && *r < x->old_teb + 0x1000) {
			*r = (DWORD)(*r - x->old_teb + x->new_teb);
			return;
		}
	}
	if (g_ctl->xs_peb_old && *r >= g_ctl->xs_peb_old && *r < g_ctl->xs_peb_old + 0x1000) {
		*r = (DWORD)(*r - g_ctl->xs_peb_old + g_ctl->xs_peb_new);
		return;
	}
	if (g_ctl->xs_ctl_old && *r >= g_ctl->xs_ctl_old &&
	    *r < g_ctl->xs_ctl_old + sizeof(Control))
		*r = (DWORD)(*r - g_ctl->xs_ctl_old + (uintptr_t)g_ctl);
}

/* The last SEH record of a chain whose links still use the addresses of
 * [lo, hi), read at lo + delta; 0 if the chain leaves that range or loops. */
static DWORD *xs_seh_last(DWORD head, uintptr_t lo, uintptr_t hi, intptr_t delta)
{
	DWORD rec = head;
	int n;

	for (n = 0; n < 4096; n++) {
		DWORD *p;

		if (rec < lo || rec + 8 > hi || (rec & 3))
			return NULL;
		p = (DWORD *)((intptr_t)rec + delta);
		if (p[0] == 0xFFFFFFFFu)
			return p;
		if (p[0] <= rec)
			return NULL;
		rec = p[0];
	}
	return NULL;
}

/* Stack words that carry a /GS cookie, found on the transplanted stacks before
 * the pointer shift can touch them and written after it.
 *
 * A protected frame stores cookie ^ frame pointer; an SEH4 frame also stores
 * its scope table ^ cookie. The frame pointer moved with the stack, and a
 * module left in the present drew a new cookie in this process, so both forms
 * are decoded with the module's saved cookie and encoded again with the live
 * one. A decode is only believed when it lands on this very stack within 64 KB
 * of the word, or inside the module's own image. */
/* Whether p is an address the restore just put back from the slot. */
static int xs_rewound_ptr(const Slot *s, uintptr_t p)
{
	int i;

	for (i = 0; i < s->nregs; i++)
		if (p >= s->regs[i].base && p - s->regs[i].base < s->regs[i].size)
			return !region_excluded(s->regs[i].base, s->regs[i].size);
	return 0;
}

/* Whether p is an object the restore leaves in the present: a block in a heap
 * we do not rewind, or inside a module image we hold. */
static int xs_held_ptr(uintptr_t p)
{
	int h;

	if (p < 0x10000)
		return 0;
	h = heap_index_of(p);
	if (h >= 0)
		return !g_ctl->heap_ours[h];
	return held_mod_at(p, p + 1) >= 0;
}

static int img_merge_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_IMGMERGE", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '0') ? 0 : 1;
	}
	return cached;
}

/* The game's globals, merged the way TLS is. The executable loads at the same
 * address in every launch, so a global holds the same variable on both sides,
 * but one that names a system object - a DirectInput device, a COM interface,
 * a block in the process heap - names the old process's copy in the save,
 * which does not exist here. The live process made its own, and its global
 * already points at it. So the saved word wins unless it points outside the
 * memory being put back while the live word points at something held. */
static int g_imgm_took, g_imgm_named, g_heapm_took, g_heapm_named;

static int heap_merge_mode(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_HEAPMERGE", v, sizeof(v));

		/* Off unless asked for: its first run rewrote words in our own
		 * allocator's header and the game died on the first frame. */
		cached = (n > 0 && n < sizeof(v) && v[0] == '1') ? 1 : 0;
	}
	return cached;
}

/* Inside a heap the restore leaves in the present. */
static int xs_held_heap_ptr(uintptr_t p)
{
	int h = p >= 0x10000 ? heap_index_of(p) : -1;

	return h >= 0 && !g_ctl->heap_ours[h];
}

/* The same merge over the rest of the restored memory, held to a narrower
 * rule: an address in the game's heap is not the same variable in both launches
 * the way a global is, so only a word that pointed into a held heap in the save
 * and does again now is taken to be the same reference. */
static void heap_merge(uintptr_t base, const uintptr_t *live, size_t size)
{
	uintptr_t *w = (uintptr_t *)base;
	size_t k, n = size / sizeof(uintptr_t);

	for (k = 0; k < n; k++) {
		uintptr_t sv = w[k], lv = live[k];
		int h;

		if (sv == lv || !xs_held_heap_ptr(sv) || !xs_held_heap_ptr(lv))
			continue;
		w[k] = lv;
		g_heapm_took++;
		if (g_heapm_named++ < 16) {
			h = heap_index_of(lv);
			ss_log("    heap word %p: saved %p, kept live %p (%s +%X)\n",
			       (void *)(base + k * sizeof(uintptr_t)), (void *)sv, (void *)lv,
			       g_ctl->heap_name[h], (unsigned)(lv - g_ctl->heap_lo[h]));
		}
	}
}

static void img_merge(const Slot *s, uintptr_t base, const uintptr_t *live, size_t size)
{
	uintptr_t *w = (uintptr_t *)base;
	size_t k, n = size / sizeof(uintptr_t);

	for (k = 0; k < n; k++) {
		uintptr_t sv = w[k], lv = live[k];

		if (sv == lv || sv < 0x10000 || !xs_held_ptr(lv) || xs_rewound_ptr(s, sv) ||
		    follows_save(sv))
			continue;
		w[k] = lv;
		g_imgm_took++;
		if (g_imgm_named++ < 12) {
			unsigned moff = 0;
			const char *mod = ss_module(lv, &moff);
			int h = heap_index_of(lv);

			ss_log("    global %p: saved %p, kept live %p (%s%s%X)\n",
			       (void *)(base + k * sizeof(uintptr_t)), (void *)sv, (void *)lv,
			       mod ? mod : (h >= 0 ? g_ctl->heap_name[h] : "held"),
			       mod ? "+" : " +", mod ? moff : (unsigned)(lv - (h >= 0 ? g_ctl->heap_lo[h] : lv)));
		}
	}
}

/* A TLS slot belongs to whichever module allocated its index, and that is not
 * recorded anywhere we can read. What it points at is: the game's slots name
 * memory that came back with the game, while a system module's - MSCTF, IMM32,
 * the COM runtime - name its own heap, held in the present, where the saved
 * value is a stranger's object from the other process. So the saved value
 * wins only when it points into restored memory, or when neither side is a
 * pointer at all; otherwise the live thread keeps its own. */
static void xs_tls_merge(const Slot *s, void **live, void *const *saved, int *took,
			 int *kept)
{
	int i;

	for (i = 0; i < TLS_MINIMUM_SLOTS; i++) {
		uintptr_t sv = (uintptr_t)saved[i], lv = (uintptr_t)live[i];
		int sptr = sv >= 0x10000, lptr = lv >= 0x10000;

		if (sv == lv)
			continue;
		if ((sptr && xs_rewound_ptr(s, sv) && (!lptr || xs_rewound_ptr(s, lv))) ||
		    (!sptr && !lptr)) {
			live[i] = saved[i];
			(*took)++;
		} else {
			(*kept)++;
		}
	}
}

#define XS_CK_CAP 4096
static uintptr_t g_xs_ck_at[XS_CK_CAP];
static DWORD g_xs_ck_val[XS_CK_CAP];
static int g_xs_ck_n;

/* An encoded SEH4 scope table sits between the frame's handler and its try
 * level: [handler][scope ^ cookie][trylevel]. Without that shape around it, a
 * word that happens to XOR into a module is a return address or data, and
 * "re-encoding" it wrecks it. */
static int xs_seh4_shape(const Slot *s, uintptr_t a, uintptr_t start, uintptr_t end)
{
	DWORD h, tl;
	int m;

	if (a < start + 4 || a + 8 > end)
		return 0;
	h = *(const DWORD *)(a - 4);
	tl = *(const DWORD *)(a + 4);
	if (tl != 0xFFFFFFFEu && tl >= 0x100)
		return 0;
	for (m = 0; m < s->nmods; m++)
		if (h >= s->mod_lo[m] && h < s->mod_hi[m])
			return 1;
	return 0;
}

static int xs_repeated(DWORD w, uintptr_t start, uintptr_t end, uintptr_t not_at)
{
	uintptr_t a;

	for (a = start; a + 4 <= end; a += 4)
		if (a != not_at && *(const DWORD *)a == w)
			return 1;
	return 0;
}

static void xs_cookies_find(const Slot *s)
{
	static uintptr_t live_ck[SS_MAX_MODS], live_lo[SS_MAX_MODS];
	int m, j, k, nmods = 0, frames = 0, scopes = 0, changed = 0, plain = 0, ptrs = 0;

	g_xs_ck_n = 0;
	for (m = 0; m < s->nmods; m++) {
		live_ck[m] = 0;
		live_lo[m] = 0;
		if (!s->mod_cookie[m])
			continue;
		for (j = 0; j < g_ctl->nmods; j++)
			if (!lstrcmpiA(s->mod_name[m], g_ctl->mod_name[j]) &&
			    s->mod_isize[m] == g_ctl->mod_isize[j]) {
				/* Whatever is in memory now: a rewound module's cookie
				 * comes back as saved, but a merge may take its data and
				 * not the word the cookie lives in. */
				live_ck[m] = mod_gs_cookie(g_ctl->mod_lo[j]);
				if (live_ck[m] == s->mod_cookie[m] && g_ctl->mod_lo[j] == s->mod_lo[m])
					live_ck[m] = 0;
				live_lo[m] = g_ctl->mod_lo[j];
				break;
			}
		if (live_ck[m])
			nmods++;
	}
	for (k = 0; k < g_ctl->nxs; k++) {
		const XsMap *x = &g_ctl->xs[k];
		uintptr_t a = (uintptr_t)((intptr_t)x->old_sp + x->delta) & ~(uintptr_t)3;
		uintptr_t end = (uintptr_t)((intptr_t)x->old_hi + x->delta);
		uintptr_t start = a;

		for (; a + 4 <= end; a += 4) {
			DWORD w = *(const DWORD *)a;
			uintptr_t olda = (uintptr_t)((intptr_t)a - x->delta);

			/* A word that already points into the stack is a saved frame
			 * pointer or SEH link. Against a cookie with its top half zero
			 * it also passes the test below, and re-encoding it scrambled
			 * DDPR's main-thread ebp and exception chain. */
			if ((w >= x->old_sp && w < x->old_hi) ||
			    (w >= a && w < end)) {
				plain++;
				continue;
			}
			for (m = 0; m < s->nmods; m++) {
				uintptr_t v;
				DWORD nw;

				if (!live_ck[m])
					continue;
				v = (uintptr_t)(w ^ (DWORD)s->mod_cookie[m]);
				if (v >= x->old_sp && v < x->old_hi && v + 0x10000 > olda &&
				    v < olda + 0x10000 && xs_rewound_ptr(s, w) &&
				    xs_repeated(w, start, end, a)) {
					/* A pinned pointer, e.g. DDPR's device, that XORs
					 * near the stack by chance under some module's
					 * cookie. Re-encoding it hands the code garbage. A
					 * cookie is a one-off; a pointer passed down a call
					 * chain sits in several frames. */
					if (ptrs++ < 4)
						ss_log("  transplant: %08lX at %08lX decodes to %08lX "
						       "under %s's cookie but is a pointer into "
						       "restored memory repeated on the stack - left "
						       "alone\n",
						       (unsigned long)w, (unsigned long)a,
						       (unsigned long)v, s->mod_name[m]);
					break;
				}
				if (v >= x->old_sp && v < x->old_hi && v + 0x10000 > olda &&
				    v < olda + 0x10000) {
					nw = (DWORD)live_ck[m] ^ (DWORD)((intptr_t)v + x->delta);
					frames++;
				} else if (live_ck[m] != s->mod_cookie[m] && v >= s->mod_lo[m] &&
					   v < s->mod_hi[m] && xs_seh4_shape(s, a, start, end)) {
					nw = (DWORD)live_ck[m] ^ (DWORD)(v - s->mod_lo[m] + live_lo[m]);
					scopes++;
				} else
					continue;
				if (nw != w) {
					changed++;
					if (changed <= 12)
						ss_log("    re-encode %08lX at %08lX: %08lX -> %08lX, "
						       "%s's cookie\n",
						       (unsigned long)w, (unsigned long)a,
						       (unsigned long)w, (unsigned long)nw,
						       s->mod_name[m]);
				}
				if (g_xs_ck_n < XS_CK_CAP) {
					g_xs_ck_at[g_xs_ck_n] = a;
					g_xs_ck_val[g_xs_ck_n++] = nw;
				}
				break;
			}
		}
	}
	ss_log("  transplant: %d /GS frame cookie(s) and %d SEH scope pointer(s) on the moved "
	       "stacks, %d to re-encode, against %d module cookie(s); %d plain stack "
	       "pointer(s) passed over, %d pointer(s) that only looked like a cookie%s\n",
	       frames, scopes, changed, nmods, plain, ptrs,
	       g_xs_ck_n >= XS_CK_CAP ? " - HIT THE CAP, the rest are left as they are" : "");
}

static void xs_cookies_write(void)
{
	int i;

	for (i = 0; i < g_xs_ck_n; i++)
		*(DWORD *)g_xs_ck_at[i] = g_xs_ck_val[i];
	g_xs_ck_n = 0;
}
#endif

/* Whose thread this is: the first module on the stack, nearest the top, that is
 * neither Windows nor the C runtime. Nearly every thread starts in ucrtbase's
 * thread_start, so the entry alone paired a saved Steam thread with a live game
 * worker and an OpenAL thread with another, and both ran straight into data. */
static int xs_owner_skip(const char *path, const char *name)
{
	static char windir[MAX_PATH];
	static UINT n;

	if (!n) {
		n = GetWindowsDirectoryA(windir, sizeof(windir));
		if (n >= sizeof(windir))
			n = 0;
	}
	return (n && _strnicmp(path, windir, n) == 0) || !_strnicmp(name, "msvcp", 5) ||
	       !_strnicmp(name, "vcruntime", 9) || !_strnicmp(name, "concrt", 6) ||
	       !_strnicmp(name, "ucrtbase", 8) || !_strnicmp(name, "api-ms-", 7);
}

static void xs_owner_saved(const Slot *s, Window *w, const ThreadState *t, char *out)
{
	static uintptr_t buf[1024];
	uintptr_t sp = t->ctx.Esp, n = t->stack_base > sp ? t->stack_base - sp : 0;
	size_t i;
	int k;

	out[0] = 0;
	if (n > sizeof(buf))
		n = sizeof(buf);
	if (!n || !xs_fetch(s, w, sp, buf, n))
		return;
	for (i = 0; i < n / sizeof(uintptr_t); i++)
		for (k = 0; k < s->nmods; k++)
			if (buf[i] >= s->mod_lo[k] && buf[i] < s->mod_hi[k]) {
				if (xs_owner_skip(s->mod_path[k], s->mod_name[k]))
					break;
				lstrcpynA(out, s->mod_name[k], 32);
				return;
			}
}

static void xs_owner_live(int j, char *out)
{
	CONTEXT c;
	unsigned char *teb = (unsigned char *)teb_of(g_ctl->handles[j]);
	uintptr_t base = teb ? *(const uintptr_t *)(teb + TIB_STACK_BASE) : 0, a, end;
	int k;

	out[0] = 0;
	memset(&c, 0, sizeof(c));
	c.ContextFlags = CONTEXT_CONTROL;
	if (!base || !GetThreadContext(g_ctl->handles[j], &c) || c.Esp >= base)
		return;
	end = c.Esp + 4096 < base ? c.Esp + 4096 : base;
	for (a = c.Esp & ~(uintptr_t)3; a + 4 <= end; a += 4) {
		uintptr_t v = *(const uintptr_t *)a;

		for (k = 0; k < g_ctl->nmods; k++)
			if (v >= g_ctl->mod_lo[k] && v < g_ctl->mod_hi[k]) {
				if (xs_owner_skip(g_ctl->mod_path[k], g_ctl->mod_name[k]))
					break;
				lstrcpynA(out, g_ctl->mod_name[k], 32);
				return;
			}
	}
}

/* Runs where the context loop would, with every thread suspended and the memory
 * already written. Returns the number of TLS blocks put back. */
static int xs_transplant(Slot *s)
{
#if defined(_M_IX86) || defined(__i386__)
	static char used[SS_MAX_THREADS];
	static int pair[SS_MAX_THREADS], kof[SS_MAX_THREADS], pre[SS_MAX_THREADS];
	static char sown[SS_MAX_THREADS][32], lown[SS_MAX_THREADS][32];
	Window w;
	int i, j, k, tls = 0, set = 0, nomatch = 0, nofit = 0, nobytes = 0, refused = 0;
	int present = 0, spared = 0, pass;
	int tls_took = 0, tls_kept = 0;
	const uintptr_t pg = 0x1000;

	memset(used, 0, sizeof(used));
	memset(&w, 0, sizeof(w));
	g_ctl->nkeep = 0;
	w.sect = s->sect;
	w.total = s->bytes;
	g_ctl->xs_peb_new = peb_base();
	g_ctl->xs_peb_old = s->save_peb != g_ctl->xs_peb_new ? s->save_peb : 0;
	/* A thread parked inside our own code at the save - the main thread
	 * waiting in request() for the helper - holds the old process's Control
	 * address in a register and in its frames. Control is ours and moves
	 * with every launch, so it is remapped like the PEB. */
	g_ctl->xs_ctl_old = s->save_ctl != (uintptr_t)g_ctl ? s->save_ctl : 0;

	/* Pair first and move nothing, so every stack's delta is known before any
	 * copy: a pointer from one thread's stack into another's is common - a
	 * worker handed a pointer to its caller's local - and has to be shifted by
	 * the target stack's delta, not its own. */
	for (i = 0; i < s->nthreads && i < SS_MAX_THREADS; i++)
		xs_owner_saved(s, &w, &s->threads[i], sown[i]);
	for (j = 0; j < g_ctl->nids && j < SS_MAX_THREADS; j++) {
		lown[j][0] = 0;
		if (g_ctl->handles[j] && g_did_suspend[j] && !g_ctl->transient[j])
			xs_owner_live(j, lown[j]);
	}
	/* Owner is the first module on the stack, so it says what a worker was
	 * doing at that instant, not what it is: a pool worker mid-job at the save
	 * and idle now names different modules. Pair same-owner first, then any
	 * live thread with the same entry point. */
	for (i = 0; i < s->nthreads && i < SS_MAX_THREADS; i++)
		pre[i] = -1;
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < s->nthreads && i < SS_MAX_THREADS; i++) {
			PVOID start = NULL;

			if (pre[i] >= 0 || !s->threads[i].stack_base)
				continue;
			for (k = 0; k < s->nids; k++)
				if (s->ids[k] == s->threads[i].tid)
					start = start_now(s, s->starts[k]);
			if (!start || xs_in_self((uintptr_t)start))
				continue;
			if (sown[i][0]) {
				for (k = 0; k < g_ctl->nmods; k++)
					if (!lstrcmpiA(g_ctl->mod_name[k], sown[i]))
						break;
				if (k == g_ctl->nmods || !g_ctl->mod_rewound[k])
					continue;
			}
			for (j = 0; j < g_ctl->nids && j < SS_MAX_THREADS; j++) {
				DWORD ec;

				if (used[j] || !g_ctl->handles[j] || !g_did_suspend[j] ||
				    g_ctl->transient[j] || g_ctl->starts[j] != start ||
				    (pass == 0 && lstrcmpiA(lown[j], sown[i])))
					continue;
				if (!GetExitCodeThread(g_ctl->handles[j], &ec) || ec != STILL_ACTIVE ||
				    !teb_of(g_ctl->handles[j]))
					continue;
				pre[i] = j;
				used[j] = 1;
				break;
			}
		}
	memset(used, 0, sizeof(used));
	for (i = 0; i < s->nthreads; i++) {
		ThreadState *t = &s->threads[i];
		PVOID start = NULL;

		pair[i] = -1;
		for (k = 0; k < s->nids; k++)
			if (s->ids[k] == t->tid)
				start = start_now(s, s->starts[k]);
		if (!start || !t->stack_base)
			continue;
		/* A thread of a module left in the present has its own live thread
		 * and its own live memory; its saved frames return into wherever that
		 * module sat in the saving launch. */
		if (sown[i][0]) {
			for (k = 0; k < g_ctl->nmods; k++)
				if (!lstrcmpiA(g_ctl->mod_name[k], sown[i]))
					break;
			if (k == g_ctl->nmods || !g_ctl->mod_rewound[k]) {
				ss_log("  transplant: saved tid %lu is %s's, left in the present - "
				       "not moved\n",
				       (unsigned long)t->tid, sown[i]);
				present++;
				continue;
			}
		}
		if (i < SS_MAX_THREADS && pre[i] >= 0) {
			pair[i] = pre[i];
			used[pair[i]] = 1;
		}
		if (pair[i] < 0 && xs_in_self((uintptr_t)start)) {
			ss_log("  transplant: saved tid %lu is one of ours (watchdog or spare), "
			       "not moved\n",
			       (unsigned long)t->tid);
			present++;
			continue;
		}
		for (j = 0; pair[i] < 0 && j < g_ctl->nids; j++) {
			int sp = xs_spare_idx(g_ctl->ids[j]);

			if (sp < 0 || used[j] || !g_ctl->handles[j] || !g_did_suspend[j] ||
			    !teb_of(g_ctl->handles[j]))
				continue;
			pair[i] = j;
			used[j] = 1;
			g_ctl->spare_tid[sp] = 0;
			if (g_ctl->nspare_taken < SS_SPARE_CAP)
				g_ctl->spare_taken[g_ctl->nspare_taken++] = g_ctl->ids[j];
			spared++;
			ss_log("  transplant: saved tid %lu (%s) has no live thread in its role - "
			       "spare thread %lu takes it\n",
			       (unsigned long)t->tid, sown[i][0] ? sown[i] : "game",
			       (unsigned long)g_ctl->ids[j]);
		}
		if (pair[i] < 0) {
			nomatch++;
			continue;
		}
		if (xs_keep_live(sown[i]) && g_ctl->nkeep < SS_MAX_THREADS) {
			DWORD nt = GetThreadId(g_ctl->handles[pair[i]]);

			g_ctl->keep_old[g_ctl->nkeep] = t->tid;
			g_ctl->keep_new[g_ctl->nkeep++] = nt;
			ss_log("  transplant: saved tid %lu is %s's worker - live tid %lu takes its "
			       "place where it already is, stack not moved\n",
			       (unsigned long)t->tid, sown[i], (unsigned long)nt);
			pair[i] = -1;
			present++;
			continue;
		}
		{
			unsigned char *teb = (unsigned char *)teb_of(g_ctl->handles[pair[i]]);
			uintptr_t nb = teb ? *(const uintptr_t *)(teb + TIB_STACK_BASE) : 0;
			uintptr_t sp = t->ctx.Esp;
			MEMORY_BASIC_INFORMATION mbi;

			if (!nb || sp < t->stack_limit || sp >= t->stack_base ||
			    VirtualQuery((LPCVOID)(nb - 1), &mbi, sizeof(mbi)) != sizeof(mbi) ||
			    ((nb - (t->stack_base - sp)) & ~(pg - 1)) <
				    (uintptr_t)mbi.AllocationBase + 4 * pg) {
				used[pair[i]] = 0;
				pair[i] = -1;
				nofit++;
				continue;
			}
			if (!xs_fetch(s, &w, sp, NULL, t->stack_base - sp)) {
				used[pair[i]] = 0;
				pair[i] = -1;
				nobytes++;
				continue;
			}
			k = g_ctl->nxs++;
			kof[i] = k;
			g_ctl->xs[k].state = 0;
			g_ctl->xs[k].live_seh = teb ? *(const DWORD *)teb : 0xFFFFFFFFu;
			g_ctl->xs[k].old_teb = t->teb != (uintptr_t)teb ? t->teb : 0;
			g_ctl->xs[k].new_teb = (uintptr_t)teb;
			g_ctl->xs[k].old_tid = t->tid;
			g_ctl->xs[k].new_tid = g_ctl->ids[pair[i]];
			g_ctl->xs[k].old_sp = sp;
			g_ctl->xs[k].old_hi = t->stack_base;
			g_ctl->xs[k].delta = (intptr_t)nb - (intptr_t)t->stack_base;
		}
	}

	for (i = 0; i < s->nthreads; i++) {
		ThreadState *t = &s->threads[i];
		HANDLE h;
		unsigned char *teb;
		uintptr_t nb, nl, nsp, low;
		intptr_t delta;
		CONTEXT c;
		DWORD old;

		if (pair[i] < 0)
			continue;
		h = g_ctl->handles[pair[i]];
		teb = (unsigned char *)teb_of(h);
		nb = *(const uintptr_t *)(teb + TIB_STACK_BASE);
		nl = *(const uintptr_t *)(teb + TIB_STACK_LIMIT);
		delta = (intptr_t)nb - (intptr_t)t->stack_base;
		nsp = (uintptr_t)((intptr_t)t->ctx.Esp + delta);
		/* One page of room below the saved SP, then a guard page, the way
		 * the kernel would have left it had the thread grown there itself.
		 * The old guard page falls inside the new committed range, so the
		 * protect clears its guard bit rather than leaving a trap in the
		 * middle of the stack. */
		low = (nsp & ~(pg - 1)) - pg;
		if (low < nl) {
			VirtualAlloc((void *)low, nl - low, MEM_COMMIT, PAGE_READWRITE);
			VirtualProtect((void *)low, nl - low, PAGE_READWRITE, &old);
			VirtualAlloc((void *)(low - pg), pg, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD);
			*(uintptr_t *)(teb + TIB_STACK_LIMIT) = low;
		}
		{
			/* SEHOP ends every chain at a FinalExceptionHandlerPad that each
			 * process picks for itself; the copied chain ends at the saving
			 * process's, and the first exception dispatched after the load
			 * (OutputDebugString raises one) fast-fails with 0x15. */
			DWORD *lf = xs_seh_last(*(const DWORD *)teb, nl < low ? nl : low, nb, 0);
			DWORD fin = lf ? lf[1] : 0, *sf;

			if (!xs_fetch(s, &w, t->ctx.Esp, (void *)nsp, t->stack_base - t->ctx.Esp)) {
				nobytes++;
				continue;
			}
			sf = t->seh_head != 0xFFFFFFFFu ?
				     xs_seh_last(t->seh_head, t->ctx.Esp, t->stack_base, delta) : NULL;
			if (fin && sf && sf[1] != fin) {
				ss_log("  transplant: thread %lu - the SEH chain's final handler "
				       "%08lX is the saving process's; this one's is %08lX\n",
				       (unsigned long)g_ctl->ids[pair[i]], (unsigned long)sf[1],
				       (unsigned long)fin);
				sf[1] = fin;
			}
		}
		c = t->ctx;
		c.ContextFlags = CONTEXT_FULL;
		xs_shift_reg(&c.Esp);
		xs_shift_reg(&c.Ebp);
		xs_shift_reg(&c.Eax);
		xs_shift_reg(&c.Ebx);
		xs_shift_reg(&c.Ecx);
		xs_shift_reg(&c.Edx);
		xs_shift_reg(&c.Esi);
		xs_shift_reg(&c.Edi);
		g_ctl->xs[kof[i]].want = c;
		g_ctl->xs[kof[i]].state = 2;
		if (!SetThreadContext(h, &c)) {
			refused++;
			ss_log("  transplant: thread %lu refused the context (err %lu) - it "
			       "keeps its own registers over a transplanted stack\n",
			       (unsigned long)g_ctl->ids[pair[i]], GetLastError());
			continue;
		}
		g_ctl->xs[kof[i]].state = 1;
		set++;
		{
			DWORD seh = t->seh_head;

			if (seh != 0xFFFFFFFFu)
				xs_shift_reg(&seh);
			*(DWORD *)teb = seh;
		}
		if (t->have_tls) {
			xs_tls_merge(s, (void **)(teb + TEB_TLS_SLOTS), t->tls, &tls_took,
				     &tls_kept);
			tls++;
		}
		if (i < 6 || g_ctl->ids[pair[i]] == g_ctl->req_tid)
			ss_log("  transplant: saved tid %lu -> live tid %lu%s, %lu byte(s) of "
			       "stack, %08lX..%08lX -> %08lX..%08lX\n",
			       (unsigned long)t->tid, (unsigned long)g_ctl->ids[pair[i]],
			       g_ctl->ids[pair[i]] == g_ctl->req_tid ? " [REQ]" : "",
			       (unsigned long)(t->stack_base - t->ctx.Esp),
			       (unsigned long)t->ctx.Esp, (unsigned long)t->stack_base,
			       (unsigned long)nsp, (unsigned long)nb);
	}
	win_close(&w);
	xs_cookies_find(s);
	ss_log("  transplant: TLS - %d slot(s) taken from the save, %d kept live because "
	       "they belong to memory the restore left in the present\n",
	       tls_took, tls_kept);
	ss_log("  transplant: %d of %d saved thread(s) now run on a live thread - %d "
	       "with no live thread in the same role, %d whose stack would not fit, %d "
	       "whose stack was not in the slot, %d refused the context, %d belong to "
	       "a module left in the present\n",
	       set, s->nthreads, nomatch, nofit, nobytes, refused, present);
	if (spared)
		ss_log("  transplant: %d of those run on a spare thread started for the "
		       "purpose\n",
		       spared);
	return tls;
#else
	(void)s;
	return 0;
#endif
}

#if defined(_M_IX86) || defined(__i386__)
/* Thread IDs the game keeps in its own image data. It records its main
 * thread's ID once at startup (rabiribi+25EA8) and gates the message pump's
 * loop and the keyboard update on GetCurrentThreadId() matching it, so after a
 * load from another process every one of those checks fails and input dies.
 * The main thread's saved ID is rewritten to the live thread now wearing its
 * stack. IDs are small numbers and any word could hold one by chance, so the
 * other threads' IDs are only counted, and only the image is searched. */
static void xs_tid_remap(void)
{
	unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
	IMAGE_NT_HEADERS *nt;
	IMAGE_SECTION_HEADER *sec;
	int k, n, fixed = 0, other = 0;
	DWORD main_old = 0, main_new = 0;

	for (k = 0; k < g_ctl->nxs; k++)
		if (g_ctl->xs[k].new_tid == g_ctl->req_tid && g_ctl->xs[k].old_tid) {
			main_old = g_ctl->xs[k].old_tid;
			main_new = g_ctl->xs[k].new_tid;
		}
	if (!base || !main_old || main_old == main_new)
		return;
	nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	sec = IMAGE_FIRST_SECTION(nt);
	for (n = 0; n < nt->FileHeader.NumberOfSections; n++, sec++) {
		DWORD *p, *e;

		if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE))
			continue;
		p = (DWORD *)(base + sec->VirtualAddress);
		e = (DWORD *)(base + sec->VirtualAddress + (sec->Misc.VirtualSize & ~3u));
		for (; p < e; p++) {
			MEMORY_BASIC_INFORMATION mbi;

			if (((uintptr_t)p & 0xFFF) == 0 &&
			    (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi) ||
			     mbi.State != MEM_COMMIT ||
			     !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
					      PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
			     (mbi.Protect & PAGE_GUARD))) {
				p = (DWORD *)((unsigned char *)p + 0x1000) - 1;
				continue;
			}
			if (*p == main_old) {
				if (fixed < 8)
					ss_log("  thread id: %08lX (rabiribi+%lX) held the main thread's "
					       "saved id %lu, now %lu\n",
					       (unsigned long)(uintptr_t)p,
					       (unsigned long)((unsigned char *)p - base),
					       (unsigned long)main_old, (unsigned long)main_new);
				*p = main_new;
				fixed++;
				continue;
			}
			for (k = 0; k < g_ctl->nxs; k++)
				if (g_ctl->xs[k].old_tid && *p == g_ctl->xs[k].old_tid &&
				    g_ctl->xs[k].old_tid != g_ctl->xs[k].new_tid) {
					if (other < 8)
						ss_log("  thread id: %08lX (rabiribi+%lX) holds %lu, the "
						       "saved id of a worker now running as %lu - left "
						       "alone\n",
						       (unsigned long)(uintptr_t)p,
						       (unsigned long)((unsigned char *)p - base),
						       (unsigned long)g_ctl->xs[k].old_tid,
						       (unsigned long)g_ctl->xs[k].new_tid);
					other++;
					break;
				}
		}
	}
	ss_log("  thread id: %d copy(ies) of the main thread's saved id rewritten in the "
	       "game image, %d word(s) matching another saved thread left alone\n",
	       fixed, other);
}
#endif

/* The last look before the release. Every transplanted thread is read back
 * and held to what the transplant meant it to resume with; one that drifted is
 * set again, once. Then the result itself is judged: the stack pointer and
 * the SEH chain head on the thread's own stack, the instruction pointer in
 * mapped code. Returns how many are still wrong. */
static int xs_verify(void)
{
#if defined(_M_IX86) || defined(__i386__)
	int k, j, bad = 0, repaired = 0, logged = 0;

	for (k = 0; k < g_ctl->nxs; k++) {
		XsMap *x = &g_ctl->xs[k];
		HANDLE h = NULL;
		unsigned char *teb;
		MEMORY_BASIC_INFORMATION m;
		uintptr_t hi, lo = 0;
		const char *why = NULL;
		CONTEXT c;
		DWORD seh;

		for (j = 0; j < g_ctl->nids; j++)
			if (g_ctl->ids[j] == x->new_tid)
				h = g_ctl->handles[j];
		teb = h ? (unsigned char *)teb_of(h) : NULL;
		if (!teb) {
			why = "the live thread is gone";
			goto judged;
		}
		if (!x->state) {
			why = "its stack was moved but no context was set";
			goto judged;
		}
		memset(&c, 0, sizeof(c));
		c.ContextFlags = CONTEXT_FULL;
		if (x->state != 1 || !GetThreadContext(h, &c) || c.Eip != x->want.Eip ||
		    c.Esp != x->want.Esp || c.Ebp != x->want.Ebp || c.Ebx != x->want.Ebx ||
		    c.Esi != x->want.Esi || c.Edi != x->want.Edi) {
			if (logged++ < 8)
				ss_log("  verify: thread %lu %s (esp %08lX ebp %08lX, meant "
				       "%08lX %08lX) - setting it again\n",
				       (unsigned long)x->new_tid,
				       x->state == 2 ? "refused its context"
						     : "came back different",
				       (unsigned long)c.Esp, (unsigned long)c.Ebp,
				       (unsigned long)x->want.Esp, (unsigned long)x->want.Ebp);
			memset(&c, 0, sizeof(c));
			c.ContextFlags = CONTEXT_FULL;
			if (!SetThreadContext(h, &x->want) || !GetThreadContext(h, &c) ||
			    c.Esp != x->want.Esp || c.Eip != x->want.Eip ||
			    c.Ebp != x->want.Ebp) {
				why = "it would not take its context";
				goto judged;
			}
			x->state = 1;
			repaired++;
		}
		hi = *(const uintptr_t *)(teb + TIB_STACK_BASE);
		if (VirtualQuery((LPCVOID)(hi - 1), &m, sizeof(m)) == sizeof(m))
			lo = (uintptr_t)m.AllocationBase;
		seh = *(const DWORD *)teb;
		if (c.Esp < lo || c.Esp >= hi)
			why = "its stack pointer is off its own stack";
		else if (seh != 0xFFFFFFFFu && (seh < c.Esp || seh >= hi))
			why = "its exception chain starts off its own stack";
		else if (VirtualQuery((LPCVOID)(uintptr_t)c.Eip, &m, sizeof(m)) != sizeof(m) ||
			 m.State != MEM_COMMIT || m.Type != MEM_IMAGE ||
			 !(m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
					PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
			why = "its instruction pointer is not in code";
judged:
		if (why) {
			bad++;
			ss_log("  verify: thread %lu (saved %lu) FAILS - %s\n",
			       (unsigned long)x->new_tid, (unsigned long)x->old_tid, why);
		}
	}
	ss_log("  verify: %d transplanted thread(s) checked before the release, %d set "
	       "again, %d still wrong\n",
	       g_ctl->nxs, repaired, bad);
	return bad;
#else
	return 0;
#endif
}

/* A failed load's threads stay frozen for the rollback: their handles move to
 * the held list, and every lock this load took is let go so the scratch load
 * can take them again. Nothing is resumed. */
static void hold_threads(void)
{
	int i, k;

	for (k = 0; k < g_ctl->nxs; k++)
		for (i = 0; i < g_ctl->nids; i++)
			if (g_ctl->ids[i] == g_ctl->xs[k].new_tid && g_ctl->handles[i]) {
				unsigned char *teb = (unsigned char *)teb_of(g_ctl->handles[i]);

				if (teb)
					*(DWORD *)teb = g_ctl->xs[k].live_seh;
			}
	g_ctl->held_n = 0;
	for (i = 0; i < g_ctl->nids; i++) {
		if (!g_ctl->handles[i])
			continue;
		if (g_did_suspend[i] && g_ctl->held_n < SS_MAX_THREADS)
			g_ctl->held[g_ctl->held_n++] = g_ctl->handles[i];
		else
			CloseHandle(g_ctl->handles[i]);
		g_ctl->handles[i] = NULL;
		g_did_suspend[i] = 0;
	}
	g_ctl->nxs = 0;
	blk_unlock_all();
	g_env_frozen = 0;
}

/* Called once the scratch load holds its own suspension, so the one the failed
 * load left behind can go without a single thread running in between. */
static void release_held(void)
{
	int i;

	for (i = 0; i < g_ctl->held_n; i++) {
		ResumeThread(g_ctl->held[i]);
		CloseHandle(g_ctl->held[i]);
	}
	g_ctl->held_n = 0;
}

static int rollback_mode(void)
{
	char v[8];
	DWORD n = ss_getenv("D3D9SW_ROLLBACK", v, sizeof(v));

	return (n > 0 && n < sizeof(v) && v[0] == '0') ? 0 : 1;
}

/* Every thread's SEH chain ends in a record whose handler is one of ntdll's
 * FinalExceptionHandlerPad stubs, picked once per process, and the dispatcher's
 * chain check insists on this process's one. Stack contents brought back from
 * another launch end in that launch's pad, so the first exception raised on the
 * thread - even OutputDebugString's - is a fail-fast 0x15. The right pad is read
 * off this thread's own chain, which this process built. */
static uintptr_t seh_last_handler(uintptr_t r, uintptr_t lo, uintptr_t hi, uintptr_t *rec)
{
	int n;

	for (n = 0; n < 512 && r >= lo && r + 8 <= hi && !(r & 3) && ss_readable(r, 8); n++) {
		uintptr_t next = ((uintptr_t *)r)[0];

		if (next == (uintptr_t)-1) {
			*rec = r;
			return ((uintptr_t *)r)[1];
		}
		if (next <= r)
			break;
		r = next;
	}
	return 0;
}

static void seh_final_fix(void)
{
#if defined(_M_IX86) || defined(__i386__)
	NT_TIB *me = (NT_TIB *)NtCurrentTeb();
	uintptr_t rec = 0, pad;
	int i, fixed = 0, walked = 0;

	pad = seh_last_handler((uintptr_t)me->ExceptionList, (uintptr_t)me->StackLimit,
			       (uintptr_t)me->StackBase, &rec);
	if (!pad || module_of(pad) < 0) {
		ss_log("  seh: could not read this process's final handler - chains left as "
		       "they are\n");
		return;
	}
	for (i = 0; i < g_ctl->nids; i++) {
		NT_TIB *t;
		uintptr_t h;

		if (!g_ctl->handles[i])
			continue;
		t = (NT_TIB *)teb_of(g_ctl->handles[i]);
		if (!t || !ss_readable((uintptr_t)t, sizeof(NT_TIB)))
			continue;
		h = seh_last_handler((uintptr_t)t->ExceptionList, (uintptr_t)t->StackLimit,
				     (uintptr_t)t->StackBase, &rec);
		if (!h)
			continue;
		walked++;
		if (h != pad && (h > pad ? h - pad : pad - h) < 0x100) {
			ss_log("  seh: thread %lu ended its chain in %08lX, this process's is "
			       "%08lX - put right at %08lX\n",
			       (unsigned long)g_ctl->ids[i], (unsigned long)h, (unsigned long)pad,
			       (unsigned long)rec);
			((uintptr_t *)rec)[1] = pad;
			fixed++;
		}
	}
	ss_log("  seh: %d chain(s) walked, %d final handler(s) put right (this process: "
	       "%08lX)\n",
	       walked, fixed, (unsigned long)pad);
#endif
}

static int wakeblast_mode(void)
{
	char v[8];
	DWORD n = ss_getenv("D3D9SW_WAKEBLAST", v, sizeof(v));

	return n > 0 && n < sizeof(v) && v[0] == '1';
}

static int pull_steps(void)
{
	char v[12];
	DWORD n = ss_getenv("D3D9SW_PULL", v, sizeof(v));

	return n > 0 && n < sizeof(v) ? atoi(v) : 2000;
}

/* "Event", "Mutant", "Semaphore", ... for a handle in this process. */
static void pull_type(HANDLE h, char *out, size_t cap)
{
	typedef LONG(NTAPI * PFN_qo)(HANDLE, int, PVOID, ULONG, PULONG);
	static PFN_qo qo;
	union {
		struct {
			USHORT Length, MaximumLength;
			WCHAR *Buffer;
		} s;
		BYTE b[512];
	} u;
	ULONG got = 0;
	int i;

	lstrcpynA(out, "invalid", (int)cap);
	if (!qo)
		qo = (PFN_qo)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryObject");
	if (!qo || qo(h, 2 /* ObjectTypeInformation */, &u, sizeof(u), &got) < 0)
		return;
	for (i = 0; i < u.s.Length / 2 && i < (int)cap - 1; i++)
		out[i] = (char)u.s.Buffer[i];
	out[i] = 0;
}

/* Wake one waited-on object the way its owner would: set an event, release a
 * semaphore. Mutexes and threads cannot be forced from here. */
static const char *pull_kick(HANDLE h, char *type, size_t cap)
{
	pull_type(h, type, cap);
	if (!lstrcmpA(type, "Event"))
		return SetEvent(h) ? "set" : "set failed";
	if (!lstrcmpA(type, "Semaphore"))
		return ReleaseSemaphore(h, 1, NULL) ? "released" : "release failed";
	return "left alone";
}

/* The caller chain, as module+offset, along the ebp links. */
static void pull_chain(const CONTEXT *c, char *out, size_t cap)
{
	uintptr_t ebp = c->Ebp;
	size_t at = 0;
	int n;

	out[0] = 0;
	for (n = 0; n < 8 && ebp && !(ebp & 3) && ss_readable(ebp, 8) && at + 40 < cap; n++) {
		uintptr_t ret = ((uintptr_t *)ebp)[1], next = ((uintptr_t *)ebp)[0];
		unsigned off = 0;
		const char *m = ss_module(ret, &off);

		at += (size_t)_snprintf(out + at, cap - at, "%s%s+%X", n ? " < " : "",
					m ? m : "?", off);
		if (next <= ebp)
			break;
		ebp = next;
	}
}

/* Keep the stalled main thread moving: each time it parks in a wait, log what on
 * and from where, then wake the object and see where it parks next. Repeats are
 * folded into a count, so the log reads as the chain of distinct waits. Ends at
 * the first frame, at an object that cannot be woken, or after D3D9SW_PULL steps
 * (0 turns it off). */
static void pull_main(void)
{
#if defined(_M_IX86) || defined(__i386__)
	int steps = pull_steps(), i, same = 0;
	LONG f0 = g_frames_since_load;
	char last[600] = "", line[600], chain[400], type[64];
	HANDLE th;

	if (steps <= 0 || !g_ctl->req_tid)
		return;
	th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
			FALSE, g_ctl->req_tid);
	if (!th)
		return;
	ss_raw("pull: pulling thread %lu forward, up to %d step(s)\n",
	       (unsigned long)g_ctl->req_tid, steps);
	for (i = 0; i < steps; i++) {
		CONTEXT c;
		const BYTE *ip;
		HANDLE w[8];
		int nw = 0, k;
		const char *what = "running";
		size_t at;

		Sleep(20);
		if (g_frames_since_load != f0) {
			ss_raw("pull: a frame arrived after %d step(s)\n", i);
			break;
		}
		if (SuspendThread(th) == (DWORD)-1)
			break;
		memset(&c, 0, sizeof(c));
		c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
		if (!GetThreadContext(th, &c)) {
			ResumeThread(th);
			break;
		}
		ip = (const BYTE *)c.Eip;
		if (ss_readable(c.Eip, 3) && ss_readable(c.Esp, 16) && ip[0] == 0xC2) {
			if (ip[1] == 0x0C) { /* NtWaitForSingleObject(h, alertable, timeout) */
				w[nw++] = *(HANDLE *)(c.Esp + 4);
				what = "single";
			} else if (ip[1] == 0x14) { /* NtWaitForMultipleObjects(n, hs, ...) */
				DWORD n = *(DWORD *)(c.Esp + 4);
				HANDLE *hs = *(HANDLE **)(c.Esp + 8);

				if (n > 8)
					n = 8;
				if (ss_readable((uintptr_t)hs, n * sizeof(HANDLE)))
					for (; nw < (int)n; nw++)
						w[nw] = hs[nw];
				what = "multiple";
			} else
				what = "other wait";
		}
		pull_chain(&c, chain, sizeof(chain));
		ResumeThread(th);
		at = (size_t)_snprintf(line, sizeof(line), "%s on", what);
		for (k = 0; k < nw; k++) {
			pull_type(w[k], type, sizeof(type));
			at += (size_t)_snprintf(line + at, sizeof(line) - at, " %lX(%s)",
						(unsigned long)(uintptr_t)w[k], type);
		}
		_snprintf(line + at, sizeof(line) - at, " from %s", chain);
		if (!lstrcmpA(line, last))
			same++;
		else {
			if (same)
				ss_raw("pull:   ... the same %d more time(s)\n", same);
			ss_raw("pull: step %d: %s\n", i, line);
			lstrcpynA(last, line, sizeof(last));
			same = 0;
		}
		for (k = 0; k < nw; k++) {
			const char *r = pull_kick(w[k], type, sizeof(type));

			if (!same)
				ss_raw("pull:   %lX %s: %s\n", (unsigned long)(uintptr_t)w[k], type, r);
			if (nw == 1 && !lstrcmpA(r, "left alone")) {
				ss_raw("pull: stopped - thread waits on a %s, which cannot be woken "
				       "from here\n",
				       type);
				i = steps;
			}
		}
	}
	if (same)
		ss_raw("pull:   ... the same %d more time(s)\n", same);
	ss_raw("pull: done, %ld frame(s) since the restore\n", (long)g_frames_since_load);
	CloseHandle(th);
#endif
}

/* Where each busy game thread spends its time: 50 samples over a second, by
 * caller chain. A thread looping on restored state shows one or two chains. */
static void pull_spinners(void)
{
#if defined(_M_IX86) || defined(__i386__)
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	THREADENTRY32 te;
	DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();

	if (snap == INVALID_HANDLE_VALUE)
		return;
	te.dwSize = sizeof(te);
	if (Thread32First(snap, &te)) {
		do {
			HANDLE h;
			int s, k, nd = 0;
			static char seen[8][300], at[8][48];
			int cnt[8];

			if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
				continue;
			h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
				       te.th32ThreadID);
			if (!h)
				continue;
			for (s = 0; s < 50; s++) {
				CONTEXT c;
				char ch[300];
				unsigned off = 0;
				const char *m;

				if (SuspendThread(h) == (DWORD)-1)
					break;
				memset(&c, 0, sizeof(c));
				c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
				k = GetThreadContext(h, &c);
				if (k)
					pull_chain(&c, ch, sizeof(ch));
				ResumeThread(h);
				if (!k)
					break;
				m = ss_module(c.Eip, &off);
				/* Parked threads sit in ntdll/win32u: not spinning. */
				if (!m || !lstrcmpiA(m, "ntdll.dll") || !lstrcmpiA(m, "win32u.dll"))
					break;
				for (k = 0; k < nd; k++)
					if (!strcmp(seen[k], ch))
						break;
				if (k < nd)
					cnt[k]++;
				else if (nd < 8) {
					lstrcpynA(seen[nd], ch, sizeof(seen[nd]));
					_snprintf(at[nd], sizeof(at[nd]), "%s+%X", m, off);
					at[nd][sizeof(at[nd]) - 1] = 0;
					cnt[nd++] = 1;
				}
				Sleep(20);
			}
			for (k = 0; k < nd; k++)
				ss_raw("spin: thread %lu, %2d of 50 sample(s) at %s from %s\n",
				       (unsigned long)te.th32ThreadID, cnt[k], at[k], seen[k]);
			CloseHandle(h);
		} while (Thread32Next(snap, &te));
	}
	CloseHandle(snap);
#endif
}

/* After a load from another launch the game can live on without drawing: the
 * audio thread streams while the render thread waits on something that came
 * from the other process. If no frame arrives for five seconds, say once where
 * every thread is standing. */
static BOOL CALLBACK xs_hung_cb(HWND h, LPARAM lp)
{
	DWORD pid = 0;

	GetWindowThreadProcessId(h, &pid);
	if (pid == GetCurrentProcessId() && IsWindowVisible(h) && IsHungAppWindow(h)) {
		*(int *)lp = 1;
		return FALSE;
	}
	return TRUE;
}

/* Frames can keep coming while the window stops answering: Windows calls that
 * not responding, and a frame count alone never notices. */
static int xs_window_hung(void)
{
	int hung = 0;

	EnumWindows(xs_hung_cb, (LPARAM)&hung);
	return hung;
}

static DWORD WINAPI xs_watchdog(LPVOID param)
{
	LONG last = -1;
	int still = 0, hung = 0;

	(void)param;
	for (;;) {
		LONG now = g_frames_since_load;

		Sleep(1000);
		hung = xs_window_hung() ? hung + 1 : 0;
		if (hung >= 5) {
			ss_raw("stall: the window has not answered for 5 s, %ld frame(s) since the "
			       "restore and still %s\n",
			       (long)now, g_frames_since_load != now ? "drawing" : "not drawing");
			last = now;
			break;
		}
		if (now != last) {
			last = now;
			still = 0;
			continue;
		}
		if (++still < 5)
			continue;
		break;
	}
#if defined(_M_IX86) || defined(__i386__)
	{
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		THREADENTRY32 te;
		DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();

		ss_raw("stall: no frame for 5 s, %ld frame(s) since the restore - where every "
		       "thread is\n",
		       (long)last);
		if (snap == INVALID_HANDLE_VALUE)
			return 0;
		te.dwSize = sizeof(te);
		if (Thread32First(snap, &te)) {
			do {
				HANDLE h;
				CONTEXT c;
				unsigned off = 0;
				const char *in;
				int ours;

				if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
					continue;
				h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
						       THREAD_QUERY_INFORMATION,
					       FALSE, te.th32ThreadID);
				if (!h)
					continue;
				if (SuspendThread(h) == (DWORD)-1) {
					CloseHandle(h);
					continue;
				}
				memset(&c, 0, sizeof(c));
				c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
				if (GetThreadContext(h, &c)) {
					in = ss_module(c.Eip, &off);
					ss_raw("stall: thread %lu at %08lX in %s+%X, esp %08lX\n",
					       (unsigned long)te.th32ThreadID, (unsigned long)c.Eip,
					       in ? in : "unknown", off, (unsigned long)c.Esp);
					ss_frames(&c);
				}
				ResumeThread(h);
				{
					HMODULE m = NULL;
					PVOID st = start_of(h);

					GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
							   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							   (LPCSTR)st, &m);
					ours = !st || m == (HMODULE)&__ImageBase;
				}
				CloseHandle(h);
				/* One-time test: wake every game worker parked in
				 * NtWaitForSingleObject (it ends in ret 0Ch, the
				 * handle is the first argument). */
				if (wakeblast_mode() && !ours && te.th32ThreadID != g_ctl->req_tid &&
				    c.Eip && ((const BYTE *)c.Eip)[0] == 0xC2 &&
				    ((const BYTE *)c.Eip)[1] == 0x0C) {
					HANDLE w = *(HANDLE *)(c.Esp + 4);
					BOOL ok = SetEvent(w);

					ss_raw("wakeblast: thread %lu waits on %08lX, SetEvent %s (err %lu)\n",
					       (unsigned long)te.th32ThreadID, (unsigned long)(uintptr_t)w,
					       ok ? "ok" : "failed", ok ? 0ul : (unsigned long)GetLastError());
				}
			} while (Thread32Next(snap, &te));
		}
		CloseHandle(snap);
	}
#endif
	pull_spinners();
	pull_main();
	return 0;
}

/* A load from another process, with a way back. The scratch save is taken
 * first, while the game still runs as it is; if the check before the release
 * fails, do_load returns -1 with every thread still frozen and the scratch is
 * put back the way any load in this process is. The requester then returns
 * from savestate_load with a failure, into the moment it asked. */
static int do_load(int slotno);

static int load_with_rollback(int slotno)
{
	Slot *s = &g_ctl->slots[slotno];
	Slot *sc = &g_ctl->slots[SS_SCRATCH];
	int rc;

	g_ctl->rollback_ready = 0;
	g_ctl->held_n = 0;
	g_ctl->nspare_taken = 0;
	if (rollback_mode() && transplant_mode()) {
		if (!s->valid && slotfile_mode())
			slotfile_read(slotno, s);
		if (s->valid && slot_provenance(s) != PROV_SAME_PROCESS) {
			ss_log("rollback: taking a scratch save before loading slot %d, which "
			       "came from another process\n",
			       slotno);
			g_ctl->rollback_ready = do_save(SS_SCRATCH);
			if (!g_ctl->rollback_ready)
				ss_log("rollback: the scratch save failed - this load has no "
				       "way back\n");
			/* After the scratch save, whose census is what heap_index_of
			 * consults for this launch's heaps. */
			bnd_report(s);
		}
	}
	rc = do_load(slotno);
	g_ctl->rollback_ready = 0;
	if (rc < 0) {
		ss_log("rollback: the load failed its check before the release - putting "
		       "back the scratch save\n");
		for (int k = 0; k < g_ctl->nspare_taken; k++) {
			HANDLE h = OpenThread(THREAD_TERMINATE, FALSE, g_ctl->spare_taken[k]);

			if (h) {
				TerminateThread(h, 0);
				CloseHandle(h);
			}
		}
		if (g_ctl->nspare_taken)
			ss_log("rollback: %d stand-in(s) the failed load had taken were ended\n",
			       g_ctl->nspare_taken);
		g_ctl->nspare_taken = 0;
		rc = do_load(SS_SCRATCH);
		if (g_ctl->held_n) {
			ss_log("rollback: the scratch load stopped before it froze the threads "
			       "- releasing them where the failed load left them\n");
			release_held();
		}
		ss_log("rollback: %s\n",
		       rc > 0 ? "done, the game is back at the moment the load was asked for"
			      : "the scratch load FAILED as well");
		rc = 0;
	}
	xs_spares_release();
	if (sc->valid)
		slot_release(sc);
	return rc;
}

/* The arenas are left in the present but the pointers to them live in our data
 * section, which the restore rewinds. From another process those pointers name
 * that process's memory - the save after one such load walked a stack reserve
 * as the heap list. An arena is ours if it lies in a range this process
 * excluded; any other value goes back to what it was before the restore, or to
 * NULL so it is allocated again on next use. Sized arenas only go to NULL,
 * since the size beside them was rewound too. */
static void **const g_arena_ptrs[] = {
	(void **)&g_blk_save,  (void **)&g_blk_now,    (void **)&g_reg_off,
	(void **)&g_blk_own,   (void **)&g_blk_sys,    (void **)&g_blk_wrote,
	(void **)&g_blk_depth, (void **)&g_blk_parent, (void **)&g_blk_root,
	(void **)&g_blk_queue, (void **)&g_seen,       (void **)&g_cov,
	(void **)&g_freed,     (void **)&g_held,       (void **)&g_der_mask,
	(void **)&g_clob_ok,   (void **)&g_clob_off,   (void **)&g_clob_pre,
};
static void **const g_arena_sized[] = {
	(void **)&g_der_pre, (void **)&g_der_at, (void **)&g_der_len, (void **)&g_der_plan,
};
#define SS_NARENA (sizeof(g_arena_ptrs) / sizeof(g_arena_ptrs[0]))

static int arena_ours(const void *p)
{
	LONG i;

	for (i = 0; i < g_ctl->nex; i++)
		if ((uintptr_t)p >= g_ctl->ex_lo[i] && (uintptr_t)p < g_ctl->ex_hi[i])
			return 1;
	return 0;
}

static void arena_hold(void **keep)
{
	unsigned i;

	for (i = 0; i < SS_NARENA; i++)
		keep[i] = *g_arena_ptrs[i];
}

static void arena_put_back(void *const *keep)
{
	unsigned i, put = 0, sized = 0;

	for (i = 0; i < SS_NARENA; i++) {
		void *now = *g_arena_ptrs[i];

		if (!now || arena_ours(now))
			continue;
		*g_arena_ptrs[i] = (keep[i] && arena_ours(keep[i])) ? keep[i] : NULL;
		put++;
	}
	for (i = 0; i < sizeof(g_arena_sized) / sizeof(g_arena_sized[0]); i++)
		if (*g_arena_sized[i] && !arena_ours(*g_arena_sized[i]))
			sized++;
	if (sized) {
		for (i = 0; i < sizeof(g_arena_sized) / sizeof(g_arena_sized[0]); i++)
			*g_arena_sized[i] = NULL;
		g_der_cap = 0;
		g_der_regs = 0;
	}
	if (put || sized)
		ss_log("  arenas: %u pointer(s) to our scratch memory came back naming "
		       "another process's - put back or cleared\n",
		       put + sized);
}

/* A refused load wrote nothing, so the audio it parked goes back to playing the
 * present, exactly as the end of a save does. */
static void load_refused_resume(void)
{
	resume_all(0);
	dsh_play();
	xa2_sw_resume();
}

/* A module the game loaded after this process reached the save point's
 * equivalent - DDPR's dat12.bin is a code DLL it loads when play starts - is
 * absent from a launch still at the title, and the restored game calls into it
 * on its first frame. Loading it from the recorded path puts it back: within
 * one boot Windows maps an image at the same address in every process when
 * that address is free, and where it is not, the reloc pass shifts pointers
 * into it like any other moved module. The reference this takes is the one the
 * restored game believes it holds. System modules are left to their owners. */
static void preload_missing_modules(const Slot *s)
{
	char windir[MAX_PATH];
	UINT wn = GetWindowsDirectoryA(windir, sizeof(windir));
	int i;

	for (i = 0; i < s->nmods; i++) {
		const char *p = s->mod_path[i];
		HMODULE h;

		if (!p[0] || (wn && _strnicmp(p, windir, wn) == 0) || GetModuleHandleA(p) ||
		    GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES)
			continue;
		h = LoadLibraryA(p);
		ss_log("  modules: %s was loaded at the save and not now - %s at %p "
		       "(saved at %08lX)%s\n",
		       s->mod_name[i], h ? "loaded it" : "could not load it", (void *)h,
		       (unsigned long)s->mod_lo[i],
		       h && (uintptr_t)h != s->mod_lo[i] ? ", a different address - "
							   "pointers into it get shifted" : "");
	}
}

/* D3D9SW_MERGE=1: a load takes only what the game's state is made of and
 * leaves the rest of the process as this launch built it. The cfg names it:
 *   D3D9SW_MERGE_TAKE=dat112.bin+328000,default.exe+177000
 *	saved regions starting at module+offset, written back whole;
 *   D3D9SW_MERGE_SITES=6830,732D
 *	arena blocks by allocation site, paired by identity through the ledger.
 * No thread is rewound and no other memory moves. */
int gameheap_merge_sites(const unsigned *sites, int nsites,
			 int (*fetch)(void *ctx, uintptr_t a, void *dst, size_t n), void *ctx);

static int merge_mode(void)
{
	char v[8];
	DWORD n = ss_getenv("D3D9SW_MERGE", v, sizeof(v));

	return n > 0 && n < sizeof(v) && v[0] == '1';
}

static volatile LONG g_merged_clear;
static volatile LONG g_saved_since;

int savestate_merged_clear(void)
{
	return InterlockedExchange(&g_merged_clear, 0) != 0;
}

/* The game's idea of whether it has focus came back from the save, and nothing
 * tells it otherwise until focus next changes. Tell it the state it is in now.
 * Only that: a game that pauses on losing focus would pause on a lost-then-
 * regained pair. D3D9SW_MERGE_REFOCUS=0 to leave it alone. */
static BOOL CALLBACK merge_refocus_one(HWND h, LPARAM lp)
{
	DWORD pid = 0, tid = GetWindowThreadProcessId(h, &pid);
	HWND fg = GetForegroundWindow();
	int front;

	(void)lp;
	if (pid != GetCurrentProcessId() || !IsWindowVisible(h))
		return TRUE;
	front = fg == h || GetAncestor(fg, GA_ROOTOWNER) == h;
	if (front) {
		PostMessageA(h, WM_ACTIVATEAPP, TRUE, 0);
		PostMessageA(h, WM_ACTIVATE, WA_ACTIVE, 0);
		PostMessageA(h, WM_SETFOCUS, 0, 0);
	} else {
		PostMessageA(h, WM_ACTIVATEAPP, FALSE, 0);
		PostMessageA(h, WM_ACTIVATE, WA_INACTIVE, 0);
		PostMessageA(h, WM_KILLFOCUS, 0, 0);
	}
	ss_log("  merge: told window %p (thread %lu) it %s focus\n", (void *)h,
	       (unsigned long)tid, front ? "has" : "does not have");
	return TRUE;
}

static void merge_refocus(void)
{
	char v[8];

	if (ss_getenv("D3D9SW_MERGE_REFOCUS", v, sizeof(v)) > 0 && v[0] == '0')
		return;
	EnumWindows(merge_refocus_one, 0);
}

#if defined(_M_IX86) || defined(__i386__)
typedef struct {
	const Slot *s;
	Window *w;
} MergeCtx;

static int merge_fetch(void *ctx, uintptr_t a, void *dst, size_t n)
{
	MergeCtx *m = (MergeCtx *)ctx;

	return xs_fetch(m->s, m->w, a, dst, n);
}

static int merge_take_span(const Slot *s, Window *w, const char *ent, uintptr_t a,
			   uintptr_t end, int commit, intptr_t shift);

/* The renderer's statics (.swdata and .swbss, see d3d9_sw.c). The rest of our
 * data - hooks, stubs, the fault handler - names this launch and stays. Our
 * image never moves, so the saved bytes sit where they belong. */
static int merge_take_self(const Slot *s, Window *w)
{
	HMODULE me = NULL;
	IMAGE_NT_HEADERS *nt;
	IMAGE_SECTION_HEADER *sec;
	int i, n = 0;

	if (s->nthreads < 1 || s->threads[0].build_id != ss_build_id()) {
		ss_log("  merge: self skipped - the save was taken by build %08lX, this is "
		       "%08lX, and the layout of our data differs between builds\n",
		       s->nthreads > 0 ? (unsigned long)s->threads[0].build_id : 0ul,
		       (unsigned long)ss_build_id());
		return 0;
	}
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCSTR)(void *)merge_take_self, &me);
	if (!me)
		return 0;
	nt = (IMAGE_NT_HEADERS *)((char *)me + ((IMAGE_DOS_HEADER *)me)->e_lfanew);
	sec = IMAGE_FIRST_SECTION(nt);
	for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
		char name[9];
		uintptr_t lo = (uintptr_t)me + sec[i].VirtualAddress;

		memcpy(name, sec[i].Name, 8);
		name[8] = 0;
		if (strcmp(name, ".swdata") && strcmp(name, ".swbss"))
			continue;
		n += merge_take_span(s, w, name, lo, lo + sec[i].Misc.VirtualSize, 0, 0);
	}
	return n > 0;
}

/* "module+offset" takes every saved page from there to the end of the module.
 * "address:length" (both hex) takes every saved page in that span, committing
 * what this launch has not touched yet. "self" is this DLL's own data. */
static int merge_take_region(const Slot *s, Window *w, const char *ent)
{
	char mod[64];
	const char *plus = strchr(ent, '+'), *colon = strchr(ent, ':');
	uintptr_t a, end = 0;
	intptr_t shift = 0;
	int i;

	if (!strcmp(ent, "self"))
		return merge_take_self(s, w);
	if (plus) {
		HMODULE h;

		if (plus - ent >= (int)sizeof(mod))
			return 0;
		memcpy(mod, ent, plus - ent);
		mod[plus - ent] = 0;
		h = GetModuleHandleA(mod);
		if (!h) {
			ss_log("  merge: %s is not loaded - %s skipped\n", mod, ent);
			return 0;
		}
		for (i = 0; i < s->nmods && lstrcmpiA(s->mod_name[i], mod); i++)
			;
		if (i == s->nmods) {
			ss_log("  merge: %s was not loaded at the save - %s skipped\n", mod, ent);
			return 0;
		}
		shift = (intptr_t)((uintptr_t)h - s->mod_lo[i]);
		a = s->mod_lo[i] + strtoul(plus + 1, NULL, 16);
		end = a < s->mod_hi[i] ? s->mod_hi[i] : 0;
	} else if (colon) {
		a = strtoul(ent, NULL, 16);
		end = a + strtoul(colon + 1, NULL, 16);
	} else {
		ss_log("  merge: \"%s\" is neither module+offset nor address:length\n", ent);
		return 0;
	}
	if (!end) {
		ss_log("  merge: %s (%p) is not in the save\n", ent, (void *)a);
		return 0;
	}
	return merge_take_span(s, w, ent, a, end, colon != NULL, shift);
}

/* What this merge wrote, for D3D9SW_MERGE_CHECK. */
#define MERGE_SPANS 512
static uintptr_t g_mt_lo[MERGE_SPANS], g_mt_hi[MERGE_SPANS];
static int g_mt_n;

static void merge_note_span(uintptr_t lo, uintptr_t hi)
{
	if (g_mt_n < MERGE_SPANS) {
		g_mt_lo[g_mt_n] = lo;
		g_mt_hi[g_mt_n] = hi;
		g_mt_n++;
	}
}

static int merge_check_mode(void)
{
	char v[8];
	DWORD n = ss_getenv("D3D9SW_MERGE_CHECK", v, sizeof(v));

	return n > 0 && n < sizeof(v) && v[0] == '1';
}

int gameheap_va_origin(uintptr_t base, char *out, int cap);
int gameheap_merge_proof(void);
void gameheap_merge_chunks(void);

/* D3D9SW_MERGE_CHECK=1, with the process still frozen: every word in what the
 * merge wrote that names saved memory the merge did not write. Those are the
 * links that will read this launch's bytes as the save's - each is either a
 * region to add, or a pointer to translate. Module images and memory nobody
 * saved are not counted: the first does not move within a boot, the second is
 * mostly numbers that happen to look like addresses. */
/* Modules load where there is room, and Steam's overlay and the others take
 * different room each launch, so a runtime DLL can sit 24 MB from where the save
 * had it. What was taken still names it at the old address. Shift those words,
 * in what was taken only - the live rest already names it where it is. */
#define MR_MAX (SS_MAX_MODS + 2 * SS_MAX_THREADS + 2)

static void merge_reloc_add(RelocEnt *map, int *n, uintptr_t lo, uintptr_t hi, intptr_t delta,
			    int mod)
{
	if (*n >= MR_MAX || !lo || hi <= lo || !delta)
		return;
	memset(&map[*n], 0, sizeof(map[*n]));
	map[*n].lo = lo;
	map[*n].hi = hi;
	map[*n].delta = delta;
	map[*n].mod = mod;
	map[*n].write = 1;
	(*n)++;
}

static void merge_reloc(const Slot *s)
{
	static RelocEnt map[MR_MAX];
	static unsigned short at[65536];
	unsigned long long hits = 0;
	DWORD t0 = GetTickCount();
	char v[8];
	int n = 0, nmod, i, k;
	uintptr_t a;

	if (!(ss_getenv("D3D9SW_MERGE_RELOC", v, sizeof(v)) > 0 && v[0] == '0'))
		n = reloc_build(s, map, SS_MAX_MODS);
	nmod = n;
	/* The moved stacks' frame links and saved stack pointers still name the
	 * old stack; shifting only the registers lets the first pop ebp bring an
	 * old-stack address back, and at a nonzero delta something else lives
	 * there now. The old TEBs, PEB and Control (mod -2) are matched on the
	 * moved stacks only, as in reloc_apply. */
	for (k = 0; k < g_ctl->nxs; k++)
		if (g_ctl->xs[k].state)
			merge_reloc_add(map, &n, g_ctl->xs[k].old_sp, g_ctl->xs[k].old_hi,
					g_ctl->xs[k].delta, -1);
	for (k = 0; k < g_ctl->nxs; k++)
		if (g_ctl->xs[k].state && g_ctl->xs[k].old_teb)
			merge_reloc_add(map, &n, g_ctl->xs[k].old_teb - 0x2000,
					g_ctl->xs[k].old_teb + 0x1000,
					(intptr_t)g_ctl->xs[k].new_teb - (intptr_t)g_ctl->xs[k].old_teb,
					-2);
	if (g_ctl->nxs && g_ctl->xs_peb_old)
		merge_reloc_add(map, &n, g_ctl->xs_peb_old, g_ctl->xs_peb_old + 0x1000,
				(intptr_t)g_ctl->xs_peb_new - (intptr_t)g_ctl->xs_peb_old, -2);
	if (g_ctl->nxs && g_ctl->xs_ctl_old)
		merge_reloc_add(map, &n, g_ctl->xs_ctl_old, g_ctl->xs_ctl_old + sizeof(Control),
				(intptr_t)g_ctl - (intptr_t)g_ctl->xs_ctl_old, -2);
	if (!n)
		return;
	memset(at, 0, sizeof(at));
	for (i = 0; i < n; i++)
		for (a = map[i].lo >> 16; a <= (map[i].hi - 1) >> 16 && a < 65536; a++)
			if (!at[a])
				at[a] = (unsigned short)(i + 1);
	for (i = 0; i < g_mt_n; i++) {
		DWORD *wp = (DWORD *)((g_mt_lo[i] + 3) & ~(uintptr_t)3);
		DWORD *e = (DWORD *)(g_mt_hi[i] & ~(uintptr_t)3);
		DWORD old;
		int stk = g_ctl->nxs && xs_on_new_stack(g_mt_lo[i], g_mt_hi[i] - g_mt_lo[i]);

		if (!VirtualProtect((void *)g_mt_lo[i], g_mt_hi[i] - g_mt_lo[i], PAGE_READWRITE,
				    &old))
			continue;
		for (; wp < e; wp++) {
			unsigned k = at[*wp >> 16];
			RelocEnt *r;

			if (!k)
				continue;
			r = &map[k - 1];
			if (*wp < r->lo || *wp >= r->hi) {
				int j;

				for (j = 0; j < n; j++)
					if (*wp >= map[j].lo && *wp < map[j].hi)
						break;
				if (j == n)
					continue;
				r = &map[j];
			}
			if (r->mod == -2 && !stk)
				continue;
			if (!r->hits++) {
				r->sample_at = (uintptr_t)wp;
				r->sample_val = *wp;
			}
			*wp = (DWORD)(*wp + r->delta);
			hits++;
		}
		VirtualProtect((void *)g_mt_lo[i], g_mt_hi[i] - g_mt_lo[i], old, &old);
	}
	for (i = 0; i < n; i++)
		if (map[i].hits || map[i].mod == -1)
			ss_log("    reloc: %lu word(s) into %s shifted %+ld, first at %08lX (%08lX)\n",
			       map[i].hits,
			       map[i].mod >= 0 ? s->mod_name[map[i].mod] :
			       map[i].mod == -1 ? "an old stack" : "an old TEB/PEB/Control",
			       (long)map[i].delta, (unsigned long)map[i].sample_at,
			       (unsigned long)map[i].sample_val);
	ss_log("  merge: %llu word(s) in what was taken named %d moved module(s) or %d moved "
	       "stack/TEB range(s) at their saved address and were shifted (%lu ms)\n",
	       hits, nmod, n - nmod, (unsigned long)(GetTickCount() - t0));
}

/* Percent of the region's words that differ between the save and now, or -1. */
static int merge_differs(const Slot *s, Window *w, uintptr_t base, uintptr_t size)
{
	MEMORY_BASIC_INFORMATION mbi;
	const DWORD *sv, *lv = (const DWORD *)base;
	unsigned long long diff = 0, n = size / 4, i;
	uintptr_t a;
	int pct = -1;

	if (size > (64u << 20))
		return -1;
	for (a = base; a < base + size; a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize)
		if (!VirtualQuery((void *)a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
		    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
			return -1;
	sv = (const DWORD *)VirtualAlloc(NULL, size, MEM_COMMIT, PAGE_READWRITE);
	if (!sv)
		return -1;
	if (xs_fetch(s, w, base, (void *)sv, size)) {
		for (i = 0; i < n; i++)
			diff += sv[i] != lv[i];
		pct = n ? (int)((diff * 100 + n - 1) / n) : 0;
	}
	VirtualFree((void *)sv, 0, MEM_RELEASE);
	return pct;
}

/* The other direction: a module's live data naming memory the merge replaced.
 * The object it named is gone - the bytes there are the save's now - so the
 * word is half of a pair from two launches. In the modules D3D9SW_MERGE_RELINK
 * lists, a word whose saved value also lands in what was taken (or in a
 * module's code) gets the saved value, which names the save's own object. */
static void merge_relink(const Slot *s, Window *w, const unsigned short *pt, int check,
			 const char *list)
{
	enum { NMOD = 48 };
	HMODULE mods[NMOD], me = NULL;
	unsigned into[NMOD], fixed[NMOD], keep[NMOD];
	uintptr_t eg[NMOD], a;
	MEMORY_BASIC_INFORMATION mbi;
	int nm = 0, i;

	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCSTR)(void *)merge_relink, &me);
	for (a = 0x10000; a < 0x80000000u && VirtualQuery((void *)a, &mbi, sizeof(mbi));
	     a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
		uintptr_t lo = (uintptr_t)mbi.BaseAddress, hi = lo + mbi.RegionSize, p;
		DWORD *sv = NULL, old;
		HMODULE m = (HMODULE)mbi.AllocationBase;
		char path[MAX_PATH], *b;
		int fix, mi;

		if (!mbi.RegionSize)
			break;
		if (mbi.Type != MEM_IMAGE || mbi.State != MEM_COMMIT ||
		    !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) || m == me)
			continue;
		for (mi = 0; mi < nm && mods[mi] != m; mi++)
			;
		if (mi == nm) {
			if (nm == NMOD)
				continue;
			mods[nm] = m;
			into[nm] = fixed[nm] = keep[nm] = 0;
			eg[nm] = 0;
			nm++;
		}
		fix = 0;
		if (list && list[0] && GetModuleFileNameA(m, path, sizeof(path))) {
			char low[512];
			const char *q;
			size_t len;

			b = strrchr(path, '\\');
			b = b ? b + 1 : path;
			CharLowerA(b);
			lstrcpynA(low, list, sizeof(low));
			CharLowerA(low);
			len = strlen(b);
			for (q = low; (q = strstr(q, b)) != NULL; q += len)
				if ((q == low || q[-1] == ',' || q[-1] == ' ') &&
				    (q[len] == 0 || q[len] == ',' || q[len] == ' '))
					fix = 1;
		}
		for (p = lo; p < hi; p += 0x1000) {
			int got = 0;
			const DWORD *wv = (const DWORD *)p;
			unsigned k;

			if (pt[p >> 12] == 1)
				continue;
			for (k = 0; k < 1024; k++) {
				DWORD v = wv[k], sv1;

				if (pt[v >> 12] != 1)
					continue;
				if (!into[mi]++)
					eg[mi] = (uintptr_t)&wv[k];
				if (!fix)
					continue;
				if (!sv) {
					sv = (DWORD *)VirtualAlloc(NULL, 0x1000, MEM_COMMIT,
								   PAGE_READWRITE);
					if (!sv)
						break;
				}
				if (!got && !xs_fetch(s, w, p, sv, 0x1000)) {
					keep[mi]++;
					break;
				}
				got = 1;
				sv1 = sv[k];
				if (sv1 == v)
					continue;
				if (pt[sv1 >> 12] != 1 && pt[sv1 >> 12] != 2) {
					keep[mi]++;
					continue;
				}
				if (VirtualProtect((void *)&wv[k], 4, PAGE_READWRITE, &old)) {
					*(DWORD *)&wv[k] = sv1;
					VirtualProtect((void *)&wv[k], 4, old, &old);
					fixed[mi]++;
				}
			}
		}
		if (sv)
			VirtualFree(sv, 0, MEM_RELEASE);
	}
	for (i = 0; i < nm; i++) {
		char path[MAX_PATH], *b;

		if (!into[i])
			continue;
		if (!fixed[i] && !check)
			continue;
		path[0] = 0;
		GetModuleFileNameA(mods[i], path, sizeof(path));
		b = strrchr(path, '\\');
		ss_log("    %7u live word(s) in %s's data name taken memory, e.g. at %08lX - %u "
		       "set to the save's value, %u left\n",
		       into[i], b ? b + 1 : path, (unsigned long)eg[i], fixed[i], keep[i]);
	}
}

static void merge_linkage(const Slot *s, Window *w, int check, const char *relink)
{
	const unsigned npages = 0x100000u;
	unsigned short *pt;
	unsigned *cnt;
	uintptr_t *eg, a;
	MEMORY_BASIC_INFORMATION mbi;
	unsigned long long words = 0, edges = 0;
	int i, k, shown;
	DWORD t0 = GetTickCount();

	pt = (unsigned short *)VirtualAlloc(NULL, npages * sizeof(*pt), MEM_COMMIT, PAGE_READWRITE);
	cnt = (unsigned *)VirtualAlloc(NULL, (size_t)s->nregs * (sizeof(*cnt) + sizeof(*eg)),
				       MEM_COMMIT, PAGE_READWRITE);
	if (!pt || !cnt) {
		ss_log("  merge check: no room for the page table\n");
		goto out;
	}
	eg = (uintptr_t *)(cnt + s->nregs);
	for (i = 0; i < s->nregs && i < 0xFFF0; i++)
		for (a = s->regs[i].base >> 12; a < (s->regs[i].base + s->regs[i].size + 0xFFF) >> 12 &&
						a < npages;
		     a++)
			pt[a] = (unsigned short)(3 + i);
	for (a = 0x10000; a < 0x80000000u && VirtualQuery((void *)a, &mbi, sizeof(mbi));
	     a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
		/* A module's data is state like any other; only its code and constants
		 * are the same in both launches. */
		if (mbi.Type == MEM_IMAGE && mbi.State == MEM_COMMIT &&
		    !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
				     PAGE_EXECUTE_WRITECOPY))) {
			uintptr_t p;

			for (p = (uintptr_t)mbi.BaseAddress >> 12;
			     p < ((uintptr_t)mbi.BaseAddress + mbi.RegionSize) >> 12; p++)
				pt[p] = 2;
		}
		if (!mbi.RegionSize)
			break;
	}
	for (i = 0; i < g_mt_n; i++)
		for (a = g_mt_lo[i] >> 12; a < (g_mt_hi[i] + 0xFFF) >> 12 && a < npages; a++)
			pt[a] = 1;
	if (check || (relink && relink[0]))
		merge_relink(s, w, pt, check, relink);
	if (!check)
		goto out;
	for (i = 0; i < g_mt_n; i++) {
		const DWORD *wp = (const DWORD *)((g_mt_lo[i] + 3) & ~(uintptr_t)3);
		const DWORD *e = (const DWORD *)(g_mt_hi[i] & ~(uintptr_t)3);

		for (; wp < e; wp++) {
			unsigned c = pt[*wp >> 12];

			if (c >= 3) {
				if (!cnt[c - 3]++)
					eg[c - 3] = (uintptr_t)wp;
				edges++;
			}
		}
		words += (unsigned long long)(e - wp);
	}
	ss_log("  merge check: %llu link(s) from what was taken into saved memory that was not "
	       "taken (%lu ms)\n",
	       edges, (unsigned long)(GetTickCount() - t0));
	for (shown = 0; shown < 24; shown++) {
		char org[96], dif[24];
		int best = -1, pct;

		for (k = 0; k < s->nregs; k++)
			if (cnt[k] && (best < 0 || cnt[k] > cnt[best]))
				best = k;
		if (best < 0)
			break;
		org[0] = 0;
		if (s->regs[best].type == MEM_IMAGE) {
			char path[MAX_PATH], *b;

			if (GetModuleFileNameA((HMODULE)s->regs[best].alloc_base, path, sizeof(path))) {
				b = strrchr(path, '\\');
				lstrcpynA(org, b ? b + 1 : path, sizeof(org));
			}
		} else
			gameheap_va_origin(s->regs[best].alloc_base, org, sizeof(org));
		pct = merge_differs(s, w, s->regs[best].base, s->regs[best].size);
		if (pct < 0)
			lstrcpyA(dif, "gone now");
		else if (!pct)
			lstrcpyA(dif, "same now");
		else
			wsprintfA(dif, "%d%% differs", pct);
		ss_log("    %7u -> %08lX +%lX %s%s%s, %s, e.g. from %08lX\n", cnt[best],
		       (unsigned long)s->regs[best].base, (unsigned long)s->regs[best].size,
		       s->regs[best].type == MEM_IMAGE    ? "image"
		       : s->regs[best].type == MEM_MAPPED ? "mapped"
							  : "private",
		       org[0] ? ", " : "", org, dif, (unsigned long)eg[best]);
		cnt[best] = 0;
	}
	(void)words;
	k = gameheap_merge_proof();
	if (k > 0)
		ss_log("  merge check: %d arena block(s) fail their header check\n", k);
out:
	if (pt)
		VirtualFree(pt, 0, MEM_RELEASE);
	if (cnt)
		VirtualFree(cnt, 0, MEM_RELEASE);
}

static unsigned merge_swap_word(uintptr_t was, uintptr_t now)
{
	unsigned hits = 0;
	int i;

	for (i = 0; i < g_mt_n; i++) {
		DWORD *wp = (DWORD *)((g_mt_lo[i] + 3) & ~(uintptr_t)3);
		DWORD *e = (DWORD *)(g_mt_hi[i] & ~(uintptr_t)3);
		DWORD old;

		if (!VirtualProtect((void *)g_mt_lo[i], g_mt_hi[i] - g_mt_lo[i], PAGE_READWRITE,
				    &old))
			continue;
		for (; wp < e; wp++)
			if (*wp == (DWORD)was) {
				*wp = (DWORD)now;
				hits++;
			}
		VirtualProtect((void *)g_mt_lo[i], g_mt_hi[i] - g_mt_lo[i], old, &old);
	}
	return hits;
}

/* What was taken names the saving launch's windows, and a game that compares
 * GetForegroundWindow() against its own never finds itself in front. Paired by
 * class, then by order among windows of one class. */
static void merge_windows(const Slot *s)
{
	uintptr_t lw[SS_MAX_WND];
	char lcls[SS_MAX_WND][64];
	int nl = wnd_list(lw, lcls, SS_MAX_WND), used[SS_MAX_WND] = { 0 }, a, b;

	if (!s->nwnd)
		ss_log("  merge: the save recorded no windows, so window handles in it are "
		       "left as they were\n");
	for (a = 0; a < s->nwnd && a < SS_MAX_WND; a++) {
		uintptr_t ldc;
		unsigned hw = 0, hd = 0;

		for (b = 0; b < nl; b++)
			if (!used[b] && !strcmp(s->wnd_cls[a], lcls[b]))
				break;
		if (b == nl) {
			ss_log("  merge: saved window %08lX (%s) has no live window of that class\n",
			       (unsigned long)s->wnd[a], s->wnd_cls[a]);
			continue;
		}
		used[b] = 1;
		if (lw[b] != s->wnd[a])
			hw = merge_swap_word(s->wnd[a], lw[b]);
		ldc = s->wnd_dc[a] ? wnd_own_dc(lw[b]) : 0;
		if (ldc && ldc != s->wnd_dc[a])
			hd = merge_swap_word(s->wnd_dc[a], ldc);
		ss_log("  merge: saved window %08lX (%s) is %08lX now - %u word(s) changed, "
		       "%u for its DC\n", (unsigned long)s->wnd[a], s->wnd_cls[a],
		       (unsigned long)lw[b], hw, hd);
	}
}

/* [a, end) is in the save's addresses; it lands `shift` bytes on, which is
 * nonzero for a module this launch loaded somewhere else. */
static int merge_take_span(const Slot *s, Window *w, const char *ent, uintptr_t a,
			   uintptr_t end, int commit, intptr_t shift)
{
	unsigned long long kb = 0;
	int i, ok = 1, n = 0;

	for (i = 0; i < s->nregs; i++) {
		uintptr_t lo = s->regs[i].base, hi = lo + s->regs[i].size;
		void *dst;
		DWORD old;

		if (lo < a)
			lo = a;
		if (hi > end)
			hi = end;
		if (lo >= hi)
			continue;
		dst = (void *)(lo + shift);
		if (commit && !VirtualAlloc(dst, hi - lo, MEM_COMMIT, PAGE_READWRITE)) {
			ss_log("  merge: %p+%lX could not be committed (%lu)\n", dst,
			       (unsigned long)(hi - lo), GetLastError());
			ok = 0;
			continue;
		}
		if (!VirtualProtect(dst, hi - lo, PAGE_READWRITE, &old)) {
			ss_log("  merge: %p+%lX is not writable (%lu)\n", dst,
			       (unsigned long)(hi - lo), GetLastError());
			ok = 0;
			continue;
		}
		if (!xs_fetch(s, w, lo, dst, hi - lo))
			ok = 0;
		else
			merge_note_span(lo + shift, hi + shift);
		VirtualProtect(dst, hi - lo, old, &old);
		kb += (hi - lo) >> 10;
		n++;
	}
	ss_log("  merge: %s at %p, %llu KB in %d saved region(s)%s%s\n", ent, (void *)(a + shift),
	       kb, n, shift ? ", moved with its module" : "",
	       ok ? "" : " - SOME COULD NOT be taken");
	return n > 0;
}

static int soak_knob(const char *name, int def);

static int merge_load(Slot *s)
{
	char take[512], take_all[512], sites_s[256];
	unsigned sites[32];
	int nsites = 0, regions = 0, blocks, i, threads, tls = 0;
	DWORD t0 = GetTickCount();
	Window w;
	MergeCtx m;
	char *p, *tok;
	CRITICAL_SECTION cs_keep[SS_OWN_CS];
	void *arena_keep[SS_NARENA];
	static uintptr_t ck_at[SS_MAX_MODS], ck_val[SS_MAX_MODS];

	take[0] = sites_s[0] = 0;
	ss_getenv("D3D9SW_MERGE_TAKE", take, sizeof(take));
	ss_getenv("D3D9SW_MERGE_SITES", sites_s, sizeof(sites_s));
	lstrcpynA(take_all, take, sizeof(take_all));
	CharLowerA(take_all);
	for (p = sites_s; *p && nsites < 32;) {
		char *e;
		unsigned long v = strtoul(p, &e, 16);

		if (e == p)
			break;
		sites[nsites++] = (unsigned)v;
		p = *e == ',' ? e + 1 : e;
	}
	ss_log("load: MERGE - taking only the listed state from the save (%s | sites %s)\n",
	       take[0] ? take : "no regions", sites_s[0] ? sites_s : "none");
	preload_missing_modules(s);
	memset(&w, 0, sizeof(w));
	w.sect = s->sect;
	w.total = s->bytes;
	m.s = s;
	m.w = &w;
	ss_prime_module_table();
	dsh_quiet();
	collect_threads();
	suspend_all();
	/* Our threads are this launch's, so our locks stay as they are live, and
	 * so do the scratch arenas this process made for itself. */
	for (i = 0; i < g_own_cs_n; i++)
		cs_keep[i] = *g_own_cs[i];
	arena_hold(arena_keep);
	g_mt_n = 0;
	/* Every module keeps this launch's /GS cookie: the frames of the threads
	 * left live are encoded with it, and a taken data section carries the
	 * saving launch's. Saved frames moved by D3D9SW_MERGE_THREADS are
	 * re-encoded to it. */
	for (i = 0; i < g_ctl->nmods && i < SS_MAX_MODS; i++) {
		ck_at[i] = mod_gs_cookie_at(g_ctl->mod_lo[i]);
		ck_val[i] = ck_at[i] ? *(const uintptr_t *)ck_at[i] : 0;
	}
	/* Before the regions: a region may hold the ledger the pairing reads. */
	blocks = nsites ? gameheap_merge_sites(sites, nsites, merge_fetch, &m) : 0;
	for (tok = take; *tok;) {
		char *e = strchr(tok, ',');

		if (e)
			*e = 0;
		while (*tok == ' ')
			tok++;
		if (*tok)
			regions += merge_take_region(s, &w, tok);
		if (!e)
			break;
		tok = e + 1;
	}
	for (i = 0; i < g_own_cs_n; i++)
		*g_own_cs[i] = cs_keep[i];
	for (i = 0; i < g_ctl->nmods && i < SS_MAX_MODS; i++)
		if (ck_at[i] && *(uintptr_t *)ck_at[i] != ck_val[i]) {
			DWORD old;

			VirtualProtect((void *)ck_at[i], sizeof(uintptr_t), PAGE_READWRITE, &old);
			*(uintptr_t *)ck_at[i] = ck_val[i];
			VirtualProtect((void *)ck_at[i], sizeof(uintptr_t), old, &old);
			ss_log("  merge: %s keeps this launch's /GS cookie\n", g_ctl->mod_name[i]);
		}
	arena_put_back(arena_keep);
	gameheap_merge_chunks();
	/* D3D9SW_MERGE_THREADS=1: the saved threads' stacks and registers too, so
	 * the game resumes in the save's frame and not this launch's, whose
	 * locals name this launch's objects. Before the reloc, which has to shift
	 * the return addresses on those stacks into modules that moved; the
	 * cookie re-encodes after it, so it cannot shift one of those. */
	threads = soak_knob("D3D9SW_MERGE_THREADS", 0) > 0;
	if (threads) {
		uintptr_t exe = (uintptr_t)GetModuleHandleA(NULL), self = (uintptr_t)merge_load;
		int k;

		for (k = 0; k < g_ctl->nmods; k++) {
			char pat[40];

			_snprintf(pat, sizeof(pat), "%s+", g_ctl->mod_name[k]);
			pat[sizeof(pat) - 1] = 0;
			CharLowerA(pat);
			if (g_ctl->mod_lo[k] == exe || (self >= g_ctl->mod_lo[k] && self < g_ctl->mod_hi[k]) ||
			    strstr(take_all, pat))
				g_ctl->mod_rewound[k] = 1;
		}
		tls = xs_transplant(s);
		for (k = 0; k < g_ctl->nxs; k++)
			if (g_ctl->xs[k].state == 1)
				merge_note_span((uintptr_t)((intptr_t)g_ctl->xs[k].old_sp + g_ctl->xs[k].delta),
						(uintptr_t)((intptr_t)g_ctl->xs[k].old_hi + g_ctl->xs[k].delta));
	}
	merge_reloc(s);
	if (threads) {
		xs_cookies_write();
		xs_tid_remap();
		ss_log("  merge: saved threads carried over, %d TLS block(s)\n", tls);
	}
	merge_windows(s);
	/* The taken memory names the saving launch's events by value. */
	xs_handles_pin(s);
	if (blocks < 0)
		ss_log("  merge: the save has no block ledger where this launch has one - "
		       "sites skipped (both launches need D3D9SW_GHPLACE=1)\n");
	{
		char relink[512];

		relink[0] = 0;
		ss_getenv("D3D9SW_MERGE_RELINK", relink, sizeof(relink));
		if (merge_check_mode() || relink[0])
			merge_linkage(s, &w, merge_check_mode(), relink);
	}
	win_close(&w);
	InterlockedExchange(&g_merged_clear, 1);
	resume_all(0);
	merge_refocus();
	dsh_play();
	xa2_sw_restored();
	xa2_sw_resume();
	ss_log("load: MERGE done - %d region(s), %d block(s), %lu ms\n", regions,
	       blocks < 0 ? 0 : blocks, (unsigned long)(GetTickCount() - t0));
	return 1;
}
#endif

static int do_load(int slotno)
{
	Slot *s = &g_ctl->slots[slotno];
	void *arena_keep[SS_NARENA];
	Window w;
	unsigned long long pos = 0;
	int i, j, restored = 0, skipped = 0, tls_done = 0, blocked = 0;
	int skipped_excl = 0, skipped_mapped = 0, skipped_dead = 0, skipped_xs = 0;
	int skipped_small = 0;
	DWORD xtid;
	int handskip = 0, handscrib = 0;
	DWORD load_t0 = GetTickCount();

	/* Only when there is nothing in memory, so a session that took its own save
	 * restores that one and the file is the fallback rather than the rule. */
	if (!s->valid && slotfile_mode())
		slotfile_read(slotno, s);
	if (!s->valid)
		return 0;
	ss_op_set("load");
	g_ctl->nlwnd = wnd_list(g_ctl->lwnd, g_ctl->lwnd_cls, SS_MAX_WND);
	g_ctl->nxs = 0;
	g_ctl->xs_peb_old = 0;
	g_ctl->xs_ctl_old = 0;
	g_ctl->load_prov = slot_provenance(s);
	if (g_ctl->load_prov != PROV_SAME_PROCESS && g_ctl->load_prov != PROV_UNKNOWN)
		g_ctl->foreign_seen = 1;
	ss_log("  provenance: saved by pid %lu, loading in pid %lu - %s\n",
	       (unsigned long)s->save_pid, (unsigned long)GetCurrentProcessId(),
	       prov_name(g_ctl->load_prov));
	if (slotfile_mode() && slotno != SS_SCRATCH && g_ctl->load_prov != PROV_SAME_PROCESS)
		xp_check(slotno, s);
#if defined(_M_IX86) || defined(__i386__)
	if (merge_mode() && g_ctl->load_prov != PROV_SAME_PROCESS)
		return merge_load(s);
#endif
	g_ctl->mf_short = -1;
	g_rsv_n = 0;
	if (slotfile_mode())
		cfg_compare_with_slot(slotno);
	if (slotfile_mode() && slotno != SS_SCRATCH)
		mf_check(slotno, s);
	/* Sampled here, before a single byte moves, because this is the only moment
	 * that answers the question. A heap created after the snapshot exists right
	 * now and will not exist in a moment; if the record of it is not taken on
	 * this line then the comparison after the restore has nothing to miss, and
	 * the check reports health exactly as it did before it was written. */
	/* A rollback runs over the world a failed load from another process just
	 * wrote, with every thread still held. The heaps are that process's, the
	 * room is that save's, and whoever was inside dsound when the failed load
	 * froze them still is: so no heap walk, no room test, no audio calls, and
	 * no block map. The regions go back whole, which is what they are for. */
	if (!g_ctl->held_n)
		heap_check("as the restore begins");
	/* Make the shared active witness THIS slot's, so room_check and the restore
	 * verification compare against the world this save was taken in rather than
	 * whatever was saved most recently into another slot. Every g_ctl->wit_*
	 * reader downstream then sees the right save without being touched. */
	if (s->wit_valid) {
		g_ctl->wit_ent = s->wit_ent;
		g_ctl->wit_x = s->wit_x;
		g_ctl->wit_y = s->wit_y;
		g_ctl->wit_map = s->wit_map;
		g_ctl->wit_valid = 1;
	}
	/* Before anything moves, so a refusal costs nothing at all. */
	if (!g_ctl->held_n && !room_check())
		return 0;
	if (!g_ctl->held_n && xs_foreign_load() && transplant_mode() && !xs_spares_topup(s))
		return 0;
	if (!g_ctl->held_n)
		preload_missing_modules(s);
	roster_save();
	poke_init();
	arena_hold(arena_keep);
	g_clob_slot = -1;
	if (clobber_mode() && clobber_marks())
		memset(g_clob_ok, 0, SS_MAX_REGIONS);
	/* Second walk, same shape as the first: lock while the process runs, walk
	 * once it is frozen. No settle loop on this path, so the locks can be
	 * taken before the only suspend there is. */
	g_blk_ready = 0;
	g_blk_match_n = 0;
	{
		int want = !ss_under_wine() && blk_mode() && g_blk_save_n && blk_alloc() &&
			   !g_ctl->held_n;
		int held = g_ctl->held_n;

		/* Same reason as at save, and more pressing: here we are the writer,
		 * putting hundreds of megabytes back into memory the card may still
		 * be feeding from. A buffer left playing across the restore is read
		 * by the hardware while its contents change underneath, and filled
		 * by the driver into pages we are simultaneously overwriting.
		 * dsh_restore at the end puts the cursors back and starts the ones
		 * that were playing again.
		 *
		 * The note comes first: once the buffers are stopped there is
		 * nothing left to observe about what the present sounded like. */
		if (!held) {
			dsh_mark_present();
			dsh_quiet();
			ss_op_set("load-park");
			park_audio_gpu();
		}
		if (want)
			blk_lock_all();
		ss_op_set("load-freeze");
		collect_threads();
		suspend_all();
		if (held)
			release_held();
		if (want) {
			int capped = 0;

			g_blk_now_n = blk_walk_locked(g_blk_now, SS_BLK_CAP, &capped);
			blk_sort(g_blk_now, (int)g_blk_now_n);
			g_blk_match_n = blk_match();
			g_blk_ready = g_blk_match_n > 0;
			/* Cleared per restore, so a fault report never describes what
			 * some earlier restore did with an address. */
			if (g_blk_match_n && blk_own_alloc()) {
				memset(g_blk_wrote, 0, g_blk_match_n);
				memset(g_blk_depth, 0, g_blk_match_n);
				memset(g_blk_own, 0, g_blk_match_n);
				memset(g_blk_sys, 0, g_blk_match_n);
				memset(g_blk_parent, 0xFF,
				       g_blk_match_n * sizeof(unsigned));
			}
			if (capped)
				ss_log("  heap blocks: hit the %u block ceiling at "
				       "restore\n", SS_BLK_CAP);
		}
	}
	g_excl_wow64 = xs_foreign_load();
	build_exclusions();
	g_excl_wow64 = 0;
	/* The module set, checked the way the thread set already is.
	 *
	 * A module that vanished between the save and now is the dangerous one: the
	 * game's restored memory still holds pointers into where it used to be, and
	 * the loader - whose list we never rewind - no longer agrees that anything
	 * is there. That is a call into freed address space, which is what
	 * "executing 70DB1303 (committed data)" was. A module that appeared is
	 * milder but still a disagreement worth naming. */
	{
		int mi, mj, gone = 0, fresh = 0, moved = 0, named = 0;

		for (mi = 0; mi < s->nmods; mi++) {
			int found = 0;

			for (mj = 0; mj < g_ctl->nmods; mj++) {
				if (s->mod_lo[mi] != g_ctl->mod_lo[mj])
					continue;
				found = 1;
				if (s->mod_hi[mi] != g_ctl->mod_hi[mj])
					moved++;
				break;
			}
			if (found)
				continue;
			gone++;
			if (named++ < 8)
				ss_log("  modules: %s was mapped at %p-%p when we saved and is "
				       "NOT mapped now - restored pointers into it lead "
				       "nowhere\n",
				       s->mod_name[mi], (void *)s->mod_lo[mi],
				       (void *)s->mod_hi[mi]);
		}
		for (mj = 0; mj < g_ctl->nmods; mj++) {
			int found = 0;

			for (mi = 0; mi < s->nmods; mi++)
				if (s->mod_lo[mi] == g_ctl->mod_lo[mj]) {
					found = 1;
					break;
				}
			if (found)
				continue;
			fresh++;
			if (named++ < 12)
				ss_log("  modules: %s at %p is mapped now and was not when we "
				       "saved\n",
				       g_ctl->mod_name[mj], (void *)g_ctl->mod_lo[mj]);
		}
		if (gone || fresh || moved)
			ss_log("  modules: %d of %d gone, %d new, %d resized%s\n", gone,
			       s->nmods, fresh, moved,
			       gone ? " <<< A MODULE THE SAVE POINTED INTO IS GONE" : "");
		else
			ss_log("  modules: %d, unchanged since the save\n", s->nmods);
	}
	/* After build_exclusions, which is what knows where the heaps are and what
	 * we leave in the present - both of which decide what counts as a root -
	 * and before any memory moves, so the graph it walks is the one the game
	 * actually has right now. */
	if (g_blk_ready && blk_owner_filter() >= 3) {
		blk_reach_mark();
		blk_sys_mark();
	}
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
				else if (s->starts[j] && start_now(s, s->starts[j]) == g_ctl->starts[i])
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
			ss_log("  thread-set invariant: %d at save, %d live now, "
			       "%d newer (%d same role), %d gone%s\n",
			       s->nids, g_ctl->nids, fresh, recycled, gone,
			       gone ? " <<< restored state refers to exited threads"
				    : "");
		else
			ss_log("  thread-set invariant: %d threads, unchanged "
			       "since the save\n", s->nids);
		if (fresh && policy() == POLICY_REFUSE) {
			ss_log("load: refused, %d thread(s) newer than the save "
			       "(D3D9SW_REWIND_NEWTHREADS=hold or run to override)\n",
			       fresh);
			load_refused_resume();
			return 0;
		}
		if (xs_foreign_load() && transplant_mode()) {
			const char *why = xs_unready(s);

			if (why) {
				ss_log("load: refused - %s. Nothing was written\n", why);
				load_refused_resume();
				return 0;
			}
		}
		g_ctl->last_fresh = fresh;
		g_ctl->last_gone = gone;
		g_ctl->last_recycled = recycled;
	}

	/* Nothing is written until every region is known to be restorable. A
	 * partial rewind is worse than none: the process keeps running with most
	 * of its state in the past and the rest in the present, which is how a
	 * silent eight-region failure turned into a game that misbehaved without
	 * anything reporting an error. */
	/* A heap that grew differently since the save can hold one allocation now
	 * where the save had two: DDPR's dat12.bin heap had 06E70000 spread over
	 * the saved 06EA0000, the three regions there were skipped, and the heap's
	 * restored lists pointed into bytes that never came back - C0000374 on the
	 * next allocation. Nothing in such an allocation is held, so all of it is
	 * the past's to define: release it and let the saved layout be rebuilt
	 * below. Never a live heap's own base, never anything excluded, and only
	 * an allocation known to be the past's - a segment of a heap we rewind, or
	 * reserved by a module we rewind. Releasing one AudioSes still used took
	 * wdmaud's reset job down with it. */
	if (!g_ctl->held_n) {
		HANDLE hp[SS_MAX_HEAPS];
		DWORD nh = GetProcessHeaps(SS_MAX_HEAPS, hp);
		int freed = 0;

		if (nh > SS_MAX_HEAPS)
			nh = SS_MAX_HEAPS;
		for (i = 0; i < s->nregs; i++) {
			MEMORY_BASIC_INFORMATION mbi;
			uintptr_t la, le;
			DWORD k;

			if (s->regs[i].type != MEM_PRIVATE ||
			    !VirtualQuery((LPCVOID)s->regs[i].base, &mbi, sizeof(mbi)) ||
			    mbi.State == MEM_FREE || mbi.Type != MEM_PRIVATE ||
			    (uintptr_t)mbi.AllocationBase == s->regs[i].alloc_base)
				continue;
			la = le = (uintptr_t)mbi.AllocationBase;
			while (VirtualQuery((LPCVOID)le, &mbi, sizeof(mbi)) == sizeof(mbi) &&
			       (uintptr_t)mbi.AllocationBase == la)
				le += mbi.RegionSize;
			if (region_excluded(la, le - la))
				continue;
			for (k = 0; k < nh; k++)
				if ((uintptr_t)hp[k] >= la && (uintptr_t)hp[k] < le)
					break;
			if (k < nh)
				continue;
			if (!relayout_rewound(la)) {
				char o[64] = "";

				gameheap_va_origin(la, o, sizeof(o));
				ss_log("  region %p+%lx sits in allocation %p+%lx laid out "
				       "differently from the save, not known to be rewound "
				       "state (reserved by %s) - kept\n",
				       (void *)s->regs[i].base, (unsigned long)s->regs[i].size,
				       (void *)la, (unsigned long)(le - la), o[0] ? o : "?");
				continue;
			}
			if (VirtualFree((void *)la, 0, MEM_RELEASE)) {
				freed++;
				ss_log("  region %p+%lx sits in allocation %p+%lx laid out "
				       "differently from the save (alloc %p there) - released "
				       "so the saved layout can be rebuilt\n",
				       (void *)s->regs[i].base, (unsigned long)s->regs[i].size,
				       (void *)la, (unsigned long)(le - la),
				       (void *)s->regs[i].alloc_base);
			}
		}
		if (freed)
			ss_log("  relayout: %d live allocation(s) released\n", freed);
	}
	g_ndead_reg = 0;
	for (i = 0; i < s->nregs; i++) {
		MEMORY_BASIC_INFORMATION mbi;
		uintptr_t base = s->regs[i].base, size = s->regs[i].size;

		/* The thread set can differ from the save, so re-test: what was
		 * ordinary memory then may be a live stack or TEB now. */
		if (region_excluded(base, size)) {
			ss_log("  region %p+%lx is excluded now but was saved\n", (void *)base,
			       (unsigned long)size);
			/* Counted apart from the other failures, because refusing over
			 * this one protects nothing.
			 *
			 * Every other way a region fails the test leaves a real choice:
			 * the memory is still there and we are declining to write it
			 * because we are not sure it is the same memory. This case has
			 * no such choice. The region is a live thread's stack or TEB
			 * now, so the only two options are to skip it or to write the
			 * game's old bytes over a running thread's stack, and the
			 * second is never right. Whatever the game had there is already
			 * gone in the present; the restore cannot bring it back and
			 * refusing does not preserve it.
			 *
			 * It became worth separating when the hardware backend arrived.
			 * The display driver runs sixteen threads of its own and cycles
			 * them, so freed stack memory gets captured as ordinary private
			 * memory at the save and is a live stack again by the restore -
			 * four 32 KB regions out of 264 was enough to refuse a restore
			 * that was otherwise entirely sound. Set D3D9SW_EXCLSKIP=0 to
			 * go back to refusing. */
			skipped_excl++;
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
			/* A live file mapping cannot take the saved private bytes - a write
			 * into a section view goes to the file or faults, it is not a memcpy
			 * target - so there is nothing to force and no choice to refuse over.
			 * The game re-mapped an asset here in this session; it will read from
			 * the mapping, not from the private data we saved. Hold it out like a
			 * regenerable region and let the load go ahead. This is the bulk of
			 * the cross-session "belongs elsewhere" set: private at save, mapped
			 * now. D3D9SW_MAPSKIP=0 goes back to counting it as unrestorable. */
			if (mbi.Type == MEM_MAPPED && mapskip_mode() &&
			    g_ndead_reg < (int)(sizeof(g_dead_reg) / sizeof(g_dead_reg[0]))) {
				g_dead_reg[g_ndead_reg++] = i;
				ss_log("  region %p+%lx is a live file mapping now (was type "
				       "%lx) - the game re-maps it, held out\n",
				       (void *)base, (unsigned long)size,
				       (unsigned long)s->regs[i].type);
				skipped_mapped++;
				continue;
			}
			/* A module this launch loaded where the old one had private
			 * memory. Writing the old bytes over its code and putting back
			 * read-write protection kills the next thread that calls its
			 * entry point. */
			if (mbi.Type == MEM_IMAGE && s->regs[i].type != MEM_IMAGE &&
			    g_ndead_reg < (int)(sizeof(g_dead_reg) / sizeof(g_dead_reg[0]))) {
				g_dead_reg[g_ndead_reg++] = i;
				ss_log("  region %p+%lx is a live module image now (was type "
				       "%lx) - held out\n",
				       (void *)base, (unsigned long)size,
				       (unsigned long)s->regs[i].type);
				skipped_mapped++;
				continue;
			}
			if (mbi.Type != s->regs[i].type ||
			    (uintptr_t)mbi.AllocationBase != s->regs[i].alloc_base) {
				uintptr_t abase = s->regs[i].alloc_base, aend = 0;
				int k;

				for (k = 0; k < s->nregs; k++)
					if (s->regs[k].alloc_base == abase &&
					    s->regs[k].base + s->regs[k].size > aend)
						aend = s->regs[k].base + s->regs[k].size;
				if (dead_stack_region(s, i, abase, aend)) {
					ss_log("  region %p+%lx was the stack of a thread no "
					       "longer here (alloc %p+%lx), reused since - "
					       "left out\n",
					       (void *)base, (unsigned long)size,
					       (void *)abase, (unsigned long)(aend - abase));
					skipped_dead++;
					continue;
				}
				if ((xtid = xs_stack_region(s, i)) != 0) {
					ss_log("  region %p+%lx is saved thread %lu's stack, "
					       "now inside another allocation - the "
					       "transplant carries it, left out\n",
					       (void *)base, (unsigned long)size,
					       (unsigned long)xtid);
					skipped_xs++;
					continue;
				}
				if (small_skip(s, i)) {
					ss_log("  region %p+%lx now belongs elsewhere (alloc "
					       "%p) - small, left out (D3D9SW_SKIP_SMALL)\n",
					       (void *)base, (unsigned long)size,
					       mbi.AllocationBase);
					skipped_small++;
					continue;
				}
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
				DWORD err = GetLastError();

				if (dead_stack_region(s, i, abase, aend)) {
					ss_log("  region %p+%lx was the stack of a thread no "
					       "longer here (alloc %p+%lx), its addresses reused "
					       "since - left out\n",
					       (void *)base, (unsigned long)size,
					       (void *)abase, (unsigned long)(aend - abase));
					skipped_dead++;
				} else if ((xtid = xs_stack_region(s, i)) != 0) {
					ss_log("  region %p+%lx is saved thread %lu's stack, "
					       "and cannot be recommitted here - the "
					       "transplant carries it, left out\n",
					       (void *)base, (unsigned long)size,
					       (unsigned long)xtid);
					skipped_xs++;
				} else if (small_skip(s, i)) {
					ss_log("  region %p+%lx cannot be recommitted, err=%lu - "
					       "small, left out (D3D9SW_SKIP_SMALL)\n",
					       (void *)base, (unsigned long)size, err);
					skipped_small++;
				} else {
					ss_log("  region %p+%lx cannot be recommitted, err=%lu, "
					       "alloc %p+%lx\n",
					       (void *)base, (unsigned long)size, err,
					       (void *)abase, (unsigned long)(aend - abase));
					skipped++;
				}
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

	if (skipped_excl) {
		if (!exclskip_mode())
			skipped += skipped_excl;
		else
			ss_log("  %d region(s) skipped because they are excluded now - "
			       "live stacks or TEBs at addresses that held ordinary memory "
			       "at the save. The restore goes ahead without them\n",
			       skipped_excl);
	}
	if (skipped_dead)
		ss_log("  %d region(s) left out - stacks of threads gone since the save, "
		       "their addresses reused; no restored thread stands on them\n",
		       skipped_dead);
	if (skipped_xs)
		ss_log("  %d region(s) left out - saved threads' stacks at addresses taken "
		       "here; the transplant moves their bytes onto the live threads\n",
		       skipped_xs);
	if (skipped_small)
		ss_log("  %d small region(s) left out under D3D9SW_SKIP_SMALL - a fault "
		       "inside one of them means something still used it\n",
		       skipped_small);
	if (skipped_mapped)
		ss_log("  %d region(s) held out - private at the save, a live file mapping "
		       "now; the game re-maps them, so the restore goes ahead without "
		       "them\n",
		       skipped_mapped);
	if (skipped) {
		if (exclforce_mode()) {
			ss_log("  EXCLFORCE: %d of %d region(s) are unrestorable (belong "
			       "elsewhere now, or will not recommit) - NOT refusing; each is "
			       "written where its address is committed and left in the present "
			       "where it is not. The whole snapshot goes in over the live "
			       "process; expect it to cry\n",
			       skipped, s->nregs);
		} else {
			ss_log("load: refused, %d of %d regions unrestorable\n", skipped,
			       s->nregs);
			rsv_release();
			load_refused_resume();
			return 0;
		}
	}

	memset(&w, 0, sizeof(w));
	w.sect = s->sect;
	w.total = s->bytes;

	/* The write set: what actually changed between the save and now.
	 *
	 * Every other measurement in this file describes what we are permitted to
	 * put back. This one says how much there is to put back at all. A captured
	 * region that is byte-identical now contributes nothing - writing it is a
	 * no-op - so the regions that differ are the whole of the game's real
	 * mutable state, and which heaps they sit in is the question that decides
	 * whether the allocator has to be split per allocation or merely left
	 * alone. 410 MB gets captured and copied; if the part that moved is a few
	 * MB confined to the game's own heaps, almost all of that work is spent
	 * restoring data that was never in danger.
	 *
	 * Necessarily before the write-back, because afterwards everything matches
	 * by construction and the answer is always zero. Reads the snapshot a
	 * second time inside the suspend, so it stays off unless asked for. */
	{
		char wsv[8];

		if (ss_getenv("D3D9SW_WRITESET", wsv, sizeof(wsv)) != 0 && wsv[0] != '0') {
			Window ws;
			unsigned long long off = pos, chg = 0, seen = 0;
			unsigned long long per[SS_MAX_HEAPS + 1], cnt[SS_MAX_HEAPS + 1];
			int nchanged = 0, unchecked = 0, hi;

			memset(per, 0, sizeof(per));
			memset(cnt, 0, sizeof(cnt));
			memset(&ws, 0, sizeof(ws));
			ws.sect = s->sect;
			ws.total = s->bytes;
			for (i = 0; i < s->nregs; i++) {
				void *base = (void *)s->regs[i].base;
				SIZE_T size = (SIZE_T)s->regs[i].size;
				unsigned long long fd = 0, words = 0, was = 0, now = 0;
				DWORD old;
				int r;

				seen += size;
				/* Same reason the verify pass does this: a region saved
				 * as PAGE_NOACCESS would fault on being read. */
				if (!VirtualProtect(base, size, PAGE_EXECUTE_READWRITE, &old)) {
					unchecked++;
					off += size;
					continue;
				}
				r = win_cmp(&ws, off, base, size, &fd, &words, &was, &now);
				VirtualProtect(base, size, old, &old);
				if (r < 0) {
					unchecked++;
				} else if (!r) {
					unsigned long long b = words * sizeof(unsigned long long);

					nchanged++;
					chg += b;
					hi = heap_index_of(s->regs[i].base);
					per[hi < 0 ? SS_MAX_HEAPS : hi] += b;
					cnt[hi < 0 ? SS_MAX_HEAPS : hi]++;
				}
				off += size;
			}
			win_close(&ws);
			ss_log("  write set: %d of %d region(s) differ, %.1f MB changed of "
			       "%.1f MB captured (%.2f%%), %d unchecked\n",
			       nchanged, s->nregs, (double)chg / (1024.0 * 1024.0),
			       (double)seen / (1024.0 * 1024.0),
			       seen ? 100.0 * (double)chg / (double)seen : 0.0, unchecked);
			for (hi = 0; hi <= SS_MAX_HEAPS; hi++) {
				if (!per[hi])
					continue;
				if (hi == SS_MAX_HEAPS)
					ss_log("    %.2f MB over %llu region(s) outside every "
					       "heap\n",
					       (double)per[hi] / (1024.0 * 1024.0), cnt[hi]);
				else
					ss_log("    %.2f MB over %llu region(s) in the %s heap "
					       "at %p, which we %s\n",
					       (double)per[hi] / (1024.0 * 1024.0), cnt[hi],
					       g_ctl->heap_name[hi], (void *)g_ctl->heap_lo[hi],
					       g_ctl->heap_ours[hi] ? "rewind"
								    : "leave in the present");
			}
		}
	}

	/* Checked inside this loop rather than after it, because the loop puts each
	 * region's saved protection back as it goes - a region that was PAGE_NOACCESS
	 * at save time would fault on being read a moment later. Here it is still
	 * PAGE_EXECUTE_READWRITE, so the comparison is free of that problem. */
	ss_op_set("load-copy");
	{
		Window wv;
		unsigned long long vwords = 0;
		int vdiffer = 0, vunchecked = 0, vnamed = 0, vmode = verify_mode();
		int pstraddle = 0, pdelta = 0, pnamed = 0;
		int excl_whole = 0, excl_part = 0;
		uintptr_t img_lo = 0, img_hi = 0;
		void *img_live;
		int in_img, heap_mg = 0;
		LARGE_INTEGER v0, v1, vf;

		QueryPerformanceFrequency(&vf);
		QueryPerformanceCounter(&v0);
		g_excl_skip_bytes = 0;
#if defined(_M_IX86) || defined(__i386__)
		g_imgm_took = g_imgm_named = 0;
		g_heapm_took = g_heapm_named = 0;
#endif
		memset(g_reg_wr, 0, sizeof(g_reg_wr));
		g_last_loaded = s;
#if defined(_M_IX86) || defined(__i386__)
		if (xs_foreign_load() && img_merge_mode()) {
			const unsigned char *mz = (const unsigned char *)GetModuleHandleA(NULL);
			const IMAGE_NT_HEADERS *nt =
				(const IMAGE_NT_HEADERS *)(mz + ((const IMAGE_DOS_HEADER *)mz)->e_lfanew);

			img_lo = (uintptr_t)mz;
			img_hi = img_lo + nt->OptionalHeader.SizeOfImage;
			heap_mg = heap_merge_mode();
		}
#endif
		memset(&wv, 0, sizeof(wv));
		wv.sect = s->sect;
		wv.total = s->bytes;

		/* Ask the heaps what they own before a single region is written. */
		lfh_find_regions(s);
		live_peer_pages_collect();
		if (der_alloc(s->nregs))
			der_plan_regions(s);
		keep_begin(s);
		heap_cookie_hold(s);

		for (i = 0; i < s->nregs; i++) {
			void *base = (void *)s->regs[i].base;
			SIZE_T size = (SIZE_T)s->regs[i].size;
			unsigned long long here = pos;
			DWORD old;
			int writable;

			/* Never the stack this restore is running on. A saved game region
			 * can land on the helper stack in a fresh session, and painting the
			 * snapshot over the frame executing this loop is not a cry, it is an
			 * instant death that reports nothing. The refuse used to keep us from
			 * ever reaching here with such a region; EXCLFORCE removes the refuse,
			 * so the guard has to be explicit. Correct in every mode - a region
			 * that IS the running stack was never restorable. */
			if (dead_reg_has(i)) {
				if (g_reg_off)
					g_reg_off[i] = pos;
				pos += size;
				continue;
			}
			if (g_ctl && (uintptr_t)base + (uintptr_t)size > g_ctl->helper_lo &&
			    (uintptr_t)base < g_ctl->helper_hi) {
				ss_log("  region %d at %p left untouched, %llu bytes - it is the "
				       "restore thread's own live stack now\n",
				       i, base, (unsigned long long)size);
				if (g_reg_off)
					g_reg_off[i] = pos;
				pos += size;
				continue;
			}

			/* The check pass above reports these as "excluded now" and
			 * lets the load go ahead without them, so this is where they
			 * are actually left out. In another process they are a live
			 * thread's stack or its 64-bit TEB and stack, and painting the
			 * old process's bytes there kills that thread on its 64-bit
			 * side, where no handler of ours runs. Wholly excluded regions
			 * are not protected or written; partly excluded ones keep
			 * their live pages. D3D9SW_EXCLSKIP=0 writes them as before. */
			g_copy_excl = exclskip_mode() &&
				      region_excluded((uintptr_t)base, (uintptr_t)size);
			if (g_copy_excl) {
				uintptr_t pg;
				int all = 1;

				for (pg = (uintptr_t)base; pg < (uintptr_t)base + size; pg += 0x1000)
					if (!region_excluded(pg, 0x1000)) {
						all = 0;
						break;
					}
				if (all) {
					excl_whole++;
					g_excl_skip_bytes += size;
					g_copy_excl = 0;
					if (g_reg_off)
						g_reg_off[i] = pos;
					pos += size;
					continue;
				}
				excl_part++;
			}

			/* Before the protect. A SIGUSR1 from this call reloads eax from
			 * the frame, so the saved status has to already be 0, and the
			 * frame's own region is not protected or written at all. */
			unix_frame_heal();
			if (unix_frame_in_span((uintptr_t)base, (size_t)size)) {
				ss_log("  unix frame: region %d at %p left untouched, %llu "
				       "bytes (live syscall frame)\n",
				       i, base, (unsigned long long)size);
				if (g_reg_off)
					g_reg_off[i] = pos;
				pos += size;
				continue;
			}
			writable =
				VirtualProtect(base, size, PAGE_EXECUTE_READWRITE, &old) != 0;

			/* Here, and not beside the writeback at the end of the loop,
			 * because by then the only protection this region has is the
			 * one we just gave it. Asked there, the probe answered with
			 * its own footprints: every PAGE_READWRITE region read back as
			 * PAGE_EXECUTE_READWRITE and every PAGE_WRITECOPY one as
			 * PAGE_EXECUTE_WRITECOPY, which is this call, not a change the
			 * process made. `old` is the value that was really there. */
			if (writable) {
				int hm = held_mod_at((uintptr_t)base,
						     (uintptr_t)base + (uintptr_t)size);

				if (hm >= 0) {
					pstraddle++;
					if (pnamed++ < 8)
						ss_log("  STRADDLE: region %d at %p (%llu "
						       "byte(s)) lies inside %s, which the "
						       "module roster says is held\n",
						       i, base, (unsigned long long)size,
						       g_ctl->mod_name[hm]);
				}
				if (old != s->regs[i].prot) {
					pdelta++;
					if (pnamed++ < 8)
						ss_log("  PROTECT: region %d at %p is %08lX "
						       "now and was %08lX at the save; "
						       "handing back the saved value\n",
						       i, base, (unsigned long)old,
						       (unsigned long)s->regs[i].prot);
				}
			}
			/* A heap region is normally not copied wholesale: its blocks
			 * are put back one at a time below, and the bytes between
			 * them - which is what the allocator keeps its lists in -
			 * are deliberately left alone, because rewinding the free
			 * lists of a heap whose allocator state straddles the region
			 * boundary kills the process. Under Wine the gameheap is not
			 * that kind of region: a wholesale memcpy races live mmdevapi
			 * TLS, so those bytes go through wine_gameheap_copy instead.
			 * A heap gameheap owns and has made self-contained (LFH off,
			 * D3D9SW_WHOLESALE) keeps its lists inside the region, so on
			 * Windows it is restored whole. */
			int hi_reg = heap_index_of(s->regs[i].base);
			int wine_gh = ss_under_wine() && blk_mode() && g_redirect_heap &&
				      hi_reg >= 0 && g_ctl->heap_h[hi_reg] == g_redirect_heap;
			int gh_whole = !wine_gh && gameheap_owned_heap(s->regs[i].base);
			int by_block = g_blk_ready && heap_rewound_at(s->regs[i].base) &&
				       !gh_whole;

			if (gh_whole) {
				static int wsaid;

				if (wsaid++ < 4)
					ss_log("  WHOLESALE: region %d at %p is an owned "
					       "self-contained heap - restoring the whole "
					       "region, freelist included, not block by "
					       "block\n",
					       i, base);
			}

			if (g_reg_off)
				g_reg_off[i] = pos;
			/* Before a byte is written: where memory has already drifted
			 * from the snapshot, and what it holds there. That is the
			 * value the restore is about to paint over, and having it is
			 * the only way to read the check afterwards - a region the
			 * game carried forward from what we wrote looks exactly like
			 * one that reverted to what was there before, unless both
			 * numbers are on the same line. */
			if (g_clob_off && writable && !by_block && !wine_gh) {
				unsigned long long fd = 0, wds = 0, sv = 0, mv = 0;

				g_clob_off[i] = ~0ull;
				if (win_cmp(&wv, here, base, size, &fd, &wds, &sv, &mv) == 0) {
					g_clob_off[i] = fd;
					g_clob_pre[i] = mv;
				}
			}
			/* The whole pre-image, not just the one sampled word, for the
			 * regions the plan picked. Must happen before the paint. */
			if (writable && !by_block && !wine_gh)
				der_capture(i, base, (size_t)size);
			img_live = NULL;
			in_img = img_hi && (uintptr_t)base >= img_lo &&
				 (uintptr_t)base + size <= img_hi;
			if (writable && !by_block && !wine_gh &&
			    (in_img || (heap_mg &&
					ss_readable((uintptr_t)base, (size_t)size))))
				img_live = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
							PAGE_READWRITE);
			if (img_live)
				memcpy(img_live, base, size);
			if (poke_skipped(s->regs[i].base)) {
				handskip++;
				ss_log("  SKIPREG: region %d at %p left in the present by "
				       "hand, %llu bytes\n",
				       i, base, (unsigned long long)size);
			} else if (by_block)
				blocked++;
			else if (wine_gh) {
				int nb = wine_gameheap_copy(&w, pos, base, size);

				if (nb >= 0) {
					restored++;
					g_reg_wr[i] = 1;
					if (g_clob_ok)
						g_clob_ok[i] = 1;
				} else
					skipped++;
			} else if (!writable && exclforce_mode()) {
				/* Force mode reaches the write phase with regions the
				 * refuse would normally have stopped. One that will not
				 * turn writable is uncommitted or otherwise untouchable;
				 * writing it would fault rather than cry, so leave it in
				 * the present instead of painting into a hole. */
				skipped++;
			} else if (win_copy_except_live(&w, pos, base, size)) {
#if defined(_M_IX86) || defined(__i386__)
				if (img_live && in_img)
					img_merge(s, (uintptr_t)base, img_live, size);
				else if (img_live)
					heap_merge((uintptr_t)base, img_live, size);
#endif
				restored++;
				g_reg_wr[i] = 1;
				/* Only regions we actually wrote are worth asking
				 * about afterwards. One left in the present was
				 * never ours to keep, and would report as taken
				 * back on every single check. */
				if (g_clob_ok)
					g_clob_ok[i] = 1;
			} else
				skipped++;
			if (writable && poke_hit(g_scrib_list, g_scrib_n, s->regs[i].base)) {
				memset(base, 0xCD, size);
				handscrib++;
				ss_log("  SCRIBBLE: region %d at %p filled with 0xCD, %llu "
				       "bytes\n",
				       i, base, (unsigned long long)size);
			}
			/* A region we chose not to write cannot fail to come back as
			 * saved, because we never claimed to put it back. Verify
			 * answers "did the copy land", and there was no copy. Left
			 * unguarded it reported the LFH regions as damage and printed
			 * THE RESTORE DID NOT TAKE over a restore that took perfectly
			 * - the same false alarm the derived mask raised earlier. */
			if (vmode && writable && !by_block && !wine_gh && !g_copy_excl &&
			    !poke_skipped(s->regs[i].base) &&
			    !live_page_in_span((uintptr_t)base, size)) {
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
			/* Hand the derived words back, after the verify above and not
			 * before it. These deliberately do not match the snapshot, so
			 * applying them first made verify count every one as damage:
			 * 4085 words "WRONG" against 4100 held back, which reads as a
			 * restore that did not take and is in fact the feature working.
			 * Verify answers "did the region come back as saved", and that
			 * question is about the copy, not about what we do after it. */
			if (writable && !by_block)
				der_apply(i, (unsigned char *)base, (size_t)size);
			/* Back to the protection the region had when it was saved, not the
			 * one it happened to have a moment ago.
			 *
			 * That is right for memory we own and dangerous for memory we do
			 * not, so both ways it can be wrong are now counted.
			 *
			 * A session ended with Steam's vstdlib_s.dll faulting on a REP
			 * STOSB into one of its own globals, zero frames after a restore,
			 * on a page reading PAGE_READONLY. The destination was a fixed
			 * address with a tidy length, which is a memset doing exactly what
			 * it was written to do - so the pointer was fine and the page was
			 * not. This line is the only thing in the engine that can make a
			 * page less writable than the process left it.
			 *
			 * Two questions, because they have different answers. Does a
			 * captured region overlap a module we said we were holding - which
			 * would mean the module roster and the region list disagree about
			 * what is ours. And did the protection change between the save and
			 * now - a copy-on-write page promoted to read-write after we wrote
			 * the value down is handed back read-only, and its next writer
			 * dies without either list being wrong. */
			if (writable)
				VirtualProtect(base, size, s->regs[i].prot, &old);
			/* Heal again after the protect returns. The audio thread can
			 * have taken the signal during it. */
			unix_frame_heal();
			g_copy_excl = 0;
			if (img_live)
				VirtualFree(img_live, 0, MEM_RELEASE);
			pos += size;
		}
		win_close(&wv);
#if defined(_M_IX86) || defined(__i386__)
		if (img_hi)
			ss_log("  globals: %d word(s) of the game's image data kept live - "
			       "the save named the old process's system objects, the live "
			       "process had made its own\n",
			       g_imgm_took);
		if (heap_mg)
			ss_log("  heap words: %d pointer(s) in restored memory named a block in "
			       "a heap left in the present and were given the live process's "
			       "value at the same place (D3D9SW_HEAPMERGE=1)\n",
			       g_heapm_took);
#endif
		if (excl_whole || excl_part)
			ss_log("  excluded now: %d region(s) left wholly in the present and %d "
			       "written around their live pages, %llu KB not written - live "
			       "stacks and thread blocks keep their own bytes\n",
			       excl_whole, excl_part, g_excl_skip_bytes >> 10);
		if (g_pres_skip_bytes)
			ss_log("  present section: %llu KB of this DLL's own pointers and handles "
			       "kept as this process has them\n",
			       g_pres_skip_bytes >> 10);
		g_pres_skip_bytes = 0;
		/* Live table is excluded, so it is still present-tense. The game
		 * now believes the save-time set is live. Rewind the journal to
		 * match; no-op when there was no snapshot. */
		gameheap_busy_rewind();
		/* The blocks themselves, now that every heap region has been left
		 * untouched and its offset recorded. One pass over the matched
		 * list; a block whose region was not captured finds no home and is
		 * skipped, which is how blocks in heaps we hold back filter
		 * themselves out without needing to be filtered. */
		if (g_blk_ready && g_reg_off) {
			Window wb;
			unsigned long long done = 0;
			unsigned bi;
			int wrote = 0, homeless = 0;

			/* The "before" half of the probe: every heap answered here, with
			 * the threads still frozen, so anything that stops answering after
			 * the writes below was broken by the writes below. */
			blk_probe_before();
			memset(&wb, 0, sizeof(wb));
			wb.sect = s->sect;
			wb.total = s->bytes;
			/* The main loop has already put each region's saved protection
			 * back, so a region is not necessarily writable any more.
			 * Blocks arrive sorted by address and regions are in address
			 * order too, so the region only ever changes forwards: open
			 * one, write every block that lands in it, close it again. */
			int open_r = -1, not_ours = 0, vetoed = 0;
			int filter = blk_owner_filter();
			DWORD open_old = 0;
			/* The verify pass in the main loop skips every by_block
			 * region, because a heap region is not copied wholesale and
			 * comparing it as one would flag the allocator's own lists as
			 * damage. The effect was that the path carrying the game's
			 * 62.5 MB heap - the largest thing we move and the thing we
			 * trust most - was the only path never checked. Blocks are
			 * checked here instead, one at a time, which is the only
			 * granularity at which the question is even well posed. */
			int vmode_b = verify_mode(), vbad = 0, vunck = 0;
			unsigned long long vwords = 0, vfirst_at = 0;
			uintptr_t vfirst_blk = 0;
			/* Blocks that passed the address-and-size identity test but
			 * were handed back to the allocator at some point since the
			 * save. Same address, same size, different owner. */
			int recycled = 0;
			unsigned long long recycled_bytes = 0;
			uintptr_t recycled_first = 0;
			int heldveto = 0, vtabveto = 0, vtabmod = -1;
			unsigned long long heldbytes = 0, vtabbytes = 0;
			uintptr_t heldfirst = 0, heldbigat = 0, vtabfirst = 0;

			memset(g_vtab_bymod, 0, sizeof(g_vtab_bymod));
			unsigned long heldbig = 0;

			held_build();
			for (bi = 0; bi < g_blk_match_n; bi++) {
				uintptr_t b = g_blk_save[bi].base;
				unsigned long n = g_blk_save[bi].size;
				int r = blk_region_of(s, b, n);
				/* The one allocation whose contents we can check
				 * against the game's own screen. Every verdict below
				 * is recorded for it. */
				int wit = g_ctl->wit_valid && g_ctl->wit_ent >= b &&
					  g_ctl->wit_ent < b + n;

				if (r < 0) {
					homeless++;
					if (wit)
						g_ctl->wit_why = WIT_HOMELESS;
					continue;
				}
				/* Ahead of the ownership tests, because this one is not
				 * about whose block it is. A block the game certainly
				 * owns is still fatal to write if a thread that keeps
				 * running is holding a pointer into it - that thread
				 * resumes and reads a value from a moment it never
				 * lived through. Leaving it in the present is the
				 * smaller error: the game loses one object's worth of
				 * rewind, and nobody is lied to. */
				if (held_hit(b, n)) {
					if (!heldveto)
						heldfirst = b;
					if (n > heldbig) {
						heldbig = n;
						heldbigat = b;
					}
					heldveto++;
					heldbytes += n;
					if (wit)
						g_ctl->wit_why = WIT_HELD;
					continue;
				}
				/* An object whose vtable belongs to somebody else is
				 * somebody else's object.
				 *
				 * DirectSound allocates its COM objects out of a heap
				 * we attribute to rabiribi.exe and rewind wholesale, and
				 * the game holding a pointer to one does not make its
				 * contents the game's to move. Two faults in one session
				 * said so directly: dsound.dll+282E2 wrote through a
				 * field of a block whose first word was 6A0412C8, and
				 * dsound.dll+5C6F8 called a function pointer out of one
				 * whose first word was 6A042AB4 - both inside the vtable
				 * range our own hook logged at startup, and one of them
				 * marked WRITTEN by the last restore.
				 *
				 * The held-pointer veto catches these only when a thread
				 * happens to be pointing at one at the instant we
				 * freeze, which is why it helps and does not cure. This
				 * test does not depend on timing.
				 *
				 * Deliberately narrow. Any first word that lands in a
				 * module would do as a heuristic, but a game object
				 * whose first field is a callback would be caught by it
				 * and silently stop rewinding, so it asks instead
				 * whether the module is one we are already holding in
				 * the present. Those are the only ones where leaving the
				 * object alone is consistent rather than arbitrary. */
				if (foreign_object(b, n, &vtabmod)) {
					if (!vtabveto)
						vtabfirst = b;
					if (vtabmod >= 0 && vtabmod < SS_MAX_MODS)
						g_vtab_bymod[vtabmod]++;
					vtabveto++;
					vtabbytes += n;
					if (wit)
						g_ctl->wit_why = WIT_VTAB;
					continue;
				}
				/* Only the shared heap needs vetting; the game's own
				 * heaps have no other tenant. */
				if (filter && heap_is_shared(b)) {
					if (filter >= 3) {
						/* Reachability decided this already, for
						 * every block at once, before a byte
						 * moved. */
						if (!g_blk_own || !g_blk_own[bi]) {
							not_ours++;
							if (wit)
								g_ctl->wit_why =
									WIT_NOTOURS;
							continue;
						}
						/* Deny wins. Reaching the game does not
						 * make it the game's if Windows can
						 * reach it too - a stale loader pointer
						 * in a dead stack frame reaches plenty. */
						if (g_blk_sys && g_blk_sys[bi]) {
							vetoed++;
							if (wit)
								g_ctl->wit_why =
									WIT_VETOED;
							continue;
						}
					} else {
						int own = blk_owner_of(b, n);

						if (own < 0 || (filter >= 2 && own == 0)) {
							not_ours++;
							if (wit)
								g_ctl->wit_why =
									WIT_NOTOURS;
							continue;
						}
					}
				}
				if (r != open_r) {
					if (open_r >= 0)
						VirtualProtect((void *)s->regs[open_r].base,
							       (SIZE_T)s->regs[open_r].size,
							       s->regs[open_r].prot,
							       &open_old);
					open_r = VirtualProtect((void *)s->regs[r].base,
								(SIZE_T)s->regs[r].size,
								PAGE_EXECUTE_READWRITE,
								&open_old)
							 ? r
							 : -1;
					if (open_r < 0)
						continue;
				}
				{
					unsigned long long off =
						g_reg_off[r] + (b - s->regs[r].base);

					if (freed_has(b)) {
						if (!recycled)
							recycled_first = b;
						recycled++;
						recycled_bytes += n;
						if (!recycled_write()) {
							if (wit)
								g_ctl->wit_why =
									WIT_RECYCLED;
							continue;
						}
					}
					if (!win_copy(&wb, off, (void *)b, n, 0)) {
						if (wit)
							g_ctl->wit_why = WIT_COPYFAIL;
						continue;
					}
					wrote++;
					done += n;
					/* Did the one allocation we can check by hand
					 * travel? The player's entity is a known-truth
					 * address, so noting when its block is written
					 * turns "she did not come back" from a symptom
					 * into a yes-or-no about the block map. */
					if (wit && g_ctl->wit_ent) {
						g_ctl->wit_blk = b;
						g_ctl->wit_why = WIT_WRITTEN;
					}
					if (g_blk_wrote)
						g_blk_wrote[bi] = 1;
					if (vmode_b) {
						unsigned long long fd = 0, wds = 0, was = 0,
								   now = 0;
						int vr = win_cmp(&wb, off, (const void *)b, n,
								 &fd, &wds, &was, &now);

						if (vr < 0) {
							vunck++;
						} else if (!vr) {
							if (!vbad) {
								vfirst_blk = b;
								vfirst_at = fd;
							}
							vbad++;
							vwords += wds;
						}
					}
				}
			}
			if (open_r >= 0)
				VirtualProtect((void *)s->regs[open_r].base,
					       (SIZE_T)s->regs[open_r].size,
					       s->regs[open_r].prot, &open_old);
			win_close(&wb);
			ss_log("  heap blocks: %u of %u saved block(s) still busy at the "
			       "same address and size, %d written (%.2f MB), %d in heaps "
			       "left in the present, %d in the shared heap with no sign "
			       "of being the game's, %d vetoed as Windows'. Allocator "
			       "metadata untouched\n",
			       g_blk_match_n, g_blk_save_n, wrote,
			       (double)done / (1024.0 * 1024.0), homeless, not_ours, vetoed);
			/* Spelled "pointed" rather than "held", which the exclusion
			 * list above already uses for every module it keeps in the
			 * present. Two unrelated things under one prefix made the log
			 * unreadable the first time this ran. */
			if (held_on())
				ss_log("  pointed: %d block(s) (%.2f MB) left in the present "
				       "because a surviving thread was pointed into them - "
				       "%d pointer(s) read from %d thread(s) that keep "
				       "running. First at %p, largest %p (%lu bytes). Those "
				       "are the blocks that would have been lies\n",
				       heldveto, (double)heldbytes / (1024.0 * 1024.0),
				       g_held_n, g_held_threads, (void *)heldfirst,
				       (void *)heldbigat, heldbig);
			if (vtab_on()) {
				int mi, shown;

				ss_log("  foreign: %d block(s) (%.2f MB) left in the present "
				       "because the vtable at their head belongs to a "
				       "module we do not rewind - somebody else's objects "
				       "living in a heap we call the game's. First at %p\n",
				       vtabveto, (double)vtabbytes / (1024.0 * 1024.0),
				       (void *)vtabfirst);
				/* Named, because the count is only reassuring if the
				 * names on it are the ones we meant to protect. */
				for (shown = 0; shown < 6; shown++) {
					int best = -1;

					for (mi = 0; mi < g_ctl->nmods; mi++)
						if (g_vtab_bymod[mi] &&
						    (best < 0 ||
						     g_vtab_bymod[mi] > g_vtab_bymod[best]))
							best = mi;
					if (best < 0)
						break;
					ss_log("    foreign: %d belong to %s\n",
					       g_vtab_bymod[best], g_ctl->mod_name[best]);
					g_vtab_bymod[best] = 0;
				}
			}
			if (vmode_b && vbad)
				ss_log("  verify by block: %d of %d written block(s) do NOT "
				       "match what we wrote, %llu word(s) differ, %d "
				       "unchecked. First at %p+%llX - something is writing "
				       "to the game's heap while we restore it\n",
				       vbad, wrote, vwords, vunck, (void *)vfirst_blk,
				       vfirst_at);
			else if (vmode_b)
				ss_log("  verify by block: all %d written block(s) match what "
				       "we wrote, %d unchecked\n",
				       wrote, vunck);
			if (g_freed && recycled)
				ss_log("  block identity: %d free(s) seen since the save; %d "
				       "matched block(s) had been freed and handed out again "
				       "at the same address and size (%.2f MB), first at %p - "
				       "%s%s\n",
				       (int)g_freed->used, recycled,
				       (double)recycled_bytes / (1024.0 * 1024.0),
				       (void *)recycled_first,
				       recycled_write()
					       ? "WRITTEN ANYWAY, because "
						 "D3D9SW_RECYCLED=1"
					       : "left in the present, because the "
						 "address holds a different object "
						 "now",
				       g_freed->overflow ? " - TABLE OVERFLOWED, a floor not a "
							   "total"
							 : "");
			else if (g_freed)
				ss_log("  block identity: %d free(s) seen since the save, none of "
				       "them an address we restored%s\n",
				       (int)g_freed->used,
				       g_freed->overflow ? " - TABLE OVERFLOWED, a floor not a "
							   "total"
							 : "");
			blk_probe_after();
		}
		QueryPerformanceCounter(&v1);
		keep_end();
		heap_cookie_put();
		if (pstraddle || pdelta)
			ss_log("  protection: %d region(s) inside a held module, %d "
			       "whose protection had changed since the save%s\n",
			       pstraddle, pdelta,
			       pstraddle ? " <<< the module roster and the region "
					   "list disagree about what is ours"
					 : "");
		if (vmode)
			ss_log("  verify at restore: %d region(s), %d WRONG (%llu word(s)), %d "
			       "unchecked, %.1f ms%s\n",
			       s->nregs, vdiffer, vwords, vunchecked,
			       (double)(v1.QuadPart - v0.QuadPart) * 1000.0 / (double)vf.QuadPart,
			       vdiffer ? " <<< THE RESTORE DID NOT TAKE" : "");
	}
	win_close(&w);
	/* The only reading that separates "we failed to write it" from "we wrote it
	 * and the game moved it".
	 *
	 * The sample taken with the clobber check is 100 ms late by design, which is
	 * right for asking whether something got overwritten and useless for asking
	 * whether it arrived: Ribbon read 16018.03 at a save and 15860.34 a hundred
	 * milliseconds after the restore, which is either a restore that missed by
	 * 158 units or a restore that landed and was followed by a hundred
	 * milliseconds of her flying back to the player. Those want opposite fixes.
	 *
	 * Here the bytes are written and no game thread has run yet, so the value is
	 * exactly what the restore put there. Deliberately live=0: the address list
	 * was resolved at the save, and this point is inside the suspended window
	 * where reading the config file is not allowed. */
	watch_at_report("as the restore finishes, before any thread resumes", 0);
	/* Armed here rather than with the clobber sample, which runs 100 ms later.
	 * The first run traced a restore from frame six of the sweep onward and
	 * showed a clean exponential converging on the saved value - but the six
	 * frames it could not see are the ones that say how far she was displaced
	 * to begin with, and that displacement is the whole effect. */
	watch_trace_arm("load");
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
		int seeked = 0, stale = 0, back = 0;
		for (i = 0; i < s->nfiles; i++) {
			LARGE_INTEGER p;
			if (GetFileType(s->files[i].h) != FILE_TYPE_DISK ||
			    path_hash(s->files[i].h) != s->files[i].hash) {
				const char *why = "";

				if (!xs_foreign_load() || !file_reinstate(&s->files[i], &why)) {
					if (xs_foreign_load())
						ss_log("  file %p %s: not put back - %s\n",
						       s->files[i].h, s->files[i].name, why);
					stale++;
					continue;
				}
				back++;
				ss_log("  file %p %s: opened again under its saved handle "
				       "value\n",
				       s->files[i].h, s->files[i].name);
			}
			p.QuadPart = s->files[i].pos;
			if (SetFilePointerEx(s->files[i].h, p, NULL, FILE_BEGIN))
				seeked++;
		}
		if (stale)
			ss_log("  %d of %d saved file handles no longer match and were left "
			       "alone\n",
			       stale, s->nfiles);
		if (back)
			ss_log("  files: %d handle(s) from the other process opened again "
			       "under the same value\n",
			       back);
		ss_log("  files: %d handle(s) seeked back\n", seeked);
	}

	/* Before touching contexts, measure whether the saved stack addresses are
	 * free here - the one fact that decides if cross-session thread adoption is
	 * possible at all. Reads only; it changes nothing. */
	stackprobe_report(s);

	if (xs_foreign_load() && transplant_mode()) {
		tls_done = xs_transplant(s);
#if defined(_M_IX86) || defined(__i386__)
		xs_tid_remap();
#endif
	} else if (dataonly_mode()) {
		/* No context is wound back. Every thread - the requesting one included -
		 * keeps its current execution and reads the rewound world, the way a
		 * cross-session restore already must and the way thread_harness showed
		 * works. The requesting thread therefore returns from savestate_load
		 * normally rather than resuming inside savestate_save. TLS is still put
		 * back, since it is data the running thread reads, not execution state. */
		int tls_only = 0;

		for (i = 0; i < s->nthreads; i++)
			for (j = 0; j < g_ctl->nids; j++)
				if (g_ctl->ids[j] == s->threads[i].tid && g_ctl->handles[j]) {
					if (s->threads[i].have_tls) {
						unsigned char *teb = (unsigned char *)teb_of(
							g_ctl->handles[j]);
						if (teb) {
							memcpy(teb + TEB_TLS_SLOTS,
							       s->threads[i].tls,
							       sizeof(s->threads[i].tls));
							tls_only++;
						}
					}
					break;
				}
		tls_done = tls_only;
		ss_log("  DATAONLY: thread contexts NOT applied - the runners keep their "
		       "current execution and read the rewound world (%d TLS block(s) put "
		       "back)\n",
		       tls_only);
	} else {
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
	cs_own_fix(s);

	/* Reported before it is acted on. If this line always says none differ,
	 * the events never mattered and the whole question is closed by
	 * observation rather than argument; putting them back stays opt-in until
	 * it says otherwise. */
	if (xs_foreign_load() && g_events)
		xs_handles_pin(s);
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
			HANDLE h = savestate_hx(s->events[i].h);
			if (!s->events[i].h || s->events[i].kind == HK_THREAD ||
			    s->events[i].kind == HK_MUTEX || !GetHandleInformation(h, &flags))
				continue;
			now = event_probe(h);
			if (now == s->events[i].signalled)
				continue;
			differ++;
			if (!enabled)
				continue;
			if (s->events[i].signalled)
				SetEvent(h);
			else
				ResetEvent(h);
			put++;
		}
		ss_log("  events: %d tracked, %d differ from the save, %d put back\n", s->nevents,
		       differ, put);
	}

	time_rewind(&s->clock);
	g_frames_since_load = 0;
	g_xlog_left = 16;
	gameheap_va_mark_restore();
	gameheap_res_restored(slotno, s->gh_seq);
	gameheap_al_restored();
	gameheap_physx_restored();
	/* Anchored at the first restore of the session and never moved again, so
	 * it measures how long the process has lasted since the rewind first
	 * touched it rather than how long since the most recent one. It lives in
	 * the control block because a static would be wound back to its
	 * pre-restore value by the second restore and re-anchor itself. */
	if (!g_ctl->anchor_tick)
		g_ctl->anchor_tick = GetTickCount();
	g_ctl->load_tick = GetTickCount();
	ss_log("  threads: %d context(s) restored, %d with TLS\n", s->nthreads, tls_done);
	/* Reported from the helper, not from savestate_load.
	 *
	 * The thread that asked for the restore has had its stack and context
	 * wound back to the save, so it does not return from savestate_load at
	 * all - it carries on from inside savestate_save. Anything written after
	 * the request is unreachable by construction, which is why the first
	 * version of this line never appeared and why the census that follows a
	 * restore prints "save". The helper is excluded from the snapshot and is
	 * the only thread that survives the restore in its own present. */
	roster_load();
	arena_put_back(arena_keep);
	{
		DWORD t0 = GetTickCount();

		ss_log("  step: carry_load (%lu ms into the tail)\n", 0ul);
		carry_load();
		DWORD t1;

		t1 = GetTickCount();
		ss_log("  step: carry_load took %lu ms; reloc_apply next\n",
		       (unsigned long)(t1 - t0));
		/* D3D9SW_RELOC=1/2: shift (or report) the restored snapshot's pointers
		 * from their save-time module bases to this run's. Deltas come from
		 * identity, so in-session there are none. Before witness_load so the
		 * relocated world is what it reads. */
		reloc_apply(s);
		t0 = GetTickCount();
		ss_log("  step: reloc_apply took %lu ms (%d range(s) indexed as %d segment(s)%s)\n",
		       (unsigned long)(t0 - t1), g_rx_mapn, g_rx_n,
		       g_rx_ok ? "" : ", NOT indexed - linear walk");
#if defined(_M_IX86) || defined(__i386__)
		if (g_ctl->nxs)
			xs_cookies_write();
		ss_log("  step: xs_cookies_write took %lu ms\n",
		       (unsigned long)(GetTickCount() - t0));
		if (xs_foreign_load())
			encoded_ptrs_reencode(s);
#endif
		xrefs_census(s);
	}
	/* After the shift, because the shift is what last moved them. */
	if (g_ctl->nxs && xs_verify()) {
		if (g_ctl->rollback_ready) {
			hold_threads();
			ss_op_set("load-rollback");
			return -1;
		}
		ss_log("  verify: no rollback point for this load - releasing anyway\n");
	}
	ss_log("  step: witness_load\n");
	witness_load();
	if (g_ctl->diff_same || g_ctl->diff_wrote) {
		unsigned long long tot = g_ctl->diff_same + g_ctl->diff_wrote;

		ss_log("  diff restore: wrote %llu MB, left %llu MB alone because it "
		       "already matched - %llu%% of %llu MB needed no write\n",
		       g_ctl->diff_wrote >> 20, g_ctl->diff_same >> 20,
		       tot ? (g_ctl->diff_same * 100) / tot : 0, tot >> 20);
	}
	ss_op_set("load-resume");
	ss_log("load: slot %d, %d restored, %d skipped, %d by block, %d threads, %d newer "
	       "than save%s, %lu ms (%s)\n",
	       slotno, restored, skipped, blocked, g_ctl->nids, g_ctl->last_fresh,
	       (g_ctl->last_fresh && policy() == POLICY_HOLD) ? " (held suspended)" : "",
	       (unsigned long)(GetTickCount() - load_t0), prov_name(g_ctl->load_prov));
	/* The SKIPREG/SCRIBBLE knobs log each region as it is handled; this is the
	 * roll-up those two counts were always meant to feed. Printed only when a knob
	 * actually acted, so a normal restore stays quiet. */
	if (handskip || handscrib)
		ss_log("load: %d region(s) left in the present by hand (SKIPREG), %d "
		       "scribbled with 0xCD (SCRIBBLE)\n", handskip, handscrib);
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
	/* The write is complete and nothing is running yet, so this is the only
	 * place that can say what the restore actually put back, as opposed to
	 * what the game had already done with it by the time anyone looked. */
	ents_report("right after the write, threads still suspended");
	ents_diff("right after the write");
	catch_arm(s);
	seh_final_fix();
	ss_log("  resume: releasing %d thread(s)\n", g_ctl->nids);
	/* Cursors before the threads, playback after them. See dsh_seek. */
	dsh_seek();
	InterlockedExchange(&g_merged_clear, 1);
	resume_all(policy() == POLICY_HOLD);
	/* Immediately, and from here rather than from the clobber watch.
	 *
	 * This lived inside clobber_check, which runs on a timer some hundreds of
	 * milliseconds after the restore and only when D3D9SW_CLOBBER is set. Both
	 * halves of that were wrong. The timer left the game reading a play cursor
	 * that was still in the present against a write position that had just been
	 * wound into the past - the negative difference this whole file exists to
	 * prevent, handed to it for the length of the delay. And hanging it off a
	 * diagnostic meant turning that diagnostic off silently turned off the
	 * audio restore with it, so runs made to study one thing quietly stopped
	 * doing another.
	 *
	 * After resume_all rather than before: putting the cursors back means
	 * calling into dsound, which takes locks of its own, and doing that while
	 * every game thread is suspended risks waiting on one a suspended thread is
	 * holding. The window is now the few microseconds between the two calls
	 * instead of the clobber delay.
	 *
	 * Only the starting half runs here. When D3D9SW_DSSEEK is on, dsh_seek
	 * above has already put every cursor where the game expects it, so a thread
	 * that reads one before this line gets the right answer from a stopped
	 * buffer. When it is off, which is the default, no cursor moves in either
	 * place and the buffers resume from where they actually are. */
	dsh_play();
	/* Before the mixer runs again, so the first post-restore notification it
	 * raises is already checked against the new generation. */
	xa2_sw_restored();
	xa2_sw_resume();
	ss_log("  resume: done, all threads runnable\n");
	if (xs_foreign_load()) {
		HANDLE wd = g_ctl->wd_tid ? OpenThread(SYNCHRONIZE, FALSE, g_ctl->wd_tid) : NULL;

		/* One at a time: it reads the frame count, which every load resets. */
		if (wd && WaitForSingleObject(wd, 0) == WAIT_TIMEOUT) {
			CloseHandle(wd);
		} else {
			DWORD tid = 0;

			if (wd)
				CloseHandle(wd);
			wd = CreateThread(NULL, 0, xs_watchdog, NULL, 0, &tid);
			g_ctl->wd_tid = wd ? tid : 0;
			if (wd)
				CloseHandle(wd);
		}
	}
	/* After the threads are running again, because the comparison is a read of
	 * a few hundred megabytes and holding every thread suspended through it
	 * would charge the restore for a diagnostic. A region the game rewrites in
	 * the first frame after resuming would be counted as uncaptured when it is
	 * merely fast, so treat single-region surprises with suspicion; the ones
	 * worth chasing are large and repeat across runs. */
	coverage_check(s);
	/* A heap written back from another process carries that process's header
	 * encoding and links, and HeapValidate walking it faulted on the helper
	 * thread in the first load that tried. Before the restore it is still this
	 * process's heap, so that check stands. */
	if (g_ctl->load_prov == PROV_SAME_PROCESS)
		heap_check("after the restore");
	else
		ss_log("  heap check: skipped after the restore - the slot came from %s, "
		       "and validating heaps written back from another process has "
		       "faulted\n",
		       prov_name(g_ctl->load_prov));
	/* The count answers a question the format strings cannot: whether Mono
	 * formats strings constantly during normal work, or only when something has
	 * gone wrong. If it is normally zero, the crash is on an error path and the
	 * printf fault is a symptom rather than the disease. */
	ss_log("  formatting: %ld call(s) through mono so far this session\n",
	       (long)(g_ctl ? g_ctl->fmt_calls : 0));
	clobber_arm(slotno);
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
	g_dev_regions = 0;
	g_dev_bytes = 0;
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

		/* linux 39948: after a living first restore, helper Sleep(1)
		 * hit C000001D in kernelbase (wineserver). VEH then parked tid
		 * 500 - this thread, not xa2 - as mixer, so KEY_2 could not
		 * start load 2. YieldProcessor is local, like request(). */
		while ((req = InterlockedExchange(&g_ctl->request, REQ_NONE)) == REQ_NONE)
			YieldProcessor();
		QueryPerformanceCounter(&h0);
		/* Held from the first checkpoint (early, while the address space still
		 * has a 64 MB hole) and released just for this operation so the views
		 * land in a guaranteed contiguous span, then re-held. This is Phase 1:
		 * save slots bounded by disk, not by finding a hole at restore time. */
		bank_reserve_init();
		bank_release();
		g_ctl->result = (req == REQ_SAVE) ? do_save((int)g_ctl->slot)
						  : load_with_rollback((int)g_ctl->slot);
		bank_reacquire();
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

/* Above this DLL's image, below the system and Steam DLLs. Reserved at DLL load,
 * because the block itself is made lazily and by then the system may have put
 * something there. */
#define SS_CTL_HOME 0x60800000u
_Static_assert(sizeof(Control) <= 0x2000000u, "Control outgrew its home");
static void *g_ctl_home SS_PRESENT;

void savestate_reserve_home(void)
{
	g_ctl_home = VirtualAlloc((void *)SS_CTL_HOME, sizeof(Control), MEM_RESERVE,
				  PAGE_READWRITE);
}

static int ensure_helper(void)
{
	if (g_helper)
		return 1;
	/* A fixed home, so no launch puts it at an address a rewound heap from
	 * another launch still claims as its own. */
	g_ctl = g_ctl_home ? (Control *)VirtualAlloc(g_ctl_home, sizeof(Control), MEM_COMMIT,
						     PAGE_READWRITE)
			   : NULL;
	if (!g_ctl)
		g_ctl = (Control *)VirtualAlloc(NULL, sizeof(Control),
						MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!g_ctl)
		return 0;
	memset(g_ctl, 0, sizeof(*g_ctl));
	g_ctl->busy = 1;
	g_ctl->policy = -1;
	g_ctl->tmode = -1;
	g_ctl->rmode = -1;
	g_ctl->guard = -1;
	savestate_hooks_install();
	ss_exclude_as("our own control block", g_ctl, sizeof(Control));
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
	(void)create_file();
	(void)query_thread();
	g_ctl->nex_fixed = g_ctl->nex;
	/* One file per launch in the launch's log folder, so the session that just
	 * died is never erased by the next one. Restores do not disturb the
	 * position: this handle is the one file for_each_file refuses to seek. */
	g_ctl->log = swlog_create("savestate.txt", OPEN_ALWAYS);
	g_logh = g_ctl->log;
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
	ss_log("savestate ready, %s build %08lX, control %u KB at %p\n",
	       SW_VARIANT_STR(D3D9SW_VARIANT), (unsigned long)ss_build_id(),
	       (unsigned)(sizeof(Control) / 1024), (void *)g_ctl);
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
		const char *const *knobs = g_knobs;
		int i, shown = 0;
		char v[64];

		/* The game's own switches matter as much as ours and were nowhere in
		 * this log. Rabi-Ribi takes -softsound, which changes how DxLib
		 * mixes, and a session was read as evidence about it while the only
		 * honest answer was that nothing here records how the game was
		 * started. Same reason the knobs are printed even when unset. */
		/* Converted from the Unicode command line rather than taken from
		 * GetCommandLineA, which in this process returns eleven characters -
		 * the line came out as "C:\Program and stopped, which reads as a
		 * formatter bug and is not one. */
		{
			const WCHAR *wc = GetCommandLineW();
			char cl[512];

			if (!wc || !WideCharToMultiByte(CP_ACP, 0, wc, -1, cl, sizeof(cl),
							NULL, NULL))
				lstrcpynA(cl, "(the command line could not be read)",
					  sizeof(cl));
			ss_log("settings: the game was started as: %s\n", cl);
		}
		ss_log("settings: d3d9_sw.cfg %s\n",
		       g_cfg_len > 0 ? "found" : "not present (environment only)");
		/* Loud, and above the knob list, because the failure it describes
		 * looks exactly like a knob nobody set. */
		if (g_cfg_over)
			ss_log("  WARNING: d3d9_sw.cfg is %u byte(s) longer than this "
			       "build reads. Everything past the first %u bytes was "
			       "never parsed, so any setting near the end of the file "
			       "is in force at its default no matter what the line "
			       "says. Move the settings you care about to the top, or "
			       "shorten the comments\n",
			       g_cfg_over, (unsigned)SS_CFG_MAX);
		for (i = 0; i < (int)(sizeof(g_knobs) / sizeof(g_knobs[0])); i++) {
			char envv[64];
			int from_env = GetEnvironmentVariableA(knobs[i], envv, sizeof(envv)) > 0;

			/* Unset is printed too. A knob that reads as absent while the
			 * config plainly sets it is the single most expensive failure
			 * this file has: it does not look like a bug, it looks like a
			 * result, and three runs were compared this way before anyone
			 * noticed the feature under test had never been switched on.
			 * Naming the source matters for the same reason - the D3D11
			 * wrapper seeds these from its own cfg into the environment,
			 * so "from the cfg file" is the only proof this file parses. */
			if (!ss_getenv(knobs[i], v, sizeof(v))) {
				ss_log("  %s = (unset, using the built-in default)\n",
				       knobs[i]);
				continue;
			}
			ss_log("  %s = %s (from the %s)\n", knobs[i], v,
			       from_env ? "environment" : "cfg file");
			shown++;
			/* Two files set this, and the quieter one wins.
			 *
			 * d3d11_sw.cfg is read by the D3D11 wrapper at load and pushed
			 * into the process environment key by key, and the environment
			 * outranks d3d9_sw.cfg here. So a savestate knob left behind in
			 * d3d11_sw.cfg silently overrides the file that is supposed to
			 * own savestate knobs, and editing the right file does nothing.
			 *
			 * D3D9SW_REWIND_SWHEAP sat like that for a whole session. The
			 * cfg said 1, the wrapper's own cfg still said 0, every run was
			 * a 0 run, and a fix written against the 1 case was deployed,
			 * tested and reasoned about without ever executing. The tag
			 * above did say "from the environment", which was true and far
			 * too quiet to notice. This says the part that matters: the
			 * value you edited is not the value in force. */
			if (from_env) {
				char fv[64];

				if (cfg_lookup(knobs[i], fv, sizeof(fv)) &&
				    lstrcmpiA(fv, v) != 0)
					ss_log("  ^^ CONFLICT: d3d9_sw.cfg asks for %s and is "
					       "being ignored. The environment wins, and the "
					       "usual source of that is a leftover %s line in "
					       "d3d11_sw.cfg, which the D3D11 wrapper pushes "
					       "into the environment at load. Effective value "
					       "is %s\n",
					       fv, knobs[i], v);
			}
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
	/* Tried here as well as per frame, and here first.
	 *
	 * By this point the log is open and this function's output is known to
	 * arrive, which is the property the per-frame attempt turned out to lack.
	 * The image is decrypted too: this runs at device creation, long after
	 * Steam's stub has handed control to the real entry point, so the bytes at
	 * +6F4C6 are the ones the disassembler saw rather than ciphertext. If it
	 * succeeds here the per-frame attempt never runs, and if it fails it says
	 * why. */
	decoder_patch_once();
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

/* Defined further down with the rest of the boundary watcher. Censused here too
 * so that all three events - the game's own load, our save, our restore - are
 * measured by the same function and can be read against each other line for
 * line. The comparison is the point: a heap the game rebuilds for itself at a
 * transition is a heap we should be declining to restore, and we cannot tell
 * those apart from two different measurements. */
static void boundary_census(const char *when);
int gameheap_va_origin(uintptr_t base, char *out, int cap);

/* Big private allocations and what in the exe points at them.
 *
 * A cross-session restore died on a DxLib handle table whose pointer lives in
 * exe data and whose 32 MB block sat at 071E0000 in the saving session and at
 * 07B50000 in the restoring one. Nothing else pins such a block, and nothing
 * logged who made it. This lists every private reservation of 1 MB and up, by
 * allocation base: its size, the heap or pin that claims it, how many words in
 * the exe's writable sections point into it (with where, and how far in), and
 * the originating call site if NTLOG saw it reserved. Run at a save and again
 * at a load, the two lists pair a block with its moved self by size and by the
 * exe slot that points at it. Log-only; D3D9SW_BIGMAP=0 turns it off. */
#define BM_CAP 96
#define BM_REFS 4
static void bigmap_census(const char *when)
{
	static struct {
		uintptr_t base, reserve, commit;
		DWORD prot;
		unsigned refs;
		uintptr_t ref_rva[BM_REFS], ref_val[BM_REFS];
	} bm[BM_CAP];
	uintptr_t ab[8], as[8], ob[256], os[256], pb[4], ps[4];
	const char *pw[4];
	int nb = 0, na, no, np, i, j;
	unsigned char *img = (unsigned char *)GetModuleHandleA(NULL);
	IMAGE_NT_HEADERS *nt;
	IMAGE_SECTION_HEADER *sec;
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t p = 0x10000, cur = 0;
	char v[8];

	if (ss_getenv("D3D9SW_BIGMAP", v, sizeof(v)) > 0 && v[0] == '0')
		return;
	/* The log opens with the helper, which the first request would only start
	 * after this - and a fresh launch's cross load is always the first. */
	if (!ensure_helper())
		return;
	while (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
		uintptr_t base = (uintptr_t)mbi.BaseAddress, size = mbi.RegionSize;
		uintptr_t alloc = (uintptr_t)mbi.AllocationBase;

		if (!size || base + size <= base)
			break;
		p = base + size;
		if (mbi.State == MEM_FREE || mbi.Type != MEM_PRIVATE)
			continue;
		if (!nb || alloc != cur) {
			if (nb && bm[nb - 1].reserve < 1024u * 1024u)
				nb--;
			if (nb == BM_CAP)
				break;
			cur = alloc;
			memset(&bm[nb], 0, sizeof(bm[nb]));
			bm[nb].base = alloc;
			nb++;
		}
		bm[nb - 1].reserve = base + size - alloc;
		if (mbi.State == MEM_COMMIT) {
			if (!bm[nb - 1].commit)
				bm[nb - 1].prot = mbi.Protect;
			bm[nb - 1].commit += size;
		}
	}
	if (nb && bm[nb - 1].reserve < 1024u * 1024u)
		nb--;

	nt = (IMAGE_NT_HEADERS *)(img + ((IMAGE_DOS_HEADER *)img)->e_lfanew);
	sec = IMAGE_FIRST_SECTION(nt);
	for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
		uintptr_t s = (uintptr_t)img + sec[i].VirtualAddress;
		uintptr_t se = s + (sec[i].Misc.VirtualSize & ~(uintptr_t)3);

		if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE))
			continue;
		while (s < se && VirtualQuery((LPCVOID)s, &mbi, sizeof(mbi)) == sizeof(mbi)) {
			uintptr_t re = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
			uintptr_t *w = (uintptr_t *)s, *e = (uintptr_t *)(re < se ? re : se);

			s = (uintptr_t)e;
			if (mbi.State != MEM_COMMIT ||
			    (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
				continue;
			for (; w < e; w++) {
				uintptr_t x = *w;
				int lo = 0, hi = nb - 1;

				while (lo <= hi) {
					j = (lo + hi) / 2;
					if (x < bm[j].base)
						hi = j - 1;
					else if (x - bm[j].base >= bm[j].reserve)
						lo = j + 1;
					else
						break;
				}
				if (lo > hi)
					continue;
				if (bm[j].refs < BM_REFS) {
					bm[j].ref_rva[bm[j].refs] =
						(uintptr_t)((unsigned char *)w - img);
					bm[j].ref_val[bm[j].refs] = x - bm[j].base;
				}
				bm[j].refs++;
			}
		}
	}

	na = gameheap_anchor_ranges(ab, as, 8);
	no = gameheap_own_va_ranges(ob, os, 256);
	np = gameheap_pin_ranges(pb, ps, pw, 4);
	ss_log("bigmap[%s]: %d private allocation(s) of 1 MB and up; exe refs are "
	       "words in its writable sections, shown as exe+slot->+offset in\n",
	       when, nb);
	for (i = 0; i < nb; i++) {
		char org[48], refs[160] = "", claim[64] = "none";
		int hi = g_ctl ? heap_index_of(bm[i].base) : -1, k, len = 0;

		if (!gameheap_va_origin(bm[i].base, org, sizeof(org)))
			lstrcpynA(org, "not seen - reserved before the hooks armed",
				  sizeof(org));
		if (hi >= 0)
			ss_fmt(claim, (int)sizeof(claim), "heap %s", g_ctl->heap_name[hi]);
		for (k = 0; k < na; k++)
			if (bm[i].base >= ab[k] && bm[i].base - ab[k] < as[k])
				lstrcpynA(claim, "gameheap pin", sizeof(claim));
		for (k = 0; k < np; k++)
			if (bm[i].base >= pb[k] && bm[i].base - pb[k] < ps[k])
				lstrcpynA(claim, pw[k], sizeof(claim));
		for (k = 0; k < no; k++)
			if (bm[i].base >= ob[k] && bm[i].base - ob[k] < os[k])
				lstrcpynA(claim, "renderer (ours)", sizeof(claim));
		for (k = 0; k < (int)bm[i].refs && k < BM_REFS; k++) {
			ss_fmt(refs + len, (int)sizeof(refs) - len, " exe+%lX->+%lX",
			       (unsigned long)bm[i].ref_rva[k],
			       (unsigned long)bm[i].ref_val[k]);
			refs[sizeof(refs) - 1] = 0;
			len = lstrlenA(refs);
		}
		ss_log("  bigmap %08lX reserve %lu KB commit %lu KB prot %lx | %s | "
		       "exe refs %u%s%s | origin %s\n",
		       (unsigned long)bm[i].base, (unsigned long)(bm[i].reserve >> 10),
		       (unsigned long)(bm[i].commit >> 10), (unsigned long)bm[i].prot,
		       claim, bm[i].refs, refs, bm[i].refs > BM_REFS ? " ..." : "", org);
	}
}

int savestate_save(int slot)
{
	static LONG first;
	int r;

	if (InterlockedCompareExchange(&first, 1, 0) == 0)
		phase_snapshot("game");
	gameheap_physx_quiet("save", slot);
	bigmap_census("save");
	r = request(REQ_SAVE, slot);

	boundary_census("save");
	if (r)
		InterlockedExchange(&g_saved_since, 1);
	return r;
}

static int soak_knob(const char *name, int def);

int savestate_knob(const char *name, int def)
{
	return soak_knob(name, def);
}

int savestate_saved_since(void)
{
	return InterlockedExchange(&g_saved_since, 0) != 0;
}

int savestate_load(int slot)
{
	/* Nothing after the request. The calling thread is rewound into the save
	 * and never arrives back here - see the note beside the diff report in
	 * do_load, which is where post-restore reporting has to live.
	 *
	 * Do not Flush here. linux 35034: presenter Flush then request() hung
	 * SetThreadContext C000001D, same as helper Flush (24023). The Present
	 * thread is about to spin in request(); it will not Present. Snapshot
	 * copy is taken from Present after swrast_flush, so software CBs are
	 * idle. Do not vkEndCommandBuffer unix-wait on the rewind path. */
	gameheap_physx_quiet("load", slot);
	xrefs_on();
	if (g_ctl)
		g_ctl->diff_same = g_ctl->diff_wrote = 0;
	bigmap_census("load");
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
/* What the hooks this file runs from inside Present actually cost.
 *
 * They are called before the wrapper opens any of its timing zones, so every
 * one of them has been landing in the residual the split line labels as the
 * game's own code - the same place the frame pacer hid a flat 16 ms. An
 * address-space walk and an audio drain are not the game, and a dip that is
 * ours should not be attributed to it. */
static LONGLONG g_hook_guard, g_hook_audio;

void savestate_perf_take(double *guard_ms, double *audio_ms)
{
	LARGE_INTEGER f;

	QueryPerformanceFrequency(&f);
	if (guard_ms)
		*guard_ms = f.QuadPart ? 1000.0 * (double)g_hook_guard / (double)f.QuadPart : 0.0;
	if (audio_ms)
		*audio_ms = f.QuadPart ? 1000.0 * (double)g_hook_audio / (double)f.QuadPart : 0.0;
	g_hook_guard = 0;
	g_hook_audio = 0;
}

static void guard_body(void);

void savestate_guard(void)
{
	LARGE_INTEGER a, b;

	QueryPerformanceCounter(&a);
	guard_body();
	QueryPerformanceCounter(&b);
	g_hook_guard += b.QuadPart - a.QuadPart;
}

static void guard_body(void)
{
	static unsigned tick;
	MEMORY_BASIC_INFORMATION mbi;

	/* Ahead of everything else here, and outside the guard's own opt-in: this
	 * has to run on the frames just after a restore, which is exactly when the
	 * reclaim work below may have nothing to do and return early. */
	clobber_tick();
	/* Late enough that the D3D11 wrapper has pushed its config into the
	 * environment, early enough that few per-frame knob reads have had to go to
	 * the heap yet.
	 *
	 * Counted here rather than off the tick below, which only advances when the
	 * guard's own opt-in is enabled and therefore sat at zero all session. */
	if (++g_seal_tick == 120)
		env_seal();
	dsh_install();
	/* A guaranteed drain on the game's own thread. The software XAudio2 queues
	 * its callbacks rather than firing them from the mixer, and the methods it
	 * drains on are all ones this game only calls while a sound is already
	 * going - so a voice started into silence could otherwise sit waiting to be
	 * asked for audio by a queue waiting to be drained. Once a frame breaks
	 * that, and costs nothing when the queue is empty. */
	{
		LARGE_INTEGER a, b;

		QueryPerformanceCounter(&a);
		xa2_sw_pump();
		QueryPerformanceCounter(&b);
		g_hook_audio += b.QuadPart - a.QuadPart;
	}
	decoder_patch_once();
	heap_watch_tick();

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
	{
		/* The slot in play, not always slot 0, now that more than one can be
		 * held - the old hardcode would guard slot 0 while another was active. */
		int gs = ((int)g_ctl->slot >= 0 && (int)g_ctl->slot < SAVESTATE_SLOTS)
				 ? (int)g_ctl->slot
				 : 0;
		s = &g_ctl->slots[gs];
	}
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

int savestate_last_load_foreign(void)
{
	return g_ctl ? (g_ctl->load_prov != PROV_SAME_PROCESS) : 0;
}

static int foreign_seen(void)
{
	return g_ctl && g_ctl->foreign_seen;
}

int savestate_addr_restored(const void *p)
{
	const Slot *s = g_last_loaded;
	uintptr_t a = (uintptr_t)p;
	int i;

	if (!s)
		return -1;
	for (i = 0; i < s->nregs && i < (int)sizeof(g_reg_wr); i++)
		if (a >= s->regs[i].base && a - s->regs[i].base < s->regs[i].size)
			return g_reg_wr[i] ? 1 : 0;
	return -1;
}

/* For the fault report: whether the address the game reached for was memory
 * the last restore put back, and from which allocation. A MEM_FREE target that
 * the save held means the restore lost it or the game freed it since; one the
 * save never held means the pointer came from the abandoned timeline. */
static void fault_saved_note(uintptr_t at)
{
	const Slot *s = g_last_loaded;
	int i;

	if (!s)
		return;
	for (i = 0; i < s->nregs; i++) {
		if (at < s->regs[i].base || at - s->regs[i].base >= s->regs[i].size)
			continue;
		ss_raw("       in the save: region %d at %08lX+%lx, allocation %08lX - %s\n", i,
		       (unsigned long)s->regs[i].base, (unsigned long)s->regs[i].size,
		       (unsigned long)s->regs[i].alloc_base,
		       dead_reg_has(i)				 ? "left out as a dead stack"
		       : i < (int)sizeof(g_reg_wr) && g_reg_wr[i] ? "written by the restore"
								 : "already matched, not written");
		return;
	}
	for (i = 0; i < s->nregs; i++)
		if (s->regs[i].alloc_base == (at & ~(uintptr_t)0xFFFF)) {
			ss_raw("       in the save: not this address, but allocation %08lX "
			       "was there (region %d at %08lX+%lx)\n",
			       (unsigned long)s->regs[i].alloc_base, i,
			       (unsigned long)s->regs[i].base, (unsigned long)s->regs[i].size);
			return;
		}
	ss_raw("       in the save: nothing at or near it - the save never held this "
	       "memory, so the pointer to it came from after the save\n");
}

void savestate_follow_save_range(const void *base, size_t bytes)
{
	uintptr_t lo = (uintptr_t)base;
	int i;

	for (i = 0; i < g_nfollow; i++)
		if (g_follow_lo[i] == lo)
			return;
	if (g_nfollow < SS_MAX_FOLLOW) {
		g_follow_lo[g_nfollow] = lo;
		g_follow_hi[g_nfollow] = lo + bytes;
		g_nfollow++;
	}
}

/* Wall time since the session's first restore, or negative if there has not
 * been one. Real time rather than the game's, which the rewind may have moved. */
double savestate_live_ms(void)
{
	if (!g_ctl || !g_ctl->anchor_tick)
		return -1.0;
	return (double)(GetTickCount() - g_ctl->anchor_tick);
}

int savestate_rewinds(const void *p, char *name, unsigned cap)
{
	int h;

	if (name && cap)
		name[0] = 0;
	if (!g_ctl || !p)
		return -1;
	h = heap_index_of((uintptr_t)p);
	if (h < 0)
		return -1;
	if (name && cap)
		lstrcpynA(name, g_ctl->heap_name[h], (int)cap);
	return g_ctl->heap_ours[h] ? 1 : 0;
}

void savestate_exclude(void *p, size_t bytes)
{
	if (!p || !bytes)
		return;
	/* The control block carries the exclusion list, so it has to exist before
	 * anything can be added to it. */
	if (!ensure_helper())
		return;
	{
		LONG i, f = g_ctl->nex_fixed, n0 = g_ctl->nex;
		uintptr_t lo = (uintptr_t)p, hi = lo + bytes;
		const char *why;

		for (i = 0; i < f; i++)
			if (g_ctl->ex_lo[i] == lo && g_ctl->ex_hi[i] == hi)
				return;
		ss_log("  savestate_exclude: %p +%lu KB from %p, %ld fixed of %ld\n", p,
		       (unsigned long)(bytes >> 10), __builtin_return_address(0), (long)f,
		       (long)n0);
		ss_exclude_as("savestate_exclude, called by the wrapper", p, bytes);
		if (g_ctl->nex == n0)
			return;
		if (g_ctl->nex - 1 == f) {
			g_ctl->nex_fixed = g_ctl->nex;
			return;
		}
		/* Fixed, because build_exclusions resets the list to nex_fixed on every
		 * save - added without it, a range is held for one snapshot and quietly
		 * rewound by the next. But only this range: latching the whole list
		 * after a save froze that save's stacks, images and heap segments into
		 * the prefix, the list grew by ~200 per save, and Haydee's fourth
		 * restore ran with it full and stacks it never got to hold. */
		i = g_ctl->nex - 1;
		why = g_ctl->ex_why[f];
		g_ctl->ex_lo[i] = g_ctl->ex_lo[f];
		g_ctl->ex_hi[i] = g_ctl->ex_hi[f];
		g_ctl->ex_why[i] = why;
		g_ctl->ex_lo[f] = lo;
		g_ctl->ex_hi[f] = hi;
		g_ctl->ex_why[f] = "savestate_exclude, called by the wrapper";
		g_ctl->nex_fixed = f + 1;
	}
}

void *savestate_kept(int id, size_t bytes)
{
	void *p;

	if (id < 0 || id >= SS_KEPT || !bytes || !ensure_helper())
		return NULL;
	if (g_ctl->kept[id])
		return g_ctl->kept[id];
	p = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!p)
		return NULL;
	savestate_exclude(p, bytes);
	g_ctl->kept[id] = p;
	return p;
}

/* Thread-set mismatch after the most recent restore.
 *
 * The restored state can refer to threads that have since exited, and threads
 * that appeared after the save hold stacks and TEBs pointing into memory the
 * restore just moved. Both are Class B straddles.
 *
 * Returns: number of threads present at save but gone now.
 * Writes *fresh (threads live now but not at save), *recycled (same entry
 * point under a new TID), *gone (at-save threads that exited) when non-NULL.
 *
 * Zero cost - reads counts the restore already computed. */
int savestate_thread_set(int *fresh, int *recycled, int *gone)
{
	if (!g_ctl) {
		if (fresh) *fresh = 0;
		if (recycled) *recycled = 0;
		if (gone) *gone = 0;
		return 0;
	}
	if (fresh) *fresh = g_ctl->last_fresh;
	if (recycled) *recycled = g_ctl->last_recycled;
	if (gone) *gone = g_ctl->last_gone;
	return g_ctl->last_gone;
}

/* -------------------------------------------------- player position only
 *
 * The smallest possible savestate: eight bytes, written straight into the
 * player entity, touching no heap bookkeeping, no thread context, no page
 * commit state and nothing the engine snapshots.
 *
 * Worth having because it splits a question the full restore cannot. Every
 * failure so far has been our whole-memory machinery tearing the game's object
 * graph in half, and from inside that it is impossible to tell whether the game
 * merely dislikes being torn or dislikes being written to at all. If a position
 * write survives, the game tolerates external state changes fine and the fault
 * is entirely ours. If even this kills it, something far more basic is wrong
 * and the whole snapshot approach needs rethinking rather than repairing.
 *
 * Addresses are the LiveSplit autosplitter's, which the speedrunning community
 * maintains per version. Ours is v1.65, identified the same way the splitter
 * does it - SizeOfImage 0x10CE000 - and checked before anything is read, since
 * these offsets are meaningless and dangerous against any other build. The
 * entity pointer is indirect: a pointer at the fixed address, then the floats
 * live inside whatever it points at. */
#define RR_V165_IMAGE 0x10CE000u
#define RR_ENTITY_PTR 0x940EE0u
#define RR_X_OFF 0x0Cu
#define RR_Y_OFF 0x10u
#define RR_MAPID 0xA908F8u

static int rr_entity(uintptr_t *ent, unsigned *mapid)
{
	HMODULE exe = GetModuleHandleA(NULL);
	uintptr_t base = (uintptr_t)exe;
	uintptr_t p;

	if (!savestate_host_is("rabiribi.exe"))
		return 0;
	if (pe_image_span(exe) != RR_V165_IMAGE) {
		ss_log("pos: this is not the v1.65 build these offsets belong to "
		       "(image is %lX, expected %lX) - refusing to read\n",
		       (unsigned long)pe_image_span(exe),
		       (unsigned long)RR_V165_IMAGE);
		return 0;
	}
	if (!ss_readable(base + RR_ENTITY_PTR, sizeof(uintptr_t)))
		return 0;
	p = *(const uintptr_t *)(base + RR_ENTITY_PTR);
	/* The pointer is null before a save file is loaded, and reading through it
	 * on the title screen is the obvious way to turn a diagnostic into a
	 * crash report about itself. */
	if (!p || !ss_readable(p + RR_Y_OFF, sizeof(float)))
		return 0;
	if (mapid && ss_readable(base + RR_MAPID, sizeof(unsigned)))
		*mapid = *(const unsigned *)(base + RR_MAPID);
	*ent = p;
	return 1;
}

/* The seam between a restored world and the game's live machinery.
 *
 * After a restore, does the entity-table root at image+RR_ENTITY_PTR point back
 * at the entity the save recorded, or did the machinery fail to reconstitute? A
 * within-map or last-save restore comes back with the root aimed at the witness
 * entity - the world is coherent. A cross-map restore of a non-primary save comes
 * back with the root null, unmapped, or aimed at a DIFFERENT entity, because we
 * swapped the owned region but not the machinery that rebuilds it - and that root
 * (plus whatever it walks into, and the terrain) is exactly what a wider bank has
 * to carry. Read-only; logged on every restore so the two cases sit side by side. */
static void rr_seam_dump(void)
{
	HMODULE exe = GetModuleHandleA(NULL);
	uintptr_t base = (uintptr_t)exe;
	uintptr_t root = 0;
	unsigned map = 0xFFFFFFFFu;
	const char *state;

	if (!g_ctl || !savestate_host_is("rabiribi.exe"))
		return;
	if (ss_readable(base + RR_ENTITY_PTR, sizeof(uintptr_t)))
		root = *(const uintptr_t *)(base + RR_ENTITY_PTR);
	if (ss_readable(base + RR_MAPID, sizeof(unsigned)))
		map = *(const unsigned *)(base + RR_MAPID);
	if (!root)
		state = "NULL - the machinery did not rebuild the player (world not "
			"reconstituted)";
	else if (!ss_readable(root + RR_Y_OFF, sizeof(float)))
		state = "unmapped - the root points at nothing committed";
	else if (g_ctl->wit_valid && root == g_ctl->wit_ent)
		state = "MATCHES the witness entity - the world came back coherent";
	else
		state = "readable but NOT the witness entity - a different world "
			"reconstituted at this root";
	ss_log("  seam: entity-root image+%lX = %08lX, live mapid %lu; save was map "
	       "%u, entity %08lX - %s\n",
	       (unsigned long)RR_ENTITY_PTR, (unsigned long)root, (unsigned long)map,
	       g_ctl->wit_map, (unsigned long)g_ctl->wit_ent, state);
}

/* How much of the entity travels. Eight bytes is x and y alone, which is
 * measured to work room to room inside one world. Widening it is how we find
 * out what else is worth carrying without guessing which fields matter: set a
 * span, see what breaks, narrow down. Capped at the buffer. */
static int pos_span(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[16];
		DWORD n = ss_getenv("D3D9SW_POS_SPAN", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v)) ? atoi(v) : 8;
		/* 0 turns the force OFF entirely - the wholesale restore now places the
		 * player on its own, and the force's single shared mark was clobbering
		 * every restore with the LAST save's position (the "last slot prevails"
		 * bug). Any other value is at least x and y (8 bytes). */
		if (cached < 0)
			cached = 0;
		else if (cached > 0 && cached < 8)
			cached = 8;
		if (cached > (int)sizeof(g_ctl->pos_buf))
			cached = (int)sizeof(g_ctl->pos_buf);
	}
	return cached;
}

/* What the load boundary does to the process, sampled either side of it.
 *
 * The interesting thing about a transition is not the position - it is that
 * the game tears down and rebuilds state at a moment it chooses, using its own
 * allocator, correctly, every time. That is the behaviour our restore is
 * trying to imitate and keeps failing at. So watch for the boundary and take a
 * cheap census across it: which heaps exist, how much each one has committed.
 * Whatever moves is state the game itself considers per-load, and whatever
 * does not is state it considers permanent. We have never had that split from
 * anything but our own guessing.
 *
 * Cheap enough to run every frame because the per-frame part is two reads; the
 * census only happens on the frames where the number actually changed. */
struct HeapFoot {
	HANDLE h;
	SIZE_T committed;
	SIZE_T regions;
};

static int heap_footprint(struct HeapFoot *out, int max)
{
	HANDLE hs[64];
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t a;
	DWORD n, i;
	int k;

	n = GetProcessHeaps(64, hs);
	if (n > 64)
		n = 64;
	k = (int)n < max ? (int)n : max;
	for (i = 0; i < (DWORD)k; i++) {
		out[i].h = hs[i];
		out[i].committed = 0;
		out[i].regions = 0;
	}
	/* One pass over the address space, not one per heap. The first version of
	 * this walked all 2 GB once for every heap, which with sixteen heaps is
	 * hundreds of thousands of queries on a frame the game is waiting on.
	 *
	 * VirtualQuery rather than HeapWalk on purpose: HeapWalk takes the heap
	 * lock and would deadlock against a game thread caught mid-allocation. */
	for (a = 0; a < 0x7FFF0000u;) {
		void *owner;
		int j;

		if (!VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)))
			break;
		if (!mbi.RegionSize)
			break;
		a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		if (mbi.State != MEM_COMMIT || !(mbi.Type & MEM_PRIVATE))
			continue;
		/* The header only lives at the allocation base, and only if that
		 * base is itself committed. Committed region, reserved base is
		 * common - a heap reserves a span and commits into the middle of
		 * it - and probing it anyway is an access violation. An earlier
		 * version of this did exactly that and took down a save. */
		if ((uintptr_t)mbi.AllocationBase != (uintptr_t)mbi.BaseAddress) {
			MEMORY_BASIC_INFORMATION base;

			if (!VirtualQuery(mbi.AllocationBase, &base, sizeof(base)))
				continue;
			if (base.State != MEM_COMMIT)
				continue;
		}
		owner = segment_owner((uintptr_t)mbi.AllocationBase, mbi.Protect);
		if (!owner)
			continue;
		for (j = 0; j < k; j++)
			if (out[j].h == owner) {
				out[j].committed += mbi.RegionSize;
				out[j].regions++;
				break;
			}
	}
	return k;
}

/* Content, not size.
 *
 * The first census measured how much each heap had committed and watched a
 * complete world load move exactly zero bytes of it. The game preallocates and
 * rewrites in place, so extent says nothing about what a load touches. What we
 * actually want is which bytes differ, so the heaps get hashed in fixed chunks
 * and the chunk hashes compared across the event.
 *
 * That turns "what does a load change" into a number in megabytes, which is
 * the number this whole exercise is trying to find. */
#define CHUNK_BYTES (64u << 10)
#define CHUNK_MAX 16384

struct Chunk {
	uintptr_t base;
	unsigned hash;
};

static struct Chunk *g_chunk_a SS_PRESENT, *g_chunk_b SS_PRESENT;
static int g_chunk_n;
static int g_chunk_have;

static unsigned chunk_hash(const unsigned *p, size_t words)
{
	unsigned h = 2166136261u;
	size_t i;

	for (i = 0; i < words; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

/* Fills out[] with one entry per committed 64 KB chunk that belongs to a
 * process heap, in ascending address order. Same walk and same base-committed
 * check as heap_footprint - a reserved allocation base must never be probed. */
static int content_scan(struct Chunk *out, int max)
{
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t a;
	int k = 0;

	for (a = 0; a < 0x7FFF0000u && k < max;) {
		uintptr_t c, end;

		if (!VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)))
			break;
		if (!mbi.RegionSize)
			break;
		a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		if (mbi.State != MEM_COMMIT || !(mbi.Type & MEM_PRIVATE))
			continue;
		if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
			continue;
		if ((uintptr_t)mbi.AllocationBase != (uintptr_t)mbi.BaseAddress) {
			MEMORY_BASIC_INFORMATION base;

			if (!VirtualQuery(mbi.AllocationBase, &base, sizeof(base)))
				continue;
			if (base.State != MEM_COMMIT)
				continue;
		}
		if (!segment_owner((uintptr_t)mbi.AllocationBase, mbi.Protect))
			continue;
		end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		for (c = (uintptr_t)mbi.BaseAddress; c < end && k < max;
		     c += CHUNK_BYTES) {
			size_t n = (size_t)(end - c);

			if (n > CHUNK_BYTES)
				n = CHUNK_BYTES;
			out[k].base = c;
			out[k].hash = chunk_hash((const unsigned *)c, n / 4);
			k++;
		}
	}
	return k;
}

static void content_report(const char *when)
{
	struct Chunk *now, *prev;
	int n, i, j;
	int same = 0, diff = 0, fresh = 0, gone;
	unsigned long long diff_bytes = 0;

	g_chunk_a = (struct Chunk *)savestate_kept(SS_KEPT_CHUNK_A,
						   sizeof(struct Chunk) * CHUNK_MAX);
	g_chunk_b = (struct Chunk *)savestate_kept(SS_KEPT_CHUNK_B,
						   sizeof(struct Chunk) * CHUNK_MAX);
	if (!g_chunk_a || !g_chunk_b)
		return;
	/* Always scan into b and compare against a, then a takes the new scan.
	 * One copy per census is nothing next to the hashing. */
	now = g_chunk_b;
	prev = g_chunk_a;
	n = content_scan(now, CHUNK_MAX);
	if (!g_chunk_have) {
		memcpy(g_chunk_a, g_chunk_b, sizeof(struct Chunk) * (size_t)n);
		g_chunk_n = n;
		g_chunk_have = 1;
		ss_log("content %s: %d chunk(s), %llu MB - first scan, nothing to "
		       "compare against yet\n",
		       when, n, ((unsigned long long)n * CHUNK_BYTES) >> 20);
		return;
	}
	/* Both lists are in ascending address order, so one merge pass does it. */
	for (i = 0, j = 0; i < n;) {
		if (j >= g_chunk_n || now[i].base < prev[j].base) {
			fresh++;
			i++;
			continue;
		}
		if (prev[j].base < now[i].base) {
			j++;
			continue;
		}
		if (now[i].hash == prev[j].hash) {
			same++;
		} else {
			diff++;
			diff_bytes += CHUNK_BYTES;
		}
		i++;
		j++;
	}
	gone = g_chunk_n - (same + diff);
	if (gone < 0)
		gone = 0;
	ss_log("content %s: %d chunk(s), %d unchanged, %d changed, %d new, %d "
	       "gone - %llu MB of %llu MB differs\n",
	       when, n, same, diff, fresh, gone, diff_bytes >> 20,
	       ((unsigned long long)n * CHUNK_BYTES) >> 20);
	memcpy(g_chunk_a, g_chunk_b, sizeof(struct Chunk) * (size_t)n);
	g_chunk_n = n;
}

static unsigned rr_mapid_now(void)
{
	HMODULE exe = GetModuleHandleA(NULL);
	uintptr_t base = (uintptr_t)exe;

	if (!base || !savestate_host_is("rabiribi.exe") ||
	    pe_image_span(exe) != RR_V165_IMAGE)
		return 0;
	if (!ss_readable(base + RR_MAPID, sizeof(unsigned)))
		return 0;
	return *(const unsigned *)(base + RR_MAPID);
}

/* Remembered between calls so every census can say what moved since the last
 * one rather than just what is there now. A heap's absolute size tells us very
 * little; the delta across an event is the whole signal. */
static void boundary_census(const char *when)
{
	static struct HeapFoot prev[64];
	static int nprev;
	struct HeapFoot f[64];
	int n, i, j, moved = 0;
	unsigned long long total = 0;

	if (!g_ctl)
		return;
	n = heap_footprint(f, 64);
	for (i = 0; i < n; i++)
		total += f[i].committed;
	ss_log("census %s: mapid %u, %d heap(s), %llu KB committed\n", when,
	       rr_mapid_now(), n, total >> 10);
	for (i = 0; i < n; i++) {
		long long delta = (long long)f[i].committed;
		int seen = 0;

		for (j = 0; j < nprev; j++)
			if (prev[j].h == f[i].h) {
				delta -= (long long)prev[j].committed;
				seen = 1;
				break;
			}
		if (delta)
			moved++;
		ss_log("  heap %08lX  %6llu KB  %llu region(s)  %s%+lld KB\n",
		       (unsigned long)(uintptr_t)f[i].h,
		       (unsigned long long)(f[i].committed >> 10),
		       (unsigned long long)f[i].regions, seen ? "" : "NEW ",
		       delta >> 10);
	}
	/* The line worth reading. Heaps that move across the game's own load are
	 * heaps the game rebuilds for itself, and are candidates to stop
	 * restoring; heaps that never move are permanent, and are the ones our
	 * restore is most likely tearing. */
	ss_log("census %s: %d of %d heap(s) changed since the previous census\n",
	       when, moved, n);
	content_report(when);
	memcpy(prev, f, sizeof(struct HeapFoot) * (size_t)n);
	nprev = n;
}

/* A census on demand, so the delta can be taken over a window we choose.
 *
 * The boundary and save censuses both straddle a load, which makes their delta
 * a mixture of what the load rewrote and what ordinary play rewrote in the same
 * window. Two of these with nothing but play between them separates the two,
 * and that separation is the whole question: state the game rebuilds at a load
 * is state we can decline to carry, state that only play changes is state a
 * savestate has to carry. */
static const char *const g_wit_why[] = {
	"her allocation never matched the block map at all - same address and "
		"size was not found at restore time, so no rule ever got to vote "
		"on it",
	"written",
	"her block lands in no captured region, so there was nothing to write "
		"from",
	"a thread that keeps running was holding a pointer into her block, so it "
		"was left in the present on purpose (D3D9SW_HELDVETO=0 to stop "
		"that)",
	"her block's first word points into a module we hold in the present, so "
		"it was read as somebody else's object (D3D9SW_VTABVETO)",
	"the ownership closure never reached her block from anything we capture, "
		"so it counts as nobody's (D3D9SW_BLKOWNER)",
	"the ownership closure reached her block and so did Windows, and deny "
		"wins (D3D9SW_BLKOWNER)",
	"her block was freed and handed out again between the save and now, so "
		"the address holds a different object and the snapshot's bytes "
		"are not its bytes (D3D9SW_RECYCLED=1 to write it anyway)",
	"the copy out of the snapshot failed"
};

static void witness_save(Slot *s)
{
	uintptr_t ent;
	unsigned map = 0;
	int i, in_save = 0;
	uintptr_t xa;

	if (!g_ctl || !rr_entity(&ent, &map))
		return;
	xa = ent + RR_X_OFF;
	for (i = 0; i < s->nregs; i++)
		if (xa >= s->regs[i].base && xa < s->regs[i].base + s->regs[i].size) {
			in_save = 1;
			g_ctl->wit_reg = i;
			break;
		}
	g_ctl->wit_blk = 0;
	g_ctl->wit_why = WIT_UNSEEN;
	g_ctl->wit_ent = ent;
	g_ctl->wit_x = *(const float *)(ent + RR_X_OFF);
	g_ctl->wit_y = *(const float *)(ent + RR_Y_OFF);
	g_ctl->wit_map = map;
	g_ctl->wit_valid = 1;
	/* Snapshot into the slot, so restoring THIS save later checks against the
	 * world it was taken in even after another slot has overwritten the shared
	 * active witness. */
	s->wit_ent = g_ctl->wit_ent;
	s->wit_x = g_ctl->wit_x;
	s->wit_y = g_ctl->wit_y;
	s->wit_map = g_ctl->wit_map;
	s->wit_valid = 1;
	ss_log("  witness: player at x=%d y=%d, entity %08lX, world %u - %s\n",
	       (int)g_ctl->wit_x, (int)g_ctl->wit_y, (unsigned long)ent, map,
	       in_save ? "INSIDE a captured region"
		       : "NOT IN ANY CAPTURED REGION, so a restore cannot move her");
	if (in_save)
		ss_log("  witness: region %d, %08lX +%lu KB, and it will be "
		       "restored %s\n",
		       g_ctl->wit_reg, (unsigned long)s->regs[g_ctl->wit_reg].base,
		       (unsigned long)(s->regs[g_ctl->wit_reg].size >> 10),
		       /* Both halves of the test the restore actually makes.
			* Reporting only heap_rewound_at said "BY BLOCK" on a run
			* with block mode switched off, which is the same lie the
			* syscall and event counters tell: a label describing a
			* path that did not run. */
		       (g_blk_ready && heap_rewound_at(s->regs[g_ctl->wit_reg].base))
			       ? "BY BLOCK - only allocations the block map matched "
				 "get written, so she travels only if her block "
				 "was matched"
			       : "wholesale");
}

/* The process heap roster.
 *
 * Windows keeps the list of live heaps in the PEB: a count and an array of
 * handles. That list is memory, so a restore rewinds it - while the heaps it
 * describes are left in the present, because we do not rewind heaps. The two
 * then disagree, and the disagreement is not symmetric: a heap the game created
 * after the save still exists, still holds allocations, and still has a handle
 * the game is using, but has been struck off the roster. The heap check calls
 * these forgotten heaps, and it found two of them in the run before this was
 * written.
 *
 * Nothing good comes of telling Windows it has fewer heaps than it has. So the
 * roster is read before the restore writes anything and put back afterwards,
 * which leaves the list describing the heaps that actually exist. It is a count
 * and a handful of pointers - the smallest possible fix for the largest
 * mismatch we have found.
 *
 * Offsets are the documented PEB layout for each architecture. The TEB gives us
 * the PEB without a syscall. */
#if defined(_M_IX86) || defined(__i386__)
#define PEB_FROM_TEB 0x30
#define PEB_NHEAPS 0x88
#define PEB_HEAPARR 0x90
#else
#define PEB_FROM_TEB 0x60
#define PEB_NHEAPS 0xE8
#define PEB_HEAPARR 0xF0
#endif

static int roster_keep(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_ROSTER", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '0') ? 0 : 1;
	}
	return cached;
}

static uintptr_t peb_base(void)
{
	uintptr_t teb = (uintptr_t)NtCurrentTeb();

	if (!teb || !ss_readable(teb + PEB_FROM_TEB, sizeof(uintptr_t)))
		return 0;
	return *(const uintptr_t *)(teb + PEB_FROM_TEB);
}

static void roster_save(void)
{
	uintptr_t peb = peb_base();
	unsigned i, n;
	uintptr_t arr;

	if (!g_ctl)
		return;
	g_ctl->roster_valid = 0;
	if (!roster_keep() || !peb)
		return;
	if (!ss_readable(peb + PEB_NHEAPS, sizeof(unsigned)) ||
	    !ss_readable(peb + PEB_HEAPARR, sizeof(uintptr_t)))
		return;
	n = *(const unsigned *)(peb + PEB_NHEAPS);
	arr = *(const uintptr_t *)(peb + PEB_HEAPARR);
	if (!arr || n > 64)
		n = n > 64 ? 64 : n;
	if (!arr || !ss_readable(arr, n * sizeof(uintptr_t)))
		return;
	for (i = 0; i < n; i++)
		g_ctl->roster_ent[i] = *(const uintptr_t *)(arr + i * sizeof(uintptr_t));
	g_ctl->roster_n = n;
	g_ctl->roster_arr = arr;
	g_ctl->roster_valid = 1;
}

/* Runs while the threads are still suspended, so no allocation can observe the
 * moment when the roster and the heaps disagreed. */
static void roster_load(void)
{
	uintptr_t peb = peb_base();
	unsigned i, now_n, put = 0;

	if (!g_ctl || !g_ctl->roster_valid || !peb)
		return;
	if (!ss_readable(peb + PEB_NHEAPS, sizeof(unsigned)))
		return;
	now_n = *(const unsigned *)(peb + PEB_NHEAPS);
	if (ss_readable(g_ctl->roster_arr, g_ctl->roster_n * sizeof(uintptr_t)))
		for (i = 0; i < g_ctl->roster_n; i++) {
			uintptr_t *slot =
				(uintptr_t *)(g_ctl->roster_arr + i * sizeof(uintptr_t));

			if (*slot == g_ctl->roster_ent[i])
				continue;
			*slot = g_ctl->roster_ent[i];
			put++;
		}
	*(unsigned *)(peb + PEB_NHEAPS) = g_ctl->roster_n;
	/* Reported even when nothing moved, because "the rewind did not touch
	 * the roster" is the result that would retire this whole idea, and a
	 * silent success looks identical to a knob that never ran. */
	if (now_n == g_ctl->roster_n && !put)
		ss_log("  peb heaps: roster already matched the present, %u heap(s) "
		       "- the rewind did not disturb it\n",
		       g_ctl->roster_n);
	else
		ss_log("  peb heaps: the rewind left %u heap(s) on the roster, the "
		       "present has %u - put back, %u handle(s) corrected, so no "
		       "live heap is disowned\n",
		       now_n, g_ctl->roster_n, put);
}

/* Hand-carried state.
 *
 * The player lives in a heap segment, and with the heaps left in the present
 * that segment is never captured - the coverage report names it directly.
 * Rewinding the heaps to reach her is the other side of a fork where both ends
 * are dead: hold the allocator's bookkeeping and ntdll divides by zero, restore
 * it and ntdll fastfails on corrupt metadata.
 *
 * So this copies the bytes and nothing else. No allocator structure is read or
 * written, no headers, no lists - just the span of one live object, taken while
 * the threads are suspended and put back while they still are. */
static int carry(void)
{
	static int cached = -1;

	if (cached < 0) {
		char v[8];
		DWORD n = ss_getenv("D3D9SW_CARRY", v, sizeof(v));

		cached = (n > 0 && n < sizeof(v) && v[0] == '0') ? 0 : 1;
	}
	return cached;
}

static void carry_save(void)
{
	uintptr_t ent;
	unsigned map = 0;
	int span;

	if (!g_ctl || !carry())
		return;
	g_ctl->carry_valid = 0;
	if (!rr_entity(&ent, &map))
		return;
	span = pos_span();
	if (span > (int)sizeof(g_ctl->carry_buf))
		span = (int)sizeof(g_ctl->carry_buf);
	if (!ss_readable(ent + RR_X_OFF, (size_t)span))
		return;
	memcpy(g_ctl->carry_buf, (const void *)(ent + RR_X_OFF), (size_t)span);
	g_ctl->carry_span = span;
	g_ctl->carry_ent = ent;
	g_ctl->carry_map = map;
	g_ctl->carry_valid = 1;
	ss_log("  carry: %d byte(s) of the player taken by hand from %08lX, "
	       "because her segment is not in the snapshot\n",
	       span, (unsigned long)ent);
}

/* Runs before the threads resume, so the game never observes the intermediate
 * state where the world has been rewound and she has not. */
static void carry_load(void)
{
	uintptr_t ent;
	unsigned map = 0;

	if (!g_ctl || !carry() || !g_ctl->carry_valid)
		return;
	if (!rr_entity(&ent, &map)) {
		ss_log("  carry: no player entity to write into\n");
		return;
	}
	if (ent != g_ctl->carry_ent)
		ss_log("  carry: entity moved, %08lX at the save and %08lX now - the "
		       "object was reallocated and this span lands on new fields\n",
		       (unsigned long)g_ctl->carry_ent, (unsigned long)ent);
	if (!ss_readable(ent + RR_X_OFF, (size_t)g_ctl->carry_span)) {
		ss_log("  carry: entity at %08lX is not writable for %d byte(s)\n",
		       (unsigned long)ent, g_ctl->carry_span);
		return;
	}
	memcpy((void *)(ent + RR_X_OFF), g_ctl->carry_buf,
	       (size_t)g_ctl->carry_span);
	ss_log("  carry: %d byte(s) written back to %08lX before the threads "
	       "resume\n",
	       g_ctl->carry_span, (unsigned long)ent);
}

/* Which room the game is standing in as the restore begins, against the one the
 * save was taken in.
 *
 * Read here rather than after the restore, because afterwards the answer is
 * always the saved room and the question disappears. A restore inside one room
 * asks the game to accept slightly different values for things it already has.
 * A restore across a room boundary asks it to un-happen a room change: the
 * entity pool was torn down and rebuilt, so the allocations the save described
 * have been freed and reissued, and the block map matches far less of what it
 * expected. Those are different amounts of work and there is no reason to
 * assume they have the same failure rate - but until this line existed the log
 * could not tell them apart, so every restore looked alike in the record. */
/* Returns 0 to refuse the restore outright.
 *
 * Every cross-world restore ever logged has killed the process, and the ones
 * measured carefully killed it at frame zero: four for four in one session, and
 * again under -softsound and -xaudio2 once those were tried. Same-world
 * restores survive, and saving inside a newly entered world is fine. It is
 * specifically going back across a world boundary that cannot be done.
 *
 * The reason is not mysterious and is not something a better copy fixes. The
 * entity pool is torn down and rebuilt on a world change, so the block map is
 * being asked to match allocations that were freed and reissued in the
 * meantime. Addresses are identity here: an object restored into memory that
 * now belongs to something else is not that object, and no amount of care
 * about which bytes we write changes who owns them.
 *
 * So refuse, and say so. A refusal costs the player one reload; letting it
 * proceed costs them the process, and has every time. This is the same
 * judgement the block ownership tests make - convict rather than acquit, and
 * withhold when we cannot show the memory is ours to move.
 *
 * D3D9SW_CROSSWORLD=1 allows it anyway, because the day the underlying problem
 * is fixed this is how it gets tested. */
static int room_check(void)
{
	uintptr_t ent;
	unsigned map = 0;
	char v[8];
	DWORD got;

	if (!g_ctl || !g_ctl->wit_valid)
		return 1;
	if (!rr_entity(&ent, &map)) {
		ss_log("  room: cannot read the map id right now, so this restore is "
		       "unclassified\n");
		return 1;
	}
	if (map == g_ctl->wit_map) {
		ss_log("  room: restoring inside map %u, the same one the save was taken "
		       "in\n", map);
		return 1;
	}
	got = ss_getenv("D3D9SW_CROSSWORLD", v, sizeof(v));
	if (got > 0 && got < sizeof(v) && v[0] == '1') {
		ss_log("  room: restoring ACROSS a room change - saved in map %u, standing "
		       "in map %u. The entity pool has been torn down and rebuilt since "
		       "the save, so the block map is being asked to match allocations "
		       "that were freed and reissued. Allowed by D3D9SW_CROSSWORLD=1, "
		       "and this has never once survived\n",
		       g_ctl->wit_map, map);
		return 1;
	}
	ss_log("  room: REFUSED - the save was taken in map %u and you are standing in "
	       "map %u. The entity pool was torn down and rebuilt on the way between "
	       "them, so the addresses in the save now belong to other objects and "
	       "restoring would write this world's memory with the last one's. Nothing "
	       "has been touched; the game is exactly as it was. Walk back to map %u "
	       "and the same save will load, or set D3D9SW_CROSSWORLD=1 to try it "
	       "anyway\n",
	       g_ctl->wit_map, map, g_ctl->wit_map);
	return 0;
}

static void witness_load(void)
{
	uintptr_t ent;
	unsigned map = 0;
	float x, y;

	/* Before the early return, so the FAILING case - the one where the root did
	 * not reconstitute and rr_entity bails - is the one that gets logged. */
	rr_seam_dump();
	if (!g_ctl || !g_ctl->wit_valid || !rr_entity(&ent, &map))
		return;
	x = *(const float *)(ent + RR_X_OFF);
	y = *(const float *)(ent + RR_Y_OFF);
	/* Only meaningful on the block path; wholesale writes everything. */
	if (g_blk_ready) {
		int why = g_ctl->wit_why;

		if (why < 0 || why >= (int)(sizeof(g_wit_why) / sizeof(g_wit_why[0])))
			why = WIT_UNSEEN;
		if (why == WIT_WRITTEN)
			ss_log("  witness: her block was MATCHED and written by the "
			       "block map\n");
		else
			ss_log("  witness: her block went unwritten - %s. Everything "
			       "else in the entity pool turned away by the same rule "
			       "went with it, which is why counters and projectiles "
			       "do not travel while the carried span does\n",
			       g_wit_why[why]);
	}
	if (ent == g_ctl->wit_ent && x == g_ctl->wit_x && y == g_ctl->wit_y) {
		ss_log("  witness: player IS back at x=%d y=%d, world %u - the "
		       "restore reached her\n",
		       (int)x, (int)y, map);
		return;
	}
	ss_log("  witness: player NOT restored. saved x=%d y=%d entity %08lX "
	       "world %u, now x=%d y=%d entity %08lX world %u%s\n",
	       (int)g_ctl->wit_x, (int)g_ctl->wit_y,
	       (unsigned long)g_ctl->wit_ent, g_ctl->wit_map, (int)x, (int)y,
	       (unsigned long)ent, map,
	       ent != g_ctl->wit_ent ? " - THE ENTITY POINTER ITSELF MOVED, so "
				       "the pointer that names her was not put back"
				     : "");
}

static int pos_ready(void);

/* Our own key edges, because GetAsyncKeyState's low bit is a process-wide
 * one-shot: it reports "pressed since anyone last asked" and clears on read.
 * DxLib polls the keyboard too, so whichever of us calls first eats the bit
 * and the other sees nothing. That is why F6 needed several presses to take
 * and why the second of two F7s never appeared in the log - the press was
 * real, the flag had already been consumed.
 *
 * The 0x8000 bit is the physical key state and is not consumed by reading, so
 * tracking the transition ourselves is reliable no matter who else polls.
 *
 * The previous-state bitmap lives in Control specifically because Control is
 * excluded from the snapshot. Held in an ordinary static it would rewind with
 * the rest of our image on every restore, and a key that was down at save time
 * would read as a fresh press afterwards - which on F5 means restoring again,
 * forever. */
/* Is the game the window the keyboard is actually talking to?
 *
 * GetAsyncKeyState reads the keyboard, not this window's share of it, so
 * every hotkey here fires from whatever the user is typing into - a browser,
 * an editor, a remote-desktop client, the middle of an IME composition. A
 * stray F5 then takes a save, or worse a restore, against a game that is not
 * even on screen, and the log records a session the player did not ask for.
 *
 * It is also wrong in a way that outlives the keypress. A restore delivered
 * while another window owns the focus resumes the game holding a keyboard
 * state it never saw arrive at, because the presses in between were addressed
 * to somebody else.
 *
 * Asked of the foreground window's process rather than a saved HWND: the game
 * has more than one window over its life, and the one that has focus is the
 * one the keys are going to. */
static int ours_has_focus(void)
{
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;

	if (!fg)
		return 0;
	GetWindowThreadProcessId(fg, &pid);
	return pid == GetCurrentProcessId();
}

int savestate_key_edge(int vk)
{
	static unsigned fallback[8];
	unsigned *bits;
	int down, was;

	/* Carry the fallback over the moment Control appears, or the handover
	 * invents a press: the key that was down in the static reads as up in the
	 * fresh zeroed Control, and a still-held key looks like a new edge. That
	 * is one press producing two censuses, which is exactly what the first
	 * build of this did. Seeded through Control's own flag rather than a
	 * static, so a restore cannot un-seed it and repeat the trick. */
	if (g_ctl && !g_ctl->key_seeded) {
		memcpy(g_ctl->key_down, fallback, sizeof(fallback));
		g_ctl->key_seeded = 1;
	}
	bits = g_ctl ? g_ctl->key_down : fallback;
	/* Tracked even when it is not ours, so a key held down across an alt-tab
	 * back into the game is already marked down and does not read as a fresh
	 * press the moment focus returns. The edge is suppressed; the state is
	 * not. */
	down = (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
	was = (bits[(vk >> 5) & 7] >> (vk & 31)) & 1;
	if (!ours_has_focus()) {
		if (down)
			bits[(vk >> 5) & 7] |= 1u << (vk & 31);
		else
			bits[(vk >> 5) & 7] &= ~(1u << (vk & 31));
		return 0;
	}

	if (down)
		bits[(vk >> 5) & 7] |= 1u << (vk & 31);
	else
		bits[(vk >> 5) & 7] &= ~(1u << (vk & 31));
	return down && !was;
}

int savestate_key_held(int vk)
{
	return ours_has_focus() && (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
}

/* A key name a person can write in the config, turned into a virtual-key code.
 *
 * Three forms, because anything looser cannot be read back unambiguously. A
 * bare "1" is the digit key, not virtual-key 1 - VK_LBUTTON is 1, and a config
 * that silently bound save to the left mouse button would be a very bad
 * afternoon. So a single character is always that character's key, a leading
 * 0x is the code itself for anything without a name, and F followed by digits
 * is the function row.
 *
 * Returns def for anything it does not recognise rather than guessing, and the
 * caller logs what it resolved, so a typo shows up as "still on F5" in the
 * session header instead of as a key that does nothing. */
static int vk_parse(const char *s, int n, int def)
{
	int i, v = 0;

	if (n <= 0)
		return def;
	if (n == 1) {
		char c = s[0];

		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		/* Digits and letters are their own virtual-key codes; nothing else
		 * on a US layout is, so punctuation has to go through 0x. */
		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))
			return (int)(unsigned char)c;
		return def;
	}
	if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		for (i = 2; i < n; i++) {
			char c = s[i];

			if (c >= '0' && c <= '9')
				v = v * 16 + (c - '0');
			else if (c >= 'a' && c <= 'f')
				v = v * 16 + (c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				v = v * 16 + (c - 'A' + 10);
			else
				return def;
		}
		return (v > 0 && v < 256) ? v : def;
	}
	if (s[0] == 'F' || s[0] == 'f') {
		for (i = 1; i < n; i++) {
			if (s[i] < '0' || s[i] > '9')
				return def;
			v = v * 10 + (s[i] - '0');
		}
		if (v >= 1 && v <= 24)
			return VK_F1 + v - 1;
	}
	return def;
}

static int g_vk_save, g_vk_load;
/* D3D9SW_SLOT_VK: one base key for interleaved save/load PAIRS - slot k saves on
 * base+2k and loads on base+2k+1. Base '1' (0x31) gives 1=save0 2=load0 3=save1
 * 4=load1 5=save2 6=load2 7=save3 8=load3: number-row, no modifier, save and load
 * for a slot side by side. Overrides the separate-range SAVE_VK/LOAD_VK scheme. */
static int g_vk_pair;

/* Resolved once and never re-read. A binding that changed mid-session would
 * mean the key that saved a slot is not the key that restores it, and the log
 * would describe a session nobody ran. */
static void hotkeys_resolve(void)
{
	static int done, said;
	char v[16];

	if (!done) {
		DWORD n;

		done = 1;
		n = ss_getenv("D3D9SW_SAVE_VK", v, sizeof(v));
		g_vk_save = vk_parse(v, (n < sizeof(v)) ? (int)n : 0, VK_F5);
		n = ss_getenv("D3D9SW_LOAD_VK", v, sizeof(v));
		g_vk_load = vk_parse(v, (n < sizeof(v)) ? (int)n : 0, 0);
		n = ss_getenv("D3D9SW_SLOT_VK", v, sizeof(v));
		g_vk_pair = vk_parse(v, (n < sizeof(v)) ? (int)n : 0, 0);
	}
	/* Deferred rather than printed above, because this runs every frame from
	 * the first one and the log does not exist until the control block does.
	 * Standing the block up from a hotkey poll would allocate it in every
	 * session whether or not anything ever asked for a save. */
	if (!said && g_ctl) {
		said = 1;
		if (g_vk_pair) {
			ss_log("hotkeys: interleaved pairs from base 0x%x - slot k saves on "
			       "base+2k, loads on base+2k+1: 0x%x/0x%x = save/load slot 0, "
			       "0x%x/0x%x = slot 1. Number row, no modifier. Uses %d key(s) "
			       "above the base for %d slot(s)\n",
			       g_vk_pair, g_vk_pair, g_vk_pair + 1, g_vk_pair + 2,
			       g_vk_pair + 3, 2 * SAVESTATE_SLOTS, SAVESTATE_SLOTS);
		} else if (g_vk_load) {
			ss_log("hotkeys: save on 0x%x, load on 0x%x - separate keys, so "
			       "no modifier has to be held across a second press\n",
			       g_vk_save, g_vk_load);
			/* Each base covers SAVESTATE_SLOTS consecutive keys, so two
			 * bases closer together than that overlap, and load is tested
			 * first: the key the config calls save-slot-1 would restore
			 * slot 0 instead. Harmless at one slot and silent at two,
			 * which is exactly the kind of thing that gets diagnosed as a
			 * broken restore a year from now. */
			if (SAVESTATE_SLOTS > 1) {
				int lo = g_vk_save < g_vk_load ? g_vk_save : g_vk_load;
				int hi = g_vk_save < g_vk_load ? g_vk_load : g_vk_save;

				if (hi - lo < SAVESTATE_SLOTS)
					ss_log("  hotkeys: WARNING - %d slot(s) means each "
					       "base claims %d consecutive keys, and these "
					       "two are %d apart, so they overlap. Load is "
					       "tested first, so a save key inside the load "
					       "range will restore instead. Move them at "
					       "least %d apart\n",
					       SAVESTATE_SLOTS, SAVESTATE_SLOTS, hi - lo,
					       SAVESTATE_SLOTS);
			}
		} else {
			ss_log("hotkeys: save on 0x%x, load on shift+0x%x. Set "
			       "D3D9SW_LOAD_VK to give load a key of its own, which is "
			       "what an automated run needs\n",
			       g_vk_save, g_vk_save);
		}
	}
}

int savestate_hotkey(int slot)
{
	hotkeys_resolve();
	if (slot < 0 || slot >= SAVESTATE_SLOTS)
		return SS_HOTKEY_NONE;
	/* Interleaved pairs: slot k saves on base+2k, loads on base+2k+1. Two
	 * different keys per slot, so order does not matter and no modifier is
	 * involved - 1/2 for slot 0, 3/4 for slot 1, straight up the number row. */
	if (g_vk_pair) {
		if (savestate_key_edge(g_vk_pair + 2 * slot)) {
			ss_log("hotkey: key 0x%x -> SAVE slot %d\n",
			       g_vk_pair + 2 * slot, slot);
			return SS_HOTKEY_SAVE;
		}
		if (savestate_key_edge(g_vk_pair + 2 * slot + 1)) {
			ss_log("hotkey: key 0x%x -> LOAD slot %d\n",
			       g_vk_pair + 2 * slot + 1, slot);
			return SS_HOTKEY_LOAD;
		}
		return SS_HOTKEY_NONE;
	}
	/* Load first. With separate keys the two are different keys and the order
	 * cannot matter; with one key and a modifier it would, and checking load
	 * first keeps the two paths reading the same way. */
	if (g_vk_load) {
		if (savestate_key_edge(g_vk_load + slot))
			return SS_HOTKEY_LOAD;
		if (savestate_key_edge(g_vk_save + slot))
			return SS_HOTKEY_SAVE;
		return SS_HOTKEY_NONE;
	}
	if (savestate_key_edge(g_vk_save + slot))
		return savestate_key_held(VK_SHIFT) ? SS_HOTKEY_LOAD : SS_HOTKEY_SAVE;
	return SS_HOTKEY_NONE;
}

void savestate_probe_census(void)
{
	if (!pos_ready())
		return;
	boundary_census("f7");
}

/* A map of what is alive, built without knowing anything about the game.
 *
 * We have no types, no symbols and no object list, so we cannot ask the game
 * what is on screen. But a thing that is on screen and moving has a signature
 * that needs no type information: a few words, close together, whose values
 * change continuously. Sweep memory, keep the addresses that changed since we
 * last looked, cluster them by proximity, and what falls out is the live
 * objects and where they live.
 *
 * The point is not the objects themselves - it is which side of the seam they
 * are on. A cluster inside a heap will not survive a restore under the current
 * settings; a cluster outside the heaps will. That turns "bullets sometimes
 * come back" from something you notice into something we can count before the
 * restore happens.
 *
 * Cost is kept to one window per frame. A full pass over the candidate memory
 * therefore takes tens of frames, which means this sees anything that lives
 * longer than about half a second and can miss anything shorter. That is a real
 * limitation and the report says so rather than implying it saw everything. */
#define OM_WIN (1u << 20)
#define OM_MAX 262144
/* One object is a run of changed words with no large gap - but in a dense
 * region like a particle pool that rule will happily swallow a hundred
 * kilobytes and call it one thing. Runs are cut off here so a busy area is
 * reported as many objects rather than one implausible one. */
#define OM_GAP 128
#define OM_SPAN 2048

/* The list the last save was built from, so the question asked is the one that
 * matters: not which heap owns this address, but whether the snapshot covers
 * it. Heap membership got this wrong - a heap's later segments sit far from its
 * handle, so live objects in them were reported as captured when they were
 * not. */
static void cap_record(Slot *s)
{
	int i;

	if (!g_ctl)
		return;
	g_ctl->cap_n = 0;
	for (i = 0; i < s->nregs && g_ctl->cap_n < 1024; i++) {
		g_ctl->cap_base[g_ctl->cap_n] = s->regs[i].base;
		g_ctl->cap_size[g_ctl->cap_n] = s->regs[i].size;
		g_ctl->cap_n++;
	}
}

static int om_captured(uintptr_t a)
{
	int i;

	if (!g_ctl)
		return 0;
	for (i = 0; i < g_ctl->cap_n; i++)
		if (a >= g_ctl->cap_base[i] && a < g_ctl->cap_base[i] + g_ctl->cap_size[i])
			return 1;
	return 0;
}

static unsigned char *g_om_shadow SS_PRESENT;
static uintptr_t g_om_base;
static size_t g_om_len;
static uintptr_t g_om_cursor;
static uintptr_t *g_om_hit SS_PRESENT;  /* address | 1 when the new value looks like a coordinate */
static int g_om_hit_n;
static uintptr_t *g_om_done SS_PRESENT;
static int g_om_done_n;
static unsigned g_om_sweeps;

/* Coordinates in this game are floats in the thousands. Anything with a sane
 * exponent and a magnitude in playfield range counts; the test only has to be
 * good enough to tell a moving position from a frame counter. */
static int om_coordlike(unsigned bits)
{
	unsigned exp = (bits >> 23) & 0xFF;
	float f;

	if (exp == 0 || exp == 0xFF)
		return 0;
	memcpy(&f, &bits, sizeof(f));
	if (f < 0)
		f = -f;
	return f >= 1.0f && f <= 200000.0f;
}

/* D3D9SW_OBJWATCH, off unless asked for.
 *
 * The object monitor shadows a megabyte of the address space each frame and
 * diffs it against the frame before, moving the window on by a VirtualQuery
 * walk. That is the right shape for finding a moving object by hand, and the
 * wrong thing to leave running: measured at up to 173.5 ms of a 185.1 ms
 * frame, against 1.9 ms for the game itself and 0.0 for the audio drain. The
 * dips came back on a cadence because the walk sweeps the address space and
 * the cost follows how densely regions are packed where the cursor happens to
 * be. It had no knob, so every session paid for it. */
static int objwatch_mode(void)
{
	static int v = -1;

	if (v < 0) {
		char b[8];
		DWORD n = ss_getenv("D3D9SW_OBJWATCH", b, sizeof(b));

		v = (n && b[0] == '1') ? 1 : 0;
	}
	return v;
}

static int om_ready(void)
{
	if (!objwatch_mode())
		return 0;
	if (g_om_shadow)
		return 1;
	g_om_shadow = (unsigned char *)blk_arena(OM_WIN);
	g_om_hit = (uintptr_t *)blk_arena(OM_MAX * sizeof(uintptr_t));
	g_om_done = (uintptr_t *)blk_arena(OM_MAX * sizeof(uintptr_t));
	return g_om_shadow && g_om_hit && g_om_done;
}

/* Ordinary writable private memory only. Images and mappings are not where the
 * game keeps moving objects, and our own bookkeeping is skipped so the map does
 * not fill up with the instrument watching itself. */
static int om_candidate(const MEMORY_BASIC_INFORMATION *mbi)
{
	DWORD p = mbi->Protect & 0xFF;

	if (mbi->State != MEM_COMMIT || mbi->Type != MEM_PRIVATE)
		return 0;
	if (p != PAGE_READWRITE && p != PAGE_EXECUTE_READWRITE)
		return 0;
	if ((uintptr_t)mbi->BaseAddress == (uintptr_t)g_ctl)
		return 0;
	return 1;
}

void savestate_object_watch(void)
{
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t at;
	size_t take;

	if (!g_ctl || !om_ready())
		return;
	/* Compare what we shadowed last frame against what it says now. */
	if (g_om_len && ss_readable(g_om_base, g_om_len)) {
		const unsigned *live = (const unsigned *)g_om_base;
		const unsigned *was = (const unsigned *)g_om_shadow;
		size_t i, words = g_om_len / sizeof(unsigned);

		for (i = 0; i < words && g_om_hit_n < OM_MAX; i++) {
			if (live[i] == was[i])
				continue;
			g_om_hit[g_om_hit_n++] = (g_om_base + i * sizeof(unsigned)) |
						 (om_coordlike(live[i]) ? 1u : 0u);
		}
	}
	g_om_len = 0;
	/* Then move the window on. */
	for (at = g_om_cursor; at < 0x7FFF0000u;) {
		if (VirtualQuery((LPCVOID)at, &mbi, sizeof(mbi)) != sizeof(mbi))
			break;
		if (!om_candidate(&mbi)) {
			at = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
			continue;
		}
		take = (size_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - at);
		if (take > OM_WIN)
			take = OM_WIN;
		if (ss_readable(at, take)) {
			memcpy(g_om_shadow, (const void *)at, take);
			g_om_base = at;
			g_om_len = take;
			g_om_cursor = at + take;
			return;
		}
		at = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
	}
	/* Wrapped. The sweep just finished is the one worth reporting, so it is
	 * kept whole rather than blended into the next one. */
	memcpy(g_om_done, g_om_hit, (size_t)g_om_hit_n * sizeof(uintptr_t));
	g_om_done_n = g_om_hit_n;
	g_om_hit_n = 0;
	g_om_cursor = 0;
	g_om_sweeps++;
}

void savestate_object_report(void)
{
	int i, j, clusters = 0, in_heap = 0, outside = 0, shown = 0;
	int obj_heap = 0, obj_out = 0;

	dsh_survey();
	if (g_ctl && !g_ctl->cap_n)
		ss_log("objects: nothing saved yet, so there is no region list to "
		       "check against - press F5 first or every object will read as "
		       "uncaptured\n");
	if (!g_ctl || !g_om_done_n) {
		ss_log("objects: no completed sweep yet - %u so far, each takes a few "
		       "dozen frames\n",
		       g_om_sweeps);
		return;
	}
	/* Insertion sort by address. The list is the changed words of one sweep,
	 * already close to sorted because the sweep walks upward. */
	for (i = 1; i < g_om_done_n; i++) {
		uintptr_t v = g_om_done[i];

		for (j = i - 1; j >= 0 && (g_om_done[j] & ~1u) > (v & ~1u); j--)
			g_om_done[j + 1] = g_om_done[j];
		g_om_done[j + 1] = v;
	}
	ss_log("objects: sweep %u, %d word(s) changed since the sweep before it%s\n",
	       g_om_sweeps, g_om_done_n,
	       g_om_done_n >= OM_MAX
		       ? " <<< THE TABLE FILLED, so this sweep stopped early and the "
			 "counts below are a floor, not a total"
		       : "");
	for (i = 0; i < g_om_done_n;) {
		uintptr_t base = g_om_done[i] & ~1u;
		uintptr_t end = base;
		int words = 0, coords = 0, heap;

		/* One object is a run of changed words with no large gap. Anything
		 * further than this apart is treated as a different thing. */
		for (j = i; j < g_om_done_n; j++) {
			uintptr_t a = g_om_done[j] & ~1u;

			if (a > end + OM_GAP || a - base > OM_SPAN)
				break;
			end = a;
			words++;
			if (g_om_done[j] & 1u)
				coords++;
		}
		heap = !om_captured(base);
		clusters++;
		if (heap)
			in_heap++;
		else
			outside++;
		/* Two coordinate-like words together is the signature of something
		 * with a position, which is what "on screen" means here. */
		if (coords >= 2) {
			if (heap)
				obj_heap++;
			else
				obj_out++;
			if (shown < 20) {
				ss_log("  object %08lX +%lu bytes, %d word(s) changed, %d "
				       "coordinate-like - %s\n",
				       (unsigned long)base,
				       (unsigned long)(end - base + 4), words, coords,
				       heap ? "NOT in the last save, so a restore will "
					      "not reach it"
					    : "in the last save, so a restore puts it "
					      "back");
				shown++;
			}
		}
		i = j;
	}
	ss_log("objects: %d cluster(s) of change, %d outside the last save and %d inside\n",
	       clusters, in_heap, outside);
	if (!g_ctl->cap_n)
		ss_log("objects: %d look like positioned things - whether any of them "
		       "survive a restore is UNKNOWN, because nothing has been saved "
		       "to compare against\n",
		       obj_heap + obj_out);
	else
		ss_log("objects: %d look like positioned things - %d of those survive a "
		       "restore, %d do not. Anything alive for less than about half a "
		       "second can be missed by a sweep this size\n",
		       obj_heap + obj_out, obj_out, obj_heap);
}

void savestate_pos_watch(void)
{
	HMODULE exe;
	uintptr_t base;
	unsigned map;

	if (!g_ctl || !savestate_host_is("rabiribi.exe"))
		return;
	exe = GetModuleHandleA(NULL);
	base = (uintptr_t)exe;
	if (!base || pe_image_span(exe) != RR_V165_IMAGE)
		return;
	if (!ss_readable(base + RR_MAPID, sizeof(unsigned)))
		return;
	map = *(const unsigned *)(base + RR_MAPID);
	if (!g_ctl->watch_seen) {
		g_ctl->watch_seen = 1;
		g_ctl->watch_map = map;
		return;
	}
	if (map == g_ctl->watch_map)
		return;
	ss_log("\n==== load boundary: mapid %u -> %u ====\n", g_ctl->watch_map,
	       map);
	boundary_census("boundary");
	g_ctl->watch_map = map;
}

/* The position tool keeps its state in Control because Control is excluded from
 * every snapshot, which is the only storage in this process that survives a
 * restore. But Control is built by the first save, so before one has happened
 * this feature silently did nothing - and ss_log discards its complaint for the
 * same reason, so it did not even say so. Measured the hard way: F11 looked
 * broken for a whole session purely because F5 had not been pressed first.
 *
 * Standing it up here costs a page and removes the ordering requirement. */
static int pos_ready(void)
{
	if (!g_ctl)
		ensure_helper();
	return g_ctl != NULL;
}

void savestate_pos_mark(void)
{
	uintptr_t ent;
	unsigned map = 0;
	int span;

	if (!pos_ready())
		return;
	span = pos_span();
	if (span == 0)
		return; /* force disabled (D3D9SW_POS_SPAN=0) */
	if (!rr_entity(&ent, &map)) {
		ss_log("pos: nothing to mark - no player entity yet\n");
		return;
	}
	if (!ss_readable(ent + RR_X_OFF, (size_t)span)) {
		ss_log("pos: entity at %08lX is not readable for %d byte(s)\n",
		       (unsigned long)ent, span);
		return;
	}
	memcpy(g_ctl->pos_buf, (const void *)(ent + RR_X_OFF), (size_t)span);
	g_ctl->pos_span = span;
	g_ctl->pos_map = map;
	g_ctl->pos_ent = ent;
	g_ctl->pos_valid = 1;
	ss_log("pos: marked x=%d y=%d on world %u, %d byte(s) from entity %08lX\n",
	       (int)*(const float *)(ent + RR_X_OFF),
	       (int)*(const float *)(ent + RR_Y_OFF), map, span,
	       (unsigned long)ent);
}

void savestate_pos_restore(void)
{
	uintptr_t ent;
	unsigned map = 0;
	float ox, oy;

	if (!pos_ready())
		return;
	if (pos_span() == 0)
		return; /* force disabled - the wholesale restore stands on its own */
	if (!g_ctl->pos_valid) {
		ss_log("pos: nothing marked yet\n");
		return;
	}
	if (!rr_entity(&ent, &map)) {
		ss_log("pos: no player entity to restore into\n");
		return;
	}
	/* Reported, never refused. An earlier version of this gated the write on
	 * the field below matching, which was a guess about what that field counts
	 * dressed up as a safety rail - and it blocked the transfers that work.
	 * The write is always allowed; the number is evidence, not a verdict. */
	if (map != g_ctl->pos_map)
		ss_log("pos: mapid %u at the mark, %u now\n", g_ctl->pos_map, map);
	if (!ss_readable(ent + RR_X_OFF, (size_t)g_ctl->pos_span)) {
		ss_log("pos: entity at %08lX is not writable for %d byte(s)\n",
		       (unsigned long)ent, g_ctl->pos_span);
		return;
	}
	ox = *(const float *)(ent + RR_X_OFF);
	oy = *(const float *)(ent + RR_Y_OFF);
	/* Worth saying when it happens: a different entity address means the
	 * object was reallocated since the mark, so we are writing into a new
	 * one and any span past x and y is landing on fields that moved. */
	if (ent != g_ctl->pos_ent)
		ss_log("pos: NOTE the entity moved, %08lX at the mark and %08lX "
		       "now\n",
		       (unsigned long)g_ctl->pos_ent, (unsigned long)ent);
	memcpy((void *)(ent + RR_X_OFF), g_ctl->pos_buf, (size_t)g_ctl->pos_span);
	ss_log("pos: restored x=%d y=%d (was x=%d y=%d), %d byte(s), world %u\n",
	       (int)*(const float *)(ent + RR_X_OFF),
	       (int)*(const float *)(ent + RR_Y_OFF), (int)ox, (int)oy,
	       g_ctl->pos_span, map);
}

/* ------------------------------------------------------- crash chain probe
 *
 * Both crashes ran the same three frames - +33DCE, +36370, +6F4xx - and both
 * produced negative copy lengths from +6E9F8 before dying. Forcing that path on
 * demand would turn a crash that costs a play session into one that costs a
 * keystroke, but it cannot be written blind, for two reasons found the hard way.
 *
 * The addresses are return addresses. +33DCE is the instruction after a call,
 * not the entry of what was called, so jumping there lands mid-function with a
 * half-built frame and crashes for reasons that have nothing to do with a
 * restore - which would look exactly like a successful reproduction.
 *
 * And the entries cannot be derived offline. rabiribi.exe carries a .bind
 * section, so the shipped file is Steam-DRM encrypted and decrypted at load:
 * the nine bytes decoder_patch_once finds in memory every session appear
 * nowhere in the six megabytes on disk, and the bytes at the faulting address
 * on disk are not the instruction cdb disassembled there. Only the running
 * process knows what this code is.
 *
 * So the probe reads the decrypted image and reports what is actually at each
 * call site. A 32-bit direct call is E8 rel32, so where the byte five back is
 * E8 the callee entry is exact arithmetic rather than a prologue guess. It
 * writes nothing: the point is to find out whether a forced call is possible
 * and where it would have to enter, before anything is patched. Printing the
 * entry RVAs each session also answers the cheaper half of the question on its
 * own, since RVAs that match across sessions mean the chain itself is stable
 * and only the heap addresses move. */
static void chain_probe_one(uintptr_t base, unsigned rva, const char *what)
{
	const unsigned char *p = (const unsigned char *)(base + rva);
	unsigned target;
	int rel;

	if (!ss_readable((uintptr_t)(p - 5), 16)) {
		ss_log("  %-12s +%06X  not readable\n", what, rva);
		return;
	}
	if (p[-5] != 0xE8) {
		ss_log("  %-12s +%06X  before: %02X %02X %02X %02X %02X | at: "
		       "%02X %02X %02X  - not E8, so this call site is indirect "
		       "and the callee cannot be derived from the bytes alone\n",
		       what, rva, p[-5], p[-4], p[-3], p[-2], p[-1], p[0], p[1],
		       p[2]);
		return;
	}
	rel = *(const int *)(p - 4);
	target = rva + (unsigned)rel;
	if (!ss_readable(base + target, 8)) {
		ss_log("  %-12s +%06X  E8 -> +%06X, but the entry is not readable\n",
		       what, rva, target);
		return;
	}
	{
		const unsigned char *e = (const unsigned char *)(base + target);

		ss_log("  %-12s +%06X  E8 rel32 -> callee entry +%06X  entry bytes "
		       "%02X %02X %02X %02X %02X %02X %02X %02X\n",
		       what, rva, target, e[0], e[1], e[2], e[3], e[4], e[5], e[6],
		       e[7]);
	}
}

void savestate_chain_probe(void)
{
	uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);

	if (!savestate_host_is("rabiribi.exe")) {
		ss_log("chain: not probing - these are rabiribi.exe offsets and this "
		       "is a different executable\n");
		return;
	}
	ss_log("chain: the decoder path both crashes ran, read from the decrypted "
	       "image at base %08lX. Return addresses, so the callee entry is what "
	       "a forced call would have to target\n",
	       (unsigned long)base);
	chain_probe_one(base, 0x33DCE, "outer");
	chain_probe_one(base, 0x36370, "middle");
	chain_probe_one(base, 0x6F533, "inner-a");
	chain_probe_one(base, 0x6F4FB, "inner-b");
	chain_probe_one(base, 0x6E9F8, "memcpy-1");
	chain_probe_one(base, 0x36913A, "memcpy-2");
	ss_log("chain: memcpy-2 at +36913A is the site the decoder patch does NOT "
	       "cover - it produced -968573 and -1220276 byte copies in two "
	       "separate sessions while the patch was reported APPLIED, so there "
	       "is a second signed comparison the nine-byte signature misses\n");
}

/* ------------------------------------------------------------- soak driver
 *
 * Every measurement of this game so far has cost a manual session: launch, play
 * somewhere interesting, press F5, press shift-F5, read one number, and the
 * process is usually dead by then so that is the whole run. One point per
 * session cannot show a shape, and the shape is the question. The last run
 * survived eight frames where earlier ones survived none, and there is no way
 * to tell from a single sample whether that means anything.
 *
 * So arm once and let it run trials by itself. Each trial saves, lets the game
 * run on for a dwell, restores, and counts the frames the game manages before
 * something kills it. The dwell doubles every rung. That ladder is the
 * experiment: if survival falls as the dwell grows, the damage is a function of
 * how far the present has moved on since the snapshot, and divergence is the
 * variable. If survival is flat, it is not, and the distance we have been
 * assuming matters does not.
 *
 * It does not stop at the first bad trial, and it is not trying to succeed.
 * Reaching a rung that kills the game reliably is the useful outcome, because
 * a repeatable death at a known dwell is something a debugger can be pointed
 * at, and a whole session of them costs one arming keystroke. */
enum { SOAK_OFF = 0, SOAK_SETTLE, SOAK_DWELL, SOAK_WATCH, SOAK_DONE };

static int soak_knob(const char *name, int def)
{
	char v[16];
	DWORD n = ss_getenv(name, v, sizeof(v));

	if (!n || n >= sizeof(v))
		return def;
	return atoi(v);
}

/* Print the whole curve every time a rung finishes.
 *
 * Reprinting costs a few lines and buys the one thing that matters here: the
 * run ends by dying, usually without warning, so whatever was written last has
 * to be self-contained. A summary that only appears at the end would be the
 * summary that never appears. */
static void soak_curve(void)
{
	int i;

	ss_log("soak: survival curve so far\n");
	for (i = 0; i < g_ctl->soak_rows; i++)
		ss_log("      dwell %5d frame(s) -> survived %5d frame(s)%s\n",
		       g_ctl->soak_dwells[i], g_ctl->soak_survived[i],
		       g_ctl->soak_survived[i] >= g_ctl->soak_watch ? "  (still alive,"
								      " capped)"
								    : "");
}

void savestate_soak_arm(void)
{
	if (!g_ctl)
		return;
	if (g_ctl->soak_state != SOAK_OFF && g_ctl->soak_state != SOAK_DONE) {
		g_ctl->soak_state = SOAK_DONE;
		ss_log("soak: disarmed by hand at trial %d\n", g_ctl->soak_trial);
		return;
	}
	g_ctl->soak_settle = soak_knob("D3D9SW_SOAK_SETTLE", 90);
	g_ctl->soak_watch = soak_knob("D3D9SW_SOAK_WATCH", 120);
	g_ctl->soak_max = soak_knob("D3D9SW_SOAK_MAX", 2048);
	g_ctl->soak_ladder = soak_knob("D3D9SW_SOAK_LADDER", 2);
	g_ctl->soak_dwell = soak_knob("D3D9SW_SOAK_DWELL", 1);
	if (g_ctl->soak_ladder < 2)
		g_ctl->soak_ladder = 2;
	if (g_ctl->soak_dwell < 1)
		g_ctl->soak_dwell = 1;
	g_ctl->soak_trial = 0;
	g_ctl->soak_rows = 0;
	g_ctl->soak_frame = 0;
	g_ctl->soak_state = SOAK_SETTLE;
	ss_log("soak: armed. settle %d, dwell %d doubling to %d, watch %d frame(s) "
	       "per trial. The game is expected to die somewhere on this ladder; "
	       "where it dies is the measurement\n",
	       g_ctl->soak_settle, g_ctl->soak_dwell, g_ctl->soak_max,
	       g_ctl->soak_watch);
}

/* Called once per frame by the wrapper, right after savestate_guard.
 *
 * Returns what it wants done this frame. It deliberately does not call save or
 * load itself: the wrapper wraps both in ledger work - register, mark, reap,
 * the retain flush - and a second call site that skipped any of it would be a
 * double free waiting to happen. Returning an action keeps one path. */
/* D3D9SW_SAVE_AT=N takes one save on frame N and never again.
 *
 * The allocation trace is written when a save is taken, so it holds everything
 * the game asked for between launch and that moment. Two traces therefore only
 * compare cleanly if both were cut at the same point - and a hand on F5 cannot
 * hit the same frame twice. The difference is not small: two runs cut by hand
 * differed by 92 operations and 107 blocks, and every block allocated after the
 * point where they parted looks unstable whether it is or not.
 *
 * Counting frames removes the hand. It says nothing about whether the game did
 * the same things in between - only that both runs were asked the question at
 * the same moment, which is the part that was in our power to fix.
 *
 * It returns an action rather than saving, for the same reason the soak driver
 * does: the wrapper wraps a save in ledger work, and a second call site that
 * skipped any of it would be a double free waiting to happen. */
#define SS_SAVE_AT_MAX 8

static int g_save_at[SS_SAVE_AT_MAX];
static int g_save_at_n = -1;
static int g_save_at_i;
static long g_save_at_frame;

/* A comma-separated list of frame numbers, ascending. Several cuts rather than
 * one because the interesting question is no longer "did it reproduce" but "how
 * far in does it stop reproducing", and one cut per run would mean one data
 * point per launch. Each cut writes the whole trace so far, so the files nest:
 * the second contains the first. */
static void save_at_parse(void)
{
	char v[64];
	DWORD n = ss_getenv("D3D9SW_SAVE_AT", v, sizeof(v));
	DWORD k;
	int cur = 0, any = 0;

	g_save_at_n = 0;
	if (!n || n >= sizeof(v))
		return;
	for (k = 0; k <= n; k++) {
		if (k < n && v[k] >= '0' && v[k] <= '9') {
			cur = cur * 10 + (v[k] - '0');
			any = 1;
		} else {
			/* Frame 0 cannot be saved - the game has not ticked once - and a
			 * bare "0" is how a cfg means OFF, the way LOAD_AT and QUIT_AT
			 * already read it. Without this guard "0" scheduled a save on
			 * frame 1 (the counter pre-increments to 1 and 1 < 0 is false),
			 * an auto-save nobody asked for. Use SAVE_AT=1 for a frame-1 cut. */
			if (any && cur > 0 && g_save_at_n < SS_SAVE_AT_MAX)
				g_save_at[g_save_at_n++] = cur;
			cur = 0;
			any = 0;
		}
	}
}

/* -------------------------------------------------------- scripted input
 *
 * D3D9SW_KEY_EVERY presses a key on a fixed frame schedule.
 *
 * Every determinism result so far has been taken with nothing pressed, which
 * leaves the obvious question open: allocation follows what the player does,
 * and a hand cannot press the same key on the same frame twice. So the hand has
 * to go, the same way it went for the save trigger.
 *
 * Injected through SendInput rather than posted to the window, because the game
 * reads the keyboard through DirectInput and DirectInput does not look at the
 * window's message queue. An injected event goes through the same kernel input
 * path a real key does, which is the only path both agree on.
 *
 * The scan code is filled in alongside the virtual key. DirectInput deals in
 * scan codes, and an event carrying only a virtual key is one it may decline to
 * translate.
 *
 * Only while the game is in front: injected input goes to the foreground
 * window, so pressing keys while the player has alt-tabbed away would type into
 * whatever they switched to. That check also makes a run non-deterministic if
 * focus is lost, which is worth knowing rather than hiding - the log line says
 * how many presses were sent, so two runs that disagree there explain
 * themselves. */
static void key_send(WORD vk, int up)
{
	INPUT in;

	memset(&in, 0, sizeof(in));
	in.type = INPUT_KEYBOARD;
	in.ki.wVk = vk;
	in.ki.wScan = (WORD)MapVirtualKeyA(vk, 0 /* MAPVK_VK_TO_VSC */);
	in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
	SendInput(1, &in, sizeof(in));
}

static long g_key_sent;

static void key_at_tick(void)
{
	static int every = -1, from, hold, vk;
	static long frame;
	static int down;

	if (every < 0) {
		every = soak_knob("D3D9SW_KEY_EVERY", 0);
		from = soak_knob("D3D9SW_KEY_FROM", 300);
		hold = soak_knob("D3D9SW_KEY_HOLD", 4);
		vk = soak_knob("D3D9SW_KEY_VK", 0x5A); /* Z */
		if (hold < 1)
			hold = 1;
		if (every > 0)
			ss_log("key-at: pressing VK %02X for %d frame(s) every %d "
			       "frame(s) from frame %d, so both runs receive the "
			       "same input on the same frames\n",
			       vk, hold, every, from);
	}
	if (every <= 0)
		return;
	frame++;
	if (frame < from)
		return;
	{
		DWORD pid = 0;
		HWND fg = GetForegroundWindow();
		long phase;

		if (fg)
			GetWindowThreadProcessId(fg, &pid);
		if (pid != GetCurrentProcessId()) {
			/* Released if it was held, so losing focus mid-press does
			 * not leave the key stuck down. */
			if (down) {
				key_send((WORD)vk, 1);
				down = 0;
			}
			return;
		}
		phase = (frame - from) % every;
		if (phase == 0 && !down) {
			key_send((WORD)vk, 0);
			down = 1;
			g_key_sent++;
		} else if (down && phase >= hold) {
			key_send((WORD)vk, 1);
			down = 0;
		}
	}
}

/* Restore on a fixed frame, for the cross-session test.
 *
 * A state written to a file in one launch and read back in the next is the
 * whole question, and it cannot be asked by hand: the two launches have to
 * reach the same frame, from the same save file, with the same input, or a
 * failure says nothing about addresses and everything about the operator.
 *
 * Paired with D3D9SW_SAVE_AT in the first launch and D3D9SW_SLOTFILE so the
 * slot survives the process. The point is not to succeed; it is to find out
 * exactly which stage refuses first, and the log after this fires is the
 * answer. */
static int load_at_frame(void)
{
	static long at = -1;
	static long frame;
	static int done;

	if (at < 0) {
		at = (long)soak_knob("D3D9SW_LOAD_AT", 0);
		if (at > 0) {
			/* This knob exists for the session that restores a file it did
			 * not write, and such a session takes no save - so nothing else
			 * reaches ensure_helper, the control block never comes up, and
			 * the caller's "if (!g_ctl)" returned before this was ever
			 * called. Three cross-session launches did nothing and wrote no
			 * savestate log at all, because the block owns the log handle
			 * too. Standing it up here is what pos_ready does, for the same
			 * reason and after the same kind of session spent chasing it. */
			ensure_helper();
			ss_log("load-at: this run will restore slot 0 at frame %ld, "
			       "from whatever the last session left in the file\n",
			       at);
		}
	}
	if (at <= 0 || done || ++frame < at)
		return 0;
	done = 1;
	ss_log("load-at: frame %ld reached, asking for the restore\n", at);
	/* D3D9SW_REC_GATE=1: hold here until a recorder is attached, so the trace
	 * starts at the load. tools\ddpr_record.ps1 waits for Want and starts TTD
	 * with Go as its init-complete event. Created here, at the game's
	 * integrity, so the elevated side can open them and not the reverse. */
	if (soak_knob("D3D9SW_REC_GATE", 0) > 0) {
		HANDLE want = CreateEventA(NULL, TRUE, FALSE, "Local\\DdprRecWant");
		HANDLE go = CreateEventA(NULL, TRUE, FALSE, "Local\\DdprRecGo");
		DWORD t0 = GetTickCount(), r = WAIT_FAILED;

		if (want && go) {
			SetEvent(want);
			r = WaitForSingleObject(go, 120000);
		}
		ss_log("load-at: recorder gate %s after %lu ms\n",
		       r == WAIT_OBJECT_0 ? "opened" : "TIMED OUT or failed - loading unrecorded",
		       (unsigned long)(GetTickCount() - t0));
	}
	return 1;
}

/* D3D9SW_SAVE_AFTER_LOAD=N saves N frames after LOAD_AT's restore, the step
 * that follows a load from another launch in play. The countdown is in the
 * present section, so the restore it follows does not wind it back. */
static long g_save_after SS_PRESENT;

static void save_after_arm(void)
{
	g_save_after = soak_knob("D3D9SW_SAVE_AFTER_LOAD", 0);
	if (g_save_after > 0)
		ss_log("save-after-load: a save is due %ld frame(s) after this restore\n",
		       g_save_after);
}

/* D3D9SW_LOAD_AFTER_SAVE=N loads that save back N frames after taking it. */
static long g_load_after SS_PRESENT;

static int load_after_frame(void)
{
	if (g_load_after <= 0 || --g_load_after > 0)
		return 0;
	ss_log("load-after-save: loading the save taken after the restore\n");
	return 1;
}

static int save_after_frame(void)
{
	if (g_save_after <= 0)
		return 0;
	if (--g_save_after % 150 == 0 && g_save_after)
		ss_log("save-after-load: %ld frame(s) to go\n", g_save_after);
	if (g_save_after > 0)
		return 0;
	g_load_after = soak_knob("D3D9SW_LOAD_AFTER_SAVE", 0);
	ss_log("save-after-load: taking the save\n");
	return 1;
}

/* Quit on a fixed frame, so that two runs are the same length.
 *
 * The first comparison pair was ended by hand at different moments. The traces
 * matched exactly, but one log carried two Vorbis lines the other did not, for
 * no better reason than that one session had lived long enough to free a
 * comment header. That difference cost real time to rule out, and a comparison
 * whose runs end whenever somebody reaches for the window is not a comparison.
 *
 * WM_CLOSE rather than a kill: teardown allocates and frees, and a run that
 * skips its own shutdown is no more comparable than one that overstays it. */
static BOOL CALLBACK quit_find_window(HWND w, LPARAM lp)
{
	DWORD pid = 0;

	GetWindowThreadProcessId(w, &pid);
	if (pid != GetCurrentProcessId() || !IsWindowVisible(w))
		return TRUE;
	*(HWND *)lp = w;
	return FALSE;
}

static void quit_at_tick(void)
{
	static long at = -1;
	static long frame;
	HWND w = NULL;

	if (at < 0) {
		at = (long)soak_knob("D3D9SW_QUIT_AT", 0);
		if (at > 0)
			ss_log("quit-at: this run will close itself at frame %ld, so "
			       "it is exactly as long as the last one\n",
			       at);
	}
	if (at <= 0 || ++frame != at)
		return;

	EnumWindows(quit_find_window, (LPARAM)&w);
	ss_log("quit-at: frame %ld reached, closing %s\n", at,
	       w ? "the game's window" : "the process, no window was found");
	if (w)
		PostMessageA(w, WM_CLOSE, 0, 0);
	else
		ExitProcess(0);
}

static int save_at_frame(void)
{
	if (g_save_at_n < 0)
		save_at_parse();
	if (g_save_at_i >= g_save_at_n)
		return 0;
	g_save_at_frame++;
	if (g_save_at_frame < g_save_at[g_save_at_i])
		return 0;
	g_save_at_i++;
	/* The save that follows would build the control block a frame later, which
	 * is too late for the line below: ss_log discards anything written before
	 * the block exists, so an unattended run's own record of where it cut would
	 * be missing from the log it was cut for. */
	ensure_helper();
	ss_log("save-at: frame %ld, cut %d of %d - the allocation trace is written "
	       "here in every session\n",
	       g_save_at_frame, g_save_at_i, g_save_at_n);
	return 1;
}

int savestate_soak_action(void)
{
	int survived;

	/* Ahead of everything, including the control-block check: scripted input
	 * is about making two runs identical, and a run that skipped its presses
	 * because a pointer was not ready yet would be a run that silently is
	 * not comparable. */
	key_at_tick();
	/* Beside the input tick and for the same reason: a run that ends on a
	 * different frame is not comparable, whether or not the control block
	 * ever came up. */
	quit_at_tick();
	/* Ahead of the soak state check, because this is armed by a knob on its
	 * own and there is no reason to make a measurement run also arm the soak
	 * ladder to get at it. */
	if (save_at_frame())
		return SS_SOAK_SAVE;
	/* After the save trigger, so a config that sets both takes the state
	 * before trying to put one back and the ordering is never in doubt. */
	if (save_after_frame())
		return SS_SOAK_SAVE;
	if (load_after_frame())
		return SS_SOAK_LOAD;
	if (load_at_frame()) {
		save_after_arm();
		return SS_SOAK_LOAD;
	}
	/* Below both triggers, not above them. The soak ladder genuinely needs the
	 * control block, but the two frame knobs do not - they stand it up
	 * themselves - and gating them on it meant D3D9SW_LOAD_AT could only fire
	 * in a session that had already saved, which is precisely the session it
	 * was not written for. */
	if (!g_ctl)
		return SS_SOAK_NOTHING;
	if (g_ctl->soak_state == SOAK_OFF || g_ctl->soak_state == SOAK_DONE)
		return SS_SOAK_NOTHING;

	g_ctl->soak_frame++;
	switch (g_ctl->soak_state) {
	case SOAK_SETTLE:
		if (g_ctl->soak_frame < g_ctl->soak_settle)
			break;
		g_ctl->soak_trial++;
		g_ctl->soak_frame = 0;
		g_ctl->soak_state = SOAK_DWELL;
		ss_log("soak: trial %d, dwell %d frame(s) - saving\n",
		       g_ctl->soak_trial, g_ctl->soak_dwell);
		return SS_SOAK_SAVE;

	case SOAK_DWELL:
		if (g_ctl->soak_frame < g_ctl->soak_dwell)
			break;
		g_ctl->soak_frame = 0;
		g_ctl->soak_state = SOAK_WATCH;
		ss_log("soak: trial %d, %d frame(s) of divergence - restoring\n",
		       g_ctl->soak_trial, g_ctl->soak_dwell);
		return SS_SOAK_LOAD;

	case SOAK_WATCH:
		/* g_frames_since_load rather than our own counter, so this number
		 * and the one the fault printer reports are the same number. When
		 * the game dies mid-trial the log then reads consistently instead
		 * of offering two survival counts that differ by a frame or two. */
		survived = (int)g_frames_since_load;
		if (survived < g_ctl->soak_watch)
			break;
		if (g_ctl->soak_rows < (int)(sizeof(g_ctl->soak_dwells) /
					     sizeof(g_ctl->soak_dwells[0]))) {
			g_ctl->soak_dwells[g_ctl->soak_rows] = g_ctl->soak_dwell;
			g_ctl->soak_survived[g_ctl->soak_rows] = survived;
			g_ctl->soak_rows++;
		}
		ss_log("soak: trial %d SURVIVED %d frame(s) at dwell %d\n",
		       g_ctl->soak_trial, survived, g_ctl->soak_dwell);
		soak_curve();
		g_ctl->soak_dwell *= g_ctl->soak_ladder;
		g_ctl->soak_frame = 0;
		if (g_ctl->soak_dwell > g_ctl->soak_max) {
			g_ctl->soak_state = SOAK_DONE;
			ss_log("soak: ladder finished without killing it, which is "
			       "itself a result - the damage did not grow with "
			       "divergence over this range\n");
			break;
		}
		g_ctl->soak_state = SOAK_SETTLE;
		break;
	}
	return SS_SOAK_NOTHING;
}

