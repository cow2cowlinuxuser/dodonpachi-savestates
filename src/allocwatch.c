/* Surviving the game's display-list overrun.
 *
 * default+0x9b730 flattens a linked list of 68-byte draw records into a buffer,
 * bounded only by a record count and with no check against the destination. In
 * training mode a stage restart leaves the previous attempt's list alive, so the
 * next attempt submits 15600 records into a buffer that holds roughly half that
 * and the copy walks off the end. Ten dumps from a vanilla machine and three
 * crashes here agree on the offset and on the count of 15600.
 *
 * Two repairs were tried before this one. Finding the buffer by shape - private,
 * committed, read/write, exactly 512 KB - identifies it in the vanilla process
 * and finds nothing at all under this wrapper. Committing pages behind it as the
 * copy reached them worked in a synthetic test and failed twice in the game,
 * because the buffer here is a heap block at 0c4e8040 rather than a standalone
 * allocation, and what follows a heap block is arbitrary: reserved runs, free
 * holes too narrow and too misaligned to reserve, and eventually memory that
 * belongs to somebody else. Feeding the copy would have meant handing the game
 * half a megabyte of another allocation to overwrite.
 *
 * So the copy is stopped instead, which is what the missing bounds check would
 * have done. The loop keeps its state in the frame the disassembly names:
 *
 *   00b4b72d  dec dword ptr [ebp-3Ch]   the records still to go
 *   00b4b730  rep movsd                 <- faults here
 *   00b4b735  jne 00b4b720              round again
 *   00b4b747  mov ecx,[ebp-34h]         the byte count reported afterwards
 *
 * Clearing ECX turns the faulting rep movsd into a no-op, setting ZF sends the
 * jne out of the loop, and [ebp-34h] is corrected to the bytes actually stored
 * so nothing downstream reads records that were never written. The frame is
 * only touched behind an exact match on the faulting instruction, so it applies
 * to this build of this executable and to nothing else.
 *
 * The records that did not fit are dropped, so a frame over capacity draws
 * short. That is a visible symptom rather than a hidden one, and it beats both
 * a crash and a silent write into another allocation.
 */

#include "allocwatch.h"

#include <windows.h>

#include <stdarg.h>
#include <stdint.h>

#ifndef EXCEPTION_ACCESS_VIOLATION
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005L
#endif

#define AW_SITE_RVA 0x9B730u
#define AW_RECORD_BYTES 68u

/* All read off the disassembly of the function containing the faulting copy. */
#define AW_EBP_TOTAL_BYTES 0x34 /* [ebp-34h], bytes the caller is told were written */
#define AW_EBP_REMAINING 0x3C   /* [ebp-3Ch], records still to copy */
#define AW_EBP_TARGET 0x08      /* [ebp+8],  the object holding the destination */
#define AW_TARGET_BASE 0x08     /* [target+8] */
#define AW_EBX_BUFFER 0x184     /* [ebx+184h] */

/* The game is PE32 and the repair reaches into a 32-bit frame by name, so there
 * is nothing here for the x64 builds of this wrapper to do. */
#if defined(__i386__)

static void *g_site;
static volatile LONG g_dumped;
static volatile LONG g_catches;
static int g_want_dump;

/* No CRT in here. The handler runs on a faulting thread that is about to be
 * resumed rather than unwound, and savestate.c already learned what it costs to
 * take an allocating path from a fault handler. */
