/* phase.c - what the process holds at fixed points of its life.
 *
 * A cross-launch load pastes one launch's memory into another, and whether a
 * reference survives depends on when its target was born. What the system
 * builds before any game code runs exists in both launches, built the same
 * way; what the game builds in play may exist in only one. These inventories
 * give every allocation, module, heap, thread and handle a birth phase, so a
 * load can decide by rule instead of by the crash it caused last time.
 *
 * Written as JSON Lines in the slot manifest's format (one object per line,
 * "k" names the kind - docs/slot-manifest-proposal.md), one file per phase, in
 * phases\ in the launch's log folder. D3D9SW_PHASES=1 turns it on.
 *
 * Every allocation in the address space is listed, each with one owner or
 * "unknown", and the closing "check" line says how much of it is accounted
 * for. A table that silently leaves things out cannot be trusted to say what
 * is missing, which is the only question it exists to answer. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdint.h>
#include "logdir.h"

extern IMAGE_DOS_HEADER __ImageBase;

typedef LONG(NTAPI *PFN_NtQSI)(ULONG, void *, ULONG, ULONG *);
typedef LONG(NTAPI *PFN_NtQIT)(HANDLE, ULONG, void *, ULONG, ULONG *);
typedef LONG(NTAPI *PFN_NtQO)(HANDLE, ULONG, void *, ULONG, ULONG *);
typedef LONG(NTAPI *PFN_QIP64)(HANDLE, ULONG, void *, ULONG, void *);
typedef LONG(NTAPI *PFN_RVM64)(HANDLE, ULONGLONG, void *, ULONGLONG, ULONGLONG *);
typedef DWORD(WINAPI *PFN_MAPNAME)(HANDLE, LPVOID, LPSTR, DWORD);

/* 32-bit layouts, from this machine's SysWOW64\ntdll symbols. */
#define PEB_LDR 0x0C
#define PEB_PARAMS 0x10
#define PEB_APISET 0x38
#define PEB_CSRSHARED 0x4C
#define PEB_SHAREDDATA 0x50
#define PEB_ANSICP 0x58
#define PEB_OEMCP 0x5C
#define PEB_CASETAB 0x60
#define PEB_NHEAPS 0x88
#define PEB_HEAPS 0x90
#define PEB_GDISHARED 0x94
#define PEB_SHIM 0x1E8
#define PEB_ACTCTX 0x1F8
#define PEB_SYSACTCTX 0x200
#define PEB_WERREG 0x230
#define PEB_TELEMETRY 0x45C
#define PEB_LEAPSEC 0x470
#define LDR_LIST 0x0C
#define LDRE_BASE 0x18
#define LDRE_SIZE 0x20
#define LDRE_FULL 0x24
#define LDRE_NAME 0x2C
#define TEB_STACKBASE 0x04
#define TEB_STACKLIMIT 0x08
#define TEB_DEALLOC 0xE0C
#define HEAP_SIG 0x60
#define HEAP_VBLOCKS 0x9C
#define HEAP_SEGS 0xA4
#define PARAMS_ENV 0x48

/* 64-bit layouts, from System32\ntdll. Below 4 GB in a WOW64 process. */
#define PEB64_PARAMS 0x20
#define PEB64_APISET 0x68
#define PEB64_CSRSHARED 0x88
#define PEB64_SHAREDDATA 0x90
#define PEB64_ANSICP 0xA0
#define PEB64_OEMCP 0xA8
#define PEB64_CASETAB 0xB0
#define PEB64_NHEAPS 0xE8
#define PEB64_HEAPS 0xF0
#define PEB64_GDISHARED 0xF8
#define PEB64_SHIM 0x2D8
#define PEB64_ACTCTX 0x2F8
#define PEB64_SYSACTCTX 0x308
#define PEB64_WERREG 0x358
#define PEB64_TELEMETRY 0x7A0
#define PEB64_LEAPSEC 0x7B8
#define TEB64_STACKBASE 0x08
#define TEB64_STACKLIMIT 0x10
#define TEB64_DEALLOC 0x1478
#define HEAP64_SIG 0x98
#define HEAP64_VBLOCKS 0x110
#define HEAP64_SEGS 0x120
#define PARAMS64_ENV 0x80

#define PH_MODS 512
#define PH_THREADS 256
#define PH_SPANS 4096
#define PH_SECS 64
#define PH_OUT (1u << 20)

typedef struct {
	uintptr_t base, size;
	DWORD stamp;
	char name[64], path[MAX_PATH];
} PhMod;

typedef struct {
	DWORD tid;
	uintptr_t teb, start, stk_lo, stk_hi, teb64, stk64_lo, stk64_hi;
} PhThr;

typedef struct {
	uintptr_t lo, hi;
	char owner[48];
} PhSpan;

static PhMod *g_mod;
static PhThr *g_thr;
static PhSpan *g_sp;
static int g_nmod, g_nthr, g_nsp;
/* The 64-bit reads refuse the GetCurrentProcess() pseudo-handle. */
static HANDLE g_self;
static char *g_out;
static DWORD g_outn;
static HANDLE g_fh;

static int readable(uintptr_t p, size_t n)
{
	MEMORY_BASIC_INFORMATION m;
	uintptr_t end = p + n;

	while (p < end) {
		if (!VirtualQuery((LPCVOID)p, &m, sizeof(m)) || m.State != MEM_COMMIT ||
		    (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !m.Protect)
			return 0;
		p = (uintptr_t)m.BaseAddress + m.RegionSize;
	}
	return 1;
}

static DWORD rd32(uintptr_t p)
{
	return readable(p, 4) ? *(const DWORD *)p : 0;
}

/* A 64-bit field, kept only when it lands below 4 GB where we can see it. */
static uintptr_t rd64lo(uintptr_t p)
{
	ULONGLONG v;

	if (!readable(p, 8))
		return 0;
	v = *(const ULONGLONG *)p;
	return v >> 32 ? 0 : (uintptr_t)v;
}

static void flush(void)
{
	DWORD w;

	if (g_fh && g_outn)
		WriteFile(g_fh, g_out, g_outn, &w, NULL);
	g_outn = 0;
}

static void out(const char *fmt, ...)
{
	char b[1100];
	va_list a;
	int n;

	va_start(a, fmt);
	n = wvsprintfA(b, fmt, a);
	va_end(a);
	if (n <= 0)
		return;
	if (g_outn + (DWORD)n > PH_OUT)
		flush();
	CopyMemory(g_out + g_outn, b, (DWORD)n);
	g_outn += (DWORD)n;
}

/* JSON string body: backslashes and quotes escaped, control bytes dropped. */
static const char *esc(const char *s, char *b, int cap)
{
	int k = 0;

	for (; *s && k < cap - 2; s++) {
		if ((unsigned char)*s < 0x20)
			continue;
		if (*s == '\\' || *s == '"')
			b[k++] = '\\';
		b[k++] = *s;
	}
	b[k] = 0;
	return b;
}

static void ustr(uintptr_t us, char *b, int cap)
{
	USHORT len = readable(us, 8) ? *(const USHORT *)us : 0;
	uintptr_t buf = len ? *(const uintptr_t *)(us + 4) : 0;

	b[0] = 0;
	if (buf && readable(buf, len)) {
		int n = WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)buf, len / 2, b, cap - 1, NULL, NULL);
		b[n > 0 ? n : 0] = 0;
	}
}

