/* Keeping DirectSound's play cursor honest across a restore.
 *
 * The game streams audio by asking where the hardware is playing, comparing
 * that against where it last wrote, and copying the difference. That works
 * because time only moves forward. A restore breaks the assumption: dsound and
 * the audio stack are excluded from the rewind and have to be - they talk to
 * the sound card, and the card keeps playing while the game is stopped - so
 * afterwards the hardware's cursor is in the present and the game's write
 * position is several seconds in the past. The subtraction comes out negative.
 *
 * As an unsigned byte count that is 4.29 billion, and the game hands it to a
 * block copy. Caught at rabiribi.exe+36913A with ecx = FFEAAD9C, and again at
 * +6E9F8 with -128819. The second one wrote decoded audio over the allocator's
 * structures before it reached unmapped memory and faulted: ntdll was later
 * found dereferencing FFD6FFE3, which is not a pointer but two quiet 16-bit
 * samples, -42 and -29.
 *
 * A fault handler cannot fix this. rep movs faults only when it reaches memory
 * that is not there, having faithfully copied everything up to that point, so
 * by the time we see the exception the damage is done and clamping the count
 * only spares the remainder. The copy has to not be issued.
 *
 * So the cursor has to agree with the game's state, which means owning it.
 *
 * There is no proxy DLL here and no wrapper objects. A COM vtable is shared by
 * every instance of an implementation, so creating one throwaway device of our
 * own is enough to learn the address of the table the game's device will also
 * use. Patch the handful of methods that matter, release the throwaway, and
 * every buffer the game creates afterwards comes through us - regardless of
 * whether it got there via DirectSoundCreate8, the older entry point, or
 * CoCreateInstance, because all three land on the same implementation. */

#include "savestate.h"
#include <windows.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>

#ifndef DSBSTATUS_PLAYING
#define DSBSTATUS_PLAYING 0x00000001
#endif
#ifndef DSBPLAY_LOOPING
#define DSBPLAY_LOOPING 0x00000001
#endif

/* Vtable slots, counted from IUnknown. Fixed by the interface contract, so they
 * are as stable as the interface itself. */
#define DS_CREATE_BUFFER 3
#define DS_DUPLICATE 5

#define DSB_QI 0
#define DSB_RELEASE 2
#define DSB_LOCK 11
#define DSB_GET_POSITION 4
#define DSB_GET_FORMAT 5
#define DSB_GET_VOLUME 6
#define DSB_GET_STATUS 9
#define DSB_PLAY 12
#define DSB_SET_POSITION 13
#define DSB_SET_VOLUME 15
#define DSB_SET_PAN 16
#define DSB_SET_FREQ 17
#define DSB_STOP 18
#define DSB_UNLOCK 19
#define DSB_RESTORE 20

#define DSERR_BUFFERLOST 0x88780096L
#define DS_OK 0L

typedef HRESULT(WINAPI *PFN_DSCREATE8)(const GUID *, void **, void *);
typedef HRESULT(WINAPI *PFN_CREATEBUF)(void *, const void *, void **, void *);
typedef HRESULT(WINAPI *PFN_DUP)(void *, void *, void **);
typedef ULONG(WINAPI *PFN_RELEASE)(void *);
typedef HRESULT(WINAPI *PFN_LOCK)(void *, DWORD, DWORD, void **, DWORD *, void **, DWORD *,
				   DWORD);
typedef HRESULT(WINAPI *PFN_GETPOS)(void *, DWORD *, DWORD *);
typedef HRESULT(WINAPI *PFN_GETSTATUS)(void *, DWORD *);
typedef HRESULT(WINAPI *PFN_PLAY)(void *, DWORD, DWORD, DWORD);
typedef HRESULT(WINAPI *PFN_SETPOS)(void *, DWORD);
typedef HRESULT(WINAPI *PFN_STOP)(void *);

/* DxLib makes a buffer per sound effect, and 256 was a guess that the game
 * silently walked past: a run reported "256 of 256 buffer(s)" recorded, which
 * is the ceiling reporting itself as a result. Anything untracked keeps a play
 * cursor in the present while its write position is wound back, which is the
 * whole bug this file exists to remove. */
#define DSH_MAX 4096

typedef struct Buf {
	void *p;
	DWORD play, write; /* as of the last snapshot */
	DWORD status;
	int held; /* the snapshot has something to say about this one */
	void *owner; /* the return address of whoever asked for it */
} Buf;

/* Declared up here because two of the methods being counted were hooked long
 * before the survey existed, and their hooks sit above it. */
enum {
	C_QI,
	C_GETPOS,
	C_GETFMT,
	C_GETVOL,
	C_GETSTAT,
	C_LOCK,
	C_UNLOCK,
	C_PLAY,
	C_STOP,
	C_SETPOS,
	C_SETVOL,
	C_SETPAN,
	C_SETFREQ,
	C_MAX
};

static unsigned long g_calls[C_MAX];

static Buf g_buf[DSH_MAX];

/* What was audible in the present, immediately before a restore.
 *
 * dsh_play restarts what the SAVE recorded as playing, which is right and is
 * also how the music dies. One save taken during a silent beat - a track
 * change, a room transition - records silence, and from then on every restore
 * faithfully reproduces it. The next save then records the silence it just
 * caused. Measured across one session: eleven restores brought one buffer back,
 * then nine in a row brought back none, and the music stayed gone until the
 * game started a new track of its own accord.
 *
 * Music is not state. Being one bar out of place is nothing; being silent for
 * the rest of the session is the failure. So the present gets a vote: anything
 * audible just before the restore starts playing again afterwards, whether or
 * not the snapshot agrees it should be.
 *
 * Kept by pointer rather than by index, and in memory the snapshot does not
 * touch. g_buf is an ordinary static and rewinds with the rest of our image, so
 * an index recorded before the restore names a different buffer after it if any
 * were created in between - and a note written in rewinding storage would be
 * erased by the very event it exists to survive. */
typedef struct {
	int n;
	void *p[DSH_MAX];
} PresentSet;

static PresentSet *g_present SS_PRESENT;

static int present_has(void *p)
{
	int i;

	if (!g_present)
		return 0;
	for (i = 0; i < g_present->n; i++)
		if (g_present->p[i] == p)
			return 1;
	return 0;
}
static int g_nbuf;
static CRITICAL_SECTION g_cs;
static int g_ready, g_armed, g_capped;

static PFN_CREATEBUF g_real_create;
static PFN_DUP g_real_dup;
static PFN_RELEASE g_real_release;
static PFN_GETPOS g_real_getpos;
static PFN_LOCK g_real_lock;

typedef HRESULT(WINAPI *PFN_RESTORE)(void *);

/* Where a failed Lock sends the game's audio. Big enough for any single fill
 * DxLib asks for, and excluded from the snapshot so a restore cannot move it
 * out from under a write that is already in flight. */
#define SINK_BYTES (256 * 1024)
static void *g_sink;
static unsigned long g_lock_fail;

