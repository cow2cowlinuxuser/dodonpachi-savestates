/*
 * d3d11_probe - the smallest useful "GPU" D3D11 program, run under Wine on a CPU
 * software backend (llvmpipe/lavapipe), headless.
 *
 * This is the first step toward the real question: can GPU-side D3D11 state be
 * made to die and live by the same in-process restore mechanism the CPU-side
 * savestate uses? Before any of that, we need a D3D11 device that renders
 * without a real GPU and without a window. This program:
 *
 *   1. creates a D3D11 device (HARDWARE -> WARP -> REFERENCE fallback; under Wine
 *      HARDWARE routes through wined3d to the software Vulkan/GL driver),
 *   2. makes an offscreen render target (no swapchain, no HWND - headless),
 *   3. clears it, then draws one shaded triangle with runtime-compiled shaders,
 *   4. copies to a staging texture, maps it, and dumps the pixels,
 *
 * so we can SEE that the whole D3D11 pipeline ran on the CPU. The rendered image
 * and the reported feature level / driver are the evidence.
 *
 * Build (see build.sh): zig cc -target x86_64-windows-gnu ... -ld3d11 -ldxgi
 * Run:   WINEPREFIX=... DISPLAY=:1 wine d3d11_probe.exe [out.ppm]
 */

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifndef D3D11_SDK_VERSION
#define D3D11_SDK_VERSION 7
#endif

/* D3DCompile is loaded dynamically from d3dcompiler_47.dll to avoid import-lib
 * naming differences between toolchains. */
typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void *,
					 void *, LPCSTR, LPCSTR, UINT, UINT,
					 ID3DBlob **, ID3DBlob **);

#define W 256
#define H 256

static const char *HLSL =
	"struct VSIn  { float3 pos:POSITION; float4 col:COLOR; };\n"
	"struct VSOut { float4 pos:SV_POSITION; float4 col:COLOR; };\n"
	"VSOut VS(VSIn i){ VSOut o; o.pos=float4(i.pos,1); o.col=i.col; return o; }\n"
	"float4 PS(VSOut i):SV_TARGET { return i.col; }\n";

typedef struct { float x, y, z; float r, g, b, a; } Vtx;

