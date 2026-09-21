#define COBJMACROS
#include "d3d11_scene.h"
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef D3D11_SDK_VERSION
#define D3D11_SDK_VERSION 7
#endif

typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void *,
					 void *, LPCSTR, LPCSTR, UINT, UINT,
					 ID3DBlob **, ID3DBlob **);

typedef struct { float x, y, z; float r, g, b, a; } Vtx;

static const char *HLSL =
	"cbuffer State : register(b0) { float4 tint; float2 offset; float2 pad; };\n"
	"struct VSIn  { float3 pos:POSITION; float4 col:COLOR; };\n"
	"struct VSOut { float4 pos:SV_POSITION; float4 col:COLOR; };\n"
	"VSOut VS(VSIn i){ VSOut o; o.pos=float4(i.pos.xy+offset, i.pos.z, 1); o.col=i.col*tint; return o; }\n"
	"float4 PS(VSOut i):SV_TARGET { return i.col; }\n";

unsigned d3d11_scene_fnv1a(const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *)p;
	unsigned h = 2166136261u;
	size_t i;
	for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
	return h;
}

void d3d11_scene_seed_state(D3d11SceneState *s, unsigned seed)
{
	memset(s, 0, sizeof(*s));
	s->seed = seed;
	s->num_quads = 12u + (seed % (D3D11_SCENE_MAX_QUADS - 11u));
	s->tint[0] = 0.15f + 0.7f * ((seed * 2654435761u >> 8) & 0xff) / 255.0f;
	s->tint[1] = 0.15f + 0.7f * ((seed * 40503u >> 4) & 0xff) / 255.0f;
	s->tint[2] = 0.15f + 0.7f * ((seed * 2246822519u >> 12) & 0xff) / 255.0f;
	s->tint[3] = 1.0f;
	s->global_off[0] = -0.25f + 0.5f * ((seed * 22695477u >> 16) & 0xff) / 255.0f;
	s->global_off[1] = -0.25f + 0.5f * ((seed * 3266489917u >> 20) & 0xff) / 255.0f;
}

static void build_vertices(const D3d11SceneState *st, Vtx *out, unsigned *nvert)
{
	unsigned i, n = 0;
	unsigned q;
	for (q = 0; q < st->num_quads && q < D3D11_SCENE_MAX_QUADS; q++) {
		unsigned h = st->seed + q * 7919u;
		float cx = -0.85f + 1.7f * ((h * 1103515245u >> 16) & 0xffff) / 65535.0f;
		float cy = -0.85f + 1.7f * ((h * 2654435761u >> 16) & 0xffff) / 65535.0f;
		float hx = 0.04f + 0.12f * ((h * 2246822519u >> 8) & 0xff) / 255.0f;
		float hy = 0.03f + 0.10f * ((h * 3266489917u >> 8) & 0xff) / 255.0f;
		float r = 0.2f + 0.8f * ((h >> 4) & 0xff) / 255.0f;
		float g = 0.2f + 0.8f * ((h >> 12) & 0xff) / 255.0f;
		float b = 0.2f + 0.8f * ((h >> 20) & 0xff) / 255.0f;
		Vtx quad[6];
		quad[0] = (Vtx){ cx - hx, cy + hy, 0, r, g, b, 1 };
		quad[1] = (Vtx){ cx + hx, cy + hy, 0, r * 0.9f, g * 0.85f, b, 1 };
		quad[2] = (Vtx){ cx - hx, cy - hy, 0, r * 0.8f, g, b * 0.9f, 1 };
		quad[3] = (Vtx){ cx + hx, cy + hy, 0, r * 0.95f, g * 0.7f, b * 0.8f, 1 };
		quad[4] = (Vtx){ cx + hx, cy - hy, 0, r, g * 0.9f, b * 0.7f, 1 };
		quad[5] = (Vtx){ cx - hx, cy - hy, 0, r * 0.85f, g * 0.8f, b, 1 };
		for (i = 0; i < 6; i++) out[n++] = quad[i];
	}
	/* extra triangle fan for depth in overdraw */
	for (i = 0; i < 8 && n + 3 <= D3D11_SCENE_MAX_QUADS * 6; i++) {
		unsigned h = st->seed ^ (i * 0x9e3779b9u);
		float cx = -0.5f + 1.0f * ((h >> 8) & 0xff) / 255.0f;
		float cy = -0.5f + 1.0f * ((h >> 16) & 0xff) / 255.0f;
		float sz = 0.08f + 0.15f * ((h >> 24) & 0xff) / 255.0f;
		out[n++] = (Vtx){ cx, cy + sz, 0, 1, 0.4f, 0.2f, 1 };
		out[n++] = (Vtx){ cx - sz, cy - sz, 0, 0.9f, 0.2f, 0.3f, 1 };
		out[n++] = (Vtx){ cx + sz, cy - sz, 0, 0.8f, 0.3f, 0.2f, 1 };
	}
	*nvert = n;
}

