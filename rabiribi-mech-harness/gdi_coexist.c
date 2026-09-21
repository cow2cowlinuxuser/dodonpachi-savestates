/*
 * gdi_coexist - the in-process savestate loop running WITH live GDI/USER32
 * objects (a window, a memory DC, a DIB section, brushes), on Wine.
 *
 * The docs flag GDI/USER32 as an upstream blocker: "USER32/GDI not rewound;
 * Proton A->B dies in USER32". This harness gives an empirical read on three
 * things, by holding the GDI/USER handles INSIDE the rewound arena and running
 * the capture/verify/restore loop around them:
 *
 *   1. Do the handles survive the rewind? (Class B: HWND/HDC/HBITMAP are handles
 *      to kernel-side (win32k) objects; the handle VALUE is rewound with the
 *      arena, the object is present. After a restore the handles must still be
 *      valid - IsWindow / GetObjectType.)
 *   2. Is GDI object CONTENT rewound? A DIB section's pixels live on the GDI
 *      side, not in our snapshot. We draw value X, save, draw value Y, then
 *      rewind the arena - and read the DIB. If it still shows Y, GDI content is
 *      OUTSIDE the snapshot: a desync source exactly like the DirectSound cursor
 *      split. The mitigation is to reconcile - redraw the GDI content from the
 *      rewound logical state.
 *   3. Does a live window + GDI + message pump BLOCK the capture? We quiesce our
 *      own workers at a cooperative safe point and never SuspendThread the UI
 *      thread, so win32k locks are never held by a frozen thread. If the loop
 *      runs to completion, GDI did not block us.
 *
 *   wine gdi_coexist.exe [cycles]
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define W 128
#define H 128
#define PIXBYTES (W * H * 4)
#define NODE_CAP 512
#define NODE_MAGIC 0x47444921u /* 'GDI!' */

typedef struct Node { unsigned magic, serial, self_lo, checksum; } Node;

/* rewound domain: logical state + the GDI/USER handles held from rewound memory */
typedef struct Arena {
	unsigned magic, clock, seed;
	HWND    hwnd;    /* USER object handle, held across the rewind */
	HDC     memdc;   /* GDI memory DC handle */
	HBITMAP dib;     /* GDI DIB section handle */
	unsigned gen;    /* generational handle for the GDI/USER objects */
	int free_head; unsigned in_use;
	Node node[NODE_CAP];
} Arena;

typedef struct Held {
	unsigned char *snapshot, *scratch;
	unsigned have_snap, saved_clock, saved_seed, saved_fp;
	unsigned gen;    /* present truth for the GDI/USER object generation */
	unsigned handles_valid, gdi_split_observed, gdi_reconciled, poisoned, pumped;
} Held;

static Arena *g_arena; static Held *g_held;
static void *g_dibbits;                 /* CPU pointer to the DIB pixels (present) */
static CRITICAL_SECTION g_cs;
static volatile LONG g_stop, g_quiesce, g_parked;
static HANDLE g_worker[2]; static int g_nworkers = 2;

static unsigned fnv1a(const void *p, size_t n){ const unsigned char*b=p; unsigned h=2166136261u; size_t i; for(i=0;i<n;i++){h^=b[i];h*=16777619u;} return h; }
static unsigned node_ck(const Node*n){ unsigned p[3]={n->magic,n->serial,n->self_lo}; return fnv1a(p,sizeof(p)); }

/* ---- GDI/USER setup ---- */
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l){ return DefWindowProcA(h, m, w, l); }

static int gdi_setup(void)
{
	WNDCLASSA wc; HWND hwnd; HDC memdc; HBITMAP dib; BITMAPINFO bmi;
	ZeroMemory(&wc, sizeof(wc)); wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "gdi_coexist_cls";
	RegisterClassA(&wc);
	hwnd = CreateWindowExA(0, "gdi_coexist_cls", "gdi_coexist", WS_OVERLAPPEDWINDOW, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);
	if (!hwnd) hwnd = CreateWindowExA(0, "gdi_coexist_cls", "gdi_coexist", 0, 0, 0, W, H, HWND_MESSAGE, NULL, wc.hInstance, NULL);
	if (!hwnd) { printf("CreateWindow failed %lu\n", GetLastError()); return 0; }

	memdc = CreateCompatibleDC(NULL);
	if (!memdc) { printf("CreateCompatibleDC failed\n"); return 0; }
	ZeroMemory(&bmi, sizeof(bmi));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bmi.bmiHeader.biWidth = W; bmi.bmiHeader.biHeight = -H; /* top-down */
	bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
	dib = CreateDIBSection(memdc, &bmi, DIB_RGB_COLORS, &g_dibbits, NULL, 0);
	if (!dib || !g_dibbits) { printf("CreateDIBSection failed\n"); return 0; }
	SelectObject(memdc, dib);

	g_arena->hwnd = hwnd; g_arena->memdc = memdc; g_arena->dib = dib; g_arena->gen = g_held->gen = 1;
	return 1;
}

