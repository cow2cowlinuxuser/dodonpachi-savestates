#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef IDirect3D9 *(WINAPI *PFN_Direct3DCreate9)(UINT);
typedef int(WINAPI *PFN_dump)(IDirect3DDevice9 *, const char *);
typedef int (*PFN_threads)(void);

typedef struct Vert {
	float x, y, z, rhw;
	DWORD color;
	float u, v;
} Vert;

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
	if (msg == WM_DESTROY) {
		PostQuitMessage(0);
		return 0;
	}
	if (msg == WM_KEYDOWN && w == VK_ESCAPE) {
		DestroyWindow(hwnd);
		return 0;
	}
	return DefWindowProcA(hwnd, msg, w, l);
}

static HWND make_window(int w, int h, const char *title)
{
	WNDCLASSA wc;
	RECT r;
	HWND hwnd;

	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = wndproc;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.lpszClassName = "d3d9_sw_test";
	RegisterClassA(&wc);

	r.left = 0;
	r.top = 0;
	r.right = w;
	r.bottom = h;
	AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
	hwnd = CreateWindowA("d3d9_sw_test", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
			     CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL,
			     wc.hInstance, NULL);
	ShowWindow(hwnd, SW_SHOW);
	return hwnd;
}

static void emit_quad4(Vert *v, float x, float y, float s, DWORD color)
{
	v[0] = (Vert){x, y, 0, 1, color, 0, 0};
	v[1] = (Vert){x + s, y, 0, 1, color, 1, 0};
	v[2] = (Vert){x, y + s, 0, 1, color, 0, 1};
	v[3] = (Vert){x + s, y + s, 0, 1, color, 1, 1};
}

static IDirect3DTexture9 *make_sprite_tex(IDirect3DDevice9 *dev)
{
	IDirect3DTexture9 *tex;
	D3DLOCKED_RECT lr;
	DWORD *p;
	int x, y;

	if (FAILED(IDirect3DDevice9_CreateTexture(dev, 16, 16, 1, 0, D3DFMT_A8R8G8B8,
						  D3DPOOL_MANAGED, &tex, NULL)))
		return NULL;
	if (FAILED(IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0))) {
		IDirect3DTexture9_Release(tex);
		return NULL;
	}
	p = (DWORD *)lr.pBits;
	for (y = 0; y < 16; y++) {
		for (x = 0; x < 16; x++) {
			int on = ((x >> 2) ^ (y >> 2)) & 1;
			int dx = x - 8, dy = y - 8;
			int a = (dx * dx + dy * dy < 36) ? 255 : 0;
			p[y * 16 + x] = on ? D3DCOLOR_ARGB(a, 255, 220, 40)
					   : D3DCOLOR_ARGB(a, 40, 180, 255);
		}
	}
	IDirect3DTexture9_UnlockRect(tex, 0);
	return tex;
}

static double qpc_seconds(void)
{
	static LARGE_INTEGER freq;
	LARGE_INTEGER t;
	if (!freq.QuadPart)
		QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart / (double)freq.QuadPart;
}

