#ifndef D3D9_SW_SAVESTATE_H
#define D3D9_SW_SAVESTATE_H

#include <stddef.h> /* size_t, for savestate_exclude */

/* In-process rewind. Must be called from the thread that is inside Present,
 * at a point where the rasteriser has already flushed. Returns 1 on success.
 *
 * Scope is deliberately narrow: rewind within a single session, held in RAM,
 * never written to disk. That removes handle re-creation, which is the part
 * that makes general process checkpointing intractable on Windows. */
/* Redirects the game's clock and event imports. Idempotent, and called at the
 * first D3D entry point so events created during start-up are still seen. */
void savestate_hooks_install(void);

int savestate_save(int slot);
int savestate_load(int slot);
int savestate_slot_valid(int slot);
/* Cheap, call once per frame. Reclaims addresses the snapshot still needs
 * before another allocator can take them. */
void savestate_guard(void);
double savestate_last_ms(void);
double savestate_last_mb(void);
/* Nonzero if the operation that just completed was a restore, whichever of
 * savestate_save and savestate_load the caller invoked. A thread restored from a
 * snapshot resumes inside the save it was taking and returns through it, so a
 * successful restore arrives back in the caller's save branch. */
int savestate_last_was_restore(void);
/* Walks every heap's block headers and logs what is in them, comparing against
 * the session's first census. Observation only - it neither saves nor restores,
 * and that is what makes it able to take the heap's lock safely. Take one before
 * a scene transition and one after to see what the transition actually did. */
void savestate_census(void);
/* Milliseconds since the session's first restore; negative before there is one.
 * Never reset by later restores - it is a survival time, not a lap timer. */
double savestate_live_ms(void);

/* Holds a range in the present: never captured, never rewound.
 *
 * For state that has to survive a restore in order to observe one. A restore
 * rewinds every thread, including the one that asked for it, so anything a caller
 * keeps on its stack or in rewound memory travels back with the program - a
 * counter incremented across restores simply returns to its old value, and a loop
 * driven by one can never reach its second iteration. The test harness hit exactly
 * that and could not advance past cycle one.
 *
 * Register before the first save. Ranges added here are permanent and survive
 * every later rebuild of the exclusion list. */
void savestate_exclude(void *p, size_t bytes);

/* A setting's value, environment first and then d3d9_sw.cfg beside the log.
 * Returns the length written, 0 if unset.
 *
 * Exposed because the wrapper needs knobs the same way this engine does, and
 * for the same reason: a game launched through Steam does not inherit variables
 * typed into a shell, so plain getenv silently ignores whatever the user set.
 * Every knob that has to be settable in practice should come through here. */
unsigned savestate_getenv(const char *name, char *buf, unsigned cap);

/* One slot. Each costs a full copy of the game's committed memory, which for
 * this title is well over a gigabyte of physical RAM. */
#define SAVESTATE_SLOTS 1

#endif