static void *lock_sink(void)
{
	if (!g_sink)
		g_sink = savestate_kept(SS_KEPT_DS_SINK, SINK_BYTES);
	return g_sink;
}
static void **g_buf_vtbl;

void savestate_log_line(const char *s); /* the engine's log, shared deliberately */
int ds_sw_armed(void); /* the software DirectSound actually served a device */

/* wsprintfA rather than the CRT: no floats are printed here and it drags in
 * nothing. It does not understand %p, though, and the two messages that used one
 * were dropped silently for the whole life of this file - which is why the
 * install was invisible in all 38 logged sessions while the save, printing only
 * %d, was not. Addresses are formatted as %08lX here for that reason. Findings go to the savestate log so they sit beside the restore they
 * explain. */
static void ss_log(const char *fmt, ...)
{
	char b[512];
	va_list ap;

	va_start(ap, fmt);
	wvsprintfA(b, fmt, ap);
	va_end(ap);
	savestate_log_line(b);
}

/* Name an address as module plus offset.
 *
 * The allocation base of any address inside a loaded image is that image's base,
 * so one VirtualQuery answers both which module and how far into it, without a
 * module list and without the loader lock. */
static void where(void *a, char *out, int n)
{
	MEMORY_BASIC_INFORMATION mbi;
	char path[MAX_PATH];
	const char *leaf;

	if (!a || !VirtualQuery(a, &mbi, sizeof(mbi)) || !mbi.AllocationBase) {
		wsprintfA(out, "%08lX (no module)", (unsigned long)(ULONG_PTR)a);
		return;
	}
	path[0] = 0;
	GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, sizeof(path));
	leaf = path[0] ? strrchr(path, '\\') : NULL;
	wsprintfA(out, "%08lX (%s+0x%lX)", (unsigned long)(ULONG_PTR)a,
		  leaf ? leaf + 1 : (path[0] ? path : "?"),
		  (unsigned long)((char *)a - (char *)mbi.AllocationBase));
	(void)n;
}

/* Who asks for sound buffers, reported once per distinct call site.
 *
 * The question this answers is which code owns the audio, and it has resisted
 * being answered from the crash: the stack walk out of a faulting copy produced
 * five frames and all five were inside dsound.dll, so the owner was never named.
 * Creation is the better place to ask. Our hook sits in the device's vtable
 * slot, so the address it returns to is the caller of CreateSoundBuffer itself -
 * not a frame guessed from a stack scan, but the actual owner, recorded at the
 * one moment it is unambiguous. */
/* Is this return address inside our own DLL?
 *
 * dsh_save reads every buffer's cursor through the same vtable slot the game
 * uses, so without this the loudest caller of GetCurrentPosition would be us,
 * and the census would report the observer instead of the subject. */
static HMODULE g_self_mod SS_PRESENT;

static HMODULE self_mod(void)
{
	if (!g_self_mod)
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				   (LPCWSTR)(void *)self_mod, &g_self_mod);
	return g_self_mod;
}

/* One frame further out than our immediate caller, or nothing.
 *
 * frame_up((void **)__builtin_frame_address(0)) does this in a line and the compiler warns that
 * it is unsafe, correctly: it dereferences a frame pointer that a frameless or
 * optimised caller may not have set, and a wrong guess reads whatever happens
 * to be at that address. The walk is the same either way, so the difference
 * that matters is the checking, and that can be made exact rather than hopeful.
 *
 * A frame pointer must lie inside this thread's stack, must point higher than
 * the frame below it, and must leave room for the saved pointer and the return
 * address. The TEB knows the stack's bounds precisely, so all three are cheap
 * to test and nothing here can fault. A chain that fails any of them yields
 * null and the log says "no module" instead of naming an address we invented. */
static void *frame_up(void **fp)
{
	NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
	char *lo = (char *)tib->StackLimit;
	char *hi = (char *)tib->StackBase;
	void **caller;

	if (!fp || (char *)fp < lo || (char *)fp + 2 * sizeof(void *) > hi)
		return NULL;
	caller = (void **)fp[0];
	if ((char *)caller <= (char *)fp ||
	    (char *)caller + 2 * sizeof(void *) > hi)
		return NULL;
	return caller[1];
}

static int from_us(void *ra)
{
	MEMORY_BASIC_INFORMATION mbi;

	if (!ra || !VirtualQuery(ra, &mbi, sizeof(mbi)))
		return 0;
	return (HMODULE)mbi.AllocationBase == self_mod();
}

#define SITE_CREATE 0
#define SITE_CURSOR 1

#define DSH_OWNERS 24
typedef struct Site {
	void *ra;
	void *up; /* one frame further out, where the interesting code usually is */
	int kind;
	int ours; /* remembered so our own frames are queried once, not per call */
	int told;
	unsigned long hits;
} Site;

static Site g_site[DSH_OWNERS];
static int g_nowner;

/* Filled by the Lock watch further down, reported from here.
 *
 * The ring copy at rabiribi.exe+0x36340 clamps with cmova, an unsigned minimum,
 * so it cannot manufacture a negative count - it can only pass through one that
 * was already sitting in the ring's available-bytes field. That field is filled
 * by Lock. Restoring the play cursor did not stop the negative copies, so the
 * assumption that the count is computed from a cursor difference at the moment
 * of the copy is wrong somewhere, and Lock is the link in the chain that has
 * never been watched. */
static unsigned long g_lock_calls, g_lock_bad;
static DWORD g_lock_worst_req, g_lock_worst_r1, g_lock_worst_r2, g_lock_worst_off;
static void *g_lock_worst_ra;

/* Filled by the cursor probe further down, reported from here. */
static void *g_probe_obj;
static DWORD g_probe_sw, g_probe_hw;
static unsigned long g_probe_calls, g_probe_sw_moves, g_probe_hw_moves;

/* Recorded now, reported later.
 *
 * Every buffer in this game is made during startup, and the savestate log is not
 * open yet - which is why the install message was invisible for the whole life
 * of this file and was blamed on a format specifier. Writing at the moment of
 * discovery loses the discovery. So the address is kept and printed from
 * dsh_save, which demonstrably reaches the log. */
static void note_owner(void *ra, void *up, int kind)
{
	int i;

	if (!ra)
		return;
	/* The scan comes first because GetCurrentPosition is on the game's audio
	 * path and is called constantly. A known address costs a few compares;
	 * only a genuinely new one pays for a VirtualQuery.
	 *
	 * Keyed on both frames. The immediate one turned out to be a forwarding
	 * accessor that every caller shares, so keying on it alone collapsed
	 * every distinct caller into a single entry. */
	for (i = 0; i < g_nowner; i++)
		if (g_site[i].ra == ra && g_site[i].up == up && g_site[i].kind == kind) {
			g_site[i].hits++;
			return;
		}
	if (g_nowner >= DSH_OWNERS)
		return;
	g_site[g_nowner].ra = ra;
	g_site[g_nowner].up = up;
	g_site[g_nowner].kind = kind;
	g_site[g_nowner].ours = from_us(ra);
	g_site[g_nowner].told = 0;
	g_site[g_nowner].hits = 1;
	g_nowner++;
}

