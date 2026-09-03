/* A test harness for the savestate engine.
 *
 * Why this exists. Against the game, the only oracle is "did it die", and
 * reaching a verdict costs a Steam launch, a debugger attach, several minutes of
 * play, and sometimes the evidence itself. Here we can assert on structures
 * immediately after every restore and get a verdict in milliseconds.
 *
 * What it drives is the real engine: savestate.c, unmodified, through the same
 * savestate_save and savestate_load the wrapper calls. A harness that
 * reimplemented any of it would prove nothing.
 *
 * Two scenarios, chosen by the third argument.
 *
 * synth - a hand-built structure shaped like _MonoJitInfoTable, filled by
 * threads that can be caught mid-loop. This modelled the crash of 2026-08-30
 * (a 380-entry chunk array with a hole at index 281) and falsified the simple
 * form of that theory: 120 cycles, 109 of them catching a fill in flight, zero
 * bad restores. Catching a writer mid-update is survivable, because the restore
 * rewinds the writer too and it does the work again.
 *
 * mono - the real mono-2.0-bdwgc.dll out of the game's own folder, initialised
 * as a runtime and made to JIT thousands of real mscorlib methods while
 * snapshots are taken underneath it. This is the scenario the synth one cannot
 * be: real jit-info tables, real chunk splits, real growable unwind tables
 * registered with ntdll, and the real GC running its own threads on its own
 * schedule. What it still does not exercise is Unity's render thread and the
 * D3D11 object graph, so OSFE stays the acceptance test.
 *
 * The oracle in mono mode is mono_jit_info_table_find. Every compile is
 * recorded as (method, code) in memory the snapshot excludes, and after each
 * restore every method compiled before the save must still be findable by its
 * code address and must still name the same method. That call walks the exact
 * structure that crashed. */

#include "savestate.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

/* savestate.c stops the rasteriser around both operations. There is no
 * rasteriser here, and stubbing it is honest: the engine's own comment says only
 * that its workers must not be running, and none are. */
void swrast_pool_shutdown(void)
{
}

/* ---------------------------------------------------------------- scenario 1 */

#define SLOTS_MAX 512
#define BLOCK_MAGIC 0x5AFE5AFEu
#define POISON 0xDEADBEEFu

/* Shaped like _MonoJitInfoTable: a count, then an array of pointers filled by a
 * loop that runs after the count is already set. That ordering is the whole
 * point - a reader trusting the count can reach a slot the loop has not written.
 */
typedef struct {
	volatile LONG count;
	void *slot[SLOTS_MAX];
} Table;

typedef struct {
	unsigned magic;
	unsigned serial;
} Block;

static Table *volatile g_table;
static volatile LONG g_stop;
static volatile LONG g_fills;      /* completed fill loops */
static volatile LONG g_filling;    /* nonzero while a loop is mid-flight */
static volatile LONG g_filler_tid; /* which thread was filling, 0 when idle */
static volatile LONG g_progress;   /* for the deadlock watchdog */

/* Deliberately not calloc. Uninitialised heap is what the real bug read as a
 * pointer, and zeroed memory would turn the failure into a null dereference -
 * a different bug with a different signature. */
static Table *table_new(LONG count)
{
	Table *t = (Table *)malloc(sizeof(*t));
	LONG i;

	if (!t)
		return NULL;
	for (i = 0; i < SLOTS_MAX; i++)
		t->slot[i] = (void *)(uintptr_t)POISON;
	t->count = count;
	return t;
}

static Block *block_new(unsigned serial)
{
	Block *b = (Block *)malloc(sizeof(*b));
	if (b) {
		b->magic = BLOCK_MAGIC;
		b->serial = serial;
	}
	return b;
}

/* Grows the table the way the real one does: allocate at the new size, publish
 * the count, then copy entries across one at a time. The sleep widens the window
 * on purpose - the harness's job is to make a rare interleaving reachable, not
 * to wait years for it. */
static DWORD WINAPI filler(LPVOID p)
{
	unsigned serial = 0;
	(void)p;

	while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
		Table *old = g_table;
		LONG n = old ? old->count : 0;
		LONG want = n + 8;
		Table *fresh;
		LONG i;

		if (want > SLOTS_MAX)
			want = 8;
		fresh = table_new(want);
		if (!fresh)
			break;

		InterlockedExchange(&g_filler_tid, (LONG)GetCurrentThreadId());
		InterlockedExchange(&g_filling, 1);
		for (i = 0; i < want; i++) {
			if (old && i < n && old->slot[i] != (void *)(uintptr_t)POISON)
				fresh->slot[i] = old->slot[i];
			else
				fresh->slot[i] = block_new(serial++);
			if ((i & 3) == 3)
				Sleep(0);
		}
		g_table = fresh;
		InterlockedExchange(&g_filling, 0);
		InterlockedExchange(&g_filler_tid, 0);
		InterlockedIncrement(&g_fills);
		InterlockedIncrement(&g_progress);
		Sleep(1);
	}
	return 0;
}

/* The operation that finds the damage. The real one bumps a refcount on every
 * chunk; this one only reads, because a write through a wild pointer would take
 * the harness down and tell us less than a report does. */
static int verify_synth(const char *when, int cycle)
{
	Table *t = g_table;
	LONG i, n;
	int holes = 0, poisoned = 0;

	if (!t) {
		printf("cycle %d %s: no table at all\n", cycle, when);
		return 1;
	}
	n = t->count;
	if (n < 0 || n > SLOTS_MAX) {
		printf("cycle %d %s: count %ld is out of range\n", cycle, when, (long)n);
		return 1;
	}
	for (i = 0; i < n; i++) {
		void *s = t->slot[i];
		Block *b = (Block *)s;

		if (s == (void *)(uintptr_t)POISON) {
			poisoned++;
			if (poisoned == 1)
				printf("cycle %d %s: slot %ld of %ld was never written\n",
				       cycle, when, (long)i, (long)n);
			continue;
		}
		/* A slot holding something that is neither our poison nor one of our
		 * blocks is the interesting case: it is whatever bytes happened to be
		 * in that heap block, read as a pointer. That is the shape of the real
		 * failure, where the value was 5 GB past the top of mapped memory. */
		if (!s || b->magic != BLOCK_MAGIC) {
			holes++;
			if (holes == 1)
				printf("cycle %d %s: slot %ld of %ld holds %p, not a block\n",
				       cycle, when, (long)i, (long)n, s);
		}
	}
	if (holes || poisoned)
		printf("cycle %d %s: %d unwritten, %d garbage, filling=%ld filler=%ld\n",
		       cycle, when, poisoned, holes, (long)g_filling, (long)g_filler_tid);
	return holes || poisoned;
}

/* ---------------------------------------------------------------- scenario 2 */

/* Metadata constants, from the ECMA-335 tables. Spelled out rather than
 * included, because mono's own headers are not shipped with the game and the
 * three numbers we need have been fixed since 2001. */
#define TABLE_METHOD 6
#define TABLE_GENERICPARAM 0x2a
#define GENERICPARAM_OWNER 2
#define TOKEN_METHOD_DEF 0x06000000u

