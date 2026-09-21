/*
 * heap_ownership_harness - plan item 8: process heap + engine heap + held Wine-like heap.
 *
 * Snapshot must exclude handle-page / metadata of heaps we do not own.
 * Restore must not HeapValidate held Wine-like heaps.
 *
 *   wine heap_ownership_harness.exe insession [cycles] [--unsafe-snap] [--validate-wine]
 *   wine heap_ownership_harness.exe xsession save|prove|restore <file> [seed]
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAYLOAD_BYTES 4096u
#define HANDLE_STUB 512u

typedef struct EngineLogical {
	unsigned magic;
	unsigned seed;
	unsigned generation;
	unsigned fp;
} EngineLogical;

typedef struct EngineBlock {
	EngineLogical hdr;
	unsigned char payload[PAYLOAD_BYTES];
} EngineBlock;

typedef struct WinePeer {
	unsigned magic;
	unsigned live_gen;
	unsigned char handle_page_stub[HANDLE_STUB];
} WinePeer;

typedef struct SnapHdr {
	char magic[8]; /* "HEAPXS8 " */
	unsigned ver;
	EngineLogical logical;
	unsigned saved_pid;
	uintptr_t engine_heap;
	uintptr_t engine_block;
	uintptr_t wine_heap;
	uintptr_t wine_peer;
	unsigned included_wine_stub;
} SnapHdr;

static HANDLE g_engine_heap;
static HANDLE g_wine_heap;
static EngineBlock *g_engine;
static WinePeer *g_wine_peer;
static void *g_process_ballast;
static size_t g_process_ballast_bytes;

static unsigned g_unsafe_snap;
static unsigned g_validate_wine;

static unsigned fnv1a(const void *p, size_t n)
{
	const unsigned char *b = p;
	unsigned h = 2166136261u;
	size_t i;
	for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
	return h;
}

static void fill_payload(EngineBlock *e, unsigned seed)
{
	unsigned i;
	for (i = 0; i < PAYLOAD_BYTES; i++)
		e->payload[i] = (unsigned char)((seed * 2654435761u + i * 97u) >> 16);
	e->hdr.seed = seed;
	e->hdr.fp = fnv1a(e->payload, PAYLOAD_BYTES);
}

static int logical_ok(const EngineBlock *e)
{
	return e && e->hdr.magic == 0x454E474Eu && e->hdr.fp == fnv1a(e->payload, PAYLOAD_BYTES);
}

static void wine_peer_touch(unsigned gen)
{
	unsigned i;
	if (!g_wine_peer) return;
	g_wine_peer->magic = 0x57494E45u;
	g_wine_peer->live_gen = gen;
	for (i = 0; i < HANDLE_STUB; i++)
		g_wine_peer->handle_page_stub[i] = (unsigned char)(gen + i);
}

static void process_heap_grow(unsigned cycle)
{
	size_t want = 4096u + (cycle % 17u) * 512u;
	void *nb;
	if (g_process_ballast)
		HeapFree(GetProcessHeap(), 0, g_process_ballast);
	nb = HeapAlloc(GetProcessHeap(), 0, want);
	if (nb) {
		memset(nb, (int)(cycle & 0xff), want);
		g_process_ballast = nb;
		g_process_ballast_bytes = want;
	}
}

static volatile LONG g_val_done;
static volatile BOOL g_val_ok;

static DWORD WINAPI wine_validate_thread(LPVOID arg)
{
	HANDLE wh = (HANDLE)arg;
	g_val_ok = HeapValidate(wh, 0, NULL);
	InterlockedExchange(&g_val_done, 1);
	return 0;
}

