/* A software XAudio2.
 *
 * The DirectSound route is closed and we know why. Steam has
 * C:\WINDOWS\System32\dsound.dll in the process before our DLL attaches, so a
 * same-folder stub is never asked for the name; and patching
 * DirectSoundCreate8 in the loaded module changed nothing either, because a
 * completed save and load afterwards produced no ds_sw report at all, which
 * means DxLib never called the export. CLSID_DirectSound is in the executable,
 * and a CoCreateInstance goes through the registry to an absolute path and
 * touches neither the name nor the export. Both of our levers miss it by
 * construction.
 *
 * xaudio2_9.dll has neither problem. Steam does not preload it, so the game
 * folder wins the search - measured, not assumed: the load probe attached as
 * ...\Rabi-Ribi\xaudio2_9.DLL and DxLib called straight into it with
 * XAudio2Create(flags=0, processor=0xFFFFFFFF).
 *
 * It is also the easier thing to stand in for, and for a reason that goes to
 * the heart of the bug. DirectSound is a ring with a hardware play cursor, and
 * every failure this project has chased came from that cursor and the game's
 * write position being two clocks that rewind differently. XAudio2 pushes:
 * the game hands us whole buffers and asks how many samples have played. There
 * is no ring to wrap, no write cursor to keep ahead of a play cursor, and no
 * unsigned subtraction that can come out negative. The state is a queue we own
 * and a sample count we advance.
 *
 * There is no audio thread here, deliberately. Real XAudio2 fires
 * OnBufferEnd from its own mixer thread, which is exactly the kind of second
 * party we are removing - something that runs while every game thread is
 * suspended and holds pointers into memory we are about to rewind. Instead the
 * clock advances lazily, inside whatever call the game itself makes, so every
 * callback runs on the game's thread. Nothing touches audio state except the
 * game, which is the process shape -nosound has.
 *
 * Phase one is silent. Nothing is mixed and nothing reaches the speakers; the
 * PCM the game submits is never even read. The question this answers is
 * whether the game survives a restore with its audio entirely inside memory we
 * own, and putting a mixer thread in before that is answered would reintroduce
 * the writer we are trying to prove is gone.
 *
 * Execute-at-0 after CreateMasteringVoice is not a missing vtable slot here.
 * DxLib delay-loads X3DAudioInitialize from the same xaudio2_9.dll that
 * answered XAudio2Create; those extra names are exported from xa2_fwd.c. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmreg.h>
#include <mmsystem.h>
#include <stdarg.h>
#include <stddef.h>
#include "savestate.h"

void savestate_log_line(const char *s);
/* Drop this region from the SWEXCLUDE held set: its ORIGIN is this DLL, so the
 * Nt hook records it as renderer memory, but audio chunks must rewind with the
 * game or the PCM stream seams. No-op unless SWEXCLUDE armed the hook. */
void gameheap_va_unhold(void *base);

static void ss_log(const char *fmt, ...)
{
	char b[512];
	va_list ap;

	va_start(ap, fmt);
	wvsprintfA(b, fmt, ap);
	va_end(ap);
	savestate_log_line(b);
}

#define XA2_LOOP_INFINITE 255
#define XA2_END_OF_STREAM 0x0040
#define XA2_MAX_QUEUED 64
#define XA2_MAX_VOICES 1024

typedef struct {
	UINT32 Flags;
	UINT32 AudioBytes;
	const BYTE *pAudioData;
	UINT32 PlayBegin;
	UINT32 PlayLength;
	UINT32 LoopBegin;
	UINT32 LoopLength;
	UINT32 LoopCount;
	void *pContext;
} XA2_BUFFER;

typedef struct {
	void *pCurrentBufferContext;
	UINT32 BuffersQueued;
	UINT64 SamplesPlayed;
} XA2_VOICE_STATE;

typedef struct {
	UINT32 CreationFlags;
	UINT32 ActiveFlags;
	UINT32 InputChannels;
	UINT32 InputSampleRate;
} XA2_VOICE_DETAILS;

/* IXAudio2VoiceCallback: seven stdcall methods, no IUnknown at the front. */
typedef struct XA2Callback XA2Callback;
struct XA2Callback {
	void(WINAPI *const *vtbl)(void);
};
enum {
	CB_PASS_START = 0,
	CB_PASS_END,
	CB_STREAM_END,
	CB_BUFFER_START,
	CB_BUFFER_END,
	CB_LOOP_END,
	CB_VOICE_ERROR
};

typedef void(WINAPI *PFN_CB_VOID)(void *);
typedef void(WINAPI *PFN_CB_CTX)(void *, void *);

/* ---------------------------------------------------------------- state */

typedef struct SwVoice SwVoice;

struct SwVoice {
	const void **vtbl; /* first: this is a COM-shaped object */
	int kind;	   /* 0 source, 1 mastering, 2 submix */
	int alive;
	int epoch;	   /* the restore generation this voice was born in */
	XA2Callback *cb;
	UINT32 channels, rate, bits, block;
	float freq_ratio, volume;
	double rate_eff; /* samples per second after the frequency ratio */
	int started;
	int bstart;	 /* OnBufferStart already raised for the head buffer */
	int pass_q;	 /* a processing-pass request is queued and not yet delivered */
	LONGLONG anchor; /* rewound QPC at which `played` and `pos` were current */
	UINT64 played;	 /* samples, the number the game asks for */
	UINT32 pos;	 /* sample index into the head buffer, absolute like PlayBegin */
	UINT32 frac;	 /* 16.16 remainder of pos, for rate conversion */
	int head, n;
	/* Panning arrives as an output matrix rather than a pan value, so it is
	 * kept in the shape it was given and folded in at mix time. */
	int mtx_src, mtx_dst;
	float mtx[8];
	int chvol_n;
	float chvol[8];
	XA2_BUFFER q[XA2_MAX_QUEUED];
	/* pcm_sum of each queued buffer's data as submitted; 0 marks one whose
	 * bytes a restore found changed, which then plays as silence. */
	UINT32 qsum[XA2_MAX_QUEUED];
};

/* See submit_audit. */
static int g_sub_audit, g_sub_ok, g_sub_held, g_sub_outside, g_sub_said;
static UINT_PTR g_sub_seen[16];

/* Whether [p, p+n) is committed and readable, so a restored buffer pointer can
 * be looked at without faulting. */
