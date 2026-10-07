/*
 * d3d11_state_harness - save/restore of D3D11 "device" state, on the CPU backend.
 *
 * This does not (and cannot, here) rewind true driver/GPU memory. What it does
 * is treat the D3D11 device's own observable state as the thing to checkpoint,
 * and answer the two questions that matter for a savestate:
 *
 *   1. Does the device keep existing across save/restore cycles? (one persistent
 *      ID3D11Device + its resources are reused for every cycle; the device is
 *      polled for removal each cycle.)
 *   2. When we save and restore, can we OBSERVE mutations - and see exactly what
 *      the comparison looks like - and, on a clean restore, VALIDATE that there
 *      is no visible mutation?
 *
 * Two views of "state" are compared every cycle:
 *
 *   - LOGICAL state: a GPU-resident constant buffer (tint + offset) that the
 *     shaders read. We read it back FROM the device (CopyResource to a staging
 *     buffer, Map) - so the comparison is against what the device actually
 *     holds, not a CPU shadow.
 *   - VISIBLE state: the rendered framebuffer. We draw a triangle whose colour
 *     and position are a function of the constant buffer, read the render target
 *     back, and fingerprint it (FNV-1a). This is what the GPU actually produced.
 *
 * Per cycle, by mode:
 *   NOOP         save -> (no change) -> compare        : expect no visible mutation
 *   MUTATE+RESTORE  save -> mutate (observe change) -> restore -> compare
 *                                                      : mutation seen, then gone
 *   MUTATE-only  save -> mutate -> compare (control)   : mutation observed, persists
 *
 * "Restore" here means writing the saved constant-buffer bytes back into the GPU
 * resource (UpdateSubresource) and re-rendering - the content-level restore a
 * real savestate would have to reproduce for the visible frame.
 *
 * Build (build.sh): zig cc -target x86_64-windows-gnu ... -ld3d11 -ldxgi
 * Run:  WINEPREFIX=... DISPLAY=:1 wine d3d11_state_harness.exe [cycles]
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

typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void *,
					 void *, LPCSTR, LPCSTR, UINT, UINT,
					 ID3DBlob **, ID3DBlob **);

#define W 128
#define H 128

/* GPU-resident logical state (a constant buffer). 32 bytes, 16-aligned. */
typedef struct State {
	float tint[4];   /* multiplies vertex colour */
	float offset[2]; /* shifts the triangle in clip space */
	float pad[2];
} State;

static const char *HLSL =
	"cbuffer State : register(b0) { float4 tint; float2 offset; float2 pad; };\n"
	"struct VSIn  { float3 pos:POSITION; float4 col:COLOR; };\n"
	"struct VSOut { float4 pos:SV_POSITION; float4 col:COLOR; };\n"
	"VSOut VS(VSIn i){ VSOut o; o.pos=float4(i.pos.xy+offset, i.pos.z, 1); o.col=i.col*tint; return o; }\n"
	"float4 PS(VSOut i):SV_TARGET { return i.col; }\n";

typedef struct { float x, y, z; float r, g, b, a; } Vtx;

/* ---- device + pipeline objects (persistent for the whole run) ---- */
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11Texture2D *rt, *stgRT;
static ID3D11RenderTargetView *rtv;
static ID3D11Buffer *cb, *stgCB, *vb;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps;
static ID3D11InputLayout *il;

static unsigned fnv1a(const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *)p; unsigned h = 2166136261u; size_t i;
	for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
	return h;
}

static void set_state(const State *s) /* write logical state INTO the device */
{ ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cb, 0, NULL, s, 0, 0); }

static void read_state(State *s)       /* read logical state BACK from the device */
{
	D3D11_MAPPED_SUBRESOURCE m;
	ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stgCB, (ID3D11Resource *)cb);
	if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stgCB, 0, D3D11_MAP_READ, 0, &m))) {
		memcpy(s, m.pData, sizeof(State));
		ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stgCB, 0);
	}
}

static void render(void) /* draw the triangle as a function of device state */
{
	float clear[4] = { 1.0f, 0.0f, 0.5f, 1.0f };
	D3D11_VIEWPORT vp; UINT stride = sizeof(Vtx), off = 0;
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
}

