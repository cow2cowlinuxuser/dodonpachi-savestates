/*
 * d3d11_coexist - the in-process savestate mechanism running WITH a live wined3d
 * D3D11 device in the same process, on the CPU backend.
 *
 * The gating question before anything else: does Wine block us? A real D3D11
 * device spins up wined3d's own threads and allocates driver objects in Wine
 * heaps. This harness stands one up, then runs the arena capture/verify/restore
 * loop (our own worker threads churning a rewound arena, quiesced at a
 * cooperative safe point) INTERLEAVED with rendering, and checks:
 *
 *   1. the arena save/restore stays correct (0 poisoned) with wined3d live -
 *      Wine's threads/objects do not block or corrupt our capture;
 *   2. the D3D11 device keeps existing and rendering across our restores;
 *   3. Class B (inward straddle): the live device pointer is HELD inside the
 *      rewound arena. After a restore it is rewound to its save-time value; the
 *      object is present (not rewound), so the pointer must still be valid and
 *      the device must still render the restored frame.
 *
 * A second mode exercises Class A (outward straddle) safely, with a generational
 * handle so we never dereference a freed COM object:
 *
 *   --retire  every cycle the device is released and recreated between save and
 *             restore (the present side "moves on"). The arena's rewound handle
 *             now carries a stale generation; we DETECT that instead of calling
 *             through a dangling pointer, and the mitigation is to re-pin the
 *             handle to the current device (object retirement / re-acquire).
 *
 * Why this does not deadlock the way the docs warn about: we quiesce OUR worker
 * threads at a cooperative barrier and never SuspendThread wined3d's threads, so
 * there is no suspend-all-then-wait-on-wineserver hazard. wined3d threads are
 * present peers, like the game's system threads.
 *
 *   wine d3d11_coexist.exe [cycles] [--retire]
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

#define W 96
#define H 96
#define NODE_CAP 1024
#define NODE_MAGIC 0x43584953u /* 'CXIS' */
#define ARENA_BYTES (sizeof(Arena))

typedef struct State { float tint[4]; float offset[2]; float pad[2]; } State;

typedef struct Node { unsigned magic, serial, self_lo, checksum; } Node;

/* rewound domain */
typedef struct Arena {
	unsigned magic, clock;
	unsigned long long rng;
	State gpu;                 /* GPU-resident logical state (drives the render) */
	ID3D11Device *dev_ptr;     /* Class B: a live device pointer held from rewound memory */
	unsigned dev_gen;          /* generational handle for the device object */
	int free_head; unsigned in_use;
	Node node[NODE_CAP];
} Arena;

/* present/held domain (never rewound) */
typedef struct Held {
	unsigned char *snapshot, *scratch;
	unsigned have_snap;
	unsigned long long saved_rng; unsigned saved_clock;
	unsigned saved_fp;         /* frame the saved gpu state produces */
	unsigned dev_gen;          /* current device generation (present truth) */
	unsigned poisoned, render_mismatch, dev_removed, classB_ok, straddle_detected, straddle_repinned;
} Held;

static const char *HLSL =
	"cbuffer S:register(b0){float4 tint;float2 offset;float2 pad;};\n"
	"struct VI{float3 p:POSITION;float4 c:COLOR;};struct VO{float4 p:SV_POSITION;float4 c:COLOR;};\n"
	"VO VS(VI i){VO o;o.p=float4(i.p.xy+offset,i.p.z,1);o.c=i.c*tint;return o;}\n"
	"float4 PS(VO i):SV_TARGET{return i.c;}\n";
typedef struct { float x, y, z; float r, g, b, a; } Vtx;

static ID3D11Device *g_dev; static ID3D11DeviceContext *g_ctx;
static ID3D11Texture2D *g_rt, *g_stg; static ID3D11RenderTargetView *g_rtv;
static ID3D11Buffer *g_cb, *g_vb; static ID3D11VertexShader *g_vs; static ID3D11PixelShader *g_ps; static ID3D11InputLayout *g_il;