static int pcm_readable(const BYTE *p, UINT32 n)
{
	const BYTE *end = p + n;

	while (p < end) {
		MEMORY_BASIC_INFORMATION mbi;
		const BYTE *top;

		if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
		    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
		    !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
				     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
			return 0;
		top = (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
		p = top;
	}
	return 1;
}

/* FNV-1a over the first and last 256 bytes and the length. Never 0. */
static UINT32 pcm_sum(const BYTE *p, UINT32 n)
{
	UINT32 h = 2166136261u ^ n, i, k = n < 256 ? n : 256;

	if (!p || !n)
		return 1;
	for (i = 0; i < k; i++)
		h = (h ^ p[i]) * 16777619u;
	for (i = n - k; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return h ? h : 1;
}

typedef struct {
	const void **vtbl;
	LONG ref;
} SwEngine;

enum {
	M_CREATE_SOURCE = 0,
	M_CREATE_MASTER,
	M_CREATE_SUBMIX,
	M_START,
	M_STOP,
	M_SUBMIT,
	M_FLUSH,
	M_GETSTATE,
	M_FREQRATIO,
	M_VOLUME,
	M_CHANVOL,
	M_OUTMATRIX,
	M_DESTROY,
	M_DISCONT,
	M_EXITLOOP,
	M_DETAILS,
	M_MAX
};
static const char *const g_mname[M_MAX] = {
	"CreateSourceVoice", "CreateMasteringVoice", "CreateSubmixVoice",
	"Start",	     "Stop",		    "SubmitSourceBuffer",
	"FlushSourceBuffers", "GetState",	    "SetFrequencyRatio",
	"SetVolume",	     "SetChannelVolumes",   "SetOutputMatrix",
	"DestroyVoice",	     "Discontinuity",	    "ExitLoop",
	"GetVoiceDetails"
};
static unsigned long g_calls[M_MAX];

/* The last few calls, in memory, written out only if something faults.
 *
 * The savestate log is no help here: the engine that owns it had not booted
 * when the game died at one second in, so every ss_log from this file went
 * nowhere. A file write per call would answer that but costs a syscall on a
 * path taken thousands of times a second. A ring costs a memcpy and is only
 * read when there is a fault to explain, which is the right trade for a hook
 * that is supposed to be invisible when it works. */
#define XA2_RING 24
static char g_ring[XA2_RING][112];
static unsigned g_ring_n;

static void tr(const char *fmt, ...)
{
	va_list ap;
	unsigned slot = g_ring_n++ % XA2_RING;

	va_start(ap, fmt);
	wvsprintfA(g_ring[slot], fmt, ap);
	va_end(ap);
}

void xa2_sw_trace_dump(void (*emit)(const char *))
{
	unsigned i, first;

	if (!emit || !g_ring_n)
		return;
	emit("xa2_sw: the calls leading up to this, oldest first:");
	first = g_ring_n > XA2_RING ? g_ring_n - XA2_RING : 0;
	for (i = first; i < g_ring_n; i++) {
		char line[160];

		wsprintfA(line, "  %4u %s", i, g_ring[i % XA2_RING]);
		emit(line);
	}
}

/* A roster the mixer can walk, kept in the arena's header with the allocation
 * cursor, so both rewind with the voices they describe. Within a session that
 * changes nothing that matters: a voice created after the save drops out of the
 * rewound roster, and its memory is handed out again. Across sessions it is the
 * only arrangement that works, because the game comes back holding the old
 * launch's voice pointers, and the roster, the cursor and the voices have to
 * agree with them rather than with whatever this launch made at start-up. */
#define XA2_PEND 256

typedef struct {
	XA2Callback *cb;
	SwVoice *v;
	int slot;
	void *ctx;
	int epoch; /* the generation the posting voice was born in */
} Pending;

typedef struct XaHead {
	unsigned char *cur, *end;
	int vn;
	SwVoice *vtab[XA2_MAX_VOICES];
	/* Notifications not yet delivered to the game. They rewind with the
	 * voices and the game's memory, so a restore delivers exactly the ones
	 * that were pending at the save - each about something that had already
	 * happened inside the snapshot, which the restored game has not yet been
	 * told. */
	int pend_head, pend_n;
	Pending pend[XA2_PEND];
} XaHead;

static XaHead g_xa_boot;
static XaHead *g_xa = &g_xa_boot;
#define g_vtab (g_xa->vtab)
#define g_vn (g_xa->vn)
#define g_pend (g_xa->pend)
#define g_pend_head (g_xa->pend_head)
#define g_pend_n (g_xa->pend_n)

#define OUT_RATE 44100
#define OUT_CH 2
#define OUT_FRAMES 1024 /* about 23 ms; four of these is a comfortable buffer */
#define OUT_BLOCKS 4

typedef struct {
	WAVEHDR hdr[OUT_BLOCKS];
	short pcm[OUT_BLOCKS][OUT_FRAMES * OUT_CH];
	int acc[OUT_FRAMES * OUT_CH];
} MixOut;

/* Everything that belongs to this process rather than to the game's moment:
 * the lock, the waveOut device, the mixer thread and the blocks the driver
 * holds. A restore writes this DLL's data back, and from another launch these
 * would come back as the old process's handles and pointers; so they live in a
 * fixed home excluded from every snapshot, and the one pointer to it that the
 * data section holds is the same in every launch. */
#define XA2_NOW_HOME 0x5FED0000u

typedef struct {
	CRITICAL_SECTION cs;
	HWAVEOUT wo;
	HANDLE thr, wake;
	volatile LONG quit, park, idle;
	int out_live; /* set once waveOut is running */
	int wo_paused, wo_discard;
	MixOut out;
} XaNow;

static XaNow g_now_boot;
static XaNow *g_now = &g_now_boot;
#define g_cs (g_now->cs)
#define g_wo (g_now->wo)
#define g_out (&g_now->out)
#define g_mix_thr (g_now->thr)
#define g_mix_wake (g_now->wake)
#define g_mix_quit (g_now->quit)
#define g_mix_park (g_now->park)
#define g_mix_idle (g_now->idle)
#define g_out_live (g_now->out_live)
#define g_wo_paused (g_now->wo_paused)
#define g_wo_discard (g_now->wo_discard)

static int g_ready;
static LONGLONG g_qpf;
static unsigned long g_voices, g_submits, g_starved, g_overflow;
static int g_over_said;

/* The restore generation - the voice-generation valve.
 *
 * g_epoch is a static in this DLL, so it is held in the present and NOT rewound;
 * SwVoice.epoch lives in the arena and IS rewound with the game. A voice records
 * the epoch it was born in. On a restore we bump g_epoch, so every voice that
 * outlived the restore now reads an epoch behind the current one, while any voice
 * the game creates afterwards reads the new one. That difference is the signal
 * cb_callable never had: a structurally-valid callback can still be driving the
 * game's audio state machine across a track change that happened after the save,
 * and delivering it walks the game into an object the restore left dangling
 * (the rabiribi.exe+4F2AA null-`this` virtual call). When the valve is on, a
 * callback whose voice predates the current generation is refused, which both
 * stops that crash and is the mechanism by which the game must re-create voices
 * to hear anything again - exactly the "tear it down cleanly" the notes wanted. */
static volatile LONG g_epoch;
static int g_gen_valve = -1;	    /* D3D9SW_XA2_GEN: -1 unread, 0 off, 1 on */
static unsigned long g_gen_refused; /* callbacks the generation valve stopped */

unsigned savestate_getenv(const char *name, char *buf, unsigned cap);

/* Same arena rule as ds_sw: ordinary private read-write memory, captured by the
 * snapshot, not on the wrapper's CRT heap and not excluded. The queue, the
 * sample count and the timestamp it was anchored to have to come back from one
 * moment or we have rebuilt the split inside our own code. The submitted PCM is
 * not copied here - it stays where the game put it, in the game's memory, and
 * rewinds with the game, which is the correct owner for it. */
#define XA2_CHUNK (1u * 1024u * 1024u)

/* One reservation at the same address in every launch, just under the wrapper
 * DLL's pinned base, so a voice pointer the game saved in one launch names the
 * same memory in the next. Reserved on first use, before the game has created
 * anything, and committed as the cursor advances. */
#define XA2_ARENA_BASE 0x5E000000u
#define XA2_ARENA_SIZE (32u * 1024u * 1024u)

/* Audio state rewinds with the game; keep SWEXCLUDE from holding each piece of
 * it in the present, which malforms the PCM stream. */
static int arena_commit(unsigned char *p, SIZE_T n)
{
	void *c = VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE);

	if (!c)
		return 0;
	gameheap_va_unhold(c);
	return 1;
}

/* Decimal, or hex with 0x. */
static UINT_PTR xa2_knob(const char *name, UINT_PTR def)
{
	char v[24];
	unsigned n = savestate_getenv(name, v, sizeof(v)), i = 0, hex = 0;
	UINT_PTR r = 0;

	if (!n || n >= sizeof(v))
		return def;
	if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
		i = 2, hex = 1;
	for (; v[i]; i++) {
		char c = v[i];
		unsigned d = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
			     : hex && c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
			     : hex && c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10)
							   : 99u;
		if (d == 99u)
			break;
		r = r * (hex ? 16u : 10u) + d;
	}
	return r;
}

/* D3D9SW_XA2_ARENA / D3D9SW_XA2_ARENA_MB move it for a game whose own pinned
 * spans already cover the default. */
static int arena_open(void)
{
	unsigned char *base;
	UINT_PTR at = xa2_knob("D3D9SW_XA2_ARENA", XA2_ARENA_BASE);
	SIZE_T size = (SIZE_T)xa2_knob("D3D9SW_XA2_ARENA_MB", XA2_ARENA_SIZE >> 20) << 20;

	base = (unsigned char *)VirtualAlloc((void *)at, size, MEM_RESERVE, PAGE_READWRITE);
	if (base) {
		gameheap_va_unhold(base);
		ss_log("xa2_sw: voice arena reserved at %p, %u MB - the same address "
		       "in every launch\n",
		       base, (unsigned)(size >> 20));
	} else {
		size = XA2_CHUNK * 8;
		base = (unsigned char *)VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE);
		if (!base)
			return 0;
		gameheap_va_unhold(base);
		ss_log("xa2_sw: %p was taken, voice arena at %p instead - voices will "
		       "not survive a load from another launch\n",
		       (void *)at, base);
	}
	if (!arena_commit(base, sizeof(XaHead)))
		return 0;
	g_xa = (XaHead *)base;
	g_xa->cur = base + ((sizeof(XaHead) + 15) & ~(SIZE_T)15);
	g_xa->end = base + size;
	return 1;
}

static void *arena_alloc(SIZE_T n)
{
	unsigned char *p;

	if (g_xa == &g_xa_boot && !arena_open())
		return NULL;
	n = (n + 15) & ~(SIZE_T)15;
	if (n > (SIZE_T)(g_xa->end - g_xa->cur))
		return NULL;
	p = g_xa->cur;
	if (!arena_commit(p, n))
		return NULL;
	g_xa->cur += n;
	return p;
}

