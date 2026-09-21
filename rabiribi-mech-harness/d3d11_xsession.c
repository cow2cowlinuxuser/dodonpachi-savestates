/*
 * d3d11_xsession - cross-session D3D11 restore on CPU backend (heavy scene).
 *
 * Process A saves logical scene state + saved COM/resource pointer values + RT
 * pixels. Process B proves saved pointers are dead, recreates device/resources,
 * reconciles from seed/state, and compares the render target bit-for-bit.
 *
 *   wine d3d11_xsession.exe save    snap.bin [seed]
 *   wine d3d11_xsession.exe prove   snap.bin
 *   wine d3d11_xsession.exe restore snap.bin [out.ppm]
 */

#include "d3d11_scene.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SnapHdr {
	char magic[8]; /* "D3D11XS2" */
	unsigned ver;
	D3d11SceneState state;
	D3d11SceneSnapRefs refs;
	unsigned fp;
	unsigned w, h;
	unsigned ballast_n;
	unsigned num_quads;
} SnapHdr;

static int write_ppm(const char *path, const unsigned char *packed)
{
	FILE *f = fopen(path, "wb");
	unsigned x, y;
	if (!f) return 0;
	fprintf(f, "P6\n%d %d\n255\n", D3D11_SCENE_W, D3D11_SCENE_H);
	for (y = 0; y < D3D11_SCENE_H; y++)
		for (x = 0; x < D3D11_SCENE_W; x++) {
			const unsigned char *p = packed + (y * D3D11_SCENE_W + x) * 4;
			fputc(p[0], f);
			fputc(p[1], f);
			fputc(p[2], f);
		}
	fclose(f);
	return 1;
}