static unsigned fingerprint_rt(void) /* hash of what the GPU actually produced */
{
	D3D11_MAPPED_SUBRESOURCE m; unsigned h = 0;
	ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stgRT, (ID3D11Resource *)rt);
	if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stgRT, 0, D3D11_MAP_READ, 0, &m))) {
		unsigned y; for (y = 0; y < H; y++) h ^= fnv1a((char *)m.pData + y * m.RowPitch, W * 4);
		ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stgRT, 0);
	}
	return h;
}

static int dump_ppm(const char *path)
{
	D3D11_MAPPED_SUBRESOURCE m; FILE *f; unsigned x, y;
	ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stgRT, (ID3D11Resource *)rt);
	if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stgRT, 0, D3D11_MAP_READ, 0, &m))) return 0;
	f = fopen(path, "wb");
	if (f) { fprintf(f, "P6\n%d %d\n255\n", W, H);
		for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
			const unsigned char *p = (unsigned char *)m.pData + y * m.RowPitch + x * 4;
			fputc(p[0], f); fputc(p[1], f); fputc(p[2], f);
		} fclose(f); }
	ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stgRT, 0);
	return f != NULL;
}

static int setup(void)
{
	D3D_FEATURE_LEVEL fl = 0, want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	D3D_DRIVER_TYPE drv[3] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_REFERENCE };
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
		if (SUCCEEDED(hr)) { printf("device: driver=%d feature_level=0x%04x\n", (int)drv[i], (unsigned)fl); break; } }
	if (FAILED(hr)) { printf("no D3D11 device\n"); return 0; }

	ZeroMemory(&td, sizeof(td)); td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
	if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt))) return 0;
	if (FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv))) return 0;
	td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &stgRT))) return 0;

	dc = LoadLibraryA("d3dcompiler_47.dll");
	D3DCompile = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : NULL;
	if (!D3DCompile) { printf("d3dcompiler_47 missing\n"); return 0; }
	if (FAILED(D3DCompile(HLSL, strlen(HLSL), NULL, NULL, NULL, "VS", "vs_4_0", 0, 0, &vsb, &err)) ||
	    FAILED(D3DCompile(HLSL, strlen(HLSL), NULL, NULL, NULL, "PS", "ps_4_0", 0, 0, &psb, &err))) {
		printf("shader compile failed\n"); if (err) printf("%.*s\n", (int)ID3D10Blob_GetBufferSize(err), (char *)ID3D10Blob_GetBufferPointer(err)); return 0; }
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
	ZeroMemory(&bd, sizeof(bd)); bd.ByteWidth = sizeof(State); bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &stgCB))) return 0;
	return 1;
}

static void mutate_state(State *s, unsigned k) /* a deterministic "game" mutation */
{
	s->tint[0] = 0.2f + 0.6f * ((k * 2654435761u >> 8) & 0xff) / 255.0f;
	s->tint[1] = 0.2f + 0.6f * ((k * 40503u >> 4) & 0xff) / 255.0f;
	s->tint[2] = 0.2f + 0.6f * ((k * 2246822519u >> 12) & 0xff) / 255.0f;
	s->tint[3] = 1.0f;
	s->offset[0] = -0.3f + 0.6f * ((k * 22695477u >> 16) & 0xff) / 255.0f;
	s->offset[1] = -0.3f + 0.6f * ((k * 3266489917u >> 20) & 0xff) / 255.0f;
}

enum { M_NOOP, M_MUT_RESTORE, M_MUT_ONLY };