static LONGLONG now_qpc(void)
{
	LARGE_INTEGER t;

	/* The hooked counter, on purpose: it rewinds with the game, which is what
	 * makes the sample count and the game's own idea of time agree after a
	 * restore. */
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

/* ------------------------------------------------------------- callbacks */

/* Deferred, and that is the point.
 *
 * Once there is a mixer thread, it is the thread that discovers a buffer has
 * finished - and calling OnBufferEnd from there would put a foreign thread
 * inside DxLib's code, allocating and submitting, possibly in the middle of a
 * save. That is the exact hazard this project exists to remove, rebuilt by our
 * own hand.
 *
 * So the mixer only records what happened. The game's next call into us drains
 * the queue on the game's own thread, which is a frame of latency at worst and
 * keeps the invariant that nothing but the game ever calls the game. The queue
 * lives in the arena header (see XaHead), so it rewinds with the state it
 * describes. */
/* Two reasons a callback never runs, and they mean opposite things.
 *
 * Dropped at the park is by design: a callback queued before a rewind is about
 * a world that no longer exists, and the mixer asks again on the next pass.
 * Dropped because the ring was full is the game talking faster than we drain,
 * and that is the shape of audio going quiet. They shared one counter, which
 * is why a jump from 32 to 505 could not be read: it was either twenty parks
 * behaving normally or the ring saturating once, and the number could not say
 * which. */
static unsigned long g_pend_full;

/* Dropping the queued entries at the park closes one hole and leaves its twin
 * open. The ring is ours and can be emptied; the callback pointer each voice
 * holds is ours too and survives the rewind, but what it points AT is the
 * game's, and a callback object the game built after the save is, once the heap
 * is wound back, an address whose contents are now some unrelated older thing.
 * Nothing is queued at that moment, so cb_drop has nothing to find - the next
 * notification the mixer raises is the one that posts a fresh entry against the
 * dead object and calls straight through its first word.
 *
 * That is what took the process down: a vtable read of DFD777C1, a value that
 * is not an address this process could ever have had, called as if it were one.
 *
 * There is no way to know from here which voices outlived their callbacks, so
 * check the object instead of trusting it. A real vtable is committed memory
 * holding a pointer into committed executable memory, which garbage satisfies
 * essentially never. The last vtable that passed is remembered so that the
 * normal case - every voice sharing DxLib's one callback class - costs a single
 * comparison rather than a pair of VirtualQuery calls per notification. */
static void const *g_cb_vt_ok;
static unsigned long g_cb_refused;

static int cb_committed(const void *p, size_t n, int want_code)
{
	MEMORY_BASIC_INFORMATION mbi;
	DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
	DWORD code = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
		     PAGE_EXECUTE_WRITECOPY;

	if (!p)
		return 0;
	if (!VirtualQuery(p, &mbi, sizeof(mbi)))
		return 0;
	if (mbi.State != MEM_COMMIT || (mbi.Protect & bad))
		return 0;
	if (want_code && !(mbi.Protect & code))
		return 0;
	return (const char *)p + n <=
	       (const char *)mbi.BaseAddress + mbi.RegionSize;
}

static int cb_callable(XA2Callback *c, int slot)
{
	const void *fn;

	if (!cb_committed(c, sizeof(void *), 0) || !c->vtbl)
		return 0;
	if (!cb_committed(c->vtbl, (size_t)(slot + 1) * sizeof(void *), 0))
		return 0;
	fn = (const void *)c->vtbl[slot];
	if ((const void *)c->vtbl == g_cb_vt_ok)
		return fn != NULL;
	if (!cb_committed(fn, 1, 1))
		return 0;
	g_cb_vt_ok = (const void *)c->vtbl;
	return 1;
}

static void cb_post(SwVoice *v, int slot, void *ctx)
{
	int at;

	if (!v || !v->cb || !v->cb->vtbl)
		return;
	if (g_pend_n >= XA2_PEND) {
		g_pend_full++;
		return;
	}
	at = (g_pend_head + g_pend_n) % XA2_PEND;
	g_pend[at].cb = v->cb;
	g_pend[at].v = v;
	g_pend[at].slot = slot;
	g_pend[at].ctx = ctx;
	g_pend[at].epoch = v->epoch; /* the generation to check when it is drained */
	g_pend_n++;
}

/* Called on the game's thread, never holding the lock: DxLib submits more audio
 * from inside OnBufferEnd, and that path comes straight back in here. */
static void cb_flush(void)
{
	static LONG busy;

	/* DxLib submits more audio from inside OnBufferEnd, and that path comes
	 * straight back here. Draining only at the outermost level keeps the
	 * recursion from nesting once per notification. */
	if (InterlockedCompareExchange(&busy, 1, 0))
		return;
	for (;;) {
		Pending p;

		EnterCriticalSection(&g_cs);
		if (!g_pend_n) {
			LeaveCriticalSection(&g_cs);
			InterlockedExchange(&busy, 0);
			return;
		}
		p = g_pend[g_pend_head];
		g_pend_head = (g_pend_head + 1) % XA2_PEND;
		g_pend_n--;
		if (p.slot == CB_PASS_END && p.v)
			p.v->pass_q = 0;
		LeaveCriticalSection(&g_cs);
		/* Two of these take no argument at all, and calling them as if they
		 * did would unbalance a stdcall stack. OnVoiceProcessingPassStart
		 * takes a UINT32 rather than a pointer, which is the same single
		 * four-byte slot, so it rides the same path as the context ones. */
		/* Checked here rather than only at the post: a rewind can land
		 * between queueing an entry and draining it, so the object that
		 * was sound when it went into the ring need not still be. */
		if (!cb_callable(p.cb, p.slot)) {
			/* The first one gets a line of its own. The summary counter
			 * only prints at a park, which puts it minutes away from the
			 * moment that matters; this lands next to the restore that
			 * caused it and quotes the word that would have been called,
			 * so the log says which restore orphaned which object. */
			if (!g_cb_refused++)
				ss_log("xa2_sw: refusing to call callback %p slot %d - "
				       "its vtable reads %p, which is not callable "
				       "memory. This object did not survive a restore; "
				       "before this check that call was the crash\n",
				       (void *)p.cb, p.slot,
				       (void *)(p.cb ? (void *)p.cb->vtbl : NULL));
			continue;
		}
		/* The generation valve. cb_callable passed - the object is structurally
		 * callable - but a voice born before the current restore generation is
		 * being driven across a save/restore boundary, and the game's own audio
		 * state on the far side of it may point at objects the restore left
		 * dangling. Delivering it is the rabiribi.exe+4F2AA null-`this` crash.
		 * Refuse it; the game re-creates a voice in the current generation to
		 * hear anything again, which is the clean teardown we could not force. */
		if (g_gen_valve > 0 && p.epoch != g_epoch) {
			if (!g_gen_refused++)
				ss_log("xa2_sw: generation valve refused callback %p slot %d "
				       "- its voice was born in generation %d, we are now in "
				       "%ld. A callback across a restore boundary; delivering "
				       "it was the track-change crash\n",
				       (void *)p.cb, p.slot, p.epoch, (long)g_epoch);
			continue;
		}
		if (p.slot == CB_STREAM_END || p.slot == CB_PASS_END)
			((PFN_CB_VOID)p.cb->vtbl[p.slot])(p.cb);
		else
			((PFN_CB_CTX)p.cb->vtbl[p.slot])(p.cb, p.ctx);
	}
}

static void cb_ctx(SwVoice *v, int slot, void *ctx)
{
	cb_post(v, slot, ctx);
}

static void cb_void(SwVoice *v, int slot)
{
	cb_post(v, slot, NULL);
}

/* ---------------------------------------------------------------- clock */

/* Positions are sample indexes into the buffer, the frame XAudio2 defines
 * PlayBegin and LoopBegin in: LoopBegin is not relative to PlayBegin and may
 * lie before it. Keeping pos relative and adding PlayBegin at the read put a
 * loop back into the wrong stretch of audio whenever both were set. */
static UINT32 play_end(const SwVoice *v, const XA2_BUFFER *b)
{
	UINT32 all = v->block ? b->AudioBytes / v->block : 0;
	UINT32 e = b->PlayLength ? b->PlayBegin + b->PlayLength : all;

	return e > all ? all : e;
}

/* Where the head buffer stops for now: its loop end while loops remain,
 * otherwise the end of its play region. */
static UINT32 seg_end(const SwVoice *v, const XA2_BUFFER *b)
{
	UINT32 e = play_end(v, b);

	if (b->LoopCount) {
		UINT32 le = b->LoopLength ? b->LoopBegin + b->LoopLength : e;

		if (le < e)
			e = le;
	}
	return e;
}

static int can_loop(const SwVoice *v, const XA2_BUFFER *b)
{
	return b->LoopCount && b->LoopBegin < seg_end(v, b);
}

/* The head of the queue changed: start at its PlayBegin. */
static void head_enter(SwVoice *v)
{
	v->pos = v->n ? v->q[v->head].PlayBegin : 0;
	v->frac = 0;
	v->bstart = 0;
}

static void head_retire(SwVoice *v)
{
	v->head = (v->head + 1) % XA2_MAX_QUEUED;
	v->n--;
	head_enter(v);
}

/* Move the voice forward to now, retiring buffers and firing the callbacks the
 * game is waiting on. Called from inside the game's own calls, so every
 * callback runs on the game's thread and nothing here needs a thread of its
 * own. */
static void advance(SwVoice *v)
{
	LONGLONG now, d;
	unsigned long long s;

	if (!v->started || v->kind != 0 || v->rate_eff <= 0.0)
		return;
	if (g_out_live)
		return; /* the mixer moves it, by audio actually produced */
	now = now_qpc();
	d = now - v->anchor;
	if (d <= 0)
		return;
	s = (unsigned long long)((double)d * v->rate_eff / (double)g_qpf);
	if (!s)
		return;
	/* Keep the sub-sample remainder rather than dropping it, or a voice polled
	 * often enough would never advance at all. */
	v->anchor += (LONGLONG)((double)s * (double)g_qpf / v->rate_eff);

	while (s) {
		XA2_BUFFER *b;
		UINT32 total, end, take, avail;

		if (!v->n) {
			/* Starved. Real XAudio2 does not advance SamplesPlayed through
			 * silence it was never given, and neither do we - the game uses
			 * that number to decide when to submit more. */
			v->anchor = now;
			g_starved++;
			break;
		}
		b = &v->q[v->head];
		total = play_end(v, b);
		if (!total) {
			cb_ctx(v, CB_BUFFER_END, b->pContext);
			head_retire(v);
			continue;
		}
		if (!v->bstart) {
			cb_ctx(v, CB_BUFFER_START, b->pContext);
			v->bstart = 1;
		}

		/* A looping buffer ends at the loop point, not at the buffer end. BGM
		 * lives on this path, and getting it wrong would retire the music
		 * after one pass. */
		end = seg_end(v, b);
		avail = end > v->pos ? end - v->pos : 0;
		take = (UINT32)(s < avail ? s : avail);
		v->pos += take;
		v->played += take;
		s -= take;
		if (v->pos < end)
			break;
		if (can_loop(v, b)) {
			if (b->LoopCount != XA2_LOOP_INFINITE)
				b->LoopCount--;
			cb_ctx(v, CB_LOOP_END, b->pContext);
			v->pos = b->LoopBegin;
			continue;
		}
		cb_ctx(v, CB_BUFFER_END, b->pContext);
		if (b->Flags & XA2_END_OF_STREAM)
			cb_void(v, CB_STREAM_END);
		head_retire(v);
	}
}

/* ---------------------------------------------------------------- mixer */

/* Speakers, at last, and on our terms.
 *
 * The rule that made the silent build safe still holds: nothing outside the
 * game may touch the game's memory at a moment of our choosing. A mixer thread
 * plainly does touch it - it reads the PCM DxLib submitted, which lives in the
 * game's heap and rewinds with the game. The difference from every audio stack
 * we have fought is that this thread is ours, so stopping it is real. XAudio2's
 * own mixer could be asked to stop and would keep reading; this one is parked
 * before the first byte of a snapshot is copied and does not run again until
 * the restore is finished.
 *
 * Sample position is driven by audio actually produced rather than by the
 * clock. That is strictly better than the QPC scheme it replaces: the count
 * only moves when we do work, so parking the mixer freezes it exactly, and
 * there is no wall-clock term left to disagree with a rewind.
 *
 * Output is fixed at 44100/16/stereo because every format in the survey
 * resamples into it cleanly and a fixed sink is one less thing that can change
 * underneath a restore. waveOut rather than anything newer: winmm is already in
 * the process, it needs no COM, no device enumeration and no session
 * management, and it brings none of AUDIOSES or MMDevApi with it. */
static unsigned long g_blocks_out;

static int voice_playing(const SwVoice *v)
{
	return v->alive && v->started && v->kind == 0 && v->n;
}

/* One voice into the accumulator. Returns having advanced that voice's position
 * by exactly the audio it contributed, which is what makes SamplesPlayed a
 * report of work done rather than of time passed. */
static void mix_voice(SwVoice *v, int *acc, unsigned frames)
{
	unsigned step, f;
	float gl, gr;

	if (v->rate_eff <= 0.0)
		return;
	step = (unsigned)((v->rate_eff * 65536.0) / (double)OUT_RATE + 0.5);
	if (!step)
		step = 1;

	gl = gr = v->volume;
	if (v->mtx_dst == 2 && v->mtx_src == 1) {
		gl *= v->mtx[0];
		gr *= v->mtx[1];
	}
	if (v->chvol_n == 1) {
		gl *= v->chvol[0];
		gr *= v->chvol[0];
	} else if (v->chvol_n >= 2) {
		gl *= v->chvol[0];
		gr *= v->chvol[1];
	}

	for (f = 0; f < frames; f++) {
		XA2_BUFFER *b;
		UINT32 total, end;
		const short *s16;
		int l, r;
		UINT32 idx;

		if (!v->n) {
			g_starved += frames - f;
			return;
		}
		b = &v->q[v->head];
		total = play_end(v, b);
		end = seg_end(v, b);

		if (!total || v->pos >= end) {
			/* Retire or loop, then take this output frame again from
			 * whatever is next. */
			if (total && can_loop(v, b)) {
				if (b->LoopCount != XA2_LOOP_INFINITE)
					b->LoopCount--;
				cb_ctx(v, CB_LOOP_END, b->pContext);
				v->pos = b->LoopBegin;
			} else {
				cb_ctx(v, CB_BUFFER_END, b->pContext);
				if (b->Flags & XA2_END_OF_STREAM)
					cb_void(v, CB_STREAM_END);
				head_retire(v);
			}
			f--;
			continue;
		}
		if (!v->bstart) {
			cb_ctx(v, CB_BUFFER_START, b->pContext);
			v->bstart = 1;
		}

		idx = v->pos;
		s16 = (const short *)b->pAudioData;
		if (!s16 || !v->qsum[v->head] || v->bits != 16 ||
		    (idx + 1) * v->block > b->AudioBytes) {
			/* Anything we cannot read confidently contributes silence. The
			 * buffer still advances, so a format we do not handle costs the
			 * sound and not the timing. */
			l = r = 0;
		} else if (v->channels >= 2) {
			l = s16[idx * v->channels];
			r = s16[idx * v->channels + 1];
		} else {
			l = r = s16[idx];
		}
		acc[f * OUT_CH] += (int)(l * gl);
		acc[f * OUT_CH + 1] += (int)(r * gr);

		v->frac += step;
		v->pos += v->frac >> 16;
		v->played += v->frac >> 16;
		v->frac &= 0xFFFF;
	}
}

/* The heartbeat DxLib is actually listening to.
 *
 * The first mixing build produced silence, and the census said why in one line:
 * 703 source voices created, 29 started, and SubmitSourceBuffer never called
 * once. GetState was never called either. DxLib does not poll this API at all -
 * it streams from IXAudio2VoiceCallback, and it waits to be told how many bytes
 * the next processing pass needs before it decodes anything. Real XAudio2 calls
 * OnVoiceProcessingPassStart on every started voice every quantum whether or
 * not there is audio queued; we called it never, so the game sat waiting to be
 * asked and we sat waiting to be fed.
 *
 * BytesRequired is the shortfall rather than the whole quantum, which is what
 * XAudio2 documents and what lets a well-fed voice be told zero. */
static void pass_callbacks(unsigned frames)
{
	int i;

	for (i = 0; i < g_vn; i++) {
		SwVoice *v = g_vtab[i];
		UINT32 need, have = 0;
		int k;

		if (!v || !v->alive || !v->started || v->kind != 0 || !v->cb)
			continue;
		/* One request outstanding per voice. Every block used to post a
		 * fresh one with the same shortfall, so a game thread that drained
		 * late answered the same need two or three times over. Real XAudio2
		 * asks synchronously and never has this problem; with the request
		 * deferred, the next one waits until the last has been delivered. */
		if (v->pass_q || g_pend_n > XA2_PEND - 2)
			continue;
		/* Ask far enough ahead to cover the round trip, not just the next
		 * block. A request is posted here, drained on the game's thread a
		 * frame later, and only then decoded and submitted - so asking for one
		 * block's worth guarantees the mixer runs dry before the answer
		 * arrives. Every dry frame is emitted as silence at the correct rate,
		 * which is why the first mixing build sounded slow and crunchy without
		 * sounding pitch-shifted: the pitch was right, there was simply
		 * nothing there for part of each block. Asking for the whole output
		 * buffer's depth puts about 93 ms between the request and the
		 * shortfall it is meant to prevent. */
		need = (UINT32)((double)(frames * OUT_BLOCKS) * v->rate_eff /
					(double)OUT_RATE +
				1.0);
		for (k = 0; k < v->n; k++) {
			const XA2_BUFFER *b = &v->q[(v->head + k) % XA2_MAX_QUEUED];
			UINT32 end = play_end(v, b);

			if (b->LoopCount) {
				have = need; /* a looping buffer never runs out */
				break;
			}
			if (k == 0)
				have += end > v->pos ? end - v->pos : 0;
			else
				have += end > b->PlayBegin ? end - b->PlayBegin : 0;
			if (have >= need)
				break;
		}
		cb_post(v, CB_PASS_START,
			(void *)(UINT_PTR)(have >= need ? 0 : (need - have) * v->block));
		cb_post(v, CB_PASS_END, NULL);
		v->pass_q = 1;
	}
}

static void mix_block(short *out, int *acc, unsigned frames)
{
	unsigned i;
	SwVoice *v;

	ZeroMemory(acc, frames * OUT_CH * sizeof(int));
	EnterCriticalSection(&g_cs);
	pass_callbacks(frames);
	for (i = 0; i < (unsigned)g_vn; i++) {
		v = g_vtab[i];
		if (v && voice_playing(v))
			mix_voice(v, acc, frames);
	}
	LeaveCriticalSection(&g_cs);
	for (i = 0; i < frames * OUT_CH; i++) {
		int s = acc[i];

		/* Clamp rather than wrap. With hundreds of voices a sum can exceed
		 * the range, and wrapping turns a loud moment into a bang. */
		if (s > 32767)
			s = 32767;
		else if (s < -32768)
			s = -32768;
		out[i] = (short)s;
	}
}

static DWORD WINAPI mix_main(LPVOID p)
{
	(void)p;
	for (;;) {
		int i, sent = 0;

		if (g_mix_quit)
			break;
		if (g_mix_park) {
			InterlockedExchange(&g_mix_idle, 1);
			WaitForSingleObject(g_mix_wake, 10);
			continue;
		}
		InterlockedExchange(&g_mix_idle, 0);
		for (i = 0; i < OUT_BLOCKS; i++) {
			WAVEHDR *h = &g_out->hdr[i];

			if (!(h->dwFlags & WHDR_DONE))
				continue;
			h->dwFlags &= ~WHDR_DONE;
			mix_block(g_out->pcm[i], g_out->acc, OUT_FRAMES);
			h->dwBufferLength = OUT_FRAMES * OUT_CH * sizeof(short);
			if (waveOutWrite(g_wo, h, sizeof(*h)) == MMSYSERR_NOERROR) {
				g_blocks_out++;
				sent = 1;
			} else
				h->dwFlags |= WHDR_DONE;
		}
		if (!sent)
			WaitForSingleObject(g_mix_wake, 5);
	}
	return 0;
}

/* Excluded from the snapshot on purpose. These are the present tense: sixteen
 * kilobytes of samples on their way to the speakers, and a waveOut header the
 * driver owns a pointer into. Rewinding either would hand the driver a block it
 * has already returned, which is the very mistake the rest of this file is
 * built to avoid. The voices, which are state, stay captured. */
static void out_start(void)
{
	WAVEFORMATEX wf;
	int i;

	if (g_out_live)
		return;

	wf.wFormatTag = WAVE_FORMAT_PCM;
	wf.nChannels = OUT_CH;
	wf.nSamplesPerSec = OUT_RATE;
	wf.wBitsPerSample = 16;
	wf.nBlockAlign = OUT_CH * 2;
	wf.nAvgBytesPerSec = OUT_RATE * wf.nBlockAlign;
	wf.cbSize = 0;

	g_mix_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, (DWORD_PTR)g_mix_wake, 0, CALLBACK_EVENT) !=
	    MMSYSERR_NOERROR) {
		g_wo = NULL;
		ss_log("xa2_sw: waveOut would not open, so the engine stays silent - "
		       "everything else about the restore is unaffected\n");
		return;
	}
	for (i = 0; i < OUT_BLOCKS; i++) {
		WAVEHDR *h = &g_out->hdr[i];

		ZeroMemory(h, sizeof(*h));
		h->lpData = (LPSTR)g_out->pcm[i];
		h->dwBufferLength = OUT_FRAMES * OUT_CH * sizeof(short);
		waveOutPrepareHeader(g_wo, h, sizeof(*h));
		h->dwFlags |= WHDR_DONE;
	}
	g_out_live = 1;
	g_mix_thr = CreateThread(NULL, 0, mix_main, NULL, 0, NULL);
	if (g_mix_thr)
		SetThreadPriority(g_mix_thr, THREAD_PRIORITY_ABOVE_NORMAL);
	ss_log("xa2_sw: mixing to waveOut at %d Hz, %d channel(s), %d x %d frame(s). "
	       "The mixer is ours, so it stops for a save - which is the one thing "
	       "no sound card has ever agreed to do\n",
	       OUT_RATE, OUT_CH, OUT_BLOCKS, OUT_FRAMES);
}

