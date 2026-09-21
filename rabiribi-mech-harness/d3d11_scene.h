/*
 * Shared D3D11 scene for mech-harness: many quads/triangles, large RT, extra
 * live GPU allocations (ballast). Deterministic from seed for cross-session restore.
 */
#ifndef D3D11_SCENE_H
#define D3D11_SCENE_H

#include <windows.h>
#include <d3d11.h>
#include <stdint.h>

#define D3D11_SCENE_W 512
#define D3D11_SCENE_H 512
#define D3D11_SCENE_PIXBYTES (D3D11_SCENE_W * D3D11_SCENE_H * 4)
#define D3D11_SCENE_MAX_QUADS 48
#define D3D11_SCENE_BALLAST_TEX 16

typedef struct D3d11SceneState {
	unsigned seed;
	unsigned num_quads;
	float tint[4];
	float global_off[2];
	float pad[2];
} D3d11SceneState;

typedef struct D3d11SceneSnapRefs {
	unsigned saved_pid;
	uintptr_t dev;
	uintptr_t ctx;
	uintptr_t cb;
	uintptr_t vb;
	uintptr_t rt;
	uintptr_t rtv;
} D3d11SceneSnapRefs;

typedef struct D3d11Scene {
	ID3D11Device *dev;
	ID3D11DeviceContext *ctx;
	ID3D11Texture2D *rt;
	ID3D11Texture2D *stg;
	ID3D11RenderTargetView *rtv;
	ID3D11Buffer *cb;
	ID3D11Buffer *vb;
	ID3D11VertexShader *vs;
	ID3D11PixelShader *ps;
	ID3D11InputLayout *il;
	ID3D11Texture2D *ballast[D3D11_SCENE_BALLAST_TEX];
	unsigned ballast_n;
	const char *driver_name;
} D3d11Scene;

unsigned d3d11_scene_fnv1a(const void *p, size_t n);
void d3d11_scene_seed_state(D3d11SceneState *s, unsigned seed);
int d3d11_scene_init(D3d11Scene *sc, unsigned ballast_seed);
void d3d11_scene_fill_refs(const D3d11Scene *sc, D3d11SceneSnapRefs *refs);
int d3d11_scene_device_recreated(const D3d11SceneSnapRefs *saved, const D3d11Scene *live);
unsigned d3d11_scene_render(const D3d11Scene *sc, const D3d11SceneState *st, unsigned char *packed_rgba);
void d3d11_scene_flush(const D3d11Scene *sc);
void d3d11_scene_shutdown(D3d11Scene *sc);

#endif
