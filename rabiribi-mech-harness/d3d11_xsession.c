/*
 * d3d11_xsession - cross-session D3D11 state restore, on the CPU backend.
 *
 * The question: can we close the program, reopen it, and reproduce a previously
 * taken snapshot? Each run is a SEPARATE process with a brand-new ID3D11Device
 * (and, under Wine, a fresh wined3d instance), so a match proves two things at
 * once:
 *
 *   - we coexist with wined3d well enough that the 3D state is reproducible, and
 *   - a snapshot persisted to disk in one session reproduces bit-for-bit in the
 *     next session's device.
 *
 * A snapshot holds the GPU-resident logical state (a constant buffer the shaders
 * read) plus the exact framebuffer that state produced. "save" renders and
 * writes both to disk, then exits. "restore" starts fresh, loads the snapshot,
 * writes the logical state into a NEW device's constant buffer, re-renders, and
 * compares the new frame against the stored one - hash and pixel-for-pixel.
 *
 *   wine d3d11_xsession.exe save    snap.bin [seed]
 *   wine d3d11_xsession.exe restore snap.bin [out.ppm]
 *
 * Determinism note: the draw is an opaque triangle with no blending, so the
 * software rasteriser's output is a pure function of the logical state; that is
 * what makes cross-session reproduction bit-exact rather than approximate.
 */

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifndef D3D11_SDK_VERSION
#define D3D11_SDK_VERSION 7
#endif

typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void *,
					 void *, LPCSTR, LPCSTR, UINT, UINT,
					 ID3DBlob **, ID3DBlob **);

#define W 128
#define H 128
#define PIXBYTES (W * H * 4)

typedef struct State { float tint[4]; float offset[2]; float pad[2]; } State;

typedef struct SnapHdr {
	char magic[8];   /* "D3D11XS1" */
	unsigned ver;
	State state;
	unsigned fp;     /* fingerprint of the stored frame */
	unsigned w, h;
} SnapHdr;

static const char *HLSL =
	"cbuffer State : register(b0) { float4 tint; float2 offset; float2 pad; };\n"
	"struct VSIn  { float3 pos:POSITION; float4 col:COLOR; };\n"
	"struct VSOut { float4 pos:SV_POSITION; float4 col:COLOR; };\n"
	"VSOut VS(VSIn i){ VSOut o; o.pos=float4(i.pos.xy+offset, i.pos.z, 1); o.col=i.col*tint; return o; }\n"
	"float4 PS(VSOut i):SV_TARGET { return i.col; }\n";

typedef struct { float x, y, z; float r, g, b, a; } Vtx;

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11Texture2D *rt, *stg;
static ID3D11RenderTargetView *rtv;
static ID3D11Buffer *cb, *vb;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps;
static ID3D11InputLayout *il;
static const char *g_driver = "?";

static unsigned fnv1a(const void *p, size_t n)
{ const unsigned char *b = p; unsigned h = 2166136261u; size_t i; for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; } return h; }

static int setup(void)
{
	D3D_FEATURE_LEVEL fl = 0, want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	D3D_DRIVER_TYPE drv[3] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_REFERENCE };
	const char *dn[3] = { "HARDWARE", "WARP", "REFERENCE" };
	HRESULT hr = E_FAIL; int i;
	D3D11_TEXTURE2D_DESC td; D3D11_BUFFER_DESC bd; D3D11_SUBRESOURCE_DATA sd;
	HMODULE dc; PFN_D3DCompile D3DCompile; ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
	D3D11_INPUT_ELEMENT_DESC ild[2] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	Vtx verts[3] = {
		{  0.0f,  0.8f, 0.0f,  1, 1, 1, 1 },
		{  0.8f, -0.8f, 0.0f,  1, 0.85f, 0.16f, 1 },
		{ -0.8f, -0.8f, 0.0f,  0.16f, 0.70f, 1, 1 },
	};
	State init = { { 1, 1, 1, 1 }, { 0, 0 }, { 0, 0 } };

	for (i = 0; i < 3; i++) { hr = D3D11CreateDevice(NULL, drv[i], NULL, 0, want, 2, D3D11_SDK_VERSION, &dev, &fl, &ctx);
		if (SUCCEEDED(hr)) { g_driver = dn[i]; break; } }
	if (FAILED(hr)) return 0;

	ZeroMemory(&td, sizeof(td)); td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
	if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt))) return 0;
	if (FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv))) return 0;
	td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &stg))) return 0;

	dc = LoadLibraryA("d3dcompiler_47.dll");
	D3DCompile = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : NULL;
	if (!D3DCompile) return 0;
	if (FAILED(D3DCompile(HLSL, strlen(HLSL), NULL, NULL, NULL, "VS", "vs_4_0", 0, 0, &vsb, &err)) ||
	    FAILED(D3DCompile(HLSL, strlen(HLSL), NULL, NULL, NULL, "PS", "ps_4_0", 0, 0, &psb, &err))) return 0;
	ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
	ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);
	ID3D11Device_CreateInputLayout(dev, ild, 2, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &il);
	ID3D10Blob_Release(vsb); ID3D10Blob_Release(psb);

	ZeroMemory(&bd, sizeof(bd)); bd.ByteWidth = sizeof(verts); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	ZeroMemory(&sd, sizeof(sd)); sd.pSysMem = verts;
	if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, &sd, &vb))) return 0;
	ZeroMemory(&bd, sizeof(bd)); bd.ByteWidth = sizeof(State); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	ZeroMemory(&sd, sizeof(sd)); sd.pSysMem = &init;
	if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, &sd, &cb))) return 0;
	return 1;
}

/* Render with the given logical state; return the frame's fingerprint and the
 * packed RGBA pixels (W*H*4). */