/* MethodAttributes / MethodImplAttributes bits we filter on. */
#define METHOD_ABSTRACT 0x0400
#define METHOD_PINVOKE 0x2000
#define IMPL_CODE_TYPE_MASK 0x0003 /* 0 = IL, anything else is not ours */
#define IMPL_INTERNAL_CALL 0x1000

static struct Mono {
	HMODULE dll;
	void (*set_dirs)(const char *, const char *);
	void (*set_assemblies_path)(const char *);
	void *(*jit_init_version)(const char *, const char *);
	void *(*thread_attach)(void *);
	void *(*get_corlib)(void);
	int (*image_get_table_rows)(void *, int);
	const void *(*image_get_table_info)(void *, int);
	unsigned (*metadata_decode_row_col)(const void *, int, unsigned);
	void *(*get_method)(void *, unsigned, void *);
	void *(*compile_method)(void *);
	void *(*compile_method_checked)(void *, void *);
	void (*error_cleanup)(void *);
	unsigned (*method_get_flags)(void *, unsigned *);
	const char *(*method_get_name)(void *);
	void *(*method_get_class)(void *);
	const char *(*class_get_name)(void *);
	void *(*class_get_nesting_type)(void *);
	void *(*jit_info_table_find)(void *, void *);
	void *(*jit_info_get_method)(void *);
	char *(*method_full_name)(void *, int);
	/* Oracles. Everything below is used to ask the runtime a question after a
	 * restore that it can only answer correctly if the restore was correct. */
	void *(*thread_current)(void);
	unsigned (*gchandle_new)(void *, int);
	void *(*gchandle_get_target)(unsigned);
	void (*gchandle_free)(unsigned);
	void *(*string_new)(void *, const char *);
	char *(*string_to_utf8)(void *);
	void (*mono_free)(void *);
	void (*gc_collect)(int);
	int (*gc_max_generation)(void);
	size_t (*gc_get_used_size)(void);
	void *(*runtime_invoke)(void *, void *, void **, void **);
	void *(*object_get_class)(void *);
	void *(*class_from_name)(void *, const char *, const char *);
	void *(*method_desc_new)(const char *, int);
	void *(*method_desc_search_in_class)(void *, void *);
	void *(*object_unbox)(void *);
} mono;

static void *g_domain;
static void *g_image;
static int g_rows;            /* methoddef rows in the image */
static unsigned char *g_skip; /* one byte per row: 1 = do not compile */
static volatile LONG g_next_row;
static volatile LONG g_compiles;
static volatile LONG g_jitting;     /* threads currently inside the JIT */
static volatile LONG g_jitter_tid;

/* A method that mono refuses to compile takes the process with it.
 *
 * This build of mono has no mono_compile_method_checked - the name is not even
 * in the binary - so the only entry point available raises on failure, and a
 * raise with no managed frame above it ends in abort(). One run died exactly
 * that way, through ExitProcess in ucrtbase with no exception before it.
 *
 * Filtering by metadata removes the whole categories (abstract, pinvoke,
 * internal call, generic) but cannot predict a method whose class fails to
 * initialise. So the row being attempted is written to a file before the call
 * and cleared after it. A run that dies leaves the offending row behind, and the
 * next run skips it and remembers it permanently. The harness teaches itself
 * where the mines are instead of asking a human to read a stack each time. */
/* Raw handles rather than FILE*, and no stdio anywhere on a worker thread.
 *
 * The first version used fopen and fflush, and the very first run of it died
 * reading NULL in ntdll!RtlpEnterCriticalSectionContended, reached through
 * ucrtbase!_acrt_stdio_flush_nolock from this function, zero frames after a
 * restore. A FILE* carries a lock; the lock lives in the heap we rewind; a
 * thread holding or waiting on it when the snapshot was taken comes back to a
 * lock whose bookkeeping has been reverted underneath the kernel's idea of who
 * is waiting. WriteFile takes no user-mode lock of ours, so the instrument stops
 * being the thing it measures. */
#define INFLIGHT_MAX 64
static HANDLE g_inflight[INFLIGHT_MAX];