/* Called before a snapshot is copied and before a restore writes anything. The
 * wait is bounded because a mixer that will not stop must not be allowed to
 * hold up a save; if it ever times out the log says so rather than continuing
 * on an assumption. */
/* The pending ring is no longer emptied here. It used to be a static in this
 * DLL, held in the present, so an entry queued for an object the game created
 * after the save survived a rewind pointing at a dead object (a fault at
 * cb_flush+0x92 reading F5C8BE54), and dropping the ring at every park was the
 * cure. But "a buffer finished" is not re-asked: a notification dropped at a
 * park is one the game never hears, at the save and again at every restore of
 * it, and DxLib's stream ring falls out of step with the voice - the section
 * that plays after a load and then plays again. The ring now lives in the
 * arena, so a restore brings back exactly the entries that were pending at the
 * save, each naming an object that exists in the restored heap. */

/* User-mode only: idle the mix thread. No waveOutPause - that waits on
 * wineserver under Proton, so park_audio_gpu calls this and returns when
 * ss_under_wine(). */
void xa2_sw_quiesce(void)
{
	int spins;

	/* g_cs exists only after XAudio2Create. A save with no engine must not
	 * enter it: that critical section is uninitialized, and the wait never
	 * ends. */
	if (!g_ready)
		return;
	if (!g_out_live)
		return;
	InterlockedExchange(&g_mix_park, 1);
	SetEvent(g_mix_wake);
	for (spins = 0; spins < 200 && !g_mix_idle; spins++)
		Sleep(1);
	if (!g_mix_idle)
		ss_log("xa2_sw: the mixer did not park within 200 ms - the copy is "
		       "going ahead anyway, so treat any audio corruption in this "
		       "restore as explained\n");
}