/* The install happens at startup too, so it is deferred for the same reason. */
static void **g_pending_dev_vtbl;
static int g_told_install;
static int g_told_flags;

static void report_owners(void)
{
	char buf[MAX_PATH + 64];
	int i;

	if (g_pending_dev_vtbl && !g_told_install) {
		g_told_install = 1;
		ss_log("dsound: device vtable at %08lX, CreateSoundBuffer and "
		       "DuplicateSoundBuffer watched; buffer vtable at %08lX, release "
		       "watched. %d buffer(s) tracked so far\n",
		       (unsigned long)(ULONG_PTR)g_pending_dev_vtbl,
		       (unsigned long)(ULONG_PTR)g_buf_vtbl, g_nbuf);
	}
	/* The two flags that choose the cursor's source, read from the image
	 * rather than hard-coded as absolute addresses, so a different base does
	 * not silently report someone else's memory. */
	{
		char *base = (char *)GetModuleHandleA(NULL);

		/* Rebasing was the only thing this guarded against, and the comment
		 * above says so. It is not enough: both offsets are five megabytes
		 * into an image, and read out of anything smaller they land past the
		 * end of it. A 363 KB harness faulted here on the first save, reading
		 * base+0x50E18C exactly. The flags are DxLib's, so the check is
		 * whether this is the game, not whether the pointer happens to be
		 * mapped - a lookalike that reads cleanly would be worse. */
		if (base && !savestate_host_is("rabiribi.exe")) {
			if (!g_told_flags) {
				g_told_flags = 1;
				ss_log("dsound: not reading the cursor source flags - they "
				       "are rabiribi.exe offsets and this is a different "
				       "executable\n");
			}
			base = NULL;
		}
		if (base) {
			DWORD hi = *(DWORD *)(base + 0x50E18C);
			DWORD lo = *(DWORD *)(base + 0x52EF18);

			ss_log("dsound: cursor source flags - +0x50E18C = %lu (nonzero "
			       "means skip DirectSound), +0x52EF18 = %lu (nonzero makes "
			       "the low-level wrapper decline). The cursor is %s\n",
			       hi, lo,
			       (hi || lo) ? "DxLib's own counter, which rewinds with the game"
					  : "the hardware's, which does not rewind");
		}
	}
	if (g_lock_calls) {
		char who[MAX_PATH + 64];

		where(g_lock_worst_ra, who, sizeof(who));
		ss_log("dsound: %lu Lock call(s), %lu of them asking for a byte count with "
		       "the top bit set. Worst: offset %lu, requested %ld, granted %lu + "
		       "%lu, from %s\n",
		       g_lock_calls, g_lock_bad, g_lock_worst_off, (long)g_lock_worst_req,
		       g_lock_worst_r1, g_lock_worst_r2, who);
		if (!g_lock_bad)
			ss_log("dsound: no Lock ever asked for a negative size, so the ring's "
			       "available count is not coming from the request - it is "
			       "being computed after the Lock returns\n");
	}
	if (g_probe_calls)
		ss_log("dsound: over %lu read(s) of one stream, DxLib's own counter "
		       "changed %lu time(s) and the hardware cursor changed %lu time(s). "
		       "If the first is zero the software source is dead in this mode "
		       "and the flag cannot simply be flipped; if both move, the counter "
		       "is maintained and rewinds with the game\n",
		       g_probe_calls, g_probe_sw_moves, g_probe_hw_moves);
	for (i = 0; i < g_nowner; i++) {
		if (g_site[i].told || g_site[i].ours)
			continue;
		g_site[i].told = 1;
		where(g_site[i].ra, buf, sizeof(buf));
		if (g_site[i].kind == SITE_CREATE)
			ss_log("dsound: sound buffers are created by %s - this is the "
			       "owner, the code whose idea of the play cursor has to "
			       "survive a restore\n",
			       buf);
		else {
			char up[MAX_PATH + 64];

			where(g_site[i].up, up, sizeof(up));
			ss_log("dsound: the play cursor is read at %s, called from %s, %lu "
			       "time(s) so far - the second address is the one that holds "
			       "the other operand of the subtraction\n",
			       buf, up, g_site[i].hits);
		}
	}
}

/* Patch one slot of a vtable that lives in read-only data. */
static void *slot_swap(void **vtbl, int i, void *fn)
{
	DWORD old;
	void *prev;

	if (!VirtualProtect(&vtbl[i], sizeof(void *), PAGE_READWRITE, &old))
		return NULL;
	prev = vtbl[i];
	vtbl[i] = fn;
	VirtualProtect(&vtbl[i], sizeof(void *), old, &old);
	return prev;
}

static void track(void *p, void *owner)
{
	int i;

	if (!p)
		return;
	EnterCriticalSection(&g_cs);
	for (i = 0; i < g_nbuf; i++)
		if (g_buf[i].p == p)
			goto out;
	if (g_nbuf < DSH_MAX) {
		g_buf[g_nbuf].p = p;
		g_buf[g_nbuf].held = 0;
		g_buf[g_nbuf].owner = owner;
		g_nbuf++;
	} else if (!g_capped) {
		g_capped = 1;
		ss_log("dsound: hit the %d buffer ceiling - every buffer past this one "
		       "keeps a play cursor we cannot wind back, so restores will still "
		       "see negative lengths\n",
		       DSH_MAX);
	}
out:
	LeaveCriticalSection(&g_cs);
}

static void untrack(void *p)
{
	int i;

	EnterCriticalSection(&g_cs);
	for (i = 0; i < g_nbuf; i++)
		if (g_buf[i].p == p) {
			g_buf[i] = g_buf[--g_nbuf];
			break;
		}
	LeaveCriticalSection(&g_cs);
}

/* Buffers are learned from the one call that makes them, so the list is exact
 * rather than discovered by scanning. Release is watched for the same reason:
 * a freed buffer is a pointer we must never call again, and asking a dead COM
 * object for its status is its own crash. */
static ULONG WINAPI hook_release(void *self)
{
	ULONG n = g_real_release(self);

	if (!n)
		untrack(self);
	return n;
}

/* Does DxLib's own cursor move while DirectSound is the one being asked?
 *
 * Both callers of the accessor have a second way to answer the question: if a
 * global is set, or if the low-level wrapper reports "not applicable", they skip
 * the hardware and compute the position as [obj+0x54] * [obj+0x3C] - a sample
 * counter and a block align, both living in the game's own object and therefore
 * both rewinding with it. A cursor sourced from there could not produce a
 * negative difference, because both operands would travel together.
 *
 * Whether that is usable turns on one fact we cannot get by reading code: does
 * [obj+0x54] advance while the hardware path is the one in use? If it does, the
 * counter is maintained regardless and the software source is live. If it sits
 * at whatever it was initialised to, the mode is chosen once at startup and
 * flipping the flag mid-run would freeze every stream instead of fixing it.
 *
 * So watch it. The object is not passed to us - we are handed the COM buffer -
 * but the forwarder's frame holds it as its first argument, and the object can
 * be confirmed rather than assumed: obj+0xC8 is where it keeps the very buffer
 * pointer we were called on. If that matches, we have the right object. */