static int try_wine_heap_validate(unsigned timeout_ms)
{
	HANDLE th;
	DWORD w;
	if (!g_wine_heap) return 0;
	InterlockedExchange(&g_val_done, 0);
	g_val_ok = FALSE;
	th = CreateThread(NULL, 0, wine_validate_thread, g_wine_heap, 0, NULL);
	if (!th) return 0;
	w = WaitForSingleObject(th, timeout_ms);
	if (w == WAIT_TIMEOUT) {
		TerminateThread(th, 1);
		CloseHandle(th);
		printf("  Wine-like HeapValidate did not finish in %ums (held heap — do not validate on restore)\n",
		       timeout_ms);
		return 0;
	}
	CloseHandle(th);
	return g_val_ok ? 1 : 0;
}

static int world_setup(unsigned seed)
{
	g_engine_heap = HeapCreate(0, 64 * 1024, 0);
	g_wine_heap = HeapCreate(HEAP_GROWABLE, 0, 0);
	if (!g_engine_heap || !g_wine_heap) return 0;
	g_engine = (EngineBlock *)HeapAlloc(g_engine_heap, 0, sizeof(EngineBlock));
	g_wine_peer = (WinePeer *)HeapAlloc(g_wine_heap, 0, sizeof(WinePeer));
	if (!g_engine || !g_wine_peer) return 0;
	memset(g_engine, 0, sizeof(*g_engine));
	g_engine->hdr.magic = 0x454E474Eu;
	g_engine->hdr.generation = 1;
	fill_payload(g_engine, seed);
	wine_peer_touch(1);
	process_heap_grow(1);
	return 1;
}

static void world_teardown(void)
{
	if (g_process_ballast) {
		HeapFree(GetProcessHeap(), 0, g_process_ballast);
		g_process_ballast = NULL;
	}
	if (g_wine_peer) { HeapFree(g_wine_heap, 0, g_wine_peer); g_wine_peer = NULL; }
	if (g_engine) { HeapFree(g_engine_heap, 0, g_engine); g_engine = NULL; }
	if (g_wine_heap) { HeapDestroy(g_wine_heap); g_wine_heap = NULL; }
	if (g_engine_heap) { HeapDestroy(g_engine_heap); g_engine_heap = NULL; }
}

static int checkpoint(EngineLogical *out, unsigned char *payload_copy, unsigned *included_stub)
{
	if (!logical_ok(g_engine)) return 0;
	*out = g_engine->hdr;
	memcpy(payload_copy, g_engine->payload, PAYLOAD_BYTES);
	*included_stub = 0;
	if (g_unsafe_snap) {
		/* Poison: include Wine handle-page stub in snap (heap we do not own). */
		memcpy(payload_copy + PAYLOAD_BYTES - HANDLE_STUB, g_wine_peer->handle_page_stub, HANDLE_STUB);
		*included_stub = 1;
	}
	return 1;
}

static int restore_logical(const EngineLogical *lg, const unsigned char *payload_copy, unsigned included_stub)
{
	unsigned gen_before = g_wine_peer ? g_wine_peer->live_gen : 0;

	if (g_validate_wine) {
		printf("  bad-restore: HeapValidate on held Wine-like heap\n");
		(void)try_wine_heap_validate(3000);
	}
	if (!g_engine) return 0;
	g_engine->hdr = *lg;
	memcpy(g_engine->payload, payload_copy, PAYLOAD_BYTES);
	if (included_stub)
		printf("  restore: snap included foreign handle-page stub (behavior poison)\n");
	if (!logical_ok(g_engine)) {
		if (included_stub && g_unsafe_snap)
			return 0;
		fill_payload(g_engine, lg->seed);
		g_engine->hdr.generation = lg->generation;
	}
	wine_peer_touch(gen_before + 1u);
	process_heap_grow(lg->seed & 31u);
	return logical_ok(g_engine) && g_engine->hdr.fp == lg->fp;
}

