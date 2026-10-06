/* A DirectSound that is ours.
 *
 * Every audio backend this game can be asked to use dies across a restore:
 * DirectSound, DxLib's -softsound, and XAudio2 all produced faults at frame
 * zero, in their own code, on memory we had rewound. -nosound survives
 * everything - room changes, boss rooms, repeated restores - and the reason is
 * not that the game stops making sound. It is that with -nosound there is no
 * second party in the process holding pointers into the game's memory,
 * advancing cursors on its own schedule, and running threads we cannot stop.
 *
 * Every fix attempted from the outside was an attempt to keep two clocks
 * agreeing: DxLib's write position rewinds because it is the game's, and a
 * hardware play cursor does not because it is the card's. Holding DirectSound's
 * objects in the present made the mixer coherent and the cursors wrong. Winding
 * the cursors back made the cursors right and the objects wrong. There is no
 * arrangement of those two that works, because the split is real.
 *
 * So the play cursor becomes an integer we compute, from a clock that already
 * rewinds - the savestate's hooked QueryPerformanceCounter - stored in memory
 * the snapshot captures. After a restore, the play position and the write
 * position are two numbers from the same moment, which is the entire property
 * -nosound has and nothing else does.
 *
 * Scope is set by measurement, not by DirectSound's surface area. The survey in
 * dsoundhook.c watched a full session: 705 buffers, every one of them 16-bit
 * PCM at 11025 to 48100 Hz, every one created with the same flags (000180E2),
 * none ever duplicated. QueryInterface, GetFormat and GetVolume were never
 * called once. The methods that were are GetCurrentPosition (19503),
 * GetStatus (17366), SetFrequency (10942), Stop (5606), Lock and Unlock (1505
 * each), SetCurrentPosition (1462), SetVolume and SetPan (1019 each), and Play
 * (62). Everything else is here to be correct if asked and is not expected to
 * be.
 *
 * Phase one is silent. Play only starts the cursor moving. That is deliberate:
 * the thing being tested is whether the game survives, and adding a mixer
 * thread before that is answered would put a new writer into the process at
 * exactly the moment we are trying to prove there is not one. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/* WIN32_LEAN_AND_MEAN drops mmsystem.h, and WAVEFORMATEX lives there. mmreg.h
 * is the piece of it that matters and carries no multimedia API surface. */
#include <mmreg.h>
#include <stdarg.h>
#include <stddef.h>
#include "savestate.h"

#define DSERR_INVALIDPARAM 0x80070057L
#define DSBSTATUS_PLAYING 0x00000001
#define DSBSTATUS_LOOPING 0x00000004
#define DSBPLAY_LOOPING 0x00000001
#define DSBLOCK_FROMWRITECURSOR 0x00000001
#define DSBLOCK_ENTIREBUFFER 0x00000002
#define DSBCAPS_PRIMARYBUFFER 0x00000001
#define DSBFREQUENCY_ORIGINAL 0

void savestate_log_line(const char *s); /* the engine's log, shared deliberately */

/* Same arrangement as dsoundhook.c: wsprintfA into the savestate log, so these
 * lines sit beside the restore they describe. No %p; addresses are %08lX. */
static void ss_log(const char *fmt, ...)
{
	char b[512];
	va_list ap;

	va_start(ap, fmt);
	wvsprintfA(b, fmt, ap);
	va_end(ap);
	savestate_log_line(b);
}

typedef struct {
	DWORD dwSize;
	DWORD dwFlags;
	DWORD dwBufferBytes;
	DWORD dwReserved;
	WAVEFORMATEX *lpwfxFormat;
	GUID guid3DAlgorithm;
} DSBUFFERDESC_SW;

typedef struct SwBuf SwBuf;
typedef struct SwDev SwDev;

struct SwBuf {
	const void **vtbl; /* first, because that is what makes it a COM object */
	LONG ref;
	SwDev *dev;
	unsigned char *pcm;
	DWORD size;
	DWORD nominal_hz, hz;
	WORD channels, bits;
	DWORD bps; /* bytes per second at the current frequency */
	LONG volume, pan;
	DWORD flags;
	/* The cursor, as two numbers rather than a hardware fact. `at` is where
	 * the cursor was when `anchor` was taken; everything else is derived. Both
	 * live in captured memory and the clock behind `anchor` rewinds, so a
	 * restore puts the pair back together rather than half of it. */
	DWORD at;
	LONGLONG anchor;
	int playing, looping, primary;
};

