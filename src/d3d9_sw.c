#define CINTERFACE
#define COBJMACROS
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocwatch.h"
#include "savestate.h"
#include "swalloc.h"
#include "swrast.h"
#include "vsinterp.h"
#include "trace.h"

#define D3D9_SW_FB_W 640
#define D3D9_SW_FB_H 480

/* When armed by an F9 capture, every draw of the following frame is logged
 * here with its screen bounds, so an artefact at known coordinates can be
 * matched back to the draw that produced it. */
static FILE *g_draw_log;
static int g_draw_log_seq;

/* Diagnostic snapshot of the last textured draw that targets the backbuffer,
 * i.e. the composite quad, written out alongside an F9 capture. */
static struct BbDraw {
	int valid, tri_count, tex_w, tex_h, rt_w, rt_h;
	int bilinear, addr_u, addr_v, samp_mag, samp_min;
	float x[3], y[3], u[3], v[3];
} g_bb_draw;

/* D3D9SW_CLIENT=WxH pins the window's client area, and therefore the
 * backbuffer, to an exact size. The game renders into a 1280x720 offscreen
 * target and composites it with a single fullscreen quad, so when the client
 * area is a few pixels short that quad resamples the whole image on its way to
 * the screen. Every pixel of the frame passes through that one filter, which
 * is why the artefacts it produces read as hairlines through detailed art but
 * leave flat areas untouched. Matching the client area to the offscreen size
 * makes the composite exactly 1:1 and removes the resample rather than
 * improving it. */
static int forced_client_size(int *w, int *h)
{
	static int cw = -1, ch;
	if (cw < 0) {
		const char *s = getenv("D3D9SW_CLIENT");
		cw = ch = 0;
		if (s && sscanf(s, "%dx%d", &cw, &ch) != 2)
			cw = ch = 0;
	}
	if (cw <= 0 || ch <= 0)
		return 0;
	*w = cw;
	*h = ch;
	return 1;
}

/* Grow the window so its client area measures exactly w by h. */
/* How long the process has lasted since the first rewind, in the title bar.
 *
 * Not drawn into the frame on purpose: the rasteriser's output is the thing
 * under test, and a counter composited into it would be one more thing to
 * doubt when a screenshot looks wrong.
 *
 * Sent with a timeout rather than SetWindowText because Present is not
 * guaranteed to run on the thread that owns the window, and a synchronous
 * message to a busy window thread would stall the frame for as long as that
 * thread felt like taking.
 *
 * The remembered title is trimmed at the separator before use. This DLL's
 * statics are rewound like everything else, so after a restore the cached copy
 * is gone while the window still carries whatever we last wrote, and reading it
 * back raw would append a second suffix to the first. */
static void rewind_clock_title(HWND hwnd)
{
	static char base[128];
	static DWORD last;
	static int have_base;
	char text[192];
	char *cut;
	double ms = savestate_live_ms();
	DWORD now = GetTickCount();

	if (ms < 0.0 || !hwnd || !IsWindow(hwnd))
		return;
	if (have_base && now - last < 250)
		return;
	last = now;
	if (!have_base) {
		if (!GetWindowTextA(hwnd, base, sizeof(base)))
			base[0] = 0;
		cut = strstr(base, "  -  live ");
		if (cut)
			*cut = 0;
		have_base = 1;
	}
	_snprintf(text, sizeof(text), "%s  -  live %.0f:%04.1f since first rewind", base,
		  floor(ms / 60000.0), fmod(ms / 1000.0, 60.0));
	text[sizeof(text) - 1] = 0;
	SendMessageTimeoutA(hwnd, WM_SETTEXT, 0, (LPARAM)text, SMTO_ABORTIFHUNG, 50, NULL);
}