static int insession(int cycles)
{
	unsigned char *snap_payload;
	EngineLogical snap_l;
	unsigned ok = 0, refused = 0, poisoned = 0, split = 0, reconciled = 0;
	int c;

	snap_payload = (unsigned char *)malloc(PAYLOAD_BYTES);
	if (!snap_payload) return 2;

	printf("insession heaps: process+engine+wine-like unsafe_snap=%u validate_wine=%u\n",
	       g_unsafe_snap, g_validate_wine);

	for (c = 0; c < cycles; c++) {
		unsigned seed = (unsigned)(c + 1) * 8803u;
		unsigned inc = 0;
		int restored;

		g_engine->hdr.generation++;
		fill_payload(g_engine, seed);
		wine_peer_touch((unsigned)(c + 10));
		process_heap_grow((unsigned)c);

		if (!checkpoint(&snap_l, snap_payload, &inc)) {
		 refused++;
			continue;
		}

		fill_payload(g_engine, seed + 404u);
		wine_peer_touch((unsigned)(c + 999));
		process_heap_grow((unsigned)(c + 50));

		restored = restore_logical(&snap_l, snap_payload, inc);
		if (!restored) {
			split++;
			if (g_unsafe_snap && inc)
				poisoned++;
			else {
				fill_payload(g_engine, snap_l.seed);
				g_engine->hdr.generation = snap_l.generation;
				reconciled++;
				restored = logical_ok(g_engine) && g_engine->hdr.fp == snap_l.fp;
			}
		}
		if (restored && g_engine->hdr.fp == snap_l.fp)
			ok++;
		else if (!g_unsafe_snap)
			poisoned++;
		else if (!inc)
			poisoned++;
	}

	printf("summary ok=%u/%d refused=%u split=%u reconciled=%u poisoned=%u\n",
	       ok, cycles, refused, split, reconciled, poisoned);
	free(snap_payload);

	if (g_unsafe_snap) {
		int pass = (poisoned > 0 && ok < (unsigned)cycles);
		printf("%s: unsafe snap including foreign handle-page stub poisons restore\n", pass ? "PASS" : "FAIL");
		return pass ? 0 : 1;
	}
	{
		int pass = (ok == (unsigned)cycles && poisoned == 0);
		printf("%s: engine logical snap only; no HeapValidate on held wine heap\n", pass ? "PASS" : "FAIL");
		return pass ? 0 : 1;
	}
}

static int count_dead(const SnapHdr *h)
{
	if (h->saved_pid != GetCurrentProcessId())
		return 4;
	{
		int d = 0;
		if (h->engine_heap && (void *)h->engine_heap != (void *)g_engine_heap) d++;
		if (h->engine_block && (void *)h->engine_block != (void *)g_engine) d++;
		if (h->wine_heap && (void *)h->wine_heap != (void *)g_wine_heap) d++;
		if (h->wine_peer && (void *)h->wine_peer != (void *)g_wine_peer) d++;
		if (d == 0) d = 4;
		return d;
	}
}

