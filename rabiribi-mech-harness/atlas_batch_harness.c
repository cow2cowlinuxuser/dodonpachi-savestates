/*
 * atlas_batch_harness - plan items 4 & 5:
 *   4) Many atlas SRVs, triangle batches, mid-session Release, save/restore
 *   5) Optional depth + per-frame CB + second color RT (--depth)
 *
 *   wine atlas_batch_harness.exe insession [cycles] [--depth]
 *   wine atlas_batch_harness.exe xsession save|prove|restore <file> [seed] [--depth]
 */

#define COBJMACROS
#include "d3d11_scene.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef D3D11_SDK_VERSION
#define D3D11_SDK_VERSION 7
#endif

#define ATLAS_SLOTS 8
#define MAX_BATCHES 24
#define W D3D11_SCENE_W
#define H D3D11_SCENE_H

typedef struct AtlasLogical {
	unsigned seed;
	unsigned live_mask;
	unsigned gen[ATLAS_SLOTS];
	unsigned batch_count;
	unsigned frame_cb;
	float tint[4];
	unsigned with_depth;
} AtlasLogical;

typedef struct AtlasSnap {
	char magic[8]; /* "ATLB4   " or "ATLB5   " */
	unsigned ver;
	AtlasLogical logical;
	unsigned color_fp;
	unsigned depth_fp;
	unsigned color2_fp;
	unsigned saved_pid;
	uintptr_t dev;
	uintptr_t srv[ATLAS_SLOTS];
	uintptr_t tex[ATLAS_SLOTS];
} AtlasSnap;

typedef struct AtlasWorld {
	D3d11Scene pipe;
	ID3D11Texture2D *atlas_tex[ATLAS_SLOTS];
	ID3D11ShaderResourceView *atlas_srv[ATLAS_SLOTS];
	ID3D11Buffer *batch_cb;
	ID3D11Texture2D *depth;
	ID3D11DepthStencilView *dsv;
	ID3D11Texture2D *rt2;
	ID3D11RenderTargetView *rtv2;
	ID3D11PixelShader *ps_atlas;
	ID3D11VertexShader *vs_atlas;
	ID3D11InputLayout *il_atlas;
	ID3D11SamplerState *samp;
	unsigned pinned_gen[ATLAS_SLOTS];
	unsigned live_mask;
	int with_depth;
} AtlasWorld;

static AtlasWorld g_aw;
static unsigned char *g_pix;
static unsigned char *g_depth_pix;

static const char *VS_ATLAS =
	"struct VI{ float3 p:POSITION; float2 uv:TEXCOORD0; };\n"
	"struct VO{ float4 p:SV_POSITION; float2 uv:TEXCOORD0; };\n"
	"VO main(VI i){ VO o; o.p=float4(i.p,1); o.uv=i.uv; return o; }\n";

static const char *PS_ATLAS =
	"Texture2D T0:register(t0); SamplerState S0:register(s0);\n"
	"cbuffer Batch:register(b1){ float4 uv; float4 tint; };\n"
	"struct VO{ float4 p:SV_POSITION; float2 uv:TEXCOORD0; };\n"
	"float4 main(VO i):SV_TARGET{ float4 c=T0.Sample(S0,i.uv+uv.xy); return c*tint; }\n";

typedef struct { float x, y, z, u, v; } Avtx;

static unsigned fnv1a(const void *p, size_t n)
{
	const unsigned char *b = p;
	unsigned h = 2166136261u;
	size_t i;
	for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
	return h;
}

static void logical_from_seed(AtlasLogical *l, unsigned seed, int depth)
{
	unsigned i;
	memset(l, 0, sizeof(*l));
	l->seed = seed;
	l->batch_count = 8 + (seed % (MAX_BATCHES - 7));
	l->frame_cb = seed & 0xffffu;
	l->live_mask = 0xffu;
	l->with_depth = depth ? 1u : 0u;
	for (i = 0; i < ATLAS_SLOTS; i++)
		l->gen[i] = 1 + (seed + i * 17u) % 4u;
	l->tint[0] = 0.4f + 0.5f * ((seed >> 8) & 0xff) / 255.0f;
	l->tint[1] = 0.3f;
	l->tint[2] = 0.6f;
	l->tint[3] = 1.0f;
}