static void inflight_open(int idx)
{
	char name[64];

	if (idx < 0 || idx >= INFLIGHT_MAX)
		return;
	sprintf(name, "harness_inflight_%d.txt", idx);
	g_inflight[idx] = CreateFileA(name, GENERIC_WRITE, FILE_SHARE_READ, NULL,
				      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (g_inflight[idx] == INVALID_HANDLE_VALUE)
		g_inflight[idx] = NULL;
}

static void inflight_set(int idx, int row)
{
	HANDLE h = (idx >= 0 && idx < INFLIGHT_MAX) ? g_inflight[idx] : NULL;
	char buf[16];
	DWORD wrote;

	int neg = 0, n = 0, k;
	unsigned v;

	if (!h)
		return;
	/* Formatted by hand rather than with sprintf, and this is the same lesson as
	 * the fopen/fflush one a few iterations ago, one layer deeper.
	 *
	 * This runs on a worker thread on every compile, so a snapshot catches it
	 * constantly. sprintf is ucrtbase's formatting machinery, whose locale and
	 * internal state live on the process heap - the heap the restore rewinds. A
	 * hundred-run batch found 30 of 49 faults with a real unwound ucrtbase frame
	 * and five of the top seven fault sites inside printf internals; this was the
	 * only formatting call on a worker thread, which makes it the prime suspect
	 * for that population.
	 *
	 * Fixed width and rewritten in place, so the file is always one complete
	 * record and a reader never sees half of two. */
	v = (unsigned)(row < 0 ? (neg = 1, -row) : row);
	do {
		buf[n++] = (char)('0' + (v % 10));
		v /= 10;
	} while (v && n < 10);
	if (neg && n < 10)
		buf[n++] = '-';
	for (k = 0; k < n / 2; k++) {
		char t = buf[k];
		buf[k] = buf[n - 1 - k];
		buf[n - 1 - k] = t;
	}
	while (n < 14)
		buf[n++] = ' ';
	buf[n++] = '\n';
	buf[n] = 0;
	SetFilePointer(h, 0, NULL, FILE_BEGIN);
	/* Not FlushFileBuffers. It forces a disk write, which cost about 2 ms per
	 * compile and held the whole harness to 161 methods where it now manages
	 * tens of thousands. The file only has to survive the process dying, not the
	 * machine losing power, and the cache does that on its own. */
	WriteFile(h, buf, (DWORD)n, &wrote, NULL);
}

/* Reads what the previous run was doing when it died, and folds it into the
 * permanent list. Called before the skip array is consulted. */
static int bad_rows_load(unsigned char *skip, int rows)
{
	int i, added = 0, row;
	char name[64];
	FILE *f;
	char v[16];
	DWORD nv = GetEnvironmentVariableA("D3D9SW_HARNESS_BLACKLIST", v, sizeof(v));

	/* Off by default now, and that is a correction rather than a preference.
	 *
	 * The list was built for one failure only: this mono has no
	 * mono_compile_method_checked, so a method it refuses to compile calls
	 * ExitProcess with no exception, and the only way to get past it is to skip
	 * the row that was in flight. But the in-flight row is recorded for ANY
	 * death, and it cannot tell a compile refusal from the process dying after a
	 * restore - which is the failure being investigated.
	 *
	 * It had collected ten rows, seven of them in System.Security and all within
	 * a band from 15304 to 25382. The sweep walks rows in order and the deaths
	 * arrive a few seconds in, so that band is simply where the sweep had got
	 * to: the list was recording elapsed time and calling it a bad method. Left
	 * on, it silently removes whatever the failure happened to touch and every
	 * later run looks cleaner for no reason - which is very probably how a
	 * single-worker run once came back 100 cycles clean and made concurrency
	 * look like a necessary ingredient. */
	if (!(nv && nv < sizeof(v) && atoi(v)))
		return 0;

	f = fopen("harness_bad.txt", "r");
	if (f) {
		while (fscanf(f, "%d", &row) == 1)
			if (row >= 1 && row <= rows && !skip[row]) {
				skip[row] = 1;
				added++;
			}
		fclose(f);
	}
	for (i = 0; i < INFLIGHT_MAX; i++) {
		sprintf(name, "harness_inflight_%d.txt", i);
		f = fopen(name, "r");
		if (!f)
			continue;
		row = -1;
		if (fscanf(f, "%d", &row) != 1)
			row = -1;
		fclose(f);
		if (row >= 1 && row <= rows) {
			FILE *b = fopen("harness_bad.txt", "a");
			if (b) {
				fprintf(b, "%d\n", row);
				fclose(b);
			}
			if (!skip[row]) {
				skip[row] = 1;
				added++;
			}
			printf("mono: row %d killed a previous run and is now on the "
			       "permanent skip list\n",
			       row);
		}
	}
	return added;
}

static void *sym(const char *n, int *miss)
{
	void *p = (void *)GetProcAddress(mono.dll, n);
	if (!p) {
		printf("mono: no export %s\n", n);
		(*miss)++;
	}
	return p;
}

static void *sym_opt(const char *n)
{
	return (void *)GetProcAddress(mono.dll, n);
}

/* The paths are the game's, because the point is to drive the same build of the
 * same runtime against the same assemblies. Overridable so this can be pointed
 * at another Unity title without a rebuild. */
static void envdef(const char *name, char *out, size_t n, const char *fallback)
{
	DWORD got = GetEnvironmentVariableA(name, out, (DWORD)n);
	if (!got || got >= n) {
		strncpy(out, fallback, n - 1);
		out[n - 1] = 0;
	}
}

static int mono_start(void)
{
	char embed[MAX_PATH], managed[MAX_PATH], etc[MAX_PATH], dll[MAX_PATH];
	const char *game = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\"
			   "One Step From Eden";
	char fb[MAX_PATH * 2];
	int miss = 0;

	sprintf(fb, "%s\\MonoBleedingEdge\\EmbedRuntime", game);
	envdef("D3D9SW_MONO_EMBED", embed, sizeof(embed), fb);
	sprintf(fb, "%s\\OSFE_Data\\Managed", game);
	envdef("D3D9SW_MONO_MANAGED", managed, sizeof(managed), fb);
	sprintf(fb, "%s\\MonoBleedingEdge\\etc", game);
	envdef("D3D9SW_MONO_ETC", etc, sizeof(etc), fb);

	/* MonoPosixHelper sits beside the runtime and is loaded by name, so the
	 * directory has to be searchable before the runtime is loaded rather than
	 * after it asks. */
	SetDllDirectoryA(embed);
	sprintf(dll, "%s\\mono-2.0-bdwgc.dll", embed);
	mono.dll = LoadLibraryA(dll);
	if (!mono.dll) {
		printf("mono: cannot load %s (err %lu)\n", dll, GetLastError());
		return 0;
	}
	printf("mono: loaded %s at %p\n", dll, (void *)mono.dll);

	mono.set_dirs = (void (*)(const char *, const char *))sym("mono_set_dirs", &miss);
	mono.set_assemblies_path =
		(void (*)(const char *))sym("mono_set_assemblies_path", &miss);
	mono.jit_init_version =
		(void *(*)(const char *, const char *))sym("mono_jit_init_version", &miss);
	mono.thread_attach = (void *(*)(void *))sym("mono_thread_attach", &miss);
	mono.get_corlib = (void *(*)(void))sym("mono_get_corlib", &miss);
	mono.image_get_table_rows =
		(int (*)(void *, int))sym("mono_image_get_table_rows", &miss);
	mono.image_get_table_info =
		(const void *(*)(void *, int))sym("mono_image_get_table_info", &miss);
	mono.metadata_decode_row_col = (unsigned (*)(const void *, int, unsigned))sym(
		"mono_metadata_decode_row_col", &miss);
	mono.get_method = (void *(*)(void *, unsigned, void *))sym("mono_get_method", &miss);
	mono.method_get_flags =
		(unsigned (*)(void *, unsigned *))sym("mono_method_get_flags", &miss);
	mono.method_get_name = (const char *(*)(void *))sym("mono_method_get_name", &miss);
	mono.method_get_class = (void *(*)(void *))sym("mono_method_get_class", &miss);
	mono.class_get_name = (const char *(*)(void *))sym("mono_class_get_name", &miss);
	mono.class_get_nesting_type =
		(void *(*)(void *))sym("mono_class_get_nesting_type", &miss);
	mono.jit_info_table_find =
		(void *(*)(void *, void *))sym("mono_jit_info_table_find", &miss);
	mono.jit_info_get_method = (void *(*)(void *))sym("mono_jit_info_get_method", &miss);
	/* Reporting only, so a build without it is not a reason to refuse to run. */
	mono.method_full_name = (char *(*)(void *, int))sym_opt("mono_method_full_name");
	mono.thread_current = (void *(*)(void))sym("mono_thread_current", &miss);
	mono.gchandle_new = (unsigned (*)(void *, int))sym("mono_gchandle_new", &miss);
	mono.gchandle_get_target = (void *(*)(unsigned))sym("mono_gchandle_get_target", &miss);
	mono.gchandle_free = (void (*)(unsigned))sym_opt("mono_gchandle_free");
	mono.string_new = (void *(*)(void *, const char *))sym("mono_string_new", &miss);
	mono.string_to_utf8 = (char *(*)(void *))sym("mono_string_to_utf8", &miss);
	mono.mono_free = (void (*)(void *))sym_opt("mono_free");
	mono.gc_collect = (void (*)(int))sym("mono_gc_collect", &miss);
	mono.gc_max_generation = (int (*)(void))sym("mono_gc_max_generation", &miss);
	mono.gc_get_used_size = (size_t(*)(void))sym_opt("mono_gc_get_used_size");
	mono.runtime_invoke =
		(void *(*)(void *, void *, void **, void **))sym("mono_runtime_invoke", &miss);
	mono.object_get_class = (void *(*)(void *))sym("mono_object_get_class", &miss);
	mono.class_from_name =
		(void *(*)(void *, const char *, const char *))sym("mono_class_from_name", &miss);
	mono.method_desc_new = (void *(*)(const char *, int))sym("mono_method_desc_new", &miss);
	mono.method_desc_search_in_class =
		(void *(*)(void *, void *))sym("mono_method_desc_search_in_class", &miss);
	mono.object_unbox = (void *(*)(void *))sym("mono_object_unbox", &miss);

	/* Preferred, because the deprecated mono_compile_method raises on failure
	 * and a raise with no managed frame above it is an abort, which takes the
	 * harness down without saying why. The checked form hands the failure back. */
	mono.compile_method_checked =
		(void *(*)(void *, void *))sym_opt("mono_compile_method_checked");
	mono.error_cleanup = (void (*)(void *))sym_opt("mono_error_cleanup");
	mono.compile_method = (void *(*)(void *))sym_opt("mono_compile_method");
	if (!mono.compile_method_checked || !mono.error_cleanup) {
		printf("mono: no checked compile; falling back to mono_compile_method, "
		       "which aborts rather than reporting a bad method\n");
		if (!mono.compile_method)
			miss++;
	}
	if (miss) {
		printf("mono: %d export(s) missing, cannot drive the JIT\n", miss);
		return 0;
	}

	/* Hooked here, between the load and the runtime init, and this ordering is
	 * the whole reason mono_start is split the way it is.
	 *
	 * The unwind-table hooks work by finding mono's own cached copy of the
	 * resolved ntdll pointer and replacing it, so mono has to be mapped before
	 * they can land - but any table mono registers before they land is invisible
	 * to us for the rest of the process. The first mono run showed exactly that:
	 * "0 registered at the snapshot" while mono had already JIT'd a million
	 * methods, because the hooks were only attempted from savestate_guard once
	 * the cycle loop started. In the game the wrapper is loaded before mono, so
	 * this window does not exist there and the bug was invisible. */
	{
		int i;
		for (i = 0; i < 8; i++)
			savestate_guard();
	}
	mono.set_dirs(managed, etc);
	mono.set_assemblies_path(managed);
	printf("mono: assemblies %s\n", managed);
	g_domain = mono.jit_init_version("ss_harness", "v4.0.30319");
	if (!g_domain) {
		printf("mono: jit_init_version returned NULL\n");
		return 0;
	}
	g_image = mono.get_corlib();
	if (!g_image) {
		printf("mono: no corlib image\n");
		return 0;
	}
	g_rows = mono.image_get_table_rows(g_image, TABLE_METHOD);
	printf("mono: runtime up, domain %p, corlib %p, %d methoddef row(s)\n", g_domain,
	       g_image, g_rows);
	if (g_rows < 1)
		return 0;

	/* Open generic methods and methods of open generic types cannot be
	 * compiled, and mono answers the attempt with an assertion rather than an
	 * error, so they have to be filtered before the call. The generic-parameter
	 * table names the methods exactly; for types the arity suffix in the name
	 * does, which is a metadata convention rather than a guess. */
	g_skip = (unsigned char *)calloc((size_t)g_rows + 1, 1);
	if (!g_skip)
		return 0;
	{
		const void *gp = mono.image_get_table_info(g_image, TABLE_GENERICPARAM);
		int n = mono.image_get_table_rows(g_image, TABLE_GENERICPARAM), i, marked = 0;

		for (i = 0; i < n; i++) {
			unsigned owner =
				mono.metadata_decode_row_col(gp, i, GENERICPARAM_OWNER);
			unsigned row = owner >> 1;

			if ((owner & 1) && row >= 1 && (int)row <= g_rows) {
				if (!g_skip[row])
					marked++;
				g_skip[row] = 1;
			}
		}
		printf("mono: %d generic-parameter row(s), %d generic method(s) skipped\n", n,
		       marked);
	}
	printf("mono: %d row(s) skipped from earlier failures\n",
	       bad_rows_load(g_skip, g_rows));
	/* Again after init, because init is where mono resolves and first calls the
	 * unwind-table entry points. */
	{
		int i;
		for (i = 0; i < 8; i++)
			savestate_guard();
	}
	return 1;
}

/* True if this class, or anything it is nested inside, is an open generic type.
 * The arity suffix is what distinguishes List`1 from List; a nested type does
 * not carry one, which is why the chain has to be walked. */
static int class_is_generic(void *k)
{
	int depth = 0;

	while (k && depth++ < 8) {
		const char *n = mono.class_get_name(k);
		if (n && strchr(n, '`'))
			return 1;
		k = mono.class_get_nesting_type(k);
	}
	return 0;
}

static void *compile_one(void *m)
{
	if (mono.compile_method_checked) {
		/* MonoError is caller-allocated and its size is part of the ABI we do
		 * not have a header for. Oversized on purpose: too big wastes stack,
		 * too small corrupts it. */
		unsigned char err[256];
		void *code;

		memset(err, 0, sizeof(err));
		code = mono.compile_method_checked(m, err);
		mono.error_cleanup(err);
		return code;
	}
	return mono.compile_method(m);
}

#define REC_MAX 8192

typedef struct {
	void *method;
	void *code;
} Rec;

/* Everything the harness must remember across a restore.
 *
 * Held in the present by savestate_exclude, and that is not a convenience. A
 * restore rewinds the thread that asked for it, so a loop counter on that
 * thread's stack returns to its old value every time and the loop never reaches
 * its second iteration. The first version of this harness kept its counters as
 * locals and ran for 99 seconds without leaving cycle one. */
typedef struct {
	LONG cycle;
	LONG bad;
	LONG caught_filling;
	LONG saves;
	LONG restores;
	LONG was_filling;
	LONG progress_at_save;
	/* mono mode */
	volatile LONG n;    /* records written */
	LONG n_at_save;     /* prefix that a restore must preserve */
	LONG compiles_at_save;
	LONG was_jitting;
	LONG caught_jitting;
	/* How long after a restore the workers take to complete their next unit of
	 * work. Measured rather than tested against a threshold: the first version
	 * asked "did anything progress within 20 ms" and answered no on nearly every
	 * cycle, which says a stall exists but not whether it is 21 ms or forever,
	 * and those two call for completely different investigations. */
	LONG stalls;      /* restores measured */
	LONG stall_worst; /* microseconds */
	LONG stall_never; /* restores where nothing progressed before the cap */
	LONGLONG stall_total_us;
	LONG workers;
	LONG attempts;
	LONG nulls;
	HANDLE worker[16];

	/* --- oracle: per-thread managed identity, i.e. does our TLS restore work
	 *
	 * Mono keeps the current MonoThread in thread-local storage, and the load
	 * reports "N context(s) restored, N with TLS" without anything ever checking
	 * that claim. If TLS comes back wrong, mono_thread_current() on a worker
	 * returns null or somebody else's thread.
	 *
	 * Recorded and compared in held memory because a worker's own locals are
	 * rewound, and reported by the MAIN thread only: a worker must never touch
	 * stdio, which is how the harness crashed itself once already. */
	LONG gen;              /* bumped by main after each restore */
	LONG seen[16];         /* last generation each worker checked */
	void *mono_thread[16]; /* what thread_current returned at attach */
	LONG tls_checks;
	LONG tls_null;
	LONG tls_moved;

	/* --- oracle: a pinned managed object survives with its contents intact
	 *
	 * The handle number lives out here, the handle TABLE and the object live
	 * inside the snapshot, so a handle taken before the first save must still
	 * resolve afterwards. Compared by contents rather than by pointer, because a
	 * non-null pointer to rubbish is the failure worth catching. */
	unsigned pin;
	char pin_want[64];
	LONG pin_checks;
	LONG pin_null;
	LONG pin_wrong;

	/* --- oracle: a full collection completes
	 *
	 * The heaviest question available, and the one the evidence points at. Boehm
	 * scans every attached thread's stack using bounds registered at attach
	 * time, then every heap section and mark bit. A restore that left a thread's
	 * stack bounds stale would fault here - and two faults inside mono have
	 * already written to the stack pointer with its low 32 bits cleared, which
	 * is what scanning a wrong stack range looks like.
	 *
	 * An intervention rather than an observation, so it also runs once before
	 * the first save. Without that control, a crash during collection cannot be
	 * told apart from us simply calling it at an unlucky moment. */
	LONG gc_checks;
	LONG gc_worst_ms; /* microseconds, despite the name */
	LONG gc_used;     /* KB in use after the last collection */

	/* --- oracle: a managed exception is thrown, unwound and caught
	 *
	 * The only test the unwind-table replay has ever had. A wrong replay does
	 * not crash; it breaks managed exception handling, which is invisible unless
	 * something deliberately throws. Int32.Parse on rubbish must come back as a
	 * FormatException object, which means the throw, the unwind through JIT'd
	 * frames, and the catch all worked. */
	void *parse;
	LONG exc_checks;
	LONG exc_missing; /* no exception where one was required */
	LONG exc_wrong;   /* an exception, but not the expected type */

	/* --- oracle: JIT'd managed code runs and returns the right answer
	 *
	 * The distinction this draws is the one the jit-info check cannot: that
	 * check asks whether compiled code can be FOUND, this asks whether it
	 * WORKS. Same method as the exception oracle, taken down its success path,
	 * so the result is a number that can be wrong rather than a pointer that can
	 * only be null. */
	LONG call_checks;
	LONG call_failed; /* threw when it should not have */
	LONG call_wrong;  /* returned the wrong number */
	LONG bucket[8]; /* <1ms, <2, <4, <8, <16, <32, <64, >=64 */
	Rec rec[REC_MAX];
} Held;

static Held *g_held;

static DWORD WINAPI jitter(LPVOID p)
{
	int idx = (int)(intptr_t)p;

	mono.thread_attach(g_domain);
	if (idx < 16) {
		g_held->mono_thread[idx] = mono.thread_current();
		g_held->seen[idx] = g_held->gen;
	}
	inflight_open(idx);

	while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
		LONG row = InterlockedIncrement(&g_next_row);
		void *m, *k, *code;
		unsigned flags, iflags = 0;

		/* Once per restore, from the worker's own thread, because that is the
		 * only place the question can be asked - TLS is per thread and the main
		 * thread's answer says nothing about this one's. Counters only; the main
		 * thread does the printing. */
		if (idx < 16 && g_held->seen[idx] != g_held->gen) {
			void *now = mono.thread_current();

			g_held->seen[idx] = g_held->gen;
			InterlockedIncrement(&g_held->tls_checks);
			if (!now)
				InterlockedIncrement(&g_held->tls_null);
			else if (now != g_held->mono_thread[idx])
				InterlockedIncrement(&g_held->tls_moved);
		}

		/* Round and round rather than stopping at the end of the table. A
		 * second pass mostly hits already-compiled methods, which is cheap and
		 * still exercises lookup, and the JIT keeps being entered. */
		if (row >= g_rows) {
			InterlockedExchange(&g_next_row, 0);
			row = 0;
		}
		if (row < 1)
			continue;
		if (g_skip[row])
			continue;

		m = mono.get_method(g_image, TOKEN_METHOD_DEF | (unsigned)row, NULL);
		if (!m)
			continue;
		flags = mono.method_get_flags(m, &iflags);
		if ((flags & (METHOD_ABSTRACT | METHOD_PINVOKE)) != 0)
			continue;
		if ((iflags & IMPL_CODE_TYPE_MASK) != 0 || (iflags & IMPL_INTERNAL_CALL) != 0)
			continue;
		k = mono.method_get_class(m);
		if (!k || class_is_generic(k))
			continue;

		InterlockedExchange(&g_jitter_tid, (LONG)GetCurrentThreadId());
		InterlockedIncrement(&g_jitting);
		inflight_set(idx, (int)row);
		code = compile_one(m);
		inflight_set(idx, -1);
		InterlockedDecrement(&g_jitting);
		/* Paced deliberately. Left to run flat out, four threads turn over a
		 * million cached lookups a second, which buries the real compiles in
		 * noise and makes every file write above expensive for nothing. */
		Sleep(0);
		/* Counted in held memory on purpose, so a restore cannot rewind them.
		 * These two separate "the thread is not running" from "the thread is
		 * running and the JIT has stopped returning code", which look identical
		 * from the outside and want completely different fixes. */
		InterlockedIncrement(&g_held->attempts);
		if (!code) {
			InterlockedIncrement(&g_held->nulls);
			continue;
		}
		InterlockedIncrement(&g_compiles);
		InterlockedIncrement(&g_progress);
		{
			LONG i = InterlockedIncrement(&g_held->n) - 1;
			if (i < REC_MAX) {
				g_held->rec[i].method = m;
				g_held->rec[i].code = code;
			} else {
				InterlockedExchange(&g_held->n, REC_MAX);
			}
		}
	}
	return 0;
}