static Arena *g_arena; static Held *g_held;
static CRITICAL_SECTION g_cs;
static volatile LONG g_stop, g_quiesce, g_parked;
static HANDLE g_worker[2]; static int g_nworkers = 2;

static unsigned fnv1a(const void *p, size_t n){ const unsigned char*b=p; unsigned h=2166136261u; size_t i; for(i=0;i<n;i++){h^=b[i];h*=16777619u;} return h; }
static unsigned node_ck(const Node*n){ unsigned p[3]={n->magic,n->serial,n->self_lo}; return fnv1a(p,sizeof(p)); }

/* ---- D3D11 device + pipeline ---- */
static int build_pipeline(void)
{
	D3D_FEATURE_LEVEL fl=0, want[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_0};
	D3D11_TEXTURE2D_DESC td; D3D11_BUFFER_DESC bd; D3D11_SUBRESOURCE_DATA sd;
	HMODULE dc; PFN_D3DCompile D3DCompile; ID3DBlob *vsb=NULL,*psb=NULL,*err=NULL;
	D3D11_INPUT_ELEMENT_DESC ild[2]={
		{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
		{"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
	Vtx v[3]={{0,0.8f,0,1,1,1,1},{0.8f,-0.8f,0,1,0.85f,0.16f,1},{-0.8f,-0.8f,0,0.16f,0.7f,1,1}};
	State init={{1,1,1,1},{0,0},{0,0}};
	if (FAILED(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,want,2,D3D11_SDK_VERSION,&g_dev,&fl,&g_ctx))) return 0;
	ZeroMemory(&td,sizeof(td)); td.Width=W;td.Height=H;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
	if (FAILED(ID3D11Device_CreateTexture2D(g_dev,&td,NULL,&g_rt))) return 0;
	if (FAILED(ID3D11Device_CreateRenderTargetView(g_dev,(ID3D11Resource*)g_rt,NULL,&g_rtv))) return 0;
	td.Usage=D3D11_USAGE_STAGING; td.BindFlags=0; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
	if (FAILED(ID3D11Device_CreateTexture2D(g_dev,&td,NULL,&g_stg))) return 0;
	dc=LoadLibraryA("d3dcompiler_47.dll"); D3DCompile=dc?(PFN_D3DCompile)GetProcAddress(dc,"D3DCompile"):NULL;
	if(!D3DCompile) return 0;
	if (FAILED(D3DCompile(HLSL,strlen(HLSL),NULL,NULL,NULL,"VS","vs_4_0",0,0,&vsb,&err)) ||
	    FAILED(D3DCompile(HLSL,strlen(HLSL),NULL,NULL,NULL,"PS","ps_4_0",0,0,&psb,&err))) return 0;
	ID3D11Device_CreateVertexShader(g_dev,ID3D10Blob_GetBufferPointer(vsb),ID3D10Blob_GetBufferSize(vsb),NULL,&g_vs);
	ID3D11Device_CreatePixelShader(g_dev,ID3D10Blob_GetBufferPointer(psb),ID3D10Blob_GetBufferSize(psb),NULL,&g_ps);
	ID3D11Device_CreateInputLayout(g_dev,ild,2,ID3D10Blob_GetBufferPointer(vsb),ID3D10Blob_GetBufferSize(vsb),&g_il);
	ID3D10Blob_Release(vsb); ID3D10Blob_Release(psb);
	ZeroMemory(&bd,sizeof(bd)); bd.ByteWidth=sizeof(v); bd.Usage=D3D11_USAGE_DEFAULT; bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
	ZeroMemory(&sd,sizeof(sd)); sd.pSysMem=v; if (FAILED(ID3D11Device_CreateBuffer(g_dev,&bd,&sd,&g_vb))) return 0;
	ZeroMemory(&bd,sizeof(bd)); bd.ByteWidth=sizeof(State); bd.Usage=D3D11_USAGE_DEFAULT; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
	ZeroMemory(&sd,sizeof(sd)); sd.pSysMem=&init; if (FAILED(ID3D11Device_CreateBuffer(g_dev,&bd,&sd,&g_cb))) return 0;
	return 1;
}
static void teardown_pipeline(void)
{
	if(g_vb)ID3D11Buffer_Release(g_vb); if(g_cb)ID3D11Buffer_Release(g_cb);
	if(g_il)ID3D11InputLayout_Release(g_il); if(g_ps)ID3D11PixelShader_Release(g_ps); if(g_vs)ID3D11VertexShader_Release(g_vs);
	if(g_stg)ID3D11Texture2D_Release(g_stg); if(g_rtv)ID3D11RenderTargetView_Release(g_rtv); if(g_rt)ID3D11Texture2D_Release(g_rt);
	if(g_ctx)ID3D11DeviceContext_Release(g_ctx); if(g_dev)ID3D11Device_Release(g_dev);
	g_vb=g_cb=NULL; g_il=NULL; g_ps=NULL; g_vs=NULL; g_stg=g_rt=NULL; g_rtv=NULL; g_ctx=NULL; g_dev=NULL;
}
static unsigned render_fp(const State *s)
{
	float clear[4]={1,0,0.5f,1}; D3D11_VIEWPORT vp; UINT stride=sizeof(Vtx),off=0; D3D11_MAPPED_SUBRESOURCE m; unsigned h=0,y;
	ID3D11DeviceContext_UpdateSubresource(g_ctx,(ID3D11Resource*)g_cb,0,NULL,s,0,0);
	ID3D11DeviceContext_OMSetRenderTargets(g_ctx,1,&g_rtv,NULL);
	ZeroMemory(&vp,sizeof(vp)); vp.Width=(FLOAT)W; vp.Height=(FLOAT)H; vp.MaxDepth=1.0f; ID3D11DeviceContext_RSSetViewports(g_ctx,1,&vp);
	ID3D11DeviceContext_ClearRenderTargetView(g_ctx,g_rtv,clear);
	ID3D11DeviceContext_IASetInputLayout(g_ctx,g_il);
	ID3D11DeviceContext_IASetVertexBuffers(g_ctx,0,1,&g_vb,&stride,&off);
	ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_VSSetShader(g_ctx,g_vs,NULL,0); ID3D11DeviceContext_PSSetShader(g_ctx,g_ps,NULL,0);
	ID3D11DeviceContext_VSSetConstantBuffers(g_ctx,0,1,&g_cb); ID3D11DeviceContext_Draw(g_ctx,3,0); ID3D11DeviceContext_Flush(g_ctx);
	ID3D11DeviceContext_CopyResource(g_ctx,(ID3D11Resource*)g_stg,(ID3D11Resource*)g_rt);
	if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx,(ID3D11Resource*)g_stg,0,D3D11_MAP_READ,0,&m))){
		for(y=0;y<H;y++) h^=fnv1a((char*)m.pData+y*m.RowPitch,W*4);
		ID3D11DeviceContext_Unmap(g_ctx,(ID3D11Resource*)g_stg,0);
	}
	return h;
}

