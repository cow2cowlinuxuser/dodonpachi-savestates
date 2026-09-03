#include "swrast.h"

#include <d3d9types.h>
#include <immintrin.h>
#include <intrin.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "swalloc.h"

static int iclamp(int v, int lo, int hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

/* Real D3D9 never rasterises from floats: it quantises post-viewport x/y onto
 * a fixed-point sub-pixel grid before edge setup, and the spec guarantees at
 * least four fractional bits. That step is what makes rasterisation
 * watertight. Two abutting quads whose shared edge differs by a hair in float
 * land on the same grid value, so the edge is genuinely shared and the
 * top-left rule can award the pixel to exactly one of them. Left in raw float
 * the sliver between them belongs to neither, and a pixel centre landing there
 * is written by neither quad, which is what draws the one-pixel gaps through
 * translucent overlays. No fill rule can close those; only snapping can. */
static float subpixel_grid(void)
{
	static float cached = -1.0f;
	if (cached < 0.0f) {
		const char *s = getenv("D3D9SW_SUBPIXEL");
		int bits = s ? atoi(s) : 4;
		if (bits < 0)
			bits = 0;
		if (bits > 8)
			bits = 8;
		cached = bits ? (float)(1u << bits) : 0.0f;
	}
	return cached;
}

/* Diagnostic only: records which draw call last wrote each pixel. A seam pixel
 * whose owner differs from its neighbours' sits on a boundary between two
 * draws; one whose owner matches means a single draw painted the line itself,
 * which points at texture sampling rather than coverage. Allocated only when
 * D3D9SW_DRAWID is set, so the cost is a predictable null check per pixel. */
#define SWRAST_ID_TARGETS 4
struct SwIdBuf {
	const void *color;
	int w, h;
	uint32_t *px;
};
static struct SwIdBuf g_idbufs[SWRAST_ID_TARGETS];
static int g_idbuf_count;
static unsigned g_draw_seq;

/* The frame is built across several render targets, so the owner map has to
 * follow each one separately; a single buffer only ever retained whichever
 * target happened to be current last. */
static uint32_t *idbuf_lookup(const void *color, int w, int h)
{
	int i;
	for (i = 0; i < g_idbuf_count; i++) {
		if (g_idbufs[i].color == color && g_idbufs[i].w == w && g_idbufs[i].h == h)
			return g_idbufs[i].px;
	}
	return NULL;
}

static void snap_vert(SwVert *v, float grid)
{
	if (grid <= 0.0f)
		return;
	v->x = floorf(v->x * grid + 0.5f) / grid;
	v->y = floorf(v->y * grid + 0.5f) / grid;
}

static float orient2d(float ax, float ay, float bx, float by, float cx, float cy)
{
	return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

/* Exact x/255 for the 0..65535 range these blends produce. */
static int div255(int v)
{
	v += 128;
	return (v + (v >> 8)) >> 8;
}

static uint32_t lerp_color(uint32_t ca, uint32_t cb, uint32_t cc, float w0, float w1, float w2)
{
	float ia, ib, ic, t;
	int ch, out = 0;
	ia = w0 / (w0 + w1 + w2);
	ib = w1 / (w0 + w1 + w2);
	ic = w2 / (w0 + w1 + w2);
	for (ch = 0; ch < 4; ch++) {
		int shift = ch * 8;
		t = ia * (float)((ca >> shift) & 255) + ib * (float)((cb >> shift) & 255) +
		    ic * (float)((cc >> shift) & 255);
		if (t < 0.0f)
			t = 0.0f;
		if (t > 255.0f)
			t = 255.0f;
		out |= ((int)(t + 0.5f) & 255) << shift;
	}
	return (uint32_t)out;
}

static int wrap_coord(int x, int n)
{
	if (n <= 0)
		return 0;
	x %= n;
	if (x < 0)
		x += n;
	return x;
}

/* D3DTEXTUREADDRESS: 1 WRAP, 2 MIRROR, 3 CLAMP, 4 BORDER, 5 MIRRORONCE */
static int addr_coord(int x, int n, int mode)
{
	if (n <= 0)
		return 0;
	/* Every atlas here is power-of-two, and wrapping is the common mode, so
	 * take the mask before falling into the general modulo path. */
	if (mode == D3DTADDRESS_WRAP && !(n & (n - 1)))
		return x & (n - 1);
	switch (mode) {
	case D3DTADDRESS_CLAMP:
	case D3DTADDRESS_BORDER:
		return iclamp(x, 0, n - 1);
	case D3DTADDRESS_MIRROR: {
		int period = 2 * n;
		int m = x % period;
		if (m < 0)
			m += period;
		return (m < n) ? m : (period - 1 - m);
	}
	case D3DTADDRESS_MIRRORONCE:
		return iclamp(x < 0 ? -x : x, 0, n - 1);
	default:
		return wrap_coord(x, n);
	}
}

static uint32_t tex_fetch(const SwTex *t, int x, int y, int au, int av)
{
	x = addr_coord(x, t->width, au);
	y = addr_coord(y, t->height, av);
	return t->pixels[y * t->width + x];
}

/* Weighted average of two packed pixels, t in 0..256. Both 8-bit channels of
 * each half-plane ride in one 32-bit word; the weights sum to 256, so no field
 * can reach 65536 and carry into its neighbour. The AVX2 kernel performs the
 * identical arithmetic, which is what lets the two paths agree bit for bit. */
static uint32_t lerp8_fixed(uint32_t a, uint32_t b, uint32_t t)
{
	uint32_t it = 256u - t;
	uint32_t arb = a & 0x00ff00ffu, aag = (a >> 8) & 0x00ff00ffu;
	uint32_t brb = b & 0x00ff00ffu, bag = (b >> 8) & 0x00ff00ffu;
	uint32_t rb = ((arb * it + brb * t + 0x00800080u) >> 8) & 0x00ff00ffu;
	uint32_t ag = ((aag * it + bag * t + 0x00800080u) >> 8) & 0x00ff00ffu;
	return rb | (ag << 8);
}

static uint32_t sample_tex(const SwTex *t, float u, float v, int bilinear, int au, int av)
{
	float fu, fv;
	/* Address wrapping happens on the integer texel, not here: folding the
	 * float into [0,1) first would turn every addressing mode into WRAP. */
	if (!(u > -1.0e6f && u < 1.0e6f))
		u = 0.0f;
	if (!(v > -1.0e6f && v < 1.0e6f))
		v = 0.0f;
	fu = u * (float)t->width;
	fv = v * (float)t->height;
	if (!bilinear)
		return tex_fetch(t, (int)floorf(fu), (int)floorf(fv), au, av);
	fu -= 0.5f;
	fv -= 0.5f;
	{
		int x0 = (int)floorf(fu);
		int y0 = (int)floorf(fv);
		uint32_t tx = (uint32_t)(int)((fu - (float)x0) * 256.0f);
		uint32_t ty = (uint32_t)(int)((fv - (float)y0) * 256.0f);
		uint32_t c00 = tex_fetch(t, x0, y0, au, av);
		uint32_t c10 = tex_fetch(t, x0 + 1, y0, au, av);
		uint32_t c01 = tex_fetch(t, x0, y0 + 1, au, av);
		uint32_t c11 = tex_fetch(t, x0 + 1, y0 + 1, au, av);
		return lerp8_fixed(lerp8_fixed(c00, c10, tx), lerp8_fixed(c01, c11, tx), ty);
	}
}

static uint32_t modulate(uint32_t a, uint32_t b)
{
	uint32_t out = 0;
	int ch;
	for (ch = 0; ch < 4; ch++) {
		int shift = ch * 8;
		int v = div255((int)((a >> shift) & 255) * (int)((b >> shift) & 255));
		out |= (v & 255) << shift;
	}
	return out;
}

/* Steepens the alpha ramp about its halfway point, leaving the colour alone.
 *
 * A distance field encodes the glyph edge at 0.5 and ramps across several texels
 * either side of it, so once a glyph is magnified there is no sharp edge left in
 * the data to sample. Keeping the ramp draws a soft blob and cutting it at 0.5
 * draws a hard silhouette that loses a fraction of a pixel everywhere the edge
 * falls between samples. Multiplying the distance from 0.5 by the magnification
 * puts the transition back inside one pixel, which is the coverage an
 * antialiased glyph wants. */
static uint32_t sharpen_alpha(uint32_t col, int k)
{
	int a = (int)((col >> 24) & 255);
	a = 128 + (((a - 128) * k) >> 8);
	if (a < 0)
		a = 0;
	else if (a > 255)
		a = 255;
	return (col & 0x00ffffffu) | ((uint32_t)a << 24);
}

/* Per-channel D3DBLEND factor in 0..255. ch is the byte index (0=B,1=G,2=R,3=A). */
static int blend_factor(int mode, int ch, uint32_t src, uint32_t dst, uint32_t bf)
{
	int shift = ch * 8;
	int sc = (int)((src >> shift) & 255);
	int dc = (int)((dst >> shift) & 255);
	int sa = (int)((src >> 24) & 255);
	int da = (int)((dst >> 24) & 255);
	switch (mode) {
	case D3DBLEND_ZERO:
		return 0;
	case D3DBLEND_SRCCOLOR:
		return sc;
	case D3DBLEND_INVSRCCOLOR:
		return 255 - sc;
	case D3DBLEND_SRCALPHA:
		return sa;
	case D3DBLEND_INVSRCALPHA:
		return 255 - sa;
	case D3DBLEND_DESTALPHA:
		return da;
	case D3DBLEND_INVDESTALPHA:
		return 255 - da;
	case D3DBLEND_DESTCOLOR:
		return dc;
	case D3DBLEND_INVDESTCOLOR:
		return 255 - dc;
	case D3DBLEND_SRCALPHASAT: {
		int f = 255 - da;
		return (ch == 3) ? 255 : (sa < f ? sa : f);
	}
	case D3DBLEND_BLENDFACTOR:
		return (int)((bf >> shift) & 255);
	case D3DBLEND_INVBLENDFACTOR:
		return 255 - (int)((bf >> shift) & 255);
	case D3DBLEND_BOTHSRCALPHA:
		return (ch == 3) ? sa : sa;
	case D3DBLEND_BOTHINVSRCALPHA:
		return 255 - sa;
	default: /* D3DBLEND_ONE and anything unrecognised */
		return 255;
	}
}

/* src*srcalpha + dst*(1-srcalpha), all four channels, no per-channel dispatch. */
static uint32_t blend_over(uint32_t src, uint32_t dst, uint32_t sa)
{
	uint32_t isa = 255u - sa;
	uint32_t sag = src & 0x00ff00ffu, dag = dst & 0x00ff00ffu;
	uint32_t srb = (src >> 8) & 0x00ff00ffu, drb = (dst >> 8) & 0x00ff00ffu;
	uint32_t lo = (sag * sa + dag * isa + 0x00800080u);
	uint32_t hi = (srb * sa + drb * isa + 0x00800080u);
	lo = ((lo + ((lo >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
	hi = ((hi + ((hi >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
	return lo | (hi << 8);
}

/* src*srcalpha + dst, saturating. */
static uint32_t blend_add(uint32_t src, uint32_t dst, uint32_t sa)
{
	uint32_t out = 0;
	int ch;
	for (ch = 0; ch < 4; ch++) {
		int shift = ch * 8;
		int v = div255((int)((src >> shift) & 255) * (int)sa) +
			(int)((dst >> shift) & 255);
		out |= (uint32_t)(v > 255 ? 255 : v) << shift;
	}
	return out;
}

static uint32_t blend_pixel(uint32_t src, uint32_t dst, const SwState *st)
{
	uint32_t out = 0;
	int ch;
	for (ch = 0; ch < 4; ch++) {
		int shift = ch * 8;
		int sc = (int)((src >> shift) & 255);
		int dc = (int)((dst >> shift) & 255);
		int sf = blend_factor(st->src_blend, ch, src, dst, st->blend_factor);
		int df = blend_factor(st->dst_blend, ch, src, dst, st->blend_factor);
		int s = div255(sc * sf);
		int d = div255(dc * df);
		int v;
		switch (st->blend_op) {
		case D3DBLENDOP_SUBTRACT:
			v = s - d;
			break;
		case D3DBLENDOP_REVSUBTRACT:
			v = d - s;
			break;
		case D3DBLENDOP_MIN:
			v = sc < dc ? sc : dc;
			break;
		case D3DBLENDOP_MAX:
			v = sc > dc ? sc : dc;
			break;
		default:
			v = s + d;
			break;
		}
		out |= (uint32_t)iclamp(v, 0, 255) << shift;
	}
	return out;
}

/* D3DCMPFUNC: 1 NEVER .. 8 ALWAYS */
static int cmp_func(int func, float a, float b)
{
	switch (func) {
	case D3DCMP_NEVER:
		return 0;
	case D3DCMP_LESS:
		return a < b;
	case D3DCMP_EQUAL:
		return a == b;
	case D3DCMP_LESSEQUAL:
		return a <= b;
	case D3DCMP_GREATER:
		return a > b;
	case D3DCMP_NOTEQUAL:
		return a != b;
	case D3DCMP_GREATEREQUAL:
		return a >= b;
	default:
		return 1;
	}
}

void swrast_state_defaults(SwState *s)
{
	if (!s)
		return;
	memset(s, 0, sizeof(*s));
	s->z_func = D3DCMP_LESSEQUAL;
	s->src_blend = D3DBLEND_ONE;
	s->dst_blend = D3DBLEND_ZERO;
	s->blend_op = D3DBLENDOP_ADD;
	s->alpha_func = D3DCMP_ALWAYS;
	s->cull = D3DCULL_NONE;
	s->addr_u = D3DTADDRESS_WRAP;
	s->addr_v = D3DTADDRESS_WRAP;
	s->write_mask = 0xffffffffu;
}

/* Zero for an inclusive (top or left) edge, otherwise a nudge small enough to
 * exclude only pixels whose centre sits exactly on the edge. */
static float edge_bias(float dwdx, float dwdy)
{
	if (dwdx > 0.0f || (dwdx == 0.0f && dwdy > 0.0f))
		return 0.0f;
	return 1.0e-4f * (fabsf(dwdx) + fabsf(dwdy)) + 1.0e-30f;
}

/* Narrow a row's x range to where one edge function is non-negative. Solving
 * per row beats testing every pixel: for the axis-aligned quads this game is
 * built from, each triangle's bounding box is twice its own area, so half the
 * walk was landing outside the triangle. */
static void span_clip(float wrow, float d, int minx, int maxx, int *xs, int *xe)
{
	float span = (float)(maxx - minx) + 1.0f;
	float t;
	if (d > 0.0f) {
		t = -wrow / d; /* need (x - minx) >= t */
		if (!(t <= span)) {
			*xs = 1; /* whole row is outside, or t is NaN/huge */
			*xe = 0;
		} else if (t > 0.0f) {
			int lo = minx + (int)ceilf(t);
			if (lo > *xs)
				*xs = lo;
		}
	} else if (d < 0.0f) {
		t = -wrow / d; /* need (x - minx) <= t */
		if (!(t >= 0.0f)) {
			*xs = 1;
			*xe = 0;
		} else if (t < span) {
			int hi = minx + (int)floorf(t);
			if (hi < *xe)
				*xe = hi;
		}
	} else if (wrow < 0.0f) {
		*xs = 1;
		*xe = 0;
	}
}

enum {
	CPU_SSE2 = 1,
	CPU_SSE41 = 2,
	CPU_AVX2 = 4,
	CPU_AVX512 = 8
};
int swrast_cpu_features(void);

/* Cleared to force the scalar reference path, both for the A/B test harness and
 * as an escape hatch if the kernel ever misbehaves in the field. */
int swrast_simd_enable = 1;
int swrast_gather_mode = -1;

enum { GATHER_AVX2 = 0, GATHER_EVEX = 1, GATHER_INSERT = 2 };

static int simd_enabled(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *v = getenv("D3D9SW_NOSIMD");
		cached = (v && *v && *v != '0') ? 0 : 1;
	}
	return cached && swrast_simd_enable;
}

/* 32-bit mode only has ymm0–ymm7 / zmm0–zmm7. 512-bit EVEX would spill 64-byte
 * ZMMs for a bilinear span; 256-bit EVEX keeps the mask registers and the
 * masked gather/store encodings without that. Auto picks EVEX when VL is
 * present. D3D9SW_GATHER=avx2|evex|insert overrides. */
static int gather_mode(void)
{
	int m = swrast_gather_mode;
	if (m >= 0)
		return m;
	{
		static int cached = -2;
		if (cached == -2) {
			const char *v = getenv("D3D9SW_GATHER");
			if (v && (v[0] == 'i' || v[0] == 'I'))
				cached = GATHER_INSERT;
			else if (v && (v[0] == 'e' || v[0] == 'E'))
				cached = GATHER_EVEX;
			else if (v && v[0] == '2')
				cached = GATHER_AVX2;
			else
				cached = (swrast_cpu_features() & CPU_AVX512) ? GATHER_EVEX
									     : GATHER_AVX2;
		}
		return cached;
	}
}

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#define SWRAST_X86 1
#include <immintrin.h>
#endif

#ifdef SWRAST_X86
/* Frame profiling says a single pixel configuration covers most of a frame:
 * textured from a power-of-two atlas with WRAP addressing, one constant vertex
 * colour, alpha tested, source-over or additive blended, and no depth buffer.
 * 256-bit is the native width in the 32-bit Steam DLL (only 8 vector regs). */
typedef struct SwSpan {
	uint32_t *crow;
	const uint32_t *texels;
	int tw_mask, th_mask, tw_shift;
	/* Set when texel addressing must clamp and index by row stride rather
	 * than mask and shift; tw_shift and the masks are then unused. */
	int clamp_idx, tw, th, stride;
	float tw_f, th_f;
	int bilinear, persp, white, seq_u;
	uint32_t flat;
	int alpha_test, alpha_func, alpha_ref;
	int blend_over, blend_add;
	float w0, w1, w2, dw0, dw1, dw2;
	float u, v, du, dv, iw, diw;
} SwSpan;

/* Only the addressing and blend modes the fast path claims to handle. */
static int span_kernel_ok(const SwTex *t, int addr_u, int addr_v, int uv_in_bounds)
{
	if (!t || !t->pixels || t->width <= 0 || t->height <= 0)
		return 0;
	/* Addressing only has to be emulated where a coordinate can actually
	 * leave the texture. Once the caller has ruled that out, every mode
	 * behaves alike and arbitrary dimensions index by row stride. */
	if (uv_in_bounds)
		return 1;
	/* Clamping is expressible in the vector index path for any dimensions,
	 * and the scalar sampler resolves BORDER to a clamp too. This is what
	 * lets a full-surface upscale blit off a non-power-of-two target take the
	 * fast path: its coordinates reach the very edge, so no in-bounds
	 * argument applies, but clamping is still the correct answer there. */
	if ((addr_u == D3DTADDRESS_CLAMP || addr_u == D3DTADDRESS_BORDER) &&
	    (addr_v == D3DTADDRESS_CLAMP || addr_v == D3DTADDRESS_BORDER))
		return 1;
	if ((t->width & (t->width - 1)) || (t->height & (t->height - 1)))
		return 0;
	if (addr_u != D3DTADDRESS_WRAP || addr_v != D3DTADDRESS_WRAP)
		return 0;
	return 1;
}

static int log2i(int v)
{
	int n = 0;
	while ((1 << n) < v)
		n++;
	return n;
}

__attribute__((target("avx2")))
static __m256i span_idx(const SwSpan *s, __m256i xi, __m256i yi)
{
	/* Clamp into range and scale by the real row stride, which works for any
	 * dimensions. This is exactly what the scalar sampler does for CLAMP and
	 * BORDER (iclamp to 0..n-1), so the two paths agree bit for bit. */
	if (s->clamp_idx) {
		__m256i zero = _mm256_setzero_si256();
		xi = _mm256_min_epi32(_mm256_max_epi32(xi, zero),
				      _mm256_set1_epi32(s->tw - 1));
		yi = _mm256_min_epi32(_mm256_max_epi32(yi, zero),
				      _mm256_set1_epi32(s->th - 1));
		return _mm256_add_epi32(
			_mm256_mullo_epi32(yi, _mm256_set1_epi32(s->stride)), xi);
	}
	return _mm256_add_epi32(
		_mm256_slli_epi32(_mm256_and_si256(yi, _mm256_set1_epi32(s->th_mask)),
				  s->tw_shift),
		_mm256_and_si256(xi, _mm256_set1_epi32(s->tw_mask)));
}

__attribute__((noinline, target("avx2")))
static __m256i span_gather_avx2(const SwSpan *s, __m256i xi, __m256i yi, __m256i live)
{
	return _mm256_mask_i32gather_epi32(_mm256_setzero_si256(), (const int *)s->texels,
					   span_idx(s, xi, yi), live, 4);
}

/* AVX2 vpgatherdd is microcoded. On several CPUs eight scalar loads beat it. */
__attribute__((noinline, target("avx2")))
static __m256i span_gather_insert(const SwSpan *s, __m256i xi, __m256i yi, __m256i live)
{
	__m256i idx = span_idx(s, xi, yi);
	int i, m, id[8];
	uint32_t out[8];
	_mm256_storeu_si256((__m256i *)id, idx);
	m = _mm256_movemask_ps(_mm256_castsi256_ps(live));
	for (i = 0; i < 8; i++)
		out[i] = (m & (1 << i)) ? s->texels[id[i]] : 0;
	return _mm256_loadu_si256((const __m256i *)out);
}

__attribute__((noinline, target("avx512f,avx512vl")))
static __m256i span_gather_evex(const SwSpan *s, __m256i xi, __m256i yi, __m256i live)
{
	__mmask8 k = (__mmask8)_mm256_movemask_ps(_mm256_castsi256_ps(live));
	return _mm256_mmask_i32gather_epi32(_mm256_setzero_si256(), k, span_idx(s, xi, yi),
					    s->texels, 4);
}

__attribute__((target("avx2")))
static __m256i span_gather(const SwSpan *s, __m256i xi, __m256i yi, __m256i live)
{
	int mode = gather_mode();
	if (mode == GATHER_EVEX)
		return span_gather_evex(s, xi, yi, live);
	if (mode == GATHER_INSERT)
		return span_gather_insert(s, xi, yi, live);
	return span_gather_avx2(s, xi, yi, live);
}

/* Weighted average of two packed-byte vectors, weight in 0..256 per 16-bit
 * lane. Operands must already be split into the 0x00ff00ff half-planes. */
__attribute__((target("avx2")))
static __m256i span_lerp8(__m256i a, __m256i b, __m256i t)
{
	__m256i it = _mm256_sub_epi16(_mm256_set1_epi16(256), t);
	__m256i v = _mm256_add_epi16(_mm256_mullo_epi16(a, it), _mm256_mullo_epi16(b, t));
	return _mm256_srli_epi16(_mm256_add_epi16(v, _mm256_set1_epi16(128)), 8);
}

__attribute__((target("avx2")))
static __m256i span_loadu_wrap(const SwSpan *s, int x0, int y0)
{
	x0 &= s->tw_mask;
	y0 &= s->th_mask;
	/* 8 consecutive texels that do not wrap: one unaligned load. */
	if (x0 <= s->tw_mask - 7)
		return _mm256_loadu_si256(
			(const __m256i *)(s->texels + ((size_t)y0 << s->tw_shift) + x0));
	{
		uint32_t tmp[8];
		int k, row = y0 << s->tw_shift;
		for (k = 0; k < 8; k++)
			tmp[k] = s->texels[row + ((x0 + k) & s->tw_mask)];
		return _mm256_loadu_si256((const __m256i *)tmp);
	}
}

__attribute__((target("avx2")))
static __m256i span_sample(const SwSpan *s, __m256 u, __m256 v, __m256i live)
{
	const __m256i lomask = _mm256_set1_epi32(0x00ff00ff);
	__m256 fu, fv;
	/* Same guard as the scalar sampler: anything wild (including NaN) samples
	 * texel zero rather than producing an out-of-range gather index. */
	__m256 lim = _mm256_set1_ps(1.0e6f);
	u = _mm256_and_ps(u, _mm256_cmp_ps(_mm256_andnot_ps(_mm256_set1_ps(-0.0f), u), lim,
					   _CMP_LT_OQ));
	v = _mm256_and_ps(v, _mm256_cmp_ps(_mm256_andnot_ps(_mm256_set1_ps(-0.0f), v), lim,
					   _CMP_LT_OQ));
	fu = _mm256_mul_ps(u, _mm256_set1_ps(s->tw_f));
	fv = _mm256_mul_ps(v, _mm256_set1_ps(s->th_f));

	/* dudx*width == 1 and dvdx == 0: the 8 texels are consecutive in a row,
	 * so a vector load replaces a gather. Bilinear still needs the +1
	 * neighbour, which is the same load shifted by one texel. */
	if (s->seq_u && !s->persp) {
		if (!s->bilinear) {
			int x0 = _mm256_extract_epi32(_mm256_cvtps_epi32(_mm256_floor_ps(fu)), 0);
			int y0 = _mm256_extract_epi32(_mm256_cvtps_epi32(_mm256_floor_ps(fv)), 0);
			return span_loadu_wrap(s, x0, y0);
		}
		fu = _mm256_sub_ps(fu, _mm256_set1_ps(0.5f));
		fv = _mm256_sub_ps(fv, _mm256_set1_ps(0.5f));
		{
			__m256 ffu = _mm256_floor_ps(fu), ffv = _mm256_floor_ps(fv);
			int x0 = _mm256_extract_epi32(_mm256_cvtps_epi32(ffu), 0);
			int y0 = _mm256_extract_epi32(_mm256_cvtps_epi32(ffv), 0);
			__m256i c00 = span_loadu_wrap(s, x0, y0);
			__m256i c10 = span_loadu_wrap(s, x0 + 1, y0);
			__m256i c01 = span_loadu_wrap(s, x0, y0 + 1);
			__m256i c11 = span_loadu_wrap(s, x0 + 1, y0 + 1);
			__m256i tx = _mm256_cvttps_epi32(
				_mm256_mul_ps(_mm256_sub_ps(fu, ffu), _mm256_set1_ps(256.0f)));
			__m256i ty = _mm256_cvttps_epi32(
				_mm256_mul_ps(_mm256_sub_ps(fv, ffv), _mm256_set1_ps(256.0f)));
			__m256i txw = _mm256_or_si256(tx, _mm256_slli_epi32(tx, 16));
			__m256i tyw = _mm256_or_si256(ty, _mm256_slli_epi32(ty, 16));
			__m256i top_rb = span_lerp8(_mm256_and_si256(c00, lomask),
						    _mm256_and_si256(c10, lomask), txw);
			__m256i bot_rb = span_lerp8(_mm256_and_si256(c01, lomask),
						    _mm256_and_si256(c11, lomask), txw);
			__m256i top_ag = span_lerp8(
				_mm256_and_si256(_mm256_srli_epi32(c00, 8), lomask),
				_mm256_and_si256(_mm256_srli_epi32(c10, 8), lomask), txw);
			__m256i bot_ag = span_lerp8(
				_mm256_and_si256(_mm256_srli_epi32(c01, 8), lomask),
				_mm256_and_si256(_mm256_srli_epi32(c11, 8), lomask), txw);
			__m256i rb = span_lerp8(top_rb, bot_rb, tyw);
			__m256i ag = span_lerp8(top_ag, bot_ag, tyw);
			return _mm256_or_si256(_mm256_and_si256(rb, lomask),
					       _mm256_slli_epi32(_mm256_and_si256(ag, lomask), 8));
		}
	}

	if (!s->bilinear)
		return span_gather(s, _mm256_cvtps_epi32(_mm256_floor_ps(fu)),
				   _mm256_cvtps_epi32(_mm256_floor_ps(fv)), live);

	fu = _mm256_sub_ps(fu, _mm256_set1_ps(0.5f));
	fv = _mm256_sub_ps(fv, _mm256_set1_ps(0.5f));
	{
		__m256 ffu = _mm256_floor_ps(fu), ffv = _mm256_floor_ps(fv);
		__m256i x0 = _mm256_cvtps_epi32(ffu), y0 = _mm256_cvtps_epi32(ffv);
		__m256i x1 = _mm256_add_epi32(x0, _mm256_set1_epi32(1));
		__m256i y1 = _mm256_add_epi32(y0, _mm256_set1_epi32(1));
		/* 0..256 fixed point; truncating matches the scalar cast exactly. */
		__m256i tx = _mm256_cvttps_epi32(
			_mm256_mul_ps(_mm256_sub_ps(fu, ffu), _mm256_set1_ps(256.0f)));
		__m256i ty = _mm256_cvttps_epi32(
			_mm256_mul_ps(_mm256_sub_ps(fv, ffv), _mm256_set1_ps(256.0f)));
		__m256i c00 = span_gather(s, x0, y0, live);
		__m256i c10 = span_gather(s, x1, y0, live);
		__m256i c01 = span_gather(s, x0, y1, live);
		__m256i c11 = span_gather(s, x1, y1, live);
		/* Broadcast each 32-bit weight across the two 16-bit lanes it governs. */
		__m256i txw = _mm256_or_si256(tx, _mm256_slli_epi32(tx, 16));
		__m256i tyw = _mm256_or_si256(ty, _mm256_slli_epi32(ty, 16));
		__m256i top_rb = span_lerp8(_mm256_and_si256(c00, lomask),
					    _mm256_and_si256(c10, lomask), txw);
		__m256i bot_rb = span_lerp8(_mm256_and_si256(c01, lomask),
					    _mm256_and_si256(c11, lomask), txw);
		__m256i top_ag = span_lerp8(
			_mm256_and_si256(_mm256_srli_epi32(c00, 8), lomask),
			_mm256_and_si256(_mm256_srli_epi32(c10, 8), lomask), txw);
		__m256i bot_ag = span_lerp8(
			_mm256_and_si256(_mm256_srli_epi32(c01, 8), lomask),
			_mm256_and_si256(_mm256_srli_epi32(c11, 8), lomask), txw);
		__m256i rb = span_lerp8(top_rb, bot_rb, tyw);
		__m256i ag = span_lerp8(top_ag, bot_ag, tyw);
		return _mm256_or_si256(_mm256_and_si256(rb, lomask),
				       _mm256_slli_epi32(_mm256_and_si256(ag, lomask), 8));
	}
}

/* div255(a*b) per channel, the vector twin of modulate(). */
__attribute__((target("avx2")))
static __m256i span_modulate(__m256i col, uint32_t flat)
{
	const __m256i lomask = _mm256_set1_epi32(0x00ff00ff);
	__m256i f = _mm256_set1_epi32((int)flat);
	__m256i crb = _mm256_and_si256(col, lomask);
	__m256i cag = _mm256_and_si256(_mm256_srli_epi32(col, 8), lomask);
	__m256i frb = _mm256_and_si256(f, lomask);
	__m256i fag = _mm256_and_si256(_mm256_srli_epi32(f, 8), lomask);
	__m256i rb = _mm256_add_epi16(_mm256_mullo_epi16(crb, frb), _mm256_set1_epi16(128));
	__m256i ag = _mm256_add_epi16(_mm256_mullo_epi16(cag, fag), _mm256_set1_epi16(128));
	rb = _mm256_srli_epi16(_mm256_add_epi16(rb, _mm256_srli_epi16(rb, 8)), 8);
	ag = _mm256_srli_epi16(_mm256_add_epi16(ag, _mm256_srli_epi16(ag, 8)), 8);
	return _mm256_or_si256(_mm256_and_si256(rb, lomask),
			       _mm256_slli_epi32(_mm256_and_si256(ag, lomask), 8));
}

__attribute__((target("avx2")))
static __m256i span_blend_over(__m256i src, __m256i dst, __m256i sa)
{
	const __m256i lomask = _mm256_set1_epi32(0x00ff00ff);
	/* sa + isa == 255 keeps every 16-bit lane below 65536. */
	__m256i saw = _mm256_or_si256(sa, _mm256_slli_epi32(sa, 16));
	__m256i isaw = _mm256_sub_epi16(_mm256_set1_epi16(255), saw);
	__m256i srb = _mm256_and_si256(src, lomask);
	__m256i sag = _mm256_and_si256(_mm256_srli_epi32(src, 8), lomask);
	__m256i drb = _mm256_and_si256(dst, lomask);
	__m256i dag = _mm256_and_si256(_mm256_srli_epi32(dst, 8), lomask);
	__m256i rb = _mm256_add_epi16(
		_mm256_add_epi16(_mm256_mullo_epi16(srb, saw), _mm256_mullo_epi16(drb, isaw)),
		_mm256_set1_epi16(128));
	__m256i ag = _mm256_add_epi16(
		_mm256_add_epi16(_mm256_mullo_epi16(sag, saw), _mm256_mullo_epi16(dag, isaw)),
		_mm256_set1_epi16(128));
	rb = _mm256_srli_epi16(_mm256_add_epi16(rb, _mm256_srli_epi16(rb, 8)), 8);
	ag = _mm256_srli_epi16(_mm256_add_epi16(ag, _mm256_srli_epi16(ag, 8)), 8);
	return _mm256_or_si256(_mm256_and_si256(rb, lomask),
			       _mm256_slli_epi32(_mm256_and_si256(ag, lomask), 8));
}

/* src*srcAlpha + dst, saturating. Matches blend_add() including div255 rounding. */
__attribute__((target("avx2")))
static __m256i span_blend_add(__m256i src, __m256i dst, __m256i sa)
{
	const __m256i lomask = _mm256_set1_epi32(0x00ff00ff);
	__m256i saw = _mm256_or_si256(sa, _mm256_slli_epi32(sa, 16));
	__m256i srb = _mm256_and_si256(src, lomask);
	__m256i sag = _mm256_and_si256(_mm256_srli_epi32(src, 8), lomask);
	__m256i rb = _mm256_add_epi16(_mm256_mullo_epi16(srb, saw), _mm256_set1_epi16(128));
	__m256i ag = _mm256_add_epi16(_mm256_mullo_epi16(sag, saw), _mm256_set1_epi16(128));
	rb = _mm256_srli_epi16(_mm256_add_epi16(rb, _mm256_srli_epi16(rb, 8)), 8);
	ag = _mm256_srli_epi16(_mm256_add_epi16(ag, _mm256_srli_epi16(ag, 8)), 8);
	{
		__m256i scaled = _mm256_or_si256(_mm256_and_si256(rb, lomask),
						 _mm256_slli_epi32(_mm256_and_si256(ag, lomask), 8));
		return _mm256_adds_epu8(scaled, dst);
	}
}

__attribute__((target("avx2")))
static __m256i span_alpha_pass(int func, __m256i alpha, int ref)
{
	__m256i r = _mm256_set1_epi32(ref);
	__m256i gt = _mm256_cmpgt_epi32(alpha, r);
	__m256i eq = _mm256_cmpeq_epi32(alpha, r);
	__m256i all = _mm256_set1_epi32(-1);
	switch (func) {
	case D3DCMP_NEVER:
		return _mm256_setzero_si256();
	case D3DCMP_LESS:
		return _mm256_andnot_si256(_mm256_or_si256(gt, eq), all);
	case D3DCMP_EQUAL:
		return eq;
	case D3DCMP_LESSEQUAL:
		return _mm256_andnot_si256(gt, all);
	case D3DCMP_GREATER:
		return gt;
	case D3DCMP_NOTEQUAL:
		return _mm256_andnot_si256(eq, all);
	case D3DCMP_GREATEREQUAL:
		return _mm256_or_si256(gt, eq);
	default: /* D3DCMP_ALWAYS */
		return all;
	}
}

__attribute__((noinline, target("avx512f,avx512vl")))
static void span_store_evex(uint32_t *dst, __m256i live, __m256i col)
{
	__mmask8 k = (__mmask8)_mm256_movemask_ps(_mm256_castsi256_ps(live));
	_mm256_mask_storeu_epi32(dst, k, col);
}

__attribute__((target("avx2")))
static void span_avx2(const SwSpan *s, int xs, int xe)
{
	const __m256 lane = _mm256_setr_ps(0, 1, 2, 3, 4, 5, 6, 7);
	const __m256i lanei = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
	int x, evex = gather_mode() == GATHER_EVEX;

	for (x = xs; x <= xe; x += 8) {
		__m256 k = _mm256_add_ps(_mm256_set1_ps((float)(x - xs)), lane);
		__m256i tail = _mm256_cmpgt_epi32(_mm256_set1_epi32(xe - x + 1), lanei);
		__m256 w0 = _mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(s->dw0), k), _mm256_set1_ps(s->w0));
		__m256 w1 = _mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(s->dw1), k), _mm256_set1_ps(s->w1));
		__m256 w2 = _mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(s->dw2), k), _mm256_set1_ps(s->w2));
		__m256 zero = _mm256_setzero_ps();
		__m256i cover = _mm256_castps_si256(_mm256_and_ps(
			_mm256_and_ps(_mm256_cmp_ps(w0, zero, _CMP_GE_OQ),
				      _mm256_cmp_ps(w1, zero, _CMP_GE_OQ)),
			_mm256_cmp_ps(w2, zero, _CMP_GE_OQ)));
		__m256i live = _mm256_and_si256(cover, tail);
		__m256 u, v;
		__m256i col, alpha;

		if (_mm256_testz_si256(live, live))
			continue;

		u = _mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(s->du), k), _mm256_set1_ps(s->u));
		v = _mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(s->dv), k), _mm256_set1_ps(s->v));
		if (s->persp) {
			__m256 iw = _mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(s->diw), k),
						  _mm256_set1_ps(s->iw));
			__m256 one = _mm256_set1_ps(1.0f);
			/* Match the scalar guard: a zero 1/w contributes q == 1. */
			__m256 nz = _mm256_cmp_ps(iw, zero, _CMP_NEQ_OQ);
			__m256 q = _mm256_blendv_ps(one, _mm256_div_ps(one, iw), nz);
			u = _mm256_mul_ps(u, q);
			v = _mm256_mul_ps(v, q);
		}

		col = span_sample(s, u, v, live);
		if (!s->white)
			col = span_modulate(col, s->flat);

		alpha = _mm256_srli_epi32(col, 24);
		if (s->alpha_test)
			live = _mm256_and_si256(live,
						span_alpha_pass(s->alpha_func, alpha, s->alpha_ref));

		if (s->blend_over) {
			/* Fully transparent writes nothing, fully opaque skips the
			 * read-modify-write entirely. */
			__m256i opaque = _mm256_cmpeq_epi32(alpha, _mm256_set1_epi32(255));
			live = _mm256_andnot_si256(
				_mm256_cmpeq_epi32(alpha, _mm256_setzero_si256()), live);
			if (!_mm256_testc_si256(opaque, live)) {
				__m256i dst = _mm256_maskload_epi32((const int *)(s->crow + x),
								    live);
				col = _mm256_blendv_epi8(span_blend_over(col, dst, alpha), col,
							 opaque);
			}
		} else if (s->blend_add) {
			live = _mm256_andnot_si256(
				_mm256_cmpeq_epi32(alpha, _mm256_setzero_si256()), live);
			if (!_mm256_testz_si256(live, live)) {
				__m256i dst = _mm256_maskload_epi32((const int *)(s->crow + x),
								    live);
				col = span_blend_add(col, dst, alpha);
			}
		}
		if (evex)
			span_store_evex(s->crow + x, live, col);
		else
			_mm256_maskstore_epi32((int *)(s->crow + x), live, col);
	}
}
#endif /* SWRAST_X86 */