#define DXSND_BUFPTR 0xC8
#define DXSND_SAMPLEPOS 0x54
#define DXSND_BLOCKALIGN 0x3C

static int obj_readable(void *o)
{
	MEMORY_BASIC_INFORMATION mbi;

	if (!o || !VirtualQuery(o, &mbi, sizeof(mbi)))
		return 0;
	if (mbi.State != MEM_COMMIT)
		return 0;
	if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
		return 0;
	/* The whole object, not just its first byte. */
	return (char *)o + DXSND_BUFPTR + sizeof(void *) <=
	       (char *)mbi.BaseAddress + mbi.RegionSize;
}

static void probe_cursor(void **fp, void *self, DWORD play)
{
	void *obj;
	DWORD sw;

	if (!fp)
		return;
	obj = fp[2]; /* the forwarder's first argument */
	if (!obj_readable(obj))
		return;
	if (*(void **)((char *)obj + DXSND_BUFPTR) != self)
		return; /* not the object we thought; say nothing rather than guess */
	sw = *(DWORD *)((char *)obj + DXSND_SAMPLEPOS) *
	     (DWORD) * (WORD *)((char *)obj + DXSND_BLOCKALIGN);
	if (!g_probe_obj)
		g_probe_obj = obj;
	if (obj != g_probe_obj)
		return; /* one object, so the counts describe one stream */
	g_probe_calls++;
	if (sw != g_probe_sw) {
		g_probe_sw_moves++;
		g_probe_sw = sw;
	}
	if (play != g_probe_hw) {
		g_probe_hw_moves++;
		g_probe_hw = play;
	}
}

/* Who asks where the hardware is.
 *
 * The negative length is a subtraction: somebody reads the play cursor, compares
 * it against a position of their own, and copies the difference. The copier is
 * known - it is the CRT's memcpy, which tells us nothing about who computed the
 * count. This names the code doing the comparing, which is the code holding the
 * other operand, and that operand is the thing whose rewind produces a negative
 * result. Recorded per distinct call site rather than per call. */
static HRESULT WINAPI hook_getpos(void *self, DWORD *play, DWORD *write)
{
	/* Two frames, because the first one is a forwarder.
	 *
	 * rabiribi.exe+0x4E010 takes the object, reads the buffer pointer from
	 * +0xC8, calls this, and returns zero. Every caller in the game arrives
	 * through it, so the immediate return address is the same constant every
	 * time and says nothing about who wanted the cursor. The frame above it
	 * is the one doing arithmetic with the answer.
	 *
	 * Asking for frame 1 makes the compiler keep a frame pointer here, and
	 * the forwarder has an ordinary push ebp / mov ebp,esp prologue, so the
	 * chain is walkable. If it ever is not, this yields null and the log
	 * simply says no module rather than inventing an address. */
	void **fp = (void **)__builtin_frame_address(0);
	HRESULT hr;

	g_calls[C_GETPOS]++; /* see the note in hook_lock */
	note_owner(__builtin_return_address(0), frame_up(fp), SITE_CURSOR);
	hr = g_real_getpos(self, play, write);
	if (SUCCEEDED(hr) && play && fp[0])
		probe_cursor((void **)fp[0], self, *play);
	return hr;
}

/* What the game asks Lock for, and what it gets.
 *
 * Three numbers matter and none of them have ever been seen: the size asked
 * for, and the two region sizes handed back. If the request already has its top
 * bit set then the negative is born upstream, in the cursor arithmetic at
 * +0x33CE4, and the ring is only carrying it. If the request is sane and the
 * ring still ends up negative, then something between Lock returning and the
 * copy running is corrupting the count - which would point at the restore
 * rather than at the audio. Those two answers need different fixes, and this
 * tells them apart. */
static HRESULT WINAPI hook_lock(void *self, DWORD off, DWORD bytes, void **p1, DWORD *b1,
				 void **p2, DWORD *b2, DWORD flags)
{
	HRESULT hr = g_real_lock(self, off, bytes, p1, b1, p2, b2, flags);

	/* A failed Lock is fatal here, and not because of anything DirectSound
	 * does. DxLib does not check the HRESULT: DxSound.cpp locks a buffer into
	 * a stack-local pair of region pointers and then feeds decoded PCM through
	 * them regardless. When Lock fails those locals are never written, so the
	 * fill loop copies through whatever the stack happened to be holding.
	 *
	 * Disassembly of the crash we kept hitting says exactly that. rabiribi.exe
	 * +6E9F8 is a memcpy whose destination was 00BF2C14 - the .rdata string
	 * "..\..\..\..\..\Source\Library\Main\DxSound.cpp". That literal is pushed
	 * as an argument by thirty-one error-reporting call sites and stored
	 * nowhere, and the frame walk found a second copy of it lying in an
	 * unrelated frame on the same stack. It was residue, read as lpvAudioPtr1.
	 *
	 * So the failure is handled here or it is not handled at all. A lost
	 * buffer is what DirectSound asks the application to fix and DxLib never
	 * does, so restore it and try once more. If that does not work, hand back
	 * a scratch buffer: the game writes a frame of audio into a bin, the sound
	 * glitches, and the process lives. That is a lie, and a deliberate one -
	 * the alternative is a write through a string constant, and unlike the
	 * lies this project usually refuses, nothing downstream reads it back. */
	if (FAILED(hr)) {
		g_lock_fail++;
		if (hr == DSERR_BUFFERLOST) {
			void **v = *(void ***)self;

			((PFN_RESTORE)v[DSB_RESTORE])(self);
			hr = g_real_lock(self, off, bytes, p1, b1, p2, b2, flags);
			ss_log("dsound: Lock returned DSERR_BUFFERLOST for %lu bytes at "
			       "offset %lu; after Restore it %s\n",
			       (unsigned long)bytes, (unsigned long)off,
			       SUCCEEDED(hr) ? "succeeded" : "failed again");
		}
		if (FAILED(hr)) {
			if (!lock_sink())
				return hr;
			if (p1)
				*p1 = g_sink;
			if (b1)
				*b1 = bytes < SINK_BYTES ? bytes : SINK_BYTES;
			if (p2)
				*p2 = NULL;
			if (b2)
				*b2 = 0;
			ss_log("dsound: Lock FAILED (%08lX) for %lu bytes at offset %lu. "
			       "DxLib does not check, so it would have written through "
			       "an uninitialised stack slot - handed it a scratch "
			       "buffer instead and lost a frame of audio\n",
			       (unsigned long)hr, (unsigned long)bytes,
			       (unsigned long)off);
			return DS_OK;
		}
	}
	g_lock_calls++;
	/* Counted for the survey too. This slot was already hooked before the
	 * survey existed, so the survey's own counter never saw it and the
	 * inventory reported Lock as never called - next to 942 Unlocks. */
	g_calls[C_LOCK]++;
	/* Worth recording the largest request as well as any negative one: a
	 * request of two billion is the same bug wearing a different sign. */
	if ((bytes & 0x80000000u) || bytes > g_lock_worst_req) {
		if (bytes & 0x80000000u)
			g_lock_bad++;
		g_lock_worst_req = bytes;
		g_lock_worst_off = off;
		g_lock_worst_r1 = (SUCCEEDED(hr) && b1) ? *b1 : 0;
		g_lock_worst_r2 = (SUCCEEDED(hr) && b2) ? *b2 : 0;
		g_lock_worst_ra = frame_up((void **)__builtin_frame_address(0));
	}
	return hr;
}