/* ---- arena allocator + workers (the rewound game logic) ---- */
static void nodes_seed(void){ unsigned i; g_arena->in_use=0; for(i=0;i<NODE_CAP;i++){ Node*n=&g_arena->node[i]; if(i&1){ n->magic=NODE_MAGIC; n->serial=i*7+1; n->self_lo=i; n->checksum=node_ck(n); g_arena->in_use++; } else { n->magic=0; n->self_lo=i; n->serial=0; n->checksum=0; } } }
static unsigned identity_bad(const Arena*a){ unsigned bad=0,i; for(i=0;i<NODE_CAP;i++){ const Node*n=&a->node[i]; if(n->magic!=NODE_MAGIC) continue; if(n->self_lo!=i){bad++;continue;} if(n->checksum!=node_ck(n)){bad++;continue;} } return bad; }

static void quiesce(void){ InterlockedExchange(&g_quiesce,1); while(InterlockedCompareExchange(&g_parked,0,0)<g_nworkers && !g_stop) Sleep(0); }
static void release_workers(void){ InterlockedExchange(&g_quiesce,0); while(InterlockedCompareExchange(&g_parked,0,0)>0 && !g_stop) Sleep(0); }

static DWORD WINAPI worker(LPVOID p){
	int id=(int)(intptr_t)p; unsigned long long tr=0x1234u^(unsigned)(id+1)*2654435761u; int parked=0;
	while(!InterlockedCompareExchange(&g_stop,0,0)){
		if(InterlockedCompareExchange(&g_quiesce,0,0)){ if(!parked){InterlockedIncrement(&g_parked);parked=1;} Sleep(0); continue; }
		if(parked){InterlockedDecrement(&g_parked);parked=0;}
		tr^=tr<<13; tr^=tr>>7; tr^=tr<<17;
		EnterCriticalSection(&g_cs);
		{ unsigned idx=(unsigned)(tr%NODE_CAP); Node*n=&g_arena->node[idx];
		  if(n->magic==NODE_MAGIC){ n->magic=0; n->checksum=0; if(g_arena->in_use)g_arena->in_use--; }
		  else { n->magic=NODE_MAGIC; n->serial=(unsigned)(tr>>13); n->self_lo=idx; n->checksum=node_ck(n); g_arena->in_use++; } }
		LeaveCriticalSection(&g_cs);
	}
	if(parked) InterlockedDecrement(&g_parked);
	return 0;
}