enum { SR_OK, SR_NOAVX2, SR_NOTEX, SR_NOTFLAT, SR_DEPTH, SR_MASK, SR_BLEND, SR_NPOT,
       SR_ADDR, SR_OTHER, SR_N };

/* Unlike every other counter here, this one is reached from the tile workers, so
 * a single shared array would put eight cores on one cache line and serialise
 * them against each other. One padded slot per thread, summed on read.
 * SR_SLOTS must cover SWRAST_MAX_THREADS plus the calling thread. */
#define SR_SLOTS 33
#define SR_STRIDE 16 /* doubles: 128 bytes, so no slot shares a line */
static double g_simd_area[SR_SLOTS * SR_STRIDE];
/* 0 is the thread that drives the flush; workers claim 1..n. */
static __thread int g_worker_slot;

void swrast_prof_simd(double *out, int n)
{
	int i, s;
	for (i = 0; i < n && i < SR_N; i++) {
		double t = 0;
		for (s = 0; s < SR_SLOTS; s++) {
			t += g_simd_area[s * SR_STRIDE + i];
			g_simd_area[s * SR_STRIDE + i] = 0.0;
		}
		out[i] = t;
	}
}

static void triangle_rect(SwRast *r, SwVert a, SwVert b, SwVert c, const SwTex *tex,
			  const SwState *st, int clip_x0, int clip_x1, int clip_y0, int clip_y1,
			  unsigned draw)
{
	float area, inv_area, dw0dx, dw0dy, dw1dx, dw1dy, dw2dx, dw2dy;
	float w0_row, w1_row, w2_row;
	float au, av, bu, bv, cu, cv;
	float dudx, dudy, dvdx, dvdy, dzdx, dzdy, diwdx, diwdy;
	float u_row, v_row, z_row, iw_row;
	int minx, miny, maxx, maxy, x, y;
	int w, h;
	int persp, textured, flat_col, depth_test, depth_write;
	SwTex ltex;
	int l_bilinear, l_addr_u, l_addr_v, l_z_func, l_alpha_test, l_alpha_func;
	int l_alpha_ref, l_blend, l_blend_over, l_blend_add, l_white, l_alpha_sharpen;
#ifdef SWRAST_X86
	int use_simd = 0;
	SwSpan simd_span;
#endif
	uint32_t l_mask;
	uint32_t *idbase;

	if (!r->color)
		return;
	w = r->width;
	h = r->height;
	if (clip_x0 < 0)
		clip_x0 = 0;
	if (clip_x1 > w)
		clip_x1 = w;
	if (st->scissor_enable) {
		if (st->scissor_x0 > clip_x0)
			clip_x0 = st->scissor_x0;
		if (st->scissor_x1 < clip_x1)
			clip_x1 = st->scissor_x1;
		if (st->scissor_y0 > clip_y0)
			clip_y0 = st->scissor_y0;
		if (st->scissor_y1 < clip_y1)
			clip_y1 = st->scissor_y1;
	}
	if (clip_y0 < 0)
		clip_y0 = 0;
	if (clip_y1 > h)
		clip_y1 = h;
	if (clip_y0 >= clip_y1 || clip_x0 >= clip_x1)
		return;

	idbase = g_idbuf_count ? idbuf_lookup(r->color, r->width, r->height) : NULL;
	area = orient2d(a.x, a.y, b.x, b.y, c.x, c.y);
	if (area == 0.0f)
		return;
	/* Screen space is y-down, so a visually clockwise (front-facing) triangle
	 * has area > 0. D3DCULL_CCW therefore discards area < 0. */
	if (st->cull == D3DCULL_CCW && area < 0.0f)
		return;
	if (st->cull == D3DCULL_CW && area > 0.0f)
		return;
	if (area < 0.0f) {
		SwVert tmp = b;
		b = c;
		c = tmp;
		area = -area;
	}

	/* Only interpolate 1/w when every vertex carries a usable one and they
	 * actually differ; otherwise affine is both correct and cheaper. */
	persp = a.rhw > 0.0f && b.rhw > 0.0f && c.rhw > 0.0f &&
		(fabsf(a.rhw - b.rhw) > 1.0e-6f || fabsf(a.rhw - c.rhw) > 1.0e-6f);

	minx = iclamp((int)floorf(fminf(a.x, fminf(b.x, c.x))), clip_x0, clip_x1 - 1);
	maxx = iclamp((int)ceilf(fmaxf(a.x, fmaxf(b.x, c.x))), clip_x0, clip_x1 - 1);
	{
		int raw_miny = (int)floorf(fminf(a.y, fminf(b.y, c.y)));
		int raw_maxy = (int)ceilf(fmaxf(a.y, fmaxf(b.y, c.y)));
		if (raw_maxy < clip_y0 || raw_miny >= clip_y1)
			return;
		miny = iclamp(raw_miny, clip_y0, clip_y1 - 1);
		maxy = iclamp(raw_maxy, clip_y0, clip_y1 - 1);
	}
	if (miny > maxy || minx > maxx)
		return;

	dw0dx = -(c.y - b.y);
	dw0dy = (c.x - b.x);
	dw1dx = -(a.y - c.y);
	dw1dy = (a.x - c.x);
	dw2dx = -(b.y - a.y);
	dw2dy = (b.x - a.x);

	w0_row = orient2d(b.x, b.y, c.x, c.y, (float)minx + 0.5f, (float)miny + 0.5f);
	w1_row = orient2d(c.x, c.y, a.x, a.y, (float)minx + 0.5f, (float)miny + 0.5f);
	w2_row = orient2d(a.x, a.y, b.x, b.y, (float)minx + 0.5f, (float)miny + 0.5f);

	/* Top-left fill rule. Winding is normalised to area > 0 above, so an edge
	 * is top or left exactly when dwdx > 0, or dwdx == 0 with dwdy > 0. Those
	 * stay inclusive and the rest become exclusive; folding the bias into the
	 * row start keeps it free per pixel. Without this, a pixel lying exactly on
	 * an edge shared by two triangles satisfies >= 0 for both and gets blended
	 * twice, which is what draws seams down quad diagonals and along the joins
	 * between adjacent UI panels. */
	w0_row -= edge_bias(dw0dx, dw0dy);
	w1_row -= edge_bias(dw1dx, dw1dy);
	w2_row -= edge_bias(dw2dx, dw2dy);

	/* Every interpolant is linear in screen space, so step it per pixel
	 * instead of recomputing barycentrics (and three divides) each time. */
	inv_area = 1.0f / area;
	textured = tex && tex->pixels;
	flat_col = a.color == b.color && b.color == c.color;
	depth_test = st->z_enable && r->depth;
	depth_write = depth_test && st->z_write;
	/* Everything below is loop-invariant, but the compiler cannot prove that
	 * the framebuffer store does not alias the state struct, so without these
	 * copies it reloads each field for every pixel. */
	ltex = textured ? *tex : (SwTex){ 0 };
	l_bilinear = st->bilinear;
	l_addr_u = st->addr_u;
	l_addr_v = st->addr_v;
	l_z_func = st->z_func;
	l_alpha_test = st->alpha_test;
	l_alpha_func = st->alpha_func;
	l_alpha_ref = st->alpha_ref;
	l_alpha_sharpen = st->alpha_sharpen == 256 ? 0 : st->alpha_sharpen;
	l_blend = st->blend_enable;
	l_mask = st->write_mask;
	/* Alpha-over and additive cover essentially every sprite this game draws;
	 * the generic path costs eight switch dispatches per pixel. */
	l_blend_over = l_blend && st->blend_op == D3DBLENDOP_ADD &&
		       st->src_blend == D3DBLEND_SRCALPHA &&
		       st->dst_blend == D3DBLEND_INVSRCALPHA;
	l_blend_add = l_blend && st->blend_op == D3DBLENDOP_ADD &&
		      st->src_blend == D3DBLEND_SRCALPHA && st->dst_blend == D3DBLEND_ONE;
	/* An opaque white vertex colour makes the modulate a no-op. */
	l_white = flat_col && a.color == 0xffffffffu;
#ifdef SWRAST_X86
	use_simd = simd_enabled() && (swrast_cpu_features() & CPU_AVX2) && textured &&
		   flat_col &&
		   !depth_test && !depth_write && !idbase && l_mask == 0xffffffffu &&
		   (!l_blend || l_blend_over || l_blend_add) &&
		   /* Alpha sharpening is not in the vector kernel, and the two
		    * paths have to agree pixel for pixel. Only text asks for it,
		    * which the mix counters put at about one percent of area, so
		    * sending those draws down the scalar path costs nothing. */
		   !l_alpha_sharpen &&
		   span_kernel_ok(&ltex, l_addr_u, l_addr_v, st->uv_in_bounds);
	if (use_simd) {
		memset(&simd_span, 0, sizeof(simd_span));
		simd_span.texels = ltex.pixels;
		simd_span.tw_mask = ltex.width - 1;
		simd_span.th_mask = ltex.height - 1;
		simd_span.tw_shift = log2i(ltex.width);
		/* Mask and shift are only valid for a power-of-two surface under
		 * WRAP; everything else the gate now admits goes through the
		 * clamping stride path. */
		simd_span.clamp_idx = (ltex.width & (ltex.width - 1)) ||
				      (ltex.height & (ltex.height - 1)) ||
				      l_addr_u != D3DTADDRESS_WRAP ||
				      l_addr_v != D3DTADDRESS_WRAP;
		simd_span.tw = ltex.width;
		simd_span.th = ltex.height;
		simd_span.stride = ltex.width;
		simd_span.tw_f = (float)ltex.width;
		simd_span.th_f = (float)ltex.height;
		simd_span.bilinear = l_bilinear;
		simd_span.persp = persp;
		simd_span.white = l_white;
		simd_span.flat = a.color;
		simd_span.alpha_test = l_alpha_test;
		simd_span.alpha_func = l_alpha_func;
		simd_span.alpha_ref = l_alpha_ref;
		simd_span.blend_over = l_blend_over;
		simd_span.blend_add = l_blend_add;
		simd_span.dw0 = dw0dx;
		simd_span.dw1 = dw1dx;
		simd_span.dw2 = dw2dx;
	}
#endif
	if (persp) {
		au = a.u * a.rhw;
		av = a.v * a.rhw;
		bu = b.u * b.rhw;
		bv = b.v * b.rhw;
		cu = c.u * c.rhw;
		cv = c.v * c.rhw;
	} else {
		au = a.u;
		av = a.v;
		bu = b.u;
		bv = b.v;
		cu = c.u;
		cv = c.v;
	}
#define PLANE_DX(pa, pb, pc) (inv_area * (dw0dx * (pa) + dw1dx * (pb) + dw2dx * (pc)))
#define PLANE_DY(pa, pb, pc) (inv_area * (dw0dy * (pa) + dw1dy * (pb) + dw2dy * (pc)))
#define PLANE_AT(pa, pb, pc) (inv_area * (w0_row * (pa) + w1_row * (pb) + w2_row * (pc)))
	dudx = PLANE_DX(au, bu, cu);
	dudy = PLANE_DY(au, bu, cu);
	dvdx = PLANE_DX(av, bv, cv);
	dvdy = PLANE_DY(av, bv, cv);
	dzdx = PLANE_DX(a.z, b.z, c.z);
	dzdy = PLANE_DY(a.z, b.z, c.z);
	diwdx = PLANE_DX(a.rhw, b.rhw, c.rhw);
	diwdy = PLANE_DY(a.rhw, b.rhw, c.rhw);
	u_row = PLANE_AT(au, bu, cu);
	v_row = PLANE_AT(av, bv, cv);
	z_row = PLANE_AT(a.z, b.z, c.z);
	iw_row = PLANE_AT(a.rhw, b.rhw, c.rhw);
#undef PLANE_DX
#undef PLANE_DY
#undef PLANE_AT

#ifdef SWRAST_X86
	if (use_simd) {
		simd_span.du = dudx;
		simd_span.dv = dvdx;
		simd_span.diw = diwdx;
		/* Consecutive texels along the span: one vector load, no gather.
		 * This is 1:1 in U with constant V, not a full axis-aligned blit.
		 * The 88% mix counter was 1:1 in V only and does not imply this. */
		/* span_loadu_wrap is still shift-and-mask, so it must not be
		 * reached with arbitrary dimensions. Costs nothing here: this
		 * needs an exact 1:1 texel-to-pixel span, which the frame mix
		 * reports as never occurring in practice. */
		simd_span.seq_u = !simd_span.clamp_idx && !persp &&
				  fabsf(dvdx * simd_span.th_f) < 1.0e-4f &&
				  fabsf(dudx * simd_span.tw_f - 1.0f) < 1.0e-4f;
	}
#endif

	/* Which gate turned the vector kernel away, weighted by the clipped area
	 * at stake. Knowing the fast path was declined is useless without knowing
	 * why; the reasons are ordered so the first failure is reported. */
	{
		int reason;
#ifdef SWRAST_X86
		if (!simd_enabled() || !(swrast_cpu_features() & CPU_AVX2))
			reason = SR_NOAVX2;
		else if (!textured)
			reason = SR_NOTEX;
		else if (!flat_col)
			reason = SR_NOTFLAT;
		else if (depth_test || depth_write)
			reason = SR_DEPTH;
		else if (idbase || l_mask != 0xffffffffu)
			reason = SR_MASK;
		else if (l_blend && !l_blend_over && !l_blend_add)
			reason = SR_BLEND;
		else if (!use_simd &&
			 ((ltex.width & (ltex.width - 1)) || (ltex.height & (ltex.height - 1))))
			reason = SR_NPOT;
		else if (!use_simd)
			reason = SR_ADDR;
		else
			reason = use_simd ? SR_OK : SR_OTHER;
#else
		reason = SR_NOAVX2;
#endif
		{
			int slot = g_worker_slot;
			if (slot < 0 || slot >= SR_SLOTS)
				slot = 0;
			g_simd_area[slot * SR_STRIDE + reason] +=
				(double)(maxx - minx + 1) * (double)(maxy - miny + 1);
		}
	}

	for (y = miny; y <= maxy; y++) {
		uint32_t *crow = r->color + (size_t)y * w;
		uint32_t *idrow = idbase ? idbase + (size_t)y * w : NULL;
		float *drow = r->depth ? r->depth + (size_t)y * w : NULL;
		int xs = minx, xe = maxx;
		float off;
		float w0_xs, w1_xs, w2_xs, u_xs, v_xs, z_xs, iw_xs;
		float w0, w1, w2, uu, vv, zz, iw;

		span_clip(w0_row, dw0dx, minx, maxx, &xs, &xe);
		span_clip(w1_row, dw1dx, minx, maxx, &xs, &xe);
		span_clip(w2_row, dw2dx, minx, maxx, &xs, &xe);
		if (xs > xe)
			goto next_row; /* honour the empty marker before widening */
		/* One pixel of slack absorbs rounding in the divides above; the
		 * exact edge test inside the loop remains the authority. */
		xs--;
		xe++;
		if (xs < minx)
			xs = minx;
		if (xe > maxx)
			xe = maxx;
		if (xs > xe)
			goto next_row;
		off = (float)(xs - minx);
		w0_xs = w0_row + dw0dx * off;
		w1_xs = w1_row + dw1dx * off;
		w2_xs = w2_row + dw2dx * off;
		u_xs = u_row + dudx * off;
		v_xs = v_row + dvdx * off;
		z_xs = z_row + dzdx * off;
		iw_xs = iw_row + diwdx * off;
#ifdef SWRAST_X86
		if (use_simd) {
			simd_span.crow = crow;
			simd_span.w0 = w0_xs;
			simd_span.w1 = w1_xs;
			simd_span.w2 = w2_xs;
			simd_span.u = u_xs;
			simd_span.v = v_xs;
			simd_span.iw = iw_xs;
			span_avx2(&simd_span, xs, xe);
			goto next_row;
		}
#endif
		/* Recomputed from the span base rather than accumulated: stepping
		 * lets rounding drift along the span, which moves edge coverage by
		 * a pixel depending on where the span happened to start. The vector
		 * kernel indexes the same way, so the two paths agree exactly. */
		for (x = xs; x <= xe; x++) {
			float u, v;
			float kf = (float)(x - xs);
			uint32_t col;
			w0 = dw0dx * kf + w0_xs;
			w1 = dw1dx * kf + w1_xs;
			w2 = dw2dx * kf + w2_xs;
			uu = dudx * kf + u_xs;
			vv = dvdx * kf + v_xs;
			zz = dzdx * kf + z_xs;
			iw = diwdx * kf + iw_xs;
			if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
				if (depth_test && !cmp_func(l_z_func, zz, drow[x]))
					goto next_pixel;
				if (textured) {
					uint32_t texel;
					if (persp) {
						float q = (iw != 0.0f) ? 1.0f / iw : 1.0f;
						u = uu * q;
						v = vv * q;
					} else {
						u = uu;
						v = vv;
					}
					texel = sample_tex(&ltex, u, v, l_bilinear, l_addr_u,
							   l_addr_v);
					if (l_white)
						col = texel;
					else
						col = modulate(flat_col
								       ? a.color
								       : lerp_color(a.color, b.color,
										    c.color, w0, w1,
										    w2),
							       texel);
				} else {
					col = flat_col ? a.color
						       : lerp_color(a.color, b.color, c.color, w0,
								    w1, w2);
				}
				if (l_alpha_sharpen)
					col = sharpen_alpha(col, l_alpha_sharpen);
				if (l_alpha_test &&
				    !cmp_func(l_alpha_func, (float)((col >> 24) & 255),
					      (float)l_alpha_ref))
					goto next_pixel;
				if (depth_write)
					drow[x] = zz;
				if (l_blend) {
					uint32_t sa = col >> 24;
					if (l_blend_over) {
						if (sa == 255) {
							/* fully opaque: dst drops out */
						} else if (sa == 0) {
							goto next_pixel;
						} else {
							col = blend_over(col, crow[x], sa);
						}
					} else if (l_blend_add) {
						if (sa == 0)
							goto next_pixel;
						col = blend_add(col, crow[x], sa);
					} else {
						col = blend_pixel(col, crow[x], st);
					}
				}
				if (l_mask != 0xffffffffu)
					col = (col & l_mask) | (crow[x] & ~l_mask);
				crow[x] = col;
				if (idrow)
					idrow[x] = draw;
			}
		next_pixel:;
		}
	next_row:
		w0_row += dw0dy;
		w1_row += dw1dy;
		w2_row += dw2dy;
		u_row += dudy;
		v_row += dvdy;
		z_row += dzdy;
		iw_row += diwdy;
	}
}