/* The survey.
 *
 * We are considering replacing DirectSound outright with our own mixer, so that
 * the play cursor becomes a number we own and advance rather than a hardware
 * fact that keeps moving while every thread is suspended. That is only a
 * sensible amount of work if we implement the part DxLib actually uses, and
 * nobody knows what that is - DirectSound has far more surface than any one
 * caller touches.
 *
 * So this counts. Every method is hooked to increment and call through, buffer
 * formats are recorded where they are created, and QueryInterface records what
 * else the game reaches for. Nothing here changes behaviour; it only answers
 * "what would we have to build". */
static const char *const g_cname[C_MAX] = {
	"QueryInterface", "GetCurrentPosition", "GetFormat", "GetVolume",
	"GetStatus",	  "Lock",		"Unlock",    "Play",
	"Stop",		  "SetCurrentPosition", "SetVolume", "SetPan",
	"SetFrequency"
};

/* One row per distinct wave format, because "what formats does it use" decides
 * how much of a resampler we would need. */
typedef struct Fmt {
	DWORD rate;
	WORD ch, bits;
	unsigned long n;
} Fmt;

static Fmt g_fmt[16];
static int g_nfmt;
static DWORD g_flags_seen;
static DWORD g_bytes_min = 0xFFFFFFFFu, g_bytes_max;
static unsigned long g_ncreate, g_ndup;
static GUID g_iid[8];
static int g_niid;

static void note_format(const void *desc)
{
	/* DSBUFFERDESC: size, flags, bufferBytes, reserved, then the format. */
	const DWORD *d = (const DWORD *)desc;
	const WORD *w;
	DWORD rate;
	WORD ch, bits;
	int i;

	if (!desc)
		return;
	g_flags_seen |= d[1];
	if (d[2] < g_bytes_min)
		g_bytes_min = d[2];
	if (d[2] > g_bytes_max)
		g_bytes_max = d[2];
	w = (const WORD *)(uintptr_t)d[4];
	if (!w)
		return; /* a primary buffer has no format here */
	ch = w[1];
	rate = *(const DWORD *)(w + 2);
	bits = w[7];
	for (i = 0; i < g_nfmt; i++)
		if (g_fmt[i].rate == rate && g_fmt[i].ch == ch && g_fmt[i].bits == bits) {
			g_fmt[i].n++;
			return;
		}
	if (g_nfmt < 16) {
		g_fmt[g_nfmt].rate = rate;
		g_fmt[g_nfmt].ch = ch;
		g_fmt[g_nfmt].bits = bits;
		g_fmt[g_nfmt].n = 1;
		g_nfmt++;
	}
}

static PFN_GETSTATUS g_real_getstatus;
static PFN_PLAY g_real_play;
static PFN_SETPOS g_real_setpos;
static PFN_STOP g_real_stop;
typedef HRESULT(WINAPI *PFN_QI)(void *, const GUID *, void **);
typedef HRESULT(WINAPI *PFN_GETFMT)(void *, void *, DWORD, DWORD *);
typedef HRESULT(WINAPI *PFN_GETVOL)(void *, LONG *);
typedef HRESULT(WINAPI *PFN_SETLONG)(void *, LONG);
typedef HRESULT(WINAPI *PFN_SETDWORD)(void *, DWORD);
typedef HRESULT(WINAPI *PFN_UNLOCK)(void *, void *, DWORD, void *, DWORD);
static PFN_QI g_real_qi;
static PFN_GETFMT g_real_getfmt;
static PFN_GETVOL g_real_getvol;
static PFN_SETLONG g_real_setvol, g_real_setpan;
static PFN_SETDWORD g_real_setfreq;
static PFN_UNLOCK g_real_unlock;

static HRESULT WINAPI hook_qi(void *self, const GUID *iid, void **out)
{
	int i;

	g_calls[C_QI]++;
	if (iid) {
		for (i = 0; i < g_niid; i++)
			if (!memcmp(&g_iid[i], iid, sizeof(GUID)))
				goto done;
		if (g_niid < 8)
			g_iid[g_niid++] = *iid;
	}
done:
	return g_real_qi(self, iid, out);
}

static HRESULT WINAPI hook_getfmt(void *self, void *f, DWORD n, DWORD *got)
{
	g_calls[C_GETFMT]++;
	return g_real_getfmt(self, f, n, got);
}

static HRESULT WINAPI hook_getvol(void *self, LONG *v)
{
	g_calls[C_GETVOL]++;
	return g_real_getvol(self, v);
}

static HRESULT WINAPI hook_getstatus(void *self, DWORD *s)
{
	g_calls[C_GETSTAT]++;
	return g_real_getstatus(self, s);
}

static HRESULT WINAPI hook_unlock(void *self, void *p1, DWORD b1, void *p2, DWORD b2)
{
	g_calls[C_UNLOCK]++;
	/* The scratch buffer never belonged to this device, so unlocking it would
	 * be asking DirectSound to account for memory it never handed out. */
	if (p1 && p1 == g_sink)
		return DS_OK;
	return g_real_unlock(self, p1, b1, p2, b2);
}

static HRESULT WINAPI hook_play(void *self, DWORD r1, DWORD pri, DWORD flags)
{
	g_calls[C_PLAY]++;
	return g_real_play(self, r1, pri, flags);
}

static HRESULT WINAPI hook_stop(void *self)
{
	g_calls[C_STOP]++;
	return g_real_stop(self);
}

static HRESULT WINAPI hook_setpos(void *self, DWORD p)
{
	g_calls[C_SETPOS]++;
	return g_real_setpos(self, p);
}

static HRESULT WINAPI hook_setvol(void *self, LONG v)
{
	g_calls[C_SETVOL]++;
	return g_real_setvol(self, v);
}

static HRESULT WINAPI hook_setpan(void *self, LONG v)
{
	g_calls[C_SETPAN]++;
	return g_real_setpan(self, v);
}