void xa2_sw_park(void)
{
	xa2_sw_quiesce();
	if (!g_out_live)
		return;
	waveOutPause(g_wo);
	g_wo_paused = 1;
}

void xa2_sw_resume(void)
{
	if (!g_out_live)
		return;
	/* After a load the blocks still queued in waveOut are up to 93 ms of the
	 * moment before it, and they play out ahead of the restored audio. A
	 * waveOutReset would drop them, but on Windows it runs as wdmaud's
	 * CResetAudioJob, and that job faulted inside AudioSes after cross-session
	 * loads - twice. 93 ms of stale sound is the cheaper side. */
	g_wo_discard = 0;
	g_wo_paused = 0;
	waveOutRestart(g_wo);
	InterlockedExchange(&g_mix_park, 0);
	SetEvent(g_mix_wake);
}

/* Called by the savestate engine after a restore has written memory, before the
 * mixer is let run again. Bumps the held-in-present generation so every voice
 * that survived the rewind now reads an epoch behind the current one, and reports
 * the gap: how many voices carried across, and (with the valve on) that their
 * callbacks will be refused until the game builds fresh ones. This is the signal
 * cb_callable never had - "structurally callable" versus "belongs to the world we
 * are now in." The knob is read here rather than per-callback so the hot drain
 * stays a single compare. */
void xa2_sw_restored(void)
{
	LONG e;
	int i, alive = 0, carried = 0;

	if (!g_ready)
		return;
	g_wo_discard = 1;
	if (g_gen_valve < 0) {
		char v[8];
		unsigned n = savestate_getenv("D3D9SW_XA2_GEN", v, sizeof(v));

		g_gen_valve = (n && v[0] != '0') ? 1 : 0;
	}
	e = InterlockedIncrement(&g_epoch);
	for (i = 0; i < g_vn; i++) {
		SwVoice *v = g_vtab[i];

		if (!v || !v->alive)
			continue;
		alive++;
		if (v->epoch != e)
			carried++;
	}
	if (savestate_last_load_foreign()) {
		int ok = 0, changed = 0, gone = 0, said = 0, kept = 0, k;

		EnterCriticalSection(&g_cs);
		for (i = 0; i < g_vn; i++) {
			SwVoice *v = g_vtab[i];

			if (!v || !v->alive || v->kind != 0)
				continue;
			for (k = 0; k < v->n; k++) {
				int at = (v->head + k) % XA2_MAX_QUEUED;
				const XA2_BUFFER *b = &v->q[at];
				MEMORY_BASIC_INFORMATION mbi;
				int readable = b->pAudioData &&
					       pcm_readable(b->pAudioData, b->AudioBytes);

				if (readable &&
				    pcm_sum(b->pAudioData, b->AudioBytes) == v->qsum[at]) {
					ok++;
					continue;
				}
				/* A game that decodes ahead into a buffer it already
				 * submitted moves the sum on its own; bytes the load
				 * wrote are the save's, so they play. */
				if (readable && savestate_addr_restored(b->pAudioData) == 1 &&
				    savestate_addr_restored((const BYTE *)b->pAudioData +
							    b->AudioBytes - 1) == 1) {
					v->qsum[at] = pcm_sum(b->pAudioData, b->AudioBytes);
					kept++;
					continue;
				}
				if (readable)
					changed++;
				else
					gone++;
				v->qsum[at] = 0;
				if (said++ < 8) {
					memset(&mbi, 0, sizeof(mbi));
					VirtualQuery(b->pAudioData, &mbi, sizeof(mbi));
					ss_log("xa2_sw:   voice %08lX buffer %d: %lu byte(s) at %08lX %s "
					       "- allocation %08lX, type %lX\n",
					       (unsigned long)(UINT_PTR)v, k,
					       (unsigned long)b->AudioBytes,
					       (unsigned long)(UINT_PTR)b->pAudioData,
					       readable ? "hold other bytes now" : "is not readable",
					       (unsigned long)(UINT_PTR)mbi.AllocationBase,
					       (unsigned long)mbi.Type);
				}
			}
		}
		LeaveCriticalSection(&g_cs);
		g_sub_audit = 512;
		g_sub_ok = g_sub_held = g_sub_outside = g_sub_said = 0;
		ss_log("xa2_sw: queued PCM after a load from another process - %d buffer(s) "
		       "hold what was submitted, %d changed since but were written by the "
		       "load and play, %d hold other bytes, %d unreadable. The last two play "
		       "as silence\n",
		       ok, kept, changed, gone);
	}
	ss_log("xa2_sw: restore generation %ld - %d voice(s) alive, %d carried across "
	       "this restore%s\n",
	       (long)e, alive, carried,
	       g_gen_valve > 0
		       ? " (their callbacks will be refused until the game re-creates them)"
		       : " (generation valve OFF - set D3D9SW_XA2_GEN=1 to refuse their callbacks)");
}

/* ---------------------------------------------------------------- voice */

static const void *g_src_vt[29];
static const void *g_mst_vt[20];
static const void *g_sub_vt[19];
/* The same voices as XAudio2 2.7 shapes them, for a game that asks COM for the
 * 2.7 engine. Only GetVoiceDetails (no ActiveFlags) and GetState (no Flags)
 * differ; the trailing 2.8 slots are never called by a 2.7 caller. */
static const void *g_src27_vt[29];
static const void *g_mst27_vt[20];
static const void *g_sub27_vt[19];
static int g_v27;

/* IXAudio2Voice is not IUnknown-derived: the first slot is GetVoiceDetails, and
 * a voice is released with DestroyVoice rather than Release. */
static void WINAPI V_GetVoiceDetails(SwVoice *v, XA2_VOICE_DETAILS *d)
{
	g_calls[M_DETAILS]++;
	tr("GetVoiceDetails v=%08lX kind=%d", (unsigned long)(UINT_PTR)v, v->kind);
	if (!d)
		return;
	d->CreationFlags = 0;
	d->ActiveFlags = 0;
	d->InputChannels = v->channels;
	d->InputSampleRate = v->rate;
}

typedef struct {
	UINT32 CreationFlags;
	UINT32 InputChannels;
	UINT32 InputSampleRate;
} XA27_VOICE_DETAILS;

static void WINAPI V27_GetVoiceDetails(SwVoice *v, XA27_VOICE_DETAILS *d)
{
	g_calls[M_DETAILS]++;
	if (!d)
		return;
	d->CreationFlags = 0;
	d->InputChannels = v->channels;
	d->InputSampleRate = v->rate;
}

static HRESULT WINAPI V_SetOutputVoices(SwVoice *v, const void *sends)
{
	(void)v;
	(void)sends;
	return S_OK;
}

static HRESULT WINAPI V_SetEffectChain(SwVoice *v, const void *chain)
{
	(void)v;
	(void)chain;
	return S_OK;
}

static HRESULT WINAPI V_EnableEffect(SwVoice *v, UINT32 i, UINT32 op)
{
	(void)v;
	(void)i;
	(void)op;
	return S_OK;
}

static HRESULT WINAPI V_DisableEffect(SwVoice *v, UINT32 i, UINT32 op)
{
	(void)v;
	(void)i;
	(void)op;
	return S_OK;
}

static void WINAPI V_GetEffectState(SwVoice *v, UINT32 i, BOOL *on)
{
	(void)v;
	(void)i;
	if (on)
		*on = FALSE;
}

static HRESULT WINAPI V_SetEffectParameters(SwVoice *v, UINT32 i, const void *p, UINT32 n,
					    UINT32 op)
{
	(void)v;
	(void)i;
	(void)p;
	(void)n;
	(void)op;
	return S_OK;
}

static HRESULT WINAPI V_GetEffectParameters(SwVoice *v, UINT32 i, void *p, UINT32 n)
{
	(void)v;
	(void)i;
	(void)p;
	(void)n;
	return S_OK;
}

static HRESULT WINAPI V_SetFilterParameters(SwVoice *v, const void *p, UINT32 op)
{
	(void)v;
	(void)p;
	(void)op;
	return S_OK;
}

static void WINAPI V_GetFilterParameters(SwVoice *v, void *p)
{
	(void)v;
	(void)p;
}

static HRESULT WINAPI V_SetOutputFilterParameters(SwVoice *v, void *dst, const void *p, UINT32 op)
{
	(void)v;
	(void)dst;
	(void)p;
	(void)op;
	return S_OK;
}