static const char *base_name(const char *p)
{
	const char *s = p;

	for (; *p; p++)
		if (*p == '\\' || *p == '/')
			s = p + 1;
	return s;
}

/* Named sections seen among the handles, for naming their views. */
static struct {
	uintptr_t size;
	char name[64];
	int used;
	BYTE head[256];
} g_sec[PH_SECS];
static int g_nsec;

/* The name with this process's id written as {pid}, so it pairs across launches. */
static void pid_norm(char *d, int cap, const char *s)
{
	char id[16];
	int n, k = 0;

	wsprintfA(id, "%lu", (unsigned long)GetCurrentProcessId());
	n = lstrlenA(id);
	while (*s && k < cap - 6) {
		int i;

		for (i = 0; i < n && s[i] == id[i]; i++)
			;
		if (i == n && (s[n] < '0' || s[n] > '9')) {
			lstrcpyA(d + k, "{pid}");
			k += 5;
			s += n;
		} else
			d[k++] = *s++;
	}
	d[k] = 0;
}

static void alloc_extent(uintptr_t p, uintptr_t *lo, uintptr_t *hi)
{
	MEMORY_BASIC_INFORMATION m;
	uintptr_t a, e;

	*lo = *hi = 0;
	if (!VirtualQuery((LPCVOID)p, &m, sizeof(m)) || m.State == MEM_FREE || !m.AllocationBase)
		return;
	a = e = (uintptr_t)m.AllocationBase;
	while (VirtualQuery((LPCVOID)e, &m, sizeof(m)) == sizeof(m) &&
	       (uintptr_t)m.AllocationBase == a && m.State != MEM_FREE)
		e += m.RegionSize;
	*lo = a;
	*hi = e;
}

static void span(uintptr_t lo, uintptr_t hi, const char *fmt, ...)
{
	va_list a;

	if (!lo || g_nsp >= PH_SPANS)
		return;
	if (hi <= lo)
		hi = lo + 1;
	g_sp[g_nsp].lo = lo;
	g_sp[g_nsp].hi = hi;
	va_start(a, fmt);
	wvsprintfA(g_sp[g_nsp].owner, fmt, a);
	va_end(a);
	g_nsp++;
}

/* The whole allocation an object lives in belongs to that object's owner. */
static void span_alloc(uintptr_t p, const char *fmt, ...)
{
	uintptr_t lo, hi;
	char o[48];
	va_list a;

	if (!p)
		return;
	alloc_extent(p, &lo, &hi);
	if (!lo)
		return;
	va_start(a, fmt);
	wvsprintfA(o, fmt, a);
	va_end(a);
	span(lo, hi, "%s", o);
}

static int mod_of(uintptr_t p, uintptr_t *off)
{
	int i;

	for (i = 0; i < g_nmod; i++)
		if (p >= g_mod[i].base && p < g_mod[i].base + g_mod[i].size) {
			*off = p - g_mod[i].base;
			return i;
		}
	return -1;
}

static const char *where(uintptr_t p, char *b)
{
	uintptr_t off;
	int m = p ? mod_of(p, &off) : -1;

	if (m >= 0)
		wsprintfA(b, "%s+%lX", g_mod[m].name, (unsigned long)off);
	else
		wsprintfA(b, "%08lX", (unsigned long)p);
	return b;
}

static void modules(uintptr_t peb)
{
	uintptr_t ldr = rd32(peb + PEB_LDR), head, e;
	char e1[600], e2[200];
	int guard = 0;

	if (!ldr)
		return;
	head = ldr + LDR_LIST;
	for (e = rd32(head); e && e != head && guard < 4096 && g_nmod < PH_MODS;
	     e = rd32(e), guard++) {
		PhMod *m = &g_mod[g_nmod];
		IMAGE_DOS_HEADER *dos;

		m->base = rd32(e + LDRE_BASE);
		m->size = rd32(e + LDRE_SIZE);
		ustr(e + LDRE_FULL, m->path, sizeof(m->path));
		ustr(e + LDRE_NAME, m->name, sizeof(m->name));
		m->stamp = 0;
		dos = (IMAGE_DOS_HEADER *)m->base;
		if (m->base && readable(m->base, sizeof(*dos)) && dos->e_magic == IMAGE_DOS_SIGNATURE &&
		    readable(m->base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS)))
			m->stamp = ((IMAGE_NT_HEADERS *)(m->base + dos->e_lfanew))->FileHeader.TimeDateStamp;
		out("{\"k\":\"module\",\"i\":%d,\"name\":\"%s\",\"base\":\"%08lX\",\"size\":\"%lX\","
		    "\"stamp\":\"%08lX\",\"path\":\"%s\"}\n",
		    g_nmod, esc(m->name, e2, sizeof(e2)), (unsigned long)m->base,
		    (unsigned long)m->size, (unsigned long)m->stamp, esc(m->path, e1, sizeof(e1)));
		span(m->base, m->base + m->size, "module:%s", m->name);
		g_nmod++;
	}
}

static int holders(uintptr_t a, uintptr_t e, uintptr_t peb, uintptr_t peb64, char *b, int cap);
static int holders64(uintptr_t a, uintptr_t e, char *b, int cap, int found);
static char g_best[96], g_baserefs[400];
static uintptr_t g_bestoff;

/* A heap's base allocation, its extra segments and its big blocks. */
static int heap_spans(uintptr_t h, int wide, const char *tag)
{
	uintptr_t heads[2], f;
	int l, k, n = 1;

	if (wide ? rd32(h + HEAP64_SIG) != 0xEEFFEEFFu : rd32(h + HEAP_SIG) != 0xEEFFEEFFu)
		return 0;
	span_alloc(h, "%s", tag);
	heads[0] = h + (wide ? HEAP64_SEGS : HEAP_SEGS);
	heads[1] = h + (wide ? HEAP64_VBLOCKS : HEAP_VBLOCKS);
	for (l = 0; l < 2; l++)
		for (f = wide ? rd64lo(heads[l]) : rd32(heads[l]), k = 0; f && f != heads[l] && k < 1024;
		     f = wide ? rd64lo(f) : rd32(f), k++) {
			uintptr_t lo, hi;

			alloc_extent(f, &lo, &hi);
			if (lo && lo != h) {
				span(lo, hi, "%s", tag);
				n++;
			}
		}
	return n;
}