int main(int argc, char **argv)
{
	HMODULE dll;
	PFN_Direct3DCreate9 create;
	PFN_dump dump = NULL;
	PFN_threads threads_fn = NULL;
	IDirect3D9 *d3d;
	IDirect3DDevice9 *dev;
	D3DPRESENT_PARAMETERS pp;
	HWND hwnd;
	int dump_only = 0, bench = 0, frames_limit = 0;
	int width = 640, height = 480, quads = 8192, sprite = 16;
	const char *dump_path = "clear.tga";
	int i;
	Vert *verts = NULL;
	IDirect3DTexture9 *tex = NULL;
	IDirect3DVertexBuffer9 *vb = NULL;
	IDirect3DIndexBuffer9 *ib = NULL;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
			dump_only = 1;
			dump_path = argv[++i];
		} else if (!strcmp(argv[i], "--bench")) {
			bench = 1;
			width = 1280;
			height = 720;
		} else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
			frames_limit = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--quads") && i + 1 < argc) {
			quads = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			sscanf(argv[++i], "%dx%d", &width, &height);
		}
	}

	dll = LoadLibraryA("d3d9_sw.dll");
	if (!dll) {
		fprintf(stderr, "LoadLibrary d3d9_sw.dll failed (%lu)\n", GetLastError());
		return 1;
	}
	create = (PFN_Direct3DCreate9)GetProcAddress(dll, "Direct3DCreate9");
	dump = (PFN_dump)GetProcAddress(dll, "d3d9_sw_dump_device");
	threads_fn = (PFN_threads)GetProcAddress(dll, "swrast_thread_count");
	if (!create) {
		fprintf(stderr, "Direct3DCreate9 missing\n");
		return 1;
	}

	hwnd = make_window(width, height, bench ? "d3d9_sw — DX9.0c CPU bench" : "d3d9_sw — Clear/Present (CPU)");
	if (!hwnd)
		return 1;

	d3d = create(D3D_SDK_VERSION);
	if (!d3d) {
		fprintf(stderr, "Direct3DCreate9 returned NULL\n");
		return 1;
	}

	memset(&pp, 0, sizeof(pp));
	pp.Windowed = TRUE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	pp.BackBufferWidth = (UINT)width;
	pp.BackBufferHeight = (UINT)height;
	pp.hDeviceWindow = hwnd;
	pp.EnableAutoDepthStencil = TRUE;
	pp.AutoDepthStencilFormat = D3DFMT_D24S8;
	pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

	if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
					   D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev))) {
		fprintf(stderr, "CreateDevice failed\n");
		return 1;
	}

	IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);

	tex = make_sprite_tex(dev);
	if (!tex) {
		fprintf(stderr, "CreateTexture failed\n");
		return 1;
	}
	IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);

	printf("D3D_SDK_VERSION=%u (9.0c is 32)  rast threads=%d\n", (unsigned)D3D_SDK_VERSION,
	       threads_fn ? threads_fn() : 0);

	if (dump_only) {
		Vert *locked;
		WORD *idx;
		Vert tri[3];
		IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
				       D3DCOLOR_XRGB(255, 0, 128), 1.0f, 0);
		tri[0] = (Vert){80, 80, 0, 1, D3DCOLOR_XRGB(255, 255, 255), 0, 0};
		tri[1] = (Vert){560, 120, 0, 1, D3DCOLOR_XRGB(255, 255, 255), 1, 0};
		tri[2] = (Vert){240, 400, 0, 1, D3DCOLOR_XRGB(255, 255, 255), 0.5f, 1};
		if (FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(tri), 0,
							       D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1,
							       D3DPOOL_MANAGED, &vb, NULL)) ||
		    FAILED(IDirect3DDevice9_CreateIndexBuffer(dev, 3 * sizeof(WORD), 0, D3DFMT_INDEX16,
							      D3DPOOL_MANAGED, &ib, NULL))) {
			fprintf(stderr, "CreateVertex/IndexBuffer failed\n");
			return 1;
		}
		IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&locked, 0);
		memcpy(locked, tri, sizeof(tri));
		IDirect3DVertexBuffer9_Unlock(vb);
		IDirect3DIndexBuffer9_Lock(ib, 0, 0, (void **)&idx, 0);
		idx[0] = 0;
		idx[1] = 1;
		idx[2] = 2;
		IDirect3DIndexBuffer9_Unlock(ib);
		IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(Vert));
		IDirect3DDevice9_SetIndices(dev, ib);
		IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
		IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
		if (!dump || !dump(dev, dump_path)) {
			fprintf(stderr, "dump failed\n");
			return 1;
		}
		printf("wrote %s (DrawIndexedPrimitive VB+IB)\n", dump_path);
		IDirect3DIndexBuffer9_Release(ib);
		IDirect3DVertexBuffer9_Release(vb);
		IDirect3DTexture9_Release(tex);
		tex = NULL;
		IDirect3DDevice9_Release(dev);
		IDirect3D9_Release(d3d);
		DestroyWindow(hwnd);
		return 0;
	}

	if (bench) {
		int frame = 0;
		double t0, t_report;
		unsigned long long pix_est;
		WORD *idx;
		int q;
		int nverts;
		UINT ib_bytes;
		if (quads < 1)
			quads = 1;
		nverts = quads * 4;
		if (nverts > 65536) {
			fprintf(stderr, "too many quads for INDEX16 (%d verts)\n", nverts);
			return 1;
		}
		ib_bytes = (UINT)quads * 6u * (UINT)sizeof(WORD);
		if (FAILED(IDirect3DDevice9_CreateVertexBuffer(
			    dev, (UINT)nverts * (UINT)sizeof(Vert), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
			    D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1, D3DPOOL_DEFAULT, &vb, NULL)) ||
		    FAILED(IDirect3DDevice9_CreateIndexBuffer(dev, ib_bytes, D3DUSAGE_WRITEONLY,
							      D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib,
							      NULL))) {
			fprintf(stderr, "CreateVertex/IndexBuffer failed\n");
			return 1;
		}
		IDirect3DIndexBuffer9_Lock(ib, 0, 0, (void **)&idx, 0);
		for (q = 0; q < quads; q++) {
			WORD base = (WORD)(q * 4);
			idx[q * 6 + 0] = (WORD)(base + 0);
			idx[q * 6 + 1] = (WORD)(base + 1);
			idx[q * 6 + 2] = (WORD)(base + 2);
			idx[q * 6 + 3] = (WORD)(base + 1);
			idx[q * 6 + 4] = (WORD)(base + 3);
			idx[q * 6 + 5] = (WORD)(base + 2);
		}
		IDirect3DIndexBuffer9_Unlock(ib);
		IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(Vert));
		IDirect3DDevice9_SetIndices(dev, ib);
		pix_est = (unsigned long long)quads * (unsigned long long)sprite *
			  (unsigned long long)sprite;
		printf("bench %dx%d  %d quads (%d tris, %d verts, INDEX16)  16x16 tex bilinear+alpha  ~%dx%d px  no vsync\n",
		       width, height, quads, quads * 2, nverts, sprite, sprite);
		t0 = t_report = qpc_seconds();
		for (;;) {
			MSG msg;
			DWORD seed;
			double now;
			char title[160];
			Vert *locked;
			while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
				if (msg.message == WM_QUIT)
					goto done;
				TranslateMessage(&msg);
				DispatchMessageA(&msg);
			}
			seed = (DWORD)frame * 1664525u + 1013904223u;
			if (FAILED(IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&locked,
							       D3DLOCK_DISCARD))) {
				fprintf(stderr, "VB Lock failed\n");
				return 1;
			}
			for (q = 0; q < quads; q++) {
				float x, y;
				DWORD col;
				seed = seed * 1664525u + 1013904223u;
				x = (float)(seed % (DWORD)(width - sprite - 1));
				seed = seed * 1664525u + 1013904223u;
				y = (float)(seed % (DWORD)(height - sprite - 1));
				col = D3DCOLOR_XRGB((seed >> 16) & 255, (seed >> 8) & 255, seed & 255);
				emit_quad4(locked + q * 4, x, y, (float)sprite, col);
			}
			IDirect3DVertexBuffer9_Unlock(vb);
			IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(8, 8, 16),
					       1.0f, 0);
			IDirect3DDevice9_BeginScene(dev);
			IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0,
							      (UINT)nverts, 0, (UINT)(quads * 2));
			IDirect3DDevice9_EndScene(dev);
			IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
			frame++;
			now = qpc_seconds();
			if (now - t_report >= 1.0 || (frames_limit && frame >= frames_limit)) {
				double elapsed = now - t0;
				double fps = (double)frame / elapsed;
				double ms = 1000.0 / fps;
				double mpix = (double)pix_est * fps / 1.0e6;
				printf("%d frames  %.1f fps  %.2f ms  ~%.1f Mpix/s shaded (bbox)\n",
				       frame, fps, ms, mpix);
				snprintf(title, sizeof(title), "d3d9_sw  %.0f fps  %d quads IB  %dx%d",
					 fps, quads, width, height);
				SetWindowTextA(hwnd, title);
				t_report = now;
			}
			if (frames_limit && frame >= frames_limit)
				goto done;
		}
	}

	for (;;) {
		MSG msg;
		DWORD t;
		int pulse;
		Vert tri[3];
		while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT)
				goto done;
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
		t = GetTickCount();
		pulse = (int)((t / 8) & 255);
		IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET,
				       D3DCOLOR_XRGB(pulse, 32, 255 - pulse), 1.0f, 0);
		tri[0] = (Vert){80, 60, 0, 1, D3DCOLOR_XRGB(255, 255, 255), 0, 0};
		tri[1] = (Vert){560, 90, 0, 1, D3DCOLOR_XRGB(255, 255, 255), 1, 0};
		tri[2] = (Vert){320, 420, 0, 1, D3DCOLOR_XRGB(255, 255, 255), 0.5f, 1};
		IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(Vert));
		IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
		Sleep(16);
	}
done:
	free(verts);
	if (ib)
		IDirect3DIndexBuffer9_Release(ib);
	if (vb)
		IDirect3DVertexBuffer9_Release(vb);
	if (tex)
		IDirect3DTexture9_Release(tex);
	IDirect3DDevice9_Release(dev);
	IDirect3D9_Release(d3d);
	return 0;
}
