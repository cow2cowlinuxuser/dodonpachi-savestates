/*
 * audio_probe - can we instantiate COM audio objects under Wine in this VM?
 *
 * Tries, in order of interest for the savestate map:
 *   1. XAudio2Create (xaudio2_8.dll / xaudio2_9.dll)
 *   2. CoCreateInstance MMDeviceEnumerator (mmdevapi.dll)
 *   3. DirectSoundCreate8 (dsound.dll)
 *
 *   wine audio_probe.exe
 */

#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

/* XAudio2 - minimal declarations */
typedef struct IXAudio2 IXAudio2;
typedef HRESULT (WINAPI *PFN_XAudio2Create)(IXAudio2 **, UINT, UINT);
#ifndef XAUDIO2_DEFAULT_PROCESSOR
#define XAUDIO2_DEFAULT_PROCESSOR 0xFFFFFFFFu
#endif

/* MMDevice API */
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

/* DirectSound */
#include <dsound.h>

static void try_xaudio2(void)
{
	HMODULE mod;
	PFN_XAudio2Create fn;
	IXAudio2 *xa = NULL;
	HRESULT hr;
	const char *names[] = { "xaudio2_9.dll", "xaudio2_8.dll", "XAudio2_9.dll", "XAudio2_8.dll" };
	int i;

	printf("\n--- XAudio2Create ---\n");
	for (i = 0; i < 4; i++) {
		mod = LoadLibraryA(names[i]);
		if (!mod) {
			printf("  LoadLibrary(%s): absent (err=%lu)\n", names[i], GetLastError());
			continue;
		}
		fn = (PFN_XAudio2Create)GetProcAddress(mod, "XAudio2Create");
		if (!fn) {
			printf("  %s: loaded but no XAudio2Create export\n", names[i]);
			continue;
		}
		hr = fn(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
		printf("  %s XAudio2Create -> hr=0x%08lx ptr=%p\n", names[i], (unsigned long)hr, (void *)xa);
		if (SUCCEEDED(hr) && xa) {
			/* IXAudio2 is COM - release via vtable if we had full headers; leak is fine for probe */
			printf("  OK: XAudio2 engine instantiated\n");
			return;
		}
	}
	printf("  FAIL: no working XAudio2Create\n");
}

static void try_mmdev(void)
{
	IMMDeviceEnumerator *enumr = NULL;
	HRESULT hr;

	printf("\n--- MMDeviceEnumerator (CoCreateInstance) ---\n");
	hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	printf("  CoInitializeEx -> hr=0x%08lx\n", (unsigned long)hr);
	hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
			      &IID_IMMDeviceEnumerator, (void **)&enumr);
	printf("  CoCreateInstance(MMDeviceEnumerator) -> hr=0x%08lx ptr=%p\n",
	       (unsigned long)hr, (void *)enumr);
	if (SUCCEEDED(hr) && enumr) {
		IMMDevice *def = NULL;
		hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumr, eRender, eMultimedia, &def);
		printf("  GetDefaultAudioEndpoint(render) -> hr=0x%08lx ptr=%p\n",
		       (unsigned long)hr, (void *)def);
		if (def) IMMDevice_Release(def);
		IMMDeviceEnumerator_Release(enumr);
		printf("  OK: mmdevapi COM path works\n");
	} else {
		printf("  FAIL: MMDeviceEnumerator\n");
	}
	CoUninitialize();
}

static void try_dsound(void)
{
	IDirectSound8 *ds = NULL;
	HRESULT hr;

	printf("\n--- DirectSoundCreate8 ---\n");
	hr = DirectSoundCreate8(NULL, &ds, NULL);
	printf("  DirectSoundCreate8 -> hr=0x%08lx ptr=%p\n", (unsigned long)hr, (void *)ds);
	if (SUCCEEDED(hr) && ds) {
		DSBUFFERDESC bd;
		IDirectSoundBuffer8 *buf = NULL;
		WAVEFORMATEX wfx;
		(void)wfx;
		ZeroMemory(&bd, sizeof(bd));
		bd.dwSize = sizeof(bd);
		bd.dwFlags = DSBCAPS_PRIMARYBUFFER;
		hr = IDirectSound8_CreateSoundBuffer(ds, &bd, (IDirectSoundBuffer **)&buf, NULL);
		printf("  CreateSoundBuffer(primary) -> hr=0x%08lx ptr=%p\n",
		       (unsigned long)hr, (void *)buf);
		if (buf) IDirectSoundBuffer8_Release(buf);
		IDirectSound8_Release(ds);
		printf("  OK: DirectSound8 instantiated\n");
	} else {
		printf("  FAIL: DirectSoundCreate8\n");
	}
}

int main(void)
{
	printf("audio_probe pid=%lu\n", (unsigned long)GetCurrentProcessId());
	try_xaudio2();
	try_mmdev();
	try_dsound();
	printf("\nprobe done\n");
	return 0;
}