static void heaps(uintptr_t peb, uintptr_t peb64, PFN_RVM64 rvm)
{
	DWORD n = rd32(peb + PEB_NHEAPS), i;
	uintptr_t arr = rd32(peb + PEB_HEAPS);
	char tag[48];

	HANDLE got[64];
	DWORD ng = GetProcessHeaps(64, got), k;

	for (i = 0; i < n && i < 64; i++) {
		uintptr_t h = rd32(arr + i * 4);
		int segs;

		if (!h)
			continue;
		wsprintfA(tag, i ? "heap:%lu" : "heap:process", (unsigned long)i);
		segs = heap_spans(h, 0, tag);
		out("{\"k\":\"heap\",\"i\":%lu,\"bits\":32,\"base\":\"%08lX\",\"owner\":\"%s\","
		    "\"allocations\":%d}\n",
		    (unsigned long)i, (unsigned long)h, tag, segs);
	}
	/* The PEB's list stops being kept after the process heap on current
	 * Windows; the API knows them all. The extra ones are named after the
	 * module variable holding their base, which is stable across launches. */
	for (k = 0; k < ng && k < 64; k++) {
		uintptr_t h = (uintptr_t)got[k];
		char refs[400];
		int segs;

		for (i = 0; i < n && i < 64 && rd32(arr + i * 4) != h; i++)
			;
		if (i < n && i < 64)
			continue;
		holders64(h, h + 1, refs, sizeof(refs), holders(h, h + 1, peb, peb64, refs, sizeof(refs)));
		if (g_best[0] && !g_bestoff)
			wsprintfA(tag, "heap:%.40s", g_best);
		else
			wsprintfA(tag, "heap:#%lu", (unsigned long)k);
		segs = heap_spans(h, 0, tag);
		out("{\"k\":\"heap\",\"i\":%lu,\"bits\":32,\"base\":\"%08lX\",\"owner\":\"%s\","
		    "\"allocations\":%d,\"source\":\"GetProcessHeaps\",\"base_refs\":[%s]}\n",
		    (unsigned long)k, (unsigned long)h, tag, segs, g_baserefs);
	}
	if (peb64 && rvm) {
		ULONGLONG list[32];
		DWORD n64 = rd32(peb64 + PEB64_NHEAPS);

		if (n64 > 32)
			n64 = 32;
		if (n64 && rvm(g_self, *(const ULONGLONG *)(peb64 + PEB64_HEAPS), list,
			       (ULONGLONG)n64 * 8, NULL) >= 0)
			for (i = 0; i < n64; i++) {
				int segs;

				if (!list[i] || list[i] >> 32)
					continue;
				wsprintfA(tag, i ? "heap64:%lu" : "heap64:process", (unsigned long)i);
				segs = heap_spans((uintptr_t)list[i], 1, tag);
				out("{\"k\":\"heap\",\"i\":%lu,\"bits\":64,\"base\":\"%08lX\","
				    "\"owner\":\"%s\",\"allocations\":%d}\n",
				    (unsigned long)i, (unsigned long)list[i], tag, segs);
			}
	}
}

static void threads(void)
{
	PFN_NtQIT qit = (PFN_NtQIT)GetProcAddress(GetModuleHandleA("ntdll.dll"),
						  "NtQueryInformationThread");
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	THREADENTRY32 te;
	DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
	char w1[96];

	if (snap == INVALID_HANDLE_VALUE)
		return;
	te.dwSize = sizeof(te);
	if (Thread32First(snap, &te))
		do {
			PhThr *t;
			HANDLE h;
			struct {
				LONG status;
				void *teb;
				void *pid, *tid;
				ULONG_PTR affinity;
				LONG prio, base;
			} tbi;
			uintptr_t lo, hi;

			if (te.th32OwnerProcessID != pid || g_nthr >= PH_THREADS)
				continue;
			t = &g_thr[g_nthr];
			ZeroMemory(t, sizeof(*t));
			t->tid = te.th32ThreadID;
			h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
			if (h && qit) {
				if (qit(h, 0, &tbi, sizeof(tbi), NULL) >= 0)
					t->teb = (uintptr_t)tbi.teb;
				qit(h, 9, &t->start, sizeof(t->start), NULL);
			}
			if (h)
				CloseHandle(h);
			/* NT_TIB.Self names the 32-bit block; if the query answered with
			 * the 64-bit one, ours is two pages above it. */
			if (t->teb && rd32(t->teb + 0x18) != (DWORD)t->teb &&
			    rd32(t->teb + 0x2000 + 0x18) == (DWORD)(t->teb + 0x2000))
				t->teb += 0x2000;
			if (t->teb && rd32(t->teb + 0x18) != (DWORD)t->teb)
				t->teb = 0;
			if (t->teb) {
				uintptr_t t64 = t->teb - 0x2000;

				span_alloc(t->teb, "teb:%d", g_nthr);
				alloc_extent(rd32(t->teb + TEB_STACKLIMIT), &lo, &hi);
				t->stk_lo = lo;
				t->stk_hi = hi;
				span(lo, hi, "stack:%d", g_nthr);
				if (readable(t64, 0x40) && rd32(t64 + 0x30) == (DWORD)t64 &&
				    !rd32(t64 + 0x34)) {
					t->teb64 = t64;
					span(t64, t64 + 0x2000, "teb64:%d", g_nthr);
					alloc_extent(rd64lo(t64 + TEB64_STACKLIMIT), &lo, &hi);
					t->stk64_lo = lo;
					t->stk64_hi = hi;
					span(lo, hi, "stack64:%d", g_nthr);
				}
			}
			out("{\"k\":\"thread\",\"i\":%d,\"tid\":%lu,\"self\":%d,\"start\":\"%s\","
			    "\"teb\":\"%08lX\",\"stack\":[\"%08lX\",\"%08lX\"],\"teb64\":\"%08lX\","
			    "\"stack64\":[\"%08lX\",\"%08lX\"]}\n",
			    g_nthr, (unsigned long)t->tid, t->tid == self, where(t->start, w1),
			    (unsigned long)t->teb, (unsigned long)t->stk_lo, (unsigned long)t->stk_hi,
			    (unsigned long)t->teb64, (unsigned long)t->stk64_lo,
			    (unsigned long)t->stk64_hi);
			g_nthr++;
		} while (Thread32Next(snap, &te));
	CloseHandle(snap);
}