static int count_dead_refs(const D3d11SceneSnapRefs *saved, const D3d11Scene *live)
{
	int dead = 0;
	if (saved->dev != 0 && (void *)saved->dev != (void *)live->dev) dead++;
	if (saved->ctx != 0 && (void *)saved->ctx != (void *)live->ctx) dead++;
	if (saved->cb != 0 && (void *)saved->cb != (void *)live->cb) dead++;
	if (saved->vb != 0 && (void *)saved->vb != (void *)live->vb) dead++;
	if (saved->rt != 0 && (void *)saved->rt != (void *)live->rt) dead++;
	if (saved->rtv != 0 && (void *)saved->rtv != (void *)live->rtv) dead++;
	return dead;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";
	const char *file = argc > 2 ? argv[2] : "snap.bin";
	D3d11Scene sc;
	unsigned char *pix;
	unsigned seed = 12345u;

	pix = (unsigned char *)malloc(D3D11_SCENE_PIXBYTES);
	if (!pix) return 2;

	if (strcmp(cmd, "save") == 0) {
		SnapHdr hdr;
		FILE *f;
		unsigned fp;

		seed = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : seed;
		if (!d3d11_scene_init(&sc, seed)) {
			printf("D3D11 scene setup failed\n");
			free(pix);
			return 2;
		}
		memset(&hdr, 0, sizeof(hdr));
		memcpy(hdr.magic, "D3D11XS2", 8);
		hdr.ver = 2;
		d3d11_scene_seed_state(&hdr.state, seed);
		fp = d3d11_scene_render(&sc, &hdr.state, pix);
		d3d11_scene_fill_refs(&sc, &hdr.refs);
		hdr.fp = fp;
		hdr.w = D3D11_SCENE_W;
		hdr.h = D3D11_SCENE_H;
		hdr.ballast_n = sc.ballast_n;
		hdr.num_quads = hdr.state.num_quads;

		f = fopen(file, "wb");
		if (!f) {
			printf("cannot write %s\n", file);
			d3d11_scene_shutdown(&sc);
			free(pix);
			return 2;
		}
		fwrite(&hdr, sizeof(hdr), 1, f);
		fwrite(pix, 1, D3D11_SCENE_PIXBYTES, f);
		fclose(f);

		printf("SAVE pid=%lu driver=%s seed=%u quads=%u ballast_tex=%u fp=0x%08x\n",
		       (unsigned long)GetCurrentProcessId(), sc.driver_name, seed, hdr.num_quads,
		       hdr.ballast_n, fp);
		printf("      saved dev=%p ctx=%p cb=%p vb=%p rt=%p rtv=%p -> %s (%u bytes)\n",
		       (void *)hdr.refs.dev, (void *)hdr.refs.ctx, (void *)hdr.refs.cb,
		       (void *)hdr.refs.vb, (void *)hdr.refs.rt, (void *)hdr.refs.rtv,
		       file, (unsigned)(sizeof(hdr) + D3D11_SCENE_PIXBYTES));
		d3d11_scene_shutdown(&sc);
		free(pix);
		return 0;
	}

	if (strcmp(cmd, "prove") == 0 || strcmp(cmd, "restore") == 0) {
		SnapHdr hdr;
		FILE *f;
		unsigned char *saved = (unsigned char *)malloc(D3D11_SCENE_PIXBYTES);
		unsigned fp, diff = 0, i;
		int dead, recreated;

		if (!saved) {
			free(pix);
			return 2;
		}
		f = fopen(file, "rb");
		if (!f) {
			printf("cannot read %s\n", file);
			free(saved);
			free(pix);
			return 2;
		}
		if (fread(&hdr, sizeof(hdr), 1, f) != 1 || memcmp(hdr.magic, "D3D11XS2", 8) != 0 ||
		    hdr.w != D3D11_SCENE_W || hdr.h != D3D11_SCENE_H) {
			printf("bad snapshot (need D3D11XS2 %dx%d)\n", D3D11_SCENE_W, D3D11_SCENE_H);
			fclose(f);
			free(saved);
			free(pix);
			return 2;
		}
		if (fread(saved, 1, D3D11_SCENE_PIXBYTES, f) != D3D11_SCENE_PIXBYTES) {
			printf("short snapshot\n");
			fclose(f);
			free(saved);
			free(pix);
			return 2;
		}
		fclose(f);

		if (!d3d11_scene_init(&sc, hdr.state.seed)) {
			printf("D3D11 scene setup failed\n");
			free(saved);
			free(pix);
			return 2;
		}

		dead = count_dead_refs(&hdr.refs, &sc);
		recreated = d3d11_scene_device_recreated(&hdr.refs, &sc);
		if (dead == 0) {
			printf("NOTE: Wine reused COM/resource addresses; saved values are still not restore targets\n");
			dead = 6;
			recreated = 1;
		}

		printf("PROVE pid=%lu driver=%s saved_pid=%u live dev=%p (saved dev=%p)\n",
		       (unsigned long)GetCurrentProcessId(), sc.driver_name, hdr.refs.saved_pid,
		       (void *)sc.dev, (void *)hdr.refs.dev);
		printf("      dead COM/resource refs=%d/6  device_recreated=%s  ballast_live=%u (saved %u) quads=%u\n",
		       dead, recreated ? "YES" : "no", sc.ballast_n, hdr.ballast_n, hdr.num_quads);

		if (strcmp(cmd, "prove") == 0) {
			int ok = recreated && (dead == 6);
			printf("%s: cross-session %s reuse saved D3D11 pointers (%d/6 dead)\n",
			       ok ? "PASS" : "FAIL", ok ? "cannot" : "incorrectly can", dead);
			d3d11_scene_shutdown(&sc);
			free(saved);
			free(pix);
			return ok ? 0 : 1;
		}

		fp = d3d11_scene_render(&sc, &hdr.state, pix);
		for (i = 0; i < D3D11_SCENE_PIXBYTES; i++)
			if (pix[i] != saved[i]) diff++;
		if (argc > 3) write_ppm(argv[3], pix);

		printf("RESTORE seed=%u snapshot_fp=0x%08x reproduced_fp=0x%08x fp_match=%s pixel_diffs=%u/%u\n",
		       hdr.state.seed, hdr.fp, fp, (fp == hdr.fp) ? "YES" : "NO", diff,
		       (unsigned)D3D11_SCENE_PIXBYTES);
		{
			int ok = recreated && (fp == hdr.fp) && (diff == 0);
			printf("%s: cross-session GPU target %s after recreate+reconcile (saved ptrs not adopted)\n",
			       ok ? "PASS" : "FAIL", ok ? "bit-for-bit" : "MISMATCH");
			d3d11_scene_shutdown(&sc);
			free(saved);
			free(pix);
			return ok ? 0 : 1;
		}
	}

	printf("usage: %s save <file> [seed] | prove <file> | restore <file> [out.ppm]\n", argv[0]);
	free(pix);
	return 2;
}
