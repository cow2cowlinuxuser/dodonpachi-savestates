#ifndef D3D9_SW_SWALLOC_H
#define D3D9_SW_SWALLOC_H

#include <stddef.h>
#include <windows.h>

/* The wrapper's own heap.
 *
 * Building against msvcrt put this DLL's allocations on msvcrt's heap, which is
 * not this DLL's heap at all - half of Windows links the same runtime. That
 * left the rewind with an impossible choice. Rewinding msvcrt's heap moves
 * memory belonging to system components whose own data sections stay in the
 * present, which is the exact split that corrupts an allocator; leaving it in
 * the present strands the wrapper's own objects while the game's pointers to
 * them travel back.
 *
 * A private heap removes the choice. Everything the wrapper allocates lives
 * somewhere no one else allocates from, so it can be rewound wholesale
 * alongside the game, and msvcrt's heap can be left alone with the modules that
 * share it.
 *
 * Blocks come back 64-byte aligned, which the raw heap does not promise and the
 * vector kernels are happier with. */
void *sw_malloc(size_t n);
void *sw_calloc(size_t count, size_t size);
void *sw_realloc(void *p, size_t n);
void sw_free(void *p);
HANDLE sw_heap(void);

/* The file that defines them opts out; everyone else gets redirected. Include
 * this after the standard headers so the declarations in stdlib.h are untouched. */
#ifndef SW_ALLOC_IMPL
#define malloc sw_malloc
#define calloc sw_calloc
#define realloc sw_realloc
#define free sw_free
#endif

#endif