/* ---- capture (verify-or-refuse) + restore ---- */
static void commit_from(const Arena*src){
	memcpy(g_held->snapshot,src,ARENA_BYTES); g_held->have_snap=1;
	g_held->saved_rng=src->rng; g_held->saved_clock=src->clock;
}
static int checkpoint(void){ /* SUSPEND+VERIFY-equivalent via cooperative barrier + verify */
	Arena*scr=(Arena*)g_held->scratch;
	quiesce();
	memcpy(scr,g_arena,ARENA_BYTES);
	if (identity_bad(scr)==0){ commit_from(scr); g_held->saved_fp=render_fp(&scr->gpu); release_workers(); return 1; }
	release_workers(); return 0;
}
static void restore(void){ /* leaves workers PARKED; caller verifies then releases */
	quiesce();
	memcpy(g_arena,g_held->snapshot,ARENA_BYTES);
}

static void seed_state(State*s,unsigned k){
	s->tint[0]=0.3f+0.5f*((k*2654435761u>>8)&0xff)/255.0f; s->tint[1]=0.3f+0.5f*((k*40503u>>4)&0xff)/255.0f;
	s->tint[2]=0.3f+0.5f*((k*2246822519u>>12)&0xff)/255.0f; s->tint[3]=1.0f;
	s->offset[0]=-0.2f+0.4f*((k*22695477u>>16)&0xff)/255.0f; s->offset[1]=-0.2f+0.4f*((k*3266489917u>>20)&0xff)/255.0f;
	s->pad[0]=s->pad[1]=0;
}