static int xsession_cmd(const char *cmd, const char *file, unsigned seed)
{
	if (strcmp(cmd, "save") == 0) {
		SnapHdr hdr;
		FILE *f;
		unsigned inc = 0;
		unsigned char *pay = (unsigned char *)malloc(PAYLOAD_BYTES);
		if (!pay) return 2;
		memset(&hdr, 0, sizeof(hdr));
		memcpy(hdr.magic, "HEAPXS8 ", 8);
		hdr.ver = 1;
		if (!checkpoint(&hdr.logical, pay, &inc)) {
			free(pay);
		 return 2;
		}
		hdr.saved_pid = GetCurrentProcessId();
		hdr.engine_heap = (uintptr_t)g_engine_heap;
		hdr.engine_block = (uintptr_t)g_engine;
		hdr.wine_heap = (uintptr_t)g_wine_heap;
		hdr.wine_peer = (uintptr_t)g_wine_peer;
		hdr.included_wine_stub = inc;
		f = fopen(file, "wb");
		if (!f) { free(pay); return 2; }
		fwrite(&hdr, sizeof(hdr), 1, f);
		fwrite(pay, 1, PAYLOAD_BYTES, f);
		fclose(f);
		printf("SAVE seed=%u fp=0x%08x engine_heap=%p wine_heap=%p included_stub=%u -> %s\n",
		       seed, hdr.logical.fp, (void *)g_engine_heap, (void *)g_wine_heap, inc, file);
		free(pay);
		return 0;
	}

	if (strcmp(cmd, "prove") == 0 || strcmp(cmd, "restore") == 0) {
		SnapHdr hdr;
		FILE *f;
		unsigned char *pay, *saved;
		unsigned diff = 0, i;
		int dead;
		f = fopen(file, "rb");
		if (!f || fread(&hdr, sizeof(hdr), 1, f) != 1 || memcmp(hdr.magic, "HEAPXS8 ", 8) != 0) {
			printf("bad snap\n");
			if (f) fclose(f);
			return 2;
		}
		pay = (unsigned char *)malloc(PAYLOAD_BYTES);
		saved = (unsigned char *)malloc(PAYLOAD_BYTES);
		if (!pay || !saved || fread(pay, 1, PAYLOAD_BYTES, f) != PAYLOAD_BYTES) {
			fclose(f);
			free(pay);
			free(saved);
			return 2;
		}
		fclose(f);
		memcpy(saved, pay, PAYLOAD_BYTES);

		dead = count_dead(&hdr);
		printf("PROVE dead=%d/4 engine_heap saved=%p live=%p wine_heap saved=%p live=%p\n", dead,
		       (void *)hdr.engine_heap, (void *)g_engine_heap, (void *)hdr.wine_heap,
		       (void *)g_wine_heap);

		if (strcmp(cmd, "prove") == 0) {
			free(pay);
			free(saved);
			printf("%s: heap handles not adopted cross-session (%d/4)\n", dead >= 4 ? "PASS" : "FAIL", dead);
			return dead >= 4 ? 0 : 1;
		}

		if (!restore_logical(&hdr.logical, pay, hdr.included_wine_stub)) {
			fill_payload(g_engine, hdr.logical.seed);
			g_engine->hdr.generation = hdr.logical.generation;
		}
		for (i = 0; i < PAYLOAD_BYTES; i++)
			if (g_engine->payload[i] != saved[i]) diff++;
		printf("RESTORE fp_match=%s payload_diffs=%u/%u (no HeapValidate on wine heap)\n",
		       logical_ok(g_engine) && g_engine->hdr.fp == hdr.logical.fp ? "YES" : "NO", diff,
		       PAYLOAD_BYTES);
		free(pay);
		free(saved);
		{
			int ok = (dead >= 4) && logical_ok(g_engine) && (g_engine->hdr.fp == hdr.logical.fp) &&
				 (diff == 0) && (hdr.included_wine_stub == 0);
			printf("%s: xsession engine heap restore from logical payload only\n", ok ? "PASS" : "FAIL");
			return ok ? 0 : 1;
		}
	}
	return 2;
}

int main(int argc, char **argv)
{
	unsigned seed = 8088u;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--unsafe-snap") == 0) g_unsafe_snap = 1;
		if (strcmp(argv[i], "--validate-wine") == 0) g_validate_wine = 1;
	}

	if (!world_setup(seed)) {
		printf("heap world setup failed\n");
		return 2;
	}

	if (argc > 1 && strcmp(argv[1], "insession") == 0) {
		int cycles = argc > 2 ? atoi(argv[2]) : 45;
		int rc = insession(cycles);
		world_teardown();
		return rc;
	}

	if (argc > 1 && strcmp(argv[1], "xsession") == 0 && argc > 3) {
		int rc;
		if (argc > 4) seed = (unsigned)strtoul(argv[4], NULL, 0);
		fill_payload(g_engine, seed);
		rc = xsession_cmd(argv[2], argv[3], seed);
		world_teardown();
		return rc;
	}

	world_teardown();
	printf("usage: %s insession [cycles] [--unsafe-snap] [--validate-wine]\n", argv[0]);
	printf("       %s xsession save|prove|restore <file> [seed]\n", argv[0]);
	return 2;
}