/* What the process block points at, by name. */
static void peb_spans(uintptr_t peb, uintptr_t peb64)
{
	uintptr_t params = rd32(peb + PEB_PARAMS);

	span_alloc(peb, "peb");
	span_alloc(params, "process-parameters");
	span_alloc(rd32(params + PARAMS_ENV), "environment");
	span_alloc(rd32(peb + PEB_APISET), "apiset-map");
	span_alloc(rd32(peb + PEB_CSRSHARED), "csr-shared");
	span_alloc(rd32(peb + PEB_SHAREDDATA), "shared-data");
	span_alloc(rd32(peb + PEB_ANSICP), "nls-ansi");
	span_alloc(rd32(peb + PEB_OEMCP), "nls-oem");
	span_alloc(rd32(peb + PEB_CASETAB), "nls-case");
	span_alloc(rd32(peb + PEB_GDISHARED), "gdi-shared");
	span_alloc(rd32(peb + PEB_SHIM), "shim-data");
	span_alloc(rd32(peb + PEB_ACTCTX), "activation-context");
	span_alloc(rd32(peb + PEB_SYSACTCTX), "activation-context-system");
	span_alloc(rd32(peb + PEB_WERREG), "wer-registration");
	span_alloc(rd32(peb + PEB_TELEMETRY), "telemetry");
	span_alloc(rd32(peb + PEB_LEAPSEC), "leap-second");
	span(0x7FFE0000u, 0x7FFF0000u, "kuser-shared");
	if (peb64) {
		uintptr_t p64 = rd64lo(peb64 + PEB64_PARAMS);

		span_alloc(peb64, "peb64");
		span_alloc(p64, "process-parameters64");
		span_alloc(rd64lo(p64 + PARAMS64_ENV), "environment64");
		span_alloc(rd64lo(peb64 + PEB64_APISET), "apiset-map64");
		span_alloc(rd64lo(peb64 + PEB64_CSRSHARED), "csr-shared64");
		span_alloc(rd64lo(peb64 + PEB64_SHAREDDATA), "shared-data64");
		span_alloc(rd64lo(peb64 + PEB64_ANSICP), "nls-ansi64");
		span_alloc(rd64lo(peb64 + PEB64_OEMCP), "nls-oem64");
		span_alloc(rd64lo(peb64 + PEB64_CASETAB), "nls-case64");
		span_alloc(rd64lo(peb64 + PEB64_GDISHARED), "gdi-shared64");
		span_alloc(rd64lo(peb64 + PEB64_SHIM), "shim-data64");
		span_alloc(rd64lo(peb64 + PEB64_ACTCTX), "activation-context64");
		span_alloc(rd64lo(peb64 + PEB64_SYSACTCTX), "activation-context-system64");
		span_alloc(rd64lo(peb64 + PEB64_WERREG), "wer-registration64");
		span_alloc(rd64lo(peb64 + PEB64_TELEMETRY), "telemetry64");
		span_alloc(rd64lo(peb64 + PEB64_LEAPSEC), "leap-second64");
	}
}

/* The pairing key of an allocation nothing claimed: the module variable that
 * points nearest its base. A variable at the base is the allocation's own
 * pointer; one further in may only be passing through. */
static void best_holder(const char *w, uintptr_t off)
{
	const char *c;

	for (c = w; *c && *c != '+'; c++)
		;
	if (!*c)
		return;
	/* Every variable at the base is a key: one of them may be a cursor that
	 * happens to sit there in this launch, so pairing takes any match. */
	if (!off && lstrlenA(g_baserefs) + lstrlenA(w) + 4 < (int)sizeof(g_baserefs)) {
		if (g_baserefs[0])
			lstrcatA(g_baserefs, ",");
		lstrcatA(g_baserefs, "\"");
		lstrcatA(g_baserefs, w);
		lstrcatA(g_baserefs, "\"");
	}
	if (g_best[0] && off >= g_bestoff)
		return;
	lstrcpynA(g_best, w, sizeof(g_best) - 16);
	g_bestoff = off;
}

/* Who points at an allocation nothing claimed: the writable data of every
 * module, the process blocks and the thread blocks, read as pointers. The
 * holder names the owner - ntdll+1D2A8 with symbols is a variable's name. */
static int holder_in(uintptr_t lo, uintptr_t hi, uintptr_t a, uintptr_t e, char *b, int cap,
		     int found)
{
	MEMORY_BASIC_INFORMATION m;
	uintptr_t p = lo;
	char w1[96];

	while (p < hi) {
		if (!VirtualQuery((LPCVOID)p, &m, sizeof(m)))
			break;
		if (m.State == MEM_COMMIT && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
		    (m.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
				  PAGE_EXECUTE_WRITECOPY))) {
			uintptr_t q = (uintptr_t)m.BaseAddress, end = q + m.RegionSize;

			if (q < lo)
				q = lo;
			if (end > hi)
				end = hi;
			for (; q + 4 <= end; q += 4) {
				uintptr_t v = *(const uintptr_t *)q;

				if (v < a || v >= e)
					continue;
				where(q, w1);
				best_holder(w1, v - a);
				if (found < 3 && lstrlenA(b) + 100 < cap) {
					if (found)
						lstrcatA(b, ",");
					lstrcatA(b, "\"");
					lstrcatA(b, w1);
					lstrcatA(b, "\"");
				}
				found++;
			}
		}
		p = (uintptr_t)m.BaseAddress + m.RegionSize;
	}
	return found;
}

static int holders(uintptr_t a, uintptr_t e, uintptr_t peb, uintptr_t peb64, char *b, int cap)
{
	int i, found = 0;

	b[0] = 0;
	g_best[0] = 0;
	g_baserefs[0] = 0;
	for (i = 0; i < g_nmod; i++)
		found = holder_in(g_mod[i].base, g_mod[i].base + g_mod[i].size, a, e, b, cap, found);
	found = holder_in(peb, peb + 0x1000, a, e, b, cap, found);
	if (peb64)
		found = holder_in(peb64, peb64 + 0x1000, a, e, b, cap, found);
	for (i = 0; i < g_nthr; i++) {
		found = holder_in(g_thr[i].teb, g_thr[i].teb + 0x1000, a, e, b, cap, found);
		if (g_thr[i].teb64)
			found = holder_in(g_thr[i].teb64, g_thr[i].teb64 + 0x2000, a, e, b, cap,
					  found);
	}
	return found;
}

static uintptr_t g_peb, g_peb64;
static int g_held, g_child;

/* The 64-bit side of the process: ntdll, wow64*.dll. Their globals live above
 * 4 GB, so their writable sections are copied in once for the holder scan. */
#define PH_MODS64 16
typedef struct {
	ULONGLONG base;
	DWORD size;
	char name[64];
	BYTE *data;	   /* writable sections, back to back */
	ULONGLONG *va; /* start address of each copied page */
	int npages;
} PhMod64;
static PhMod64 g_mod64[PH_MODS64];
static int g_nmod64;

static int rq(PFN_RVM64 rvm, ULONGLONG a, void *b, ULONG n)
{
	ULONGLONG got = 0;

	return rvm(g_self, a, b, n, &got) >= 0 && got == n;
}