#define SWRAST_MAX_THREADS 32
/* The default below is tuned for the D3D9 title, whose flush already fits in
 * the frame budget. A backend whose flush overruns by an order of magnitude
 * wants every core instead, so the cap is overridable at build time. */
#ifndef SWRAST_DEFAULT_THREADS
#define SWRAST_DEFAULT_THREADS 4
#endif
#define SWRAST_TILE_SHIFT 6
#define SWRAST_TILE (1 << SWRAST_TILE_SHIFT)

/* Draws are not rasterised as they arrive. Each triangle is recorded, together
 * with the state it needs, and appended to the bin of every screen tile it
 * touches. At flush time workers claim whole tiles, so threads never share
 * framebuffer memory, submission order is preserved within a tile (which is all
 * blending requires), and one synchronisation covers a whole frame's worth of
 * draws instead of one per draw call. */

typedef struct SwBatch {
	SwState st;
	SwTex tex;
	int has_tex;
} SwBatch;

typedef struct SwBinTri {
	SwVert a, b, c;
	unsigned batch;
	unsigned draw;
} SwBinTri;

typedef struct SwTile {
	unsigned *idx;
	unsigned count;
	unsigned cap;
} SwTile;

static HANDLE g_wake[SWRAST_MAX_THREADS];
static HANDLE g_thread[SWRAST_MAX_THREADS];
static HANDLE g_done;
static volatile LONG g_left;
static volatile int g_die;
static int g_nthreads;
static volatile LONG g_pool_state;