/* The oracle. Every method compiled before the snapshot must still be findable
 * by its code address, and must still name itself, which means walking the
 * jit-info table - the structure that took the process down in OSFE. */
static int verify_mono(const char *when, int cycle)
{
	LONG i, upto = g_held->n_at_save;
	int lost = 0, wrong = 0;

	if (upto > REC_MAX)
		upto = REC_MAX;
	for (i = 0; i < upto; i++) {
		void *ji = mono.jit_info_table_find(g_domain, g_held->rec[i].code);

		if (!ji) {
			lost++;
			if (lost == 1)
				printf("cycle %d %s: code %p (record %ld of %ld) is no longer "
				       "in the jit-info table\n",
				       cycle, when, g_held->rec[i].code, (long)i, (long)upto);
			continue;
		}
		if (mono.jit_info_get_method(ji) != g_held->rec[i].method) {
			wrong++;
			if (wrong == 1)
				printf("cycle %d %s: code %p now names method %p, recorded as "
				       "%p\n",
				       cycle, when, g_held->rec[i].code,
				       mono.jit_info_get_method(ji), g_held->rec[i].method);
		}
	}
	if (lost || wrong)
		printf("cycle %d %s: %d lost, %d misattributed of %ld, jitting=%ld "
		       "jitter=%ld\n",
		       cycle, when, lost, wrong, (long)upto, (long)g_jitting,
		       (long)g_jitter_tid);
	return lost || wrong;
}

