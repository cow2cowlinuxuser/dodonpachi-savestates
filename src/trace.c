#include "trace.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define TRACE_MAX_UNIQUE 8000

static CRITICAL_SECTION g_cs;
static volatile LONG g_cs_ready;
static FILE *g_f;
static LONG g_seq;
static LONG g_unique;
static int g_off;
static char g_prev[192];
static unsigned g_repeat;

static int trace_wanted(void)
{
	char v[8];
	DWORD n = GetEnvironmentVariableA("D3D9_SW_TRACE", v, sizeof(v));
	if (n > 0 && n < sizeof(v) && (v[0] == '0' || v[0] == 'n' || v[0] == 'N'))
		return 0;
	return 1;
}

static void trace_init(void)
{
	if (InterlockedCompareExchange(&g_cs_ready, 1, 0) == 0) {
		InitializeCriticalSection(&g_cs);
		g_off = !trace_wanted();
	}
}

static void flush_repeat(void)
{
	if (!g_f || !g_repeat)
		return;
	fprintf(g_f, "     x %u\n", g_repeat + 1);
	g_repeat = 0;
}

void sw_trace(const char *fmt, ...)
{
	char body[192];
	va_list ap;
	LONG seq;

	trace_init();
	if (g_off || !fmt)
		return;
	seq = InterlockedIncrement(&g_seq);

	va_start(ap, fmt);
	_vsnprintf(body, sizeof(body), fmt, ap);
	va_end(ap);
	body[sizeof(body) - 1] = 0;

	EnterCriticalSection(&g_cs);
	if (!g_f) {
		g_f = fopen("d3d9_sw_trace.log", "w");
		if (g_f)
			fputs("# sequential IDirect3D9 / Device calls (AddRef/Release omitted)\n", g_f);
	}
	if (g_f) {
		if (g_prev[0] && strcmp(g_prev, body) == 0) {
			g_repeat++;
		} else {
			flush_repeat();
			if (g_unique >= TRACE_MAX_UNIQUE) {
				if (g_unique == TRACE_MAX_UNIQUE) {
					fprintf(g_f, "# truncated at %d unique lines\n", TRACE_MAX_UNIQUE);
					fflush(g_f);
					g_unique++;
				}
			} else {
				fprintf(g_f, "%6ld %s\n", seq, body);
				g_unique++;
				strncpy(g_prev, body, sizeof(g_prev) - 1);
				g_prev[sizeof(g_prev) - 1] = 0;
				if ((g_unique & 63) == 0)
					fflush(g_f);
			}
		}
	}
	LeaveCriticalSection(&g_cs);
}
