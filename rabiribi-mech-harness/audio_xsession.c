/*
 * audio_xsession - cross-session XAudio2 logical state restore (Wine, headless VM).
 *
 * Uses IXAudio2 engine only (XAudio2Create) — no mastering voice on this VM.
 *
 *   wine audio_xsession.exe save    snap.bin [seed]
 *   wine audio_xsession.exe prove   snap.bin
 *   wine audio_xsession.exe restore snap.bin
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
#define PCM_BYTES (SAMPLES * (unsigned)sizeof(short))

typedef struct SnapHdr {
	char magic[8]; /* "AUDXS1\0" */
	unsigned ver;
	unsigned seed;
	unsigned fp;
	unsigned saved_pid;
	uintptr_t xa2_ptr;
	uintptr_t pcm_ptr;
	unsigned xa2_version;
} SnapHdr;

static short *g_pcm;
static IXAudio2 *g_xa2;

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
		g_pcm[i] = (short)((v >> 16) & 0x7fff);
	}
}

static int setup_xa2(void)
{
	HRESULT hr;
	if (!load_xaudio2()) return 0;
	hr = pXAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
	return SUCCEEDED(hr) && g_xa2;
}

static int ptr_engine_stale(const SnapHdr *hdr)
{
	/* Saved COM pointer must not be the live re-created engine. */
	return hdr->xa2_ptr != 0 && (void *)hdr->xa2_ptr != (void *)g_xa2;
}

static int ptr_pcm_stale_for_restore(const SnapHdr *hdr)
{
	/* Client PCM mapping must be re-filled from seed, not rewound as a raw pointer. */
	if (hdr->pcm_ptr == 0) return 1;
	if ((void *)hdr->pcm_ptr != (void *)g_pcm) return 1;
	/* Same VA can reappear on a new process under Wine; logical state still re-seeded. */
	return hdr->saved_pid != GetCurrentProcessId();
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";
	const char *file = argc > 2 ? argv[2] : "audio_snap.bin";

	g_pcm = (short *)VirtualAlloc(NULL, PCM_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!g_pcm) return 2;

	if (strcmp(cmd, "save") == 0) {
		unsigned seed = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : 12345u;
		SnapHdr hdr;
		FILE *f;
		unsigned fp;

		if (!setup_xa2()) { printf("XAudio2Create failed\n"); return 2; }
		fill_pcm(seed);
		fp = fnv1a(g_pcm, PCM_BYTES);
		memset(&hdr, 0, sizeof(hdr));
		memcpy(hdr.magic, "AUDXS1\0", 8);
		hdr.ver = 1;
		hdr.seed = seed;
		hdr.fp = fp;
		hdr.saved_pid = GetCurrentProcessId();
		hdr.xa2_ptr = (uintptr_t)g_xa2;
		hdr.pcm_ptr = (uintptr_t)g_pcm;
		hdr.xa2_version = (unsigned)g_xa2->lpVtbl->AddRef(g_xa2);
		g_xa2->lpVtbl->Release(g_xa2);
		f = fopen(file, "wb");
		if (!f) { printf("cannot write %s\n", file); return 2; }
		fwrite(&hdr, sizeof(hdr), 1, f);
		fwrite(g_pcm, 1, PCM_BYTES, f);
		fclose(f);
		printf("SAVE pid=%lu seed=%u fp=0x%08x xa2=%p pcm=%p -> %s\n",
		       (unsigned long)GetCurrentProcessId(), seed, fp,
		       (void *)g_xa2, (void *)g_pcm, file);
		return 0;
	}

	if (strcmp(cmd, "prove") == 0 || strcmp(cmd, "restore") == 0) {
		SnapHdr hdr;
		FILE *f;
		unsigned char *saved_pcm = (unsigned char *)malloc(PCM_BYTES);
		unsigned fp, diff = 0, i;
		int engine_stale, pcm_stale;

		if (!saved_pcm) return 2;
		f = fopen(file, "rb");
		if (!f) { printf("cannot read %s\n", file); free(saved_pcm); return 2; }
		if (fread(&hdr, sizeof(hdr), 1, f) != 1 || memcmp(hdr.magic, "AUDXS1\0", 8) != 0) {
			printf("bad snapshot\n"); fclose(f); free(saved_pcm); return 2;
		}
		if (fread(saved_pcm, 1, PCM_BYTES, f) != PCM_BYTES) {
			printf("short snapshot\n"); fclose(f); free(saved_pcm); return 2;
		}
		fclose(f);

		if (!setup_xa2()) { printf("XAudio2Create failed\n"); free(saved_pcm); return 2; }

		engine_stale = ptr_engine_stale(&hdr);
		pcm_stale = ptr_pcm_stale_for_restore(&hdr);

		printf("PROVE pid=%lu saved xa2=%p pcm=%p (saved pid %u)\n",
		       (unsigned long)GetCurrentProcessId(),
		       (void *)hdr.xa2_ptr, (void *)hdr.pcm_ptr, hdr.saved_pid);
		printf("      live  xa2=%p pcm=%p (pid %lu)\n",
		       (void *)g_xa2, (void *)g_pcm, (unsigned long)GetCurrentProcessId());
		printf("      engine recreated (new COM)=%s  pcm needs re-seed=%s\n",
		       engine_stale ? "YES" : "no", pcm_stale ? "YES" : "no");

		if (strcmp(cmd, "prove") == 0) {
			int ok = engine_stale;
			printf("%s: cross-session %s reuse saved IXAudio2 pointer\n", ok ? "PASS" : "FAIL",
			       ok ? "cannot" : "incorrectly can");
			free(saved_pcm);
			return ok ? 0 : 1;
		}

		fill_pcm(hdr.seed);
		fp = fnv1a(g_pcm, PCM_BYTES);
		for (i = 0; i < PCM_BYTES; i++)
			if (((unsigned char *)g_pcm)[i] != saved_pcm[i]) diff++;

		printf("RESTORE seed=%u snapshot_fp=0x%08x reproduced_fp=0x%08x fp_match=%s byte_diffs=%u/%u\n",
		       hdr.seed, hdr.fp, fp, (fp == hdr.fp) ? "YES" : "NO", diff, PCM_BYTES);
		{
			int ok = engine_stale && (fp == hdr.fp) && (diff == 0);
			printf("%s: cross-session restore %s (new IXAudio2 + seed PCM)\n", ok ? "PASS" : "FAIL",
			       ok ? "bit-exact" : "failed");
			free(saved_pcm);
			return ok ? 0 : 1;
		}
	}

	if (strcmp(cmd, "corrupt") == 0) {
		unsigned char *buf = (unsigned char *)malloc(sizeof(SnapHdr) + PCM_BYTES);
		FILE *f;
		if (!buf) return 2;
		f = fopen(file, "rb+");
		if (!f) { free(buf); return 2; }
		if (fread(buf, 1, sizeof(SnapHdr) + PCM_BYTES, f) != sizeof(SnapHdr) + PCM_BYTES) {
			fclose(f); free(buf); return 2;
		}
		buf[sizeof(SnapHdr) + 17] ^= 0x01;
		fseek(f, 0, SEEK_SET);
		fwrite(buf, 1, sizeof(SnapHdr) + PCM_BYTES, f);
		fclose(f);
		free(buf);
		printf("corrupted 1 byte in %s\n", file);
		return 0;
	}

	printf("usage: %s save|prove|restore|corrupt <file> [seed]\n", argv[0]);
	return 2;
}