/* -------------------------------------------------------------------- driver */

enum { MODE_SYNTH, MODE_MONO };
static int g_mode;

/* Names an address the way the fault logger does, so the two can be compared by
 * eye: owning module and offset, or the bare address when it owns nothing. */
static void where(char *out, size_t n, void *p)
{
	MEMORY_BASIC_INFORMATION mbi;
	char path[MAX_PATH];
	const char *base;

	if (!VirtualQuery(p, &mbi, sizeof(mbi)) || !mbi.AllocationBase) {
		_snprintf(out, n, "%p", p);
		return;
	}
	if (!GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, sizeof(path))) {
		_snprintf(out, n, "%p (no module)", p);
		return;
	}
	base = strrchr(path, '\\');
	_snprintf(out, n, "%s+%llX", base ? base + 1 : path,
		  (unsigned long long)((char *)p - (char *)mbi.AllocationBase));
}

/* Samples a live worker's program counter repeatedly. A thread that is alive and
 * not finishing work is either parked on something or spinning, and those two
 * want opposite fixes: several samples at one address means parked, samples that
 * move means running and simply not completing a unit. */
static void sample_workers(void)
{
	int s, w;

	for (s = 0; s < 5; s++) {
		for (w = 0; w < g_held->workers; w++) {
			CONTEXT c;
			char buf[160];
			HANDLE h = g_held->worker[w];

			if (!h || WaitForSingleObject(h, 0) == WAIT_OBJECT_0)
				continue;
			memset(&c, 0, sizeof(c));
			c.ContextFlags = CONTEXT_CONTROL;
			if (SuspendThread(h) == (DWORD)-1)
				continue;
			if (GetThreadContext(h, &c)) {
#ifdef _WIN64
				where(buf, sizeof(buf), (void *)c.Rip);
#else
				where(buf, sizeof(buf), (void *)c.Eip);
#endif
				printf("    worker %d sample %d: %s\n", w, s, buf);
			}
			ResumeThread(h);
		}
		Sleep(20);
	}
}