struct SwDev {
	const void **vtbl;
	LONG ref;
	DWORD coop;
};

/* ---------------------------------------------------------------- arena */

/* Ordinary private read-write memory, deliberately not excluded from the
 * snapshot and deliberately not on the wrapper's CRT heap, which is held in the
 * present under REWIND_SWHEAP=0. The whole design depends on this memory
 * travelling with the game: PCM, cursor and anchor have to come back from the
 * same instant or we have rebuilt the problem inside our own code.
 *
 * Committed in chunks rather than one reservation because the survey gives the
 * range of buffer sizes (2408 bytes to 4.2 MB) but not their sum, and reserving
 * for the worst case would add hundreds of megabytes to every snapshot. A
 * buffer larger than a chunk gets a region of its own. */
#define DS_CHUNK (8u * 1024u * 1024u)

static CRITICAL_SECTION g_cs;
static int g_ready;
static unsigned char *g_chunk;
static SIZE_T g_chunk_left;
static unsigned long g_bufs, g_bytes;
static LONGLONG g_qpf;

static void *arena_alloc(SIZE_T n)
{
	void *p;

	n = (n + 15) & ~(SIZE_T)15;
	if (n > g_chunk_left) {
		SIZE_T want = n > DS_CHUNK ? n : DS_CHUNK;

		p = VirtualAlloc(NULL, want, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!p)
			return NULL;
		/* A request bigger than a chunk keeps its own region and does not
		 * strand the remainder of the current one. */
		if (n > DS_CHUNK)
			return p;
		g_chunk = (unsigned char *)p;
		g_chunk_left = want;
	}
	p = g_chunk;
	g_chunk += n;
	g_chunk_left -= n;
	return p;
}

/* ---------------------------------------------------------------- cursor */