static int upload_vertices(D3d11Scene *sc, const D3d11SceneState *st)
{
	Vtx verts[D3D11_SCENE_MAX_QUADS * 6 + 32];
	unsigned nvert = 0;
	build_vertices(st, verts, &nvert);
	ID3D11DeviceContext_UpdateSubresource(sc->ctx, (ID3D11Resource *)sc->vb, 0, NULL,
					      verts, 0, 0);
	return (int)nvert;
}

static int create_ballast(D3d11Scene *sc, unsigned seed)
{
	D3D11_TEXTURE2D_DESC td;
	D3D11_SUBRESOURCE_DATA sd;
	unsigned char *pix;
	unsigned i, x, y;
	HRESULT hr;

	ZeroMemory(&td, sizeof(td));
	td.Width = 256;
	td.Height = 256;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_IMMUTABLE;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	pix = (unsigned char *)malloc(256u * 256u * 4u);
	if (!pix) return 0;

	for (i = 0; i < D3D11_SCENE_BALLAST_TEX; i++) {
		unsigned base = seed + i * 104729u;
		for (y = 0; y < 256; y++)
			for (x = 0; x < 256; x++) {
				unsigned idx = (y * 256 + x) * 4;
				pix[idx + 0] = (unsigned char)((base + x * 3 + y) & 0xff);
				pix[idx + 1] = (unsigned char)((base >> 8) + x) & 0xff;
				pix[idx + 2] = (unsigned char)((base >> 16) + y) & 0xff;
				pix[idx + 3] = 255;
			}
		ZeroMemory(&sd, sizeof(sd));
		sd.pSysMem = pix;
		sd.SysMemPitch = 256 * 4;
		hr = ID3D11Device_CreateTexture2D(sc->dev, &td, &sd, &sc->ballast[i]);
		if (FAILED(hr)) {
			free(pix);
			return 0;
		}
		sc->ballast_n++;
	}
	free(pix);
	return 1;
}