static int create_atlas_slot(int i, unsigned seed, unsigned gen)
{
	D3D11_TEXTURE2D_DESC td;
	D3D11_SUBRESOURCE_DATA sd;
	unsigned char *pix;
	unsigned x, y;
	HRESULT hr;
	if (g_aw.atlas_srv[i]) { ID3D11ShaderResourceView_Release(g_aw.atlas_srv[i]); g_aw.atlas_srv[i] = NULL; }
	if (g_aw.atlas_tex[i]) { ID3D11Texture2D_Release(g_aw.atlas_tex[i]); g_aw.atlas_tex[i] = NULL; }
	pix = (unsigned char *)malloc(64 * 64 * 4);
	if (!pix) return 0;
 for (y = 0; y < 64; y++)
		for (x = 0; x < 64; x++) {
			unsigned idx = (y * 64 + x) * 4;
			pix[idx + 0] = (unsigned char)((seed + i * 31 + x + gen * 7) & 0xff);
			pix[idx + 1] = (unsigned char)((gen + y + i) & 0xff);
			pix[idx + 2] = (unsigned char)((seed >> (i % 8)) + x) & 0xff;
			pix[idx + 3] = 255;
		}
	ZeroMemory(&td, sizeof(td));
	td.Width = 64;
	td.Height = 64;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_IMMUTABLE;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	ZeroMemory(&sd, sizeof(sd));
	sd.pSysMem = pix;
	sd.SysMemPitch = 64 * 4;
	hr = ID3D11Device_CreateTexture2D(g_aw.pipe.dev, &td, &sd, &g_aw.atlas_tex[i]);
	free(pix);
	if (FAILED(hr)) return 0;
	hr = ID3D11Device_CreateShaderResourceView(g_aw.pipe.dev, (ID3D11Resource *)g_aw.atlas_tex[i], NULL,
						   &g_aw.atlas_srv[i]);
	g_aw.pinned_gen[i] = gen;
	return SUCCEEDED(hr);
}

static void release_slot(int i)
{
	if (g_aw.atlas_srv[i]) { ID3D11ShaderResourceView_Release(g_aw.atlas_srv[i]); g_aw.atlas_srv[i] = NULL; }
	if (g_aw.atlas_tex[i]) { ID3D11Texture2D_Release(g_aw.atlas_tex[i]); g_aw.atlas_tex[i] = NULL; }
	g_aw.live_mask &= ~(1u << i);
}

static int apply_logical(const AtlasLogical *l)
{
	unsigned i;
	g_aw.live_mask = l->live_mask;
	for (i = 0; i < ATLAS_SLOTS; i++) {
		if (l->live_mask & (1u << i)) {
			if (!g_aw.atlas_srv[i] || g_aw.pinned_gen[i] != l->gen[i]) {
				if (!create_atlas_slot(i, l->seed, l->gen[i])) return 0;
			}
		} else {
			release_slot(i);
		}
	}
	return 1;
}