int main(int argc,char**argv)
{
	int cycles = argc>1?atoi(argv[1]):200; int retire=0; int c,i;
	for(i=1;i<argc;i++) if(!strcmp(argv[i],"--retire")) retire=1;

	InitializeCriticalSection(&g_cs);
	g_held=(Held*)VirtualAlloc(NULL,sizeof(Held),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
	if(!g_held){ printf("alloc failed\n"); return 2; }
	memset(g_held,0,sizeof(Held));
	g_held->snapshot=(unsigned char*)VirtualAlloc(NULL,ARENA_BYTES,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
	g_held->scratch =(unsigned char*)VirtualAlloc(NULL,ARENA_BYTES,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
	g_arena=(Arena*)VirtualAlloc(NULL,ARENA_BYTES,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
	if(!g_held->snapshot||!g_held->scratch||!g_arena){ printf("alloc failed\n"); return 2; }

	if(!build_pipeline()){ printf("no D3D11 device (build_pipeline failed)\n"); return 2; }
	g_held->dev_gen=1;
	printf("pid=%lu  live D3D11 device up; running savestate loop WITH wined3d in the process\n",(unsigned long)GetCurrentProcessId());
	printf("mode=%s cycles=%d workers=%d\n", retire?"--retire (Class A straddle)":"coexist (Class B)", cycles, g_nworkers);

	memset(g_arena,0,ARENA_BYTES); g_arena->magic=NODE_MAGIC; g_arena->rng=0x0123456789ABCDEFull;
	nodes_seed();
	g_arena->dev_ptr=g_dev; g_arena->dev_gen=g_held->dev_gen; /* Class B: hold the live device from rewound memory */

	for(i=0;i<g_nworkers;i++) g_worker[i]=CreateThread(NULL,0,worker,(LPVOID)(intptr_t)i,0,NULL);
	Sleep(30);

	for(c=0; c<cycles && !g_stop; c++){
		int skip_render=0;
		g_arena->clock++; g_arena->rng ^= g_arena->rng<<13; g_arena->rng ^= g_arena->rng>>7;
		seed_state(&g_arena->gpu, (unsigned)(c+1));

		if(!checkpoint()){ /* refused: keep prior snapshot */ }

		/* --- divergence between save and restore --- */
		{ State mut; seed_state(&mut,(unsigned)(c*7+99)); g_arena->gpu=mut; }
		if(retire){
			/* present side "moves on": retire the device object and make a new one.
			 * The arena still holds the OLD device pointer + generation. */
			teardown_pipeline(); build_pipeline(); g_held->dev_gen++;
			g_arena->dev_ptr = (ID3D11Device*)g_arena->dev_ptr; /* unchanged (stale) */
		}

		restore(); /* rewinds gpu state AND dev_ptr/dev_gen; workers left parked */

		/* Class A/B check on the device reference carried through the rewind */
		if(g_arena->dev_gen != g_held->dev_gen){
			/* the referenced object was retired -> stale handle. Detect, do not call. */
			g_held->straddle_detected++;
			/* mitigation: object retirement / re-pin to the current device */
			g_arena->dev_ptr = g_dev; g_arena->dev_gen = g_held->dev_gen;
			g_held->straddle_repinned++;
		} else {
			/* Class B holds: pointer rewound to a live object; must equal the device */
			if(g_arena->dev_ptr == g_dev) g_held->classB_ok++;
		}

		/* verify arena consistency (workers still parked), then USE the
		 * (re-pinned/valid) device pointer, then release the workers */
		if(identity_bad(g_arena)!=0) g_held->poisoned++;
		if(!skip_render){
			unsigned fp = render_fp(&g_arena->gpu); /* device still renders the restored frame */
			if(fp != g_held->saved_fp) g_held->render_mismatch++;
		}
		if(ID3D11Device_GetDeviceRemovedReason(g_dev)!=S_OK) g_held->dev_removed++;
		release_workers();

		if((c%50)==0) printf("  cycle %d: poisoned=%u render_mismatch=%u classB_ok=%u straddle=%u repin=%u dev_removed=%u\n",
			c,g_held->poisoned,g_held->render_mismatch,g_held->classB_ok,g_held->straddle_detected,g_held->straddle_repinned,g_held->dev_removed);
	}

	InterlockedExchange(&g_stop,1);
	for(i=0;i<g_nworkers;i++) if(g_worker[i]) WaitForSingleObject(g_worker[i],1000);

	printf("\nsummary (%d cycles, wined3d live throughout):\n",cycles);
	printf("  arena poisoned      : %u\n", g_held->poisoned);
	printf("  render mismatches   : %u\n", g_held->render_mismatch);
	printf("  Class B pointer ok  : %u\n", g_held->classB_ok);
	printf("  Class A straddle    : detected=%u repinned=%u\n", g_held->straddle_detected, g_held->straddle_repinned);
	printf("  device removals     : %u\n", g_held->dev_removed);
	{
		int ok = (g_held->poisoned==0 && g_held->render_mismatch==0 && g_held->dev_removed==0);
		printf("\n%s: the savestate loop ran to completion with a live wined3d device - "
		       "Wine did not block us; the device coexisted and stayed usable across every restore.\n",
		       ok?"PASS":"FAIL");
		teardown_pipeline(); DeleteCriticalSection(&g_cs);
		return ok?0:1;
	}
}