int d3d11_scene_init(D3d11Scene *sc, unsigned ballast_seed)
{
	D3D_FEATURE_LEVEL fl = 0, want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	D3D_DRIVER_TYPE drv[3] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_REFERENCE };
	const char *dn[3] = { "HARDWARE", "WARP", "REFERENCE" };
	HRESULT hr = E_FAIL;
	int i;
	D3D11_TEXTURE2D_DESC td;
	D3D11_BUFFER_DESC bd;
	D3D11_SUBRESOURCE_DATA sd;
	HMODULE dc;
	PFN_D3DCompile D3DCompile;
	ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
	D3D11_INPUT_ELEMENT_DESC ild[2] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	D3d11SceneState init;
	Vtx dummy[D3D11_SCENE_MAX_QUADS * 6 + 32];

	memset(sc, 0, sizeof(*sc));
	sc->driver_name = "?";

	for (i = 0; i < 3; i++) {
		hr = D3D11CreateDevice(NULL, drv[i], NULL, 0, want, 2, D3D11_SDK_VERSION, &sc->dev, &fl, &sc->ctx);
		if (SUCCEEDED(hr)) { sc->driver_name = dn[i]; break; }
	}
	if (FAILED(hr)) return 0;

	ZeroMemory(&td, sizeof(td));
	td.Width = D3D11_SCENE_W;
	td.Height = D3D11_SCENE_H;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET;
	if (FAILED(ID3D11Device_CreateTexture2D(sc->dev, &td, NULL, &sc->rt))) return 0;
	if (FAILED(ID3D11Device_CreateRenderTargetView(sc->dev, (ID3D11Resource *)sc->rt, NULL, &sc->rtv))) return 0;
	td.Usage = D3D11_USAGE_STAGING;
	td.BindFlags = 0;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(ID3D11Device_CreateTexture2D(sc->dev, &td, NULL, &sc->stg))) return 0;

	dc = LoadLibraryA("d3dcompiler_47.dll");
	D3DCompile = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : NULL;
	if (!D3DCompile) return 0;
	if (FAILED(D3DCompile(HLSL, strlen(HLSL), NULL, NULL, NULL, "VS", "vs_4_0", 0, 0, &vsb, &err)) ||
	    FAILED(D3DCompile(HLSL, strlen(HLSL), NULL, NULL, NULL, "PS", "ps_4_0", 0, 0, &psb, &err)))
		return 0;
	ID3D11Device_CreateVertexShader(sc->dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &sc->vs);
	ID3D11Device_CreatePixelShader(sc->dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &sc->ps);
	ID3D11Device_CreateInputLayout(sc->dev, ild, 2, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &sc->il);
	ID3D10Blob_Release(vsb);
	ID3D10Blob_Release(psb);

	ZeroMemory(&bd, sizeof(bd));
	bd.ByteWidth = (UINT)(sizeof(dummy));
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	ZeroMemory(&sd, sizeof(sd));
	sd.pSysMem = dummy;
	if (FAILED(ID3D11Device_CreateBuffer(sc->dev, &bd, &sd, &sc->vb))) return 0;

	ZeroMemory(&bd, sizeof(bd));
	bd.ByteWidth = 32;
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	d3d11_scene_seed_state(&init, 1);
	init.pad[0] = init.global_off[0];
	init.pad[1] = init.global_off[1];
	ZeroMemory(&sd, sizeof(sd));
	sd.pSysMem = init.tint;
	if (FAILED(ID3D11Device_CreateBuffer(sc->dev, &bd, &sd, &sc->cb))) return 0;

	if (!create_ballast(sc, ballast_seed)) return 0;
	return 1;
}

void d3d11_scene_fill_refs(const D3d11Scene *sc, D3d11SceneSnapRefs *refs)
{
	memset(refs, 0, sizeof(*refs));
	refs->saved_pid = GetCurrentProcessId();
	refs->dev = (uintptr_t)sc->dev;
	refs->ctx = (uintptr_t)sc->ctx;
	refs->cb = (uintptr_t)sc->cb;
	refs->vb = (uintptr_t)sc->vb;
	refs->rt = (uintptr_t)sc->rt;
	refs->rtv = (uintptr_t)sc->rtv;
}

int d3d11_scene_device_recreated(const D3d11SceneSnapRefs *saved, const D3d11Scene *live)
{
	if (!saved || !live || !live->dev) return 0;
	return saved->dev != 0 && (void *)saved->dev != (void *)live->dev;
}

