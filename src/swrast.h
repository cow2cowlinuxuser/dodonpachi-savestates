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
	/* Content generation, bumped by the caller whenever the texels change
	 * under the same pointer. Read only by the paint statistics, to tell a
	 * tile that draws the same thing from one that draws a new frame of it. */
	unsigned gen;
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
	/* Under a multiply blend, fade the source toward white by its own alpha
	 * before blending, so a transparent source is the multiplicative
	 * identity instead of black. Set by callers that approximate a pixel
	 * shader rather than running one. */
	int mul_identity;
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
#define SWRAST_MAX_THREADS 32
/* Runs fn(arg, worker, job) for job 0..njobs-1 on up to max_threads threads,
 * the caller included, and returns when all are done. worker is below
 * SWRAST_JOB_SLOTS and no two concurrent calls share one. Returns the number of
 * threads that took part. Must not overlap a flush. */
#define SWRAST_JOB_SLOTS (SWRAST_MAX_THREADS + 1)
int swrast_parallel(int njobs, int max_threads, void (*fn)(void *arg, int worker, int job),
		    void *arg);
/* Retires the worker pool; the next flush recreates it. */
void swrast_pool_shutdown(void);
/* Rasterise everything recorded so far. Required before any read of, or write
 * outside the rasteriser to, a surface that has pending draws. */
void swrast_flush(void);
/* Cheaper form: only flushes when the given pixel buffer is the pending target. */
void swrast_flush_if_pending(const void *pixels);
/* Reads and zeroes the accumulated counters for the frame just finished. */
void swrast_prof_take(double *raster_ms, unsigned *flushes, unsigned *tris, unsigned *bins);
/* Cumulative, never cleared: geometry dropped because a bin could not grow. */
void swrast_prof_drops(unsigned *tris, unsigned *tiles);
/* Accumulated flush milliseconds so far, without clearing. For attributing how
 * much of the rasteriser's time a given caller is nested inside. */
double swrast_prof_peek_raster(void);
void swrast_prof_take2(double *area, double *bbox, int *tw, int *th);
/* Area-weighted mix of pixel configurations: tex, bilinear, over, add,
 * other-blend, alpha-test, z-test, flat-colour, linear-v. */
void swrast_prof_mix(double *out, int n);
/* Clipped area accepted by the AVX2 span kernel, and the area each gate turned
 * away: ok, no-avx2, untextured, non-flat colour, depth, mask, blend mode,
 * non-power-of-two texture, addressing mode, other. An eleventh entry, not a
 * reason and not part of the total, is the accepted area folded by wrap or
 * mirror at any dimensions. */
void swrast_prof_simd(double *out, int n);

/* Names the blend states that fell off the vector path, worst area first.
 * Returns how many were filled in. */
int swrast_prof_blend_other(int *op, int *src, int *dst, double *area, int n);

/* Paint statistics: how much of the shaded area is work that changes nothing.
 * Off unless swrast_paintstat is set; N > 1 measures two consecutive frames in
 * every N, so the tile comparison always spans adjacent frames.
 *
 * Every shaded pixel lands in exactly one of KILLED (alpha or depth test
 * failed after the texel was fetched), ZERO (alpha 0 under over or add), NOOP
 * (add of black, multiply by white), OPAQUE (replaces the destination) or RMW
 * (a real blend). HIDDEN counts earlier writes to a pixel that a later OPAQUE
 * write in the same frame buried, before anything sampled the target. VEC and
 * FULL8 are the vector-path pixels and those in full 8-lane groups; UNIFORM is
 * the part of FULL8 whose eight source colours were identical, split into
 * UNIF_CLEAR (alpha 0), UNIF_OPAQUE (alpha 255) and, by difference,
 * translucent. The TILE rows
 * compare each 64px tile's draw list against the previous frame's; the RECT
 * rows are clipped screen area drawn as axis-aligned two-triangle quads, split
 * by texel scale, and FULL those that cover their whole target. */
enum {
	SWPS_SHADED, SWPS_KILLED, SWPS_ZERO, SWPS_NOOP, SWPS_OPAQUE, SWPS_RMW,
	SWPS_HIDDEN, SWPS_VEC, SWPS_FULL8, SWPS_UNIFORM, SWPS_UNIF_CLEAR, SWPS_UNIF_OPAQUE,
	SWPS_TILES, SWPS_TILES_SAME, SWPS_TILE_PX, SWPS_TILE_PX_SAME,
	SWPS_TGT_FRAMES, SWPS_TGT_STATIC,
	SWPS_RECT, SWPS_RECT_1TO1, SWPS_RECT_INT, SWPS_RECT_FULL, SWPS_RECT_FULL_N,
	SWPS_FRAMES, SWPS_N
};
extern int swrast_paintstat;
/* Call once per presented frame, after the last draw of it. */
void swrast_paintstat_frame(void);
/* Reads and zeroes SWPS_N totals. */
void swrast_prof_paint(double *out);

/* Hardware backend. Presents the finished software frame through a real
 * swapchain, or returns 0 if no device is up and the caller should present the
 * way it always has. */
void gpu_set_log(void (*log)(const char *));
int gpu_present_framebuffer(HWND hwnd, const uint32_t *pixels, int w, int h);

/* Stage 2: the draws themselves. gpu_draw returns 0 for anything it cannot
 * honour exactly, and the caller must then decline the whole frame - a frame
 * split between two backends is two half-drawn images rather than one. */
int gpu_ensure(HWND hwnd);
int gpu_tex_sync(void **slot, const uint32_t *pixels, int w, int h, unsigned gen);
void gpu_tex_drop(void **slot);
int gpu_frame_begin(int w, int h, int clear, uint32_t argb);
int gpu_draw(const SwTri *tris, int n, void *texslot, const SwState *st);
int gpu_frame_end(void);
int gpu_readback(uint32_t *dst, unsigned dst_pitch, int w, int h);
void gpu_park(int on);
void gpu_dxgi_reconcile(HWND hwnd);
void gpu_prof_take(unsigned *uploads, unsigned *draws, unsigned *verts);

/* Installed by whichever front end has a hardware backend. Left null everywhere
 * else, which is how the OpenGL path and the harnesses avoid linking one. */
void swrast_set_gpu_present(int (*fn)(HWND, const uint32_t *, int, int));
int gpu_is_up(void);
void gpu_shutdown(void);
int swrast_cpu_features(void);

/* Set to 0 to force the scalar reference rasteriser. */
extern int swrast_simd_enable;
/* -1 auto, 0 AVX2 gather, 1 AVX-512VL 256-bit masked gather, 2 scalar insert. */
extern int swrast_gather_mode;
void swrast_present(SwRast *r, HWND hwnd_override);
/* A line or two of text drawn over the presented frame. Set it from anywhere;
 * it is copied under a lock and painted by the next present. NULL or "" clears
 * it. Newlines are honoured. */
void swrast_overlay_set(const char *text);
void swrast_overlay_draw(HDC hdc, int client_w);
int swrast_dump_tga(const SwRast *r, const char *path);
int swrast_dump_drawid(const char *prefix);
void swrast_drawid_newframe(void);
void swrast_drawid_arm(void);
void swrast_drawid_disarm(void);

#endif