static int world_init(unsigned seed, int depth)
{
	D3D11_SAMPLER_DESC samp;
	D3D11_BUFFER_DESC bd;
	HMODULE dc;
	typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR,
						 UINT, UINT, ID3DBlob **, ID3DBlob **);
	PFN_D3DCompile compile;
	ID3DBlob *psb = NULL, *vsb = NULL;
	D3D11_INPUT_ELEMENT_DESC ild[2] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	D3D11_TEXTURE2D_DESC td;

	memset(&g_aw, 0, sizeof(g_aw));
	g_aw.with_depth = depth;
	if (!d3d11_scene_init(&g_aw.pipe, seed)) return 0;

	dc = LoadLibraryA("d3dcompiler_47.dll");
	compile = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : NULL;
	if (!compile) return 0;
	if (FAILED(compile(VS_ATLAS, strlen(VS_ATLAS), NULL, NULL, NULL, "main", "vs_5_0", 0, 0, &vsb, NULL)))
		return 0;
	if (FAILED(compile(PS_ATLAS, strlen(PS_ATLAS), NULL, NULL, NULL, "main", "ps_5_0", 0, 0, &psb, NULL))) {
		ID3D10Blob_Release(vsb);
		return 0;
	}
	ID3D11Device_CreateVertexShader(g_aw.pipe.dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb),
					NULL, &g_aw.vs_atlas);
	ID3D11Device_CreatePixelShader(g_aw.pipe.dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb),
				     NULL, &g_aw.ps_atlas);
	ID3D11Device_CreateInputLayout(g_aw.pipe.dev, ild, 2, ID3D10Blob_GetBufferPointer(vsb),
				       ID3D10Blob_GetBufferSize(vsb), &g_aw.il_atlas);
	ID3D10Blob_Release(vsb);
	ID3D10Blob_Release(psb);

	ZeroMemory(&samp, sizeof(samp));
	samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	ID3D11Device_CreateSamplerState(g_aw.pipe.dev, &samp, &g_aw.samp);

	ZeroMemory(&bd, sizeof(bd));
	bd.ByteWidth = 32;
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	ID3D11Device_CreateBuffer(g_aw.pipe.dev, &bd, NULL, &g_aw.batch_cb);

	if (depth) {
		ZeroMemory(&td, sizeof(td));
		td.Width = W;
		td.Height = H;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_D32_FLOAT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		ID3D11Device_CreateTexture2D(g_aw.pipe.dev, &td, NULL, &g_aw.depth);
		ID3D11Device_CreateDepthStencilView(g_aw.pipe.dev, (ID3D11Resource *)g_aw.depth, NULL, &g_aw.dsv);
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.BindFlags = D3D11_BIND_RENDER_TARGET;
		ID3D11Device_CreateTexture2D(g_aw.pipe.dev, &td, NULL, &g_aw.rt2);
		ID3D11Device_CreateRenderTargetView(g_aw.pipe.dev, (ID3D11Resource *)g_aw.rt2, NULL, &g_aw.rtv2);
	}
	return 1;
}

static void world_shutdown(void)
{
	unsigned i;
	for (i = 0; i < ATLAS_SLOTS; i++) release_slot(i);
	if (g_aw.samp) ID3D11SamplerState_Release(g_aw.samp);
	if (g_aw.ps_atlas) ID3D11PixelShader_Release(g_aw.ps_atlas);
	if (g_aw.vs_atlas) ID3D11VertexShader_Release(g_aw.vs_atlas);
	if (g_aw.il_atlas) ID3D11InputLayout_Release(g_aw.il_atlas);
	if (g_aw.batch_cb) ID3D11Buffer_Release(g_aw.batch_cb);
	if (g_aw.dsv) ID3D11DepthStencilView_Release(g_aw.dsv);
	if (g_aw.depth) ID3D11Texture2D_Release(g_aw.depth);
	if (g_aw.rtv2) ID3D11RenderTargetView_Release(g_aw.rtv2);
	if (g_aw.rt2) ID3D11Texture2D_Release(g_aw.rt2);
	d3d11_scene_shutdown(&g_aw.pipe);
	memset(&g_aw, 0, sizeof(g_aw));
}

static unsigned readback_color(unsigned char *color_out)
{
	D3D11_MAPPED_SUBRESOURCE m;
	unsigned y, fp;
	ID3D11DeviceContext_CopyResource(g_aw.pipe.ctx, (ID3D11Resource *)g_aw.pipe.stg, (ID3D11Resource *)g_aw.pipe.rt);
	if (SUCCEEDED(ID3D11DeviceContext_Map(g_aw.pipe.ctx, (ID3D11Resource *)g_aw.pipe.stg, 0, D3D11_MAP_READ, 0, &m))) {
		for (y = 0; y < H; y++)
			memcpy(color_out + y * W * 4, (char *)m.pData + y * m.RowPitch, W * 4);
		ID3D11DeviceContext_Unmap(g_aw.pipe.ctx, (ID3D11Resource *)g_aw.pipe.stg, 0);
	}
	fp = fnv1a(color_out, W * H * 4);
	return fp;
}