static HRESULT WINAPI hook_setfreq(void *self, DWORD v)
{
	g_calls[C_SETFREQ]++;
	return g_real_setfreq(self, v);
}

static void learn_buffer_vtbl(void *b)
{
	if (g_buf_vtbl || !b)
		return;
	g_buf_vtbl = *(void ***)b;
	g_real_release = (PFN_RELEASE)slot_swap(g_buf_vtbl, DSB_RELEASE, (void *)hook_release);
	g_real_getpos =
		(PFN_GETPOS)slot_swap(g_buf_vtbl, DSB_GET_POSITION, (void *)hook_getpos);
	g_real_lock = (PFN_LOCK)slot_swap(g_buf_vtbl, DSB_LOCK, (void *)hook_lock);
	ss_hook_note("vtable", "IDirectSoundBuffer::Release", g_buf_vtbl[DSB_RELEASE], 1);
	ss_hook_note("vtable", "IDirectSoundBuffer::GetCurrentPosition",
		     g_buf_vtbl[DSB_GET_POSITION], 1);
	ss_hook_note("vtable", "IDirectSoundBuffer::Lock", g_buf_vtbl[DSB_LOCK], 1);
	/* Counting only. Each of these increments and calls through, so the survey
	 * costs an add per call and changes nothing the game can observe. */
	g_real_qi = (PFN_QI)slot_swap(g_buf_vtbl, DSB_QI, (void *)hook_qi);
	g_real_getfmt = (PFN_GETFMT)slot_swap(g_buf_vtbl, DSB_GET_FORMAT, (void *)hook_getfmt);
	g_real_getvol = (PFN_GETVOL)slot_swap(g_buf_vtbl, DSB_GET_VOLUME, (void *)hook_getvol);
	g_real_getstatus =
		(PFN_GETSTATUS)slot_swap(g_buf_vtbl, DSB_GET_STATUS, (void *)hook_getstatus);
	g_real_unlock = (PFN_UNLOCK)slot_swap(g_buf_vtbl, DSB_UNLOCK, (void *)hook_unlock);
	g_real_play = (PFN_PLAY)slot_swap(g_buf_vtbl, DSB_PLAY, (void *)hook_play);
	g_real_stop = (PFN_STOP)slot_swap(g_buf_vtbl, DSB_STOP, (void *)hook_stop);
	g_real_setpos = (PFN_SETPOS)slot_swap(g_buf_vtbl, DSB_SET_POSITION, (void *)hook_setpos);
	g_real_setvol = (PFN_SETLONG)slot_swap(g_buf_vtbl, DSB_SET_VOLUME, (void *)hook_setvol);
	g_real_setpan = (PFN_SETLONG)slot_swap(g_buf_vtbl, DSB_SET_PAN, (void *)hook_setpan);
	g_real_setfreq =
		(PFN_SETDWORD)slot_swap(g_buf_vtbl, DSB_SET_FREQ, (void *)hook_setfreq);
}

/* What we would have to build. */
void dsh_survey(void)
{
	int i;

	if (!g_ready) {
		ss_log("dsound survey: the hook never armed, so nothing was counted\n");
		return;
	}
	ss_log("dsound survey: %lu buffer(s) created, %lu duplicated, flags seen %08lX, "
	       "buffer bytes %lu to %lu\n",
	       g_ncreate, g_ndup, (unsigned long)g_flags_seen,
	       (unsigned long)(g_bytes_min == 0xFFFFFFFFu ? 0 : g_bytes_min),
	       (unsigned long)g_bytes_max);
	for (i = 0; i < g_nfmt; i++)
		ss_log("  format: %lu Hz, %u bit, %u channel(s) - %lu buffer(s)\n",
		       (unsigned long)g_fmt[i].rate, (unsigned)g_fmt[i].bits,
		       (unsigned)g_fmt[i].ch, g_fmt[i].n);
	for (i = 0; i < C_MAX; i++)
		if (g_calls[i])
			ss_log("  %-20s %lu call(s)\n", g_cname[i], g_calls[i]);
	/* A method with no calls is the useful half of this: it is surface we
	 * would not have to implement. */
	for (i = 0; i < C_MAX; i++)
		if (!g_calls[i])
			ss_log("  %-20s never called\n", g_cname[i]);
	for (i = 0; i < g_niid; i++)
		ss_log("  QueryInterface asked for {%08lX-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X}\n",
		       (unsigned long)g_iid[i].Data1, g_iid[i].Data2, g_iid[i].Data3,
		       g_iid[i].Data4[0], g_iid[i].Data4[1], g_iid[i].Data4[2],
		       g_iid[i].Data4[3], g_iid[i].Data4[4], g_iid[i].Data4[5],
		       g_iid[i].Data4[6], g_iid[i].Data4[7]);
}

static HRESULT WINAPI hook_create(void *self, const void *desc, void **out, void *unk)
{
	void *ra = __builtin_return_address(0);
	HRESULT hr = g_real_create(self, desc, out, unk);

	if (SUCCEEDED(hr) && out && *out) {
		learn_buffer_vtbl(*out);
		track(*out, ra);
		note_owner(ra, frame_up((void **)__builtin_frame_address(0)), SITE_CREATE);
		g_ncreate++;
		note_format(desc);
	}
	return hr;
}

static HRESULT WINAPI hook_dup(void *self, void *src, void **out)
{
	void *ra = __builtin_return_address(0);
	HRESULT hr = g_real_dup(self, src, out);

	if (SUCCEEDED(hr) && out && *out) {
		learn_buffer_vtbl(*out);
		track(*out, ra);
		note_owner(ra, frame_up((void **)__builtin_frame_address(0)), SITE_CREATE);
		g_ndup++;
	}
	return hr;
}

/* Learn the device vtable from a device of our own.
 *
 * Nothing about the throwaway is used except its type. It is created with the
 * default device and no cooperative level, which is enough to allocate the
 * object and therefore enough to read the pointer, and it is released
 * immediately. The game's device - created before or after this, by whatever
 * route - shares the table. */
/* Ours if it sits in the same directory as this DLL. Comparing paths rather
 * than looking for an export is what distinguishes the trampoline from
 * System32's copy even before anything has called into it. */
static int dsh_stub_is_ours(HMODULE ds)
{
	char theirs[MAX_PATH], mine[MAX_PATH];
	HMODULE self = NULL;
	char *a, *b;

	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCSTR)(void *)dsh_stub_is_ours, &self) ||
	    !self)
		return 0;
	if (!GetModuleFileNameA(ds, theirs, MAX_PATH) ||
	    !GetModuleFileNameA(self, mine, MAX_PATH))
		return 0;
	a = strrchr(theirs, '\\');
	b = strrchr(mine, '\\');
	if (!a || !b)
		return 0;
	*a = 0;
	*b = 0;
	return lstrcmpiA(theirs, mine) == 0;
}