unsigned d3d11_scene_render(const D3d11Scene *sc, const D3d11SceneState *st, unsigned char *packed_rgba)
{
	float clear[4] = { 0.05f, 0.04f, 0.12f, 1.0f };
	unsigned char cb_blob[32];
	D3D11_VIEWPORT vp;
	UINT stride = sizeof(Vtx), off = 0;
	D3D11_MAPPED_SUBRESOURCE m;
	unsigned y, h = 0;
	int nvert;
	D3d11Scene *mut = (D3d11Scene *)sc;

	memcpy(cb_blob, st->tint, 16);
	memcpy(cb_blob + 16, st->global_off, 8);
	memset(cb_blob + 24, 0, 8);
	ID3D11DeviceContext_UpdateSubresource(mut->ctx, (ID3D11Resource *)mut->cb, 0, NULL, cb_blob, 0, 0);

	nvert = upload_vertices(mut, st);

	ID3D11DeviceContext_OMSetRenderTargets(mut->ctx, 1, &mut->rtv, NULL);
	ZeroMemory(&vp, sizeof(vp));
	vp.Width = (FLOAT)D3D11_SCENE_W;
	vp.Height = (FLOAT)D3D11_SCENE_H;
	vp.MaxDepth = 1.0f;
	ID3D11DeviceContext_RSSetViewports(mut->ctx, 1, &vp);
	ID3D11DeviceContext_ClearRenderTargetView(mut->ctx, mut->rtv, clear);
	ID3D11DeviceContext_IASetInputLayout(mut->ctx, mut->il);
	ID3D11DeviceContext_IASetVertexBuffers(mut->ctx, 0, 1, &mut->vb, &stride, &off);
	ID3D11DeviceContext_IASetPrimitiveTopology(mut->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_VSSetShader(mut->ctx, mut->vs, NULL, 0);
	ID3D11DeviceContext_PSSetShader(mut->ctx, mut->ps, NULL, 0);
	ID3D11DeviceContext_VSSetConstantBuffers(mut->ctx, 0, 1, &mut->cb);
	ID3D11DeviceContext_Draw(mut->ctx, (UINT)nvert, 0);
	ID3D11DeviceContext_Flush(mut->ctx);

	ID3D11DeviceContext_CopyResource(mut->ctx, (ID3D11Resource *)mut->stg, (ID3D11Resource *)mut->rt);
	if (SUCCEEDED(ID3D11DeviceContext_Map(mut->ctx, (ID3D11Resource *)mut->stg, 0, D3D11_MAP_READ, 0, &m))) {
		for (y = 0; y < D3D11_SCENE_H; y++)
			memcpy(packed_rgba + y * D3D11_SCENE_W * 4, (char *)m.pData + y * m.RowPitch, D3D11_SCENE_W * 4);
		ID3D11DeviceContext_Unmap(mut->ctx, (ID3D11Resource *)mut->stg, 0);
	}
	h = d3d11_scene_fnv1a(packed_rgba, D3D11_SCENE_PIXBYTES);
	return h;
}

void d3d11_scene_flush(const D3d11Scene *sc)
{
	if (sc && sc->ctx)
		ID3D11DeviceContext_Flush(sc->ctx);
}

void d3d11_scene_shutdown(D3d11Scene *sc)
{
	unsigned i;
	if (!sc) return;
	for (i = 0; i < sc->ballast_n; i++)
		if (sc->ballast[i]) ID3D11Texture2D_Release(sc->ballast[i]);
	if (sc->il) ID3D11InputLayout_Release(sc->il);
	if (sc->vs) ID3D11VertexShader_Release(sc->vs);
	if (sc->ps) ID3D11PixelShader_Release(sc->ps);
	if (sc->vb) ID3D11Buffer_Release(sc->vb);
	if (sc->cb) ID3D11Buffer_Release(sc->cb);
	if (sc->rtv) ID3D11RenderTargetView_Release(sc->rtv);
	if (sc->stg) ID3D11Texture2D_Release(sc->stg);
	if (sc->rt) ID3D11Texture2D_Release(sc->rt);
	if (sc->ctx) ID3D11DeviceContext_Release(sc->ctx);
	if (sc->dev) ID3D11Device_Release(sc->dev);
	memset(sc, 0, sizeof(*sc));
}