static void module64_data(PFN_RVM64 rvm, PhMod64 *m)
{
	BYTE hdr[0x1000];
	DWORD pe, i, nsec, opt;
	int pages = 0, cap = 0;

	if (!rq(rvm, m->base, hdr, sizeof(hdr)))
		return;
	pe = *(DWORD *)(hdr + 0x3C);
	if (pe > 0xE00 || *(DWORD *)(hdr + pe) != 0x4550)
		return;
	nsec = *(WORD *)(hdr + pe + 6);
	opt = *(WORD *)(hdr + pe + 20);
	for (i = 0; i < nsec && pe + 24 + opt + (i + 1) * 40 <= sizeof(hdr); i++) {
		BYTE *s = hdr + pe + 24 + opt + i * 40;

		if (*(DWORD *)(s + 36) & 0x80000000)
			cap += (int)((*(DWORD *)(s + 8) + 0xFFF) >> 12);
	}
	if (!cap)
		return;
	m->data = (BYTE *)VirtualAlloc(NULL, (SIZE_T)cap << 12, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	m->va = (ULONGLONG *)VirtualAlloc(NULL, cap * sizeof(ULONGLONG), MEM_COMMIT | MEM_RESERVE,
					  PAGE_READWRITE);
	if (!m->data || !m->va)
		return;
	span_alloc((uintptr_t)m->data, "phase-inventory");
	span_alloc((uintptr_t)m->va, "phase-inventory");
	for (i = 0; i < nsec && pe + 24 + opt + (i + 1) * 40 <= sizeof(hdr); i++) {
		BYTE *s = hdr + pe + 24 + opt + i * 40;
		DWORD rva = *(DWORD *)(s + 12), n = (*(DWORD *)(s + 8) + 0xFFF) >> 12, p;

		if (!(*(DWORD *)(s + 36) & 0x80000000))
			continue;
		for (p = 0; p < n && pages < cap; p++)
			if (rq(rvm, m->base + rva + ((ULONGLONG)p << 12), m->data + ((SIZE_T)pages << 12),
			       0x1000))
				m->va[pages++] = m->base + rva + ((ULONGLONG)p << 12);
	}
	m->npages = pages;
}

static void modules64(uintptr_t peb64, PFN_RVM64 rvm)
{
	ULONGLONG ldr = 0, head, link;
	char e1[600];
	int guard = 0;

	if (peb64)
		ldr = *(const ULONGLONG *)(peb64 + 0x18);
	if (!peb64 || !rvm || !ldr) {
		out("{\"k\":\"note\",\"text\":\"no 64-bit loader list (peb64 %08lX, ldr %08lX%08lX)\"}\n",
		    (unsigned long)peb64, (unsigned long)(ldr >> 32), (unsigned long)ldr);
		return;
	}
	head = ldr + 0x10;
	if (!rq(rvm, head, &link, 8)) {
		ULONGLONG got = 0;
		LONG st = rvm(g_self, head, &link, 8, &got);

		out("{\"k\":\"note\",\"text\":\"64-bit loader list unreadable (ldr %08lX%08lX, status %lX, got %lu)\"}\n",
		    (unsigned long)(ldr >> 32), (unsigned long)ldr, (unsigned long)st,
		    (unsigned long)got);
		return;
	}
	while (link && link != head && g_nmod64 < PH_MODS64 && guard++ < 64) {
		BYTE ent[0x70];
		WCHAR wname[64];
		PhMod64 *m = &g_mod64[g_nmod64];
		USHORT len;
		int k;

		if (!rq(rvm, link, ent, sizeof(ent)))
			break;
		ZeroMemory(m, sizeof(*m));
		m->base = *(ULONGLONG *)(ent + 0x30);
		m->size = *(DWORD *)(ent + 0x40);
		len = *(USHORT *)(ent + 0x58);
		if (len > sizeof(wname) - 2)
			len = sizeof(wname) - 2;
		ZeroMemory(wname, sizeof(wname));
		rq(rvm, *(ULONGLONG *)(ent + 0x60), wname, len);
		for (k = 0; wname[k] && k < 63; k++)
			m->name[k] = (char)wname[k];
		if (m->base) {
			if (!(m->base >> 32))
				span((uintptr_t)m->base, (uintptr_t)m->base + m->size, "module64:%.30s",
				     m->name);
			module64_data(rvm, m);
			out("{\"k\":\"module64\",\"name\":\"%s\",\"base\":\"%08lX%08lX\",\"size\":\"%lX\","
			    "\"data_pages\":%d}\n",
			    esc(m->name, e1, sizeof(e1)), (unsigned long)(m->base >> 32),
			    (unsigned long)m->base, (unsigned long)m->size, m->npages);
			g_nmod64++;
		}
		link = *(ULONGLONG *)ent;
	}
}

static int holders64(uintptr_t a, uintptr_t e, char *b, int cap, int found)
{
	int i, p, k;
	char w[96];

	for (i = 0; i < g_nmod64; i++)
		for (p = 0; p < g_mod64[i].npages; p++) {
			const ULONGLONG *q = (const ULONGLONG *)(g_mod64[i].data + ((SIZE_T)p << 12));

			for (k = 0; k < 512; k++) {
				if (q[k] < a || q[k] >= e)
					continue;
				wsprintfA(w, "%s64+%lX", g_mod64[i].name,
					  (unsigned long)(g_mod64[i].va[p] + k * 8 - g_mod64[i].base));
				best_holder(w, (uintptr_t)(q[k] - a));
				if (found < 3 && lstrlenA(b) + lstrlenA(w) + 4 < cap) {
					if (found)
						lstrcatA(b, ",");
					lstrcatA(b, "\"");
					lstrcatA(b, w);
					lstrcatA(b, "\"");
				}
				found++;
			}
		}
	return found;
}

static void modules64_free(void)
{
	int i;

	for (i = 0; i < g_nmod64; i++) {
		if (g_mod64[i].data)
			VirtualFree(g_mod64[i].data, 0, MEM_RELEASE);
		if (g_mod64[i].va)
			VirtualFree(g_mod64[i].va, 0, MEM_RELEASE);
	}
	g_nmod64 = 0;
}

/* A block nothing points at by its base may point at its parent: a heap's
 * side blocks link back into the heap from their header. The owner the header
 * points into most often - and only one that already has a name - is the parent. */
static const char *parent_of(uintptr_t a, uintptr_t e, char *tally, int cap)
{
	MEMORY_BASIC_INFORMATION m;
	const char *who[16];
	int votes[16], first[16], nw = 0, i, j, k, best = -1;
	DWORD firstv[16];
	uintptr_t p = a;

	while (p < e && VirtualQuery((LPCVOID)p, &m, sizeof(m))) {
		if (m.State == MEM_COMMIT && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
			const DWORD *w = (const DWORD *)m.BaseAddress;

			/* Only the header, only aligned words: further in, tables of
			 * small integer pairs read as addresses all over the space. */
			for (k = 0; k < 16; k++) {
				if ((w[k] >= a && w[k] < e) || (w[k] & 7))
					continue;
				for (i = 0; i < g_nsp; i++)
					if (w[k] >= g_sp[i].lo && w[k] < g_sp[i].hi)
						break;
				if (i == g_nsp || !lstrcmpA(g_sp[i].owner, "phase-inventory"))
					continue;
				for (j = 0; j < nw && lstrcmpA(who[j], g_sp[i].owner); j++)
					;
				if (j == nw) {
					if (nw == 16)
						continue;
					who[nw] = g_sp[i].owner;
					first[nw] = k * 4;
					firstv[nw] = w[k];
					votes[nw++] = 0;
				}
				votes[j]++;
			}
			break;
		}
		p = (uintptr_t)m.BaseAddress + m.RegionSize;
	}
	tally[0] = 0;
	for (j = 0; j < nw; j++) {
		char t[80];

		/* One stray word is chance; a list link back is two or more. */
		if (votes[j] >= 2 && (best < 0 || votes[j] > votes[best]))
			best = j;
		wsprintfA(t, "%s\"%.40s=%d first +%X:%08lX\"", j ? "," : "", who[j], votes[j],
			  first[j], (unsigned long)firstv[j]);
		if (lstrlenA(tally) + lstrlenA(t) < cap)
			lstrcatA(tally, t);
	}
	return best < 0 ? NULL : who[best];
}

/* Nothing names it and nothing points at it: its first words are the last clue. */
static void head_of(uintptr_t a, uintptr_t e, char *b, int cap)
{
	MEMORY_BASIC_INFORMATION m;
	uintptr_t p = a;
	char w[24];
	int k;

	while (p < e && VirtualQuery((LPCVOID)p, &m, sizeof(m))) {
		if (m.State == MEM_COMMIT && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
			if (b[0] && lstrlenA(b) + 2 < cap)
				lstrcatA(b, ",");
			wsprintfA(w, "\"head %08lX:", (unsigned long)m.BaseAddress);
			lstrcatA(b, w);
			for (k = 0; k < 12 && lstrlenA(b) + 12 < cap; k++) {
				wsprintfA(w, " %08lX", ((const DWORD *)m.BaseAddress)[k]);
				lstrcatA(b, w);
			}
			lstrcatA(b, "\"");
			return;
		}
		p = (uintptr_t)m.BaseAddress + m.RegionSize;
	}
}

static const char *kind_name(DWORD type)
{
	return type == MEM_IMAGE ? "image" : type == MEM_MAPPED ? "mapped" : "private";
}

/* Every allocation, low to high, with the owners whose spans touch it. */
/* 0 when s starts with prefix, like strncmp. */
static int StrPrefix(const char *s, const char *prefix)
{
	while (*prefix)
		if (*s++ != *prefix++)
			return 1;
	return 0;
}

/* Pass 0 only finds the held allocations, so that pass 1 can name a child
 * after a held parent whichever comes first in the address space. */
static void allocations(int pass, int *n, int *unknown, unsigned long *unknown_kb, int *multi,
			int *images_unlisted, unsigned long *total_kb)
{
	PFN_MAPNAME mapname = (PFN_MAPNAME)GetProcAddress(GetModuleHandleA("kernel32.dll"),
							  "K32GetMappedFileNameA");
	SYSTEM_INFO si;
	uintptr_t p;

	GetSystemInfo(&si);
	p = (uintptr_t)si.lpMinimumApplicationAddress;
	while (p < (uintptr_t)si.lpMaximumApplicationAddress) {
		MEMORY_BASIC_INFORMATION m;
		uintptr_t a, e, commit = 0;
		DWORD type, aprot;
		char owners[160], path[MAX_PATH], e1[600], refby[400], tally[400];
		int i, no = 0;

		if (!VirtualQuery((LPCVOID)p, &m, sizeof(m)))
			break;
		if (m.State == MEM_FREE) {
			p = (uintptr_t)m.BaseAddress + m.RegionSize;
			continue;
		}
		a = e = (uintptr_t)m.AllocationBase;
		type = m.Type;
		aprot = m.AllocationProtect;
		while (VirtualQuery((LPCVOID)e, &m, sizeof(m)) == sizeof(m) &&
		       (uintptr_t)m.AllocationBase == a && m.State != MEM_FREE) {
			if (m.State == MEM_COMMIT)
				commit += m.RegionSize;
			e += m.RegionSize;
		}
		if (e <= p)
			e = p + 0x1000;
		owners[0] = 0;
		for (i = 0; i < g_nsp; i++) {
			int dup = 0, k;

			if (g_sp[i].hi <= a || g_sp[i].lo >= e || g_sp[i].owner[0] == '>')
				continue;
			for (k = 0; k < i && !dup; k++)
				dup = g_sp[k].hi > a && g_sp[k].lo < e &&
				      !lstrcmpA(g_sp[k].owner, g_sp[i].owner);
			if (dup)
				continue;
			if (no && lstrlenA(owners) + lstrlenA(g_sp[i].owner) + 2 < (int)sizeof(owners))
				lstrcatA(owners, "|");
			if (lstrlenA(owners) + lstrlenA(g_sp[i].owner) + 1 < (int)sizeof(owners))
				lstrcatA(owners, g_sp[i].owner);
			no++;
		}
		path[0] = 0;
		if (type != MEM_PRIVATE && mapname)
			mapname(GetCurrentProcess(), (LPVOID)a, path, sizeof(path));
		if (!no && type == MEM_MAPPED && path[0]) {
			lstrcpynA(owners, "file:", sizeof(owners));
			lstrcatA(owners, base_name(path));
			no = 1;
		}
		if (!no && type == MEM_MAPPED && !path[0]) {
			int k, hit = -1, hits = 0;

			for (k = 0; k < g_nsec; k++) {
				MEMORY_BASIC_INFORMATION m;

				if (g_sec[k].size != e - a)
					continue;
				if (VirtualQuery((LPCVOID)a, &m, sizeof(m)) && m.State == MEM_COMMIT &&
				    !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
				    memcmp((const void *)a, g_sec[k].head, sizeof(g_sec[0].head)))
					continue;
				hit = k;
				hits++;
			}
			/* Only one candidate with the same size and bytes is evidence. */
			if (hits == 1) {
				lstrcpyA(owners, "section:");
				pid_norm(owners + 8, (int)sizeof(owners) - 8, g_sec[hit].name);
				no = 1;
			}
		}
		if (!no && type == MEM_IMAGE) {
			lstrcpynA(owners, "image-unlisted:", sizeof(owners));
			lstrcatA(owners, path[0] ? base_name(path) : "?");
			(*images_unlisted)++;
			no = 1;
		}
		refby[0] = 0;
		tally[0] = 0;
		g_baserefs[0] = 0;
		if (!no) {
			holders64(a, e, refby, sizeof(refby),
				  holders(a, e, g_peb, g_peb64, refby, sizeof(refby)));
			/* Marked '>' so it serves only as a parent for others. */
			if (!pass && g_best[0] && !g_bestoff)
				span(a, e, ">held:%.40s", g_best);
		}
		if (!pass) {
			p = e;
			continue;
		}
		/* The header comes first: it is the block's own structure, while a
		 * variable at the base may be a cursor that stopped there this launch. */
		if (!no && type == MEM_PRIVATE) {
			const char *parent = parent_of(a, e, tally, sizeof(tally));

			if (parent) {
				wsprintfA(owners, "child:%.48s:%s:%lX", parent + (*parent == '>'),
					  kind_name(type), (unsigned long)(e - a));
				g_child++;
				no = 1;
			}
		}
		/* module+offset is the same in every launch of one build. A
		 * pointer further in is a cursor and moves between launches. */
		if (!no && g_best[0] && !g_bestoff) {
			wsprintfA(owners, "held:%s", g_best);
			g_held++;
			no = 1;
		}
		if (!no) {
			wsprintfA(owners, "unknown:%s:%lX:%lX", kind_name(type), (unsigned long)(e - a),
				  (unsigned long)aprot);
			(*unknown)++;
			*unknown_kb += (unsigned long)((e - a) >> 10);
		}
		if (!StrPrefix(owners, "held:") || !StrPrefix(owners, "child:") ||
		    !StrPrefix(owners, "unknown:"))
			head_of(a, e, refby, sizeof(refby));
		if (no > 1)
			(*multi)++;
		*total_kb += (unsigned long)((e - a) >> 10);
		out("{\"k\":\"alloc\",\"i\":%d,\"base\":\"%08lX\",\"reserve\":\"%lX\",\"commit\":\"%lX\","
		    "\"type\":\"%s\",\"prot\":\"%lX\",\"owner\":\"%s\"%s%s%s%s%s%s%s%s%s%s%s%s}\n",
		    *n, (unsigned long)a, (unsigned long)(e - a), (unsigned long)commit,
		    kind_name(type), (unsigned long)aprot, owners, path[0] ? ",\"path\":\"" : "",
		    path[0] ? esc(path, e1, sizeof(e1)) : "", path[0] ? "\"" : "",
		    g_baserefs[0] ? ",\"base_refs\":[" : "", g_baserefs, g_baserefs[0] ? "]" : "",
		    tally[0] ? ",\"parent_votes\":[" : "", tally, tally[0] ? "]" : "",
		    refby[0] ? ",\"refby\":[" : "", refby, refby[0] ? "]" : "");
		(*n)++;
		p = e;
	}
}

/* Kernel handles, by type and, where asking cannot block, by name. A marker
 * event made just for this proves the walk reads this process's entries with
 * the right layout before any of it is believed. */
static void handles(int *nh, const char **selftest)
{
	HMODULE nt = GetModuleHandleA("ntdll.dll");
	PFN_NtQSI qsi = (PFN_NtQSI)GetProcAddress(nt, "NtQuerySystemInformation");
	PFN_NtQO qo = (PFN_NtQO)GetProcAddress(nt, "NtQueryObject");
	PFN_NtQO qs = (PFN_NtQO)GetProcAddress(nt, "NtQuerySection");
	HANDLE marker = CreateEventA(NULL, TRUE, FALSE, NULL);
	ULONG cap = 1u << 22, need = 0;
	unsigned char *buf = NULL;
	char tnames[256][32];
	unsigned char tknown[256];
	ULONG_PTR count, i;
	DWORD pid = GetCurrentProcessId();
	int found = 0;

	*selftest = "not run";
	ZeroMemory(tknown, sizeof(tknown));
	if (!qsi || !qo)
		goto done;
	for (;;) {
		LONG st;

		buf = (unsigned char *)VirtualAlloc(NULL, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!buf)
			goto done;
		st = qsi(64, buf, cap, &need);
		if (st >= 0)
			break;
		VirtualFree(buf, 0, MEM_RELEASE);
		buf = NULL;
		if (cap >= (1u << 28))
			goto done;
		cap <<= 1;
	}
	/* SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX, 28 bytes in a 32-bit view. */
	count = *(ULONG_PTR *)buf;
	for (i = 0; i < count; i++) {
		unsigned char *e = buf + 8 + i * 28;
		ULONG_PTR owner = *(ULONG_PTR *)(e + 4), hv = *(ULONG_PTR *)(e + 8);
		ULONG access = *(ULONG *)(e + 12);
		USHORT ti = *(USHORT *)(e + 18);
		HANDLE h = (HANDLE)hv;
		char name[MAX_PATH], e1[600];
		const char *tn;

		if (owner != pid)
			continue;
		if (!tknown[ti & 255]) {
			unsigned char tb[512];

			tnames[ti & 255][0] = 0;
			if (qo(h, 2, tb, sizeof(tb), NULL) >= 0)
				ustr((uintptr_t)tb, tnames[ti & 255], sizeof(tnames[0]));
			tknown[ti & 255] = 1;
		}
		tn = tnames[ti & 255];
		name[0] = 0;
		if (!lstrcmpA(tn, "File")) {
			DWORD ft = GetFileType(h);

			if (ft == FILE_TYPE_DISK)
				GetFinalPathNameByHandleA(h, name, sizeof(name), 0);
			else
				lstrcpyA(name, ft == FILE_TYPE_PIPE   ? "<pipe>"
					       : ft == FILE_TYPE_CHAR ? "<char>"
								  : "<other>");
		} else if (!lstrcmpA(tn, "Key") || !lstrcmpA(tn, "Directory") ||
			   !lstrcmpA(tn, "Section") || !lstrcmpA(tn, "Event") ||
			   !lstrcmpA(tn, "Mutant") || !lstrcmpA(tn, "Semaphore") ||
			   !lstrcmpA(tn, "SymbolicLink") || !lstrcmpA(tn, "Desktop") ||
			   !lstrcmpA(tn, "WindowStation") || !lstrcmpA(tn, "ALPC Port")) {
			unsigned char nb[1024];

			if (qo(h, 1, nb, sizeof(nb), NULL) >= 0)
				ustr((uintptr_t)nb, name, sizeof(name));
		}
		if (h == marker) {
			found = !lstrcmpA(tn, "Event");
			continue;
		}
		if (h == g_self || h == g_fh)
			continue;
		/* A pagefile-backed view has no file name. Our own short-lived view
		 * of the named section gives its size and first bytes, and a view of
		 * the same size holding the same bytes is that section's. */
		if (!lstrcmpA(tn, "Section") && name[0] && (access & 4) && g_nsec < PH_SECS) {
			void *v = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
			MEMORY_BASIC_INFORMATION m;
			uintptr_t sz = 0, q;

			(void)qs;
			if (v) {
				for (q = (uintptr_t)v; VirtualQuery((LPCVOID)q, &m, sizeof(m)) &&
						       m.AllocationBase == v && m.State != MEM_FREE;
				     q += m.RegionSize)
					sz += m.RegionSize;
				ZeroMemory(g_sec[g_nsec].head, sizeof(g_sec[0].head));
				if (VirtualQuery(v, &m, sizeof(m)) && m.State == MEM_COMMIT)
					CopyMemory(g_sec[g_nsec].head, v, sizeof(g_sec[0].head));
				UnmapViewOfFile(v);
			}
			if (sz) {
				g_sec[g_nsec].size = sz;
				lstrcpynA(g_sec[g_nsec].name, base_name(name), sizeof(g_sec[0].name));
				g_sec[g_nsec].used = 0;
				out("{\"k\":\"handle\",\"h\":\"%lX\",\"type\":\"%s\",\"access\":\"%lX\","
				    "\"name\":\"%s\",\"size\":\"%lX\"}\n",
				    (unsigned long)hv, tn, (unsigned long)access, esc(name, e1, sizeof(e1)),
				    (unsigned long)g_sec[g_nsec].size);
				g_nsec++;
				(*nh)++;
				continue;
			}
		}
		out("{\"k\":\"handle\",\"h\":\"%lX\",\"type\":\"%s\",\"access\":\"%lX\",\"name\":\"%s\"}\n",
		    (unsigned long)hv, tn, (unsigned long)access, esc(name, e1, sizeof(e1)));
		(*nh)++;
	}
	*selftest = found ? "ok" : "FAILED - the marker event was not found as an Event, the "
				   "handle lines are not to be trusted";
done:
	if (buf)
		VirtualFree(buf, 0, MEM_RELEASE);
	if (marker)
		CloseHandle(marker);
}

static int enabled(char *dir)
{
	char v[8], cfg[MAX_PATH + 32];
	DWORD n = GetEnvironmentVariableA("D3D9SW_PHASES", v, sizeof(v));
	HANDLE f;

	if (n > 0 && n < sizeof(v))
		return v[0] == '1';
	lstrcpyA(cfg, dir);
	lstrcatA(cfg, "d3d9_sw.cfg");
	f = CreateFileA(cfg, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
			NULL);
	if (f != INVALID_HANDLE_VALUE) {
		char b[8192];
		DWORD got = 0, i;

		ReadFile(f, b, sizeof(b) - 1, &got, NULL);
		CloseHandle(f);
		b[got] = 0;
		for (i = 0; i + 14 < got; i++)
			if ((i == 0 || b[i - 1] == '\n') && !_strnicmp(b + i, "D3D9SW_PHASES", 13)) {
				char *q = b + i + 13;

				while (*q == ' ' || *q == '\t')
					q++;
				if (*q == '=') {
					q++;
					while (*q == ' ' || *q == '\t')
						q++;
					return *q == '1';
				}
			}
	}
	return 0;
}

void phase_snapshot(const char *phase)
{
	static int ordinal;
	char dir[MAX_PATH], path[MAX_PATH + 64], e1[600];
	uintptr_t peb, peb64 = 0;
	PFN_QIP64 qip;
	PFN_RVM64 rvm;
	int nalloc = 0, unknown = 0, multi = 0, unlisted = 0, nh = 0, i, unmapped = 0;
	unsigned long unknown_kb = 0, total_kb = 0;
	const char *selftest;
	DWORD t0 = GetTickCount();
	FILETIME ft;
	ULONGLONG now, boot;
	char *slash;

	GetModuleFileNameA((HMODULE)&__ImageBase, dir, sizeof(dir));
	slash = dir;
	for (i = 0; dir[i]; i++)
		if (dir[i] == '\\')
			slash = dir + i + 1;
	*slash = 0;
	if (!enabled(dir))
		return;
	ordinal++;
	g_mod = (PhMod *)VirtualAlloc(NULL, sizeof(PhMod) * PH_MODS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	g_thr = (PhThr *)VirtualAlloc(NULL, sizeof(PhThr) * PH_THREADS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	g_sp = (PhSpan *)VirtualAlloc(NULL, sizeof(PhSpan) * PH_SPANS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	g_out = (char *)VirtualAlloc(NULL, PH_OUT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!g_mod || !g_thr || !g_sp || !g_out)
		goto free;
	g_nmod = g_nthr = g_nsp = g_held = g_child = g_nsec = 0;
	g_outn = 0;
	{
		char leaf[96];

		wsprintfA(leaf, "phases\\%lu_%d_%s.jsonl", (unsigned long)GetCurrentProcessId(),
			  ordinal, phase);
		if (!swlog_path(leaf, path, sizeof(path)))
			goto free;
	}
	g_fh = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
	if (g_fh == INVALID_HANDLE_VALUE) {
		g_fh = NULL;
		goto free;
	}
	/* Our own working memory, named so it is not mistaken for the process's. */
	span_alloc((uintptr_t)g_mod, "phase-inventory");
	span_alloc((uintptr_t)g_thr, "phase-inventory");
	span_alloc((uintptr_t)g_sp, "phase-inventory");
	span_alloc((uintptr_t)g_out, "phase-inventory");

	peb = *(const uintptr_t *)((const char *)NtCurrentTeb() + 0x30);
	qip = (PFN_QIP64)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtWow64QueryInformationProcess64");
	rvm = (PFN_RVM64)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtWow64ReadVirtualMemory64");
	if (qip) {
		struct {
			LONG status;
			ULONG pad;
			ULONGLONG peb, affinity;
			LONG prio;
			ULONG pad2;
			ULONGLONG pid, ppid;
		} pbi;

		ZeroMemory(&pbi, sizeof(pbi));
		if (qip(GetCurrentProcess(), 0, &pbi, sizeof(pbi), NULL) >= 0 && pbi.peb &&
		    !(pbi.peb >> 32))
			peb64 = (uintptr_t)pbi.peb;
	}
	GetSystemTimeAsFileTime(&ft);
	now = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	boot = (now - GetTickCount64() * 10000ull) / 10000000ull;
	{
		char exe[MAX_PATH];

		GetModuleFileNameA(NULL, exe, sizeof(exe));
		out("{\"k\":\"header\",\"v\":1,\"phase\":\"%s\",\"ordinal\":%d,\"pid\":%lu,"
		    "\"exe\":\"%s\",\"boot\":\"%lu\",\"peb\":\"%08lX\",\"peb64\":\"%08lX\"}\n",
		    phase, ordinal, (unsigned long)GetCurrentProcessId(), esc(exe, e1, sizeof(e1)),
		    /* Boot time to the minute: the same boot reads the same here. */
		    (unsigned long)(boot / 60), (unsigned long)peb, (unsigned long)peb64);
	}
	g_peb = peb;
	g_peb64 = peb64;
	g_self = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE,
			     GetCurrentProcessId());
	if (!g_self)
		rvm = NULL;
	modules(peb);
	modules64(peb64, rvm);
	threads();
	heaps(peb, peb64, rvm);
	peb_spans(peb, peb64);
	handles(&nh, &selftest);
	allocations(0, &nalloc, &unknown, &unknown_kb, &multi, &unlisted, &total_kb);
	nalloc = unlisted = 0;
	allocations(1, &nalloc, &unknown, &unknown_kb, &multi, &unlisted, &total_kb);
	for (i = 0; i < g_nmod; i++) {
		MEMORY_BASIC_INFORMATION m;

		if (!VirtualQuery((LPCVOID)g_mod[i].base, &m, sizeof(m)) || m.Type != MEM_IMAGE)
			unmapped++;
	}
	out("{\"k\":\"check\",\"allocations\":%d,\"total_kb\":%lu,\"held\":%d,\"child\":%d,"
	    "\"unknown\":%d,\"unknown_kb\":%lu,\"multi_owner\":%d,\"images_unlisted\":%d,"
	    "\"modules_not_mapped\":%d,\"modules64\":%d,\"threads\":%d,\"handles\":%d,"
	    "\"handle_selftest\":\"%s\",\"ms\":%lu}\n",
	    nalloc, total_kb, g_held, g_child, unknown, unknown_kb, multi, unlisted, unmapped, g_nmod64,
	    g_nthr, nh, selftest, (unsigned long)(GetTickCount() - t0));
	flush();
	CloseHandle(g_fh);
	g_fh = NULL;
free:
	modules64_free();
	if (g_self)
		CloseHandle(g_self);
	g_self = NULL;
	if (g_mod)
		VirtualFree(g_mod, 0, MEM_RELEASE);
	if (g_thr)
		VirtualFree(g_thr, 0, MEM_RELEASE);
	if (g_sp)
		VirtualFree(g_sp, 0, MEM_RELEASE);
	if (g_out)
		VirtualFree(g_out, 0, MEM_RELEASE);
	g_mod = NULL;
	g_thr = NULL;
	g_sp = NULL;
	g_out = NULL;
}