static unsigned render_capture(const State *s, unsigned char *packed)
{
	float clear[4] = { 1.0f, 0.0f, 0.5f, 1.0f };
	D3D11_VIEWPORT vp; UINT stride = sizeof(Vtx), off = 0; D3D11_MAPPED_SUBRESOURCE m; unsigned h = 0, y;

	ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cb, 0, NULL, s, 0, 0);
	ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
	ZeroMemory(&vp, sizeof(vp)); vp.Width = (FLOAT)W; vp.Height = (FLOAT)H; vp.MaxDepth = 1.0f;
	ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
	ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
	ID3D11DeviceContext_IASetInputLayout(ctx, il);
	ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &off);
	ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
	ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
	ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb);
	ID3D11DeviceContext_Draw(ctx, 3, 0);
	ID3D11DeviceContext_Flush(ctx);

	ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stg, (ID3D11Resource *)rt);
	if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
		for (y = 0; y < H; y++) memcpy(packed + y * W * 4, (char *)m.pData + y * m.RowPitch, W * 4);
		ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg, 0);
	}
	h = fnv1a(packed, PIXBYTES);
	return h;
}

static int write_ppm(const char *path, const unsigned char *packed)
{
	FILE *f = fopen(path, "wb"); unsigned x, y;
	if (!f) return 0;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (y = 0; y < H; y++) for (x = 0; x < W; x++) { const unsigned char *p = packed + (y * W + x) * 4; fputc(p[0], f); fputc(p[1], f); fputc(p[2], f); }
	fclose(f); return 1;
}

static void seed_state(State *s, unsigned k)
{
	s->tint[0] = 0.2f + 0.6f * ((k * 2654435761u >> 8) & 0xff) / 255.0f;
	s->tint[1] = 0.2f + 0.6f * ((k * 40503u >> 4) & 0xff) / 255.0f;
	s->tint[2] = 0.2f + 0.6f * ((k * 2246822519u >> 12) & 0xff) / 255.0f;
	s->tint[3] = 1.0f;
	s->offset[0] = -0.3f + 0.6f * ((k * 22695477u >> 16) & 0xff) / 255.0f;
	s->offset[1] = -0.3f + 0.6f * ((k * 3266489917u >> 20) & 0xff) / 255.0f;
	s->pad[0] = s->pad[1] = 0.0f;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";
	const char *file = argc > 2 ? argv[2] : "snap.bin";
	unsigned char *pix = (unsigned char *)malloc(PIXBYTES);

	if (!pix) return 2;
	if (!setup()) { printf("no D3D11 device\n"); return 2; }
	printf("pid=%lu driver=%s\n", (unsigned long)GetCurrentProcessId(), g_driver);

	if (strcmp(cmd, "save") == 0) {
		unsigned seed = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : 1u;
		State s; SnapHdr hdr; FILE *f; unsigned fp;
		seed_state(&s, seed);
		fp = render_capture(&s, pix);
		memset(&hdr, 0, sizeof(hdr)); memcpy(hdr.magic, "D3D11XS1", 8); hdr.ver = 1; hdr.state = s; hdr.fp = fp; hdr.w = W; hdr.h = H;
		f = fopen(file, "wb");
		if (!f) { printf("cannot write %s\n", file); return 2; }
		fwrite(&hdr, sizeof(hdr), 1, f); fwrite(pix, 1, PIXBYTES, f); fclose(f);
		printf("SAVE seed=%u tint=(%.2f,%.2f,%.2f) off=(%.2f,%.2f) fp=0x%08x -> %s (%u bytes)\n",
		       seed, s.tint[0], s.tint[1], s.tint[2], s.offset[0], s.offset[1], fp,
		       file, (unsigned)(sizeof(hdr) + PIXBYTES));
		return 0;
	}

	if (strcmp(cmd, "restore") == 0) {
		const char *out = argc > 3 ? argv[3] : NULL;
		SnapHdr hdr; FILE *f; unsigned char *saved = (unsigned char *)malloc(PIXBYTES);
		unsigned fp; unsigned diff = 0, i;
		if (!saved) return 2;
		f = fopen(file, "rb");
		if (!f) { printf("cannot read %s\n", file); return 2; }
		if (fread(&hdr, sizeof(hdr), 1, f) != 1 || memcmp(hdr.magic, "D3D11XS1", 8) != 0 ||
		    hdr.w != W || hdr.h != H) { printf("bad snapshot\n"); fclose(f); return 2; }
		if (fread(saved, 1, PIXBYTES, f) != PIXBYTES) { printf("short snapshot\n"); fclose(f); return 2; }
		fclose(f);

		/* reproduce into THIS fresh process's device */
		fp = render_capture(&hdr.state, pix);
		for (i = 0; i < PIXBYTES; i++) if (pix[i] != saved[i]) diff++;
		if (out) write_ppm(out, pix);

		printf("RESTORE loaded tint=(%.2f,%.2f,%.2f) off=(%.2f,%.2f) snapshot_fp=0x%08x\n",
		       hdr.state.tint[0], hdr.state.tint[1], hdr.state.tint[2],
		       hdr.state.offset[0], hdr.state.offset[1], hdr.fp);
		printf("        reproduced_fp=0x%08x  fp_match=%s  pixel_diffs=%u/%u\n",
		       fp, (fp == hdr.fp) ? "YES" : "NO", diff, (unsigned)PIXBYTES);
		{
			int ok = (fp == hdr.fp) && (diff == 0);
			printf("%s: a fresh process reproduced the snapshot's frame %s\n",
			       ok ? "PASS" : "FAIL", ok ? "bit-for-bit" : "with differences");
			free(saved); free(pix);
			return ok ? 0 : 1;
		}
	}

	printf("usage: %s save <file> [seed] | restore <file> [out.ppm]\n", argv[0]);
	return 2;
}