void dsh_install(void)
{
	HMODULE ds;
	PFN_DSCREATE8 create8;
	void *dev = NULL;
	void **vtbl;

	if (g_armed)
		return;
	ds = GetModuleHandleA("dsound.dll");
	if (!ds)
		return; /* not loaded yet; called again next frame */
	{
		/* Which file won the name, in the log the restores are in. The attach
		 * message that decides this is written before the savestate log exists,
		 * so without this the answer only lives in the other log. */
		char path[MAX_PATH];

		if (!GetModuleFileNameA(ds, path, MAX_PATH))
			lstrcpynA(path, "(no path)", MAX_PATH);
		ss_log("dsound: the name resolved to %s\n", path);
	}
	/* Standing down on the patch alone was wrong, and it cost a run. Taking
	 * DirectSoundCreate8 is not the same as being asked for a device: DxLib
	 * reaches DirectSound some other way - CLSID_DirectSound is in the
	 * executable, and a CoCreateInstance never touches the export - so the
	 * entry point sat patched and unused while these hooks, the only thing
	 * actually holding that audio stack together across a restore, were
	 * switched off. The test has to be that ds_sw served a device, not that it
	 * is ready to. */
	if (dsh_stub_is_ours(ds) || ds_sw_armed()) {
		/* The software DirectSound in ds_sw.c answered the game instead of
		 * Windows'. Everything below this point exists to make somebody else's
		 * audio stack survive a rewind, and there is no longer somebody else:
		 * no vtable of theirs to patch, no hardware cursor to wind back, no
		 * mixer thread reading while we copy. Leaving it armed would have us
		 * hooking our own methods to correct for a card that is not there. */
		g_armed = 1;
		ss_log("dsound: the same-folder stub answered, so the Windows hooks "
		       "stand down - there is no foreign audio stack left to correct "
		       "for\n");
		return;
	}
	create8 = (PFN_DSCREATE8)(void *)GetProcAddress(ds, "DirectSoundCreate8");
	if (!create8) {
		g_armed = 1; /* nothing to hook, and no point retrying every frame */
		return;
	}
	if (FAILED(create8(NULL, &dev, NULL)) || !dev) {
		g_armed = 1;
		ss_log("dsound: could not create a probe device, so the play cursor stays "
		       "the sound card's. Expect negative-length copies after a restore\n");
		return;
	}
	InitializeCriticalSection(&g_cs);
	savestate_own_cs(&g_cs);
	vtbl = *(void ***)dev;
	g_real_create = (PFN_CREATEBUF)slot_swap(vtbl, DS_CREATE_BUFFER, (void *)hook_create);
	g_real_dup = (PFN_DUP)slot_swap(vtbl, DS_DUPLICATE, (void *)hook_dup);
	ss_hook_note("vtable", "IDirectSound::CreateSoundBuffer", vtbl[DS_CREATE_BUFFER], 1);
	ss_hook_note("vtable", "IDirectSound::DuplicateSoundBuffer", vtbl[DS_DUPLICATE], 1);
	((PFN_RELEASE)(*(void ***)dev)[2])(dev);
	g_armed = 1;
	g_ready = g_real_create != NULL;
	g_pending_dev_vtbl = vtbl;
}

/* Note what is audible right now, before anything is wound back.
 *
 * Called at the top of a restore, while the present is still the present. The
 * allocation happens here rather than at install because this is the first
 * moment it is needed, and it is excluded so that the note survives the rewind
 * it describes. */
void dsh_mark_present(void)
{
	int i;

	if (!g_ready)
		return;
	if (!g_present) {
		g_present = (PresentSet *)savestate_kept(SS_KEPT_DS_PRESENT,
							 sizeof(PresentSet));
		if (!g_present)
			return;
	}
	g_present->n = 0;
	EnterCriticalSection(&g_cs);
	for (i = 0; i < g_nbuf && g_present->n < DSH_MAX; i++) {
		void **v = *(void ***)g_buf[i].p;
		DWORD st = 0;

		if (FAILED(((PFN_GETSTATUS)v[DSB_GET_STATUS])(g_buf[i].p, &st)))
			continue;
		if (st & DSBSTATUS_PLAYING)
			g_present->p[g_present->n++] = g_buf[i].p;
	}
	LeaveCriticalSection(&g_cs);
	ss_log("dsound: %d buffer(s) were audible in the present when the restore "
	       "began; those come back whatever the snapshot says\n",
	       g_present->n);
}

/* Where every buffer is, at the instant the rest of the state is captured. */
void dsh_save(void)
{
	int i, n = 0;

	if (!g_ready)
		return;
	report_owners();
	EnterCriticalSection(&g_cs);
	for (i = 0; i < g_nbuf; i++) {
		void **v = *(void ***)g_buf[i].p;
		DWORD st = 0;

		g_buf[i].held = 0;
		/* The real method, not the slot: the slot is our own hook now, and
		 * routing the snapshot through it would file us as a caller. */
		if (FAILED((g_real_getpos ? g_real_getpos
					  : (PFN_GETPOS)v[DSB_GET_POSITION])(
			    g_buf[i].p, &g_buf[i].play, &g_buf[i].write)) ||
		    FAILED(((PFN_GETSTATUS)v[DSB_GET_STATUS])(g_buf[i].p, &st))) {
			/* A buffer that will not say where it is cannot be wound
			 * back, so it keeps a cursor in the present while its
			 * owner's write position goes back - exactly the state
			 * that produces a negative length. Name the owner: these
			 * are the buffers that matter. */
			char who[MAX_PATH + 64];

			where(g_buf[i].owner, who, sizeof(who));
			ss_log("dsound: buffer %08lX would not report its position, so it "
			       "cannot be wound back. Created by %s\n",
			       (unsigned long)(ULONG_PTR)g_buf[i].p, who);
			continue;
		}
		g_buf[i].status = st;
		g_buf[i].held = 1;
		n++;
	}
	LeaveCriticalSection(&g_cs);
	ss_log("dsound: %d of %d buffer(s) had their play position recorded\n", n, g_nbuf);
}

/* Silence for the duration of the copy.
 *
 * A save reported "14F3E088 CHANGED while we copied it", and in the same breath
 * that every thread in the process was suspended. So the writer was not a
 * thread. Suspending user-mode code does not stop a sound card: the mixer feeds
 * from a mapped buffer on its own schedule, and a driver completing a transfer
 * writes whether or not anything in this process is runnable. Deciding which
 * memory belongs to whom has no answer to that - the memory is the game's by any
 * definition and it still changes underneath us.
 *
 * What can be done is to remove the writer. A stopped buffer is not being read
 * by the card and not being filled by the driver, so for as long as everything
 * is stopped the copy has no competition. That is a smaller claim than owning
 * the audio stack and it is one we can actually make good on.
 *
 * The cost is honest and small: audio stops for the length of the snapshot. It
 * was going to glitch there anyway. */