static SwRast g_target;
static int g_have_target;
static int g_drawid_armed;

/* Armed for a single frame by the snapshot key, so recording costs nothing
 * until someone asks for it. */
void swrast_drawid_arm(void)
{
	g_drawid_armed = 1;
}

void swrast_drawid_disarm(void)
{
	int i;
	g_drawid_armed = 0;
	for (i = 0; i < g_idbuf_count; i++)
		free(g_idbufs[i].px);
	memset(g_idbufs, 0, sizeof(g_idbufs));
	g_idbuf_count = 0;
}

static void idbuf_ensure(const SwRast *r)
{
	struct SwIdBuf *e;
	if (!g_drawid_armed || !r || r->width <= 0 || r->height <= 0)
		return;
	if (idbuf_lookup(r->color, r->width, r->height))
		return;
	if (g_idbuf_count >= SWRAST_ID_TARGETS)
		return;
	e = &g_idbufs[g_idbuf_count];
	e->px = (uint32_t *)calloc((size_t)r->width * (size_t)r->height, sizeof(uint32_t));
	if (!e->px)
		return;
	e->color = r->color;
	e->w = r->width;
	e->h = r->height;
	g_idbuf_count++;
}

static SwBinTri *g_tris;
static unsigned g_tri_count, g_tri_cap;
static SwBatch *g_batches;
static unsigned g_batch_count, g_batch_cap;
static SwTile *g_tiles;
static unsigned g_tile_cap;
static int g_tiles_x, g_tiles_y;
static volatile LONG g_next_tile;
static volatile LONG g_tile_total;
static int g_wake_n;

