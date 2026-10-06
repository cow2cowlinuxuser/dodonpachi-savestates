#include "logdir.h"

#ifndef SS_PRESENT
#define SS_PRESENT __attribute__((section(".sspres")))
#endif

DWORD ss_build_id(void);

/* Kept in the present: a load from another launch would otherwise hand this
 * process the saving launch's folder, which may not exist on this machine. */
static char g_dir[MAX_PATH] SS_PRESENT;
static volatile LONG g_state SS_PRESENT;

static void dir_make(void)
{
	char exe[MAX_PATH], *base, *p, *dot = NULL;
	FILETIME c, x, k, u, lc;
	SYSTEMTIME t;
	int n;

	if (!GetModuleFileNameA(NULL, exe, sizeof(exe)))
		lstrcpyA(exe, "unknown");
	base = exe;
	for (p = exe; *p; p++)
		if (*p == '\\' || *p == '/')
			base = p + 1;
	for (p = base; *p; p++)
		if (*p == '.')
			dot = p;
	if (dot)
		*dot = 0;
	if (!GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u) ||
	    !FileTimeToLocalFileTime(&c, &lc) || !FileTimeToSystemTime(&lc, &t))
		GetLocalTime(&t);
	n = (int)(base - exe);
	if (n >= MAX_PATH - 80)
		n = 0;
	memcpy(g_dir, exe, (size_t)n);
	wsprintfA(g_dir + n, "logs");
	CreateDirectoryA(g_dir, NULL);
	wsprintfA(g_dir + n + 4, "\\%04u-%02u-%02u_%02u-%02u-%02u_%.40s_%lu", t.wYear, t.wMonth,
		  t.wDay, t.wHour, t.wMinute, t.wSecond, base,
		  (unsigned long)GetCurrentProcessId());
	if (!CreateDirectoryA(g_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
		g_dir[n ? n - 1 : 0] = 0;
}

const char *swlog_dir(void)
{
	if (g_state != 2) {
		if (InterlockedCompareExchange(&g_state, 1, 0) == 0) {
			dir_make();
			InterlockedExchange(&g_state, 2);
		} else {
			while (g_state != 2)
				Sleep(0);
		}
	}
	return g_dir;
}

int swlog_path(const char *name, char *out, unsigned cap)
{
	const char *d = swlog_dir();
	int n = lstrlenA(d), i;

	if ((unsigned)(n + lstrlenA(name) + 2) > cap)
		return 0;
	lstrcpyA(out, d);
	if (n)
		out[n++] = '\\';
	lstrcpyA(out + n, name);
	for (i = n; out[i]; i++)
		if (out[i] == '\\') {
			out[i] = 0;
			CreateDirectoryA(out, NULL);
			out[i] = '\\';
		}
	return 1;
}

static const IMAGE_NT_HEADERS *nt_of(HMODULE m)
{
	const IMAGE_DOS_HEADER *d = (const IMAGE_DOS_HEADER *)m;

	return d ? (const IMAGE_NT_HEADERS *)((const char *)m + d->e_lfanew) : NULL;
}

int swlog_header(char *out, int cap, const char *name)
{
	typedef LONG(WINAPI * RGV)(OSVERSIONINFOW *);
	char exe[MAX_PATH] = "", host[64] = "?";
	DWORD hn = sizeof(host);
	HMODULE me = GetModuleHandleA(NULL), nd = GetModuleHandleA("ntdll.dll");
	const IMAGE_NT_HEADERS *ne = nt_of(me), *nn = nt_of(nd);
	OSVERSIONINFOW v;
	RGV rgv = nd ? (RGV)GetProcAddress(nd, "RtlGetVersion") : NULL;
	FILETIME c, x, k, u, lc;
	SYSTEMTIME t;
	char buf[1400];
	int n;

	memset(&v, 0, sizeof(v));
	v.dwOSVersionInfoSize = sizeof(v);
	if (rgv)
		rgv(&v);
	GetModuleFileNameA(NULL, exe, sizeof(exe));
	GetComputerNameA(host, &hn);
	if (!GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u) ||
	    !FileTimeToLocalFileTime(&c, &lc) || !FileTimeToSystemTime(&lc, &t))
		GetLocalTime(&t);
	n = snprintf(buf, sizeof(buf),
		      "# %s\n"
		      "# session %04u-%02u-%02u %02u:%02u:%02u, pid %lu\n"
		      "# game    %s, stamp %08lX, at %08lX\n"
		      "# wrapper build %08lX\n"
		      "# windows %lu.%lu.%lu, ntdll stamp %08lX size %lX, machine %s\n"
		      "# folder  %s\n\n",
		      name, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
		      (unsigned long)GetCurrentProcessId(), exe,
		      ne ? (unsigned long)ne->FileHeader.TimeDateStamp : 0ul, (unsigned long)me,
		      (unsigned long)ss_build_id(), v.dwMajorVersion, v.dwMinorVersion,
		      v.dwBuildNumber, nn ? (unsigned long)nn->FileHeader.TimeDateStamp : 0ul,
		      nn ? (unsigned long)nn->OptionalHeader.SizeOfImage : 0ul, host, swlog_dir());
	if (n < 0 || n >= (int)sizeof(buf))
		n = (int)sizeof(buf) - 1;
	if (n >= cap)
		n = cap - 1;
	memcpy(out, buf, (size_t)n);
	out[n] = 0;
	return n;
}

static const char *leaf(const char *name)
{
	const char *b = name;

	for (; *name; name++)
		if (*name == '\\' || *name == '/')
			b = name + 1;
	return b;
}

FILE *swlog_fopen(const char *name, const char *mode)
{
	char path[MAX_PATH], hdr[1400];
	FILE *f;

	if (!swlog_path(name, path, sizeof(path)) || !(f = fopen(path, mode)))
		return NULL;
	fseek(f, 0, SEEK_END);
	if (ftell(f) == 0) {
		swlog_header(hdr, sizeof(hdr), leaf(name));
		fputs(hdr, f);
		fflush(f);
	}
	return f;
}

HANDLE swlog_create(const char *name, DWORD creation)
{
	char path[MAX_PATH], hdr[1400];
	LARGE_INTEGER sz;
	HANDLE h;
	DWORD wrote;
	int n;

	if (!swlog_path(name, path, sizeof(path)))
		return INVALID_HANDLE_VALUE;
	h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, creation,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return h;
	if (GetFileSizeEx(h, &sz) && sz.QuadPart == 0) {
		n = swlog_header(hdr, sizeof(hdr), leaf(name));
		WriteFile(h, hdr, (DWORD)n, &wrote, NULL);
	} else {
		sz.QuadPart = 0;
		SetFilePointerEx(h, sz, NULL, FILE_END);
	}
	return h;
}