void dsh_quiet(void)
{
	int i, n = 0;

	if (!g_ready)
		return;
	EnterCriticalSection(&g_cs);
	for (i = 0; i < g_nbuf; i++) {
		void **v = *(void ***)g_buf[i].p;

		((PFN_STOP)v[DSB_STOP])(g_buf[i].p);
		n++;
	}
	LeaveCriticalSection(&g_cs);
	ss_log("dsound: %d buffer(s) stopped, so the card is not reading or filling "
	       "anything while we copy memory\n",
	       n);
}

/* Whether cursors go back at all.
 *
 * In one place because dsh_seek and dsh_play both act on the answer, and them
 * disagreeing about it was a real defect rather than a stale comment. dsh_seek
 * declined to wind cursors back, and said so in the log, while dsh_play wound
 * them back anyway for every buffer the snapshot knew was playing - which is
 * exactly the case dsh_seek's reasoning is about, not an exception to it. The
 * result was the failure that reasoning exists to prevent, on the buffers most
 * likely to matter: a cursor in the past against ring contents in the present. */
static int dsh_seek_on(void)
{
	char v[8];
	unsigned got = savestate_getenv("D3D9SW_DSSEEK", v, sizeof(v));

	return got > 0 && got < sizeof(v) && v[0] == '1';
}

/* Start again from where each buffer was when it was stopped.
 *
 * Only the ones that were playing at the snapshot: starting a buffer that was
 * idle would produce sound the game never asked for. */
void dsh_play(void)
{
	int i, n = 0, rescued = 0, seek = dsh_seek_on();

	if (!g_ready)
		return;
	EnterCriticalSection(&g_cs);
	for (i = 0; i < g_nbuf; i++) {
		void **v;
		int snap = g_buf[i].held && (g_buf[i].status & DSBSTATUS_PLAYING);
		int now = present_has(g_buf[i].p);

		if (!snap && !now)
			continue;
		v = *(void ***)g_buf[i].p;
		/* Only when cursors are going back at all, and then only for
		 * buffers the snapshot knows about. One the present alone vouches
		 * for has no saved position worth having, and writing a stale
		 * figure into it would be inventing a position rather than keeping
		 * one. With seeking off - which is the default, because
		 * DirectSound's objects stay in the present - the buffer plays on
		 * from wherever it actually is, which is the only place the ring
		 * agrees with. */
		if (seek && snap)
			((PFN_SETPOS)v[DSB_SET_POSITION])(g_buf[i].p, g_buf[i].play);
		((PFN_PLAY)v[DSB_PLAY])(g_buf[i].p, 0, 0, DSBPLAY_LOOPING);
		n++;
		if (now && !snap)
			rescued++;
	}
	if (g_present)
		g_present->n = 0;
	LeaveCriticalSection(&g_cs);
	ss_log("dsound: %d buffer(s) playing again from %s%s\n", n,
	       seek ? "their saved position" : "where they actually are",
	       rescued ? " (including ones the snapshot thought were silent)" : "");
}

/* Move every cursor back, without starting anything.
 *
 * Split out of dsh_restore because the two halves want opposite sides of
 * resume_all and lumping them together got both wrong. Doing the whole thing
 * after the threads restart means stopping, seeking and starting 700 buffers
 * while the game is already reading those same cursors - a race we caused.
 * Doing the whole thing before means calling Play into dsound with every thread
 * suspended, which risks waiting on a lock a suspended thread holds.
 *
 * Seeking has neither problem. The buffers are already stopped, because
 * dsh_quiet stopped them at the top of the restore, so there is no mixer to
 * race and no work for dsound to do beyond writing a field. And once it is
 * done, a buffer that is asked where it is - by a thread that has only just
 * resumed - answers with the position the game expects rather than one from
 * several seconds in its future. That is the whole point, and it has to be
 * true from the first instruction the game executes, not from whenever we get
 * around to it. */
void dsh_seek(void)
{
	int i, moved = 0;

	if (!g_ready)
		return;
	/* Only coherent while DirectSound is going back with us, and it no longer
	 * is.
	 *
	 * The restore now leaves DirectSound's own objects in the present - close
	 * to eight thousand of them, identified by the dsound.dll vtables at their
	 * heads - because rewinding them was killing the process outright. That was
	 * the right call and it makes this one wrong: winding a cursor back to a
	 * saved position, on an object whose every other field is present-time,
	 * points the mixer at a place in the ring that does not hold what the
	 * position implies. Which is white noise, and is what appeared in the same
	 * session the object veto did.
	 *
	 * Either DirectSound goes back entirely or it stays entirely, and it cannot
	 * go back, because its threads never stopped. So it stays, cursors
	 * included. D3D9SW_DSSEEK=1 restores the old behaviour for comparison, and
	 * dsh_play reads the same answer so that it cannot quietly contradict this
	 * one for the buffers the snapshot happens to know about. */
	if (!dsh_seek_on()) {
		ss_log("dsound: cursors left where they are, in dsh_play as well as "
		       "here - DirectSound's objects stay in the present now, so "
		       "winding only their play positions back would put the mixer "
		       "somewhere the ring does not agree with\n");
		return;
	}
	EnterCriticalSection(&g_cs);
	for (i = 0; i < g_nbuf; i++) {
		void **v;

		if (!g_buf[i].held)
			continue;
		v = *(void ***)g_buf[i].p;
		((PFN_STOP)v[DSB_STOP])(g_buf[i].p);
		if (SUCCEEDED(((PFN_SETPOS)v[DSB_SET_POSITION])(g_buf[i].p, g_buf[i].play)))
			moved++;
	}
	LeaveCriticalSection(&g_cs);
	ss_log("dsound: %d buffer(s) wound back to their saved play position before the "
	       "threads restart, so the first cursor the game reads is the one it "
	       "expects\n",
	       moved);
}

/* Put the hardware back where the game believes it is.
 *
 * Stop before moving: SetCurrentPosition on a playing buffer is documented to
 * be honoured but races the mixer, and the whole point here is to remove a race
 * rather than to relocate it. A buffer that was playing is started again with
 * the looping flag it almost certainly had, because a streaming buffer that
 * stops looping goes silent at the end of its first pass and never recovers. */
void dsh_restore(void)
{
	int i, moved = 0;

	if (!g_ready)
		return;
	EnterCriticalSection(&g_cs);
	for (i = 0; i < g_nbuf; i++) {
		void **v;

		if (!g_buf[i].held)
			continue;
		v = *(void ***)g_buf[i].p;
		((PFN_STOP)v[DSB_STOP])(g_buf[i].p);
		if (FAILED(((PFN_SETPOS)v[DSB_SET_POSITION])(g_buf[i].p, g_buf[i].play)))
			continue;
		if (g_buf[i].status & DSBSTATUS_PLAYING)
			((PFN_PLAY)v[DSB_PLAY])(g_buf[i].p, 0, 0, DSBPLAY_LOOPING);
		moved++;
	}
	LeaveCriticalSection(&g_cs);
	ss_log("dsound: %d buffer(s) wound back to their saved play position, so the "
	       "game's write cursor is ahead of the hardware's again\n",
	       moved);
}