/* Takes a pinned handle on a managed string with known contents, before the
 * first save so that both the object and the handle table are inside every
 * snapshot. Pinned so the object cannot legitimately move, which keeps the check
 * honest: any change of contents is then a fault rather than a relocation. */
static void pin_create(void)
{
	void *s;

	_snprintf(g_held->pin_want, sizeof(g_held->pin_want),
		  "savestate oracle string 0123456789");
	s = mono.string_new(g_domain, g_held->pin_want);
	if (!s) {
		printf("oracle: could not create the pinned string\n");
		return;
	}
	g_held->pin = mono.gchandle_new(s, 1);
	printf("oracle: pinned handle %u on a %d-byte string\n", g_held->pin,
	       (int)strlen(g_held->pin_want));
}

/* Compared by contents, not by pointer. A handle that resolves to non-null
 * rubbish is the interesting failure, and a pointer comparison would pass it. */
static void pin_verify(void)
{
	void *obj;
	char *utf8;

	if (!g_held->pin)
		return;
	g_held->pin_checks++;
	obj = mono.gchandle_get_target(g_held->pin);
	if (!obj) {
		g_held->pin_null++;
		printf("cycle %ld: pinned handle %u no longer resolves\n",
		       (long)g_held->cycle, g_held->pin);
		return;
	}
	utf8 = mono.string_to_utf8(obj);
	if (!utf8 || strcmp(utf8, g_held->pin_want) != 0) {
		g_held->pin_wrong++;
		printf("cycle %ld: pinned string came back as \"%.60s\" instead of "
		       "\"%s\"\n",
		       (long)g_held->cycle, utf8 ? utf8 : "(null)", g_held->pin_want);
	}
	if (utf8 && mono.mono_free)
		mono.mono_free(utf8);
}

static void gc_oracle(const char *when)
{
	LARGE_INTEGER f, t0, t1;
	int us;
	size_t before = 0, after = 0;

	if (!mono.gc_collect || !mono.gc_max_generation)
		return;
	/* Timed in microseconds and reported with the used-size either side, because
	 * the first version printed "0 ms" for all 31 collections - which is exactly
	 * what a call that does nothing at all also prints. A collection that moves
	 * no bytes and takes no time is not evidence of a healthy runtime, it is an
	 * oracle that has not been shown to work. */
	if (mono.gc_get_used_size)
		before = mono.gc_get_used_size();
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t0);
	mono.gc_collect(mono.gc_max_generation());
	QueryPerformanceCounter(&t1);
	if (mono.gc_get_used_size)
		after = mono.gc_get_used_size();
	us = (int)((double)(t1.QuadPart - t0.QuadPart) * 1000000.0 / (double)f.QuadPart);
	g_held->gc_checks++;
	if (us > g_held->gc_worst_ms)
		g_held->gc_worst_ms = us;
	g_held->gc_used = (LONG)(after / 1024);
	if (when)
		printf("oracle: full collection %s took %d us, used %llu -> %llu KB\n", when,
		       us, (unsigned long long)(before / 1024),
		       (unsigned long long)(after / 1024));
}

/* Resolved once and kept in held memory. The MonoMethod itself lives inside the
 * snapshot and existed before the first save, so it is valid after a restore for
 * the same reason the pinned handle is. */
static void exc_oracle_init(void)
{
	void *desc, *klass;

	klass = mono.class_from_name(g_image, "System", "Int32");
	if (!klass) {
		printf("oracle: no System.Int32\n");
		return;
	}
	desc = mono.method_desc_new("Int32:Parse(string)", 0);
	if (!desc) {
		printf("oracle: could not build the method description\n");
		return;
	}
	g_held->parse = mono.method_desc_search_in_class(desc, klass);
	if (!g_held->parse)
		printf("oracle: could not find Int32:Parse(string)\n");
}

static void exc_oracle(const char *when)
{
	void *args[1];
	void *exc = NULL;
	const char *name;

	if (!g_held->parse)
		return;
	args[0] = mono.string_new(g_domain, "not a number at all");
	if (!args[0])
		return;
	g_held->exc_checks++;
	mono.runtime_invoke(g_held->parse, NULL, args, &exc);
	if (!exc) {
		g_held->exc_missing++;
		printf("cycle %ld: Int32.Parse on rubbish did NOT throw%s - managed "
		       "exception handling is broken\n",
		       (long)g_held->cycle, when ? when : "");
		return;
	}
	name = mono.class_get_name(mono.object_get_class(exc));
	if (!name || strcmp(name, "FormatException") != 0) {
		g_held->exc_wrong++;
		printf("cycle %ld: expected FormatException, got %s\n", (long)g_held->cycle,
		       name ? name : "(unnamed)");
		return;
	}
	if (when)
		printf("oracle: managed throw/catch works %s (%s)\n", when, name);
}

#define CALL_ORACLE_WANT 1234567

static void call_oracle(const char *when)
{
	void *args[1];
	void *exc = NULL;
	void *ret;
	int got;

	if (!g_held->parse || !mono.object_unbox)
		return;
	args[0] = mono.string_new(g_domain, "1234567");
	if (!args[0])
		return;
	g_held->call_checks++;
	ret = mono.runtime_invoke(g_held->parse, NULL, args, &exc);
	if (exc || !ret) {
		g_held->call_failed++;
		printf("cycle %ld: Int32.Parse(\"1234567\") failed%s\n", (long)g_held->cycle,
		       exc ? " by throwing" : " by returning nothing");
		return;
	}
	got = *(int *)mono.object_unbox(ret);
	if (got != CALL_ORACLE_WANT) {
		g_held->call_wrong++;
		printf("cycle %ld: Int32.Parse(\"1234567\") returned %d\n",
		       (long)g_held->cycle, got);
		return;
	}
	if (when)
		printf("oracle: managed call works %s (returned %d)\n", when, got);
}