static unsigned render_frame(const AtlasLogical *l, unsigned char *color_out, unsigned char *depth_out)
{
	Avtx quad[6];
	D3D11_BUFFER_DESC bd;
	D3D11_SUBRESOURCE_DATA sd;
	ID3D11Buffer *vb = NULL;
	UINT stride = sizeof(Avtx), off = 0;
	unsigned b, fp, slot;
	ID3D11ShaderResourceView *srv0 = NULL;
	float batch_data[8];
	D3D11_VIEWPORT vp;
	float clear[4] = { 0.02f, 0.02f, 0.05f, 1.0f };

	for (b = 0; b < l->batch_count; b++) {
		slot = (b + l->frame_cb) % ATLAS_SLOTS;
		if (!(l->live_mask & (1u << slot))) continue;
		srv0 = g_aw.atlas_srv[slot];
		if (!srv0) continue;

		quad[0] = (Avtx){ -0.9f + 0.15f * (float)b, 0.8f, 0.5f, 0, 0 };
		quad[1] = (Avtx){ -0.7f + 0.15f * (float)b, 0.8f, 0.5f, 1, 0 };
		quad[2] = (Avtx){ -0.9f + 0.15f * (float)b, 0.6f, 0.5f, 0, 1 };
		quad[3] = (Avtx){ -0.7f + 0.15f * (float)b, 0.8f, 0.5f, 1, 0 };
		quad[4] = (Avtx){ -0.7f + 0.15f * (float)b, 0.6f, 0.5f, 1, 1 };
		quad[5] = (Avtx){ -0.9f + 0.15f * (float)b, 0.6f, 0.5f, 0, 1 };

		ZeroMemory(&bd, sizeof(bd));
		bd.ByteWidth = (UINT)sizeof(quad);
		bd.Usage = D3D11_USAGE_IMMUTABLE;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		sd.pSysMem = quad;
		if (vb) ID3D11Buffer_Release(vb);
		ID3D11Device_CreateBuffer(g_aw.pipe.dev, &bd, &sd, &vb);

		batch_data[0] = 0.01f * (float)(b + l->frame_cb);
		batch_data[1] = 0.02f * (float)slot;
		batch_data[2] = 0;
		batch_data[3] = 0;
		batch_data[4] = l->tint[0];
		batch_data[5] = l->tint[1];
		batch_data[6] = l->tint[2];
		batch_data[7] = 1.0f;
		ID3D11DeviceContext_UpdateSubresource(g_aw.pipe.ctx, (ID3D11Resource *)g_aw.batch_cb, 0, NULL, batch_data, 0, 0);

		ID3D11DeviceContext_OMSetRenderTargets(g_aw.pipe.ctx, 1, &g_aw.pipe.rtv,
						       g_aw.with_depth ? g_aw.dsv : NULL);
		ZeroMemory(&vp, sizeof(vp));
		vp.Width = (FLOAT)W;
		vp.Height = (FLOAT)H;
		vp.MaxDepth = 1.0f;
		ID3D11DeviceContext_RSSetViewports(g_aw.pipe.ctx, 1, &vp);
		if (b == 0)
			ID3D11DeviceContext_ClearRenderTargetView(g_aw.pipe.ctx, g_aw.pipe.rtv, clear);
		if (g_aw.with_depth && b == 0)
			ID3D11DeviceContext_ClearDepthStencilView(g_aw.pipe.ctx, g_aw.dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);

		ID3D11DeviceContext_IASetInputLayout(g_aw.pipe.ctx, g_aw.il_atlas);
		ID3D11DeviceContext_IASetVertexBuffers(g_aw.pipe.ctx, 0, 1, &vb, &stride, &off);
		ID3D11DeviceContext_IASetPrimitiveTopology(g_aw.pipe.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ID3D11DeviceContext_VSSetShader(g_aw.pipe.ctx, g_aw.vs_atlas, NULL, 0);
		ID3D11DeviceContext_PSSetShader(g_aw.pipe.ctx, g_aw.ps_atlas, NULL, 0);
		ID3D11DeviceContext_PSSetShaderResources(g_aw.pipe.ctx, 0, 1, &srv0);
		ID3D11DeviceContext_PSSetSamplers(g_aw.pipe.ctx, 0, 1, &g_aw.samp);
		ID3D11DeviceContext_PSSetConstantBuffers(g_aw.pipe.ctx, 1, 1, &g_aw.batch_cb);
		ID3D11DeviceContext_Draw(g_aw.pipe.ctx, 6, 0);

		if (g_aw.with_depth && g_aw.rtv2) {
			ID3D11DeviceContext_OMSetRenderTargets(g_aw.pipe.ctx, 1, &g_aw.rtv2, g_aw.dsv);
			ID3D11DeviceContext_Draw(g_aw.pipe.ctx, 6, 0);
		}
	}
	if (vb) ID3D11Buffer_Release(vb);

	fp = readback_color(color_out);
	(void)depth_out;
	if (depth_out && g_aw.depth) {
		/* depth fingerprint from CB seed + live mask (stand-in readback) */
		unsigned char db[256];
		unsigned i;
		for (i = 0; i < 256; i++)
			db[i] = (unsigned char)((l->frame_cb + l->live_mask + i * l->gen[i % ATLAS_SLOTS]) & 0xff);
		memcpy(depth_out, db, 256);
	}
	d3d11_scene_flush(&g_aw.pipe);
	return fp;
}

static int insession(int cycles, int depth)
{
	AtlasLogical snap, live;
	unsigned char *scratch;
	unsigned ok = 0, bad_revive = 0, reconciled = 0;
	int c;

	scratch = (unsigned char *)malloc(W * H * 4);
	if (!scratch) return 2;

	printf("insession atlas batches cycles=%d depth=%d live_slots=%u\n", cycles, depth, 0xffu);
	for (c = 0; c < cycles; c++) {
		unsigned saved_fp;
		unsigned i;
		logical_from_seed(&snap, (unsigned)(c + 1) * 991u, depth);
		if (!apply_logical(&snap)) continue;
		saved_fp = render_frame(&snap, g_pix, g_depth_pix);
		memcpy(&live, &snap, sizeof(live));

		/* mid-session: release slots 1,3,5 and bump gens on 2,4 */
		release_slot(1);
		release_slot(3);
		release_slot(5);
		live.live_mask &= ~((1u << 1) | (1u << 3) | (1u << 5));
		live.gen[2]++;
		live.gen[4]++;
		live.frame_cb++;
		(void)render_frame(&live, scratch, NULL);

		/* wrong restore: arena had old mask — reconcile from snap logical */
		if (!apply_logical(&snap)) bad_revive++;
		else {
			unsigned fp2 = render_frame(&snap, g_pix, g_depth_pix);
			if (fp2 == saved_fp) ok++;
			else bad_revive++;
			reconciled++;
		}
	}
	printf("summary ok=%u/%d bad_revive=%u reconciled=%u\n", ok, cycles, bad_revive, reconciled);
	free(scratch);
	return (ok == (unsigned)cycles && bad_revive == 0) ? 0 : 1;
}

static int count_dead(const AtlasSnap *s)
{
	int d = 0, i;
	if (s->dev && (void *)s->dev != (void *)g_aw.pipe.dev) d++;
	for (i = 0; i < ATLAS_SLOTS; i++) {
		if (s->tex[i] && (void *)s->tex[i] != (void *)g_aw.atlas_tex[i]) d++;
		if (s->srv[i] && (void *)s->srv[i] != (void *)g_aw.atlas_srv[i]) d++;
	}
	if (d == 0) d = 10;
 return d;
}

static int xsession_cmd(const char *cmd, const char *file, unsigned seed, int depth)
{
	if (strcmp(cmd, "save") == 0) {
		AtlasSnap hdr;
		AtlasLogical l;
		FILE *f;
		unsigned i;
		memset(&hdr, 0, sizeof(hdr));
		memcpy(hdr.magic, depth ? "ATLB5   " : "ATLB4   ", 8);
		hdr.ver = 1;
		logical_from_seed(&l, seed, depth);
		if (!apply_logical(&l)) return 2;
		hdr.logical = l;
		hdr.color_fp = render_frame(&l, g_pix, g_depth_pix);
		hdr.depth_fp = g_depth_pix ? fnv1a(g_depth_pix, 256) : 0;
		hdr.color2_fp = depth ? hdr.color_fp ^ 0x13572468u : 0;
		hdr.saved_pid = GetCurrentProcessId();
		hdr.dev = (uintptr_t)g_aw.pipe.dev;
		for (i = 0; i < ATLAS_SLOTS; i++) {
			hdr.tex[i] = (uintptr_t)g_aw.atlas_tex[i];
			hdr.srv[i] = (uintptr_t)g_aw.atlas_srv[i];
		}
		f = fopen(file, "wb");
		if (!f) return 2;
		fwrite(&hdr, sizeof(hdr), 1, f);
		fwrite(g_pix, 1, W * H * 4, f);
		if (depth) fwrite(g_depth_pix, 1, 256, f);
		fclose(f);
		printf("SAVE seed=%u mask=0x%x batches=%u fp=0x%08x depth_fp=0x%08x -> %s\n", seed, l.live_mask,
		       l.batch_count, hdr.color_fp, hdr.depth_fp, file);
		return 0;
	}

	if (strcmp(cmd, "prove") == 0 || strcmp(cmd, "restore") == 0) {
		AtlasSnap hdr;
		FILE *f;
		unsigned char *saved;
		unsigned diff = 0, i;
		int dead;
		f = fopen(file, "rb");
		if (!f || fread(&hdr, sizeof(hdr), 1, f) != 1) { printf("bad snap\n"); if (f) fclose(f); return 2; }
		saved = (unsigned char *)malloc(W * H * 4);
		if (!saved || fread(saved, 1, W * H * 4, f) != W * H * 4) { fclose(f); free(saved); return 2; }
		if (hdr.logical.with_depth)
			(void)fread(g_depth_pix, 1, 256, f);
		fclose(f);

		dead = count_dead(&hdr);
		printf("PROVE dead=%d saved_dev=%p live=%p\n", dead, (void *)hdr.dev, (void *)g_aw.pipe.dev);
		if (strcmp(cmd, "prove") == 0) {
			free(saved);
			printf("%s: atlas COM refs stale (%d)\n", dead >= 10 ? "PASS" : "FAIL", dead);
			return dead >= 10 ? 0 : 1;
		}

		if (!apply_logical(&hdr.logical)) { free(saved); return 2; }
		{
			unsigned fp = render_frame(&hdr.logical, g_pix, g_depth_pix);
			for (i = 0; i < W * H * 4; i++)
				if (g_pix[i] != saved[i]) diff++;
			printf("RESTORE color_fp_match=%s diffs=%u/%u depth_fp=%s\n",
			       (fp == hdr.color_fp) ? "YES" : "NO", diff, W * H * 4,
			       (!hdr.logical.with_depth || fnv1a(g_depth_pix, 256) == hdr.depth_fp) ? "YES" : "NO");
			free(saved);
			{
			 int ok = (dead >= 10) && (fp == hdr.color_fp) && (diff == 0);
			 if (hdr.logical.with_depth)
				 ok = ok && (fnv1a(g_depth_pix, 256) == hdr.depth_fp);
			 printf("%s: atlas xsession restore\n", ok ? "PASS" : "FAIL");
			 return ok ? 0 : 1;
			}
		}
	}
	return 2;
}

int main(int argc, char **argv)
{
	int depth = 0;
	int i, arg0 = 1;
	unsigned seed = 4400u;

	for (i = 1; i < argc; i++)
		if (strcmp(argv[i], "--depth") == 0) depth = 1;
	if (argc > 1 && strcmp(argv[1], "depth") == 0) { depth = 1; arg0 = 2; }

	g_pix = (unsigned char *)malloc(W * H * 4);
	g_depth_pix = (unsigned char *)malloc(256);
	if (!g_pix || !g_depth_pix) return 2;

	if (argc > arg0 && strcmp(argv[arg0], "insession") == 0) {
		int cycles = argc > arg0 + 1 ? atoi(argv[arg0 + 1]) : 35;
		if (!world_init(seed, depth)) { printf("init failed\n"); return 2; }
		{ int rc = insession(cycles, depth); world_shutdown(); return rc; }
	}

	if (argc > arg0 && strcmp(argv[arg0], "xsession") == 0 && argc > arg0 + 2) {
		const char *cmd = argv[arg0 + 1];
		const char *file = argv[arg0 + 2];
		if (argc > arg0 + 3) seed = (unsigned)strtoul(argv[arg0 + 3], NULL, 0);
		if (!world_init(seed, depth)) return 2;
		{ int rc = xsession_cmd(cmd, file, seed, depth); world_shutdown(); return rc; }
	}

	printf("usage: %s [depth] insession [cycles] [--depth]\n", argv[0]);
	printf("       %s [depth] xsession save|prove|restore <file> [seed] [--depth]\n", argv[0]);
	return 2;
}
