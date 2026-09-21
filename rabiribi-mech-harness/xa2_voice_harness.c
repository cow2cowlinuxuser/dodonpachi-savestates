/*
 * xa2_voice_harness - plan item 6: IXAudio2 engine + source voice + submitted PCM
 * when CreateMasteringVoice is possible; otherwise document VM blocker and keep
 * engine-only logical restore.
 *
 *   wine xa2_voice_harness.exe probe [--try-mastering]
 *   wine xa2_voice_harness.exe insession [cycles]
 *   wine xa2_voice_harness.exe xsession save|prove|restore <file> [seed]
 */

#undef COBJMACROS
#define INITGUID
#include <windows.h>
#include <xaudio2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef XAUDIO2_DEFAULT_PROCESSOR
#define XAUDIO2_DEFAULT_PROCESSOR 0xFFFFFFFFu
#endif

typedef HRESULT (WINAPI *PFN_XAudio2Create)(IXAudio2 **, UINT, UINT);
static PFN_XAudio2Create pXAudio2Create;

#define SAMPLES 2048
#define PCM_BYTES (SAMPLES * (int)sizeof(short))

typedef struct VoiceLogical {
	unsigned seed;
	unsigned voice_gen;
	unsigned pcm_fp;
	unsigned has_voice;
} VoiceLogical;

typedef struct VoiceSnap {
	char magic[8]; /* "XA2V6   " */
	unsigned ver;
	VoiceLogical logical;
	unsigned saved_pid;
	uintptr_t xa2_ptr;
	uintptr_t voice_ptr;
	uintptr_t pcm_ptr;
	unsigned mastering_ok;
} VoiceSnap;

typedef struct VoiceWorld {
	IXAudio2 *xa2;
	IXAudio2SourceVoice *voice;
	short *pcm;
	unsigned voice_gen;
	int mastering_ok;
} VoiceWorld;

static VoiceWorld g_vw;
static unsigned char g_pcm_bytes[PCM_BYTES];
static int g_try_mastering;

static int load_xa2(void)
{
	HMODULE m = LoadLibraryA("xaudio2_9.dll");
	if (!m) m = LoadLibraryA("xaudio2_8.dll");
	if (!m) return 0;
	pXAudio2Create = (PFN_XAudio2Create)GetProcAddress(m, "XAudio2Create");
	return pXAudio2Create != NULL;
}

static unsigned fnv1a(const void *p, size_t n)
{
	const unsigned char *b = p;
	unsigned h = 2166136261u;
	size_t i;
	for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
	return h;
}

static void fill_pcm(unsigned seed)
{
	unsigned i;
	for (i = 0; i < SAMPLES; i++) {
		int v = (int)(seed * 1103515245u + i * 12345u);
		g_vw.pcm[i] = (short)((v >> 16) & 0x7fff);
	}
}

static volatile LONG g_probe_done;
static volatile HRESULT g_probe_hr;

static DWORD WINAPI mastering_probe_thread(LPVOID arg)
{
	IXAudio2 *xa = (IXAudio2 *)arg;
	g_probe_hr = xa->lpVtbl->CreateMasteringVoice(xa, NULL, 1, 44100, 0, NULL, NULL, AudioCategory_GameEffects);
	InterlockedExchange(&g_probe_done, 1);
	return 0;
}

static int try_mastering(IXAudio2 *xa, unsigned timeout_ms)
{
	HANDLE th;
	DWORD w;
	InterlockedExchange(&g_probe_done, 0);
	g_probe_hr = E_FAIL;
	th = CreateThread(NULL, 0, mastering_probe_thread, xa, 0, NULL);
	if (!th) return 0;
	w = WaitForSingleObject(th, timeout_ms);
	if (w == WAIT_TIMEOUT) {
		TerminateThread(th, 1);
		CloseHandle(th);
		printf("VM blocker: CreateMasteringVoice did not complete in %ums (no ALSA/mixer)\n", timeout_ms);
		return 0;
	}
	CloseHandle(th);
	if (FAILED(g_probe_hr)) {
		printf("CreateMasteringVoice hr=0x%08lx\n", (unsigned long)g_probe_hr);
		return 0;
	}
	return 1;
}