static void aw_raw(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	HANDLE h;
	DWORD wrote;
	int n;

	va_start(ap, fmt);
	n = wvsprintfA(line, fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	h = CreateFileA("d3d9_sw_allocwatch.txt", FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
			OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;
	SetFilePointer(h, 0, NULL, FILE_END);
	WriteFile(h, line, (DWORD)n, &wrote, NULL);
	CloseHandle(h);
}

/* Through the process API rather than a dereference: a stray pointer read from
 * inside an exception handler would fault recursively, and every pointer used
 * here was reconstructed from a register. */
static int aw_peek(uintptr_t at, DWORD *out)
{
	SIZE_T got = 0;
	if (!ReadProcessMemory(GetCurrentProcess(), (void *)at, out, sizeof(*out), &got))
		return 0;
	return got == sizeof(*out);
}

static int aw_poke(uintptr_t at, DWORD v)
{
	SIZE_T put = 0;
	if (!WriteProcessMemory(GetCurrentProcess(), (void *)at, &v, sizeof(v), &put))
		return 0;
	return put == sizeof(v);
}

static void aw_dump(EXCEPTION_POINTERS *ep)
{
	typedef BOOL(WINAPI * pfn_mdwd)(HANDLE, DWORD, HANDLE, DWORD, void *, void *, void *);
	struct {
		DWORD thread_id;
		EXCEPTION_POINTERS *ep;
		BOOL client_pointers;
	} mei;
	HMODULE dbg;
	pfn_mdwd write_dump;
	char path[MAX_PATH];
	HANDLE f;

	dbg = LoadLibraryA("dbghelp.dll");
	if (!dbg)
		return;
	write_dump = (pfn_mdwd)(void *)GetProcAddress(dbg, "MiniDumpWriteDump");
	if (!write_dump)
		return;

	wsprintfA(path, "d3d9_sw_overrun_%lu.dmp", GetCurrentProcessId());
	f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;

	mei.thread_id = GetCurrentThreadId();
	mei.ep = ep;
	mei.client_pointers = FALSE;

	/* FullMemory | HandleData | UnloadedModules | FullMemoryInfo | ThreadInfo.
	 * The vanilla dumps were written without FullMemoryInfo, which is why
	 * !address could say nothing about the buffer that had just overrun. */
	if (write_dump(GetCurrentProcess(), GetCurrentProcessId(), f,
		       0x0002 | 0x0004 | 0x0020 | 0x0800 | 0x1000, &mei, NULL, NULL))
		aw_raw("wrote %s\r\n", path);
	CloseHandle(f);
}

static int aw_truncate(EXCEPTION_POINTERS *ep, DWORD *out_kept, DWORD *out_dropped)
{
	CONTEXT *c = ep->ContextRecord;
	DWORD target, base, buffer, start, kept;

	if (!aw_peek(c->Ebp + AW_EBP_TARGET, &target))
		return 0;
	if (!aw_peek((uintptr_t)target + AW_TARGET_BASE, &base))
		return 0;
	if (!aw_peek(c->Ebx + AW_EBX_BUFFER, &buffer))
		return 0;
	start = base + buffer;

	/* EDX is advanced to one record past the destination before the copy runs,
	 * so the record boundary behind it is the last one fully stored. */
	if (c->Edx < start + AW_RECORD_BYTES)
		return 0;
	kept = (c->Edx - AW_RECORD_BYTES) - start;
	if (kept % AW_RECORD_BYTES)
		return 0; /* not the frame layout we think it is; leave it alone */

	if (!aw_poke(c->Ebp - AW_EBP_TOTAL_BYTES, kept))
		return 0;

	if (!aw_peek(c->Ebp - AW_EBP_REMAINING, out_dropped))
		*out_dropped = 0;
	aw_poke(c->Ebp - AW_EBP_REMAINING, 0);

	c->Ecx = 0;        /* the faulting rep movsd becomes a no-op */
	c->EFlags |= 0x40; /* ZF, so the jne behind it leaves the loop */

	*out_kept = kept / AW_RECORD_BYTES;
	return 1;
}

static LONG CALLBACK aw_veh(EXCEPTION_POINTERS *ep)
{
	const EXCEPTION_RECORD *er = ep->ExceptionRecord;
	DWORD kept = 0;
	DWORD dropped = 0;
	LONG n;

	if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
		return EXCEPTION_CONTINUE_SEARCH;
	if (er->NumberParameters < 2 || er->ExceptionInformation[0] != 1 /* write */)
		return EXCEPTION_CONTINUE_SEARCH;
	if (er->ExceptionAddress != g_site)
		return EXCEPTION_CONTINUE_SEARCH;

	/* Before the state is disturbed by anything done to fix it. Off by default:
	 * a full-memory dump of this process runs past a gigabyte. */
	if (g_want_dump && !InterlockedExchange(&g_dumped, 1))
		aw_dump(ep);

	if (!aw_truncate(ep, &kept, &dropped)) {
		aw_raw("overrun at %08lX: could not read the loop frame (ebp %08lX, ebx %08lX, "
		       "edx %08lX) - letting it fault\r\n",
		       (ULONG)er->ExceptionInformation[1], (ULONG)ep->ContextRecord->Ebp,
		       (ULONG)ep->ContextRecord->Ebx, (ULONG)ep->ContextRecord->Edx);
		return EXCEPTION_CONTINUE_SEARCH;
	}

	/* Once per frame for as long as the list is over capacity, so it is logged
	 * while it is news and counted after that. */
	n = InterlockedIncrement(&g_catches);
	if (n <= 5 || (n % 300) == 0)
		aw_raw("overrun %ld: kept %lu records, dropped %lu, buffer ended at %08lX\r\n", n,
		       (ULONG)kept, (ULONG)dropped, (ULONG)er->ExceptionInformation[1]);
	return EXCEPTION_CONTINUE_EXECUTION;
}

/* Hex when it is written like hex. The one value anybody would ever set here is
 * a code offset, which is quoted in hex everywhere it appears - in the log this
 * prints, in WER, in the debugger - and a decimal-only parser that silently fell
 * back to the default on "9B730" would be a trap laid for one user. */
static DWORD aw_env_num(const char *name, DWORD dflt)
{
	char buf[16];
	DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
	DWORD v = 0;
	DWORD i = 0;
	int hex = 0;

	if (n == 0 || n >= sizeof(buf))
		return dflt;
	if (n > 2 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) {
		hex = 1;
		i = 2;
	} else {
		/* The base has to be settled before a single digit is accumulated;
		 * deciding it partway through reads 19B as 315 rather than 411. */
		DWORD j;
		for (j = 0; j < n; j++) {
			char c = buf[j];
			if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
				hex = 1;
		}
	}
	for (; i < n; i++) {
		char c = buf[i];
		DWORD d;
		if (c >= '0' && c <= '9')
			d = (DWORD)(c - '0');
		else if (c >= 'a' && c <= 'f')
			d = (DWORD)(c - 'a') + 10;
		else if (c >= 'A' && c <= 'F')
			d = (DWORD)(c - 'A') + 10;
		else
			return dflt;
		if (!hex && d > 9)
			return dflt;
		v = hex ? v * 16 + d : v * 10 + d;
	}
	return v;
}

/* Is the code at p the `rep movsd` this fix exists for? Probed rather than
 * assumed, and probed defensively: an unrelated executable may not have
 * anything mapped at that offset at all. */
static int aw_is_rep_movsd(const void *p)
{
	MEMORY_BASIC_INFORMATION mbi;
	const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
			   PAGE_EXECUTE_WRITECOPY;
	const unsigned char *b = (const unsigned char *)p;

	if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
	    (mbi.Protect & exec) == 0)
		return 0;
	return b[0] == 0xF3 && b[1] == 0xA5;
}

/* Installed from the first Present rather than from DllMain: the fault happens
 * deep in a stage, and there is nothing to gain from holding the loader lock. */
void allocwatch_frame(void)
{
	static volatile LONG once;
	SYSTEMTIME st;
	DWORD rva;

	if (InterlockedExchange(&once, 1))
		return;
	if (aw_env_num("D3D9SW_OVERRUN", 1) == 0)
		return;

	rva = aw_env_num("D3D9SW_OVERRUN_RVA", AW_SITE_RVA);
	g_site = (char *)GetModuleHandleA(NULL) + rva;
	g_want_dump = aw_env_num("D3D9SW_OVERRUN_DUMP", 0) != 0;

	/* This address is one build of one executable's bug. Point the same DLL
	 * at a different game - which people do, these engines are shared - and
	 * the RVA lands on unrelated code that must not be touched.
	 *
	 * So confirm the instruction really is the copy we came for. F3 A5 is
	 * rep movsd. Reading it from memory rather than the file also sidesteps
	 * the Steam DRM, which only decrypts the section once it is running. If
	 * it does not match, arm nothing: no handler, no risk. */
	if (!aw_is_rep_movsd(g_site)) {
		GetLocalTime(&st);
		aw_raw("\r\n=== overrun guard %04d-%02d-%02d %02d:%02d:%02d pid %lu ===\r\n"
		       "not arming: %08lX is not a rep movsd, so this is not the "
		       "executable the fix was written for\r\n",
		       st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
		       GetCurrentProcessId(), (ULONG)(uintptr_t)g_site);
		return;
	}

	if (!AddVectoredExceptionHandler(1, aw_veh))
		return;

	GetLocalTime(&st);
	aw_raw("\r\n=== overrun guard %04d-%02d-%02d %02d:%02d:%02d pid %lu ===\r\n"
	       "watching %08lX (exe %08lX + %lX)\r\n",
	       st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
	       GetCurrentProcessId(), (ULONG)(uintptr_t)g_site,
	       (ULONG)(uintptr_t)GetModuleHandleA(NULL), rva);
}

#else

void allocwatch_frame(void)
{
}

#endif