static LONG breath_ms(void)
{
	static LONG ms = -1;

	if (ms < 0) {
		char v[16];
		DWORD n = GetEnvironmentVariableA("D3D9SW_HARNESS_BREATH", v, sizeof(v));
		ms = (n && n < sizeof(v)) ? atoi(v) : 20;
	}
	return ms;
}

/* Spins until a worker finishes something, or until the cap. Spinning rather
 * than sleeping because the interesting range is single-digit milliseconds and
 * Sleep's granularity is about 15 ms - the resolution the old check lacked is
 * exactly the resolution needed to tell a scheduler artefact from a stall. */
static void wait_for_progress(void)
{
	static LONG cap_ms = -1;
	LARGE_INTEGER pf, t0, t1;
	double us;
	LONG b, a0, n0;

	if (cap_ms < 0) {
		char v[16];
		DWORD n = GetEnvironmentVariableA("D3D9SW_HARNESS_WAIT", v, sizeof(v));
		cap_ms = (n && n < sizeof(v)) ? atoi(v) : 500;
	}
	a0 = g_held->attempts;
	n0 = g_held->nulls;
	QueryPerformanceFrequency(&pf);
	QueryPerformanceCounter(&t0);
	for (;;) {
		QueryPerformanceCounter(&t1);
		us = (double)(t1.QuadPart - t0.QuadPart) * 1000000.0 / (double)pf.QuadPart;
		if (g_progress != g_held->progress_at_save)
			break;
		if (us > (double)cap_ms * 1000.0) {
			LONG w, alive = 0, dead = 0;

			g_held->stall_never++;
			for (w = 0; w < g_held->workers; w++) {
				if (!g_held->worker[w])
					continue;
				if (WaitForSingleObject(g_held->worker[w], 0) == WAIT_OBJECT_0)
					dead++;
				else
					alive++;
			}
			/* Only on the first timeout of a run: the answer does not change
			 * from cycle to cycle, and the whole point of the check is that a
			 * run either stalls on every restore or on none. */
			if (g_held->stall_never == 1) {
				printf("cycle %ld: nothing progressed in %ld ms - %ld worker(s) "
				       "still running, %ld already exited\n",
				       (long)g_held->cycle, (long)cap_ms, (long)alive,
				       (long)dead);
				printf("    %ld compile(s) attempted during the wait, %ld of "
				       "them returned no code\n",
				       (long)(g_held->attempts - a0),
				       (long)(g_held->nulls - n0));
				if (alive)
					sample_workers();
			}
			break;
		}
		YieldProcessor();
	}
	g_held->stalls++;
	g_held->stall_total_us += (LONGLONG)us;
	if ((LONG)us > g_held->stall_worst)
		g_held->stall_worst = (LONG)us;
	for (b = 0; b < 7 && us >= 1000.0 * (double)(1 << b); b++)
		;
	g_held->bucket[b]++;
}

static int verify_now(const char *when, int cycle)
{
	return g_mode == MODE_MONO ? verify_mono(when, cycle) : verify_synth(when, cycle);
}