int main(int argc, char **argv)
{
	int cycles = argc > 1 ? atoi(argv[1]) : 300;
	int c, mode;
	const char *mode_name[3] = { "NOOP", "MUTATE+RESTORE", "MUTATE-only(control)" };
	unsigned removed_events = 0, draws = 0;

	if (!setup()) { printf("setup failed\n"); return 2; }

	/* one detailed example, so the comparison is concrete */
	{
		State saved, mut, back; unsigned fp_saved, fp_mut, fp_back;
		mutate_state(&saved, 1); set_state(&saved); render(); draws++;
		read_state(&saved); fp_saved = fingerprint_rt();
		mutate_state(&mut, 999); set_state(&mut); render(); draws++;
		read_state(&mut); fp_mut = fingerprint_rt();
		set_state(&saved); render(); draws++;              /* restore */
		read_state(&back); fp_back = fingerprint_rt();
		printf("\nexample cycle:\n");
		printf("  saved   cb tint=(%.2f,%.2f,%.2f) off=(%.2f,%.2f)  RT fp=0x%08x\n",
		       saved.tint[0], saved.tint[1], saved.tint[2], saved.offset[0], saved.offset[1], fp_saved);
		printf("  mutated cb tint=(%.2f,%.2f,%.2f) off=(%.2f,%.2f)  RT fp=0x%08x  %s\n",
		       mut.tint[0], mut.tint[1], mut.tint[2], mut.offset[0], mut.offset[1], fp_mut,
		       (fp_mut != fp_saved) ? "<- mutation OBSERVED" : "(no change?!)");
		printf("  restored cb tint=(%.2f,%.2f,%.2f) off=(%.2f,%.2f)  RT fp=0x%08x  %s\n",
		       back.tint[0], back.tint[1], back.tint[2], back.offset[0], back.offset[1], fp_back,
		       (fp_back == fp_saved && memcmp(&back, &saved, sizeof(State)) == 0) ? "<- restored, NO visible mutation" : "<- MISMATCH");
		dump_ppm("d3d11_state_saved.ppm");
	}

	printf("\n=== per-mode over %d cycles each ===\n", cycles);
	for (mode = 0; mode < 3; mode++) {
		unsigned observed = 0, visible_equal = 0, cb_equal = 0, restored_equal = 0;
		for (c = 0; c < cycles; c++) {
			State saved, cur; unsigned fp_saved, fp_cur;
			mutate_state(&saved, (unsigned)(c + 1)); set_state(&saved); render(); draws++;
			read_state(&saved); fp_saved = fingerprint_rt();

			if (mode == M_NOOP) {
				render(); draws++;                    /* no change, just re-render */
				read_state(&cur); fp_cur = fingerprint_rt();
			} else {
				State mut; mutate_state(&mut, (unsigned)(c * 7 + 13)); set_state(&mut); render(); draws++;
				read_state(&cur); fp_cur = fingerprint_rt();
				if (fp_cur != fp_saved) observed++;   /* mutation seen */
				if (mode == M_MUT_RESTORE) {
					set_state(&saved); render(); draws++;   /* restore */
					read_state(&cur); fp_cur = fingerprint_rt();
				}
			}
			if (fp_cur == fp_saved) visible_equal++;
			if (memcmp(&cur, &saved, sizeof(State)) == 0) cb_equal++;
			if ((mode == M_NOOP || mode == M_MUT_RESTORE) && fp_cur == fp_saved) restored_equal++;

			if (ID3D11Device_GetDeviceRemovedReason(dev) != S_OK) removed_events++;
		}
		if (mode == M_NOOP)
			printf("  %-20s: visible_equal=%u/%d  cb_equal=%u/%d  (expect all equal)\n",
			       mode_name[mode], visible_equal, cycles, cb_equal, cycles);
		else if (mode == M_MUT_RESTORE)
			printf("  %-20s: mutation_observed=%u/%d  then visible_equal=%u/%d  cb_equal=%u/%d\n",
			       mode_name[mode], observed, cycles, visible_equal, cycles, cb_equal, cycles);
		else
			printf("  %-20s: mutation_observed=%u/%d  visible_equal=%u/%d (expect 0)\n",
			       mode_name[mode], observed, cycles, visible_equal, cycles);
	}

	printf("\ndevice: %s across the run (removal events=%u), %u draws total\n",
	       removed_events ? "REMOVED at least once" : "stayed alive", removed_events, draws);
	{
		int ok = (removed_events == 0);
		printf("\n%s: the D3D11 device persisted; mutations were observable and a "
		       "restore left no visible mutation.\n", ok ? "PASS" : "FAIL");
		return ok ? 0 : 1;
	}
}