static LONGLONG now_qpc(void)
{
	LARGE_INTEGER t;

	/* The hooked one on purpose. d3d11_sw.c's pacing probe goes out of its way
	 * to reach the real kernel32 counter; this wants the opposite, because a
	 * cursor derived from unrewound time is exactly the bug being removed. */
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

static DWORD cursor(SwBuf *b)
{
	LONGLONG d;
	unsigned long long bytes;

	if (!b->playing || !b->size || !b->bps)
		return b->at;
	d = now_qpc() - b->anchor;
	if (d < 0)
		return b->at;
	bytes = (unsigned long long)d * b->bps / (unsigned long long)g_qpf;
	if (!b->looping && bytes >= b->size) {
		/* One-shots stop at the end, the way a real buffer does, so the game
		 * sees the status change rather than a cursor that wrapped. */
		b->playing = 0;
		b->at = b->size - 1;
		return b->at;
	}
	return (DWORD)((b->at + bytes) % b->size);
}

/* Fold elapsed time into the stored position, so that whatever changes next -
 * frequency, a seek, a stop - starts from a settled number. */
static void settle(SwBuf *b)
{
	b->at = cursor(b);
	b->anchor = now_qpc();
}

/* ---------------------------------------------------------------- buffer */

static const void *g_buf_vt[24];
static const void *g_dev_vt[12];

static HRESULT WINAPI B_QueryInterface(SwBuf *b, const GUID *iid, void **out)
{
	(void)iid;
	if (!out)
		return E_INVALIDARG;
	/* Never called in the whole survey. Handing back self is right for every
	 * buffer interface DxLib could ask for, and there is no other object here
	 * to confuse it with. */
	InterlockedIncrement(&b->ref);
	*out = b;
	return S_OK;
}

static ULONG WINAPI B_AddRef(SwBuf *b)
{
	return (ULONG)InterlockedIncrement(&b->ref);
}

static ULONG WINAPI B_Release(SwBuf *b)
{
	LONG r = InterlockedDecrement(&b->ref);

	/* Nothing is freed. The arena is a bump allocator and reuse is the one
	 * thing that would reintroduce the bug this file exists to remove: an
	 * address that meant one sound at save time and another at restore time is
	 * the entity-pool problem wearing an audio hat. 705 buffers of a few
	 * kilobytes each is a cheap price for addresses that never lie. */
	return (ULONG)(r < 0 ? 0 : r);
}

static HRESULT WINAPI B_GetCaps(SwBuf *b, void *caps)
{
	DWORD *c = (DWORD *)caps;

	if (!c || c[0] < 16)
		return DSERR_INVALIDPARAM;
	c[1] = b->flags;
	c[2] = b->size;
	c[3] = 0;
	return S_OK;
}

static HRESULT WINAPI B_GetCurrentPosition(SwBuf *b, DWORD *play, DWORD *write)
{
	DWORD c;

	EnterCriticalSection(&g_cs);
	c = cursor(b);
	if (play)
		*play = c;
	if (write) {
		/* Hardware keeps the write cursor a little ahead of play, and DxLib
		 * subtracts the two to decide how much room it has. The gap has to be
		 * small and it has to be consistent; what it must never do is make
		 * that unsigned subtraction wrap, which is the negative length this
		 * project chased for weeks. */
		DWORD lead = b->bps / 100; /* about 10 ms, as a card would */

		if (!b->size)
			*write = c;
		else {
			if (lead >= b->size)
				lead = b->size / 2;
			*write = (DWORD)((c + lead) % b->size);
		}
	}
	LeaveCriticalSection(&g_cs);
	return S_OK;
}

static HRESULT WINAPI B_GetFormat(SwBuf *b, WAVEFORMATEX *f, DWORD cap, DWORD *got)
{
	WAVEFORMATEX w;

	w.wFormatTag = WAVE_FORMAT_PCM;
	w.nChannels = b->channels;
	w.nSamplesPerSec = b->nominal_hz;
	w.wBitsPerSample = b->bits;
	w.nBlockAlign = (WORD)(b->channels * (b->bits / 8));
	w.nAvgBytesPerSec = b->nominal_hz * w.nBlockAlign;
	w.cbSize = 0;
	if (got)
		*got = sizeof(w);
	if (f && cap >= sizeof(w))
		*f = w;
	return S_OK;
}

static HRESULT WINAPI B_GetVolume(SwBuf *b, LONG *v)
{
	if (v)
		*v = b->volume;
	return S_OK;
}

static HRESULT WINAPI B_GetPan(SwBuf *b, LONG *p)
{
	if (p)
		*p = b->pan;
	return S_OK;
}

static HRESULT WINAPI B_GetFrequency(SwBuf *b, DWORD *f)
{
	if (f)
		*f = b->hz;
	return S_OK;
}

static HRESULT WINAPI B_GetStatus(SwBuf *b, DWORD *s)
{
	if (!s)
		return E_INVALIDARG;
	EnterCriticalSection(&g_cs);
	cursor(b); /* may retire a finished one-shot */
	*s = b->playing ? (DWORD)(DSBSTATUS_PLAYING |
				  (b->looping ? DSBSTATUS_LOOPING : 0))
			: 0;
	LeaveCriticalSection(&g_cs);
	return S_OK;
}

static HRESULT WINAPI B_Initialize(SwBuf *b, void *dev, const void *desc)
{
	(void)b;
	(void)dev;
	(void)desc;
	return S_OK;
}

static HRESULT WINAPI B_Lock(SwBuf *b, DWORD off, DWORD bytes, void **p1, DWORD *b1,
			     void **p2, DWORD *b2, DWORD flags)
{
	DWORD first;

	/* Cannot fail, and that is a feature rather than laziness. DxLib does not
	 * check this HRESULT: DxSound.cpp locks into a pair of stack locals and
	 * feeds decoded PCM through them regardless, so a failed Lock is a write
	 * through whatever the stack was holding. That is how rabiribi.exe+6E9F8
	 * came to memcpy 344 bytes over a string constant in .rdata. Windows'
	 * DirectSound can fail a lock for reasons that have nothing to do with us
	 * - a lost buffer, a device change. Ours has no such reasons. */
	EnterCriticalSection(&g_cs);
	if (flags & DSBLOCK_ENTIREBUFFER) {
		off = 0;
		bytes = b->size;
	}
	if (flags & DSBLOCK_FROMWRITECURSOR) {
		DWORD lead = b->bps / 100;

		if (b->size) {
			if (lead >= b->size)
				lead = b->size / 2;
			off = (DWORD)((cursor(b) + lead) % b->size);
		}
	}
	if (!b->size) {
		if (p1)
			*p1 = NULL;
		if (b1)
			*b1 = 0;
		if (p2)
			*p2 = NULL;
		if (b2)
			*b2 = 0;
		LeaveCriticalSection(&g_cs);
		return S_OK;
	}
	off %= b->size;
	if (bytes > b->size)
		bytes = b->size;
	first = b->size - off;
	if (first > bytes)
		first = bytes;
	if (p1)
		*p1 = b->pcm + off;
	if (b1)
		*b1 = first;
	if (p2)
		*p2 = bytes > first ? b->pcm : NULL;
	if (b2)
		*b2 = bytes - first;
	LeaveCriticalSection(&g_cs);
	return S_OK;
}

static HRESULT WINAPI B_Play(SwBuf *b, DWORD res, DWORD pri, DWORD flags)
{
	(void)res;
	(void)pri;
	EnterCriticalSection(&g_cs);
	b->looping = (flags & DSBPLAY_LOOPING) ? 1 : 0;
	if (!b->playing) {
		b->anchor = now_qpc();
		b->playing = 1;
	}
	LeaveCriticalSection(&g_cs);
	return S_OK;
}

static HRESULT WINAPI B_SetCurrentPosition(SwBuf *b, DWORD pos)
{
	EnterCriticalSection(&g_cs);
	b->at = b->size ? pos % b->size : 0;
	b->anchor = now_qpc();
	LeaveCriticalSection(&g_cs);
	return S_OK;
}

static HRESULT WINAPI B_SetFormat(SwBuf *b, const WAVEFORMATEX *f)
{
	/* Only legal on the primary buffer, which here is a bookkeeping object
	 * with no memory behind it. Accepting it keeps the cooperative-level dance
	 * DxLib does at startup uneventful. */
	if (f && b->primary) {
		b->channels = f->nChannels ? f->nChannels : 2;
		b->bits = f->wBitsPerSample ? f->wBitsPerSample : 16;
		b->nominal_hz = f->nSamplesPerSec ? f->nSamplesPerSec : 44100;
	}
	return S_OK;
}

static HRESULT WINAPI B_SetVolume(SwBuf *b, LONG v)
{
	b->volume = v;
	return S_OK;
}

static HRESULT WINAPI B_SetPan(SwBuf *b, LONG p)
{
	b->pan = p;
	return S_OK;
}

static HRESULT WINAPI B_SetFrequency(SwBuf *b, DWORD hz)
{
	EnterCriticalSection(&g_cs);
	/* Settle first: the bytes already played were played at the old rate, and
	 * changing the rate without folding them in would retroactively move the
	 * cursor. Ten thousand calls a session, so this matters. */
	settle(b);
	b->hz = hz == DSBFREQUENCY_ORIGINAL ? b->nominal_hz : hz;
	b->bps = b->hz * b->channels * (b->bits / 8);
	LeaveCriticalSection(&g_cs);
	return S_OK;
}

static HRESULT WINAPI B_Stop(SwBuf *b)
{
	EnterCriticalSection(&g_cs);
	settle(b);
	b->playing = 0;
	LeaveCriticalSection(&g_cs);
	return S_OK;
}

static HRESULT WINAPI B_Unlock(SwBuf *b, void *p1, DWORD b1, void *p2, DWORD b2)
{
	(void)b;
	(void)p1;
	(void)b1;
	(void)p2;
	(void)b2;
	return S_OK;
}

static HRESULT WINAPI B_Restore(SwBuf *b)
{
	/* Our buffers are never lost. There is no device to lose them to. */
	(void)b;
	return S_OK;
}

/* ---------------------------------------------------------------- device */

static HRESULT WINAPI D_QueryInterface(SwDev *d, const GUID *iid, void **out)
{
	(void)iid;
	if (!out)
		return E_INVALIDARG;
	InterlockedIncrement(&d->ref);
	*out = d;
	return S_OK;
}

static ULONG WINAPI D_AddRef(SwDev *d)
{
	return (ULONG)InterlockedIncrement(&d->ref);
}

static ULONG WINAPI D_Release(SwDev *d)
{
	LONG r = InterlockedDecrement(&d->ref);

	return (ULONG)(r < 0 ? 0 : r);
}

static HRESULT WINAPI D_CreateSoundBuffer(SwDev *d, const DSBUFFERDESC_SW *desc, void **out,
					  void *outer)
{
	SwBuf *b;
	const WAVEFORMATEX *f;

	(void)outer;
	if (!out || !desc)
		return DSERR_INVALIDPARAM;
	*out = NULL;
	EnterCriticalSection(&g_cs);
	b = (SwBuf *)arena_alloc(sizeof(*b));
	if (!b) {
		LeaveCriticalSection(&g_cs);
		return E_OUTOFMEMORY;
	}
	ZeroMemory(b, sizeof(*b));
	b->vtbl = g_buf_vt;
	b->ref = 1;
	b->dev = d;
	b->flags = desc->dwFlags;
	b->primary = (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) ? 1 : 0;
	f = desc->lpwfxFormat;
	b->channels = (f && f->nChannels) ? f->nChannels : 2;
	b->bits = (f && f->wBitsPerSample) ? f->wBitsPerSample : 16;
	b->nominal_hz = (f && f->nSamplesPerSec) ? f->nSamplesPerSec : 44100;
	b->hz = b->nominal_hz;
	b->bps = b->hz * b->channels * (b->bits / 8);
	b->volume = 0;
	b->pan = 0;
	/* The primary buffer is a handle to the output format, not a ring. DxLib
	 * creates one, sets a cooperative level against it and never locks it. */
	if (!b->primary && desc->dwBufferBytes) {
		b->size = desc->dwBufferBytes;
		b->pcm = (unsigned char *)arena_alloc(b->size);
		if (!b->pcm) {
			LeaveCriticalSection(&g_cs);
			return E_OUTOFMEMORY;
		}
		ZeroMemory(b->pcm, b->size);
		g_bufs++;
		g_bytes += b->size;
	}
	LeaveCriticalSection(&g_cs);
	*out = b;
	return S_OK;
}

static HRESULT WINAPI D_GetCaps(SwDev *d, void *caps)
{
	DWORD *c = (DWORD *)caps;

	(void)d;
	if (!c || c[0] < 8)
		return DSERR_INVALIDPARAM;
	c[1] = 0;
	return S_OK;
}

static HRESULT WINAPI D_DuplicateSoundBuffer(SwDev *d, SwBuf *src, void **out)
{
	SwBuf *b;

	/* Zero calls across 705 buffers in the survey, so this is correctness
	 * rather than need. A duplicate shares the sample data and gets its own
	 * cursor, which is what DirectSound documents. */
	if (!out || !src)
		return DSERR_INVALIDPARAM;
	EnterCriticalSection(&g_cs);
	b = (SwBuf *)arena_alloc(sizeof(*b));
	if (!b) {
		LeaveCriticalSection(&g_cs);
		return E_OUTOFMEMORY;
	}
	*b = *src;
	b->ref = 1;
	b->dev = d;
	b->playing = 0;
	b->at = 0;
	b->anchor = now_qpc();
	LeaveCriticalSection(&g_cs);
	*out = b;
	return S_OK;
}

static HRESULT WINAPI D_SetCooperativeLevel(SwDev *d, HWND w, DWORD level)
{
	(void)w;
	d->coop = level;
	return S_OK;
}

static HRESULT WINAPI D_Compact(SwDev *d)
{
	(void)d;
	return S_OK;
}

static HRESULT WINAPI D_GetSpeakerConfig(SwDev *d, DWORD *cfg)
{
	(void)d;
	if (cfg)
		*cfg = 0x00000006; /* DSSPEAKER_STEREO */
	return S_OK;
}

static HRESULT WINAPI D_SetSpeakerConfig(SwDev *d, DWORD cfg)
{
	(void)d;
	(void)cfg;
	return S_OK;
}

static HRESULT WINAPI D_Initialize(SwDev *d, const GUID *g)
{
	(void)d;
	(void)g;
	return S_OK;
}

static HRESULT WINAPI D_VerifyCertification(SwDev *d, DWORD *cert)
{
	(void)d;
	if (cert)
		*cert = 0;
	return S_OK;
}

/* ---------------------------------------------------------------- entry */

static void vt_init(void)
{
	g_dev_vt[0] = (const void *)D_QueryInterface;
	g_dev_vt[1] = (const void *)D_AddRef;
	g_dev_vt[2] = (const void *)D_Release;
	g_dev_vt[3] = (const void *)D_CreateSoundBuffer;
	g_dev_vt[4] = (const void *)D_GetCaps;
	g_dev_vt[5] = (const void *)D_DuplicateSoundBuffer;
	g_dev_vt[6] = (const void *)D_SetCooperativeLevel;
	g_dev_vt[7] = (const void *)D_Compact;
	g_dev_vt[8] = (const void *)D_GetSpeakerConfig;
	g_dev_vt[9] = (const void *)D_SetSpeakerConfig;
	g_dev_vt[10] = (const void *)D_Initialize;
	g_dev_vt[11] = (const void *)D_VerifyCertification;

	/* Slot numbers are the ones dsoundhook.c already hooks by index, verified
	 * against a live device: Lock is 11, Play 12, Stop 18, Unlock 19. */
	g_buf_vt[0] = (const void *)B_QueryInterface;
	g_buf_vt[1] = (const void *)B_AddRef;
	g_buf_vt[2] = (const void *)B_Release;
	g_buf_vt[3] = (const void *)B_GetCaps;
	g_buf_vt[4] = (const void *)B_GetCurrentPosition;
	g_buf_vt[5] = (const void *)B_GetFormat;
	g_buf_vt[6] = (const void *)B_GetVolume;
	g_buf_vt[7] = (const void *)B_GetPan;
	g_buf_vt[8] = (const void *)B_GetFrequency;
	g_buf_vt[9] = (const void *)B_GetStatus;
	g_buf_vt[10] = (const void *)B_Initialize;
	g_buf_vt[11] = (const void *)B_Lock;
	g_buf_vt[12] = (const void *)B_Play;
	g_buf_vt[13] = (const void *)B_SetCurrentPosition;
	g_buf_vt[14] = (const void *)B_SetFormat;
	g_buf_vt[15] = (const void *)B_SetVolume;
	g_buf_vt[16] = (const void *)B_SetPan;
	g_buf_vt[17] = (const void *)B_SetFrequency;
	g_buf_vt[18] = (const void *)B_Stop;
	g_buf_vt[19] = (const void *)B_Unlock;
	g_buf_vt[20] = (const void *)B_Restore;
	g_buf_vt[21] = (const void *)B_Restore; /* DS8: SetFX */
	g_buf_vt[22] = (const void *)B_Restore; /* DS8: AcquireResources */
	g_buf_vt[23] = (const void *)B_Restore; /* DS8: GetObjectInPath */
}

int ds_sw_armed(void)
{
	return g_ready;
}

void ds_sw_report(void)
{
	if (!g_ready)
		return;
	ss_log("ds_sw: %lu software buffer(s) holding %lu KB, all of it ordinary "
	       "private memory that travels with the snapshot. No dsound.dll, no "
	       "AUDIOSES, no system audio threads, no cursor moving while we copy\n",
	       g_bufs, (unsigned long)(g_bytes / 1024));
}

__declspec(dllexport) HRESULT WINAPI ds_sw_create(const GUID *dev, void **out, void *outer)
{
	SwDev *d;
	LARGE_INTEGER f;

	(void)dev;
	(void)outer;
	if (!out)
		return DSERR_INVALIDPARAM;
	*out = NULL;
	if (!g_ready) {
		InitializeCriticalSection(&g_cs);
		savestate_own_cs(&g_cs);
		QueryPerformanceFrequency(&f);
		g_qpf = f.QuadPart ? f.QuadPart : 1;
		vt_init();
		g_ready = 1;
		ss_log("ds_sw: the software DirectSound is answering instead of "
		       "Windows'. Cursors are integers derived from the rewound clock "
		       "and live in captured memory, so a restore puts the play "
		       "position and the write position back from the same moment\n");
	}
	d = (SwDev *)arena_alloc(sizeof(*d));
	if (!d)
		return E_OUTOFMEMORY;
	ZeroMemory(d, sizeof(*d));
	d->vtbl = g_dev_vt;
	d->ref = 1;
	*out = d;
	return S_OK;
}

/* ---------------------------------------------------------------- install */

/* Winning the name was the clean way and it is not available. Steam has
 * C:\WINDOWS\System32\dsound.dll in the process before our DLL attaches, and a
 * plain LoadLibrary name matches an already-loaded module before any directory
 * is searched, so the same-folder stub never gets asked. The mechanism was
 * never at fault - a probe run from the game folder does get our copy - we were
 * simply second.
 *
 * So take the entry point instead of the file. DirectSoundCreate8 in the loaded
 * module gets a jump to ds_sw_create, and whoever calls it gets a software
 * device no matter which dsound.dll won the loader's argument. Nothing of
 * Windows' DirectSound runs after that: no device, no buffers, no mixer thread,
 * and nothing of theirs holding a pointer into memory we rewind.
 *
 * The original is never called, so there is no trampoline to build - the five
 * bytes are simply gone. Both signatures are stdcall with three arguments and
 * the same meaning, so ds_sw_create can stand in for either directly. */
static int g_patched;

__declspec(dllexport) HRESULT WINAPI ds_sw_create(const GUID *dev, void **out, void *outer);

static int jmp_patch(void *at, void *to)
{
	unsigned char *p = (unsigned char *)at;
	DWORD old;
	INT_PTR rel;

	if (!p || !to)
		return 0;
	rel = (INT_PTR)((unsigned char *)to - (p + 5));
	if (rel != (INT_PTR)(int)rel)
		return 0; /* out of rel32 reach; would need a longer form */
	if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old))
		return 0;
	p[0] = 0xE9;
	*(int *)(p + 1) = (int)rel;
	VirtualProtect(p, 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), p, 5);
	return 1;
}