static void WINAPI V_GetOutputFilterParameters(SwVoice *v, void *dst, void *p)
{
	(void)v;
	(void)dst;
	(void)p;
}

static HRESULT WINAPI V_SetVolume(SwVoice *v, float vol, UINT32 op)
{
	(void)op;
	g_calls[M_VOLUME]++;
	v->volume = vol;
	return S_OK;
}

static void WINAPI V_GetVolume(SwVoice *v, float *vol)
{
	if (vol)
		*vol = v->volume;
}

static HRESULT WINAPI V_SetChannelVolumes(SwVoice *v, UINT32 n, const float *vols, UINT32 op)
{
	UINT32 i;

	(void)op;
	g_calls[M_CHANVOL]++;
	/* This is how the game sets its levels - 586 calls a session against zero
	 * for SetVolume - so ignoring it meant every sound played at full scale and
	 * the sum of them clipped. That is the other half of "crunchy": gaps
	 * account for the stutter, saturation for the distortion. */
	EnterCriticalSection(&g_cs);
	v->chvol_n = n > 8 ? 8 : (int)n;
	for (i = 0; vols && i < (UINT32)v->chvol_n; i++)
		v->chvol[i] = vols[i];
	LeaveCriticalSection(&g_cs);
	cb_flush();
	return S_OK;
}

static void WINAPI V_GetChannelVolumes(SwVoice *v, UINT32 n, float *vols)
{
	UINT32 i;

	(void)v;
	for (i = 0; vols && i < n; i++)
		vols[i] = 1.0f;
}

static HRESULT WINAPI V_SetOutputMatrix(SwVoice *v, void *dst, UINT32 src_ch, UINT32 dst_ch,
					const float *m, UINT32 op)
{
	(void)v;
	(void)dst;
	(void)src_ch;
	(void)dst_ch;
	(void)m;
	(void)op;
	/* Panning arrives here rather than as a pan value. Stored nowhere while we
	 * are silent, but counted, because it is the second half of the question
	 * about how this game positions its sounds. */
	tr("SetOutputMatrix src=%u dst=%u", src_ch, dst_ch);
	g_calls[M_OUTMATRIX]++;
	return S_OK;
}

static void WINAPI V_GetOutputMatrix(SwVoice *v, void *dst, UINT32 src_ch, UINT32 dst_ch, float *m)
{
	UINT32 i;

	(void)v;
	(void)dst;
	for (i = 0; m && i < src_ch * dst_ch; i++)
		m[i] = 1.0f;
}

static void WINAPI V_DestroyVoice(SwVoice *v)
{
	g_calls[M_DESTROY]++;
	tr("DestroyVoice v=%08lX", (unsigned long)(UINT_PTR)v);
	if (!v) {
		static volatile LONG said;

		if (!InterlockedExchange(&said, 1))
			ss_log("xa2_sw: DestroyVoice on a null voice, from %p - ignored\n",
			       __builtin_return_address(0));
		return;
	}
	EnterCriticalSection(&g_cs);
	/* Marked dead, never reused. An address that meant one sound at save time
	 * and a different one at restore time is the entity-pool problem in
	 * miniature, and the arena is cheap enough that we simply never take that
	 * risk. */
	v->alive = 0;
	v->started = 0;
	v->n = 0;
	LeaveCriticalSection(&g_cs);
}

/* --------------------------------------------------- source voice extras */

static HRESULT WINAPI S_Start(SwVoice *v, UINT32 flags, UINT32 op)
{
	(void)flags;
	(void)op;
	g_calls[M_START]++;
	tr("Start v=%08lX", (unsigned long)(UINT_PTR)v);
	EnterCriticalSection(&g_cs);
	if (!v->started) {
		v->anchor = now_qpc();
		v->started = 1;
	}
	LeaveCriticalSection(&g_cs);
	cb_flush();
	return S_OK;
}

static HRESULT WINAPI S_Stop(SwVoice *v, UINT32 flags, UINT32 op)
{
	(void)flags;
	(void)op;
	g_calls[M_STOP]++;
	tr("Stop v=%08lX", (unsigned long)(UINT_PTR)v);
	EnterCriticalSection(&g_cs);
	advance(v);
	v->started = 0;
	LeaveCriticalSection(&g_cs);
	cb_flush();
	return S_OK;
}

/* After a load from another process: where the PCM the game goes on submitting
 * lives. A sample the game loaded once and keeps replaying from memory is only
 * the save's sample if the load wrote that memory; left in the present, it is
 * whatever this launch keeps at that address. */
static void submit_audit(SwVoice *v, const XA2_BUFFER *b)
{
	int r = b->pAudioData ? savestate_addr_restored(b->pAudioData) : 1, k;
	MEMORY_BASIC_INFORMATION mbi;

	if (r == 1)
		g_sub_ok++;
	else {
		memset(&mbi, 0, sizeof(mbi));
		VirtualQuery(b->pAudioData, &mbi, sizeof(mbi));
		if (r == 0)
			g_sub_held++;
		else
			g_sub_outside++;
		for (k = 0; k < g_sub_said; k++)
			if (g_sub_seen[k] == (UINT_PTR)mbi.AllocationBase)
				break;
		if (k == g_sub_said && g_sub_said < 16) {
			g_sub_seen[g_sub_said++] = (UINT_PTR)mbi.AllocationBase;
			ss_log("xa2_sw: submit to voice %08lX reads %lu byte(s) at %08lX, in "
			       "allocation %08lX type %lX - %s\n",
			       (unsigned long)(UINT_PTR)v, (unsigned long)b->AudioBytes,
			       (unsigned long)(UINT_PTR)b->pAudioData,
			       (unsigned long)(UINT_PTR)mbi.AllocationBase, (unsigned long)mbi.Type,
			       r == 0 ? "the save had this region and the load LEFT IT in the present"
				      : "not in the save at all");
		}
	}
	if (--g_sub_audit == 0)
		ss_log("xa2_sw: of the first 512 submits after the load, %d read restored "
		       "memory, %d memory the load left in the present, %d memory the save "
		       "never held\n",
		       g_sub_ok, g_sub_held, g_sub_outside);
}

static HRESULT WINAPI S_SubmitSourceBuffer(SwVoice *v, const XA2_BUFFER *b, const void *wmadata)
{
	(void)wmadata;
	g_calls[M_SUBMIT]++;
	tr("SubmitSourceBuffer v=%08lX bytes=%u loop=%u", (unsigned long)(UINT_PTR)v,
	   b ? b->AudioBytes : 0, b ? b->LoopCount : 0);
	if (!b)
		return E_INVALIDARG;
	if (g_sub_audit > 0)
		submit_audit(v, b);
	EnterCriticalSection(&g_cs);
	advance(v);
	if (v->n >= XA2_MAX_QUEUED) {
		/* Real XAudio2 allows 64 queued buffers and returns an error past
		 * that; DxLib streams with two or three, so reaching this means
		 * something is wrong rather than busy. Counted so it cannot happen
		 * quietly.
		 *
		 * The count alone could not say what was wrong, and sessions have
		 * ended with 24, 57 and 128 of these. A queue only fills if nothing
		 * is retiring it, and nothing retires a voice the mixer will not
		 * touch: voice_playing wants alive, started, kind 0 and a non-empty
		 * queue, and Stop clears started just as surely as never having
		 * begun does. Those are different bugs with the same counter, so
		 * the first one says which it is. */
		g_overflow++;
		if (!g_over_said) {
			g_over_said = 1;
			ss_log("xa2_sw: voice %08lX refused a buffer, %d already queued "
			       "- alive %d, started %d, kind %d, rate %d Hz after the "
			       "ratio, callback %08lX. Nothing retires a voice the mixer "
			       "skips, so a stopped or never-started one fills to the "
			       "limit and every submit after that is lost audio\n",
			       (unsigned long)(UINT_PTR)v, v->n, v->alive, v->started,
			       v->kind, (int)v->rate_eff,
			       (unsigned long)(UINT_PTR)v->cb);
		}
		LeaveCriticalSection(&g_cs);
		return E_FAIL;
	}
	v->q[(v->head + v->n) % XA2_MAX_QUEUED] = *b;
	v->qsum[(v->head + v->n) % XA2_MAX_QUEUED] = pcm_sum(b->pAudioData, b->AudioBytes);
	if (v->n++ == 0)
		head_enter(v);
	g_submits++;
	LeaveCriticalSection(&g_cs);
	cb_flush();
	return S_OK;
}

static HRESULT WINAPI S_FlushSourceBuffers(SwVoice *v)
{
	g_calls[M_FLUSH]++;
	EnterCriticalSection(&g_cs);
	v->n = 0;
	head_enter(v);
	v->anchor = now_qpc();
	LeaveCriticalSection(&g_cs);
	cb_flush();
	return S_OK;
}

static HRESULT WINAPI S_Discontinuity(SwVoice *v)
{
	(void)v;
	g_calls[M_DISCONT]++;
	return S_OK;
}

static HRESULT WINAPI S_ExitLoop(SwVoice *v, UINT32 op)
{
	(void)op;
	g_calls[M_EXITLOOP]++;
	EnterCriticalSection(&g_cs);
	if (v->n)
		v->q[v->head].LoopCount = 0;
	LeaveCriticalSection(&g_cs);
	return S_OK;
}

/* Two arguments, as in the XAudio2 2.8 and later headers. This matters more
 * than it looks: these are stdcall, so the callee pops the arguments, and a
 * one-argument definition called with two would walk the caller's stack. The
 * game asks for xaudio2_9 by name, so it is compiled against the header that
 * has the Flags parameter. */
static void WINAPI S_GetState(SwVoice *v, XA2_VOICE_STATE *st, UINT32 flags)
{
	(void)flags;
	g_calls[M_GETSTATE]++;
	if (!st)
		return;
	EnterCriticalSection(&g_cs);
	advance(v);
	st->pCurrentBufferContext = v->n ? v->q[v->head].pContext : NULL;
	st->BuffersQueued = (UINT32)v->n;
	st->SamplesPlayed = v->played;
	LeaveCriticalSection(&g_cs);
	cb_flush();
}