static DWORD WINAPI tile_worker(LPVOID param);

#ifdef SWRAST_THREADS_PHYSICAL
/* Logical processors overcount on an SMT host: two siblings share one set of
 * vector units, so a second raster thread on the same core adds little while
 * taking a runnable slot the game's own thread needs. Returns 0 if the topology
 * cannot be read, leaving the caller on its logical-processor count. */
static int physical_cores(void)
{
	SYSTEM_LOGICAL_PROCESSOR_INFORMATION *buf;
	DWORD bytes = 0;
	int n = 0;
	if (GetLogicalProcessorInformation(NULL, &bytes) || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return 0;
	buf = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION *)malloc(bytes);
	if (!buf)
		return 0;
	if (GetLogicalProcessorInformation(buf, &bytes)) {
		DWORD i, count = bytes / sizeof(buf[0]);
		for (i = 0; i < count; i++)
			if (buf[i].Relationship == RelationProcessorCore)
				n++;
	}
	free(buf);
	return n;
}
#endif

/* 0 = untouched, 1 = one thread is building it, 2 = usable. */
static void ensure_pool(void)
{
	SYSTEM_INFO si;
	int i;
	if (g_pool_state == 2)
		return;
	if (InterlockedCompareExchange(&g_pool_state, 1, 0) != 0) {
		while (g_pool_state != 2)
			Sleep(0);
		return;
	}
	GetSystemInfo(&si);
	g_nthreads = (int)si.dwNumberOfProcessors;
#ifdef SWRAST_THREADS_PHYSICAL
	/* Measured on this backend: 8 threads and 16 reach the same raster time,
	 * but 16 takes it out of the game's own frame time, so the wider pool
	 * finished fewer frames overall. */
	{
		int phys = physical_cores();
		if (phys > 0 && phys < g_nthreads)
			g_nthreads = phys;
	}
#endif
	{
		/* Scaling past four buys wall time the frame budget does not need
		 * and pays for it in CPU: measured on this rasteriser, 16 threads
		 * cut 1.2ms off a flush that already fits, for roughly 40% more CPU.
		 * A host with fewer cores still uses all of them, and
		 * D3D9SW_THREADS overrides in either direction. */
		const char *v = getenv("D3D9SW_THREADS");
		int want = v ? atoi(v) : 0;
		if (want > 0)
			g_nthreads = want;
		else if (g_nthreads > SWRAST_DEFAULT_THREADS)
			g_nthreads = SWRAST_DEFAULT_THREADS;
	}
	if (g_nthreads < 1)
		g_nthreads = 1;
	if (g_nthreads > SWRAST_MAX_THREADS)
		g_nthreads = SWRAST_MAX_THREADS;
	g_done = CreateEventA(NULL, TRUE, FALSE, NULL);
	g_die = 0;
	for (i = 0; i < g_nthreads; i++) {
		g_wake[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
		g_thread[i] = CreateThread(NULL, 0, tile_worker, (LPVOID)(intptr_t)i, 0, NULL);
	}
	InterlockedExchange(&g_pool_state, 2);
}

int swrast_thread_count(void)
{
	ensure_pool();
	return g_nthreads;
}

/* Retires the worker threads so their stacks stop existing. A snapshot taken
 * while they are parked would capture stacks we then rewind underneath live
 * threads; tearing the pool down removes them from the address space instead.
 * The next flush rebuilds it. */
void swrast_pool_shutdown(void)
{
	int i;
	if (g_pool_state != 2)
		return;
	g_die = 1;
	for (i = 0; i < g_nthreads; i++)
		SetEvent(g_wake[i]);
	for (i = 0; i < g_nthreads; i++) {
		if (g_thread[i]) {
			WaitForSingleObject(g_thread[i], INFINITE);
			CloseHandle(g_thread[i]);
			g_thread[i] = NULL;
		}
		if (g_wake[i]) {
			CloseHandle(g_wake[i]);
			g_wake[i] = NULL;
		}
	}
	if (g_done) {
		CloseHandle(g_done);
		g_done = NULL;
	}
	g_die = 0;
	g_nthreads = 0;
	InterlockedExchange(&g_pool_state, 0);
}

static void run_tile(int t)
{
	const SwTile *tile = &g_tiles[t];
	int tx = t % g_tiles_x;
	int ty = t / g_tiles_x;
	int x0 = tx << SWRAST_TILE_SHIFT;
	int y0 = ty << SWRAST_TILE_SHIFT;
	int x1 = x0 + SWRAST_TILE;
	int y1 = y0 + SWRAST_TILE;
	unsigned i;
	if (x1 > g_target.width)
		x1 = g_target.width;
	if (y1 > g_target.height)
		y1 = g_target.height;
	for (i = 0; i < tile->count; i++) {
		const SwBinTri *bt = &g_tris[tile->idx[i]];
		const SwBatch *ba = &g_batches[bt->batch];
		triangle_rect(&g_target, bt->a, bt->b, bt->c, ba->has_tex ? &ba->tex : NULL,
			      &ba->st, x0, x1, y0, y1, bt->draw);
	}
}

static DWORD WINAPI tile_worker(LPVOID param)
{
	g_worker_slot = (int)(intptr_t)param + 1;
	for (;;) {
		WaitForSingleObject(g_wake[(int)(intptr_t)param], INFINITE);
		if (g_die)
			return 0;
		for (;;) {
			LONG t = InterlockedIncrement(&g_next_tile) - 1;
			if (t >= g_tile_total)
				break;
			run_tile((int)t);
		}
		if (InterlockedDecrement(&g_left) == 0)
			SetEvent(g_done);
	}
}

static void bins_reset(void)
{
	unsigned i;
	for (i = 0; i < g_tile_cap; i++)
		g_tiles[i].count = 0;
	g_tri_count = 0;
	g_batch_count = 0;
}

/* Most reads touch a surface the binner has no work for, and a blanket flush
 * costs a full worker round-trip plus the loss of everything batched so far.
 * Deferred draws hold a raw pointer to their source texels as well as to the
 * target, so a texture about to be rewritten has to resolve first or the queued
 * draws would sample whatever the caller writes next. */
void swrast_flush_if_pending(const void *pixels)
{
	unsigned i;
	if (!g_have_target || g_tri_count == 0 || !pixels)
		return;
	if ((const void *)g_target.color == pixels) {
		swrast_flush();
		return;
	}
	for (i = 0; i < g_batch_count; i++) {
		if (g_batches[i].has_tex && (const void *)g_batches[i].tex.pixels == pixels) {
			swrast_flush();
			return;
		}
	}
}

/* The intrinsic form of xgetbv requires the xsave target feature under clang,
 * which we do not want to enable for the whole translation unit. */
static unsigned long long read_xcr0(void)
{
#if defined(__clang__) || defined(__GNUC__)
	unsigned int lo, hi;
	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	return ((unsigned long long)hi << 32) | lo;
#else
	return _xgetbv(0);
#endif
}

/* AVX-512 needs OS support for the opmask and both upper ZMM state groups, so
 * the xgetbv check matters as much as the feature bits. */
static int cpu_detect(void)
{
	int r[4];
	int f = CPU_SSE2; /* guaranteed on every CPU this can run on */
	unsigned long long xcr;
	__cpuid(r, 0);
	if (r[0] < 1)
		return f;
	__cpuid(r, 1);
	if (r[2] & (1 << 19))
		f |= CPU_SSE41;
	if (!(r[2] & (1 << 27)) || !(r[2] & (1 << 28)))
		return f; /* no OSXSAVE or no AVX */
	xcr = read_xcr0();
	if ((xcr & 0x6) != 0x6)
		return f; /* OS is not saving YMM */
	__cpuidex(r, 7, 0);
	if (r[1] & (1 << 5))
		f |= CPU_AVX2;
	if ((xcr & 0xe6) == 0xe6) {
		int need = (1 << 16) | (1 << 30) | (1 << 31); /* F, BW, VL */
		if ((r[1] & need) == need)
			f |= CPU_AVX512;
	}
	return f;
}

int swrast_cpu_features(void)
{
	static int cached = -1;
	if (cached < 0)
		cached = cpu_detect();
	return cached;
}

/* Area-weighted tally of which pixel configurations the frame is actually made
 * of, so the vector kernels get written for the cases that dominate. */
enum { MIX_TEX, MIX_BILIN, MIX_OVER, MIX_ADD, MIX_BLENDOTHER, MIX_ATEST, MIX_ZTEST,
       MIX_FLAT, MIX_LINEARU, MIX_N };
static double g_mix[MIX_N];

void swrast_prof_mix(double *out, int n)
{
	int i;
	for (i = 0; i < n && i < MIX_N; i++) {
		out[i] = g_mix[i];
		g_mix[i] = 0.0;
	}
}

static double g_prof_raster_ms;
static unsigned g_prof_flushes, g_prof_tris, g_prof_bins;
static double g_prof_area, g_prof_bbox;
static int g_prof_tw, g_prof_th;

void swrast_prof_take2(double *area, double *bbox, int *tw, int *th)
{
	if (area)
		*area = g_prof_area;
	if (bbox)
		*bbox = g_prof_bbox;
	if (tw)
		*tw = g_prof_tw;
	if (th)
		*th = g_prof_th;
	g_prof_area = 0.0;
	g_prof_bbox = 0.0;
	g_prof_tw = 0;
	g_prof_th = 0;
}

/* Flushes are triggered from wherever a surface is about to be read, not only
 * from the draw and present paths, so the caller cannot tell from its own zones
 * how much of a flush it is inside. Reading the accumulator without clearing it
 * lets a caller difference it across a zone and attribute the rest honestly. */
double swrast_prof_peek_raster(void)
{
	return g_prof_raster_ms;
}

void swrast_prof_take(double *raster_ms, unsigned *flushes, unsigned *tris, unsigned *bins)
{
	if (raster_ms)
		*raster_ms = g_prof_raster_ms;
	if (flushes)
		*flushes = g_prof_flushes;
	if (tris)
		*tris = g_prof_tris;
	if (bins)
		*bins = g_prof_bins;
	g_prof_raster_ms = 0.0;
	g_prof_flushes = 0;
	g_prof_tris = 0;
	g_prof_bins = 0;
}

void swrast_flush(void)
{
	int i;
	LARGE_INTEGER t0, t1, fq;
	if (!g_have_target || g_tri_count == 0) {
		bins_reset();
		return;
	}
	QueryPerformanceCounter(&t0);
	g_prof_flushes++;
	g_prof_tris += g_tri_count;
	g_tile_total = g_tiles_x * g_tiles_y;
	g_next_tile = 0;
	/* Most flushes touch a handful of tiles. Waking every worker for those
	 * costs more in signalling and wakeups than the tiles are worth, and on a
	 * CPU-only host that overhead is the product's whole budget. */
	{
		int busy = 0, t;
		for (t = 0; t < g_tile_total; t++)
			if (g_tiles[t].count)
				busy++;
		g_wake_n = busy < g_nthreads ? busy : g_nthreads;
		if (g_wake_n < 1)
			g_wake_n = 1;
	}
	if (g_wake_n > 1 && g_tile_total > 1) {
		g_left = g_wake_n;
		ResetEvent(g_done);
		for (i = 0; i < g_wake_n; i++)
			SetEvent(g_wake[i]);
		WaitForSingleObject(g_done, INFINITE);
	} else {
		LONG t;
		for (t = 0; t < g_tile_total; t++)
			run_tile((int)t);
	}
	{
		unsigned t;
		for (t = 0; t < (unsigned)g_tile_total; t++)
			g_prof_bins += g_tiles[t].count;
	}
	QueryPerformanceCounter(&t1);
	QueryPerformanceFrequency(&fq);
	g_prof_raster_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
	bins_reset();
}

static int tiles_fit(const SwRast *r)
{
	int tx = (r->width + SWRAST_TILE - 1) >> SWRAST_TILE_SHIFT;
	int ty = (r->height + SWRAST_TILE - 1) >> SWRAST_TILE_SHIFT;
	unsigned need = (unsigned)tx * (unsigned)ty;
	if (tx <= 0 || ty <= 0)
		return 0;
	if (need > g_tile_cap) {
		SwTile *nt = (SwTile *)realloc(g_tiles, (size_t)need * sizeof(SwTile));
		if (!nt)
			return 0;
		memset(nt + g_tile_cap, 0, (size_t)(need - g_tile_cap) * sizeof(SwTile));
		g_tiles = nt;
		g_tile_cap = need;
	}
	g_tiles_x = tx;
	g_tiles_y = ty;
	return 1;
}

static int tile_push(SwTile *tile, unsigned v)
{
	if (tile->count == tile->cap) {
		unsigned nc = tile->cap ? tile->cap * 2u : 64u;
		unsigned *ni = (unsigned *)realloc(tile->idx, (size_t)nc * sizeof(unsigned));
		if (!ni)
			return 0;
		tile->idx = ni;
		tile->cap = nc;
	}
	tile->idx[tile->count++] = v;
	return 1;
}

static unsigned batch_for(const SwTex *tex, const SwState *st)
{
	/* Consecutive draws usually share state, so check the tail before adding. */
	if (g_batch_count > 0) {
		const SwBatch *last = &g_batches[g_batch_count - 1];
		int same_tex = tex ? (last->has_tex && last->tex.pixels == tex->pixels &&
				      last->tex.width == tex->width &&
				      last->tex.height == tex->height)
				   : !last->has_tex;
		if (same_tex && memcmp(&last->st, st, sizeof(SwState)) == 0)
			return g_batch_count - 1;
	}
	if (g_batch_count == g_batch_cap) {
		unsigned nc = g_batch_cap ? g_batch_cap * 2u : 128u;
		SwBatch *nb = (SwBatch *)realloc(g_batches, (size_t)nc * sizeof(SwBatch));
		if (!nb)
			return g_batch_count ? g_batch_count - 1 : 0;
		g_batches = nb;
		g_batch_cap = nc;
	}
	g_batches[g_batch_count].st = *st;
	g_batches[g_batch_count].has_tex = tex != NULL;
	if (tex)
		g_batches[g_batch_count].tex = *tex;
	else
		memset(&g_batches[g_batch_count].tex, 0, sizeof(SwTex));
	return g_batch_count++;
}

void swrast_triangles(SwRast *r, const SwTri *tris, int count, const SwTex *tex,
		      const SwState *st)
{
	SwState fallback;
	unsigned batch;
	float grid;
	int i;

	if (!r || !r->color || !tris || count <= 0)
		return;
	if (!st) {
		swrast_state_defaults(&fallback);
		st = &fallback;
	}
	ensure_pool();
	/* Only one target can have pending work, so a switch forces a flush. */
	if (g_have_target && (g_target.color != r->color || g_target.width != r->width ||
			      g_target.height != r->height || g_target.depth != r->depth))
		swrast_flush();
	if (!tiles_fit(r))
		return;
	g_target = *r;
	g_have_target = 1;

	batch = batch_for(tex, st);
	{
		int has_tex = tex && tex->pixels;
		int over = st->blend_enable && st->blend_op == D3DBLENDOP_ADD &&
			   st->src_blend == D3DBLEND_SRCALPHA &&
			   st->dst_blend == D3DBLEND_INVSRCALPHA;
		int add = st->blend_enable && st->blend_op == D3DBLENDOP_ADD &&
			  st->src_blend == D3DBLEND_SRCALPHA && st->dst_blend == D3DBLEND_ONE;
		double total = 0.0;
		for (i = 0; i < count; i++)
			total += 0.5 * fabsf((tris[i].b.x - tris[i].a.x) *
						     (tris[i].c.y - tris[i].a.y) -
					     (tris[i].c.x - tris[i].a.x) *
						     (tris[i].b.y - tris[i].a.y));
		if (has_tex)
			g_mix[MIX_TEX] += total;
		if (has_tex && st->bilinear)
			g_mix[MIX_BILIN] += total;
		if (over)
			g_mix[MIX_OVER] += total;
		else if (add)
			g_mix[MIX_ADD] += total;
		else if (st->blend_enable)
			g_mix[MIX_BLENDOTHER] += total;
		if (st->alpha_test)
			g_mix[MIX_ATEST] += total;
		if (st->z_enable && r->depth)
			g_mix[MIX_ZTEST] += total;
		if (count > 0 && tris[0].a.color == tris[0].b.color &&
		    tris[0].b.color == tris[0].c.color)
			g_mix[MIX_FLAT] += total;
		/* Screen-aligned 1:1 sprites let a vector load replace a gather. */
		if (has_tex && count > 0) {
			const SwTri *t = &tris[0];
			float dy = fmaxf(fabsf(t->a.y - t->b.y), fabsf(t->b.y - t->c.y));
			float dv = fmaxf(fabsf(t->a.v - t->b.v), fabsf(t->b.v - t->c.v));
			if (dy > 0.5f && dv * (float)tex->height <= dy * 1.02f &&
			    dv * (float)tex->height >= dy * 0.98f)
				g_mix[MIX_LINEARU] += total;
		}
	}
	idbuf_ensure(r);
	g_draw_seq++;
	grid = subpixel_grid();
	for (i = 0; i < count; i++) {
		SwTri tt = tris[i];
		const SwTri *t = &tt;
		float fx0, fx1, fy0, fy1;
		int x0, x1, y0, y1, tx, ty;
		unsigned slot;
		/* Snap before the bounding box and before the copy into the bin
		 * list, so the binner and every tile that touches this triangle
		 * see one identical set of coordinates. */
		snap_vert(&tt.a, grid);
		snap_vert(&tt.b, grid);
		snap_vert(&tt.c, grid);
		fx0 = fminf(t->a.x, fminf(t->b.x, t->c.x));
		fx1 = fmaxf(t->a.x, fmaxf(t->b.x, t->c.x));
		fy0 = fminf(t->a.y, fminf(t->b.y, t->c.y));
		fy1 = fmaxf(t->a.y, fmaxf(t->b.y, t->c.y));
		if (!(fx1 >= fx0) || !(fy1 >= fy0))
			continue; /* NaN */
		x0 = (int)floorf(fx0);
		x1 = (int)ceilf(fx1);
		y0 = (int)floorf(fy0);
		y1 = (int)ceilf(fy1);
		if (st->scissor_enable) {
			if (x0 < st->scissor_x0)
				x0 = st->scissor_x0;
			if (y0 < st->scissor_y0)
				y0 = st->scissor_y0;
			if (x1 > st->scissor_x1)
				x1 = st->scissor_x1;
			if (y1 > st->scissor_y1)
				y1 = st->scissor_y1;
		}
		if (x0 < 0)
			x0 = 0;
		if (y0 < 0)
			y0 = 0;
		if (x1 > r->width)
			x1 = r->width;
		if (y1 > r->height)
			y1 = r->height;
		if (x0 >= x1 || y0 >= y1)
			continue;
		/* Measured here rather than in the pixel loop so the counters cost
		 * nothing per pixel. Triangle area is the work that must happen;
		 * bbox area is the window the scanline walk actually visits. */
		g_prof_bbox += (double)(x1 - x0) * (double)(y1 - y0);
		g_prof_area += 0.5 * fabsf((t->b.x - t->a.x) * (t->c.y - t->a.y) -
					   (t->c.x - t->a.x) * (t->b.y - t->a.y));
		if (r->width > g_prof_tw)
			g_prof_tw = r->width;
		if (r->height > g_prof_th)
			g_prof_th = r->height;
		if (g_tri_count == g_tri_cap) {
			unsigned nc = g_tri_cap ? g_tri_cap * 2u : 4096u;
			SwBinTri *nt = (SwBinTri *)realloc(g_tris, (size_t)nc * sizeof(SwBinTri));
			if (!nt)
				break;
			g_tris = nt;
			g_tri_cap = nc;
		}
		slot = g_tri_count;
		g_tris[slot].a = t->a;
		g_tris[slot].b = t->b;
		g_tris[slot].c = t->c;
		g_tris[slot].batch = batch;
		g_tris[slot].draw = g_draw_seq;
		g_tri_count++;
		for (ty = y0 >> SWRAST_TILE_SHIFT; ty <= ((y1 - 1) >> SWRAST_TILE_SHIFT); ty++) {
			for (tx = x0 >> SWRAST_TILE_SHIFT; tx <= ((x1 - 1) >> SWRAST_TILE_SHIFT);
			     tx++)
				tile_push(&g_tiles[ty * g_tiles_x + tx], slot);
		}
	}
}

int swrast_init(SwRast *r, HWND hwnd, int width, int height)
{
	memset(r, 0, sizeof(*r));
	r->hwnd = hwnd;
	swrast_resize(r, width, height);
	return r->color ? 1 : 0;
}

/* Pending bins hold raw pointers into the target, so any teardown must drain
 * them and forget the target before the memory goes away. */
static void target_invalidate(const SwRast *r)
{
	swrast_flush();
	if (g_have_target && g_target.color == r->color)
		g_have_target = 0;
}

void swrast_resize(SwRast *r, int width, int height)
{
	target_invalidate(r);
	free(r->color);
	free(r->depth);
	r->color = NULL;
	r->depth = NULL;
	r->width = width;
	r->height = height;
	if (width <= 0 || height <= 0)
		return;
	r->color = (uint32_t *)calloc((size_t)width * (size_t)height, sizeof(uint32_t));
	r->depth = (float *)malloc((size_t)width * (size_t)height * sizeof(float));
	if (r->depth) {
		size_t i, n = (size_t)width * (size_t)height;
		for (i = 0; i < n; i++)
			r->depth[i] = 1.0f;
	}
}

void swrast_free(SwRast *r)
{
	target_invalidate(r);
	free(r->color);
	free(r->depth);
	memset(r, 0, sizeof(*r));
}

void swrast_clear_color(SwRast *r, uint32_t d3d_color)
{
	size_t i, n;
	swrast_flush();
	if (!r->color)
		return;
	n = (size_t)r->width * (size_t)r->height;
	for (i = 0; i < n; i++)
		r->color[i] = d3d_color;
}

void swrast_clear_depth(SwRast *r, float z)
{
	size_t i, n;
	swrast_flush();
	if (!r->depth)
		return;
	n = (size_t)r->width * (size_t)r->height;
	for (i = 0; i < n; i++)
		r->depth[i] = z;
}

void swrast_present(SwRast *r, HWND hwnd_override)
{
	BITMAPINFO bmi;
	HDC hdc;
	HWND hwnd = hwnd_override ? hwnd_override : r->hwnd;
	RECT rc;
	int dw, dh;

	swrast_flush();
	if (!r->color || !hwnd)
		return;

	memset(&bmi, 0, sizeof(bmi));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = r->width;
	bmi.bmiHeader.biHeight = -r->height; /* top-down */
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	GetClientRect(hwnd, &rc);
	dw = rc.right - rc.left;
	dh = rc.bottom - rc.top;
	if (dw <= 0 || dh <= 0) {
		dw = r->width;
		dh = r->height;
	}

	hdc = GetDC(hwnd);
	if (!hdc)
		return;
	StretchDIBits(hdc, 0, 0, dw, dh, 0, 0, r->width, r->height, r->color, &bmi,
		      DIB_RGB_COLORS, SRCCOPY);
	ReleaseDC(hwnd, hdc);
}

/* Companion to the colour snapshot: same dimensions, one draw index per pixel.
 * Written raw because it is read back by an offline script, not by the game. */
/* Writes one file per render target seen this frame, each holding a draw index
 * for every pixel of that target. */
int swrast_dump_drawid(const char *prefix)
{
	int i, written = 0;
	swrast_flush();
	if (!prefix)
		return 0;
	for (i = 0; i < g_idbuf_count; i++) {
		char path[256];
		FILE *f;
		int32_t dims[2];
		if (!g_idbufs[i].px)
			continue;
		/* Dump the target's colour alongside its owner map, so an artefact
		 * can be located in the surface it was actually drawn into. */
		{
			SwRast img;
			memset(&img, 0, sizeof(img));
			img.color = (uint32_t *)g_idbufs[i].color;
			img.width = g_idbufs[i].w;
			img.height = g_idbufs[i].h;
			_snprintf(path, sizeof(path), "%s_%dx%d.tga", prefix, img.width,
				  img.height);
			path[sizeof(path) - 1] = 0;
			swrast_dump_tga(&img, path);
		}
		_snprintf(path, sizeof(path), "%s_%dx%d.did", prefix, g_idbufs[i].w,
			  g_idbufs[i].h);
		path[sizeof(path) - 1] = 0;
		f = fopen(path, "wb");
		if (!f)
			continue;
		dims[0] = g_idbufs[i].w;
		dims[1] = g_idbufs[i].h;
		fwrite("DID1", 1, 4, f);
		fwrite(dims, sizeof(int32_t), 2, f);
		fwrite(g_idbufs[i].px, sizeof(uint32_t),
		       (size_t)g_idbufs[i].w * (size_t)g_idbufs[i].h, f);
		fclose(f);
		written++;
	}
	return written;
}

void swrast_drawid_newframe(void)
{
	int i;
	g_draw_seq = 0;
	for (i = 0; i < g_idbuf_count; i++) {
		if (g_idbufs[i].px)
			memset(g_idbufs[i].px, 0,
			       (size_t)g_idbufs[i].w * (size_t)g_idbufs[i].h * sizeof(uint32_t));
	}
}

int swrast_dump_tga(const SwRast *r, const char *path)
{
	swrast_flush();
	FILE *f;
	int x, y;
	unsigned char hdr[18];

	if (!r->color || !path)
		return 0;
	f = fopen(path, "wb");
	if (!f)
		return 0;
	memset(hdr, 0, sizeof(hdr));
	hdr[2] = 2; /* uncompressed truecolor */
	hdr[12] = (unsigned char)(r->width & 0xff);
	hdr[13] = (unsigned char)((r->width >> 8) & 0xff);
	hdr[14] = (unsigned char)(r->height & 0xff);
	hdr[15] = (unsigned char)((r->height >> 8) & 0xff);
	hdr[16] = 24;
	hdr[17] = 0x20; /* top-down */
	fwrite(hdr, 1, 18, f);
	for (y = 0; y < r->height; y++) {
		for (x = 0; x < r->width; x++) {
			uint32_t c = r->color[y * r->width + x];
			unsigned char bgr[3];
			bgr[0] = (unsigned char)(c & 0xff);
			bgr[1] = (unsigned char)((c >> 8) & 0xff);
			bgr[2] = (unsigned char)((c >> 16) & 0xff);
			fwrite(bgr, 1, 3, f);
		}
	}
	fclose(f);
	return 1;
}