int ds_sw_hooked(void)
{
	return g_patched || g_ready;
}

/* Returns how many entry points were taken. Called from DLL attach, which is
 * the earliest moment we exist and, more to the point, earlier than DxLib's
 * sound init - the survey's 705 buffer creations were all seen by hooks armed
 * from the same place. */
int ds_sw_take_over(void)
{
	HMODULE ds;
	void *f;
	int n = 0;

	if (g_patched)
		return g_patched;
	ds = GetModuleHandleA("dsound.dll");
	if (!ds)
		return 0;
	f = (void *)GetProcAddress(ds, "DirectSoundCreate8");
	if (f && jmp_patch(f, (void *)ds_sw_create))
		n++;
	f = (void *)GetProcAddress(ds, "DirectSoundCreate");
	if (f && jmp_patch(f, (void *)ds_sw_create))
		n++;
	g_patched = n;
	return n;
}

/* DxLib calls neither of these in the survey, but a device enumeration that
 * returns nothing is a legitimate answer that some callers handle badly, so
 * report one default device. */
__declspec(dllexport) HRESULT WINAPI ds_sw_enum_a(void *cb, void *ctx)
{
	BOOL(CALLBACK * fn)(GUID *, const char *, const char *, void *) =
		(BOOL(CALLBACK *)(GUID *, const char *, const char *, void *))cb;

	if (fn)
		fn(NULL, "Primary Sound Driver", "", ctx);
	return S_OK;
}

__declspec(dllexport) HRESULT WINAPI ds_sw_enum_w(void *cb, void *ctx)
{
	BOOL(CALLBACK * fn)(GUID *, const wchar_t *, const wchar_t *, void *) =
		(BOOL(CALLBACK *)(GUID *, const wchar_t *, const wchar_t *, void *))cb;

	if (fn)
		fn(NULL, L"Primary Sound Driver", L"", ctx);
	return S_OK;
}