static void WINAPI S27_GetState(SwVoice *v, XA2_VOICE_STATE *st)
{
	S_GetState(v, st, 0);
}

static HRESULT WINAPI S_SetFrequencyRatio(SwVoice *v, float ratio, UINT32 op)
{
	(void)op;
	g_calls[M_FREQRATIO]++;
	EnterCriticalSection(&g_cs);
	/* Settle first: samples already played were played at the old rate, and
	 * changing the rate without folding them in would move the count
	 * retroactively. */
	advance(v);
	v->freq_ratio = ratio > 0.0f ? ratio : 1.0f;
	v->rate_eff = (double)v->rate * (double)v->freq_ratio;
	LeaveCriticalSection(&g_cs);
	cb_flush();
	return S_OK;
}

static void WINAPI S_GetFrequencyRatio(SwVoice *v, float *ratio)
{
	if (ratio)
		*ratio = v->freq_ratio;
}

static HRESULT WINAPI S_SetSourceSampleRate(SwVoice *v, UINT32 rate)
{
	EnterCriticalSection(&g_cs);
	advance(v);
	v->rate = rate ? rate : v->rate;
	v->rate_eff = (double)v->rate * (double)v->freq_ratio;
	LeaveCriticalSection(&g_cs);
	return S_OK;
}

static HRESULT WINAPI M_GetChannelMask(SwVoice *v, DWORD *mask)
{
	(void)v;
	if (mask)
		*mask = 0x3; /* front left and right */
	return S_OK;
}

/* --------------------------------------------------------------- engine */

static SwVoice *voice_new(int kind, UINT32 channels, UINT32 rate, UINT32 bits, XA2Callback *cb)
{
	SwVoice *v = (SwVoice *)arena_alloc(sizeof(*v));

	if (!v)
		return NULL;
	ZeroMemory(v, sizeof(*v));
	if (g_v27)
		v->vtbl = kind == 0 ? g_src27_vt : (kind == 1 ? g_mst27_vt : g_sub27_vt);
	else
		v->vtbl = kind == 0 ? g_src_vt : (kind == 1 ? g_mst_vt : g_sub_vt);
	v->kind = kind;
	v->alive = 1;
	v->epoch = g_epoch; /* the generation it is born in; rewinds with the arena */
	v->cb = cb;
	v->channels = channels ? channels : 2;
	v->rate = rate ? rate : 44100;
	v->bits = bits ? bits : 16;
	v->block = v->channels * (v->bits / 8);
	v->freq_ratio = 1.0f;
	v->volume = 1.0f;
	v->chvol[0] = v->chvol[1] = 1.0f;
	v->rate_eff = (double)v->rate;
	if (g_vn < XA2_MAX_VOICES)
		g_vtab[g_vn++] = v;
	g_voices++;
	return v;
}

static HRESULT WINAPI E_QueryInterface(SwEngine *e, const GUID *iid, void **out)
{
	(void)iid;
	if (!out)
		return E_INVALIDARG;
	InterlockedIncrement(&e->ref);
	*out = e;
	return S_OK;
}

static ULONG WINAPI E_AddRef(SwEngine *e)
{
	return (ULONG)InterlockedIncrement(&e->ref);
}

static ULONG WINAPI E_Release(SwEngine *e)
{
	LONG r = InterlockedDecrement(&e->ref);

	return (ULONG)(r < 0 ? 0 : r);
}

static HRESULT WINAPI E_RegisterForCallbacks(SwEngine *e, void *cb)
{
	(void)e;
	(void)cb;
	return S_OK;
}

/* Leaving this out cost a run. It sits between RegisterForCallbacks and
 * CreateSourceVoice, so omitting it shifted every entry after it up by one and
 * DxLib's CreateMasteringVoice landed on StartEngine - which returns S_OK
 * without ever writing the out-pointer, so the game read through the NULL it
 * was left holding and died at rabiribi.exe+0x4d6d4 before the first present.
 * A vtable is a contract counted in slots, and a missing void method is as
 * damaging as a wrong one. */
static void WINAPI E_UnregisterForCallbacks(SwEngine *e, void *cb)
{
	(void)e;
	(void)cb;
}

static HRESULT WINAPI E_CreateSourceVoice(SwEngine *e, void **out, const WAVEFORMATEX *fmt,
					  UINT32 flags, float max_ratio, XA2Callback *cb,
					  const void *sends, const void *chain)
{
	SwVoice *v;

	(void)e;
	(void)flags;
	(void)max_ratio;
	(void)sends;
	(void)chain;
	g_calls[M_CREATE_SOURCE]++;
	tr("CreateSourceVoice ch=%u rate=%u bits=%u cb=%08lX",
	   fmt ? fmt->nChannels : 0, fmt ? fmt->nSamplesPerSec : 0,
	   fmt ? fmt->wBitsPerSample : 0, (unsigned long)(UINT_PTR)cb);
	if (!out)
		return E_INVALIDARG;
	EnterCriticalSection(&g_cs);
	v = voice_new(0, fmt ? fmt->nChannels : 0, fmt ? fmt->nSamplesPerSec : 0,
		      fmt ? fmt->wBitsPerSample : 0, cb);
	LeaveCriticalSection(&g_cs);
	if (!v)
		return E_OUTOFMEMORY;
	*out = v;
	return S_OK;
}

static HRESULT WINAPI E_CreateSubmixVoice(SwEngine *e, void **out, UINT32 channels, UINT32 rate,
					  UINT32 flags, UINT32 stage, const void *sends,
					  const void *chain)
{
	SwVoice *v;

	(void)e;
	(void)flags;
	(void)stage;
	(void)sends;
	(void)chain;
	g_calls[M_CREATE_SUBMIX]++;
	tr("CreateSubmixVoice ch=%u rate=%u", channels, rate);
	if (!out)
		return E_INVALIDARG;
	EnterCriticalSection(&g_cs);
	v = voice_new(2, channels, rate, 16, NULL);
	LeaveCriticalSection(&g_cs);
	if (!v)
		return E_OUTOFMEMORY;
	*out = v;
	return S_OK;
}

/* Seven parameters, the XAudio2 2.8 and later shape. The 2.7 version took a
 * device index where this takes a device id string and has no stream category,
 * and mixing the two up on stdcall would unbalance the stack. */
static HRESULT WINAPI E_CreateMasteringVoice(SwEngine *e, void **out, UINT32 channels, UINT32 rate,
					     UINT32 flags, const wchar_t *device,
					     const void *chain, int category)
{
	SwVoice *v;

	(void)e;
	(void)flags;
	(void)device;
	(void)chain;
	(void)category;
	g_calls[M_CREATE_MASTER]++;
	tr("CreateMasteringVoice ch=%u rate=%u", channels, rate);
	if (!out)
		return E_INVALIDARG;
	EnterCriticalSection(&g_cs);
	v = voice_new(1, channels ? channels : 2, rate ? rate : 44100, 16, NULL);
	LeaveCriticalSection(&g_cs);
	if (!v)
		return E_OUTOFMEMORY;
	*out = v;
	return S_OK;
}

static HRESULT WINAPI E_StartEngine(SwEngine *e)
{
	(void)e;
	return S_OK;
}

static void WINAPI E_StopEngine(SwEngine *e)
{
	(void)e;
}

static HRESULT WINAPI E_CommitChanges(SwEngine *e, UINT32 op)
{
	(void)e;
	(void)op;
	return S_OK;
}

static void WINAPI E_GetPerformanceData(SwEngine *e, void *data)
{
	(void)e;
	/* The struct is large and entirely statistics; zeroing it is honest and is
	 * what a silent engine has to report. */
	if (data)
		ZeroMemory(data, 64);
}

static HRESULT WINAPI E_SetDebugConfiguration(SwEngine *e, const void *cfg, void *reserved)
{
	(void)e;
	(void)cfg;
	(void)reserved;
	return S_OK;
}

/* XAudio2 2.7 is a COM class: created uninitialised, then Initialize, and it
 * enumerates devices by index. XAudio2.h of that era is pack(1), so the device
 * details are 1068 bytes: two 256-WCHAR strings, the role, then a
 * WAVEFORMATEXTENSIBLE. */
static HRESULT WINAPI E27_GetDeviceCount(SwEngine *e, UINT32 *n)
{
	(void)e;
	if (n)
		*n = 1;
	return S_OK;
}