static int dump_ppm(const char *path, const unsigned char *rgba, int pitch)
{
	FILE *f = fopen(path, "wb");
	int x, y;
	if (!f) return 0;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++) {
			const unsigned char *p = rgba + y * pitch + x * 4;
			fputc(p[0], f); fputc(p[1], f); fputc(p[2], f); /* R G B */
		}
	fclose(f);
	return 1;
}

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "d3d11_out.ppm";
	ID3D11Device *dev = NULL;
	ID3D11DeviceContext *ctx = NULL;
	D3D_FEATURE_LEVEL fl = 0;
	D3D_DRIVER_TYPE tried[3] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_REFERENCE };
	const char *tried_name[3] = { "HARDWARE", "WARP", "REFERENCE" };
	D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
	HRESULT hr = E_FAIL;
	int i;

	ID3D11Texture2D *rt = NULL, *stg = NULL;
	ID3D11RenderTargetView *rtv = NULL;
	D3D11_TEXTURE2D_DESC td;
	D3D11_VIEWPORT vp;
	float clear[4] = { 1.0f, 0.0f, 0.5f, 1.0f }; /* magenta, like the d3d9 test */

	for (i = 0; i < 3; i++) {
		hr = D3D11CreateDevice(NULL, tried[i], NULL, 0, want, 3, D3D11_SDK_VERSION, &dev, &fl, &ctx);
		if (SUCCEEDED(hr)) { printf("D3D11 device: driver=%s feature_level=0x%04x\n", tried_name[i], (unsigned)fl); break; }
		printf("D3D11CreateDevice(%s) failed hr=0x%08lx\n", tried_name[i], (unsigned long)hr);
	}
	if (FAILED(hr)) { printf("no D3D11 device available\n"); return 1; }

	/* offscreen render target - no swapchain, no window */
	ZeroMemory(&td, sizeof(td));
	td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
	hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt);
	if (FAILED(hr)) { printf("CreateTexture2D(RT) failed 0x%08lx\n", (unsigned long)hr); return 1; }
	hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv);
	if (FAILED(hr)) { printf("CreateRenderTargetView failed 0x%08lx\n", (unsigned long)hr); return 1; }

	ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
	ZeroMemory(&vp, sizeof(vp)); vp.Width = (FLOAT)W; vp.Height = (FLOAT)H; vp.MaxDepth = 1.0f;
	ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
	ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);

	/* one shaded triangle, shaders compiled at runtime */
	{
		HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
		PFN_D3DCompile D3DCompile = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : NULL;
		ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
		int drew = 0;
		if (!D3DCompile) {
			printf("note: d3dcompiler_47 unavailable, drawing clear only\n");
		} else if (SUCCEEDED(D3DCompile(HLSL, strlen(HLSL), NULL, NULL, NULL, "VS", "vs_4_0", 0, 0, &vsb, &err)) &&
			   SUCCEEDED(D3DCompile(HLSL, strlen(HLSL), NULL, NULL, NULL, "PS", "ps_4_0", 0, 0, &psb, &err))) {
			ID3D11VertexShader *vs = NULL; ID3D11PixelShader *ps = NULL;
			ID3D11InputLayout *il = NULL; ID3D11Buffer *vb = NULL;
			D3D11_INPUT_ELEMENT_DESC il_desc[2] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};
			Vtx verts[3] = {
				{  0.0f,  0.8f, 0.0f,  1, 1, 1, 1 },
				{  0.8f, -0.8f, 0.0f,  1, 0.85f, 0.16f, 1 }, /* yellow */
				{ -0.8f, -0.8f, 0.0f,  0.16f, 0.70f, 1, 1 }, /* blue */
			};
			D3D11_BUFFER_DESC bd; D3D11_SUBRESOURCE_DATA sd;
			UINT stride = sizeof(Vtx), off = 0;

			ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
			ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);
			ID3D11Device_CreateInputLayout(dev, il_desc, 2, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &il);

			ZeroMemory(&bd, sizeof(bd)); bd.ByteWidth = sizeof(verts); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			ZeroMemory(&sd, sizeof(sd)); sd.pSysMem = verts;
			ID3D11Device_CreateBuffer(dev, &bd, &sd, &vb);

			if (vs && ps && il && vb) {
				ID3D11DeviceContext_IASetInputLayout(ctx, il);
				ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &off);
				ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
				ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
				ID3D11DeviceContext_Draw(ctx, 3, 0);
				drew = 1;
			}
			if (vb) ID3D11Buffer_Release(vb);
			if (il) ID3D11InputLayout_Release(il);
			if (ps) ID3D11PixelShader_Release(ps);
			if (vs) ID3D11VertexShader_Release(vs);
		} else {
			printf("shader compile failed%s\n", err ? "" : " (no error blob)");
			if (err) printf("  %.*s\n", (int)ID3D10Blob_GetBufferSize(err), (char *)ID3D10Blob_GetBufferPointer(err));
		}
		if (vsb) ID3D10Blob_Release(vsb);
		if (psb) ID3D10Blob_Release(psb);
		printf("triangle: %s\n", drew ? "drawn" : "skipped (clear only)");
	}

	ID3D11DeviceContext_Flush(ctx);

	/* read back through a staging texture */
	td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &stg);
	if (FAILED(hr)) { printf("CreateTexture2D(staging) failed 0x%08lx\n", (unsigned long)hr); return 1; }
	ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stg, (ID3D11Resource *)rt);

	{
		D3D11_MAPPED_SUBRESOURCE m;
		hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m);
		if (FAILED(hr)) { printf("Map failed 0x%08lx\n", (unsigned long)hr); return 1; }
		{
			const unsigned char *px = (const unsigned char *)m.pData;
			const unsigned char *c = px + (H / 2) * m.RowPitch + (W / 2) * 4;
			const unsigned char *tl = px; /* corner should be the clear colour */
			printf("center pixel rgba=%u,%u,%u,%u  corner rgba=%u,%u,%u,%u  pitch=%u\n",
			       c[0], c[1], c[2], c[3], tl[0], tl[1], tl[2], tl[3], (unsigned)m.RowPitch);
			dump_ppm(out, px, (int)m.RowPitch);
		}
		ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg, 0);
	}
	printf("wrote %s (%dx%d) - D3D11 pipeline ran on the CPU backend\n", out, W, H);

	if (stg) ID3D11Texture2D_Release(stg);
	if (rtv) ID3D11RenderTargetView_Release(rtv);
	if (rt) ID3D11Texture2D_Release(rt);
	if (ctx) ID3D11DeviceContext_Release(ctx);
	if (dev) ID3D11Device_Release(dev);
	return 0;
}
