#ifndef D3D9_SW_SWRAST_H
#define D3D9_SW_SWRAST_H

#include <windows.h>
#include <stdint.h>

typedef struct SwRast {
	int width;
	int height;
	uint32_t *color; /* D3DCOLOR 0xAARRGGBB, GDI-compatible byte order */
	float *depth;
	HWND hwnd;
} SwRast;

typedef struct SwVert {
	float x, y, z, rhw;
	uint32_t color;
	float u, v;
} SwVert;

typedef struct SwTex {
	int width;
	int height;
	const uint32_t *pixels;
} SwTex;

typedef struct SwTri {
	SwVert a, b, c;
} SwTri;

/* Pipeline state the rasterizer honours. Values use the D3D9 enums directly so
 * the wrap can copy render/sampler state across without translation. */
typedef struct SwState {
	int bilinear;
	int z_enable;
	int z_write;
	int z_func;  /* D3DCMPFUNC, 0 = LESSEQUAL */
	int blend_enable;
	int src_blend; /* D3DBLEND */
	int dst_blend;
	int blend_op; /* D3DBLENDOP */
	uint32_t blend_factor;
	int alpha_test;
	int alpha_func; /* D3DCMPFUNC, 0 = ALWAYS */
	int alpha_ref;
	int cull; /* D3DCULL */
	int addr_u; /* D3DTEXTUREADDRESS */
	int addr_v;
	/* Caller guarantees every sampled coordinate lands inside the texture, so
	 * addressing is a no-op and wrapping is never exercised. That lets the
	 * vector kernel index a non-power-of-two texture by row stride instead of
	 * a shift and mask, which is otherwise its only reason to require one.
	 * Zero unless the caller has actually proved it. */
	int uv_in_bounds;
	/* Sharpening applied to sampled alpha before testing and blending, as a
	 * Q8 slope about the halfway point: 256 leaves alpha alone, larger values
	 * pull the ramp either side of 0.5 towards fully out or fully in. A
	 * distance field stores the glyph edge at 0.5 and ramps over several
	 * texels, so a magnified one has no crisp edge to sample - the choice
	 * otherwise is a soft blob if the ramp is kept or a chewed silhouette if
	 * it is cut. The slope recovers an antialiased edge from the ramp.
	 * Zero and 256 both mean no remapping. */
	int alpha_sharpen;
	uint32_t write_mask; /* 0xAARRGGBB byte mask */
	int scissor_enable;
	int scissor_x0, scissor_y0, scissor_x1, scissor_y1;
} SwState;

void swrast_state_defaults(SwState *s);

int swrast_init(SwRast *r, HWND hwnd, int width, int height);
void swrast_resize(SwRast *r, int width, int height);
void swrast_free(SwRast *r);
void swrast_clear_color(SwRast *r, uint32_t d3d_color);
void swrast_clear_depth(SwRast *r, float z);
void swrast_triangles(SwRast *r, const SwTri *tris, int count, const SwTex *tex,
		      const SwState *st);
int swrast_thread_count(void);
/* Retires the worker pool; the next flush recreates it. */
void swrast_pool_shutdown(void);
/* Rasterise everything recorded so far. Required before any read of, or write
 * outside the rasteriser to, a surface that has pending draws. */
void swrast_flush(void);
/* Cheaper form: only flushes when the given pixel buffer is the pending target. */
void swrast_flush_if_pending(const void *pixels);
/* Reads and zeroes the accumulated counters for the frame just finished. */
void swrast_prof_take(double *raster_ms, unsigned *flushes, unsigned *tris, unsigned *bins);
/* Accumulated flush milliseconds so far, without clearing. For attributing how
 * much of the rasteriser's time a given caller is nested inside. */
double swrast_prof_peek_raster(void);
void swrast_prof_take2(double *area, double *bbox, int *tw, int *th);
/* Area-weighted mix of pixel configurations: tex, bilinear, over, add,
 * other-blend, alpha-test, z-test, flat-colour, linear-v. */
void swrast_prof_mix(double *out, int n);
/* Clipped area accepted by the AVX2 span kernel, and the area each gate turned
 * away: ok, no-avx2, untextured, non-flat colour, depth, mask, blend mode,
 * non-power-of-two texture, addressing mode, other. */
void swrast_prof_simd(double *out, int n);
int swrast_cpu_features(void);

/* Set to 0 to force the scalar reference rasteriser. */
extern int swrast_simd_enable;
/* -1 auto, 0 AVX2 gather, 1 AVX-512VL 256-bit masked gather, 2 scalar insert. */
extern int swrast_gather_mode;
void swrast_present(SwRast *r, HWND hwnd_override);
int swrast_dump_tga(const SwRast *r, const char *path);
int swrast_dump_drawid(const char *prefix);
void swrast_drawid_newframe(void);
void swrast_drawid_arm(void);
void swrast_drawid_disarm(void);

#endif