static HRESULT WINAPI E27_GetDeviceDetails(SwEngine *e, UINT32 i, BYTE *d)
{
	static const BYTE pcm[16] = { 0x01, 0, 0, 0, 0, 0, 0x10, 0,
				      0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
	WAVEFORMATEX *wf;

	(void)e;
	if (i != 0 || !d)
		return E_INVALIDARG;
	ZeroMemory(d, 1068);
	lstrcpyW((WCHAR *)d, L"xa2_sw");
	lstrcpyW((WCHAR *)(d + 512), L"xa2_sw");
	*(UINT32 *)(d + 1024) = 0xF; /* GlobalDefaultDevice */
	wf = (WAVEFORMATEX *)(d + 1028);
	wf->wFormatTag = 0xFFFE;
	wf->nChannels = 2;
	wf->nSamplesPerSec = 44100;
	wf->wBitsPerSample = 16;
	wf->nBlockAlign = 4;
	wf->nAvgBytesPerSec = 44100 * 4;
	wf->cbSize = 22;
	*(WORD *)(d + 1028 + 18) = 16;	  /* valid bits */
	*(DWORD *)(d + 1028 + 20) = 0x3; /* front left and right */
	CopyMemory(d + 1028 + 24, pcm, 16);
	return S_OK;
}

static HRESULT WINAPI E27_Initialize(SwEngine *e, UINT32 flags, UINT32 processor)
{
	(void)e;
	tr("Initialize (2.7) flags=%u processor=%u", flags, processor);
	return S_OK;
}

static HRESULT WINAPI E27_CreateMasteringVoice(SwEngine *e, void **out, UINT32 channels,
					       UINT32 rate, UINT32 flags, UINT32 device,
					       const void *chain)
{
	(void)device;
	return E_CreateMasteringVoice(e, out, channels, rate, flags, NULL, chain, 0);
}

static const void *g_eng_vt[13];
static const void *g_eng27_vt[16];

static void vt_init(void)
{
	g_eng_vt[0] = (const void *)E_QueryInterface;
	g_eng_vt[1] = (const void *)E_AddRef;
	g_eng_vt[2] = (const void *)E_Release;
	g_eng_vt[3] = (const void *)E_RegisterForCallbacks;
	g_eng_vt[4] = (const void *)E_UnregisterForCallbacks;
	g_eng_vt[5] = (const void *)E_CreateSourceVoice;
	g_eng_vt[6] = (const void *)E_CreateSubmixVoice;
	g_eng_vt[7] = (const void *)E_CreateMasteringVoice;
	g_eng_vt[8] = (const void *)E_StartEngine;
	g_eng_vt[9] = (const void *)E_StopEngine;
	g_eng_vt[10] = (const void *)E_CommitChanges;
	g_eng_vt[11] = (const void *)E_GetPerformanceData;
	g_eng_vt[12] = (const void *)E_SetDebugConfiguration;

	/* IXAudio2Voice, shared prefix of all three voice kinds. */
	g_sub_vt[0] = (const void *)V_GetVoiceDetails;
	g_sub_vt[1] = (const void *)V_SetOutputVoices;
	g_sub_vt[2] = (const void *)V_SetEffectChain;
	g_sub_vt[3] = (const void *)V_EnableEffect;
	g_sub_vt[4] = (const void *)V_DisableEffect;
	g_sub_vt[5] = (const void *)V_GetEffectState;
	g_sub_vt[6] = (const void *)V_SetEffectParameters;
	g_sub_vt[7] = (const void *)V_GetEffectParameters;
	g_sub_vt[8] = (const void *)V_SetFilterParameters;
	g_sub_vt[9] = (const void *)V_GetFilterParameters;
	g_sub_vt[10] = (const void *)V_SetOutputFilterParameters;
	g_sub_vt[11] = (const void *)V_GetOutputFilterParameters;
	g_sub_vt[12] = (const void *)V_SetVolume;
	g_sub_vt[13] = (const void *)V_GetVolume;
	g_sub_vt[14] = (const void *)V_SetChannelVolumes;
	g_sub_vt[15] = (const void *)V_GetChannelVolumes;
	g_sub_vt[16] = (const void *)V_SetOutputMatrix;
	g_sub_vt[17] = (const void *)V_GetOutputMatrix;
	g_sub_vt[18] = (const void *)V_DestroyVoice;

	CopyMemory(g_mst_vt, g_sub_vt, 19 * sizeof(void *));
	g_mst_vt[19] = (const void *)M_GetChannelMask;

	CopyMemory(g_src_vt, g_sub_vt, 19 * sizeof(void *));
	g_src_vt[19] = (const void *)S_Start;
	g_src_vt[20] = (const void *)S_Stop;
	g_src_vt[21] = (const void *)S_SubmitSourceBuffer;
	g_src_vt[22] = (const void *)S_FlushSourceBuffers;
	g_src_vt[23] = (const void *)S_Discontinuity;
	g_src_vt[24] = (const void *)S_ExitLoop;
	g_src_vt[25] = (const void *)S_GetState;
	g_src_vt[26] = (const void *)S_SetFrequencyRatio;
	g_src_vt[27] = (const void *)S_GetFrequencyRatio;
	g_src_vt[28] = (const void *)S_SetSourceSampleRate;

	g_eng27_vt[0] = (const void *)E_QueryInterface;
	g_eng27_vt[1] = (const void *)E_AddRef;
	g_eng27_vt[2] = (const void *)E_Release;
	g_eng27_vt[3] = (const void *)E27_GetDeviceCount;
	g_eng27_vt[4] = (const void *)E27_GetDeviceDetails;
	g_eng27_vt[5] = (const void *)E27_Initialize;
	g_eng27_vt[6] = (const void *)E_RegisterForCallbacks;
	g_eng27_vt[7] = (const void *)E_UnregisterForCallbacks;
	g_eng27_vt[8] = (const void *)E_CreateSourceVoice;
	g_eng27_vt[9] = (const void *)E_CreateSubmixVoice;
	g_eng27_vt[10] = (const void *)E27_CreateMasteringVoice;
	g_eng27_vt[11] = (const void *)E_StartEngine;
	g_eng27_vt[12] = (const void *)E_StopEngine;
	g_eng27_vt[13] = (const void *)E_CommitChanges;
	g_eng27_vt[14] = (const void *)E_GetPerformanceData;
	g_eng27_vt[15] = (const void *)E_SetDebugConfiguration;

	CopyMemory(g_sub27_vt, g_sub_vt, sizeof(g_sub_vt));
	g_sub27_vt[0] = (const void *)V27_GetVoiceDetails;
	CopyMemory(g_mst27_vt, g_mst_vt, sizeof(g_mst_vt));
	g_mst27_vt[0] = (const void *)V27_GetVoiceDetails;
	CopyMemory(g_src27_vt, g_src_vt, sizeof(g_src_vt));
	g_src27_vt[0] = (const void *)V27_GetVoiceDetails;
	g_src27_vt[25] = (const void *)S27_GetState;
}

/* ---------------------------------------------------------------- entry */

int xa2_sw_armed(void)
{
	return g_ready;
}

/* Once a frame, from the game's thread. */
void xa2_sw_pump(void)
{
	if (g_ready)
		cb_flush();
}

/* Printed at every save, next to the restore it explains. The call census is
 * the same instrument the DirectSound survey was, and for the same reason:
 * nobody knows which part of an audio API a game actually uses until it is
 * counted, and the answer decides what a stand-in has to be correct about. */
void xa2_sw_report(void)
{
	int i;

	if (!g_ready)
		return;
	/* Dry frames against blocks mixed is the ratio that names a stutter without
	 * anyone having to listen for it: at 1024 frames a block, a few thousand
	 * dry frames is an underrun and not a rounding error. */
	ss_log("xa2_sw: %lu voice(s), %lu buffer(s) submitted, %lu block(s) mixed, "
	       "%lu dry frame(s), %lu overflow(s), %d callback(s) pending and carried "
	       "with the snapshot, %lu dropped because the ring was full%s, %lu "
	       "refused as no longer callable%s. Every callback ran on the game's "
	       "own thread\n",
	       g_voices, g_submits, g_blocks_out, g_starved, g_overflow, g_pend_n,
	       g_pend_full,
	       g_pend_full ? " <<< the second number is audio going quiet" : "",
	       g_cb_refused,
	       g_cb_refused ? " <<< each of those would have been a crash" : "");
	if (g_epoch || g_gen_refused)
		ss_log("xa2_sw: restore generation %ld, %lu callback(s) refused by the "
		       "generation valve as born before the current one%s\n",
		       (long)g_epoch, g_gen_refused,
		       g_gen_refused ? " <<< each would have driven the game's audio "
				       "across a restore boundary" : "");
	for (i = 0; i < M_MAX; i++)
		if (g_calls[i])
			ss_log("  %-22s %lu call(s)\n", g_mname[i], g_calls[i]);
	for (i = 0; i < M_MAX; i++)
		if (!g_calls[i])
			ss_log("  %-22s never called\n", g_mname[i]);
}

static HRESULT engine_new(void **out, UINT32 flags, int v27);

__declspec(dllexport) HRESULT WINAPI xa2_sw_create(void **out, UINT32 flags, UINT32 processor)
{
	(void)processor;
	return engine_new(out, flags, 0);
}

/* What CoCreateInstance(CLSID_XAudio2) hands a 2.7 game: our engine with the
 * 2.7 vtables, and every voice it makes shaped the same way. */
HRESULT xa2_sw_create27(void **out)
{
	return engine_new(out, 0, 1);
}

static HRESULT engine_new(void **out, UINT32 flags, int v27)
{
	SwEngine *e;
	LARGE_INTEGER f;

	(void)flags;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	tr("XAudio2Create flags=%u%s", flags, v27 ? " (2.7, through COM)" : "");
	if (!g_ready) {
		XaNow *now = (XaNow *)VirtualAlloc((void *)XA2_NOW_HOME, sizeof(XaNow),
						   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

		if (!now) {
			ss_log("xa2_sw: present-tense home %08lX taken (error %lu) - placed by "
			       "the OS, so a load from another launch will point at the old one\n",
			       (unsigned long)XA2_NOW_HOME, GetLastError());
			now = (XaNow *)VirtualAlloc(NULL, sizeof(XaNow), MEM_COMMIT | MEM_RESERVE,
						    PAGE_READWRITE);
		}
		if (now) {
			savestate_exclude(now, sizeof(XaNow));
			g_now = now;
		}
		InitializeCriticalSection(&g_cs);
		savestate_own_cs(&g_cs);
		QueryPerformanceFrequency(&f);
		g_qpf = f.QuadPart ? f.QuadPart : 1;
		vt_init();
		g_ready = 1;
		ss_log("xa2_sw: the software XAudio2 is answering. Samples played is a "
		       "number we advance from the rewound clock, held in captured "
		       "memory, and buffers are retired on the game's own thread - so "
		       "there is no second party in this process with an opinion about "
		       "where the audio is\n");
	}
	e = (SwEngine *)arena_alloc(sizeof(*e));
	if (!e)
		return E_OUTOFMEMORY;
	ZeroMemory(e, sizeof(*e));
	g_v27 = v27;
	e->vtbl = v27 ? g_eng27_vt : g_eng_vt;
	e->ref = 1;
	if (v27)
		ss_log("xa2_sw: answering CoCreateInstance(XAudio2 2.7) - the real "
		       "XAudio2_7.dll is never loaded\n");
	*out = e;
	out_start();
	return S_OK;
}