static void resize_client_area(HWND hwnd, int w, int h)
{
	RECT rc;
	LONG style, exstyle;
	if (!hwnd || !IsWindow(hwnd))
		return;
	if (GetClientRect(hwnd, &rc) && rc.right - rc.left == w && rc.bottom - rc.top == h)
		return;
	style = GetWindowLong(hwnd, GWL_STYLE);
	exstyle = GetWindowLong(hwnd, GWL_EXSTYLE);
	rc.left = 0;
	rc.top = 0;
	rc.right = w;
	rc.bottom = h;
	AdjustWindowRectEx(&rc, (DWORD)style, GetMenu(hwnd) != NULL, (DWORD)exstyle);
	SetWindowPos(hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
		     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* D3D9 treats a zero backbuffer size in windowed mode as "match the window".
 * Falling back to 640x480 instead silently downsamples everything. */
static void backbuffer_size(HWND hwnd, int *w, int *h)
{
	RECT rc;
	int fw, fh;
	if (forced_client_size(&fw, &fh)) {
		resize_client_area(hwnd, fw, fh);
		*w = fw;
		*h = fh;
		return;
	}
	if (*w > 0 && *h > 0)
		return;
	if (hwnd && GetClientRect(hwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
		*w = (int)(rc.right - rc.left);
		*h = (int)(rc.bottom - rc.top);
		return;
	}
	*w = D3D9_SW_FB_W;
	*h = D3D9_SW_FB_H;
}

typedef struct SwD3D9 {
	IDirect3D9 iface;
	LONG ref;
} SwD3D9;

typedef struct SwDevice {
	IDirect3DDevice9 iface;
	LONG ref;
	SwD3D9 *parent;
	HWND focus;
	HWND device_window;
	D3DPRESENT_PARAMETERS pp;
	D3DVIEWPORT9 viewport;
	D3DMATRIX world;
	D3DMATRIX view;
	D3DMATRIX proj;
	DWORD rs[256];
	DWORD fvf;
	DWORD samp_min;
	DWORD samp_mag;
	DWORD samp_addru;
	DWORD samp_addrv;
	IDirect3DBaseTexture9 *tex0;
	IDirect3DVertexBuffer9 *vb0;
	UINT vb0_off;
	UINT vb0_stride;
	IDirect3DVertexBuffer9 *vb1;
	UINT vb1_off;
	UINT vb1_stride;
	IDirect3DVertexBuffer9 *vb2;
	UINT vb2_off;
	UINT vb2_stride;
	IDirect3DIndexBuffer9 *ib;
	IDirect3DVertexDeclaration9 *decl;
	IDirect3DVertexShader9 *vs;
	IDirect3DPixelShader9 *ps;
	float vs_c[256][4];
	unsigned vs_c_ver;
	float ps_c[224][4];
	int vs_i[16][4];
	int ps_i[16][4];
	WINBOOL vs_b[16];
	WINBOOL ps_b[16];
	IDirect3DSurface9 *backbuf;
	IDirect3DSurface9 *ds;
	IDirect3DSurface9 *rt0;
	UINT stream_freq[3];
	int recording;
	RECT scissor;
	uint32_t *off_color;
	int off_w;
	int off_h;
	int off_vx;
	int off_vy;
	int off_vw;
	int off_vh;
	int drew_off;
	int drew_bb;
	int off_used_valid;
	int off_used_x0;
	int off_used_y0;
	int off_used_x1;
	int off_used_y1;
	int off_fit_w;
	int off_fit_h;
	UINT frame_dips;
	UINT frame_draw_calls;
	UINT frame_rejects;
	UINT frame_reject_code;
	UINT frame_prims;
	UINT frame_clears;
	UINT frame_vs_fail;
	UINT frame_vs_ok;
	float frame_tri_x;
	float frame_tri_y;
	float frame_tri_u;
	float frame_tri_v;
	struct SwSwap *chain;
	SwRast rast;
} SwDevice;

typedef struct SwSwap {
	IDirect3DSwapChain9 iface;
	LONG ref;
	IDirect3DDevice9 *dev;
} SwSwap;

typedef struct SwPriv {
	GUID guid;
	DWORD size;
	DWORD flags;
	unsigned char *data;
	struct SwPriv *next;
} SwPriv;

typedef struct SwSurface SwSurface;

typedef struct SwTexture {
	IDirect3DTexture9 iface;
	LONG ref;
	IDirect3DDevice9 *dev;
	int w;
	int h;
	UINT nlevels;
	D3DFORMAT fmt;
	DWORD usage;
	D3DPOOL pool;
	uint32_t *pixels;
	unsigned char *native;
	UINT native_pitch;
	UINT native_size;
	int native_is_pixels;
	SwPriv *priv;
	SwSurface *level0;
} SwTexture;

typedef struct SwSurface {
	IDirect3DSurface9 iface;
	LONG ref;
	IDirect3DDevice9 *dev;
	SwTexture *tex;
	SwPriv *priv;
	int w;
	int h;
	D3DFORMAT fmt;
	DWORD usage;
	void *bits;
	int pitch;
	int own_bits;
	int mip;
} SwSurface;

typedef struct SwDecl {
	IDirect3DVertexDeclaration9 iface;
	LONG ref;
	IDirect3DDevice9 *dev;
	D3DVERTEXELEMENT9 *elems;
	UINT count;
} SwDecl;

typedef struct SwShader {
	IDirect3DVertexShader9 vs;
	LONG ref;
	IDirect3DDevice9 *dev;
	int is_ps;
	DWORD *code;
	UINT bytes;
} SwShader;

typedef struct SwVB {
	IDirect3DVertexBuffer9 iface;
	LONG ref;
	IDirect3DDevice9 *dev;
	UINT size;
	DWORD usage;
	DWORD fvf;
	D3DPOOL pool;
	unsigned char *bytes;
} SwVB;

typedef struct SwIB {
	IDirect3DIndexBuffer9 iface;
	LONG ref;
	IDirect3DDevice9 *dev;
	UINT size;
	DWORD usage;
	D3DFORMAT fmt;
	D3DPOOL pool;
	unsigned char *bytes;
} SwIB;

static void priv_free_all(SwPriv *p)
{
	while (p) {
		SwPriv *n = p->next;
		if ((p->flags & D3DSPD_IUNKNOWN) && p->data && p->size >= sizeof(IUnknown *)) {
			IUnknown *u;
			memcpy(&u, p->data, sizeof(u));
			if (u)
				u->lpVtbl->Release(u);
		}
		free(p->data);
		free(p);
		p = n;
	}
}

static HRESULT priv_set(SwPriv **head, REFGUID guid, const void *data, DWORD size, DWORD flags)
{
	SwPriv *p, **pp;
	IUnknown *unk = NULL;

	if (!guid || (!data && size) || (data && !size))
		return D3DERR_INVALIDCALL;
	if (flags & D3DSPD_IUNKNOWN) {
		if (size < sizeof(IUnknown *))
			return D3DERR_INVALIDCALL;
		size = sizeof(IUnknown *);
		memcpy(&unk, data, sizeof(unk));
		if (unk)
			unk->lpVtbl->AddRef(unk);
	}
	for (pp = head; *pp; pp = &(*pp)->next) {
		if (IsEqualGUID(&(*pp)->guid, guid)) {
			p = *pp;
			if ((p->flags & D3DSPD_IUNKNOWN) && p->data && p->size >= sizeof(IUnknown *)) {
				IUnknown *old;
				memcpy(&old, p->data, sizeof(old));
				if (old)
					old->lpVtbl->Release(old);
			}
			free(p->data);
			p->data = (unsigned char *)malloc(size);
			if (!p->data)
				return E_OUTOFMEMORY;
			memcpy(p->data, data, size);
			p->size = size;
			p->flags = flags;
			return D3D_OK;
		}
	}
	p = (SwPriv *)calloc(1, sizeof(*p));
	if (!p) {
		if (unk)
			unk->lpVtbl->Release(unk);
		return E_OUTOFMEMORY;
	}
	p->guid = *guid;
	p->size = size;
	p->flags = flags;
	p->data = (unsigned char *)malloc(size);
	if (!p->data) {
		free(p);
		if (unk)
			unk->lpVtbl->Release(unk);
		return E_OUTOFMEMORY;
	}
	memcpy(p->data, data, size);
	p->next = *head;
	*head = p;
	return D3D_OK;
}

static HRESULT priv_get(SwPriv *p, REFGUID guid, void *data, DWORD *size)
{
	if (!guid || !size)
		return D3DERR_INVALIDCALL;
	for (; p; p = p->next) {
		if (IsEqualGUID(&p->guid, guid)) {
			if (!data) {
				*size = p->size;
				return D3D_OK;
			}
			if (*size < p->size) {
				*size = p->size;
				return D3DERR_MOREDATA;
			}
			memcpy(data, p->data, p->size);
			*size = p->size;
			return D3D_OK;
		}
	}
	return D3DERR_NOTFOUND;
}

static HRESULT priv_free_one(SwPriv **head, REFGUID guid)
{
	SwPriv **pp;
	if (!guid)
		return D3DERR_INVALIDCALL;
	for (pp = head; *pp; pp = &(*pp)->next) {
		if (IsEqualGUID(&(*pp)->guid, guid)) {
			SwPriv *p = *pp;
			*pp = p->next;
			if ((p->flags & D3DSPD_IUNKNOWN) && p->data && p->size >= sizeof(IUnknown *)) {
				IUnknown *u;
				memcpy(&u, p->data, sizeof(u));
				if (u)
					u->lpVtbl->Release(u);
			}
			free(p->data);
			free(p);
			return D3D_OK;
		}
	}
	return D3DERR_NOTFOUND;
}

static void com_replace(IUnknown **slot, IUnknown *obj)
{
	if (obj)
		obj->lpVtbl->AddRef(obj);
	if (*slot)
		(*slot)->lpVtbl->Release(*slot);
	*slot = obj;
}

static void sw_log(const char *msg)
{
	static volatile LONG n;
	char line[256];
	FILE *f;
	if (InterlockedIncrement(&n) > 512)
		return;
	_snprintf(line, sizeof(line), "[d3d9_sw] %s\n", msg);
	OutputDebugStringA(line);
	f = fopen("d3d9_sw.log", "a");
	if (f) {
		fputs(line, f);
		fclose(f);
	}
}

static void sw_log_notimpl(const char *name)
{
	static char seen[64][64];
	static int nseen;
	char msg[160];
	int i;
	for (i = 0; i < nseen; i++) {
		if (strcmp(seen[i], name) == 0)
			return;
	}
	if (nseen < 64) {
		strncpy(seen[nseen], name, 63);
		seen[nseen][63] = 0;
		nseen++;
	}
	_snprintf(msg, sizeof(msg), "E_NOTIMPL %s", name);
	sw_log(msg);
}

#define STUB0(name)                                                                 \
	static HRESULT WINAPI Stub_##name(IDirect3DDevice9 *this)                   \
	{                                                                           \
		(void)this;                                                         \
		sw_log_notimpl(#name);                                              \
		return E_NOTIMPL;                                                   \
	}
#define STUB1(name)                                                                 \
	static HRESULT WINAPI Stub_##name(IDirect3DDevice9 *this, void *a)          \
	{                                                                           \
		(void)this;                                                         \
		(void)a;                                                            \
		sw_log_notimpl(#name);                                              \
		return E_NOTIMPL;                                                   \
	}
#define STUB2(name)                                                                 \
	static HRESULT WINAPI Stub_##name(IDirect3DDevice9 *this, void *a, void *b) \
	{                                                                           \
		(void)this;                                                         \
		(void)a;                                                            \
		(void)b;                                                            \
		sw_log_notimpl(#name);                                              \
		return E_NOTIMPL;                                                   \
	}
#define STUB3(name)                                                                 \
	static HRESULT WINAPI Stub_##name(IDirect3DDevice9 *this, void *a, void *b,  \
					  void *c)                                  \
	{                                                                           \
		(void)this;                                                         \
		(void)a;                                                            \
		(void)b;                                                            \
		(void)c;                                                            \
		sw_log_notimpl(#name);                                              \
		return E_NOTIMPL;                                                   \
	}
#define STUB4(name)                                                                 \
	static HRESULT WINAPI Stub_##name(IDirect3DDevice9 *this, void *a, void *b,  \
					  void *c, void *d)                         \
	{                                                                           \
		(void)this;                                                         \
		(void)a;                                                            \
		(void)b;                                                            \
		(void)c;                                                            \
		(void)d;                                                            \
		sw_log_notimpl(#name);                                              \
		return E_NOTIMPL;                                                   \
	}
#define STUB5(name)                                                                 \
	static HRESULT WINAPI Stub_##name(IDirect3DDevice9 *this, void *a, void *b,  \
					  void *c, void *d, void *e)                \
	{                                                                           \
		(void)this;                                                         \
		(void)a;                                                            \
		(void)b;                                                            \
		(void)c;                                                            \
		(void)d;                                                            \
		(void)e;                                                            \
		sw_log_notimpl(#name);                                              \
		return E_NOTIMPL;                                                   \
	}
#define STUB6(name)                                                                 \
	static HRESULT WINAPI Stub_##name(IDirect3DDevice9 *this, void *a, void *b,  \
					  void *c, void *d, void *e, void *f)       \
	{                                                                           \
		(void)this;                                                         \
		(void)a;                                                            \
		(void)b;                                                            \
		(void)c;                                                            \
		(void)d;                                                            \
		(void)e;                                                            \
		(void)f;                                                            \
		sw_log_notimpl(#name);                                              \
		return E_NOTIMPL;                                                   \
	}
#define STUB7(name)                                                                 \
	static HRESULT WINAPI Stub_##name(IDirect3DDevice9 *this, void *a, void *b,  \
					  void *c, void *d, void *e, void *f,       \
					  void *g)                                  \
	{                                                                           \
		(void)this;                                                         \
		(void)a;                                                            \
		(void)b;                                                            \
		(void)c;                                                            \
		(void)d;                                                            \
		(void)e;                                                            \
		(void)f;                                                            \
		(void)g;                                                            \
		sw_log_notimpl(#name);                                              \
		return E_NOTIMPL;                                                   \
	}
#define STUB8(name)                                                                 \
	static HRESULT WINAPI Stub_##name(IDirect3DDevice9 *this, void *a, void *b,  \
					  void *c, void *d, void *e, void *f,       \
					  void *g, void *h)                         \
	{                                                                           \
		(void)this;                                                         \
		(void)a;                                                            \
		(void)b;                                                            \
		(void)c;                                                            \
		(void)d;                                                            \
		(void)e;                                                            \
		(void)f;                                                            \
		(void)g;                                                            \
		(void)h;                                                            \
		sw_log_notimpl(#name);                                              \
		return E_NOTIMPL;                                                   \
	}
#define STUB9(name)                                                                 \
	static HRESULT WINAPI Stub_##name(IDirect3DDevice9 *this, void *a, void *b,  \
					  void *c, void *d, void *e, void *f,       \
					  void *g, void *h, void *i)                \
	{                                                                           \
		(void)this;                                                         \
		(void)a;                                                            \
		(void)b;                                                            \
		(void)c;                                                            \
		(void)d;                                                            \
		(void)e;                                                            \
		(void)f;                                                            \
		(void)g;                                                            \
		(void)h;                                                            \
		(void)i;                                                            \
		sw_log_notimpl(#name);                                              \
		return E_NOTIMPL;                                                   \
	}

STUB3(SetCursorProperties)
STUB2(CreateAdditionalSwapChain)
STUB9(CreateVolumeTexture)
STUB7(CreateCubeTexture)
STUB4(UpdateSurface)
STUB2(GetRenderTargetData)
STUB2(GetFrontBufferData)
STUB3(ColorFill)
STUB6(CreateOffscreenPlainSurface)
STUB1(SetMaterial)
STUB1(GetMaterial)
STUB2(SetLight)
STUB2(GetLight)
STUB2(LightEnable)
STUB2(GetLightEnable)
STUB2(SetClipPlane)
STUB2(GetClipPlane)
STUB1(SetClipStatus)
STUB1(GetClipStatus)
STUB2(SetPaletteEntries)
STUB2(GetPaletteEntries)
STUB1(SetCurrentTexturePalette)
STUB1(GetCurrentTexturePalette)
STUB1(SetNPatchMode)
STUB6(ProcessVertices)
STUB3(DrawRectPatch)
STUB3(DrawTriPatch)
STUB1(DeletePatch)

static HRESULT WINAPI Dev_NotImpl(IDirect3DDevice9 *this)
{
	(void)this;
	sw_log_notimpl("IDirect3DDevice9(unknown slot)");
	return E_NOTIMPL;
}

static void identity_matrix(D3DMATRIX *m)
{
	memset(m, 0, sizeof(*m));
	m->m[0][0] = m->m[1][1] = m->m[2][2] = m->m[3][3] = 1.0f;
}

static void ensure_surf_vtbl(void);
static void device_rt_rast(SwDevice *d, SwRast *out);
static HRESULT WINAPI Dev_GetBackBuffer(IDirect3DDevice9 *this, UINT swapchain, UINT bb,
					D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **out);
static HRESULT WINAPI Dev_GetRasterStatus(IDirect3DDevice9 *this, UINT swapchain,
					  D3DRASTER_STATUS *st);

static SwDevice *dev_from(IDirect3DDevice9 *this)
{
	return (SwDevice *)this;
}

static SwD3D9 *d3d_from(IDirect3D9 *this)
{
	return (SwD3D9 *)this;
}

static HRESULT WINAPI Dev_QueryInterface(IDirect3DDevice9 *this, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3DDevice9)) {
		*ppv = this;
		this->lpVtbl->AddRef(this);
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI Dev_AddRef(IDirect3DDevice9 *this)
{
	return (ULONG)InterlockedIncrement(&dev_from(this)->ref);
}

static ULONG WINAPI Dev_Release(IDirect3DDevice9 *this)
{
	SwDevice *d = dev_from(this);
	LONG n = InterlockedDecrement(&d->ref);
	if (n == 0) {
		if (d->tex0)
			d->tex0->lpVtbl->Release(d->tex0);
		if (d->vb0)
			d->vb0->lpVtbl->Release(d->vb0);
		if (d->vb1)
			d->vb1->lpVtbl->Release(d->vb1);
		if (d->vb2)
			d->vb2->lpVtbl->Release(d->vb2);
		if (d->ib)
			d->ib->lpVtbl->Release(d->ib);
		if (d->decl)
			d->decl->lpVtbl->Release(d->decl);
		if (d->vs)
			d->vs->lpVtbl->Release(d->vs);
		if (d->ps)
			d->ps->lpVtbl->Release(d->ps);
		if (d->backbuf)
			d->backbuf->lpVtbl->Release(d->backbuf);
		if (d->ds)
			d->ds->lpVtbl->Release(d->ds);
		if (d->rt0)
			d->rt0->lpVtbl->Release(d->rt0);
		if (d->chain) {
			SwSwap *c = d->chain;
			d->chain = NULL;
			c->dev = NULL;
			if (InterlockedDecrement(&c->ref) == 0)
				free(c);
		}
		swrast_free(&d->rast);
		if (d->parent)
			d->parent->iface.lpVtbl->Release(&d->parent->iface);
		free(d);
	}
	return (ULONG)n;
}

static HRESULT WINAPI Dev_TestCooperativeLevel(IDirect3DDevice9 *this)
{
	(void)this;
	return D3D_OK;
}

static UINT WINAPI Dev_GetAvailableTextureMem(IDirect3DDevice9 *this)
{
	(void)this;
	return 256u * 1024u * 1024u;
}

static HRESULT WINAPI Dev_EvictManagedResources(IDirect3DDevice9 *this)
{
	(void)this;
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetDirect3D(IDirect3DDevice9 *this, IDirect3D9 **out)
{
	SwDevice *d = dev_from(this);
	if (!out)
		return E_POINTER;
	*out = &d->parent->iface;
	(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetDeviceCaps(IDirect3DDevice9 *this, D3DCAPS9 *caps)
{
	(void)this;
	if (!caps)
		return E_POINTER;
	memset(caps, 0, sizeof(*caps));
	caps->DeviceType = D3DDEVTYPE_HAL;
	caps->AdapterOrdinal = 0;
	caps->MaxTextureWidth = 4096;
	caps->MaxTextureHeight = 4096;
	caps->MaxSimultaneousTextures = 8;
	caps->PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE;
	caps->DevCaps = D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_HWTRANSFORMANDLIGHT;
	caps->PrimitiveMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
	caps->RasterCaps = D3DPRASTERCAPS_ZTEST;
	caps->ZCmpCaps = 0xff;
	caps->SrcBlendCaps = 0xff;
	caps->DestBlendCaps = 0xff;
	caps->TextureCaps = D3DPTEXTURECAPS_ALPHA;
	caps->MaxVertexIndex = 0x00ffffff;
	caps->MaxPrimitiveCount = 0xfffff;
	caps->MaxStreams = 1;
	caps->MaxStreamStride = 256;
	caps->VertexShaderVersion = D3DVS_VERSION(2, 0);
	caps->PixelShaderVersion = D3DPS_VERSION(2, 0);
	caps->MaxVertexShaderConst = 256;
	caps->PixelShader1xMaxValue = 8.0f;
	caps->NumSimultaneousRTs = 1;
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetDisplayMode(IDirect3DDevice9 *this, UINT swapchain, D3DDISPLAYMODE *mode)
{
	SwDevice *d = dev_from(this);
	if (swapchain != 0 || !mode)
		return D3DERR_INVALIDCALL;
	mode->Width = (UINT)d->rast.width;
	mode->Height = (UINT)d->rast.height;
	mode->RefreshRate = 60;
	mode->Format = D3DFMT_X8R8G8B8;
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetCreationParameters(IDirect3DDevice9 *this,
						D3DDEVICE_CREATION_PARAMETERS *p)
{
	SwDevice *d = dev_from(this);
	if (!p)
		return E_POINTER;
	memset(p, 0, sizeof(*p));
	p->AdapterOrdinal = 0;
	p->DeviceType = D3DDEVTYPE_HAL;
	p->hFocusWindow = d->focus;
	p->BehaviorFlags = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
	return D3D_OK;
}

static void WINAPI Dev_SetCursorPosition(IDirect3DDevice9 *this, int x, int y, DWORD flags)
{
	(void)this;
	(void)x;
	(void)y;
	(void)flags;
}

static WINBOOL WINAPI Dev_ShowCursor(IDirect3DDevice9 *this, WINBOOL show)
{
	(void)this;
	return show;
}

static UINT WINAPI Dev_GetNumberOfSwapChains(IDirect3DDevice9 *this)
{
	(void)this;
	return 1;
}

static HRESULT WINAPI Dev_Reset(IDirect3DDevice9 *this, D3DPRESENT_PARAMETERS *pp)
{
	SwDevice *d = dev_from(this);
	int w, h;
	if (!pp)
		return D3DERR_INVALIDCALL;
	d->pp = *pp;
	w = (int)pp->BackBufferWidth;
	h = (int)pp->BackBufferHeight;
	backbuffer_size(pp->hDeviceWindow ? pp->hDeviceWindow : d->device_window, &w, &h);
	d->pp.BackBufferWidth = (UINT)w;
	d->pp.BackBufferHeight = (UINT)h;
	swrast_resize(&d->rast, w, h);
	d->viewport.X = 0;
	d->viewport.Y = 0;
	d->viewport.Width = (DWORD)w;
	d->viewport.Height = (DWORD)h;
	d->viewport.MinZ = 0.0f;
	d->viewport.MaxZ = 1.0f;
	return D3D_OK;
}

static uint32_t hash_rect(const uint32_t *p, int w, int h, int uw, int uh)
{
	uint32_t hsh = 2166136261u;
	int y, x;
	if (!p || w <= 0 || h <= 0)
		return 0;
	if (uw > w)
		uw = w;
	if (uh > h)
		uh = h;
	if (uw < 1)
		uw = w;
	if (uh < 1)
		uh = h;
	for (y = 0; y < uh; y += 7) {
		for (x = 0; x < uw; x += 7)
			hsh = (hsh ^ p[y * w + x]) * 16777619u;
	}
	return hsh;
}

static void blit_rect(uint32_t *dp, int dw, int dh, const uint32_t *sp, int sw, int sh, int sx0,
		      int sy0, int src_w, int src_h)
{
	int y, x;
	if (!dp || !sp || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || src_w <= 0 || src_h <= 0)
		return;
	if (sx0 < 0)
		sx0 = 0;
	if (sy0 < 0)
		sy0 = 0;
	if (sx0 + src_w > sw)
		src_w = sw - sx0;
	if (sy0 + src_h > sh)
		src_h = sh - sy0;
	if (src_w <= 0 || src_h <= 0)
		return;
	{
		/* 16.16 stepping: the per-pixel integer divides here were costing a
		 * division for every pixel presented. */
		unsigned xstep = (unsigned)(((INT64)src_w << 16) / dw);
		unsigned ystep = (unsigned)(((INT64)src_h << 16) / dh);
		unsigned yfix = 0;
		for (y = 0; y < dh; y++, yfix += ystep) {
			int sy = sy0 + (int)(yfix >> 16);
			const uint32_t *srow;
			uint32_t *drow = dp + (size_t)y * dw;
			unsigned xfix = 0;
			if (sy >= sh)
				sy = sh - 1;
			srow = sp + (size_t)sy * sw;
			for (x = 0; x < dw; x++, xfix += xstep) {
				int sx = sx0 + (int)(xfix >> 16);
				if (sx >= sw)
					sx = sw - 1;
				drow[x] = srow[sx];
			}
		}
	}
}

static void off_expand_tris(SwDevice *d, const SwTri *batch, UINT n)
{
	UINT i;
	int k;
	if (!batch || n == 0)
		return;
	if (!d->off_used_valid) {
		d->off_used_x0 = 100000;
		d->off_used_y0 = 100000;
		d->off_used_x1 = -100000;
		d->off_used_y1 = -100000;
		d->off_used_valid = 1;
	}
	for (i = 0; i < n; i++) {
		const SwVert *vs[3];
		vs[0] = &batch[i].a;
		vs[1] = &batch[i].b;
		vs[2] = &batch[i].c;
		for (k = 0; k < 3; k++) {
			int x0 = (int)floorf(vs[k]->x);
			int y0 = (int)floorf(vs[k]->y);
			int x1 = (int)ceilf(vs[k]->x);
			int y1 = (int)ceilf(vs[k]->y);
			if (x0 < d->off_used_x0)
				d->off_used_x0 = x0;
			if (y0 < d->off_used_y0)
				d->off_used_y0 = y0;
			if (x1 > d->off_used_x1)
				d->off_used_x1 = x1;
			if (y1 > d->off_used_y1)
				d->off_used_y1 = y1;
		}
	}
}

/* The game renders its scene at the origin of a larger render target, so the
 * composite always starts at (0,0). Deriving the source rect from the bounding
 * box of whatever was drawn this frame makes it track the sprites and the whole
 * image slides around, so only the extent is measured, and it is latched to the
 * high-water mark to keep it steady once the first full frame has been seen. */
static void blit_off_to_bb(SwDevice *d)
{
	int vw, vh;
	if (!d->off_color || !d->rast.color || d->off_color == d->rast.color)
		return;
	if (d->off_used_valid && d->off_used_x1 > d->off_fit_w)
		d->off_fit_w = d->off_used_x1;
	if (d->off_used_valid && d->off_used_y1 > d->off_fit_h)
		d->off_fit_h = d->off_used_y1;
	vw = d->off_fit_w;
	vh = d->off_fit_h;
	if (vw > d->off_w)
		vw = d->off_w;
	if (vh > d->off_h)
		vh = d->off_h;
	if (vw < 2 || vh < 2) {
		vw = d->off_w;
		vh = d->off_h;
	}
	blit_rect(d->rast.color, d->rast.width, d->rast.height, d->off_color, d->off_w, d->off_h, 0,
		  0, vw, vh);
}

static double g_present_ms;

/* Wall-clock frame time cannot distinguish work from waiting: with vsync on it
 * is pinned to the refresh interval whatever we do. Process CPU time across all
 * threads is what actually answers "is this costing anything". */
static double process_cpu_ms(void)
{
	FILETIME cr, ex, kt, ut;
	ULARGE_INTEGER k, u;
	if (!GetProcessTimes(GetCurrentProcess(), &cr, &ex, &kt, &ut))
		return 0.0;
	k.LowPart = kt.dwLowDateTime;
	k.HighPart = kt.dwHighDateTime;
	u.LowPart = ut.dwLowDateTime;
	u.HighPart = ut.dwHighDateTime;
	return (double)(k.QuadPart + u.QuadPart) / 10000.0; /* 100ns units to ms */
}

#ifndef D3D9SW_VARIANT
#define D3D9SW_VARIANT stock
#endif
#define SW_VARIANT_STR2(x) #x
#define SW_VARIANT_STR(x) SW_VARIANT_STR2(x)

/* Set D3D9SW_PROF=1 to log a per-frame CSV next to the executable. The whole
 * point is the distribution: a steady frame time means a fixed stall, a spread
 * one means we are genuinely short on throughput.
 *
 * cpu_ms is the column to read when the question is heat rather than speed. On
 * a laptop that throttles, frame time is held roughly constant by the vsync and
 * the clock drops instead, so a change that halves the work shows up there and
 * almost nowhere else. */
static void prof_frame(void)
{
	static double prev_cpu;
	double cpu_ms;
	static int enabled = -1;
	static FILE *f;
	static LARGE_INTEGER prev, fq;
	static unsigned frame;
	LARGE_INTEGER now;
	double raster_ms, total_ms;
	unsigned flushes, tris, bins;
	(void)frame;

	if (enabled < 0) {
		const char *e = getenv("D3D9SW_PROF");
		enabled = (e && *e && *e != '0') ? 1 : 0;
		if (enabled) {
			f = fopen("d3d9_sw_frames.csv", "w");
			if (f) {
				int cf = swrast_cpu_features();
				fprintf(f,
					"# build: %s   cpu: sse2=%d sse41=%d avx2=%d "
					"avx512=%d   threads: %d\n",
					SW_VARIANT_STR(D3D9SW_VARIANT), !!(cf & 1),
					!!(cf & 2), !!(cf & 4), !!(cf & 8),
					swrast_thread_count());
				fprintf(f, "frame,total_ms,raster_ms,other_ms,cpu_ms,"
					   "present_ms,flushes,tris,"
					   "binned,area_px,bbox_px,tw,th,"
					   "m_tex,m_bilin,m_over,m_add,m_blendoth,m_atest,"
					   "m_ztest,m_flat,m_linv\n");
			}
			QueryPerformanceFrequency(&fq);
			QueryPerformanceCounter(&prev);
			prev_cpu = process_cpu_ms();
		}
	}
	if (!enabled || !f)
		return;
	QueryPerformanceCounter(&now);
	total_ms = (double)(now.QuadPart - prev.QuadPart) * 1000.0 / (double)fq.QuadPart;
	prev = now;
	{
		double c = process_cpu_ms();
		cpu_ms = c - prev_cpu;
		prev_cpu = c;
	}
	swrast_prof_take(&raster_ms, &flushes, &tris, &bins);
	{
		double area, bbox;
		int tw, th;
		(void)0;
		double mix[9];
		int i;
		swrast_prof_take2(&area, &bbox, &tw, &th);
		swrast_prof_mix(mix, 9);
		frame++;
		fprintf(f, "%u,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%.0f,%.0f,%d,%d", frame,
			total_ms, raster_ms, total_ms - raster_ms, cpu_ms, g_present_ms,
			flushes, tris, bins, area, bbox, tw, th);
		for (i = 0; i < 9; i++)
			fprintf(f, ",%.0f", mix[i]);
		fprintf(f, "\n");
	}
	if ((frame & 63) == 0)
		fflush(f);
}

static HRESULT WINAPI Dev_Present(IDirect3DDevice9 *this, const RECT *src, const RECT *dst,
				  HWND hwnd_override, const RGNDATA *dirty)
{
	SwDevice *d = dev_from(this);
	static volatile LONG presents;
	SwRast rt;
	(void)src;
	(void)dst;
	(void)dirty;
	swrast_flush();
	prof_frame();
	allocwatch_frame();
	savestate_guard();
	/* Rewind hotkeys. Taken after the flush, so no draw is in flight and the
	 * snapshot sees a quiescent renderer: F5..F8 save, shift+F5..F8 restore. */
	{
		int k;
		for (k = 0; k < SAVESTATE_SLOTS; k++) {
			if (!(GetAsyncKeyState(VK_F5 + k) & 1))
				continue;
			if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
				if (savestate_load(k))
					sw_trace("savestate: restored slot %d in %.1f ms\n", k,
						 savestate_last_ms());
			} else if (savestate_save(k)) {
				/* See the same branch in d3d11_sw.c: a restored thread
				 * returns through the save it was taking. */
				if (savestate_last_was_restore())
					sw_trace("savestate: restored slot %d in %.1f ms, "
						 "resumed through the save\n",
						 k, savestate_last_ms());
				else
					sw_trace("savestate: saved slot %d, %.1f MB in "
						 "%.1f ms\n",
						 k, savestate_last_mb(), savestate_last_ms());
			}
		}
	}
	rewind_clock_title(d->device_window);
	/* F9 grabs the backbuffer losslessly; screenshots of seam artefacts are
	 * useless once a JPEG encoder has been near them. */
	/* F9 arms draw-id recording, and the frame after it is the one captured,
	 * so the colour image and the per-pixel owner map describe the same
	 * frame. Recording is off at every other moment. */
	{
		static int pending;
		if (GetAsyncKeyState(VK_F9) & 1) {
			swrast_drawid_arm();
			if (!g_draw_log) {
				g_draw_log = fopen("d3d9_sw_draws.txt", "w");
				g_draw_log_seq = 0;
			}
			pending = 1;
		} else if (pending) {
			static int snap;
			char name[64];
			_snprintf(name, sizeof(name), "d3d9_sw_snap%d.tga", ++snap);
			swrast_dump_tga(&d->rast, name);
			_snprintf(name, sizeof(name), "d3d9_sw_snap%d", snap);
			swrast_dump_drawid(name);
			/* The source the composite quad samples. Comparing it against
			 * the backbuffer separates seams the game rendered from seams
			 * our sampling of it introduces. */
			if (d->off_color) {
				SwRast src;
				memset(&src, 0, sizeof(src));
				src.color = d->off_color;
				src.width = d->off_w;
				src.height = d->off_h;
				_snprintf(name, sizeof(name), "d3d9_sw_snap%d_src.tga", snap);
				swrast_dump_tga(&src, name);
			}
			if (g_bb_draw.valid) {
				FILE *bf;
				_snprintf(name, sizeof(name), "d3d9_sw_snap%d_bb.txt", snap);
				bf = fopen(name, "w");
				if (bf) {
					int k;
					fprintf(bf, "tris=%d tex=%dx%d rt=%dx%d\n", g_bb_draw.tri_count,
						g_bb_draw.tex_w, g_bb_draw.tex_h, g_bb_draw.rt_w,
						g_bb_draw.rt_h);
					fprintf(bf, "bilinear=%d samp_mag=%d samp_min=%d addr=%d,%d\n",
						g_bb_draw.bilinear, g_bb_draw.samp_mag,
						g_bb_draw.samp_min, g_bb_draw.addr_u, g_bb_draw.addr_v);
					for (k = 0; k < 3; k++)
						fprintf(bf, "v%d xy=(%.4f,%.4f) uv=(%.6f,%.6f) texel=(%.3f,%.3f)\n",
							k, g_bb_draw.x[k], g_bb_draw.y[k], g_bb_draw.u[k],
							g_bb_draw.v[k], g_bb_draw.u[k] * g_bb_draw.tex_w,
							g_bb_draw.v[k] * g_bb_draw.tex_h);
					fclose(bf);
				}
			}
			swrast_drawid_disarm();
			if (g_draw_log) {
				fclose(g_draw_log);
				g_draw_log = NULL;
			}
			pending = 0;
		}
	}
	device_rt_rast(d, &rt);
	/* The game composites its offscreen target itself by drawing a textured
	 * fullscreen quad to the backbuffer. Blitting the raw target on top of
	 * that overwrites a correct image with a guessed source rect, so only fall
	 * back to the blit when nothing reached the backbuffer at all. */
	if (d->drew_off && !d->drew_bb)
		blit_off_to_bb(d);
	else if (!d->drew_off && rt.color && rt.color != d->rast.color && d->rast.color)
		blit_rect(d->rast.color, d->rast.width, d->rast.height, rt.color, rt.width, rt.height,
			  0, 0, rt.width, rt.height);
	{
		int ux0 = d->off_used_x0, uy0 = d->off_used_y0;
		int uw = d->off_used_x1 - d->off_used_x0;
		int uh = d->off_used_y1 - d->off_used_y0;
		LONG p;
		FILE *cf;
		d->drew_off = 0;
		d->drew_bb = 0;
		d->off_used_valid = 0;
		/* Draw indices are only meaningful within a frame. */
		swrast_drawid_newframe();
		p = InterlockedIncrement(&presents);
		{
			/* Hashing the frame and rewriting the status file cost a full
			 * scan plus three file syscalls on every Present. Keep them for
			 * the early frames where they are diagnostic, then stop. */
			int want_stats = p <= 240 || (p % 300) == 0;
			uint32_t hsh = 0;
			if (!want_stats)
				goto stats_done;
			if (d->off_color)
				hsh = hash_rect(d->off_color, d->off_w, d->off_h, 1280, 720);
			else if (d->rast.color)
				hsh = hash_rect(d->rast.color, d->rast.width, d->rast.height,
						d->rast.width, d->rast.height);
			cf = fopen("d3d9_sw_present.txt", "w");
			if (cf) {
				fprintf(cf,
					"%ld calls=%u dips=%u rej=%u/%u prims=%u clr=%u vsok=%u vsfail=%u hash=%08x tri=%.2f,%.2f uv=%.3f,%.3f c0=%.4f,%.4f,%.4f,%.4f\n",
					p, d->frame_draw_calls, d->frame_dips, d->frame_rejects,
					d->frame_reject_code, d->frame_prims, d->frame_clears,
					d->frame_vs_ok, d->frame_vs_fail, hsh, d->frame_tri_x,
					d->frame_tri_y, d->frame_tri_u, d->frame_tri_v, d->vs_c[0][0],
					d->vs_c[0][1], d->vs_c[0][2], d->vs_c[0][3]);
				fclose(cf);
			}
			if (p == 1 || p == 30 || p == 120) {
				char msg[320];
				_snprintf(msg, sizeof(msg),
					  "Present n=%ld rt=%dx%d bb=%dx%d off=%dx%d used=%d,%d %dx%d calls=%u dips=%u rej=%u/%u prims=%u clr=%u vsok=%u vsfail=%u hash=%08x tri=%.1f,%.1f uv=%.3f,%.3f c0=%.3f,%.3f,%.3f,%.3f",
					  p, rt.width, rt.height, d->rast.width, d->rast.height, d->off_w,
					  d->off_h, ux0, uy0, uw, uh, d->frame_draw_calls, d->frame_dips,
					  d->frame_rejects, d->frame_reject_code, d->frame_prims,
					  d->frame_clears, d->frame_vs_ok, d->frame_vs_fail, hsh,
					  d->frame_tri_x, d->frame_tri_y, d->frame_tri_u, d->frame_tri_v,
					  d->vs_c[0][0], d->vs_c[0][1], d->vs_c[0][2], d->vs_c[0][3]);
				sw_log(msg);
			}
		stats_done:;
		}
		d->frame_dips = 0;
		d->frame_prims = 0;
		d->frame_clears = 0;
		d->frame_vs_fail = 0;
		d->frame_vs_ok = 0;
		d->frame_draw_calls = 0;
		d->frame_rejects = 0;
		d->frame_reject_code = 0;
		if (p == 1 || p == 30) {
			SwRast dump;
			if (d->off_color) {
				memset(&dump, 0, sizeof(dump));
				dump.color = d->off_color;
				dump.width = d->off_w;
				dump.height = d->off_h;
				swrast_dump_tga(&dump, p == 1 ? "d3d9_sw_rt.tga" : "d3d9_sw_rt30.tga");
			}
			swrast_dump_tga(&d->rast, p == 1 ? "d3d9_sw_bb.tga" : "d3d9_sw_bb30.tga");
		}
	}
	{
		LARGE_INTEGER pa, pb, pf;
		QueryPerformanceCounter(&pa);
		swrast_present(&d->rast, hwnd_override);
		QueryPerformanceCounter(&pb);
		QueryPerformanceFrequency(&pf);
		if (pf.QuadPart > 0)
			g_present_ms = (double)(pb.QuadPart - pa.QuadPart) * 1000.0 /
				       (double)pf.QuadPart;
	}
	return D3D_OK;
}

static IDirect3DSwapChain9Vtbl kSwapVtbl;

static SwSwap *swap_from(IDirect3DSwapChain9 *this)
{
	return (SwSwap *)this;
}

static HRESULT WINAPI Chain_QueryInterface(IDirect3DSwapChain9 *this, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3DSwapChain9)) {
		*ppv = this;
		this->lpVtbl->AddRef(this);
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI Chain_AddRef(IDirect3DSwapChain9 *this)
{
	return (ULONG)InterlockedIncrement(&swap_from(this)->ref);
}

static ULONG WINAPI Chain_Release(IDirect3DSwapChain9 *this)
{
	SwSwap *c = swap_from(this);
	LONG n = InterlockedDecrement(&c->ref);
	if (n == 0) {
		if (c->dev) {
			SwDevice *d = (SwDevice *)c->dev;
			if (d->chain == c)
				d->chain = NULL;
		}
		free(c);
	}
	return (ULONG)n;
}

static HRESULT WINAPI Chain_Present(IDirect3DSwapChain9 *this, const RECT *src, const RECT *dst,
				    HWND hwnd, const RGNDATA *dirty, DWORD flags)
{
	SwSwap *c = swap_from(this);
	static LONG once;
	(void)flags;
	sw_trace("SwapChain.Present flags=%08x", flags);
	if (InterlockedIncrement(&once) == 1)
		sw_log("SwapChain Present");
	if (!c->dev)
		return D3DERR_DEVICELOST;
	return Dev_Present(c->dev, src, dst, hwnd, dirty);
}

static HRESULT WINAPI Chain_GetFrontBufferData(IDirect3DSwapChain9 *this, IDirect3DSurface9 *dst)
{
	(void)this;
	(void)dst;
	return D3D_OK;
}

static HRESULT WINAPI Chain_GetBackBuffer(IDirect3DSwapChain9 *this, UINT i, D3DBACKBUFFER_TYPE type,
					  IDirect3DSurface9 **out)
{
	SwSwap *c = swap_from(this);
	if (!c->dev)
		return D3DERR_DEVICELOST;
	return Dev_GetBackBuffer(c->dev, 0, i, type, out);
}

static HRESULT WINAPI Chain_GetRasterStatus(IDirect3DSwapChain9 *this, D3DRASTER_STATUS *st)
{
	SwSwap *c = swap_from(this);
	if (!c->dev)
		return D3DERR_DEVICELOST;
	return Dev_GetRasterStatus(c->dev, 0, st);
}

static HRESULT WINAPI Chain_GetDisplayMode(IDirect3DSwapChain9 *this, D3DDISPLAYMODE *mode)
{
	SwSwap *c = swap_from(this);
	if (!c->dev)
		return D3DERR_DEVICELOST;
	return Dev_GetDisplayMode(c->dev, 0, mode);
}

static HRESULT WINAPI Chain_GetDevice(IDirect3DSwapChain9 *this, IDirect3DDevice9 **out)
{
	SwSwap *c = swap_from(this);
	if (!out)
		return E_POINTER;
	*out = c->dev;
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static HRESULT WINAPI Chain_GetPresentParameters(IDirect3DSwapChain9 *this, D3DPRESENT_PARAMETERS *pp)
{
	SwSwap *c = swap_from(this);
	if (!pp)
		return E_POINTER;
	if (!c->dev)
		return D3DERR_DEVICELOST;
	*pp = ((SwDevice *)c->dev)->pp;
	return D3D_OK;
}

static void ensure_swap_vtbl(void)
{
	static int ready;
	if (ready)
		return;
	memset(&kSwapVtbl, 0, sizeof(kSwapVtbl));
	kSwapVtbl.QueryInterface = Chain_QueryInterface;
	kSwapVtbl.AddRef = Chain_AddRef;
	kSwapVtbl.Release = Chain_Release;
	kSwapVtbl.Present = Chain_Present;
	kSwapVtbl.GetFrontBufferData = Chain_GetFrontBufferData;
	kSwapVtbl.GetBackBuffer = Chain_GetBackBuffer;
	kSwapVtbl.GetRasterStatus = Chain_GetRasterStatus;
	kSwapVtbl.GetDisplayMode = Chain_GetDisplayMode;
	kSwapVtbl.GetDevice = Chain_GetDevice;
	kSwapVtbl.GetPresentParameters = Chain_GetPresentParameters;
	ready = 1;
}

static HRESULT WINAPI Dev_GetSwapChain(IDirect3DDevice9 *this, UINT i, IDirect3DSwapChain9 **out)
{
	SwDevice *d = dev_from(this);
	static LONG once;
	if (i != 0 || !out)
		return D3DERR_INVALIDCALL;
	ensure_swap_vtbl();
	if (!d->chain) {
		d->chain = (SwSwap *)calloc(1, sizeof(SwSwap));
		if (!d->chain)
			return E_OUTOFMEMORY;
		d->chain->iface.lpVtbl = &kSwapVtbl;
		d->chain->ref = 1;
		d->chain->dev = this;
	}
	d->chain->iface.lpVtbl->AddRef(&d->chain->iface);
	*out = &d->chain->iface;
	if (InterlockedIncrement(&once) == 1)
		sw_log("GetSwapChain 0");
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetRasterStatus(IDirect3DDevice9 *this, UINT swapchain,
					  D3DRASTER_STATUS *st)
{
	LARGE_INTEGER t, f;
	DWORD line;
	(void)this;
	if (swapchain != 0 || !st)
		return D3DERR_INVALIDCALL;
	QueryPerformanceCounter(&t);
	QueryPerformanceFrequency(&f);
	if (f.QuadPart <= 0)
		f.QuadPart = 1;
	line = (DWORD)((t.QuadPart * 525) / f.QuadPart);
	st->ScanLine = line % 480;
	st->InVBlank = (st->ScanLine < 16) ? TRUE : FALSE;
	return D3D_OK;
}

static void WINAPI Dev_SetGammaRamp(IDirect3DDevice9 *this, UINT swap, DWORD flags,
				    const D3DGAMMARAMP *ramp)
{
	(void)this;
	(void)swap;
	(void)flags;
	(void)ramp;
}

static void WINAPI Dev_GetGammaRamp(IDirect3DDevice9 *this, UINT swap, D3DGAMMARAMP *ramp)
{
	int i;
	(void)this;
	(void)swap;
	if (!ramp)
		return;
	for (i = 0; i < 256; i++)
		ramp->red[i] = ramp->green[i] = ramp->blue[i] = (WORD)(i << 8);
}

static HRESULT WINAPI Dev_BeginScene(IDirect3DDevice9 *this)
{
	(void)this;
	return D3D_OK;
}

static HRESULT WINAPI Dev_EndScene(IDirect3DDevice9 *this)
{
	(void)this;
	return D3D_OK;
}

static HRESULT WINAPI Dev_Clear(IDirect3DDevice9 *this, DWORD rect_count, const D3DRECT *rects,
				DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	SwDevice *d = dev_from(this);
	(void)rect_count;
	(void)rects;
	(void)stencil;
	if (flags & D3DCLEAR_TARGET) {
		SwRast tmp;
		device_rt_rast(d, &tmp);
		swrast_clear_color(&tmp, (uint32_t)color);
		d->frame_clears++;
	}
	if (flags & D3DCLEAR_ZBUFFER)
		swrast_clear_depth(&d->rast, z);
	return D3D_OK;
}

static D3DMATRIX *matrix_slot(SwDevice *d, D3DTRANSFORMSTATETYPE state)
{
	if (state == D3DTS_WORLD)
		return &d->world;
	if (state == D3DTS_VIEW)
		return &d->view;
	if (state == D3DTS_PROJECTION)
		return &d->proj;
	return NULL;
}

static HRESULT WINAPI Dev_SetTransform(IDirect3DDevice9 *this, D3DTRANSFORMSTATETYPE state,
				       const D3DMATRIX *matrix)
{
	D3DMATRIX *slot = matrix_slot(dev_from(this), state);
	if (!slot || !matrix)
		return D3DERR_INVALIDCALL;
	*slot = *matrix;
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetTransform(IDirect3DDevice9 *this, D3DTRANSFORMSTATETYPE state,
				       D3DMATRIX *matrix)
{
	D3DMATRIX *slot = matrix_slot(dev_from(this), state);
	if (!slot || !matrix)
		return D3DERR_INVALIDCALL;
	*matrix = *slot;
	return D3D_OK;
}

static HRESULT WINAPI Dev_MultiplyTransform(IDirect3DDevice9 *this, D3DTRANSFORMSTATETYPE state,
					    const D3DMATRIX *matrix)
{
	(void)this;
	(void)state;
	(void)matrix;
	return E_NOTIMPL;
}

static HRESULT WINAPI Dev_SetViewport(IDirect3DDevice9 *this, const D3DVIEWPORT9 *vp)
{
	if (!vp)
		return D3DERR_INVALIDCALL;
	dev_from(this)->viewport = *vp;
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetViewport(IDirect3DDevice9 *this, D3DVIEWPORT9 *vp)
{
	if (!vp)
		return E_POINTER;
	*vp = dev_from(this)->viewport;
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetRenderState(IDirect3DDevice9 *this, D3DRENDERSTATETYPE state,
					 DWORD value)
{
	if ((UINT)state < 256)
		dev_from(this)->rs[state] = value;
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetRenderState(IDirect3DDevice9 *this, D3DRENDERSTATETYPE state,
					 DWORD *value)
{
	if (!value)
		return E_POINTER;
	*value = ((UINT)state < 256) ? dev_from(this)->rs[state] : 0;
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetFVF(IDirect3DDevice9 *this, DWORD fvf)
{
	SwDevice *d = dev_from(this);
	d->fvf = fvf;
	if (d->decl) {
		d->decl->lpVtbl->Release(d->decl);
		d->decl = NULL;
	}
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetFVF(IDirect3DDevice9 *this, DWORD *fvf)
{
	if (!fvf)
		return E_POINTER;
	*fvf = dev_from(this)->fvf;
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetSoftwareVertexProcessing(IDirect3DDevice9 *this, WINBOOL b)
{
	(void)this;
	(void)b;
	return D3D_OK;
}

static WINBOOL WINAPI Dev_GetSoftwareVertexProcessing(IDirect3DDevice9 *this)
{
	(void)this;
	return TRUE;
}

static float WINAPI Dev_GetNPatchMode(IDirect3DDevice9 *this)
{
	(void)this;
	return 0.0f;
}

static int fvf_stride(DWORD fvf)
{
	int n = 0;
	switch (fvf & D3DFVF_POSITION_MASK) {
	case D3DFVF_XYZRHW:
		n += 16;
		break;
	case D3DFVF_XYZ:
		n += 12;
		break;
	default:
		return 0;
	}
	if (fvf & D3DFVF_NORMAL)
		n += 12;
	if (fvf & D3DFVF_PSIZE)
		n += 4;
	if (fvf & D3DFVF_DIFFUSE)
		n += 4;
	if (fvf & D3DFVF_SPECULAR)
		n += 4;
	n += 8 * (int)((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
	return n;
}

static SwVert load_xyzrhw(const unsigned char *p, DWORD fvf)
{
	SwVert v;
	memset(&v, 0, sizeof(v));
	v.rhw = 1.0f;
	v.color = 0xffffffff;
	if ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) {
		memcpy(&v.x, p, 16);
		p += 16;
	} else if ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZ) {
		memcpy(&v.x, p, 12);
		p += 12;
	}
	if (fvf & D3DFVF_NORMAL)
		p += 12;
	if (fvf & D3DFVF_PSIZE)
		p += 4;
	if (fvf & D3DFVF_DIFFUSE) {
		memcpy(&v.color, p, 4);
		p += 4;
	}
	if ((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT) {
		memcpy(&v.u, p, 8);
	}
	return v;
}

static int decl_type_size(BYTE type)
{
	switch (type) {
	case D3DDECLTYPE_FLOAT1:
		return 4;
	case D3DDECLTYPE_FLOAT2:
	case D3DDECLTYPE_SHORT2:
	case D3DDECLTYPE_USHORT2N:
	case D3DDECLTYPE_FLOAT16_2:
		return 8;
	case D3DDECLTYPE_FLOAT3:
	case D3DDECLTYPE_UDEC3:
	case D3DDECLTYPE_DEC3N:
		return 12;
	case D3DDECLTYPE_FLOAT4:
	case D3DDECLTYPE_SHORT4:
	case D3DDECLTYPE_SHORT4N:
	case D3DDECLTYPE_USHORT4N:
	case D3DDECLTYPE_FLOAT16_4:
		return 16;
	case D3DDECLTYPE_D3DCOLOR:
	case D3DDECLTYPE_UBYTE4:
	case D3DDECLTYPE_UBYTE4N:
	case D3DDECLTYPE_SHORT2N:
		return 4;
	default:
		return 4;
	}
}

static UINT decl_stride0(const D3DVERTEXELEMENT9 *elems)
{
	UINT max = 0;
	for (; elems->Stream != 0xff; elems++) {
		UINT end;
		if (elems->Stream != 0)
			continue;
		end = (UINT)elems->Offset + (UINT)decl_type_size(elems->Type);
		if (end > max)
			max = end;
	}
	return max;
}

static SwVert load_decl(const unsigned char *p, const D3DVERTEXELEMENT9 *elems)
{
	SwVert v;
	memset(&v, 0, sizeof(v));
	v.rhw = 1.0f;
	v.color = 0xffffffff;
	for (; elems->Stream != 0xff; elems++) {
		const unsigned char *e;
		if (elems->Stream != 0)
			continue;
		e = p + elems->Offset;
		if (elems->Usage == D3DDECLUSAGE_POSITIONT && elems->Type == D3DDECLTYPE_FLOAT4)
			memcpy(&v.x, e, 16);
		else if (elems->Usage == D3DDECLUSAGE_POSITION && elems->Type == D3DDECLTYPE_FLOAT3)
			memcpy(&v.x, e, 12);
		else if (elems->Usage == D3DDECLUSAGE_POSITION && elems->Type == D3DDECLTYPE_FLOAT4)
			memcpy(&v.x, e, 16);
		else if (elems->Usage == D3DDECLUSAGE_COLOR && elems->UsageIndex == 0) {
			if (elems->Type == D3DDECLTYPE_D3DCOLOR)
				memcpy(&v.color, e, 4);
			else if (elems->Type == D3DDECLTYPE_FLOAT4) {
				const float *f = (const float *)e;
				int r = (int)(f[0] * 255.0f + 0.5f);
				int g = (int)(f[1] * 255.0f + 0.5f);
				int b = (int)(f[2] * 255.0f + 0.5f);
				int a = (int)(f[3] * 255.0f + 0.5f);
				if (r < 0) r = 0;
				if (g < 0) g = 0;
				if (b < 0) b = 0;
				if (a < 0) a = 0;
				if (r > 255) r = 255;
				if (g > 255) g = 255;
				if (b > 255) b = 255;
				if (a > 255) a = 255;
				v.color = ((DWORD)a << 24) | ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
			}
		} else if (elems->Usage == D3DDECLUSAGE_TEXCOORD && elems->UsageIndex == 0) {
			if (elems->Type == D3DDECLTYPE_FLOAT2)
				memcpy(&v.u, e, 8);
			else if (elems->Type == D3DDECLTYPE_FLOAT1) {
				v.u = *(const float *)e;
				v.v = 0.0f;
			} else if (elems->Type == D3DDECLTYPE_FLOAT4)
				memcpy(&v.u, e, 8);
		}
	}
	return v;
}

static int decl_is_transformed(const D3DVERTEXELEMENT9 *elems)
{
	for (; elems && elems->Stream != 0xff; elems++) {
		if (elems->Stream == 0 && elems->Usage == D3DDECLUSAGE_POSITIONT)
			return 1;
	}
	return 0;
}

static void mat_mul(D3DMATRIX *out, const D3DMATRIX *a, const D3DMATRIX *b)
{
	D3DMATRIX t;
	int i, j, k;
	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) {
			float s = 0.0f;
			for (k = 0; k < 4; k++)
				s += a->m[i][k] * b->m[k][j];
			t.m[i][j] = s;
		}
	}
	*out = t;
}

static void xform_vec(const D3DMATRIX *m, float x, float y, float z, float w, SwVert *v)
{
	v->x = x * m->m[0][0] + y * m->m[1][0] + z * m->m[2][0] + w * m->m[3][0];
	v->y = x * m->m[0][1] + y * m->m[1][1] + z * m->m[2][1] + w * m->m[3][1];
	v->z = x * m->m[0][2] + y * m->m[1][2] + z * m->m[2][2] + w * m->m[3][2];
	v->rhw = x * m->m[0][3] + y * m->m[1][3] + z * m->m[2][3] + w * m->m[3][3];
}

static int vs_c_nonzero(const SwDevice *d)
{
	int r, c;
	for (r = 0; r < 4; r++) {
		for (c = 0; c < 4; c++) {
			if (d->vs_c[r][c] != 0.0f)
				return 1;
		}
	}
	return 0;
}

static D3DMATRIX draw_wvp(SwDevice *d)
{
	D3DMATRIX wv, wvp;
	if (d->vs && vs_c_nonzero(d)) {
		int r, c;
		memset(&wvp, 0, sizeof(wvp));
		for (r = 0; r < 4; r++) {
			for (c = 0; c < 4; c++)
				wvp.m[r][c] = d->vs_c[r][c];
		}
		return wvp;
	}
	mat_mul(&wv, &d->world, &d->view);
	mat_mul(&wvp, &wv, &d->proj);
	return wvp;
}

static void viewport_map(const D3DVIEWPORT9 *vp, SwVert *v)
{
	float w = v->rhw != 0.0f ? v->rhw : 1.0f;
	float nx = v->x / w;
	float ny = v->y / w;
	float nz = v->z / w;
	v->x = (nx + 1.0f) * 0.5f * (float)vp->Width + (float)vp->X;
	v->y = (1.0f - ny) * 0.5f * (float)vp->Height + (float)vp->Y;
	v->z = nz * (vp->MaxZ - vp->MinZ) + vp->MinZ;
	v->rhw = 1.0f / w;
}

static int vert_in_px(const SwVert *v, int w, int h)
{
	return v->x >= -8.0f && v->x <= (float)w + 8.0f && v->y >= -8.0f &&
	       v->y <= (float)h + 8.0f;
}

static int tri_looks_pixel(const SwTri *t, int w, int h)
{
	float maxc = fabsf(t->a.x);
	maxc = fmaxf(maxc, fabsf(t->a.y));
	maxc = fmaxf(maxc, fabsf(t->b.x));
	maxc = fmaxf(maxc, fabsf(t->b.y));
	maxc = fmaxf(maxc, fabsf(t->c.x));
	maxc = fmaxf(maxc, fabsf(t->c.y));
	return maxc > 2.5f && vert_in_px(&t->a, w, h) && vert_in_px(&t->b, w, h) &&
	       vert_in_px(&t->c, w, h);
}

static void tri_ix(D3DPRIMITIVETYPE type, UINT i, UINT *i0, UINT *i1, UINT *i2)
{
	if (type == D3DPT_TRIANGLESTRIP) {
		if (i & 1u) {
			*i0 = i + 1u;
			*i1 = i;
			*i2 = i + 2u;
		} else {
			*i0 = i;
			*i1 = i + 1u;
			*i2 = i + 2u;
		}
	} else if (type == D3DPT_TRIANGLEFAN) {
		*i0 = 0;
		*i1 = i + 1u;
		*i2 = i + 2u;
	} else {
		*i0 = i * 3u;
		*i1 = i * 3u + 1u;
		*i2 = i * 3u + 2u;
	}
}

static void log_draw(SwDevice *d, const char *api, D3DPRIMITIVETYPE type, UINT prims, HRESULT hr)
{
	static volatile LONG n;
	char msg[220];
	if (InterlockedIncrement(&n) > 12)
		return;
	_snprintf(msg, sizeof(msg),
		  "%s type=%u prims=%u hr=%08x decl=%d vs=%d fvf=%08x rt=%d", api, (unsigned)type,
		  prims, (unsigned)hr, d->decl != NULL, d->vs != NULL, (unsigned)d->fvf,
		  d->rt0 != NULL);
	sw_log(msg);
}

static void device_rt_rast(SwDevice *d, SwRast *out)
{
	SwSurface *s = d->rt0 ? (SwSurface *)d->rt0 : NULL;
	*out = d->rast;
	if (!s)
		return;
	if (s->tex && s->tex->pixels) {
		out->color = s->tex->pixels;
		out->width = s->tex->w;
		out->height = s->tex->h;
		out->depth = NULL;
		return;
	}
	if (s->bits && (s->usage & D3DUSAGE_RENDERTARGET)) {
		out->color = (uint32_t *)s->bits;
		out->width = s->w;
		out->height = s->h;
		out->depth = NULL;
	}
}

static const unsigned char *stream_at(SwDevice *d, UINT stream, UINT geom_index, UINT inst,
				      int instanced)
{
	IDirect3DVertexBuffer9 *vb = NULL;
	UINT stride = 0, base = 0;
	SwVB *b;
	UINT idx;
	UINT64 off;
	int use_inst;

	if (stream == 1) {
		vb = d->vb1;
		stride = d->vb1_stride;
		base = d->vb1_off;
	} else if (stream == 2) {
		vb = d->vb2;
		stride = d->vb2_stride;
		base = d->vb2_off;
	} else
		return NULL;
	if (!vb)
		return NULL;
	b = (SwVB *)vb;
	if (!stride || !b->bytes)
		return NULL;
	use_inst = instanced || (d->stream_freq[stream] & D3DSTREAMSOURCE_INSTANCEDATA) != 0;
	idx = use_inst ? inst : geom_index;
	off = (UINT64)base + (UINT64)idx * stride;
	if (off + stride > b->size)
		return NULL;
	return b->bytes + (size_t)off;
}

static int shader_has_op(const DWORD *code, UINT bytes, int want)
{
	UINT pc, ndwords;
	if (!code || bytes < 8)
		return 0;
	ndwords = bytes / 4u;
	pc = 1;
	while (pc < ndwords) {
		DWORD op = code[pc];
		int opcode = (int)(op & D3DSI_OPCODE_MASK);
		int extra = (int)((op & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT);
		if (opcode == D3DSIO_END)
			break;
		if (opcode == D3DSIO_COMMENT) {
			pc += 1u + (UINT)((op & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT);
			continue;
		}
		if (opcode == want)
			return 1;
		if (extra < 0)
			extra = 0;
		pc += 1u + (UINT)extra;
	}
	return 0;
}

static int ps_tex_only(SwDevice *d)
{
	SwShader *ps;
	if (!d->ps)
		return 0;
	ps = (SwShader *)d->ps;
	if (!shader_has_op(ps->code, ps->bytes, D3DSIO_TEX))
		return 0;
	if (shader_has_op(ps->code, ps->bytes, D3DSIO_MUL) ||
	    shader_has_op(ps->code, ps->bytes, D3DSIO_MAD))
		return 0;
	return 1;
}

static void dump_tex_once(SwTexture *t, const char *path)
{
	SwRast r;
	char name[80];
	static UINT seen[8];
	static int nseen;
	UINT key;
	int i;
	(void)path;
	if (!t || !t->pixels)
		return;
	if (t->usage & D3DUSAGE_RENDERTARGET)
		return;
	{
		const uint32_t *p = t->pixels;
		UINT n = (UINT)t->w * (UINT)t->h;
		UINT j, step;
		int hit = 0;
		if (n == 0)
			return;
		step = n > 8192u ? 17u : 1u;
		for (j = 0; j < n; j += step) {
			if ((p[j] & 0x00ffffffu) != 0) {
				hit = 1;
				break;
			}
		}
		if (!hit)
			return;
	}
	key = ((UINT)t->w << 16) | (UINT)t->h;
	for (i = 0; i < nseen; i++) {
		if (seen[i] == key)
			return;
	}
	if (nseen >= 8)
		return;
	seen[nseen++] = key;
	memset(&r, 0, sizeof(r));
	r.color = t->pixels;
	r.width = t->w;
	r.height = t->h;
	_snprintf(name, sizeof(name), "d3d9_sw_tex_%dx%d.tga", t->w, t->h);
	if (swrast_dump_tga(&r, name)) {
		char msg[96];
		_snprintf(msg, sizeof(msg), "dumped %s fmt=%08x pool=%u", name, (unsigned)t->fmt,
			  (unsigned)t->pool);
		sw_log(msg);
	}
}

static uint32_t colorwrite_mask(DWORD cw)
{
	uint32_t m = 0;
	if (cw & D3DCOLORWRITEENABLE_RED)
		m |= 0x00ff0000u;
	if (cw & D3DCOLORWRITEENABLE_GREEN)
		m |= 0x0000ff00u;
	if (cw & D3DCOLORWRITEENABLE_BLUE)
		m |= 0x000000ffu;
	if (cw & D3DCOLORWRITEENABLE_ALPHA)
		m |= 0xff000000u;
	return m;
}

static int env_flag(const char *name, int dflt)
{
	char buf[8];
	DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
	if (n == 0 || n >= sizeof(buf))
		return dflt;
	return buf[0] != '0';
}

/* Keeping the text services framework out of the process.
 *
 * MSCTF keeps its objects on the process heap but its pointers to them in module
 * state, and a rewind restores the former without the latter. On a machine with
 * an IME loaded it eventually dereferences a block that now belongs to something
 * else and faults reading a fragment of a wide string. Observed twice on the
 * same instruction with the same impossible address, roughly a minute after
 * restores that were themselves clean.
 *
 * There is nothing to lose here: the game takes input through DirectInput and
 * XInput, and its score-entry screen draws its own characters rather than asking
 * Windows for text. The escape hatch exists only because disabling IME for a
 * whole process is the kind of thing that deserves one. */
static int ime_unwanted(void)
{
	return !env_flag("D3D9SW_KEEP_IME", 0);
}

static void ime_detach_process(void)
{
	static LONG once;
	HMODULE imm;
	BOOL(WINAPI * disable)(DWORD);

	if (InterlockedExchange(&once, 1) || !ime_unwanted())
		return;
	imm = GetModuleHandleA("imm32.dll");
	if (!imm)
		imm = LoadLibraryA("imm32.dll");
	if (!imm)
		return;
	disable = (BOOL(WINAPI *)(DWORD))(void *)GetProcAddress(imm, "ImmDisableIME");
	/* -1 covers every thread, including ones the game has yet to create. */
	if (disable && disable((DWORD)-1))
		sw_trace("ime: disabled for the process");
}

/* ImmDisableIME only governs windows created after it, so any window already
 * standing when we first run has to be detached by hand. */
static void ime_detach_window(HWND hwnd)
{
	HMODULE imm;
	HANDLE(WINAPI * assoc)(HWND, HANDLE);

	if (!hwnd || !ime_unwanted())
		return;
	imm = GetModuleHandleA("imm32.dll");
	if (!imm)
		return;
	assoc = (HANDLE(WINAPI *)(HWND, HANDLE))(void *)GetProcAddress(imm,
									"ImmAssociateContext");
	if (assoc)
		assoc(hwnd, NULL);
}

/* Backface culling is the one piece of newly-honoured state that can hide
 * geometry outright if the winding convention is wrong, so leave a switch. */
static int cull_enabled(void)
{
	static int cached = -1;
	if (cached < 0)
		cached = env_flag("D3D9SW_NOCULL", 0) ? 0 : 1;
	return cached;
}

/* Overriding shader-computed UVs with a bilerp of the instance rect is a guess.
 * The interpreted vertex shader already derives TEXCOORD0 from the same data,
 * so this stays off unless it is explicitly asked for. */
static int sprite_uv_hack(void)
{
	static int cached = -1;
	if (cached < 0)
		cached = env_flag("D3D9SW_SPRITEUV", 0);
	return cached;
}

/* Every D3D9 filter mode above POINT interpolates: LINEAR, ANISOTROPIC, and
 * the pyramidal and gaussian quad modes. Testing for LINEAR alone demoted the
 * rest to point sampling, which turns any non-integer scale into duplicated
 * and dropped rows and columns. */
static int filter_is_smooth(DWORD f)
{
	return f > (DWORD)D3DTEXF_POINT;
}

/* D3D9SW_FILTER=linear forces interpolation on every draw, point forces it off.
 * Diagnostic: it separates sampling artefacts from everything else in one run. */
static int filter_override(void)
{
	static int cached = -2;
	if (cached == -2) {
		const char *v = getenv("D3D9SW_FILTER");
		cached = !v ? -1 : (*v == 'l' || *v == 'L') ? 1 : (*v == 'p' || *v == 'P') ? 0 : -1;
	}
	return cached;
}

static int pixel_uv_hack(void)
{
	static int cached = -1;
	if (cached < 0)
		cached = env_flag("D3D9SW_PIXELUV", 1);
	return cached;
}

/* D3D9 puts pixel centres at integer screen coordinates. This rasterizer
 * samples at i+0.5, which is the D3D10 convention and everything after it.
 *
 * That half pixel is the entire reason D3D9 titles carry the notorious -0.5
 * vertex offset - the SDK's own "Directly Mapping Texels to Pixels" tells them
 * to apply it so a pixel centre lands on a texel centre. This game does exactly
 * that: its composite quad runs -0.5..1279.5 across 1280 texels.
 *
 * Read back under our convention, that same quad puts every pixel centre
 * exactly on a texel *boundary*, which is the worst place it could possibly be.
 * With linear filtering it is a 2x2 average of the entire frame, which is the
 * blur already measured at 100% agreement. With point filtering the choice
 * between two adjacent texels comes down to the last bit of the interpolator,
 * so a row where that bit tips reads the neighbouring sprite out of a packed
 * atlas and returns an unrelated, saturated colour - inside one draw call, with
 * one owner, along a shared edge. The seams.
 *
 * Shifting screen positions by +0.5 puts our sample point back where the game
 * intends it. Every transform path has to get it, so it happens at the single
 * point they all funnel through, just before rasterizing. */
static int half_pixel_fix(void)
{
	static int cached = -1;
	if (cached < 0)
		cached = env_flag("D3D9SW_HALFPIXEL", 1);
	return cached;
}

static void draw_state(SwDevice *d, SwState *s, int bilinear, int rt_w, int rt_h)
{
	swrast_state_defaults(s);
	s->bilinear = bilinear;
	s->z_enable = d->rs[D3DRS_ZENABLE] != 0 && d->rs[D3DRS_ZENABLE] != D3DZB_FALSE;
	s->z_write = d->rs[D3DRS_ZWRITEENABLE] != 0;
	s->z_func = (int)d->rs[D3DRS_ZFUNC];
	s->blend_enable = d->rs[D3DRS_ALPHABLENDENABLE] != 0;
	s->src_blend = (int)d->rs[D3DRS_SRCBLEND];
	s->dst_blend = (int)d->rs[D3DRS_DESTBLEND];
	s->blend_op = (int)d->rs[D3DRS_BLENDOP];
	s->blend_factor = (uint32_t)d->rs[D3DRS_BLENDFACTOR];
	s->alpha_test = d->rs[D3DRS_ALPHATESTENABLE] != 0;
	s->alpha_func = (int)d->rs[D3DRS_ALPHAFUNC];
	s->alpha_ref = (int)(d->rs[D3DRS_ALPHAREF] & 0xffu);
	s->cull = cull_enabled() ? (int)d->rs[D3DRS_CULLMODE] : D3DCULL_NONE;
	s->addr_u = (int)d->samp_addru;
	s->addr_v = (int)d->samp_addrv;
	s->write_mask = colorwrite_mask(d->rs[D3DRS_COLORWRITEENABLE]);
	if (d->rs[D3DRS_SCISSORTESTENABLE]) {
		s->scissor_enable = 1;
		s->scissor_x0 = (int)d->scissor.left;
		s->scissor_y0 = (int)d->scissor.top;
		s->scissor_x1 = (int)d->scissor.right;
		s->scissor_y1 = (int)d->scissor.bottom;
	}
	(void)rt_w;
	(void)rt_h;
}

static int tri_in_front(const SwTri *t)
{
	/* Pre-divide w. Anything at or behind the eye plane produces a mirrored,
	 * unbounded projection, so drop it rather than rasterise garbage. */
	return t->a.rhw > 1.0e-6f && t->b.rhw > 1.0e-6f && t->c.rhw > 1.0e-6f;
}

/* A vertex shader's oPos is homogeneous clip space by definition, so this is
 * unconditional: there is nothing to sniff for. */
static UINT apply_vs_viewport(SwDevice *d, SwTri *batch, UINT n)
{
	UINT i, kept = 0;
	for (i = 0; i < n; i++) {
		if (!tri_in_front(&batch[i]))
			continue;
		batch[kept] = batch[i];
		viewport_map(&d->viewport, &batch[kept].a);
		viewport_map(&d->viewport, &batch[kept].b);
		viewport_map(&d->viewport, &batch[kept].c);
		kept++;
	}
	return kept;
}

static int decl_elem(const D3DVERTEXELEMENT9 *elems, BYTE stream, BYTE usage, BYTE idx,
		     const D3DVERTEXELEMENT9 **out)
{
	const D3DVERTEXELEMENT9 *e;
	if (!elems)
		return 0;
	for (e = elems; e->Stream != 0xff; e++) {
		if (e->Stream == stream && e->Usage == usage && e->UsageIndex == idx) {
			if (out)
				*out = e;
			return 1;
		}
	}
	return 0;
}

static void read_decl_f2(const unsigned char *base, const D3DVERTEXELEMENT9 *e, float *x, float *y)
{
	const float *p;
	*x = 0.0f;
	*y = 0.0f;
	if (!base || !e)
		return;
	p = (const float *)(base + e->Offset);
	*x = p[0];
	*y = p[1];
}

static int sprite_inst_uv(const unsigned char *s0, const unsigned char *s1,
			  const D3DVERTEXELEMENT9 *decl, int tw, int th, SwVert *v)
{
	const D3DVERTEXELEMENT9 *local, *c1, *c2, *c3, *c4;
	float lu, lv, t1u, t1v, t2u, t2v, t3u, t3v, t4u, t4v, w00, w10, w01, w11;
	float minu, maxu, minv, maxv;
	if (!s0 || !s1 || !decl || !v)
		return 0;
	if (!decl_elem(decl, 0, D3DDECLUSAGE_TEXCOORD, 0, &local) ||
	    !decl_elem(decl, 1, D3DDECLUSAGE_TEXCOORD, 1, &c1) ||
	    !decl_elem(decl, 1, D3DDECLUSAGE_TEXCOORD, 2, &c2) ||
	    !decl_elem(decl, 1, D3DDECLUSAGE_TEXCOORD, 3, &c3) ||
	    !decl_elem(decl, 1, D3DDECLUSAGE_TEXCOORD, 4, &c4))
		return 0;
	read_decl_f2(s0, local, &lu, &lv);
	read_decl_f2(s1, c1, &t1u, &t1v);
	read_decl_f2(s1, c2, &t2u, &t2v);
	read_decl_f2(s1, c3, &t3u, &t3v);
	read_decl_f2(s1, c4, &t4u, &t4v);
	if (lu < -0.1f || lu > 1.1f || lv < -0.1f || lv > 1.1f)
		return 0;
	if (tw < 1)
		tw = 1;
	if (th < 1)
		th = 1;
	if (fabsf(t1u) > 2.0f || fabsf(t1v) > 2.0f || fabsf(t2u) > 2.0f || fabsf(t2v) > 2.0f ||
	    fabsf(t3u) > 2.0f || fabsf(t3v) > 2.0f || fabsf(t4u) > 2.0f || fabsf(t4v) > 2.0f) {
		float fw = (float)tw, fh = (float)th;
		t1u /= fw;
		t1v /= fh;
		t2u /= fw;
		t2v /= fh;
		t3u /= fw;
		t3v /= fh;
		t4u /= fw;
		t4v /= fh;
	}
	minu = fminf(t1u, fminf(t2u, fminf(t3u, t4u)));
	maxu = fmaxf(t1u, fmaxf(t2u, fmaxf(t3u, t4u)));
	minv = fminf(t1v, fminf(t2v, fminf(t3v, t4v)));
	maxv = fmaxf(t1v, fmaxf(t2v, fmaxf(t3v, t4v)));
	if ((maxu - minu) > 0.55f || (maxv - minv) > 0.55f)
		return 0;
	w00 = (1.0f - lu) * (1.0f - lv);
	w10 = lu * (1.0f - lv);
	w01 = (1.0f - lu) * lv;
	w11 = lu * lv;
	v->u = w00 * t1u + w10 * t2u + w01 * t3u + w11 * t4u;
	v->v = w00 * t1v + w10 * t2v + w01 * t3v + w11 * t4v;
	return 1;
}

/* Some draws hand us TEXCOORD0 in texel units because the real pixel shader
 * (which we do not interpret) would have scaled it. Guess from the UV range:
 * anything staying inside a couple of units is already normalised, and anything
 * running off the end of the texture is a genuinely tiled UV such as a
 * scrolling background, which must be left alone. */
static int uv_looks_like_texels(float minu, float maxu, float minv, float maxv, int tw, int th)
{
	if (tw < 1 || th < 1)
		return 0;
	if (maxu <= 2.0f && minu >= -2.0f && maxv <= 2.0f && minv >= -2.0f)
		return 0;
	if (minu < -1.0f || minv < -1.0f)
		return 0;
	if (maxu > (float)tw + 1.0f || maxv > (float)th + 1.0f)
		return 0;
	return 1;
}

static void tri_uv_bounds(const SwTri *t, float *minu, float *maxu, float *minv, float *maxv)
{
	const SwVert *v[3];
	int k;
	v[0] = &t->a;
	v[1] = &t->b;
	v[2] = &t->c;
	*minu = *maxu = v[0]->u;
	*minv = *maxv = v[0]->v;
	for (k = 1; k < 3; k++) {
		*minu = fminf(*minu, v[k]->u);
		*maxu = fmaxf(*maxu, v[k]->u);
		*minv = fminf(*minv, v[k]->v);
		*maxv = fmaxf(*maxv, v[k]->v);
	}
}

/* Whether TEXCOORD0 arrives in texel units is a property of the pixel shader
 * and the bound texture, and both are fixed for the whole draw call. Asking the
 * question once per triangle let neighbours in one mesh answer differently, and
 * a triangle that declined the rescale then sampled at raw texel coordinates -
 * off by the size of the texture. Under wrap addressing that wraps into an
 * unrelated part of the atlas, so the pixels along one side of a shared edge
 * came back a saturated, unrelated colour: the seams.
 *
 * So decide once, over the union of every triangle's UVs, and apply the answer
 * to all of them. Returns the number of triangles that would have disagreed
 * with the batch, which is the number the old code would have torn loose. */
static UINT norm_pixel_uv_batch(SwTri *b, UINT n, int tw, int th)
{
	float minu, maxu, minv, maxv;
	UINT i, split = 0;
	int rescale;

	if (!b || !n || tw < 1 || th < 1)
		return 0;
	tri_uv_bounds(&b[0], &minu, &maxu, &minv, &maxv);
	for (i = 1; i < n; i++) {
		float tu0, tu1, tv0, tv1;
		tri_uv_bounds(&b[i], &tu0, &tu1, &tv0, &tv1);
		minu = fminf(minu, tu0);
		maxu = fmaxf(maxu, tu1);
		minv = fminf(minv, tv0);
		maxv = fmaxf(maxv, tv1);
	}
	rescale = uv_looks_like_texels(minu, maxu, minv, maxv, tw, th);

	for (i = 0; i < n; i++) {
		float tu0, tu1, tv0, tv1;
		SwVert *v[3];
		int k;
		tri_uv_bounds(&b[i], &tu0, &tu1, &tv0, &tv1);
		if (uv_looks_like_texels(tu0, tu1, tv0, tv1, tw, th) != rescale)
			split++;
		if (!rescale)
			continue;
		v[0] = &b[i].a;
		v[1] = &b[i].b;
		v[2] = &b[i].c;
		for (k = 0; k < 3; k++) {
			v[k]->u /= (float)tw;
			v[k]->v /= (float)th;
		}
	}
	return split;
}

static HRESULT raster_tri_bytes(SwDevice *d, D3DPRIMITIVETYPE type, UINT prims,
				const unsigned char *vb, UINT stride, UINT vb_bytes,
				const unsigned char *ib, D3DFORMAT ibfmt, UINT ib_bytes,
				INT base_vertex, UINT start_index)
{
	SwTex st;
	SwState rs;
	const SwTex *tex = NULL;
	UINT i, inst, total;
	int bilinear = filter_override() >= 0
			       ? filter_override()
			       : (filter_is_smooth(d->samp_mag) || filter_is_smooth(d->samp_min));
	DWORD fvf = d->fvf;
	const D3DVERTEXELEMENT9 *decl_elems = NULL;
	static SwTri *batch;
	static UINT batch_cap;
	UINT ib_elem = (ibfmt == D3DFMT_INDEX32) ? 4u : 2u;
	UINT idx_count;
	int transformed;
	int instanced = 0;
	UINT inst_count = 1;
	SwRast rt;
	SwShader *vsh = d->vs ? (SwShader *)d->vs : NULL;
	int used_vs = 0;
	int vs_clip = 0;

	d->frame_draw_calls++;
	if (!vb || prims == 0) {
		d->frame_rejects++;
		d->frame_reject_code = 1;
		return D3D_OK;
	}
	if (type != D3DPT_TRIANGLELIST && type != D3DPT_TRIANGLESTRIP &&
	    type != D3DPT_TRIANGLEFAN) {
		d->frame_rejects++;
		d->frame_reject_code = 2;
		return D3DERR_INVALIDCALL;
	}
	idx_count = (type == D3DPT_TRIANGLELIST) ? prims * 3u : prims + 2u;
	if (d->decl)
		decl_elems = ((SwDecl *)d->decl)->elems;
	if (!stride) {
		if (decl_elems)
			stride = decl_stride0(decl_elems);
		else
			stride = (UINT)fvf_stride(fvf);
	}
	if (!stride) {
		d->frame_rejects++;
		d->frame_reject_code = 3;
		return D3DERR_INVALIDCALL;
	}
	if (!decl_elems && fvf_stride(fvf) <= 0) {
		d->frame_rejects++;
		d->frame_reject_code = 4;
		return D3DERR_INVALIDCALL;
	}
	if (ib) {
		UINT last = start_index + idx_count;
		if ((UINT64)last * ib_elem > ib_bytes) {
			d->frame_rejects++;
			d->frame_reject_code = 5;
			return D3DERR_INVALIDCALL;
		}
	} else if ((UINT64)idx_count * stride > vb_bytes) {
		d->frame_rejects++;
		d->frame_reject_code = 6;
		return D3DERR_INVALIDCALL;
	}
	if (d->stream_freq[0] & D3DSTREAMSOURCE_INDEXEDDATA) {
		instanced = 1;
		inst_count = d->stream_freq[0] & 0x3fffffffu;
		if (inst_count < 1)
			inst_count = 1;
		if (inst_count > 2048)
			inst_count = 2048;
	}
	if (d->tex0) {
		SwTexture *stex = (SwTexture *)d->tex0;
		st.width = stex->w;
		st.height = stex->h;
		st.pixels = stex->pixels;
		tex = &st;
	}
	total = prims * inst_count;
	if (total > batch_cap) {
		SwTri *nbuf = (SwTri *)realloc(batch, (size_t)total * sizeof(SwTri));
		if (!nbuf)
			return E_OUTOFMEMORY;
		batch = nbuf;
		batch_cap = total;
	}
	for (inst = 0; inst < inst_count; inst++) {
		for (i = 0; i < prims; i++) {
			UINT a, b, c, i0, i1, i2;
			SwTri *tri = &batch[inst * prims + i];
			tri_ix(type, i, &a, &b, &c);
			if (ib) {
				UINT s0 = start_index + a;
				UINT s1 = start_index + b;
				UINT s2 = start_index + c;
				if (ibfmt == D3DFMT_INDEX32) {
					const DWORD *idx = (const DWORD *)ib;
					i0 = (UINT)((INT)idx[s0] + base_vertex);
					i1 = (UINT)((INT)idx[s1] + base_vertex);
					i2 = (UINT)((INT)idx[s2] + base_vertex);
				} else {
					const WORD *idx = (const WORD *)ib;
					i0 = (UINT)((INT)idx[s0] + base_vertex);
					i1 = (UINT)((INT)idx[s1] + base_vertex);
					i2 = (UINT)((INT)idx[s2] + base_vertex);
				}
			} else {
				i0 = a;
				i1 = b;
				i2 = c;
			}
			if ((UINT64)(i0 + 1u) * stride > vb_bytes ||
			    (UINT64)(i1 + 1u) * stride > vb_bytes ||
			    (UINT64)(i2 + 1u) * stride > vb_bytes) {
				d->frame_rejects++;
				d->frame_reject_code = 7;
				return D3DERR_INVALIDCALL;
			}
			if (vsh && vsh->code && decl_elems) {
				char err[64];
				const unsigned char *p0 = vb + (size_t)stride * i0;
				const unsigned char *p1 = vb + (size_t)stride * i1;
				const unsigned char *p2 = vb + (size_t)stride * i2;
				const unsigned char *i0s1 = stream_at(d, 1, i0, inst, instanced);
				const unsigned char *i1s1 = stream_at(d, 1, i1, inst, instanced);
				const unsigned char *i2s1 = stream_at(d, 1, i2, inst, instanced);
				const unsigned char *i0s2 = stream_at(d, 2, i0, inst, instanced);
				const unsigned char *i1s2 = stream_at(d, 2, i1, inst, instanced);
				const unsigned char *i2s2 = stream_at(d, 2, i2, inst, instanced);
				err[0] = 0;
				if (!vs_exec(vsh->code, vsh->bytes, d->vs_c, d->vs_c_ver, decl_elems,
					     p0, i0s1, i0s2, &tri->a, err, (int)sizeof(err)) ||
				    !vs_exec(vsh->code, vsh->bytes, d->vs_c, d->vs_c_ver, decl_elems,
					     p1, i1s1, i1s2, &tri->b, err, (int)sizeof(err)) ||
				    !vs_exec(vsh->code, vsh->bytes, d->vs_c, d->vs_c_ver, decl_elems,
					     p2, i2s1, i2s2, &tri->c, err, (int)sizeof(err))) {
					static LONG once;
					d->frame_vs_fail++;
					if (InterlockedIncrement(&once) == 1) {
						char msg[96];
						_snprintf(msg, sizeof(msg), "vs_exec fail %s", err);
						sw_log(msg);
					}
					tri->a = load_decl(p0, decl_elems);
					tri->b = load_decl(p1, decl_elems);
					tri->c = load_decl(p2, decl_elems);
				} else {
					float du, dv;
					used_vs = 1;
					d->frame_vs_ok++;
					du = fabsf(tri->a.u - tri->b.u) + fabsf(tri->b.u - tri->c.u) +
					     fabsf(tri->a.u - tri->c.u);
					dv = fabsf(tri->a.v - tri->b.v) + fabsf(tri->b.v - tri->c.v) +
					     fabsf(tri->a.v - tri->c.v);
					if ((du + dv) < 0.04f && i0s1 && sprite_uv_hack()) {
						int tw = tex ? tex->width : 1;
						int th = tex ? tex->height : 1;
						SwVert na = tri->a, nb = tri->b, nc = tri->c;
						if (sprite_inst_uv(p0, i0s1, decl_elems, tw, th, &na) &&
						    sprite_inst_uv(p1, i1s1, decl_elems, tw, th, &nb) &&
						    sprite_inst_uv(p2, i2s1, decl_elems, tw, th, &nc)) {
							tri->a = na;
							tri->b = nb;
							tri->c = nc;
						}
					}
				}
			} else if (decl_elems) {
				tri->a = load_decl(vb + (size_t)stride * i0, decl_elems);
				tri->b = load_decl(vb + (size_t)stride * i1, decl_elems);
				tri->c = load_decl(vb + (size_t)stride * i2, decl_elems);
			} else {
				tri->a = load_xyzrhw(vb + (size_t)stride * i0, fvf);
				tri->b = load_xyzrhw(vb + (size_t)stride * i1, fvf);
				tri->c = load_xyzrhw(vb + (size_t)stride * i2, fvf);
			}
		}
	}
	if (used_vs && total > 0) {
		total = apply_vs_viewport(d, batch, total);
		vs_clip = 1;
	}
	device_rt_rast(d, &rt);
	draw_state(d, &rs, bilinear, rt.width, rt.height);
	if (rt.color && rt.color != d->rast.color) {
		d->off_color = rt.color;
		d->off_w = rt.width;
		d->off_h = rt.height;
		d->off_vx = (int)d->viewport.X;
		d->off_vy = (int)d->viewport.Y;
		d->off_vw = (int)d->viewport.Width;
		d->off_vh = (int)d->viewport.Height;
		if (d->off_vw <= 0)
			d->off_vw = rt.width;
		if (d->off_vh <= 0)
			d->off_vh = rt.height;
		d->drew_off = 1;
	} else {
		d->drew_bb = 1;
	}
	transformed = decl_elems ? decl_is_transformed(decl_elems)
				 : ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW);
	if (!used_vs && !transformed && total > 0 && !tri_looks_pixel(&batch[0], rt.width, rt.height)) {
		D3DMATRIX wvp = draw_wvp(d);
		for (i = 0; i < total; i++) {
			SwVert *vs[3];
			int k;
			vs[0] = &batch[i].a;
			vs[1] = &batch[i].b;
			vs[2] = &batch[i].c;
			for (k = 0; k < 3; k++) {
				float x = vs[k]->x, y = vs[k]->y, z = vs[k]->z, w = vs[k]->rhw;
				if (w == 0.0f)
					w = 1.0f;
				xform_vec(&wvp, x, y, z, w, vs[k]);
			}
		}
		total = apply_vs_viewport(d, batch, total);
	}
	if (tex && total > 0) {
		int texonly = ps_tex_only(d);
		for (i = 0; i < total; i++) {
			SwVert *vs[3];
			int k;
			vs[0] = &batch[i].a;
			vs[1] = &batch[i].b;
			vs[2] = &batch[i].c;
			/* Only a texture-only shader justifies discarding the
			 * diffuse. Every loader already defaults a missing colour
			 * to white (load_decl, the FVF path, and vs_exec's attr0),
			 * so treating black as "unset" here just repainted every
			 * legitimately black surface white. */
			for (k = 0; k < 3; k++) {
				if (texonly)
					vs[k]->color = 0xffffffffu;
			}
		}
	}
	if (tex && total > 0 && pixel_uv_hack()) {
		UINT split = norm_pixel_uv_batch(batch, total, tex->width, tex->height);
		/* Capped: this fires per draw per frame, and a log that scrolls
		 * past at 60Hz is a log nobody reads. The first few say whether
		 * the old per-triangle vote was tearing meshes apart at all. */
		static LONG reported;
		if (split && InterlockedIncrement(&reported) <= 20) {
			char msg[192];
			_snprintf(msg, sizeof(msg),
				  "uv: draw of %u tris had %u disagree about texel-vs-"
				  "normalised UVs on a %dx%d texture; resolved batch-wide",
				  total, split, tex->width, tex->height);
			sw_log(msg);
		}
	}
	if (g_draw_log && total > 0) {
		float x0 = batch[0].a.x, x1 = x0, y0 = batch[0].a.y, y1 = y0;
		float u0 = batch[0].a.u, u1 = u0, v0 = batch[0].a.v, v1 = v0;
		UINT j;
		for (j = 0; j < total; j++) {
			const SwVert *vv[3];
			int k;
			vv[0] = &batch[j].a;
			vv[1] = &batch[j].b;
			vv[2] = &batch[j].c;
			for (k = 0; k < 3; k++) {
				x0 = fminf(x0, vv[k]->x); x1 = fmaxf(x1, vv[k]->x);
				y0 = fminf(y0, vv[k]->y); y1 = fmaxf(y1, vv[k]->y);
				u0 = fminf(u0, vv[k]->u); u1 = fmaxf(u1, vv[k]->u);
				v0 = fminf(v0, vv[k]->v); v1 = fmaxf(v1, vv[k]->v);
			}
		}
		fprintf(g_draw_log,
			"draw=%d tris=%u target=%s rt=%dx%d xy=[%.2f,%.2f]-[%.2f,%.2f] "
			"uv=[%.5f,%.5f]-[%.5f,%.5f] tex=%dx%d texel=[%.2f,%.2f]-[%.2f,%.2f] "
			"vs=%d ps=%d bilin=%d mag=%d min=%d blend=%d/%d,%d,%d atest=%d/%d,%d "
			"addr=%d,%d col=%08x\n",
			++g_draw_log_seq, total,
			rt.color == d->rast.color ? "bb" : "off", rt.width, rt.height,
			x0, y0, x1, y1, u0, v0, u1, v1,
			tex ? tex->width : 0, tex ? tex->height : 0,
			tex ? u0 * tex->width : 0.0f, tex ? v0 * tex->height : 0.0f,
			tex ? u1 * tex->width : 0.0f, tex ? v1 * tex->height : 0.0f,
			used_vs, d->ps ? (int)((SwShader *)d->ps)->bytes : 0, rs.bilinear,
			(int)d->samp_mag, (int)d->samp_min,
			rs.blend_enable, rs.src_blend, rs.dst_blend,
			rs.blend_op, rs.alpha_test, rs.alpha_func, rs.alpha_ref,
			rs.addr_u, rs.addr_v, batch[0].a.color);
		/* Draws built from a handful of quads hide their internal seams
		 * behind a single bounding box, so spell the triangles out. */
		if (total <= 16) {
			for (j = 0; j < total; j++) {
				const SwVert *vv[3];
				int k;
				vv[0] = &batch[j].a;
				vv[1] = &batch[j].b;
				vv[2] = &batch[j].c;
				fprintf(g_draw_log, "    tri%u", j);
				for (k = 0; k < 3; k++)
					fprintf(g_draw_log, " (%.3f,%.3f uv %.6f,%.6f%s)",
						vv[k]->x, vv[k]->y, vv[k]->u, vv[k]->v,
						tex ? "" : " notex");
				if (tex)
					fprintf(g_draw_log, "  texel (%.2f,%.2f) (%.2f,%.2f) (%.2f,%.2f)",
						vv[0]->u * tex->width, vv[0]->v * tex->height,
						vv[1]->u * tex->width, vv[1]->v * tex->height,
						vv[2]->u * tex->width, vv[2]->v * tex->height);
				fprintf(g_draw_log, "\n");
			}
		}
	}
	/* Record the last textured draw that lands on the backbuffer. That is the
	 * composite quad, and its mapping plus filter state is what decides how
	 * the offscreen image is resampled on the way to the screen. */
	if (tex && total > 0 && rt.color == d->rast.color) {
		g_bb_draw.valid = 1;
		g_bb_draw.tri_count = (int)total;
		g_bb_draw.tex_w = tex->width;
		g_bb_draw.tex_h = tex->height;
		g_bb_draw.rt_w = rt.width;
		g_bb_draw.rt_h = rt.height;
		g_bb_draw.bilinear = rs.bilinear;
		g_bb_draw.addr_u = rs.addr_u;
		g_bb_draw.addr_v = rs.addr_v;
		g_bb_draw.samp_mag = (int)d->samp_mag;
		g_bb_draw.samp_min = (int)d->samp_min;
		g_bb_draw.x[0] = batch[0].a.x; g_bb_draw.y[0] = batch[0].a.y;
		g_bb_draw.x[1] = batch[0].b.x; g_bb_draw.y[1] = batch[0].b.y;
		g_bb_draw.x[2] = batch[0].c.x; g_bb_draw.y[2] = batch[0].c.y;
		g_bb_draw.u[0] = batch[0].a.u; g_bb_draw.v[0] = batch[0].a.v;
		g_bb_draw.u[1] = batch[0].b.u; g_bb_draw.v[1] = batch[0].b.v;
		g_bb_draw.u[2] = batch[0].c.u; g_bb_draw.v[2] = batch[0].c.v;
	}
	{
		static LONG once;
		if (total > 0 && InterlockedIncrement(&once) == 1) {
			char msg[360];
			_snprintf(msg, sizeof(msg),
				  "tri0 xy=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) uv=(%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f) col=%08x vs=%d clip=%d freq=%08x s1=%d rt=%dx%d tex=%dx%d fmt=%08x blend=%d/%d,%d,%d atest=%d/%d,%d z=%d/%d,%d cull=%d addr=%d,%d",
				  batch[0].a.x, batch[0].a.y, batch[0].b.x, batch[0].b.y, batch[0].c.x,
				  batch[0].c.y, batch[0].a.u, batch[0].a.v, batch[0].b.u, batch[0].b.v,
				  batch[0].c.u, batch[0].c.v, batch[0].a.color, used_vs, vs_clip,
				  d->stream_freq[0], d->vb1 != NULL,
				  rt.width, rt.height, tex ? tex->width : 0, tex ? tex->height : 0,
				  d->tex0 ? (unsigned)((SwTexture *)d->tex0)->fmt : 0,
				  rs.blend_enable, rs.src_blend, rs.dst_blend, rs.blend_op,
				  rs.alpha_test, rs.alpha_func, rs.alpha_ref, rs.z_enable, rs.z_write,
				  rs.z_func, rs.cull, rs.addr_u, rs.addr_v);
			sw_log(msg);
		}
	}
	{
		static LONG small_once;
		if (used_vs && total > 0 && InterlockedIncrement(&small_once) == 1) {
			UINT si;
			for (si = 0; si < total; si++) {
				float minx = fminf(batch[si].a.x, fminf(batch[si].b.x, batch[si].c.x));
				float maxx = fmaxf(batch[si].a.x, fmaxf(batch[si].b.x, batch[si].c.x));
				float miny = fminf(batch[si].a.y, fminf(batch[si].b.y, batch[si].c.y));
				float maxy = fmaxf(batch[si].a.y, fmaxf(batch[si].b.y, batch[si].c.y));
				if ((maxx - minx) < 128.0f && (maxy - miny) < 128.0f &&
				    (maxx - minx) > 2.0f && (maxy - miny) > 2.0f) {
					char msg[360];
					_snprintf(msg, sizeof(msg),
						  "tri_small xy=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) uv=(%.4f,%.4f)(%.4f,%.4f)(%.4f,%.4f) col=%08x tex=%dx%d prims=%u inst=%u",
						  batch[si].a.x, batch[si].a.y, batch[si].b.x,
						  batch[si].b.y, batch[si].c.x, batch[si].c.y,
						  batch[si].a.u, batch[si].a.v, batch[si].b.u,
						  batch[si].b.v, batch[si].c.u, batch[si].c.v,
						  batch[si].a.color, tex ? tex->width : 0,
						  tex ? tex->height : 0, prims, inst_count);
					sw_log(msg);
					break;
				}
			}
		}
	}
	if (tex && total > 0 && tex->width >= 512) {
		static LONG art_once;
		if (InterlockedIncrement(&art_once) == 1) {
			char msg[320];
			SwTexture *stex = d->tex0 ? (SwTexture *)d->tex0 : NULL;
			_snprintf(msg, sizeof(msg),
				  "tri_art xy=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) uv=(%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f) tex=%dx%d fmt=%08x blend=%d",
				  batch[0].a.x, batch[0].a.y, batch[0].b.x, batch[0].b.y, batch[0].c.x,
				  batch[0].c.y, batch[0].a.u, batch[0].a.v, batch[0].b.u, batch[0].b.v,
				  batch[0].c.u, batch[0].c.v, tex->width, tex->height,
				  stex ? (unsigned)stex->fmt : 0, rs.blend_enable);
			sw_log(msg);
		}
	}
	if (d->drew_off && total > 0)
		off_expand_tris(d, batch, total);
	if (total > 0 && d->frame_dips == 0) {
		d->frame_tri_x = batch[0].a.x;
		d->frame_tri_y = batch[0].a.y;
		d->frame_tri_u = batch[0].a.u;
		d->frame_tri_v = batch[0].a.v;
	}
	d->frame_dips++;
	d->frame_prims += total;
	/* After the draw log deliberately, so logged coordinates stay the game's
	 * own and remain comparable against every capture taken before this. */
	if (half_pixel_fix()) {
		for (i = 0; i < total; i++) {
			batch[i].a.x += 0.5f;
			batch[i].a.y += 0.5f;
			batch[i].b.x += 0.5f;
			batch[i].b.y += 0.5f;
			batch[i].c.x += 0.5f;
			batch[i].c.y += 0.5f;
		}
	}
	swrast_triangles(&rt, batch, (int)total, tex, &rs);
	return D3D_OK;
}

static HRESULT WINAPI Dev_DrawPrimitiveUP(IDirect3DDevice9 *this, D3DPRIMITIVETYPE type,
					  UINT prims, const void *data, UINT stride)
{
	SwDevice *d = dev_from(this);
	HRESULT hr;
	UINT idx_count;
	int fs;

	if (!data || prims == 0)
		return D3D_OK;
	fs = fvf_stride(d->fvf);
	if (!stride)
		stride = (UINT)fs;
	if (type != D3DPT_TRIANGLELIST && type != D3DPT_TRIANGLESTRIP &&
	    type != D3DPT_TRIANGLEFAN) {
		log_draw(d, "DrawPrimitiveUP", type, prims, D3DERR_INVALIDCALL);
		return D3D_OK;
	}
	idx_count = (type == D3DPT_TRIANGLELIST) ? prims * 3u : prims + 2u;
	hr = raster_tri_bytes(d, type, prims, (const unsigned char *)data, stride,
			      stride * idx_count, NULL, 0, 0, 0, 0);
	log_draw(d, "DrawPrimitiveUP", type, prims, hr);
	return hr;
}

static HRESULT WINAPI Dev_DrawIndexedPrimitiveUP(IDirect3DDevice9 *this, D3DPRIMITIVETYPE type,
						 UINT min_vert, UINT vert_count, UINT prims,
						 const void *indices, D3DFORMAT ibfmt,
						 const void *data, UINT stride)
{
	SwDevice *d = dev_from(this);
	HRESULT hr;
	int fs;
	UINT ib_elem;
	UINT idx_count;

	(void)min_vert;
	if (!data || !indices || prims == 0)
		return D3D_OK;
	fs = fvf_stride(d->fvf);
	if (!stride)
		stride = (UINT)fs;
	if (type != D3DPT_TRIANGLELIST && type != D3DPT_TRIANGLESTRIP &&
	    type != D3DPT_TRIANGLEFAN) {
		log_draw(d, "DrawIndexedPrimitiveUP", type, prims, D3DERR_INVALIDCALL);
		return D3D_OK;
	}
	if (ibfmt != D3DFMT_INDEX16 && ibfmt != D3DFMT_INDEX32)
		return D3DERR_INVALIDCALL;
	ib_elem = (ibfmt == D3DFMT_INDEX32) ? 4u : 2u;
	idx_count = (type == D3DPT_TRIANGLELIST) ? prims * 3u : prims + 2u;
	hr = raster_tri_bytes(d, type, prims, (const unsigned char *)data, stride,
			      stride * vert_count, (const unsigned char *)indices, ibfmt,
			      ib_elem * idx_count, 0, 0);
	log_draw(d, "DrawIndexedPrimitiveUP", type, prims, hr);
	return hr;
}

static HRESULT WINAPI Dev_DrawPrimitive(IDirect3DDevice9 *this, D3DPRIMITIVETYPE type,
					UINT start_vertex, UINT prims)
{
	SwDevice *d = dev_from(this);
	SwVB *vb;
	UINT stride, off;
	UINT64 start_bytes;
	HRESULT hr;

	if (prims == 0)
		return D3D_OK;
	if (!d->vb0) {
		log_draw(d, "DrawPrimitive", type, prims, D3DERR_INVALIDCALL);
		return D3DERR_INVALIDCALL;
	}
	if (type != D3DPT_TRIANGLELIST && type != D3DPT_TRIANGLESTRIP &&
	    type != D3DPT_TRIANGLEFAN) {
		log_draw(d, "DrawPrimitive", type, prims, D3DERR_INVALIDCALL);
		return D3D_OK;
	}
	vb = (SwVB *)d->vb0;
	stride = d->vb0_stride ? d->vb0_stride : (UINT)fvf_stride(d->fvf);
	off = d->vb0_off;
	start_bytes = (UINT64)off + (UINT64)start_vertex * stride;
	if (!stride || start_bytes >= vb->size)
		return D3DERR_INVALIDCALL;
	hr = raster_tri_bytes(d, type, prims, vb->bytes + (size_t)start_bytes, stride,
			      vb->size - (UINT)start_bytes, NULL, 0, 0, 0, 0);
	log_draw(d, "DrawPrimitive", type, prims, hr);
	return hr;
}

static HRESULT WINAPI Dev_DrawIndexedPrimitive(IDirect3DDevice9 *this, D3DPRIMITIVETYPE type,
					       INT base_vertex, UINT min_vert, UINT num_verts,
					       UINT start_index, UINT prims)
{
	SwDevice *d = dev_from(this);
	SwVB *vb;
	SwIB *ib;
	UINT stride, off;
	HRESULT hr;

	(void)min_vert;
	(void)num_verts;
	if (prims == 0)
		return D3D_OK;
	if (!d->vb0 || !d->ib) {
		log_draw(d, "DrawIndexedPrimitive", type, prims, D3DERR_INVALIDCALL);
		return D3DERR_INVALIDCALL;
	}
	if (type != D3DPT_TRIANGLELIST && type != D3DPT_TRIANGLESTRIP &&
	    type != D3DPT_TRIANGLEFAN) {
		log_draw(d, "DrawIndexedPrimitive", type, prims, D3DERR_INVALIDCALL);
		return D3D_OK;
	}
	vb = (SwVB *)d->vb0;
	ib = (SwIB *)d->ib;
	stride = d->vb0_stride ? d->vb0_stride : (UINT)fvf_stride(d->fvf);
	off = d->vb0_off;
	if (!stride || off >= vb->size)
		return D3DERR_INVALIDCALL;
	hr = raster_tri_bytes(d, type, prims, vb->bytes + off, stride, vb->size - off, ib->bytes,
			      ib->fmt, ib->size, base_vertex, start_index);
	log_draw(d, "DrawIndexedPrimitive", type, prims, hr);
	return hr;
}

static HRESULT WINAPI Dev_SetStreamSource(IDirect3DDevice9 *this, UINT stream,
					  IDirect3DVertexBuffer9 *buf, UINT offset, UINT stride)
{
	SwDevice *d = dev_from(this);
	if (stream > 2)
		return D3D_OK;
	if (stream == 2) {
		if (buf)
			buf->lpVtbl->AddRef(buf);
		if (d->vb2)
			d->vb2->lpVtbl->Release(d->vb2);
		d->vb2 = buf;
		d->vb2_off = offset;
		d->vb2_stride = stride;
		return D3D_OK;
	}
	if (stream == 1) {
		if (buf)
			buf->lpVtbl->AddRef(buf);
		if (d->vb1)
			d->vb1->lpVtbl->Release(d->vb1);
		d->vb1 = buf;
		d->vb1_off = offset;
		d->vb1_stride = stride;
		return D3D_OK;
	}
	if (buf)
		buf->lpVtbl->AddRef(buf);
	if (d->vb0)
		d->vb0->lpVtbl->Release(d->vb0);
	d->vb0 = buf;
	d->vb0_off = offset;
	d->vb0_stride = stride;
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetStreamSource(IDirect3DDevice9 *this, UINT stream,
					  IDirect3DVertexBuffer9 **out, UINT *offset, UINT *stride)
{
	SwDevice *d = dev_from(this);
	if (stream > 2 || !out || !offset || !stride)
		return D3DERR_INVALIDCALL;
	if (stream == 2) {
		*out = d->vb2;
		*offset = d->vb2_off;
		*stride = d->vb2_stride;
	} else if (stream == 1) {
		*out = d->vb1;
		*offset = d->vb1_off;
		*stride = d->vb1_stride;
	} else {
		*out = d->vb0;
		*offset = d->vb0_off;
		*stride = d->vb0_stride;
	}
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetIndices(IDirect3DDevice9 *this, IDirect3DIndexBuffer9 *buf)
{
	SwDevice *d = dev_from(this);
	if (buf)
		buf->lpVtbl->AddRef(buf);
	if (d->ib)
		d->ib->lpVtbl->Release(d->ib);
	d->ib = buf;
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetIndices(IDirect3DDevice9 *this, IDirect3DIndexBuffer9 **out)
{
	SwDevice *d = dev_from(this);
	if (!out)
		return D3DERR_INVALIDCALL;
	*out = d->ib;
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static HRESULT WINAPI Tex_NotImpl(IDirect3DTexture9 *this)
{
	(void)this;
	sw_log("E_NOTIMPL IDirect3DTexture9");
	return E_NOTIMPL;
}

static SwTexture *tex_from(IDirect3DTexture9 *this)
{
	return (SwTexture *)this;
}

static IDirect3DTexture9Vtbl kTexVtbl;

static HRESULT WINAPI Tex_QueryInterface(IDirect3DTexture9 *this, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3DResource9) ||
	    IsEqualGUID(riid, &IID_IDirect3DBaseTexture9) || IsEqualGUID(riid, &IID_IDirect3DTexture9)) {
		*ppv = this;
		this->lpVtbl->AddRef(this);
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI Tex_AddRef(IDirect3DTexture9 *this)
{
	return (ULONG)InterlockedIncrement(&tex_from(this)->ref);
}

static ULONG WINAPI Tex_Release(IDirect3DTexture9 *this)
{
	SwTexture *t = tex_from(this);
	LONG n = InterlockedDecrement(&t->ref);
	if (n == 0) {
		priv_free_all(t->priv);
		if (t->native && !t->native_is_pixels)
			free(t->native);
		free(t->pixels);
		free(t);
	}
	return (ULONG)n;
}

static HRESULT WINAPI Tex_GetDevice(IDirect3DTexture9 *this, IDirect3DDevice9 **out)
{
	SwTexture *t = tex_from(this);
	if (!out)
		return E_POINTER;
	*out = t->dev;
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static DWORD WINAPI Tex_SetPriority(IDirect3DTexture9 *this, DWORD p)
{
	(void)this;
	return p;
}

static DWORD WINAPI Tex_GetPriority(IDirect3DTexture9 *this)
{
	(void)this;
	return 0;
}

static void WINAPI Tex_PreLoad(IDirect3DTexture9 *this)
{
	(void)this;
}

static D3DRESOURCETYPE WINAPI Tex_GetType(IDirect3DTexture9 *this)
{
	(void)this;
	return D3DRTYPE_TEXTURE;
}

static DWORD WINAPI Tex_SetLOD(IDirect3DTexture9 *this, DWORD lod)
{
	(void)this;
	return lod;
}

static DWORD WINAPI Tex_GetLOD(IDirect3DTexture9 *this)
{
	(void)this;
	return 0;
}

static DWORD WINAPI Tex_GetLevelCount(IDirect3DTexture9 *this)
{
	UINT n = tex_from(this)->nlevels;
	return n ? n : 1;
}

static int tex_fmt_dxt(D3DFORMAT f)
{
	return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 ||
	       f == D3DFMT_DXT5;
}

static UINT tex_dxt_block(D3DFORMAT f)
{
	return f == D3DFMT_DXT1 ? 8u : 16u;
}

static UINT tex_fmt_pitch(D3DFORMAT f, UINT w)
{
	if (tex_fmt_dxt(f))
		return ((w + 3u) / 4u) * tex_dxt_block(f);
	switch (f) {
	case D3DFMT_A8:
	case D3DFMT_L8:
	case D3DFMT_P8:
		return w;
	case D3DFMT_A8L8:
	case D3DFMT_R5G6B5:
	case D3DFMT_X1R5G5B5:
	case D3DFMT_A1R5G5B5:
	case D3DFMT_A4R4G4B4:
	case D3DFMT_X4R4G4B4:
	case D3DFMT_V8U8:
	case D3DFMT_L6V5U5:
	case D3DFMT_R16F:
		return w * 2u;
	case D3DFMT_R8G8B8:
		return w * 3u;
	case D3DFMT_A16B16G16R16:
	case D3DFMT_A16B16G16R16F:
	case D3DFMT_G32R32F:
		return w * 8u;
	case D3DFMT_A32B32G32R32F:
		return w * 16u;
	default:
		return w * 4u;
	}
}

static UINT tex_fmt_size(D3DFORMAT f, UINT w, UINT h)
{
	if (tex_fmt_dxt(f))
		return ((w + 3u) / 4u) * ((h + 3u) / 4u) * tex_dxt_block(f);
	return tex_fmt_pitch(f, w) * h;
}

static UINT tex_mip_dim(UINT v, UINT level)
{
	v >>= level;
	return v ? v : 1u;
}

static uint32_t rgb565(unsigned c)
{
	unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
	r = (r * 255 + 15) / 31;
	g = (g * 255 + 31) / 63;
	b = (b * 255 + 15) / 31;
	return 0xff000000u | (r << 16) | (g << 8) | b;
}

static uint32_t decode_rgb16(D3DFORMAT f, unsigned c)
{
	unsigned a, r, g, b;
	switch (f) {
	case D3DFMT_R5G6B5:
		return rgb565(c);
	case D3DFMT_A1R5G5B5:
		a = (c & 0x8000u) ? 255u : 0u;
		r = (c >> 10) & 31u;
		g = (c >> 5) & 31u;
		b = c & 31u;
		r = (r * 255u + 15u) / 31u;
		g = (g * 255u + 15u) / 31u;
		b = (b * 255u + 15u) / 31u;
		return (a << 24) | (r << 16) | (g << 8) | b;
	case D3DFMT_X1R5G5B5:
		r = (c >> 10) & 31u;
		g = (c >> 5) & 31u;
		b = c & 31u;
		r = (r * 255u + 15u) / 31u;
		g = (g * 255u + 15u) / 31u;
		b = (b * 255u + 15u) / 31u;
		return 0xff000000u | (r << 16) | (g << 8) | b;
	case D3DFMT_A4R4G4B4:
		a = ((c >> 12) & 15u) * 17u;
		r = ((c >> 8) & 15u) * 17u;
		g = ((c >> 4) & 15u) * 17u;
		b = (c & 15u) * 17u;
		return (a << 24) | (r << 16) | (g << 8) | b;
	case D3DFMT_X4R4G4B4:
		r = ((c >> 8) & 15u) * 17u;
		g = ((c >> 4) & 15u) * 17u;
		b = (c & 15u) * 17u;
		return 0xff000000u | (r << 16) | (g << 8) | b;
	default:
		return 0xff000000u;
	}
}

static void dxt_colors(unsigned c0, unsigned c1, uint32_t out[4], int dxt1_alpha)
{
	uint32_t a = rgb565(c0), b = rgb565(c1);
	out[0] = a;
	out[1] = b;
	if (!dxt1_alpha || c0 > c1) {
		out[2] = 0xff000000u |
			 (((((a >> 16) & 255) * 2 + ((b >> 16) & 255)) / 3) << 16) |
			 (((((a >> 8) & 255) * 2 + ((b >> 8) & 255)) / 3) << 8) |
			 (((a & 255) * 2 + (b & 255)) / 3);
		out[3] = 0xff000000u |
			 (((((a >> 16) & 255) + ((b >> 16) & 255) * 2) / 3) << 16) |
			 (((((a >> 8) & 255) + ((b >> 8) & 255) * 2) / 3) << 8) |
			 (((a & 255) + (b & 255) * 2) / 3);
	} else {
		out[2] = 0xff000000u |
			 (((((a >> 16) & 255) + ((b >> 16) & 255)) / 2) << 16) |
			 (((((a >> 8) & 255) + ((b >> 8) & 255)) / 2) << 8) |
			 (((a & 255) + (b & 255)) / 2);
		out[3] = 0;
	}
}

static void decode_dxt_color(const unsigned char *src, uint32_t *dst, int dw, int bx, int by,
			     int dxt1)
{
	unsigned c0 = src[0] | ((unsigned)src[1] << 8);
	unsigned c1 = src[2] | ((unsigned)src[3] << 8);
	unsigned idx = src[4] | ((unsigned)src[5] << 8) | ((unsigned)src[6] << 16) |
		       ((unsigned)src[7] << 24);
	uint32_t col[4];
	int x, y;
	dxt_colors(c0, c1, col, dxt1);
	for (y = 0; y < 4; y++) {
		for (x = 0; x < 4; x++) {
			int i = (int)((idx >> (2 * (y * 4 + x))) & 3);
			int px = bx + x, py = by + y;
			if (px < dw)
				dst[py * dw + px] = col[i];
		}
	}
}

static void decode_dxt5_alpha(const unsigned char *src, uint32_t *dst, int dw, int bx, int by)
{
	unsigned a0 = src[0], a1 = src[1];
	unsigned long long bits = 0;
	unsigned a[8];
	int i, x, y;
	a[0] = a0;
	a[1] = a1;
	if (a0 > a1) {
		for (i = 1; i <= 6; i++)
			a[i + 1] = (unsigned)(((7 - i) * a0 + i * a1) / 7);
	} else {
		for (i = 1; i <= 4; i++)
			a[i + 1] = (unsigned)(((5 - i) * a0 + i * a1) / 5);
		a[6] = 0;
		a[7] = 255;
	}
	for (i = 0; i < 6; i++)
		bits |= (unsigned long long)src[2 + i] << (8 * i);
	for (y = 0; y < 4; y++) {
		for (x = 0; x < 4; x++) {
			int sel = (int)((bits >> (3 * (y * 4 + x))) & 7);
			int px = bx + x, py = by + y;
			if (px < dw) {
				uint32_t c = dst[py * dw + px] & 0x00ffffffu;
				dst[py * dw + px] = c | (a[sel] << 24);
			}
		}
	}
}

static void tex_native_to_argb(SwTexture *t)
{
	UINT x, y, w, h;
	if (!t || !t->pixels || t->native_is_pixels)
		return;
	w = (UINT)t->w;
	h = (UINT)t->h;
	if (!t->native) {
		memset(t->pixels, 0, (size_t)w * h * 4u);
		return;
	}
	if (tex_fmt_dxt(t->fmt)) {
		UINT bx, by, bs = tex_dxt_block(t->fmt);
		int dxt1 = t->fmt == D3DFMT_DXT1;
		memset(t->pixels, 0, (size_t)w * h * 4u);
		for (by = 0; by < h; by += 4) {
			for (bx = 0; bx < w; bx += 4) {
				const unsigned char *blk =
					t->native + ((by / 4) * ((w + 3) / 4) + (bx / 4)) * bs;
				if (bs == 16) {
					decode_dxt_color(blk + 8, t->pixels, (int)w, (int)bx, (int)by, 0);
					if (t->fmt == D3DFMT_DXT5 || t->fmt == D3DFMT_DXT4)
						decode_dxt5_alpha(blk, t->pixels, (int)w, (int)bx, (int)by);
				} else
					decode_dxt_color(blk, t->pixels, (int)w, (int)bx, (int)by, dxt1);
			}
		}
		return;
	}
	switch (t->fmt) {
	case D3DFMT_A8:
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++) {
				unsigned a = t->native[y * w + x];
				t->pixels[y * w + x] = (a << 24) | 0x00ffffffu;
			}
		break;
	case D3DFMT_L8:
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++) {
				unsigned l = t->native[y * w + x];
				t->pixels[y * w + x] = 0xff000000u | (l << 16) | (l << 8) | l;
			}
		break;
	case D3DFMT_A8L8:
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++) {
				const unsigned char *p = t->native + (y * w + x) * 2u;
				unsigned l = p[0], a = p[1];
				t->pixels[y * w + x] = (a << 24) | (l << 16) | (l << 8) | l;
			}
		break;
	case D3DFMT_R5G6B5:
	case D3DFMT_A1R5G5B5:
	case D3DFMT_X1R5G5B5:
	case D3DFMT_A4R4G4B4:
	case D3DFMT_X4R4G4B4:
		for (y = 0; y < h; y++) {
			const unsigned char *row = t->native + (size_t)y * (size_t)t->native_pitch;
			for (x = 0; x < w; x++) {
				unsigned c = row[x * 2u] | ((unsigned)row[x * 2u + 1] << 8);
				t->pixels[y * w + x] = decode_rgb16(t->fmt, c);
			}
		}
		break;
	case D3DFMT_A8B8G8R8:
	case D3DFMT_X8B8G8R8:
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++) {
				const unsigned char *p = t->native + (y * w + x) * 4u;
				t->pixels[y * w + x] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
						       ((uint32_t)p[1] << 8) | p[2];
			}
		break;
	default:
		if (t->native_pitch == w * 4u && t->native_size >= w * h * 4u)
			memcpy(t->pixels, t->native, (size_t)w * h * 4u);
		break;
	}
}

static HRESULT WINAPI Tex_SetAutoGenFilterType(IDirect3DTexture9 *this, D3DTEXTUREFILTERTYPE f)
{
	(void)this;
	(void)f;
	return D3D_OK;
}

static D3DTEXTUREFILTERTYPE WINAPI Tex_GetAutoGenFilterType(IDirect3DTexture9 *this)
{
	(void)this;
	return D3DTEXF_LINEAR;
}

static void WINAPI Tex_GenerateMipSubLevels(IDirect3DTexture9 *this)
{
	(void)this;
}

static HRESULT WINAPI Tex_GetLevelDesc(IDirect3DTexture9 *this, UINT level, D3DSURFACE_DESC *desc)
{
	SwTexture *t = tex_from(this);
	UINT n = t->nlevels ? t->nlevels : 1;
	if (level >= n || !desc)
		return D3DERR_INVALIDCALL;
	memset(desc, 0, sizeof(*desc));
	desc->Format = t->fmt ? t->fmt : D3DFMT_A8R8G8B8;
	desc->Type = D3DRTYPE_SURFACE;
	desc->Usage = t->usage;
	desc->Pool = t->pool;
	desc->MultiSampleType = D3DMULTISAMPLE_NONE;
	desc->Width = tex_mip_dim((UINT)t->w, level);
	desc->Height = tex_mip_dim((UINT)t->h, level);
	return D3D_OK;
}

static HRESULT WINAPI Tex_LockRect(IDirect3DTexture9 *this, UINT level, D3DLOCKED_RECT *lr,
				   const RECT *rect, DWORD flags)
{
	SwTexture *t = tex_from(this);
	UINT n = t->nlevels ? t->nlevels : 1;
	swrast_flush_if_pending(t->pixels);
	(void)flags;
	(void)rect;
	if (level >= n || !lr)
		return D3DERR_INVALIDCALL;
	if (level != 0)
		return D3DERR_INVALIDCALL;
	if (t->native) {
		lr->Pitch = (INT)t->native_pitch;
		lr->pBits = t->native;
	} else {
		lr->Pitch = t->w * 4;
		lr->pBits = t->pixels;
	}
	if (rect) {
		unsigned char *bits = (unsigned char *)lr->pBits;
		if (rect->left < 0 || rect->top < 0 || rect->right > t->w || rect->bottom > t->h ||
		    rect->right <= rect->left || rect->bottom <= rect->top)
			return D3DERR_INVALIDCALL;
		if (tex_fmt_dxt(t->fmt)) {
			UINT bs = tex_dxt_block(t->fmt);
			bits += (size_t)(rect->top / 4) * (size_t)lr->Pitch +
				(size_t)(rect->left / 4) * bs;
		} else {
			UINT bpp = t->w ? (UINT)lr->Pitch / (UINT)t->w : 4u;
			if (bpp < 1)
				bpp = 4;
			bits += (size_t)rect->top * (size_t)lr->Pitch + (size_t)rect->left * bpp;
		}
		lr->pBits = bits;
	}
	return D3D_OK;
}

static HRESULT WINAPI Tex_UnlockRect(IDirect3DTexture9 *this, UINT level)
{
	SwTexture *t = tex_from(this);
	(void)level;
	if (t->native && !t->native_is_pixels)
		tex_native_to_argb(t);
	dump_tex_once(t, NULL);
	return D3D_OK;
}

static HRESULT WINAPI Tex_AddDirtyRect(IDirect3DTexture9 *this, const RECT *r)
{
	(void)this;
	(void)r;
	return D3D_OK;
}

static IDirect3DSurface9Vtbl kSurfVtbl;

static SwSurface *surf_from(IDirect3DSurface9 *this)
{
	return (SwSurface *)this;
}

static HRESULT WINAPI Surf_NotImpl(IDirect3DSurface9 *this)
{
	(void)this;
	sw_log_notimpl("IDirect3DSurface9");
	return E_NOTIMPL;
}

static HRESULT WINAPI Surf_QueryInterface(IDirect3DSurface9 *this, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3DResource9) ||
	    IsEqualGUID(riid, &IID_IDirect3DSurface9)) {
		*ppv = this;
		this->lpVtbl->AddRef(this);
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI Surf_AddRef(IDirect3DSurface9 *this)
{
	return (ULONG)InterlockedIncrement(&surf_from(this)->ref);
}

static ULONG WINAPI Surf_Release(IDirect3DSurface9 *this)
{
	SwSurface *s = surf_from(this);
	LONG n = InterlockedDecrement(&s->ref);
	if (n == 0) {
		if (s->tex) {
			if (s->tex->level0 == s)
				s->tex->level0 = NULL;
			s->tex->iface.lpVtbl->Release(&s->tex->iface);
		}
		if (s->own_bits)
			free(s->bits);
		priv_free_all(s->priv);
		free(s);
	}
	return (ULONG)n;
}

static HRESULT WINAPI Surf_GetDevice(IDirect3DSurface9 *this, IDirect3DDevice9 **out)
{
	SwSurface *s = surf_from(this);
	if (!out)
		return E_POINTER;
	*out = s->dev ? s->dev : (s->tex ? s->tex->dev : NULL);
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static HRESULT WINAPI Surf_SetPrivateData(IDirect3DSurface9 *this, REFGUID guid, const void *data,
					  DWORD size, DWORD flags)
{
	return priv_set(&surf_from(this)->priv, guid, data, size, flags);
}

static HRESULT WINAPI Surf_GetPrivateData(IDirect3DSurface9 *this, REFGUID guid, void *data,
					  DWORD *size)
{
	return priv_get(surf_from(this)->priv, guid, data, size);
}

static HRESULT WINAPI Surf_FreePrivateData(IDirect3DSurface9 *this, REFGUID guid)
{
	return priv_free_one(&surf_from(this)->priv, guid);
}

static DWORD WINAPI Surf_SetPriority(IDirect3DSurface9 *this, DWORD p)
{
	(void)this;
	return p;
}

static DWORD WINAPI Surf_GetPriority(IDirect3DSurface9 *this)
{
	(void)this;
	return 0;
}

static void WINAPI Surf_PreLoad(IDirect3DSurface9 *this)
{
	(void)this;
}

static D3DRESOURCETYPE WINAPI Surf_GetType(IDirect3DSurface9 *this)
{
	(void)this;
	return D3DRTYPE_SURFACE;
}

static HRESULT WINAPI Surf_GetContainer(IDirect3DSurface9 *this, REFIID riid, void **ppv)
{
	SwSurface *s = surf_from(this);
	if (s->tex)
		return s->tex->iface.lpVtbl->QueryInterface(&s->tex->iface, riid, ppv);
	if (s->dev)
		return s->dev->lpVtbl->QueryInterface(s->dev, riid, ppv);
	return D3DERR_INVALIDCALL;
}

static HRESULT WINAPI Surf_GetDesc(IDirect3DSurface9 *this, D3DSURFACE_DESC *desc)
{
	SwSurface *s = surf_from(this);
	int w = s->w, h = s->h;
	if (!desc)
		return D3DERR_INVALIDCALL;
	if (s->tex) {
		w = s->tex->w;
		h = s->tex->h;
	} else if (s->dev && (s->usage & D3DUSAGE_RENDERTARGET)) {
		SwDevice *d = (SwDevice *)s->dev;
		w = d->rast.width;
		h = d->rast.height;
	}
	if (w <= 0 || h <= 0)
		return D3DERR_INVALIDCALL;
	memset(desc, 0, sizeof(*desc));
	desc->Format = s->fmt ? s->fmt : D3DFMT_A8R8G8B8;
	desc->Type = D3DRTYPE_SURFACE;
	desc->Usage = s->usage;
	desc->Pool = s->tex ? D3DPOOL_MANAGED : D3DPOOL_DEFAULT;
	desc->MultiSampleType = D3DMULTISAMPLE_NONE;
	desc->Width = (UINT)w;
	desc->Height = (UINT)h;
	return D3D_OK;
}

static HRESULT WINAPI Surf_LockRect(IDirect3DSurface9 *this, D3DLOCKED_RECT *lr, const RECT *rect,
				    DWORD flags)
{
	SwSurface *s = surf_from(this);
	unsigned char *bits = NULL;
	int w, h, pitch;
	swrast_flush_if_pending(s->tex ? (const void *)s->tex->pixels : (const void *)s->bits);
	(void)flags;
	if (!lr)
		return D3DERR_INVALIDCALL;
	if (s->tex) {
		w = s->tex->w;
		h = s->tex->h;
		if (s->tex->native) {
			pitch = (int)s->tex->native_pitch;
			bits = s->tex->native;
		} else {
			pitch = w * 4;
			bits = (unsigned char *)s->tex->pixels;
		}
	} else if (s->dev && (s->usage & D3DUSAGE_RENDERTARGET) && !s->bits) {
		SwDevice *d = (SwDevice *)s->dev;
		w = d->rast.width;
		h = d->rast.height;
		pitch = w * 4;
		bits = (unsigned char *)d->rast.color;
	} else if (s->bits) {
		w = s->w;
		h = s->h;
		pitch = s->pitch ? s->pitch : w * 4;
		bits = (unsigned char *)s->bits;
	} else
		return D3DERR_INVALIDCALL;
	if (rect) {
		int bpp;
		if (rect->left < 0 || rect->top < 0 || rect->right > w || rect->bottom > h ||
		    rect->right <= rect->left || rect->bottom <= rect->top)
			return D3DERR_INVALIDCALL;
		bpp = w ? pitch / w : 4;
		if (bpp < 1)
			bpp = 4;
		bits += (size_t)rect->top * (size_t)pitch + (size_t)rect->left * (size_t)bpp;
	}
	lr->Pitch = pitch;
	lr->pBits = bits;
	return D3D_OK;
}

static HRESULT WINAPI Surf_UnlockRect(IDirect3DSurface9 *this)
{
	SwSurface *s = surf_from(this);
	if (s->tex) {
		tex_native_to_argb(s->tex);
		dump_tex_once(s->tex, NULL);
	}
	return D3D_OK;
}

static void ensure_surf_vtbl(void)
{
	void **slots;
	size_t i, n;
	static int ready;
	if (ready)
		return;
	memset(&kSurfVtbl, 0, sizeof(kSurfVtbl));
	kSurfVtbl.QueryInterface = Surf_QueryInterface;
	kSurfVtbl.AddRef = Surf_AddRef;
	kSurfVtbl.Release = Surf_Release;
	kSurfVtbl.GetDevice = Surf_GetDevice;
	kSurfVtbl.SetPrivateData = Surf_SetPrivateData;
	kSurfVtbl.GetPrivateData = Surf_GetPrivateData;
	kSurfVtbl.FreePrivateData = Surf_FreePrivateData;
	kSurfVtbl.SetPriority = Surf_SetPriority;
	kSurfVtbl.GetPriority = Surf_GetPriority;
	kSurfVtbl.PreLoad = Surf_PreLoad;
	kSurfVtbl.GetType = Surf_GetType;
	kSurfVtbl.GetContainer = Surf_GetContainer;
	kSurfVtbl.GetDesc = Surf_GetDesc;
	kSurfVtbl.LockRect = Surf_LockRect;
	kSurfVtbl.UnlockRect = Surf_UnlockRect;
	slots = (void **)&kSurfVtbl;
	n = sizeof(kSurfVtbl) / sizeof(void *);
	for (i = 0; i < n; i++) {
		if (!slots[i])
			slots[i] = (void *)Surf_NotImpl;
	}
	ready = 1;
}

static HRESULT WINAPI Tex_SetPrivateData(IDirect3DTexture9 *this, REFGUID guid, const void *data,
					 DWORD size, DWORD flags)
{
	return priv_set(&tex_from(this)->priv, guid, data, size, flags);
}

static HRESULT WINAPI Tex_GetPrivateData(IDirect3DTexture9 *this, REFGUID guid, void *data,
					 DWORD *size)
{
	return priv_get(tex_from(this)->priv, guid, data, size);
}

static HRESULT WINAPI Tex_FreePrivateData(IDirect3DTexture9 *this, REFGUID guid)
{
	return priv_free_one(&tex_from(this)->priv, guid);
}

static HRESULT WINAPI Tex_GetSurfaceLevel(IDirect3DTexture9 *this, UINT level,
					  IDirect3DSurface9 **out)
{
	SwTexture *t = tex_from(this);
	SwSurface *s;
	if (!out || level >= (t->nlevels ? t->nlevels : 1u))
		return D3DERR_INVALIDCALL;
	if (level != 0) {
		UINT mw = tex_mip_dim((UINT)t->w, level);
		UINT mh = tex_mip_dim((UINT)t->h, level);
		ensure_surf_vtbl();
		s = (SwSurface *)calloc(1, sizeof(*s));
		if (!s)
			return E_OUTOFMEMORY;
		s->iface.lpVtbl = &kSurfVtbl;
		s->ref = 1;
		s->dev = t->dev;
		s->w = (int)mw;
		s->h = (int)mh;
		s->fmt = t->fmt ? t->fmt : D3DFMT_A8R8G8B8;
		s->usage = t->usage;
		s->mip = (int)level;
		s->pitch = (int)tex_fmt_pitch(s->fmt, mw);
		s->own_bits = 1;
		s->bits = calloc(tex_fmt_size(s->fmt, mw, mh), 1);
		if (!s->bits) {
			free(s);
			return E_OUTOFMEMORY;
		}
		*out = &s->iface;
		return D3D_OK;
	}
	if (!t->level0) {
		ensure_surf_vtbl();
		s = (SwSurface *)calloc(1, sizeof(*s));
		if (!s)
			return E_OUTOFMEMORY;
		s->iface.lpVtbl = &kSurfVtbl;
		s->ref = 0;
		s->tex = t;
		s->dev = t->dev;
		s->w = t->w;
		s->h = t->h;
		s->fmt = t->fmt ? t->fmt : D3DFMT_A8R8G8B8;
		s->usage = t->usage;
		s->mip = 0;
		t->iface.lpVtbl->AddRef(&t->iface);
		t->level0 = s;
	}
	t->level0->iface.lpVtbl->AddRef(&t->level0->iface);
	*out = &t->level0->iface;
	return D3D_OK;
}

static void ensure_tex_vtbl(void)
{
	void **slots;
	size_t i, n;
	static int ready;
	if (ready)
		return;
	memset(&kTexVtbl, 0, sizeof(kTexVtbl));
	kTexVtbl.QueryInterface = Tex_QueryInterface;
	kTexVtbl.AddRef = Tex_AddRef;
	kTexVtbl.Release = Tex_Release;
	kTexVtbl.GetDevice = Tex_GetDevice;
	kTexVtbl.SetPrivateData = Tex_SetPrivateData;
	kTexVtbl.GetPrivateData = Tex_GetPrivateData;
	kTexVtbl.FreePrivateData = Tex_FreePrivateData;
	kTexVtbl.SetPriority = Tex_SetPriority;
	kTexVtbl.GetPriority = Tex_GetPriority;
	kTexVtbl.PreLoad = Tex_PreLoad;
	kTexVtbl.GetType = Tex_GetType;
	kTexVtbl.SetLOD = Tex_SetLOD;
	kTexVtbl.GetLOD = Tex_GetLOD;
	kTexVtbl.GetLevelCount = Tex_GetLevelCount;
	kTexVtbl.SetAutoGenFilterType = Tex_SetAutoGenFilterType;
	kTexVtbl.GetAutoGenFilterType = Tex_GetAutoGenFilterType;
	kTexVtbl.GenerateMipSubLevels = Tex_GenerateMipSubLevels;
	kTexVtbl.GetLevelDesc = Tex_GetLevelDesc;
	kTexVtbl.GetSurfaceLevel = Tex_GetSurfaceLevel;
	kTexVtbl.LockRect = Tex_LockRect;
	kTexVtbl.UnlockRect = Tex_UnlockRect;
	kTexVtbl.AddDirtyRect = Tex_AddDirtyRect;
	slots = (void **)&kTexVtbl;
	n = sizeof(kTexVtbl) / sizeof(void *);
	for (i = 0; i < n; i++) {
		if (!slots[i])
			slots[i] = (void *)Tex_NotImpl;
	}
	ready = 1;
}

static IDirect3DVertexBuffer9Vtbl kVBVtbl;
static IDirect3DIndexBuffer9Vtbl kIBVtbl;

static HRESULT WINAPI Buf_NotImpl(void *this)
{
	(void)this;
	sw_log("E_NOTIMPL buffer");
	return E_NOTIMPL;
}

static SwVB *vb_from(IDirect3DVertexBuffer9 *this)
{
	return (SwVB *)this;
}

static SwIB *ib_from(IDirect3DIndexBuffer9 *this)
{
	return (SwIB *)this;
}

static HRESULT WINAPI VB_QueryInterface(IDirect3DVertexBuffer9 *this, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3DResource9) ||
	    IsEqualGUID(riid, &IID_IDirect3DVertexBuffer9)) {
		*ppv = this;
		this->lpVtbl->AddRef(this);
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI VB_AddRef(IDirect3DVertexBuffer9 *this)
{
	return (ULONG)InterlockedIncrement(&vb_from(this)->ref);
}

static ULONG WINAPI VB_Release(IDirect3DVertexBuffer9 *this)
{
	SwVB *b = vb_from(this);
	LONG n = InterlockedDecrement(&b->ref);
	if (n == 0) {
		free(b->bytes);
		free(b);
	}
	return (ULONG)n;
}

static HRESULT WINAPI VB_GetDevice(IDirect3DVertexBuffer9 *this, IDirect3DDevice9 **out)
{
	SwVB *b = vb_from(this);
	if (!out)
		return E_POINTER;
	*out = b->dev;
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static DWORD WINAPI VB_SetPriority(IDirect3DVertexBuffer9 *this, DWORD p)
{
	(void)this;
	return p;
}

static DWORD WINAPI VB_GetPriority(IDirect3DVertexBuffer9 *this)
{
	(void)this;
	return 0;
}

static void WINAPI VB_PreLoad(IDirect3DVertexBuffer9 *this)
{
	(void)this;
}

static D3DRESOURCETYPE WINAPI VB_GetType(IDirect3DVertexBuffer9 *this)
{
	(void)this;
	return D3DRTYPE_VERTEXBUFFER;
}

static HRESULT WINAPI VB_Lock(IDirect3DVertexBuffer9 *this, UINT off, UINT size, void **out, DWORD flags)
{
	SwVB *b = vb_from(this);
	sw_trace("VB.Lock off=%u size=%u flags=%08x", off, size, flags);
	(void)flags;
	if (!out)
		return D3DERR_INVALIDCALL;
	if (off > b->size)
		return D3DERR_INVALIDCALL;
	if (!size)
		size = b->size - off;
	if ((UINT64)off + size > b->size)
		return D3DERR_INVALIDCALL;
	*out = b->bytes + off;
	return D3D_OK;
}

static HRESULT WINAPI VB_Unlock(IDirect3DVertexBuffer9 *this)
{
	(void)this;
	return D3D_OK;
}

static HRESULT WINAPI VB_GetDesc(IDirect3DVertexBuffer9 *this, D3DVERTEXBUFFER_DESC *desc)
{
	SwVB *b = vb_from(this);
	if (!desc)
		return D3DERR_INVALIDCALL;
	memset(desc, 0, sizeof(*desc));
	desc->Format = D3DFMT_VERTEXDATA;
	desc->Type = D3DRTYPE_VERTEXBUFFER;
	desc->Usage = b->usage;
	desc->Pool = b->pool;
	desc->Size = b->size;
	desc->FVF = b->fvf;
	return D3D_OK;
}

static void ensure_vb_vtbl(void)
{
	void **slots;
	size_t i, n;
	static int ready;
	if (ready)
		return;
	memset(&kVBVtbl, 0, sizeof(kVBVtbl));
	kVBVtbl.QueryInterface = VB_QueryInterface;
	kVBVtbl.AddRef = VB_AddRef;
	kVBVtbl.Release = VB_Release;
	kVBVtbl.GetDevice = VB_GetDevice;
	kVBVtbl.SetPriority = VB_SetPriority;
	kVBVtbl.GetPriority = VB_GetPriority;
	kVBVtbl.PreLoad = VB_PreLoad;
	kVBVtbl.GetType = VB_GetType;
	kVBVtbl.Lock = VB_Lock;
	kVBVtbl.Unlock = VB_Unlock;
	kVBVtbl.GetDesc = VB_GetDesc;
	slots = (void **)&kVBVtbl;
	n = sizeof(kVBVtbl) / sizeof(void *);
	for (i = 0; i < n; i++) {
		if (!slots[i])
			slots[i] = (void *)Buf_NotImpl;
	}
	ready = 1;
}

static HRESULT WINAPI IB_QueryInterface(IDirect3DIndexBuffer9 *this, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3DResource9) ||
	    IsEqualGUID(riid, &IID_IDirect3DIndexBuffer9)) {
		*ppv = this;
		this->lpVtbl->AddRef(this);
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI IB_AddRef(IDirect3DIndexBuffer9 *this)
{
	return (ULONG)InterlockedIncrement(&ib_from(this)->ref);
}

static ULONG WINAPI IB_Release(IDirect3DIndexBuffer9 *this)
{
	SwIB *b = ib_from(this);
	LONG n = InterlockedDecrement(&b->ref);
	if (n == 0) {
		free(b->bytes);
		free(b);
	}
	return (ULONG)n;
}

static HRESULT WINAPI IB_GetDevice(IDirect3DIndexBuffer9 *this, IDirect3DDevice9 **out)
{
	SwIB *b = ib_from(this);
	if (!out)
		return E_POINTER;
	*out = b->dev;
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static DWORD WINAPI IB_SetPriority(IDirect3DIndexBuffer9 *this, DWORD p)
{
	(void)this;
	return p;
}

static DWORD WINAPI IB_GetPriority(IDirect3DIndexBuffer9 *this)
{
	(void)this;
	return 0;
}

static void WINAPI IB_PreLoad(IDirect3DIndexBuffer9 *this)
{
	(void)this;
}

static D3DRESOURCETYPE WINAPI IB_GetType(IDirect3DIndexBuffer9 *this)
{
	(void)this;
	return D3DRTYPE_INDEXBUFFER;
}

static HRESULT WINAPI IB_Lock(IDirect3DIndexBuffer9 *this, UINT off, UINT size, void **out, DWORD flags)
{
	SwIB *b = ib_from(this);
	sw_trace("IB.Lock off=%u size=%u flags=%08x", off, size, flags);
	(void)flags;
	if (!out)
		return D3DERR_INVALIDCALL;
	if (off > b->size)
		return D3DERR_INVALIDCALL;
	if (!size)
		size = b->size - off;
	if ((UINT64)off + size > b->size)
		return D3DERR_INVALIDCALL;
	*out = b->bytes + off;
	return D3D_OK;
}

static HRESULT WINAPI IB_Unlock(IDirect3DIndexBuffer9 *this)
{
	(void)this;
	return D3D_OK;
}

static HRESULT WINAPI IB_GetDesc(IDirect3DIndexBuffer9 *this, D3DINDEXBUFFER_DESC *desc)
{
	SwIB *b = ib_from(this);
	if (!desc)
		return D3DERR_INVALIDCALL;
	memset(desc, 0, sizeof(*desc));
	desc->Format = b->fmt;
	desc->Type = D3DRTYPE_INDEXBUFFER;
	desc->Usage = b->usage;
	desc->Pool = b->pool;
	desc->Size = b->size;
	return D3D_OK;
}

static void ensure_ib_vtbl(void)
{
	void **slots;
	size_t i, n;
	static int ready;
	if (ready)
		return;
	memset(&kIBVtbl, 0, sizeof(kIBVtbl));
	kIBVtbl.QueryInterface = IB_QueryInterface;
	kIBVtbl.AddRef = IB_AddRef;
	kIBVtbl.Release = IB_Release;
	kIBVtbl.GetDevice = IB_GetDevice;
	kIBVtbl.SetPriority = IB_SetPriority;
	kIBVtbl.GetPriority = IB_GetPriority;
	kIBVtbl.PreLoad = IB_PreLoad;
	kIBVtbl.GetType = IB_GetType;
	kIBVtbl.Lock = IB_Lock;
	kIBVtbl.Unlock = IB_Unlock;
	kIBVtbl.GetDesc = IB_GetDesc;
	slots = (void **)&kIBVtbl;
	n = sizeof(kIBVtbl) / sizeof(void *);
	for (i = 0; i < n; i++) {
		if (!slots[i])
			slots[i] = (void *)Buf_NotImpl;
	}
	ready = 1;
}

static HRESULT WINAPI Dev_CreateTexture(IDirect3DDevice9 *this, UINT width, UINT height, UINT levels,
					DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
					IDirect3DTexture9 **out, HANDLE *shared)
{
	SwTexture *t;
	static LONG ncreate;
	LONG nth;
	(void)shared;
	if (!out)
		return D3DERR_INVALIDCALL;
	*out = NULL;
	if (!width || !height)
		return D3DERR_INVALIDCALL;
	if (fmt == D3DFMT_UNKNOWN)
		fmt = D3DFMT_A8R8G8B8;
	ensure_tex_vtbl();
	t = (SwTexture *)calloc(1, sizeof(*t));
	if (!t)
		return E_OUTOFMEMORY;
	t->iface.lpVtbl = &kTexVtbl;
	t->ref = 1;
	t->dev = this;
	t->w = (int)width;
	t->h = (int)height;
	t->fmt = fmt;
	t->usage = usage;
	t->pool = pool;
	t->nlevels = levels ? (levels > 16 ? 16 : levels) : 1u;
	t->pixels = (uint32_t *)calloc((size_t)width * (size_t)height, sizeof(uint32_t));
	if (!t->pixels) {
		free(t);
		return E_OUTOFMEMORY;
	}
	t->native_pitch = tex_fmt_pitch(fmt, width);
	t->native_size = tex_fmt_size(fmt, width, height);
	if (fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_X8R8G8B8) {
		t->native = (unsigned char *)t->pixels;
		t->native_is_pixels = 1;
		t->native_pitch = width * 4u;
		t->native_size = width * height * 4u;
	} else {
		t->native = (unsigned char *)calloc(1, t->native_size ? t->native_size : 4);
		if (!t->native) {
			free(t->pixels);
			free(t);
			return E_OUTOFMEMORY;
		}
	}
	nth = InterlockedIncrement(&ncreate);
	if (nth <= 48 || (nth & 1023) == 0) {
		char msg[160];
		_snprintf(msg, sizeof(msg),
			  "CreateTexture #%ld %ux%u lv=%u/%u usage=%08lx fmt=%08x pool=%u", nth, width,
			  height, levels, t->nlevels, (unsigned long)usage, (unsigned)fmt,
			  (unsigned)pool);
		sw_trace("%s", msg);
	}
	*out = &t->iface;
	return D3D_OK;
}

static HRESULT WINAPI Dev_UpdateTexture(IDirect3DDevice9 *this, IDirect3DBaseTexture9 *src,
					IDirect3DBaseTexture9 *dst)
{
	swrast_flush();
	SwTexture *s = (SwTexture *)src;
	SwTexture *d = (SwTexture *)dst;
	int w, h, y;
	(void)this;
	if (!src || !dst)
		return D3DERR_INVALIDCALL;
	if (!s->pixels || !d->pixels)
		return D3D_OK;
	w = s->w < d->w ? s->w : d->w;
	h = s->h < d->h ? s->h : d->h;
	if (w <= 0 || h <= 0)
		return D3D_OK;
	for (y = 0; y < h; y++)
		memcpy(d->pixels + (size_t)y * (size_t)d->w, s->pixels + (size_t)y * (size_t)s->w,
		       (size_t)w * 4u);
	return D3D_OK;
}

static HRESULT WINAPI Dev_CreateVertexBuffer(IDirect3DDevice9 *this, UINT length, DWORD usage,
					     DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **out,
					     HANDLE *shared)
{
	SwVB *b;
	(void)shared;
	if (!out || !length)
		return D3DERR_INVALIDCALL;
	ensure_vb_vtbl();
	b = (SwVB *)calloc(1, sizeof(*b));
	if (!b)
		return E_OUTOFMEMORY;
	b->bytes = (unsigned char *)calloc(1, length);
	if (!b->bytes) {
		free(b);
		return E_OUTOFMEMORY;
	}
	b->iface.lpVtbl = &kVBVtbl;
	b->ref = 1;
	b->dev = this;
	b->size = length;
	b->usage = usage;
	b->fvf = fvf;
	b->pool = pool;
	*out = &b->iface;
	return D3D_OK;
}

static HRESULT WINAPI Dev_CreateIndexBuffer(IDirect3DDevice9 *this, UINT length, DWORD usage,
					    D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9 **out,
					    HANDLE *shared)
{
	SwIB *b;
	(void)shared;
	if (!out || !length)
		return D3DERR_INVALIDCALL;
	if (fmt != D3DFMT_INDEX16 && fmt != D3DFMT_INDEX32)
		return D3DERR_INVALIDCALL;
	ensure_ib_vtbl();
	b = (SwIB *)calloc(1, sizeof(*b));
	if (!b)
		return E_OUTOFMEMORY;
	b->bytes = (unsigned char *)calloc(1, length);
	if (!b->bytes) {
		free(b);
		return E_OUTOFMEMORY;
	}
	b->iface.lpVtbl = &kIBVtbl;
	b->ref = 1;
	b->dev = this;
	b->size = length;
	b->usage = usage;
	b->fmt = fmt;
	b->pool = pool;
	*out = &b->iface;
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetTexture(IDirect3DDevice9 *this, DWORD stage, IDirect3DBaseTexture9 *tex)
{
	SwDevice *d = dev_from(this);
	if (stage != 0)
		return D3D_OK;
	/* Binding a render target as a source is the only case that needs the
	 * queued draws resolved first. */
	if (tex)
		swrast_flush_if_pending(((SwTexture *)tex)->pixels);
	if (tex)
		tex->lpVtbl->AddRef(tex);
	if (d->tex0)
		d->tex0->lpVtbl->Release(d->tex0);
	d->tex0 = tex;
	if (tex)
		dump_tex_once((SwTexture *)tex, "d3d9_sw_tex256.tga");
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetTexture(IDirect3DDevice9 *this, DWORD stage, IDirect3DBaseTexture9 **out)
{
	SwDevice *d = dev_from(this);
	if (stage != 0 || !out)
		return D3DERR_INVALIDCALL;
	*out = d->tex0;
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetSamplerState(IDirect3DDevice9 *this, DWORD sampler,
					  D3DSAMPLERSTATETYPE type, DWORD value)
{
	SwDevice *d = dev_from(this);
	/* Only sampler 0 drives rasterisation today, but record the rest during a
	 * capture: a game that filters through a different sampler would
	 * otherwise look like it had asked for point sampling. */
	if (g_draw_log && sampler < 16)
		fprintf(g_draw_log, "  samplerstate s%u type=%u value=%u\n",
			(unsigned)sampler, (unsigned)type, (unsigned)value);
	if (sampler != 0)
		return D3D_OK;
	if (type == D3DSAMP_MINFILTER)
		d->samp_min = value;
	else if (type == D3DSAMP_MAGFILTER)
		d->samp_mag = value;
	else if (type == D3DSAMP_ADDRESSU)
		d->samp_addru = value;
	else if (type == D3DSAMP_ADDRESSV)
		d->samp_addrv = value;
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetSamplerState(IDirect3DDevice9 *this, DWORD sampler,
					  D3DSAMPLERSTATETYPE type, DWORD *value)
{
	SwDevice *d = dev_from(this);
	if (sampler != 0 || !value)
		return D3DERR_INVALIDCALL;
	if (type == D3DSAMP_MINFILTER)
		*value = d->samp_min;
	else if (type == D3DSAMP_MAGFILTER)
		*value = d->samp_mag;
	else if (type == D3DSAMP_ADDRESSU)
		*value = d->samp_addru;
	else if (type == D3DSAMP_ADDRESSV)
		*value = d->samp_addrv;
	else
		*value = 0;
	return D3D_OK;
}

static SwSurface *make_plain_surface(IDirect3DDevice9 *dev, int w, int h, D3DFORMAT fmt,
				     DWORD usage, int alloc_bits)
{
	SwSurface *s;
	ensure_surf_vtbl();
	s = (SwSurface *)calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->iface.lpVtbl = &kSurfVtbl;
	s->ref = 1;
	s->dev = dev;
	s->w = w;
	s->h = h;
	s->fmt = fmt;
	s->usage = usage;
	s->pitch = w * 4;
	if (alloc_bits) {
		s->bits = calloc((size_t)w * (size_t)h, 4);
		if (!s->bits) {
			free(s);
			return NULL;
		}
		s->own_bits = 1;
	}
	return s;
}

static HRESULT ensure_backbuf(SwDevice *d)
{
	if (d->backbuf)
		return D3D_OK;
	{
		SwSurface *s = make_plain_surface(&d->iface, d->rast.width, d->rast.height,
						  D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, 0);
		if (!s)
			return E_OUTOFMEMORY;
		d->backbuf = &s->iface;
	}
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetBackBuffer(IDirect3DDevice9 *this, UINT swapchain, UINT bb,
					D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **out)
{
	SwDevice *d = dev_from(this);
	(void)type;
	if (swapchain != 0 || bb != 0 || !out)
		return D3DERR_INVALIDCALL;
	{
		HRESULT hr = ensure_backbuf(d);
		if (FAILED(hr))
			return hr;
	}
	d->backbuf->lpVtbl->AddRef(d->backbuf);
	*out = d->backbuf;
	return D3D_OK;
}

static HRESULT WINAPI Dev_CreateDepthStencilSurface(IDirect3DDevice9 *this, UINT w, UINT h,
						    D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
						    DWORD quality, WINBOOL discard,
						    IDirect3DSurface9 **out, HANDLE *shared)
{
	SwSurface *s;
	(void)ms;
	(void)quality;
	(void)discard;
	(void)shared;
	if (!out || !w || !h)
		return D3DERR_INVALIDCALL;
	if (fmt != D3DFMT_D24S8 && fmt != D3DFMT_D16 && fmt != D3DFMT_D24X8 &&
	    fmt != D3DFMT_D32 && fmt != D3DFMT_D32F_LOCKABLE && fmt != D3DFMT_D24FS8)
		fmt = D3DFMT_D24S8;
	s = make_plain_surface(this, (int)w, (int)h, fmt, D3DUSAGE_DEPTHSTENCIL, 1);
	if (!s)
		return E_OUTOFMEMORY;
	*out = &s->iface;
	return D3D_OK;
}

static HRESULT WINAPI Dev_CreateRenderTarget(IDirect3DDevice9 *this, UINT w, UINT h,
					     D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD quality,
					     WINBOOL lockable, IDirect3DSurface9 **out,
					     HANDLE *shared)
{
	SwSurface *s;
	(void)ms;
	(void)quality;
	(void)lockable;
	(void)shared;
	if (!out || !w || !h)
		return D3DERR_INVALIDCALL;
	if (fmt != D3DFMT_A8R8G8B8 && fmt != D3DFMT_X8R8G8B8 && fmt != D3DFMT_A8B8G8R8 &&
	    fmt != D3DFMT_X8B8G8R8)
		fmt = D3DFMT_A8R8G8B8;
	s = make_plain_surface(this, (int)w, (int)h, fmt, D3DUSAGE_RENDERTARGET, 1);
	if (!s)
		return E_OUTOFMEMORY;
	*out = &s->iface;
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetDepthStencilSurface(IDirect3DDevice9 *this, IDirect3DSurface9 *surf)
{
	com_replace((IUnknown **)&dev_from(this)->ds, (IUnknown *)surf);
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetDepthStencilSurface(IDirect3DDevice9 *this, IDirect3DSurface9 **out)
{
	SwDevice *d = dev_from(this);
	if (!out)
		return D3DERR_INVALIDCALL;
	*out = d->ds;
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetRenderTarget(IDirect3DDevice9 *this, DWORD index,
					  IDirect3DSurface9 *surf)
{
	SwDevice *d = dev_from(this);
	if (index != 0)
		return D3D_OK;
	if (!surf) {
		HRESULT hr = ensure_backbuf(d);
		if (FAILED(hr))
			return hr;
		surf = d->backbuf;
	}
	com_replace((IUnknown **)&d->rt0, (IUnknown *)surf);
	{
		SwSurface *s = (SwSurface *)surf;
		int w = s->tex ? s->tex->w : s->w;
		int h = s->tex ? s->tex->h : s->h;
		if (w > 0 && h > 0) {
			d->viewport.X = 0;
			d->viewport.Y = 0;
			d->viewport.Width = (DWORD)w;
			d->viewport.Height = (DWORD)h;
			d->scissor.left = 0;
			d->scissor.top = 0;
			d->scissor.right = w;
			d->scissor.bottom = h;
		}
	}
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetRenderTarget(IDirect3DDevice9 *this, DWORD index,
					  IDirect3DSurface9 **out)
{
	SwDevice *d = dev_from(this);
	HRESULT hr;
	if (index != 0 || !out)
		return D3DERR_INVALIDCALL;
	hr = ensure_backbuf(d);
	if (FAILED(hr))
		return hr;
	*out = d->rt0 ? d->rt0 : d->backbuf;
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetStreamSourceFreq(IDirect3DDevice9 *this, UINT stream, UINT divider)
{
	SwDevice *d = dev_from(this);
	if (stream > 2)
		return D3D_OK;
	d->stream_freq[stream] = divider;
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetStreamSourceFreq(IDirect3DDevice9 *this, UINT stream, UINT *divider)
{
	SwDevice *d = dev_from(this);
	if (stream > 2 || !divider)
		return D3DERR_INVALIDCALL;
	*divider = d->stream_freq[stream] ? d->stream_freq[stream] : 1;
	return D3D_OK;
}

static int surf_pixels(SwSurface *s, uint32_t **pixels, int *w, int *h, int *pitch)
{
	if (!s || !pixels || !w || !h || !pitch)
		return 0;
	if (s->tex && s->tex->pixels) {
		*pixels = s->tex->pixels;
		*w = s->tex->w;
		*h = s->tex->h;
		*pitch = s->tex->w;
		return 1;
	}
	if (s->dev && (s->usage & D3DUSAGE_RENDERTARGET) && !s->own_bits) {
		SwDevice *d = (SwDevice *)s->dev;
		*pixels = d->rast.color;
		*w = d->rast.width;
		*h = d->rast.height;
		*pitch = d->rast.width;
		return *pixels != NULL;
	}
	if (s->bits) {
		*pixels = (uint32_t *)s->bits;
		*w = s->w;
		*h = s->h;
		*pitch = s->pitch ? s->pitch / 4 : s->w;
		return 1;
	}
	return 0;
}

static HRESULT WINAPI Dev_StretchRect(IDirect3DDevice9 *this, IDirect3DSurface9 *src_s,
				      const RECT *src_r, IDirect3DSurface9 *dst_s,
				      const RECT *dst_r, D3DTEXTUREFILTERTYPE filter)
{
	swrast_flush();
	SwSurface *src, *dst;
	uint32_t *sp = NULL, *dp = NULL;
	int sw, sh, spitch, dw, dh, dpitch;
	RECT sr, dr;
	int y, x;
	(void)this;
	(void)filter;
	if (!src_s || !dst_s)
		return D3DERR_INVALIDCALL;
	src = (SwSurface *)src_s;
	dst = (SwSurface *)dst_s;
	if (!surf_pixels(src, &sp, &sw, &sh, &spitch) || !surf_pixels(dst, &dp, &dw, &dh, &dpitch)) {
		static LONG once;
		if (InterlockedIncrement(&once) == 1)
			sw_log("StretchRect no pixels");
		return D3DERR_INVALIDCALL;
	}
	sr.left = 0;
	sr.top = 0;
	sr.right = sw;
	sr.bottom = sh;
	dr.left = 0;
	dr.top = 0;
	dr.right = dw;
	dr.bottom = dh;
	if (src_r)
		sr = *src_r;
	if (dst_r)
		dr = *dst_r;
	if (sr.right <= sr.left || sr.bottom <= sr.top || dr.right <= dr.left ||
	    dr.bottom <= dr.top)
		return D3DERR_INVALIDCALL;
	{
		int src_w = sr.right - sr.left;
		int src_h = sr.bottom - sr.top;
		int dst_w = dr.right - dr.left;
		int dst_h = dr.bottom - dr.top;
		for (y = 0; y < dst_h; y++) {
			int sy = sr.top + y * src_h / dst_h;
			if (sy < 0)
				sy = 0;
			if (sy >= sh)
				sy = sh - 1;
			for (x = 0; x < dst_w; x++) {
				int sx = sr.left + x * src_w / dst_w;
				int dx = dr.left + x;
				int dy = dr.top + y;
				if (sx < 0)
					sx = 0;
				if (sx >= sw)
					sx = sw - 1;
				if (dx < 0 || dy < 0 || dx >= dw || dy >= dh)
					continue;
				dp[dy * dpitch + dx] = sp[sy * spitch + sx];
			}
		}
	}
	{
		static LONG once;
		if (InterlockedIncrement(&once) == 1)
			sw_log("StretchRect first ok");
	}
	return D3D_OK;
}

static IDirect3DStateBlock9Vtbl kSBVtbl;

typedef struct SwSnap {
	IDirect3DSurface9 *rt0;
	IDirect3DSurface9 *ds;
	IDirect3DBaseTexture9 *tex0;
	IDirect3DVertexBuffer9 *vb0;
	UINT vb0_off;
	UINT vb0_stride;
	IDirect3DVertexBuffer9 *vb1;
	UINT vb1_off;
	UINT vb1_stride;
	IDirect3DVertexBuffer9 *vb2;
	UINT vb2_off;
	UINT vb2_stride;
	IDirect3DIndexBuffer9 *ib;
	IDirect3DVertexDeclaration9 *decl;
	IDirect3DVertexShader9 *vs;
	IDirect3DPixelShader9 *ps;
	D3DVIEWPORT9 viewport;
	DWORD fvf;
	DWORD rs[256];
	DWORD samp_min;
	DWORD samp_mag;
	DWORD samp_addru;
	DWORD samp_addrv;
	UINT stream_freq[3];
	D3DMATRIX world, view, proj;
	RECT scissor;
	int valid;
} SwSnap;

typedef struct SwSB {
	IDirect3DStateBlock9 iface;
	LONG ref;
	IDirect3DDevice9 *dev;
	SwSnap snap;
} SwSB;

static void snap_release(SwSnap *s)
{
	if (!s)
		return;
	if (s->rt0)
		s->rt0->lpVtbl->Release(s->rt0);
	if (s->ds)
		s->ds->lpVtbl->Release(s->ds);
	if (s->tex0)
		s->tex0->lpVtbl->Release(s->tex0);
	if (s->vb0)
		s->vb0->lpVtbl->Release(s->vb0);
	if (s->vb1)
		s->vb1->lpVtbl->Release(s->vb1);
	if (s->vb2)
		s->vb2->lpVtbl->Release(s->vb2);
	if (s->ib)
		s->ib->lpVtbl->Release(s->ib);
	if (s->decl)
		s->decl->lpVtbl->Release(s->decl);
	if (s->vs)
		s->vs->lpVtbl->Release(s->vs);
	if (s->ps)
		s->ps->lpVtbl->Release(s->ps);
	memset(s, 0, sizeof(*s));
}

static void snap_from_dev(SwSnap *s, SwDevice *d)
{
	snap_release(s);
	s->rt0 = d->rt0;
	if (s->rt0)
		s->rt0->lpVtbl->AddRef(s->rt0);
	s->ds = d->ds;
	if (s->ds)
		s->ds->lpVtbl->AddRef(s->ds);
	s->tex0 = d->tex0;
	if (s->tex0)
		s->tex0->lpVtbl->AddRef(s->tex0);
	s->vb0 = d->vb0;
	if (s->vb0)
		s->vb0->lpVtbl->AddRef(s->vb0);
	s->vb0_off = d->vb0_off;
	s->vb0_stride = d->vb0_stride;
	s->vb1 = d->vb1;
	if (s->vb1)
		s->vb1->lpVtbl->AddRef(s->vb1);
	s->vb1_off = d->vb1_off;
	s->vb1_stride = d->vb1_stride;
	s->vb2 = d->vb2;
	if (s->vb2)
		s->vb2->lpVtbl->AddRef(s->vb2);
	s->vb2_off = d->vb2_off;
	s->vb2_stride = d->vb2_stride;
	s->ib = d->ib;
	if (s->ib)
		s->ib->lpVtbl->AddRef(s->ib);
	s->decl = d->decl;
	if (s->decl)
		s->decl->lpVtbl->AddRef(s->decl);
	s->vs = d->vs;
	if (s->vs)
		s->vs->lpVtbl->AddRef(s->vs);
	s->ps = d->ps;
	if (s->ps)
		s->ps->lpVtbl->AddRef(s->ps);
	s->viewport = d->viewport;
	s->fvf = d->fvf;
	memcpy(s->rs, d->rs, sizeof(s->rs));
	s->samp_min = d->samp_min;
	s->samp_mag = d->samp_mag;
	s->samp_addru = d->samp_addru;
	s->samp_addrv = d->samp_addrv;
	s->stream_freq[0] = d->stream_freq[0];
	s->stream_freq[1] = d->stream_freq[1];
	s->stream_freq[2] = d->stream_freq[2];
	s->world = d->world;
	s->view = d->view;
	s->proj = d->proj;
	s->scissor = d->scissor;
	s->valid = 1;
}

static void snap_apply(SwDevice *d, const SwSnap *s)
{
	if (!s || !s->valid)
		return;
	com_replace((IUnknown **)&d->rt0, (IUnknown *)s->rt0);
	com_replace((IUnknown **)&d->ds, (IUnknown *)s->ds);
	com_replace((IUnknown **)&d->tex0, (IUnknown *)s->tex0);
	com_replace((IUnknown **)&d->vb0, (IUnknown *)s->vb0);
	d->vb0_off = s->vb0_off;
	d->vb0_stride = s->vb0_stride;
	com_replace((IUnknown **)&d->vb1, (IUnknown *)s->vb1);
	d->vb1_off = s->vb1_off;
	d->vb1_stride = s->vb1_stride;
	com_replace((IUnknown **)&d->vb2, (IUnknown *)s->vb2);
	d->vb2_off = s->vb2_off;
	d->vb2_stride = s->vb2_stride;
	com_replace((IUnknown **)&d->ib, (IUnknown *)s->ib);
	com_replace((IUnknown **)&d->decl, (IUnknown *)s->decl);
	com_replace((IUnknown **)&d->vs, (IUnknown *)s->vs);
	com_replace((IUnknown **)&d->ps, (IUnknown *)s->ps);
	d->viewport = s->viewport;
	d->fvf = s->fvf;
	memcpy(d->rs, s->rs, sizeof(d->rs));
	d->samp_min = s->samp_min;
	d->samp_mag = s->samp_mag;
	d->samp_addru = s->samp_addru;
	d->samp_addrv = s->samp_addrv;
	d->stream_freq[0] = s->stream_freq[0];
	d->stream_freq[1] = s->stream_freq[1];
	d->stream_freq[2] = s->stream_freq[2];
	d->world = s->world;
	d->view = s->view;
	d->proj = s->proj;
	d->scissor = s->scissor;
}

static SwSB *sb_from(IDirect3DStateBlock9 *this)
{
	return (SwSB *)this;
}

static HRESULT WINAPI SB_QueryInterface(IDirect3DStateBlock9 *this, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3DStateBlock9)) {
		*ppv = this;
		this->lpVtbl->AddRef(this);
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI SB_AddRef(IDirect3DStateBlock9 *this)
{
	return (ULONG)InterlockedIncrement(&sb_from(this)->ref);
}

static ULONG WINAPI SB_Release(IDirect3DStateBlock9 *this)
{
	SwSB *s = sb_from(this);
	LONG n = InterlockedDecrement(&s->ref);
	if (n == 0) {
		snap_release(&s->snap);
		free(s);
	}
	return (ULONG)n;
}

static HRESULT WINAPI SB_GetDevice(IDirect3DStateBlock9 *this, IDirect3DDevice9 **out)
{
	SwSB *s = sb_from(this);
	if (!out)
		return E_POINTER;
	*out = s->dev;
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static HRESULT WINAPI SB_Capture(IDirect3DStateBlock9 *this)
{
	SwSB *s = sb_from(this);
	snap_from_dev(&s->snap, (SwDevice *)s->dev);
	return D3D_OK;
}

static HRESULT WINAPI SB_Apply(IDirect3DStateBlock9 *this)
{
	SwSB *s = sb_from(this);
	snap_apply((SwDevice *)s->dev, &s->snap);
	return D3D_OK;
}

static void ensure_sb_vtbl(void)
{
	static int ready;
	if (ready)
		return;
	memset(&kSBVtbl, 0, sizeof(kSBVtbl));
	kSBVtbl.QueryInterface = SB_QueryInterface;
	kSBVtbl.AddRef = SB_AddRef;
	kSBVtbl.Release = SB_Release;
	kSBVtbl.GetDevice = SB_GetDevice;
	kSBVtbl.Capture = SB_Capture;
	kSBVtbl.Apply = SB_Apply;
	ready = 1;
}

static HRESULT make_stateblock(IDirect3DDevice9 *dev, IDirect3DStateBlock9 **out)
{
	SwSB *s;
	ensure_sb_vtbl();
	s = (SwSB *)calloc(1, sizeof(*s));
	if (!s)
		return E_OUTOFMEMORY;
	s->iface.lpVtbl = &kSBVtbl;
	s->ref = 1;
	s->dev = dev;
	*out = &s->iface;
	return D3D_OK;
}

static HRESULT WINAPI Dev_BeginStateBlock(IDirect3DDevice9 *this)
{
	SwDevice *d = dev_from(this);
	if (d->recording)
		return D3DERR_INVALIDCALL;
	d->recording = 1;
	return D3D_OK;
}

static HRESULT WINAPI Dev_EndStateBlock(IDirect3DDevice9 *this, IDirect3DStateBlock9 **out)
{
	SwDevice *d = dev_from(this);
	if (!d->recording || !out)
		return D3DERR_INVALIDCALL;
	d->recording = 0;
	{
		HRESULT hr = make_stateblock(this, out);
		if (FAILED(hr))
			return hr;
		snap_from_dev(&((SwSB *)*out)->snap, d);
	}
	return D3D_OK;
}

static HRESULT WINAPI Dev_CreateStateBlock(IDirect3DDevice9 *this, D3DSTATEBLOCKTYPE type,
					   IDirect3DStateBlock9 **out)
{
	HRESULT hr;
	(void)type;
	if (!out)
		return D3DERR_INVALIDCALL;
	hr = make_stateblock(this, out);
	if (FAILED(hr))
		return hr;
	snap_from_dev(&((SwSB *)*out)->snap, dev_from(this));
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetScissorRect(IDirect3DDevice9 *this, const RECT *rect)
{
	if (!rect)
		return D3DERR_INVALIDCALL;
	dev_from(this)->scissor = *rect;
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetScissorRect(IDirect3DDevice9 *this, RECT *rect)
{
	if (!rect)
		return D3DERR_INVALIDCALL;
	*rect = dev_from(this)->scissor;
	return D3D_OK;
}

static IDirect3DQuery9Vtbl kQueryVtbl;

typedef struct SwQuery {
	IDirect3DQuery9 iface;
	LONG ref;
	IDirect3DDevice9 *dev;
	D3DQUERYTYPE type;
	UINT64 timestamp;
	DWORD occlusion;
} SwQuery;

static SwQuery *query_from(IDirect3DQuery9 *this)
{
	return (SwQuery *)this;
}

static HRESULT WINAPI Query_QueryInterface(IDirect3DQuery9 *this, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3DQuery9)) {
		*ppv = this;
		this->lpVtbl->AddRef(this);
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI Query_AddRef(IDirect3DQuery9 *this)
{
	return (ULONG)InterlockedIncrement(&query_from(this)->ref);
}

static ULONG WINAPI Query_Release(IDirect3DQuery9 *this)
{
	SwQuery *q = query_from(this);
	LONG n = InterlockedDecrement(&q->ref);
	if (n == 0)
		free(q);
	return (ULONG)n;
}

static HRESULT WINAPI Query_GetDevice(IDirect3DQuery9 *this, IDirect3DDevice9 **out)
{
	SwQuery *q = query_from(this);
	if (!out)
		return E_POINTER;
	*out = q->dev;
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static D3DQUERYTYPE WINAPI Query_GetType(IDirect3DQuery9 *this)
{
	return query_from(this)->type;
}

static DWORD WINAPI Query_GetDataSize(IDirect3DQuery9 *this)
{
	switch (query_from(this)->type) {
	case D3DQUERYTYPE_EVENT:
	case D3DQUERYTYPE_TIMESTAMPDISJOINT:
		return sizeof(WINBOOL);
	case D3DQUERYTYPE_OCCLUSION:
		return sizeof(DWORD);
	case D3DQUERYTYPE_TIMESTAMP:
	case D3DQUERYTYPE_TIMESTAMPFREQ:
		return sizeof(UINT64);
	default:
		return sizeof(DWORD);
	}
}

static UINT64 qpc_now(void)
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return (UINT64)t.QuadPart;
}

static UINT64 qpc_freq(void)
{
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	return f.QuadPart > 0 ? (UINT64)f.QuadPart : 1ull;
}

static HRESULT WINAPI Query_Issue(IDirect3DQuery9 *this, DWORD flags)
{
	SwQuery *q = query_from(this);
	sw_trace("Query.Issue type=%u flags=%08x", (unsigned)q->type, flags);
	if (q->type == D3DQUERYTYPE_TIMESTAMP)
		q->timestamp = qpc_now();
	else if (q->type == D3DQUERYTYPE_TIMESTAMPFREQ)
		q->timestamp = qpc_freq();
	else if (q->type == D3DQUERYTYPE_OCCLUSION) {
		if (flags & D3DISSUE_BEGIN)
			q->occlusion = 0;
		if (flags & D3DISSUE_END)
			q->occlusion = 1280u * 720u;
	}
	return D3D_OK;
}

static HRESULT WINAPI Query_GetData(IDirect3DQuery9 *this, void *data, DWORD size, DWORD flags)
{
	SwQuery *q = query_from(this);
	DWORD need = Query_GetDataSize(this);
	(void)flags;
	if (!data || size == 0)
		return S_OK;
	if (size < need)
		return D3DERR_INVALIDCALL;
	memset(data, 0, size);
	switch (q->type) {
	case D3DQUERYTYPE_EVENT: {
		WINBOOL done = TRUE;
		memcpy(data, &done, sizeof(done));
		break;
	}
	case D3DQUERYTYPE_OCCLUSION: {
		DWORD pix = q->occlusion ? q->occlusion : 1;
		memcpy(data, &pix, sizeof(pix));
		break;
	}
	case D3DQUERYTYPE_TIMESTAMP: {
		UINT64 t = q->timestamp ? q->timestamp : qpc_now();
		memcpy(data, &t, sizeof(t));
		break;
	}
	case D3DQUERYTYPE_TIMESTAMPFREQ: {
		UINT64 f = q->timestamp ? q->timestamp : qpc_freq();
		memcpy(data, &f, sizeof(f));
		break;
	}
	case D3DQUERYTYPE_TIMESTAMPDISJOINT: {
		WINBOOL disjoint = FALSE;
		memcpy(data, &disjoint, sizeof(disjoint));
		break;
	}
	default:
		break;
	}
	return S_OK;
}

static void ensure_query_vtbl(void)
{
	static int ready;
	if (ready)
		return;
	memset(&kQueryVtbl, 0, sizeof(kQueryVtbl));
	kQueryVtbl.QueryInterface = Query_QueryInterface;
	kQueryVtbl.AddRef = Query_AddRef;
	kQueryVtbl.Release = Query_Release;
	kQueryVtbl.GetDevice = Query_GetDevice;
	kQueryVtbl.GetType = Query_GetType;
	kQueryVtbl.GetDataSize = Query_GetDataSize;
	kQueryVtbl.Issue = Query_Issue;
	kQueryVtbl.GetData = Query_GetData;
	ready = 1;
}

static HRESULT WINAPI Dev_CreateQuery(IDirect3DDevice9 *this, D3DQUERYTYPE type,
				      IDirect3DQuery9 **out)
{
	SwQuery *q;
	{
		static LONG seen[32];
		unsigned idx = (unsigned)type;
		if (idx < 32 && InterlockedCompareExchange(&seen[idx], 1, 0) == 0) {
			char msg[48];
			_snprintf(msg, sizeof(msg), "CreateQuery type=%u", (unsigned)type);
			sw_log(msg);
		}
	}
	if (!out)
		return D3D_OK;
	ensure_query_vtbl();
	q = (SwQuery *)calloc(1, sizeof(*q));
	if (!q)
		return E_OUTOFMEMORY;
	q->iface.lpVtbl = &kQueryVtbl;
	q->ref = 1;
	q->dev = this;
	q->type = type;
	if (type == D3DQUERYTYPE_TIMESTAMPFREQ)
		q->timestamp = qpc_freq();
	*out = &q->iface;
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetDialogBoxMode(IDirect3DDevice9 *this, WINBOOL enable)
{
	(void)this;
	(void)enable;
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetTextureStageState(IDirect3DDevice9 *this, DWORD stage,
					       D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	(void)this;
	(void)stage;
	(void)type;
	(void)value;
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetTextureStageState(IDirect3DDevice9 *this, DWORD stage,
					       D3DTEXTURESTAGESTATETYPE type, DWORD *value)
{
	(void)this;
	(void)stage;
	(void)type;
	if (!value)
		return D3DERR_INVALIDCALL;
	*value = (type == D3DTSS_COLOROP) ? D3DTOP_MODULATE : 0;
	return D3D_OK;
}

static HRESULT WINAPI Dev_ValidateDevice(IDirect3DDevice9 *this, DWORD *passes)
{
	(void)this;
	if (!passes)
		return D3DERR_INVALIDCALL;
	*passes = 1;
	return D3D_OK;
}

static IDirect3DVertexDeclaration9Vtbl kDeclVtbl;
static IDirect3DVertexShader9Vtbl kVSVtbl;
static IDirect3DPixelShader9Vtbl kPSVtbl;

static SwDecl *decl_from(IDirect3DVertexDeclaration9 *this)
{
	return (SwDecl *)this;
}

static SwShader *sh_from_vs(IDirect3DVertexShader9 *this)
{
	return (SwShader *)this;
}

static HRESULT WINAPI Decl_QueryInterface(IDirect3DVertexDeclaration9 *this, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3DVertexDeclaration9)) {
		*ppv = this;
		this->lpVtbl->AddRef(this);
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI Decl_AddRef(IDirect3DVertexDeclaration9 *this)
{
	return (ULONG)InterlockedIncrement(&decl_from(this)->ref);
}

static ULONG WINAPI Decl_Release(IDirect3DVertexDeclaration9 *this)
{
	SwDecl *d = decl_from(this);
	LONG n = InterlockedDecrement(&d->ref);
	if (n == 0) {
		free(d->elems);
		free(d);
	}
	return (ULONG)n;
}

static HRESULT WINAPI Decl_GetDevice(IDirect3DVertexDeclaration9 *this, IDirect3DDevice9 **out)
{
	SwDecl *d = decl_from(this);
	if (!out)
		return E_POINTER;
	*out = d->dev;
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static HRESULT WINAPI Decl_GetDeclaration(IDirect3DVertexDeclaration9 *this, D3DVERTEXELEMENT9 *data,
					  UINT *n)
{
	SwDecl *d = decl_from(this);
	if (!n)
		return D3DERR_INVALIDCALL;
	if (!data) {
		*n = d->count;
		return D3D_OK;
	}
	if (*n < d->count) {
		*n = d->count;
		return D3DERR_MOREDATA;
	}
	memcpy(data, d->elems, d->count * sizeof(D3DVERTEXELEMENT9));
	*n = d->count;
	return D3D_OK;
}

static void ensure_decl_vtbl(void)
{
	static int ready;
	if (ready)
		return;
	memset(&kDeclVtbl, 0, sizeof(kDeclVtbl));
	kDeclVtbl.QueryInterface = Decl_QueryInterface;
	kDeclVtbl.AddRef = Decl_AddRef;
	kDeclVtbl.Release = Decl_Release;
	kDeclVtbl.GetDevice = Decl_GetDevice;
	kDeclVtbl.GetDeclaration = Decl_GetDeclaration;
	ready = 1;
}

static UINT shader_bytes(const DWORD *code)
{
	UINT i;
	if (!code)
		return 0;
	for (i = 0; i < 8192; i++) {
		if ((code[i] & 0xffff) == 0xffff)
			return (i + 1) * 4u;
		if ((code[i] & 0xffff) == 0xfffe)
			i += (code[i] >> 16) & 0x7fff;
	}
	return 0;
}

static void log_shader_ops(const char *tag, const DWORD *code, UINT bytes)
{
	char buf[400];
	int pos;
	UINT pc, ndwords;
	static LONG n;
	if (InterlockedIncrement(&n) > 8)
		return;
	ndwords = bytes / 4u;
	pos = _snprintf(buf, sizeof(buf), "%s", tag);
	pc = 1;
	while (pc < ndwords && pos < (int)sizeof(buf) - 20) {
		DWORD op = code[pc];
		int opcode = (int)(op & D3DSI_OPCODE_MASK);
		int extra = (int)((op & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT);
		if (opcode == D3DSIO_END)
			break;
		if (opcode == D3DSIO_COMMENT) {
			pc += 1u + (UINT)((op & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT);
			continue;
		}
		pos += _snprintf(buf + pos, sizeof(buf) - (size_t)pos, " %d", opcode);
		if (extra < 0)
			extra = 0;
		pc += 1u + (UINT)extra;
	}
	sw_log(buf);
}

static int sh_reg_type(DWORD t)
{
	return (int)(((t & D3DSP_REGTYPE_MASK) >> D3DSP_REGTYPE_SHIFT) |
		     ((t & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2));
}

static void log_shader_dcls(const char *tag, const DWORD *code, UINT bytes)
{
	char buf[400];
	int pos;
	UINT pc, ndwords;
	static const char *usage_n[] = { "pos", "bw", "bi", "nrm", "psize", "tex", "tan",
					 "bin", "tess", "posT", "col", "fog", "depth", "samp" };
	static LONG n;
	if (InterlockedIncrement(&n) > 8)
		return;
	ndwords = bytes / 4u;
	pos = _snprintf(buf, sizeof(buf), "%s dcl", tag);
	pc = 1;
	while (pc < ndwords && pos < (int)sizeof(buf) - 48) {
		DWORD op = code[pc];
		int opcode = (int)(op & D3DSI_OPCODE_MASK);
		int extra = (int)((op & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT);
		if (opcode == D3DSIO_END)
			break;
		if (opcode == D3DSIO_COMMENT) {
			pc += 1u + (UINT)((op & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT);
			continue;
		}
		if (opcode == D3DSIO_DCL && extra >= 2 && pc + 2 < ndwords) {
			DWORD usage_tok = code[pc + 1];
			DWORD dest = code[pc + 2];
			int usage = (int)(usage_tok & D3DSP_DCL_USAGE_MASK);
			int uidx = (int)((usage_tok & D3DSP_DCL_USAGEINDEX_MASK) >>
					 D3DSP_DCL_USAGEINDEX_SHIFT);
			int rtype = sh_reg_type(dest);
			int rnum = (int)(dest & D3DSP_REGNUM_MASK);
			const char *un = (usage >= 0 && usage < 14) ? usage_n[usage] : "?";
			const char *rn = (rtype == D3DSPR_INPUT)    ? "v"
					 : (rtype == D3DSPR_RASTOUT) ? "oPos"
					 : (rtype == D3DSPR_ATTROUT) ? "oD"
					 : (rtype == D3DSPR_OUTPUT)  ? "o"
					 : "r";
			pos += _snprintf(buf + pos, sizeof(buf) - (size_t)pos, " %s%d:%s%d", rn, rnum,
					 un, uidx);
		}
		if (extra < 0)
			extra = 0;
		pc += 1u + (UINT)extra;
	}
	sw_log(buf);
}

static HRESULT WINAPI VS_QueryInterface(IDirect3DVertexShader9 *this, REFIID riid, void **ppv)
{
	SwShader *s = sh_from_vs(this);
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) ||
	    (!s->is_ps && IsEqualGUID(riid, &IID_IDirect3DVertexShader9)) ||
	    (s->is_ps && IsEqualGUID(riid, &IID_IDirect3DPixelShader9))) {
		*ppv = this;
		this->lpVtbl->AddRef(this);
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI VS_AddRef(IDirect3DVertexShader9 *this)
{
	return (ULONG)InterlockedIncrement(&sh_from_vs(this)->ref);
}

static ULONG WINAPI VS_Release(IDirect3DVertexShader9 *this)
{
	SwShader *s = sh_from_vs(this);
	LONG n = InterlockedDecrement(&s->ref);
	if (n == 0) {
		free(s->code);
		free(s);
	}
	return (ULONG)n;
}

static HRESULT WINAPI VS_GetDevice(IDirect3DVertexShader9 *this, IDirect3DDevice9 **out)
{
	SwShader *s = sh_from_vs(this);
	if (!out)
		return E_POINTER;
	*out = s->dev;
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static HRESULT WINAPI VS_GetFunction(IDirect3DVertexShader9 *this, void *data, UINT *size)
{
	SwShader *s = sh_from_vs(this);
	if (!size)
		return D3DERR_INVALIDCALL;
	if (!data) {
		*size = s->bytes;
		return D3D_OK;
	}
	if (*size < s->bytes) {
		*size = s->bytes;
		return D3DERR_MOREDATA;
	}
	memcpy(data, s->code, s->bytes);
	*size = s->bytes;
	return D3D_OK;
}

static void ensure_shader_vtbls(void)
{
	static int ready;
	if (ready)
		return;
	memset(&kVSVtbl, 0, sizeof(kVSVtbl));
	kVSVtbl.QueryInterface = VS_QueryInterface;
	kVSVtbl.AddRef = VS_AddRef;
	kVSVtbl.Release = VS_Release;
	kVSVtbl.GetDevice = VS_GetDevice;
	kVSVtbl.GetFunction = VS_GetFunction;
	kPSVtbl.QueryInterface = (void *)VS_QueryInterface;
	kPSVtbl.AddRef = (void *)VS_AddRef;
	kPSVtbl.Release = (void *)VS_Release;
	kPSVtbl.GetDevice = (void *)VS_GetDevice;
	kPSVtbl.GetFunction = (void *)VS_GetFunction;
	ready = 1;
}

static HRESULT WINAPI Dev_CreateVertexDeclaration(IDirect3DDevice9 *this,
						  const D3DVERTEXELEMENT9 *elems,
						  IDirect3DVertexDeclaration9 **out)
{
	SwDecl *d;
	UINT n = 0;
	if (!elems || !out)
		return D3DERR_INVALIDCALL;
	while (n < MAXD3DDECLLENGTH + 1u) {
		if (elems[n].Stream == 0xff)
			break;
		n++;
	}
	if (n >= MAXD3DDECLLENGTH + 1u || elems[n].Stream != 0xff)
		return D3DERR_INVALIDCALL;
	n++;
	ensure_decl_vtbl();
	d = (SwDecl *)calloc(1, sizeof(*d));
	if (!d)
		return E_OUTOFMEMORY;
	d->elems = (D3DVERTEXELEMENT9 *)malloc(n * sizeof(D3DVERTEXELEMENT9));
	if (!d->elems) {
		free(d);
		return E_OUTOFMEMORY;
	}
	memcpy(d->elems, elems, n * sizeof(D3DVERTEXELEMENT9));
	d->count = n;
	d->iface.lpVtbl = &kDeclVtbl;
	d->ref = 1;
	d->dev = this;
	{
		static LONG nlog;
		if (InterlockedIncrement(&nlog) <= 8) {
			UINT i;
			for (i = 0; i + 1 < n && i < 10; i++) {
				char msg[128];
				_snprintf(msg, sizeof(msg),
					  "decl#%ld [%u] s=%u off=%u type=%u usage=%u idx=%u", nlog, i,
					  elems[i].Stream, elems[i].Offset, elems[i].Type,
					  elems[i].Usage, elems[i].UsageIndex);
				sw_log(msg);
			}
		}
	}
	*out = &d->iface;
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetVertexDeclaration(IDirect3DDevice9 *this,
					       IDirect3DVertexDeclaration9 *decl)
{
	SwDevice *d = dev_from(this);
	com_replace((IUnknown **)&d->decl, (IUnknown *)decl);
	if (decl)
		d->fvf = 0;
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetVertexDeclaration(IDirect3DDevice9 *this,
					       IDirect3DVertexDeclaration9 **out)
{
	SwDevice *d = dev_from(this);
	if (!out)
		return D3DERR_INVALIDCALL;
	*out = d->decl;
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static HRESULT create_shader(IDirect3DDevice9 *this, const DWORD *code, int is_ps, void **out)
{
	SwShader *s;
	UINT bytes;
	if (!code || !out)
		return D3DERR_INVALIDCALL;
	bytes = shader_bytes(code);
	if (!bytes)
		return D3DERR_INVALIDCALL;
	ensure_shader_vtbls();
	s = (SwShader *)calloc(1, sizeof(*s));
	if (!s)
		return E_OUTOFMEMORY;
	s->code = (DWORD *)malloc(bytes);
	if (!s->code) {
		free(s);
		return E_OUTOFMEMORY;
	}
	memcpy(s->code, code, bytes);
	s->bytes = bytes;
	s->is_ps = is_ps;
	s->ref = 1;
	s->dev = this;
	log_shader_ops(is_ps ? "ps ops" : "vs ops", s->code, bytes);
	log_shader_dcls(is_ps ? "ps" : "vs", s->code, bytes);
	if (is_ps)
		s->vs.lpVtbl = (IDirect3DVertexShader9Vtbl *)&kPSVtbl;
	else
		s->vs.lpVtbl = &kVSVtbl;
	*out = &s->vs;
	return D3D_OK;
}

static HRESULT WINAPI Dev_CreateVertexShader(IDirect3DDevice9 *this, const DWORD *code,
					     IDirect3DVertexShader9 **out)
{
	return create_shader(this, code, 0, (void **)out);
}

static HRESULT WINAPI Dev_CreatePixelShader(IDirect3DDevice9 *this, const DWORD *code,
					    IDirect3DPixelShader9 **out)
{
	return create_shader(this, code, 1, (void **)out);
}

static HRESULT WINAPI Dev_SetVertexShader(IDirect3DDevice9 *this, IDirect3DVertexShader9 *sh)
{
	SwDevice *d = dev_from(this);
	com_replace((IUnknown **)&d->vs, (IUnknown *)sh);
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetPixelShader(IDirect3DDevice9 *this, IDirect3DPixelShader9 *sh)
{
	SwDevice *d = dev_from(this);
	com_replace((IUnknown **)&d->ps, (IUnknown *)sh);
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetVertexShader(IDirect3DDevice9 *this, IDirect3DVertexShader9 **out)
{
	SwDevice *d = dev_from(this);
	if (!out)
		return D3DERR_INVALIDCALL;
	*out = d->vs;
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetPixelShader(IDirect3DDevice9 *this, IDirect3DPixelShader9 **out)
{
	SwDevice *d = dev_from(this);
	if (!out)
		return D3DERR_INVALIDCALL;
	*out = d->ps;
	if (*out)
		(*out)->lpVtbl->AddRef(*out);
	return D3D_OK;
}

static HRESULT set_const_f(float dst[][4], UINT cap, UINT reg, const float *data, UINT count)
{
	if (!data)
		return D3DERR_INVALIDCALL;
	if (reg >= cap)
		return D3D_OK;
	if (reg + count > cap)
		count = cap - reg;
	memcpy(dst[reg], data, count * 4u * sizeof(float));
	return D3D_OK;
}

static HRESULT get_const_f(float src[][4], UINT cap, UINT reg, float *data, UINT count)
{
	if (!data)
		return D3DERR_INVALIDCALL;
	if (reg >= cap)
		return D3DERR_INVALIDCALL;
	if (reg + count > cap)
		count = cap - reg;
	memcpy(data, src[reg], count * 4u * sizeof(float));
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetVertexShaderConstantF(IDirect3DDevice9 *this, UINT reg,
						   const float *data, UINT count)
{
	SwDevice *d = dev_from(this);
	d->vs_c_ver++;
	return set_const_f(d->vs_c, 256, reg, data, count);
}

static HRESULT WINAPI Dev_GetVertexShaderConstantF(IDirect3DDevice9 *this, UINT reg, float *data,
						   UINT count)
{
	return get_const_f(dev_from(this)->vs_c, 256, reg, data, count);
}

static HRESULT WINAPI Dev_SetPixelShaderConstantF(IDirect3DDevice9 *this, UINT reg,
						  const float *data, UINT count)
{
	return set_const_f(dev_from(this)->ps_c, 224, reg, data, count);
}

static HRESULT WINAPI Dev_GetPixelShaderConstantF(IDirect3DDevice9 *this, UINT reg, float *data,
						  UINT count)
{
	return get_const_f(dev_from(this)->ps_c, 224, reg, data, count);
}

static HRESULT WINAPI Dev_SetVertexShaderConstantI(IDirect3DDevice9 *this, UINT reg, const int *data,
						   UINT count)
{
	SwDevice *d = dev_from(this);
	if (!data)
		return D3DERR_INVALIDCALL;
	if (reg >= 16)
		return D3D_OK;
	if (reg + count > 16)
		count = 16 - reg;
	memcpy(d->vs_i[reg], data, count * 4u * sizeof(int));
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetVertexShaderConstantI(IDirect3DDevice9 *this, UINT reg, int *data,
						   UINT count)
{
	SwDevice *d = dev_from(this);
	if (!data)
		return D3DERR_INVALIDCALL;
	if (reg >= 16)
		return D3DERR_INVALIDCALL;
	if (reg + count > 16)
		count = 16 - reg;
	memcpy(data, d->vs_i[reg], count * 4u * sizeof(int));
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetPixelShaderConstantI(IDirect3DDevice9 *this, UINT reg, const int *data,
						  UINT count)
{
	SwDevice *d = dev_from(this);
	if (!data)
		return D3DERR_INVALIDCALL;
	if (reg >= 16)
		return D3D_OK;
	if (reg + count > 16)
		count = 16 - reg;
	memcpy(d->ps_i[reg], data, count * 4u * sizeof(int));
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetPixelShaderConstantI(IDirect3DDevice9 *this, UINT reg, int *data,
						  UINT count)
{
	SwDevice *d = dev_from(this);
	if (!data)
		return D3DERR_INVALIDCALL;
	if (reg >= 16)
		return D3DERR_INVALIDCALL;
	if (reg + count > 16)
		count = 16 - reg;
	memcpy(data, d->ps_i[reg], count * 4u * sizeof(int));
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetVertexShaderConstantB(IDirect3DDevice9 *this, UINT reg,
						   const WINBOOL *data, UINT count)
{
	SwDevice *d = dev_from(this);
	UINT i;
	if (!data)
		return D3DERR_INVALIDCALL;
	for (i = 0; i < count && reg + i < 16; i++)
		d->vs_b[reg + i] = data[i];
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetVertexShaderConstantB(IDirect3DDevice9 *this, UINT reg, WINBOOL *data,
						   UINT count)
{
	SwDevice *d = dev_from(this);
	UINT i;
	if (!data)
		return D3DERR_INVALIDCALL;
	for (i = 0; i < count && reg + i < 16; i++)
		data[i] = d->vs_b[reg + i];
	return D3D_OK;
}

static HRESULT WINAPI Dev_SetPixelShaderConstantB(IDirect3DDevice9 *this, UINT reg,
						  const WINBOOL *data, UINT count)
{
	SwDevice *d = dev_from(this);
	UINT i;
	if (!data)
		return D3DERR_INVALIDCALL;
	for (i = 0; i < count && reg + i < 16; i++)
		d->ps_b[reg + i] = data[i];
	return D3D_OK;
}

static HRESULT WINAPI Dev_GetPixelShaderConstantB(IDirect3DDevice9 *this, UINT reg, WINBOOL *data,
						  UINT count)
{
	SwDevice *d = dev_from(this);
	UINT i;
	if (!data)
		return D3DERR_INVALIDCALL;
	for (i = 0; i < count && reg + i < 16; i++)
		data[i] = d->ps_b[reg + i];
	return D3D_OK;
}

static IDirect3DDevice9Vtbl kDevVtbl;

static void ensure_dev_vtbl(void)
{
	void **slots;
	size_t i, n;
	static int ready;
	if (ready)
		return;
	memset(&kDevVtbl, 0, sizeof(kDevVtbl));
	kDevVtbl.QueryInterface = Dev_QueryInterface;
	kDevVtbl.AddRef = Dev_AddRef;
	kDevVtbl.Release = Dev_Release;
	kDevVtbl.TestCooperativeLevel = Dev_TestCooperativeLevel;
	kDevVtbl.GetAvailableTextureMem = Dev_GetAvailableTextureMem;
	kDevVtbl.EvictManagedResources = Dev_EvictManagedResources;
	kDevVtbl.GetDirect3D = Dev_GetDirect3D;
	kDevVtbl.GetDeviceCaps = Dev_GetDeviceCaps;
	kDevVtbl.GetDisplayMode = Dev_GetDisplayMode;
	kDevVtbl.GetCreationParameters = Dev_GetCreationParameters;
	kDevVtbl.SetCursorPosition = Dev_SetCursorPosition;
	kDevVtbl.ShowCursor = Dev_ShowCursor;
	kDevVtbl.GetNumberOfSwapChains = Dev_GetNumberOfSwapChains;
	kDevVtbl.Reset = Dev_Reset;
	kDevVtbl.Present = Dev_Present;
	kDevVtbl.GetRasterStatus = Dev_GetRasterStatus;
	kDevVtbl.SetGammaRamp = Dev_SetGammaRamp;
	kDevVtbl.GetGammaRamp = Dev_GetGammaRamp;
	kDevVtbl.BeginScene = Dev_BeginScene;
	kDevVtbl.EndScene = Dev_EndScene;
	kDevVtbl.Clear = Dev_Clear;
	kDevVtbl.SetTransform = Dev_SetTransform;
	kDevVtbl.GetTransform = Dev_GetTransform;
	kDevVtbl.MultiplyTransform = Dev_MultiplyTransform;
	kDevVtbl.SetViewport = Dev_SetViewport;
	kDevVtbl.GetViewport = Dev_GetViewport;
	kDevVtbl.SetRenderState = Dev_SetRenderState;
	kDevVtbl.GetRenderState = Dev_GetRenderState;
	kDevVtbl.SetSoftwareVertexProcessing = Dev_SetSoftwareVertexProcessing;
	kDevVtbl.GetSoftwareVertexProcessing = Dev_GetSoftwareVertexProcessing;
	kDevVtbl.GetNPatchMode = Dev_GetNPatchMode;
	kDevVtbl.DrawPrimitive = Dev_DrawPrimitive;
	kDevVtbl.DrawIndexedPrimitive = Dev_DrawIndexedPrimitive;
	kDevVtbl.DrawPrimitiveUP = Dev_DrawPrimitiveUP;
	kDevVtbl.DrawIndexedPrimitiveUP = Dev_DrawIndexedPrimitiveUP;
	kDevVtbl.SetFVF = Dev_SetFVF;
	kDevVtbl.GetFVF = Dev_GetFVF;
	kDevVtbl.CreateTexture = Dev_CreateTexture;
	kDevVtbl.CreateVertexBuffer = Dev_CreateVertexBuffer;
	kDevVtbl.CreateIndexBuffer = Dev_CreateIndexBuffer;
	kDevVtbl.SetStreamSource = Dev_SetStreamSource;
	kDevVtbl.GetStreamSource = Dev_GetStreamSource;
	kDevVtbl.SetIndices = Dev_SetIndices;
	kDevVtbl.GetIndices = Dev_GetIndices;
	kDevVtbl.SetTexture = Dev_SetTexture;
	kDevVtbl.GetTexture = Dev_GetTexture;
	kDevVtbl.SetSamplerState = Dev_SetSamplerState;
	kDevVtbl.GetSamplerState = Dev_GetSamplerState;
	kDevVtbl.SetDialogBoxMode = Dev_SetDialogBoxMode;
	kDevVtbl.SetTextureStageState = Dev_SetTextureStageState;
	kDevVtbl.GetTextureStageState = Dev_GetTextureStageState;
	kDevVtbl.ValidateDevice = Dev_ValidateDevice;
	kDevVtbl.CreateVertexDeclaration = Dev_CreateVertexDeclaration;
	kDevVtbl.SetVertexDeclaration = Dev_SetVertexDeclaration;
	kDevVtbl.GetVertexDeclaration = Dev_GetVertexDeclaration;
	kDevVtbl.CreateVertexShader = Dev_CreateVertexShader;
	kDevVtbl.SetVertexShader = Dev_SetVertexShader;
	kDevVtbl.GetVertexShader = Dev_GetVertexShader;
	kDevVtbl.SetVertexShaderConstantF = Dev_SetVertexShaderConstantF;
	kDevVtbl.GetVertexShaderConstantF = Dev_GetVertexShaderConstantF;
	kDevVtbl.SetVertexShaderConstantI = Dev_SetVertexShaderConstantI;
	kDevVtbl.GetVertexShaderConstantI = Dev_GetVertexShaderConstantI;
	kDevVtbl.SetVertexShaderConstantB = Dev_SetVertexShaderConstantB;
	kDevVtbl.GetVertexShaderConstantB = Dev_GetVertexShaderConstantB;
	kDevVtbl.CreatePixelShader = Dev_CreatePixelShader;
	kDevVtbl.SetPixelShader = Dev_SetPixelShader;
	kDevVtbl.GetPixelShader = Dev_GetPixelShader;
	kDevVtbl.SetPixelShaderConstantF = Dev_SetPixelShaderConstantF;
	kDevVtbl.GetPixelShaderConstantF = Dev_GetPixelShaderConstantF;
	kDevVtbl.SetPixelShaderConstantI = Dev_SetPixelShaderConstantI;
	kDevVtbl.GetPixelShaderConstantI = Dev_GetPixelShaderConstantI;
	kDevVtbl.SetPixelShaderConstantB = Dev_SetPixelShaderConstantB;
	kDevVtbl.GetPixelShaderConstantB = Dev_GetPixelShaderConstantB;
	kDevVtbl.SetCursorProperties = Stub_SetCursorProperties;
	kDevVtbl.CreateAdditionalSwapChain = Stub_CreateAdditionalSwapChain;
	kDevVtbl.GetSwapChain = Dev_GetSwapChain;
	kDevVtbl.GetBackBuffer = Dev_GetBackBuffer;
	kDevVtbl.CreateVolumeTexture = Stub_CreateVolumeTexture;
	kDevVtbl.CreateCubeTexture = Stub_CreateCubeTexture;
	kDevVtbl.CreateRenderTarget = Dev_CreateRenderTarget;
	kDevVtbl.CreateDepthStencilSurface = Dev_CreateDepthStencilSurface;
	kDevVtbl.UpdateSurface = Stub_UpdateSurface;
	kDevVtbl.UpdateTexture = Dev_UpdateTexture;
	kDevVtbl.GetRenderTargetData = Stub_GetRenderTargetData;
	kDevVtbl.GetFrontBufferData = Stub_GetFrontBufferData;
	kDevVtbl.StretchRect = Dev_StretchRect;
	kDevVtbl.ColorFill = Stub_ColorFill;
	kDevVtbl.CreateOffscreenPlainSurface = Stub_CreateOffscreenPlainSurface;
	kDevVtbl.SetRenderTarget = Dev_SetRenderTarget;
	kDevVtbl.GetRenderTarget = Dev_GetRenderTarget;
	kDevVtbl.SetDepthStencilSurface = Dev_SetDepthStencilSurface;
	kDevVtbl.GetDepthStencilSurface = Dev_GetDepthStencilSurface;
	kDevVtbl.SetMaterial = Stub_SetMaterial;
	kDevVtbl.GetMaterial = Stub_GetMaterial;
	kDevVtbl.SetLight = Stub_SetLight;
	kDevVtbl.GetLight = Stub_GetLight;
	kDevVtbl.LightEnable = Stub_LightEnable;
	kDevVtbl.GetLightEnable = Stub_GetLightEnable;
	kDevVtbl.SetClipPlane = Stub_SetClipPlane;
	kDevVtbl.GetClipPlane = Stub_GetClipPlane;
	kDevVtbl.CreateStateBlock = Dev_CreateStateBlock;
	kDevVtbl.BeginStateBlock = Dev_BeginStateBlock;
	kDevVtbl.EndStateBlock = Dev_EndStateBlock;
	kDevVtbl.SetClipStatus = Stub_SetClipStatus;
	kDevVtbl.GetClipStatus = Stub_GetClipStatus;
	kDevVtbl.SetPaletteEntries = Stub_SetPaletteEntries;
	kDevVtbl.GetPaletteEntries = Stub_GetPaletteEntries;
	kDevVtbl.SetCurrentTexturePalette = Stub_SetCurrentTexturePalette;
	kDevVtbl.GetCurrentTexturePalette = Stub_GetCurrentTexturePalette;
	kDevVtbl.SetScissorRect = Dev_SetScissorRect;
	kDevVtbl.GetScissorRect = Dev_GetScissorRect;
	kDevVtbl.SetNPatchMode = Stub_SetNPatchMode;
	kDevVtbl.ProcessVertices = Stub_ProcessVertices;
	kDevVtbl.SetStreamSourceFreq = Dev_SetStreamSourceFreq;
	kDevVtbl.GetStreamSourceFreq = Dev_GetStreamSourceFreq;
	kDevVtbl.DrawRectPatch = Stub_DrawRectPatch;
	kDevVtbl.DrawTriPatch = Stub_DrawTriPatch;
	kDevVtbl.DeletePatch = Stub_DeletePatch;
	kDevVtbl.CreateQuery = Dev_CreateQuery;
	slots = (void **)&kDevVtbl;
	n = sizeof(kDevVtbl) / sizeof(void *);
	for (i = 0; i < n; i++) {
		if (!slots[i])
			slots[i] = (void *)Dev_NotImpl;
	}
	d3d9_trace_wrap_dev((void **)&kDevVtbl, sizeof(kDevVtbl));
	ready = 1;
}


static HRESULT WINAPI D3D_QueryInterface(IDirect3D9 *this, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3D9)) {
		*ppv = this;
		this->lpVtbl->AddRef(this);
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI D3D_AddRef(IDirect3D9 *this)
{
	return (ULONG)InterlockedIncrement(&d3d_from(this)->ref);
}

static ULONG WINAPI D3D_Release(IDirect3D9 *this)
{
	SwD3D9 *d = d3d_from(this);
	LONG n = InterlockedDecrement(&d->ref);
	if (n == 0)
		free(d);
	return (ULONG)n;
}

static HRESULT WINAPI D3D_RegisterSoftwareDevice(IDirect3D9 *this, void *fn)
{
	(void)this;
	(void)fn;
	return D3D_OK;
}

static UINT WINAPI D3D_GetAdapterCount(IDirect3D9 *this)
{
	(void)this;
	return 1;
}

static HRESULT WINAPI D3D_GetAdapterIdentifier(IDirect3D9 *this, UINT adapter, DWORD flags,
					       D3DADAPTER_IDENTIFIER9 *id)
{
	(void)this;
	(void)flags;
	if (adapter != 0 || !id)
		return D3DERR_INVALIDCALL;
	memset(id, 0, sizeof(*id));
	strcpy(id->Driver, "d3d9_sw");
	strcpy(id->Description, "d3d9_sw software raster");
	id->VendorId = 0x8086;
	id->DeviceId = 0x0001;
	return D3D_OK;
}

static UINT WINAPI D3D_GetAdapterModeCount(IDirect3D9 *this, UINT adapter, D3DFORMAT fmt)
{
	(void)this;
	(void)fmt;
	return adapter == 0 ? 1 : 0;
}

static HRESULT WINAPI D3D_EnumAdapterModes(IDirect3D9 *this, UINT adapter, D3DFORMAT fmt,
					   UINT mode, D3DDISPLAYMODE *out)
{
	(void)this;
	(void)fmt;
	if (adapter != 0 || mode != 0 || !out)
		return D3DERR_INVALIDCALL;
	out->Width = (UINT)GetSystemMetrics(SM_CXSCREEN);
	out->Height = (UINT)GetSystemMetrics(SM_CYSCREEN);
	if (out->Width == 0 || out->Height == 0) {
		out->Width = D3D9_SW_FB_W;
		out->Height = D3D9_SW_FB_H;
	}
	out->RefreshRate = 60;
	out->Format = D3DFMT_X8R8G8B8;
	return D3D_OK;
}

static HRESULT WINAPI D3D_GetAdapterDisplayMode(IDirect3D9 *this, UINT adapter,
						D3DDISPLAYMODE *out)
{
	return D3D_EnumAdapterModes(this, adapter, D3DFMT_X8R8G8B8, 0, out);
}

static HRESULT WINAPI D3D_CheckDeviceType(IDirect3D9 *this, UINT a, D3DDEVTYPE t, D3DFORMAT d,
					  D3DFORMAT b, WINBOOL w)
{
	(void)this;
	(void)a;
	(void)t;
	(void)d;
	(void)b;
	(void)w;
	return D3D_OK;
}

static HRESULT WINAPI D3D_CheckDeviceFormat(IDirect3D9 *this, UINT a, D3DDEVTYPE t, D3DFORMAT af,
					    DWORD usage, D3DRESOURCETYPE rt, D3DFORMAT cf)
{
	(void)this;
	(void)a;
	(void)t;
	(void)af;
	(void)usage;
	(void)rt;
	(void)cf;
	return D3D_OK;
}

static HRESULT WINAPI D3D_CheckDeviceMultiSampleType(IDirect3D9 *this, UINT a, D3DDEVTYPE t,
						     D3DFORMAT f, WINBOOL w, D3DMULTISAMPLE_TYPE m,
						     DWORD *q)
{
	(void)this;
	(void)a;
	(void)t;
	(void)f;
	(void)w;
	(void)m;
	if (q)
		*q = 1;
	return D3D_OK;
}

static HRESULT WINAPI D3D_CheckDepthStencilMatch(IDirect3D9 *this, UINT a, D3DDEVTYPE t,
						 D3DFORMAT af, D3DFORMAT rt, D3DFORMAT ds)
{
	(void)this;
	(void)a;
	(void)t;
	(void)af;
	(void)rt;
	(void)ds;
	return D3D_OK;
}

static HRESULT WINAPI D3D_CheckDeviceFormatConversion(IDirect3D9 *this, UINT a, D3DDEVTYPE t,
						      D3DFORMAT s, D3DFORMAT d)
{
	(void)this;
	(void)a;
	(void)t;
	(void)s;
	(void)d;
	return D3D_OK;
}

static HRESULT WINAPI D3D_GetDeviceCaps(IDirect3D9 *this, UINT adapter, D3DDEVTYPE type,
					D3DCAPS9 *caps)
{
	(void)this;
	(void)adapter;
	(void)type;
	return Dev_GetDeviceCaps((IDirect3DDevice9 *)&kDevVtbl, caps);
}

static HMONITOR WINAPI D3D_GetAdapterMonitor(IDirect3D9 *this, UINT adapter)
{
	(void)this;
	(void)adapter;
	return MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

static HRESULT WINAPI D3D_CreateDevice(IDirect3D9 *this, UINT adapter, D3DDEVTYPE type,
				       HWND focus, DWORD behavior, D3DPRESENT_PARAMETERS *pp,
				       IDirect3DDevice9 **out)
{
	SwDevice *dev;
	HWND hwnd;
	int w, h;
	(void)type;
	(void)behavior;
	sw_trace("D3D9.CreateDevice adapter=%u type=%u behavior=%08x %ux%u", adapter, (unsigned)type,
		 behavior, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
	ensure_dev_vtbl();
	if (adapter != 0 || !pp || !out)
		return D3DERR_INVALIDCALL;
	*out = NULL;
	hwnd = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
	w = (int)pp->BackBufferWidth;
	h = (int)pp->BackBufferHeight;
	backbuffer_size(hwnd, &w, &h);
	dev = (SwDevice *)calloc(1, sizeof(*dev));
	if (!dev)
		return E_OUTOFMEMORY;
	dev->iface.lpVtbl = &kDevVtbl;
	dev->ref = 1;
	dev->parent = d3d_from(this);
	dev->parent->iface.lpVtbl->AddRef(&dev->parent->iface);
	dev->focus = focus;
	dev->device_window = hwnd;
	ime_detach_window(hwnd);
	if (focus != hwnd)
		ime_detach_window(focus);
	dev->pp = *pp;
	identity_matrix(&dev->world);
	identity_matrix(&dev->view);
	identity_matrix(&dev->proj);
	dev->samp_min = D3DTEXF_LINEAR;
	dev->samp_mag = D3DTEXF_LINEAR;
	dev->samp_addru = D3DTADDRESS_WRAP;
	dev->samp_addrv = D3DTADDRESS_WRAP;
	dev->rs[D3DRS_ZENABLE] = D3DZB_TRUE;
	dev->rs[D3DRS_ZWRITEENABLE] = TRUE;
	dev->rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
	dev->rs[D3DRS_ALPHATESTENABLE] = FALSE;
	dev->rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
	dev->rs[D3DRS_ALPHAREF] = 0;
	dev->rs[D3DRS_ALPHABLENDENABLE] = FALSE;
	dev->rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
	dev->rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
	dev->rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
	dev->rs[D3DRS_CULLMODE] = D3DCULL_CCW;
	dev->rs[D3DRS_COLORWRITEENABLE] = 0xf;
	dev->rs[D3DRS_SEPARATEALPHABLENDENABLE] = FALSE;
	dev->rs[D3DRS_SCISSORTESTENABLE] = FALSE;
	dev->viewport.Width = (DWORD)w;
	dev->viewport.Height = (DWORD)h;
	dev->viewport.MaxZ = 1.0f;
	dev->scissor.right = w;
	dev->scissor.bottom = h;
	if (!swrast_init(&dev->rast, hwnd, w, h)) {
		dev->iface.lpVtbl->Release(&dev->iface);
		return E_OUTOFMEMORY;
	}
	*out = &dev->iface;
	return D3D_OK;
}

static IDirect3D9Vtbl kD3DVtbl = {
    D3D_QueryInterface,
    D3D_AddRef,
    D3D_Release,
    D3D_RegisterSoftwareDevice,
    D3D_GetAdapterCount,
    D3D_GetAdapterIdentifier,
    D3D_GetAdapterModeCount,
    D3D_EnumAdapterModes,
    D3D_GetAdapterDisplayMode,
    D3D_CheckDeviceType,
    D3D_CheckDeviceFormat,
    D3D_CheckDeviceMultiSampleType,
    D3D_CheckDepthStencilMatch,
    D3D_CheckDeviceFormatConversion,
    D3D_GetDeviceCaps,
    D3D_GetAdapterMonitor,
    D3D_CreateDevice,
};

static LONG g_perf_nest;

int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, const WCHAR *name)
{
	(void)color;
	(void)name;
	return (int)InterlockedIncrement(&g_perf_nest) - 1;
}

int WINAPI D3DPERF_EndEvent(void)
{
	LONG n = InterlockedDecrement(&g_perf_nest);
	if (n < 0) {
		InterlockedIncrement(&g_perf_nest);
		return -1;
	}
	return (int)n;
}

DWORD WINAPI D3DPERF_GetStatus(void)
{
	return 0;
}

WINBOOL WINAPI D3DPERF_QueryRepeatFrame(void)
{
	return FALSE;
}

void WINAPI D3DPERF_SetMarker(D3DCOLOR color, const WCHAR *name)
{
	(void)color;
	(void)name;
}

void WINAPI D3DPERF_SetOptions(DWORD options)
{
	(void)options;
}

void WINAPI D3DPERF_SetRegion(D3DCOLOR color, const WCHAR *name)
{
	(void)color;
	(void)name;
}

/* A process only ever sees the environment its parent had when the parent
 * started, and the game's parent is Steam, which can have been running for
 * days. So a diagnostic variable set an hour ago never arrives, and a switch
 * that silently never arrived is indistinguishable from one that arrived and
 * changed nothing - which is exactly how a seam experiment came back "no
 * difference" when the setting had never reached the game at all. Record the
 * raw string alongside what it resolved to, so a run can be trusted or
 * discarded from its own log rather than from memory of what was set. */
static void log_env_once(void)
{
	static LONG done;
	const char *filt, *puv, *sub, *nosimd;
	char msg[256];

	if (InterlockedCompareExchange(&done, 1, 0) != 0)
		return;
	filt = getenv("D3D9SW_FILTER");
	puv = getenv("D3D9SW_PIXELUV");
	sub = getenv("D3D9SW_SUBPIXEL");
	nosimd = getenv("D3D9SW_NOSIMD");
	/* sw_log, not sw_trace: the trace log is off by default, and a banner
	 * nobody can see is worse than none - it reads as a wrapper that never
	 * loaded. */
	_snprintf(msg, sizeof(msg),
		  "env: HALFPIXEL=%s  FILTER=%s (%s)  PIXELUV=%s (%s)  SUBPIXEL=%s  NOSIMD=%s",
		  half_pixel_fix() ? "on, D3D9 pixel centres" : "OFF, D3D10 pixel centres",
		  filt ? filt : "unset",
		  filter_override() < 0	  ? "game decides" :
		  filter_override()	  ? "forced linear" :
					    "forced point",
		  puv ? puv : "unset", pixel_uv_hack() ? "rescale on" : "rescale off",
		  sub ? sub : "unset, 4 bits", nosimd ? nosimd : "unset");
	sw_log(msg);
}

IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk)
{
	SwD3D9 *d;
	static LONG wrapped;
	(void)sdk;
	ime_detach_process();
	savestate_hooks_install();
	ensure_dev_vtbl();
	if (InterlockedCompareExchange(&wrapped, 1, 0) == 0)
		d3d9_trace_wrap_d3d((void **)&kD3DVtbl, sizeof(kD3DVtbl));
	sw_trace("Direct3DCreate9 sdk=%u", sdk);
	log_env_once();
	d = (SwD3D9 *)calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	d->iface.lpVtbl = &kD3DVtbl;
	d->ref = 1;
	return &d->iface;
}

/* test.exe --dump uses this to read pixels without going through COM. */
__declspec(dllexport) int WINAPI d3d9_sw_dump_device(IDirect3DDevice9 *dev, const char *path)
{
	if (!dev || !path)
		return 0;
	return swrast_dump_tga(&dev_from(dev)->rast, path);
}