/* Draw GDI content as a function of the logical seed. This is the GDI-side state
 * whose pixels live outside our snapshot. */
static void gdi_draw(HDC dc, unsigned seed)
{
	RECT full = { 0, 0, W, H };
	HBRUSH bg = CreateSolidBrush(RGB(20 + (seed * 37) % 200, 30, 120));
	HBRUSH fg = CreateSolidBrush(RGB(240, 60 + (seed * 53) % 180, 40));
	int x = (int)(seed * 17) % (W - 40), y = (int)(seed * 29) % (H - 40);
	RECT r = { x, y, x + 40, y + 40 };
	FillRect(dc, &full, bg);
	FillRect(dc, &r, fg);
	GdiFlush();
	DeleteObject(bg); DeleteObject(fg);
}
static unsigned gdi_fingerprint(void){ return fnv1a(g_dibbits, PIXBYTES); }

/* ---- arena workers (rewound game logic) ---- */
static void nodes_seed(void){ unsigned i; g_arena->in_use=0; for(i=0;i<NODE_CAP;i++){ Node*n=&g_arena->node[i]; if(i&1){ n->magic=NODE_MAGIC; n->serial=i*7+1; n->self_lo=i; n->checksum=node_ck(n); g_arena->in_use++; } else { n->magic=0; n->self_lo=i; n->serial=0; n->checksum=0; } } }
static unsigned identity_bad(const Arena*a){ unsigned bad=0,i; for(i=0;i<NODE_CAP;i++){ const Node*n=&a->node[i]; if(n->magic!=NODE_MAGIC) continue; if(n->self_lo!=i){bad++;continue;} if(n->checksum!=node_ck(n)){bad++;continue;} } return bad; }
static void quiesce(void){ InterlockedExchange(&g_quiesce,1); while(InterlockedCompareExchange(&g_parked,0,0)<g_nworkers && !g_stop) Sleep(0); }
static void release_workers(void){ InterlockedExchange(&g_quiesce,0); while(InterlockedCompareExchange(&g_parked,0,0)>0 && !g_stop) Sleep(0); }
static DWORD WINAPI worker(LPVOID p){
	int id=(int)(intptr_t)p; unsigned long long tr=0x51u^(unsigned)(id+1)*2654435761u; int parked=0;
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

/* ---- capture (verify) + restore ---- */
#define ARENA_BYTES (sizeof(Arena))
static int checkpoint(void){
	Arena*scr=(Arena*)g_held->scratch;
	quiesce();
	memcpy(scr,g_arena,ARENA_BYTES);
	if(identity_bad(scr)==0){ memcpy(g_held->snapshot,scr,ARENA_BYTES); g_held->have_snap=1; g_held->saved_clock=scr->clock; g_held->saved_seed=scr->seed; release_workers(); return 1; }
	release_workers(); return 0;
}
static void restore(void){ quiesce(); memcpy(g_arena,g_held->snapshot,ARENA_BYTES); } /* workers left parked */

static void pump(void){ MSG m; while(PeekMessageA(&m,NULL,0,0,PM_REMOVE)){ TranslateMessage(&m); DispatchMessageA(&m); g_held->pumped++; } }

int main(int argc,char**argv)
{
	int cycles = argc>1?atoi(argv[1]):200; int c,i;

	InitializeCriticalSection(&g_cs);
	g_held=(Held*)VirtualAlloc(NULL,sizeof(Held),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
	if(!g_held){printf("alloc failed\n");return 2;} memset(g_held,0,sizeof(Held));
	g_held->snapshot=(unsigned char*)VirtualAlloc(NULL,ARENA_BYTES,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
	g_held->scratch =(unsigned char*)VirtualAlloc(NULL,ARENA_BYTES,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
	g_arena=(Arena*)VirtualAlloc(NULL,ARENA_BYTES,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
	if(!g_held->snapshot||!g_held->scratch||!g_arena){printf("alloc failed\n");return 2;}
	memset(g_arena,0,ARENA_BYTES); g_arena->magic=NODE_MAGIC;

	if(!gdi_setup()){ printf("GDI/USER setup failed\n"); return 2; }
	nodes_seed();
	printf("pid=%lu  live window=%p memDC=%p DIB=%p; savestate loop WITH GDI/USER in the process\n",
	       (unsigned long)GetCurrentProcessId(), (void*)g_arena->hwnd, (void*)g_arena->memdc, (void*)g_arena->dib);
	printf("cycles=%d workers=%d\n", cycles, g_nworkers);

	for(i=0;i<g_nworkers;i++) g_worker[i]=CreateThread(NULL,0,worker,(LPVOID)(intptr_t)i,0,NULL);
	Sleep(30);

	for(c=0;c<cycles && !g_stop;c++){
		g_arena->clock++; g_arena->seed=(unsigned)(c+1);
		gdi_draw(g_arena->memdc, g_arena->seed);      /* GDI content for this cycle's state */
		if(!checkpoint()){ /* refused */ }
		/* saved_fp = the GDI content the saved state produced */
		g_held->saved_fp = gdi_fingerprint();

		/* --- divergence: mutate GDI-side content + churn arena + pump --- */
		gdi_draw(g_arena->memdc, g_arena->seed + 12345u);
		pump();

		restore();  /* rewinds arena (seed, handles, nodes); workers parked */

		/* (1) Class B: do the GDI/USER handles survive the rewind? */
		{
			int ok = IsWindow(g_arena->hwnd) &&
				 GetObjectType(g_arena->memdc) == OBJ_MEMDC &&
				 GetObjectType((HGDIOBJ)g_arena->dib) == OBJ_BITMAP;
			if (ok) g_held->handles_valid++;
		}
		/* (2) GDI content split: is the DIB rewound? (it is not) */
		if (gdi_fingerprint() != g_held->saved_fp) g_held->gdi_split_observed++;
		/* (2b) mitigation: reconcile by redrawing from the rewound logical state */
		gdi_draw(g_arena->memdc, g_arena->seed);
		if (gdi_fingerprint() == g_held->saved_fp) g_held->gdi_reconciled++;

		/* (arena consistency, workers still parked) */
		if (identity_bad(g_arena) != 0) g_held->poisoned++;
		release_workers();

		if((c%50)==0) printf("  cycle %d: handles_valid=%u gdi_split=%u gdi_reconciled=%u poisoned=%u pumped=%u\n",
			c, g_held->handles_valid, g_held->gdi_split_observed, g_held->gdi_reconciled, g_held->poisoned, g_held->pumped);
	}

	InterlockedExchange(&g_stop,1);
	for(i=0;i<g_nworkers;i++) if(g_worker[i]) WaitForSingleObject(g_worker[i],1000);

	printf("\nsummary (%d cycles, GDI/USER live throughout):\n", cycles);
	printf("  handles valid after restore : %u/%d  (Class B: kernel-side objects persist)\n", g_held->handles_valid, cycles);
	printf("  GDI content split observed  : %u/%d  (DIB pixels NOT rewound - outside the snapshot)\n", g_held->gdi_split_observed, cycles);
	printf("  GDI reconciled by redraw    : %u/%d  (mitigation: redraw from rewound state)\n", g_held->gdi_reconciled, cycles);
	printf("  arena poisoned              : %u\n", g_held->poisoned);
	printf("  messages pumped             : %u\n", g_held->pumped);
	{
		int no_block = 1; /* we reached here => loop completed => no deadlock */
		int handles_ok = (g_held->handles_valid == (unsigned)cycles);
		int split_real = (g_held->gdi_split_observed > 0);
		int reconciled = (g_held->gdi_reconciled == (unsigned)cycles);
		printf("\n%s: GDI/USER did not block the loop%s.\n",
		       (no_block && handles_ok && reconciled && g_held->poisoned==0) ? "PASS" : "PARTIAL",
		       no_block ? " (ran to completion, no deadlock)" : "");
		printf("finding: GDI/USER HANDLES survive the rewind (%s), but GDI CONTENT is a split like the audio cursor "
		       "(%s) and must be reconciled by redraw (%s).\n",
		       handles_ok ? "yes" : "NO",
		       split_real ? "observed" : "not observed",
		       reconciled ? "works" : "FAILED");
		DeleteDC(g_arena->memdc); DeleteObject((HGDIOBJ)g_arena->dib); DestroyWindow(g_arena->hwnd);
		DeleteCriticalSection(&g_cs);
		return (handles_ok && reconciled && g_held->poisoned==0) ? 0 : 1;
	}
}