int main(int argc, char **argv)
{
	int cycles = argc > 1 ? atoi(argv[1]) : 200;
	int nthreads = argc > 2 ? atoi(argv[2]) : 4;
	const char *mode = argc > 3 ? argv[3] : "mono";
	int i;
	HANDLE th[64];

	if (nthreads > 64)
		nthreads = 64;
	/* Unbuffered, because the first run of this harness died with an access
	 * violation and printed nothing at all: block buffering into a pipe means a
	 * crash takes the explanation with it. The whole value of a harness is that
	 * it says what happened. */
	setvbuf(stdout, NULL, _IONBF, 0);
	g_mode = strcmp(mode, "mono") == 0 ? MODE_MONO : MODE_SYNTH;
	printf("savestate harness: %d cycles, %d worker thread(s), mode %s\n", cycles,
	       nthreads, g_mode == MODE_MONO ? "mono" : "synth");

	/* Before the runtime, so the interposed allocator and the exit hooks are in
	 * place for everything mono goes on to do. */
	printf("installing hooks\n");
	savestate_hooks_install();
	g_held = (Held *)VirtualAlloc(NULL, sizeof(Held), MEM_COMMIT | MEM_RESERVE,
				      PAGE_READWRITE);
	if (!g_held)
		return 2;
	savestate_exclude(g_held, sizeof(Held));

	if (g_mode == MODE_MONO && !mono_start()) {
		/* Said rather than silently substituted: a synth run reported as a mono
		 * run would be the most misleading result this harness could produce. */
		printf("mono unavailable - NOT falling back, because a synth result "
		       "labelled mono proves nothing\n");
		return 3;
	}
	if (g_mode == MODE_SYNTH) {
		printf("seeding table\n");
		g_table = table_new(8);
		for (i = 0; i < g_table->count; i++)
			g_table->slot[i] = block_new(0xF000u + (unsigned)i);
	}

	printf("starting workers\n");
	for (i = 0; i < nthreads; i++) {
		th[i] = CreateThread(NULL, 0, g_mode == MODE_MONO ? jitter : filler,
				     (LPVOID)(intptr_t)i, 0, NULL);
		/* Kept where a restore cannot rewind them, so that "nothing progressed"
		 * can be told apart from "there is nobody left to progress". */
		if (i < 16)
			g_held->worker[i] = th[i];
	}
	g_held->workers = nthreads < 16 ? nthreads : 16;
	/* Not a savestate test at all: names the rows the auto-blacklist has
	 * collected, so that a row recurring across runs can be identified instead
	 * of remaining a number. Runs the runtime and nothing else. */
	if (g_mode == MODE_MONO && strcmp(mode, "mono") == 0 && argc > 4 &&
	    strcmp(argv[4], "names") == 0) {
		FILE *f = fopen("harness_bad.txt", "r");
		char line[64];

		if (!f) {
			printf("no harness_bad.txt\n");
			return 0;
		}
		while (fgets(line, sizeof(line), f)) {
			int row = atoi(line);
			void *m;

			if (row < 1 || row > g_rows)
				continue;
			m = mono.get_method(g_image, 0x06000000u | (unsigned)row, NULL);
			printf("row %d: %s\n", row,
			       (m && mono.method_full_name) ? mono.method_full_name(m, 1)
							    : "(could not be named)");
		}
		fclose(f);
		return 0;
	}

	if (g_mode == MODE_MONO) {
		/* The verifier calls into mono from this thread, so it needs a managed
		 * thread of its own; and there has to be something in the table before
		 * the first snapshot for the oracle to have anything to say. */
		mono.thread_attach(g_domain);
		pin_create();
		exc_oracle_init();
		/* The controls. If either of these fails here, the oracle is broken and
		 * nothing it says after a restore means anything. */
		call_oracle("before any save");
		exc_oracle("before any save");
		gc_oracle("before any save");
		Sleep(400);
		printf("mono: %ld method(s) compiled before the first save\n",
		       (long)g_compiles);
	}
	printf("entering cycle loop\n");

	/* Written in the wrapper's idiom, because a restore is a jump backwards out
	 * of savestate_load and back out through the savestate_save that took the
	 * snapshot. savestate_load does not return; the thread reappears at the top,
	 * returning from the save, with savestate_last_was_restore set. Anything
	 * written as "save, then load, then check" describes control flow that does
	 * not exist. */
	while (g_held->cycle < cycles && !g_stop) {
		savestate_guard();
		if (!savestate_save(0)) {
			printf("cycle %ld: save refused\n", (long)g_held->cycle);
			g_held->cycle++;
			continue;
		}
		if (savestate_last_was_restore()) {
			g_held->restores++;
			/* Records added between the save and the restore describe
			 * compiles the rewind has undone. They live in excluded memory
			 * so they survived when their subject did not, and dropping
			 * them is the reconciliation - keeping them would report mono's
			 * correct behaviour as a fault. */
			InterlockedExchange(&g_held->n, g_held->n_at_save);
			if (verify_now("after restore", (int)g_held->cycle)) {
				g_held->bad++;
				printf("cycle %ld: the snapshot behind that restore was taken "
				       "%s a %s\n",
				       (long)g_held->cycle,
				       (g_mode == MODE_MONO ? g_held->was_jitting
							    : g_held->was_filling)
					       ? "DURING"
					       : "outside",
				       g_mode == MODE_MONO ? "compile" : "fill");
			}
			/* A restore that leaves nobody able to make progress is the
			 * other failure we have seen, and an invariant check cannot
			 * see it - the memory is perfectly consistent and nothing
			 * runs.
			 *
			 * g_progress is rewound with everything else, so immediately
			 * after a restore it holds exactly the value recorded at save
			 * time. That makes the comparison sound: any increase is work
			 * done since the restore, not a leftover from before it. */
			/* Bumped before anything else so the workers pick it up as
			 * early as possible after their contexts resume. */
			g_held->gen++;
			if (g_mode == MODE_MONO) {
				pin_verify();
				call_oracle(NULL);
				exc_oracle(NULL);
				gc_oracle(NULL);
			}
			wait_for_progress();
			/* The measurement above returns the instant a worker finishes
			 * something, which in a healthy run is immediate, so without this
			 * the next save follows the restore with no gap at all. The version
			 * that slept a flat 20 ms here ran 100 single-worker cycles clean;
			 * removing the gap made the same configuration fail about half the
			 * time. Kept adjustable rather than removed, because "fails only
			 * when snapshots come back to back" is a finding about cadence and
			 * not a licence to hide it. */
			Sleep(breath_ms());
			g_held->cycle++;
			if ((g_held->cycle % 25) == 0) {
				if (g_mode == MODE_MONO)
					printf("  %ld cycles, %ld bad, %ld caught mid-compile, "
					       "%ld compiles, %ld tracked\n",
					       (long)g_held->cycle, (long)g_held->bad,
					       (long)g_held->caught_jitting,
					       (long)g_compiles, (long)g_held->n);
				else
					printf("  %ld cycles, %ld bad, %ld caught mid-fill, "
					       "%ld fills\n",
					       (long)g_held->cycle, (long)g_held->bad,
					       (long)g_held->caught_filling, (long)g_fills);
			}
			continue;
		}
		/* A genuine save. Whether a worker was in flight at the instant of the
		 * snapshot is the fact the whole harness turns on, so it is recorded
		 * now rather than inferred later from a failure. */
		g_held->saves++;
		g_held->was_filling = g_filling;
		g_held->was_jitting = g_jitting;
		g_held->progress_at_save = g_progress;
		g_held->n_at_save = g_held->n;
		g_held->compiles_at_save = g_compiles;
		if (g_held->was_filling)
			g_held->caught_filling++;
		if (g_held->was_jitting)
			g_held->caught_jitting++;
		if (verify_now("before restore", (int)g_held->cycle))
			printf("cycle %ld: ALREADY inconsistent before the restore - the "
			       "fault is not the savestate's\n",
			       (long)g_held->cycle);
		Sleep(5);
		if (!savestate_load(0)) {
			printf("cycle %ld: load refused\n", (long)g_held->cycle);
			g_held->cycle++;
		}
	}

	InterlockedExchange(&g_stop, 1);
	for (i = 0; i < nthreads; i++)
		if (th[i])
			WaitForSingleObject(th[i], 2000);
	/* Removed on a clean exit so the next run does not read a row that was
	 * merely in flight when the run finished as one that killed it. */
	for (i = 0; i < INFLIGHT_MAX; i++)
		if (g_inflight[i]) {
			char name[64];
			CloseHandle(g_inflight[i]);
			g_inflight[i] = NULL;
			sprintf(name, "harness_inflight_%d.txt", i);
			DeleteFileA(name);
		}

	printf("done: %ld cycles, %ld save(s), %ld restore(s), %ld bad, %ld caught "
	       "mid-work\n",
	       (long)g_held->cycle, (long)g_held->saves, (long)g_held->restores,
	       (long)g_held->bad,
	       (long)(g_mode == MODE_MONO ? g_held->caught_jitting
					  : g_held->caught_filling));
	if (g_mode == MODE_MONO) {
		printf("oracle managed identity: %ld check(s) across %ld restore(s), %ld "
		       "returned null, %ld returned a different thread\n",
		       (long)g_held->tls_checks, (long)g_held->restores,
		       (long)g_held->tls_null, (long)g_held->tls_moved);
		printf("oracle pinned object: %ld check(s), %ld failed to resolve, %ld "
		       "came back with wrong contents\n",
		       (long)g_held->pin_checks, (long)g_held->pin_null,
		       (long)g_held->pin_wrong);
		printf("oracle full collection: %ld completed, worst %ld us, %ld KB in "
		       "use after the last one\n",
		       (long)g_held->gc_checks, (long)g_held->gc_worst_ms,
		       (long)g_held->gc_used);
		printf("oracle managed throw/catch: %ld check(s), %ld did not throw, %ld "
		       "threw the wrong type\n",
		       (long)g_held->exc_checks, (long)g_held->exc_missing,
		       (long)g_held->exc_wrong);
		printf("oracle managed call: %ld check(s), %ld failed, %ld returned the "
		       "wrong value\n",
		       (long)g_held->call_checks, (long)g_held->call_failed,
		       (long)g_held->call_wrong);
	}
	if (g_held->stalls) {
		static const char *edge[8] = { "<1ms",  "<2ms",  "<4ms",  "<8ms",
					       "<16ms", "<32ms", "<64ms", ">=64ms" };
		int b;

		printf("time from restore to the next completed unit of work: mean %.2f ms, "
		       "worst %.2f ms, %ld never resumed\n",
		       (double)g_held->stall_total_us / (double)g_held->stalls / 1000.0,
		       (double)g_held->stall_worst / 1000.0, (long)g_held->stall_never);
		printf("  ");
		for (b = 0; b < 8; b++)
			printf("%s %ld  ", edge[b], (long)g_held->bucket[b]);
		printf("\n");
	}
	return g_held->bad ? 1 : 0;
}