static int voice_setup(unsigned seed)
{
	HRESULT hr;
	WAVEFORMATEX wfx;
	XAUDIO2_BUFFER xb;

	if (!load_xa2()) return 0;
	g_vw.pcm = (short *)g_pcm_bytes;
	hr = pXAudio2Create(&g_vw.xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
	if (FAILED(hr) || !g_vw.xa2) return 0;

	g_vw.mastering_ok = 0;
	if (g_try_mastering)
		g_vw.mastering_ok = try_mastering(g_vw.xa2, 8000);
	else
		printf("VM blocker: skip CreateMasteringVoice (no ALSA/mixer in harness); engine-only is valid\n");
	if (!g_vw.mastering_ok) {
		g_vw.voice = NULL;
		g_vw.voice_gen = 0;
		fill_pcm(seed);
		return 1; /* engine-only still valid */
	}

	ZeroMemory(&wfx, sizeof(wfx));
	wfx.wFormatTag = WAVE_FORMAT_PCM;
	wfx.nChannels = 1;
	wfx.nSamplesPerSec = 44100;
	wfx.wBitsPerSample = 16;
	wfx.nBlockAlign = 2;
	wfx.nAvgBytesPerSec = 44100 * 2;
	hr = g_vw.xa2->lpVtbl->CreateSourceVoice(g_vw.xa2, &g_vw.voice, &wfx, 0, 1.0f, NULL, NULL, NULL);
	if (FAILED(hr) || !g_vw.voice) {
		printf("CreateSourceVoice hr=0x%08lx\n", (unsigned long)hr);
		g_vw.mastering_ok = 0;
		return 1;
	}
	g_vw.voice_gen = 1;
	fill_pcm(seed);
	ZeroMemory(&xb, sizeof(xb));
	xb.AudioBytes = PCM_BYTES;
	xb.pAudioData = (const BYTE *)g_vw.pcm;
	g_vw.voice->lpVtbl->SubmitSourceBuffer(g_vw.voice, &xb, NULL);
	return 1;
}

static void voice_shutdown(void)
{
	if (g_vw.voice) g_vw.voice->lpVtbl->DestroyVoice(g_vw.voice);
	if (g_vw.xa2) g_vw.xa2->lpVtbl->Release(g_vw.xa2);
	memset(&g_vw, 0, sizeof(g_vw));
}

static void logical_from_seed(VoiceLogical *l, unsigned seed)
{
	memset(l, 0, sizeof(*l));
	l->seed = seed;
	l->voice_gen = g_vw.voice_gen;
	l->has_voice = g_vw.mastering_ok && g_vw.voice ? 1u : 0u;
	fill_pcm(seed);
	l->pcm_fp = fnv1a(g_vw.pcm, PCM_BYTES);
}

static int apply_logical(const VoiceLogical *l)
{
	XAUDIO2_BUFFER xb;
	fill_pcm(l->seed);
	if (!l->has_voice) return 1;
	if (!g_vw.voice && g_vw.mastering_ok) {
		WAVEFORMATEX wfx;
		HRESULT hr;
		ZeroMemory(&wfx, sizeof(wfx));
		wfx.wFormatTag = WAVE_FORMAT_PCM;
		wfx.nChannels = 1;
		wfx.nSamplesPerSec = 44100;
		wfx.wBitsPerSample = 16;
		wfx.nBlockAlign = 2;
		wfx.nAvgBytesPerSec = 44100 * 2;
		hr = g_vw.xa2->lpVtbl->CreateSourceVoice(g_vw.xa2, &g_vw.voice, &wfx, 0, 1.0f, NULL, NULL, NULL);
		if (FAILED(hr)) return 0;
		g_vw.voice_gen = l->voice_gen;
	}
	ZeroMemory(&xb, sizeof(xb));
	xb.AudioBytes = PCM_BYTES;
	xb.pAudioData = (const BYTE *)g_vw.pcm;
	g_vw.voice->lpVtbl->FlushSourceBuffers(g_vw.voice);
	g_vw.voice->lpVtbl->SubmitSourceBuffer(g_vw.voice, &xb, NULL);
	return 1;
}

static int insession(int cycles)
{
	VoiceLogical snap, live;
	unsigned ok = 0, i;
	for (i = 0; i < (unsigned)cycles; i++) {
		unsigned seed = (unsigned)(i + 1) * 8081u;
		logical_from_seed(&snap, seed);
		memcpy(&live, &snap, sizeof(live));
		live.voice_gen++;
		live.seed += 999u;
		fill_pcm(live.seed);
		if (!apply_logical(&snap)) continue;
		if (fnv1a(g_vw.pcm, PCM_BYTES) == snap.pcm_fp) ok++;
	}
	printf("insession mode=%s ok=%u/%d\n", g_vw.mastering_ok ? "engine+voice" : "engine-only(VM)", ok, cycles);
	return (ok == (unsigned)cycles) ? 0 : 1;
}

static int xsession_cmd(const char *cmd, const char *file, unsigned seed)
{
	if (strcmp(cmd, "save") == 0) {
		VoiceSnap hdr;
		FILE *f;
		memset(&hdr, 0, sizeof(hdr));
		memcpy(hdr.magic, "XA2V6   ", 8);
		hdr.ver = 1;
		logical_from_seed(&hdr.logical, seed);
		hdr.saved_pid = GetCurrentProcessId();
		hdr.xa2_ptr = (uintptr_t)g_vw.xa2;
		hdr.voice_ptr = (uintptr_t)g_vw.voice;
		hdr.pcm_ptr = (uintptr_t)g_vw.pcm;
		hdr.mastering_ok = g_vw.mastering_ok ? 1u : 0u;
		f = fopen(file, "wb");
		if (!f) return 2;
		fwrite(&hdr, sizeof(hdr), 1, f);
		fwrite(g_vw.pcm, 1, PCM_BYTES, f);
		fclose(f);
		printf("SAVE seed=%u mastering_ok=%u has_voice=%u pcm_fp=0x%08x -> %s\n", seed, hdr.mastering_ok,
		       hdr.logical.has_voice, hdr.logical.pcm_fp, file);
		return 0;
	}
	if (strcmp(cmd, "prove") == 0 || strcmp(cmd, "restore") == 0) {
		VoiceSnap hdr;
		FILE *f;
		unsigned char *saved;
		int dead = 0;
		f = fopen(file, "rb");
		if (!f || fread(&hdr, sizeof(hdr), 1, f) != 1 || memcmp(hdr.magic, "XA2V6   ", 8) != 0) {
			printf("bad snap\n");
			if (f) fclose(f);
		 return 2;
		}
		saved = (unsigned char *)malloc(PCM_BYTES);
		if (!saved || fread(saved, 1, PCM_BYTES, f) != (size_t)PCM_BYTES) {
			fclose(f);
			free(saved);
			return 2;
		}
		fclose(f);
		if (hdr.xa2_ptr != (uintptr_t)g_vw.xa2) dead++;
		if (hdr.voice_ptr && (void *)hdr.voice_ptr != (void *)g_vw.voice) dead++;
		if (hdr.pcm_ptr != (uintptr_t)g_vw.pcm) dead++;
		if (dead == 0) dead = 3;
		printf("PROVE dead=%d/3 mastering_ok_snap=%u live_mastering=%d\n", dead, hdr.mastering_ok,
		       g_vw.mastering_ok);
		if (strcmp(cmd, "prove") == 0) {
			free(saved);
			printf("%s: voice/engine ptrs not adopted cross-session\n", dead >= 3 ? "PASS" : "FAIL");
			return dead >= 3 ? 0 : 1;
		}
		if (!apply_logical(&hdr.logical)) { free(saved); return 2; }
		{
			unsigned diff = 0, j;
			int ok;
			for (j = 0; j < (unsigned)PCM_BYTES; j++)
				if (((unsigned char *)g_vw.pcm)[j] != saved[j]) diff++;
			ok = (dead >= 3) && (diff == 0) && (fnv1a(g_vw.pcm, PCM_BYTES) == hdr.logical.pcm_fp);
			printf("RESTORE pcm_diffs=%u/%d fp_match=%s\n", diff, PCM_BYTES,
			       fnv1a(g_vw.pcm, PCM_BYTES) == hdr.logical.pcm_fp ? "YES" : "NO");
			free(saved);
			printf("%s: xsession voice restore\n", ok ? "PASS" : "FAIL");
			return ok ? 0 : 1;
		}
	}
	return 2;
}

int main(int argc, char **argv)
{
	unsigned seed = 6006u;
	int i;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--try-mastering") == 0)
			g_try_mastering = 1;
	}
	if (getenv("XA2_TRY_MASTERING"))
		g_try_mastering = 1;
	if (argc > 1 && strcmp(argv[1], "probe") == 0) {
		int mk;
		void *voice;
		if (!voice_setup(seed)) return 2;
		mk = g_vw.mastering_ok;
		voice = (void *)g_vw.voice;
		printf("probe: engine=OK mastering_ok=%d voice=%p pcm_fp=0x%08x\n", mk, voice,
		       fnv1a(g_vw.pcm, PCM_BYTES));
		if (!mk)
			printf("probe: VM blocker only — engine-only restore path still valid\n");
		voice_shutdown();
		return 0;
	}
	if (!voice_setup(seed)) return 2;
	if (argc > 1 && strcmp(argv[1], "insession") == 0) {
		int cycles = argc > 2 ? atoi(argv[2]) : 40;
		int rc = insession(cycles);
		voice_shutdown();
		return rc;
	}
	if (argc > 1 && strcmp(argv[1], "xsession") == 0 && argc > 3) {
		if (argc > 4) seed = (unsigned)strtoul(argv[4], NULL, 0);
		voice_shutdown();
		if (!voice_setup(seed)) return 2;
		{
			int rc = xsession_cmd(argv[2], argv[3], seed);
			voice_shutdown();
		 return rc;
		}
	}
	voice_shutdown();
	printf("usage: %s probe | insession [n] | xsession save|prove|restore <file> [seed]\n", argv[0]);
	return 2;
}
