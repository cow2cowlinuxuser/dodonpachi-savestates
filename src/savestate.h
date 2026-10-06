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
void savestate_reserve_home(void);

/* Records a redirection we have installed, so the inventory can report what this
 * wrapper is in a position to observe. Declared here because the hooks are
 * spread across several translation units - dsoundhook.c among them - and the
 * whole point of the record is that no single file knows all of them. */
void ss_hook_note(const char *kind, const char *what, void *at, int sites);

int savestate_save(int slot);
int savestate_load(int slot);
int savestate_slot_valid(int slot);
/* Cheap, call once per frame. Reclaims addresses the snapshot still needs
 * before another allocator can take them. */
void savestate_guard(void);
/* 1 once after any load: the backbuffer still holds what was drawn before it,
 * and the game's device objects may not be the ones it saved with. */
int savestate_merged_clear(void);
/* A D3D9SW_ knob from the cfg beside the game, or def. */
int savestate_knob(const char *name, int def);
/* 1 once after a save. */
int savestate_saved_since(void);
/* Milliseconds spent inside savestate_guard, and inside the audio drain it
 * makes, since the last call. Both are reset by reading them. */
void savestate_perf_take(double *guard_ms, double *audio_ms);
double savestate_last_ms(void);
double savestate_last_mb(void);
/* Nonzero if the operation that just completed was a restore, whichever of
 * savestate_save and savestate_load the caller invoked. A thread restored from a
 * snapshot resumes inside the save it was taking and returns through it, so a
 * successful restore arrives back in the caller's save branch. */
int savestate_last_was_restore(void);
/* Nonzero if the most recent load wrote back a slot saved by another process.
 * Per-process bookkeeping - lists of what this process created since the save -
 * describes a process that the restored memory knows nothing about. */
int savestate_last_load_foreign(void);

/* Memory we hold in the present whose contents are nonetheless made to match
 * the save after a load from another process - the renderer's object pool. A
 * game global pointing into it keeps its saved value. */
void savestate_follow_save_range(const void *base, size_t bytes);

/* After a load: 1 if p lies in a region the load wrote, 0 if in one it saved
 * but left in the present, -1 if the slot never held it or nothing was loaded. */
int savestate_addr_restored(const void *p);
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

/* A global that belongs to this process rather than to the game's moment: a
 * pointer to an excluded or OS-placed block, a handle, a thread. These sit in a
 * section of their own that a restore never writes, because from another launch
 * the saved values are the old process's - the rest of this image rewinds. */
#define SS_PRESENT __attribute__((section(".sspres")))

/* A lock in this DLL's own data. It rewinds with the threads that hold it, but
 * its debug link and wait handle belong to the process, so a load from another
 * launch repairs them the way it repairs the game's. Call after initialising. */
struct _RTL_CRITICAL_SECTION;
void savestate_own_cs(struct _RTL_CRITICAL_SECTION *cs);

/* Zeroed memory held in the present, allocated once per id for the life of the
 * process. The wrapper's own globals rewind with the game, so a static "already
 * allocated" pointer goes back to NULL on every restore and the next call
 * allocates and excludes another buffer; this remembers it in the control
 * block, which never rewinds. NULL if the id is out of range or memory is out. */
enum { SS_KEPT_CHUNK_A, SS_KEPT_CHUNK_B, SS_KEPT_DS_PRESENT, SS_KEPT_DS_SINK, SS_KEPT };
void *savestate_kept(int id, size_t bytes);

/* The pre-suspend heap census (gameheap.c) calls this for each heap it judges to
 * be regenerable game ASSETS - a large, pure-data HeapCreate heap that balloons on
 * a room change - when D3D9SW_ASSETEXCL is set. heaps_partition then holds those
 * heaps in the present (heap_ours=0) instead of rewinding bytes that a cross-session
 * restore would land at a moved base; the game reloads the assets itself. Handles
 * are valid only within the save that noted them; the list is cleared each save. */
void savestate_note_asset_heap(void *h);

/* A setting's value, environment first and then d3d9_sw.cfg beside the log.
 * Returns the length written, 0 if unset.
 *
 * Exposed because the wrapper needs knobs the same way this engine does, and
 * for the same reason: a game launched through Steam does not inherit variables
 * typed into a shell, so plain getenv silently ignores whatever the user set.
 * Every knob that has to be settable in practice should come through here. */
unsigned savestate_getenv(const char *name, char *buf, unsigned cap);

/* True when the main executable has this file name.
 *
 * Parts of this wrapper carry offsets and byte patterns that only mean anything
 * inside one game's binary. Nothing used to check which binary it was in, and
 * both places that do this were found reading and writing a stranger's image
 * within minutes of a test harness existing: the decoder patch turned a jne
 * into a jg after a nine-byte pattern matched by coincidence, and the dsound
 * cursor report dereferenced exe+0x50E18C in a 363 KB executable and faulted.
 * Call this before touching anything whose address came from a disassembly. */
int savestate_host_is(const char *exe_name);

/* Whether an address sits in a heap this engine takes back in time.
 *
 * 1 rewound, 0 left in the present, -1 not in any heap we know about. Writes
 * the heap's owner into name when there is one.
 *
 * For crash reports. Nearly every failure after a restore is a pointer whose
 * target sits on the other side of the rewind from the code holding it, and
 * which side is which decides the fix - so a fault handler that can name the
 * side turns a guess into a reading. Allocates nothing and takes no lock, so it
 * is safe from a handler that may be holding the heap lock already. */
int savestate_rewinds(const void *p, char *name, unsigned cap);

/* Thread-set mismatch after the most recent restore.
 *
 * The restore already compares the live thread set against the saved one and
 * logs the difference, but until now the counts lived only in the log. This
 * exports them cheaply so the harness can assert on them.
 *
 * Returns: count of threads present at save time that have exited.
 * Optionally fills *fresh (live now, not at save), *recycled (same entry point
 * under a new TID), *gone (same as the return value). */
int savestate_thread_set(int *fresh, int *recycled, int *gone);

/* Soak driver. F6 arms it; it then runs save/restore trials on its own with a
 * doubling dwell, so one manual session yields a survival curve instead of a
 * single number. savestate_soak_action is called once per frame and says what
 * it wants done - the wrapper still performs the save or load itself, because
 * the ledger work around both must not get a second call site. */
#define SS_SOAK_NOTHING 0
#define SS_SOAK_SAVE 1
#define SS_SOAK_LOAD 2
/* The eight-byte savestate: F11 marks the player position, shift-F11 writes it
 * back. Rabi-Ribi v1.65 only, checked by image size before anything is read. */
void savestate_pos_mark(void);
void savestate_pos_restore(void);
/* Called every frame. Notices when the game crosses a load boundary of its own
 * accord and censuses the heaps across it, which is the only view we have of
 * what the game itself treats as per-load state versus permanent state. */
void savestate_pos_watch(void);
void savestate_object_watch(void);
void savestate_object_report(void);
/* F7. Takes a census right now, so two presses with only play between them
 * measure what play alone changes, uncontaminated by a load. */
void savestate_probe_census(void);
/* Reliable hotkey edges. GetAsyncKeyState's low bit is consumed by whoever
 * reads it first, and the game polls the keyboard as well, so presses were
 * being lost. These track the physical key bit instead. */
int savestate_key_edge(int vk);
int savestate_key_held(int vk);

/* What the hotkeys asked for on this frame, for one slot.
 *
 * The binding lives here rather than in each wrapper because there are three
 * of them and they had drifted: d3d11_sw went through savestate_key_edge while
 * d3d9_sw and gl_sw still read GetAsyncKeyState's low bit directly, which is
 * the unreliable path the comment above describes. One resolver means one
 * answer to "which key saves" for every backend and every title.
 *
 * Shift is the part that does not survive automation. A modifier has to be
 * held across a second keystroke, and neither Wine's input path nor a remote
 * driver reproduces that reliably - the modifier arrives, the key arrives, and
 * the two are not overlapping by the time anything reads them. A load key of
 * its own needs no overlap, so D3D9SW_LOAD_VK is what makes an unattended run
 * able to restore at all. Shift+save remains the default, so nothing changes
 * for a player who sets nothing. */
enum { SS_HOTKEY_NONE = 0, SS_HOTKEY_SAVE, SS_HOTKEY_LOAD };
int savestate_hotkey(int slot);

void savestate_chain_probe(void);

/* Hold every thread still for a while and then let them go, copying nothing.
 * The control experiment for every restore failure: a restore freezes, copies
 * and writes back, and only the last two have ever been varied. */
int savestate_park(int ms);

/* Stands a hardware backend down around a save, restore or park, the way
 * dsh_quiet already does for audio. Null unless a front end registers one. */
void savestate_set_gpu_park(void (*fn)(int on));

/* Told by the present path when the window changes state: 0 foreground,
 * 1 background, 2 minimised. Recorded in a small ring and printed by the fault
 * report, because a focus change is the thing that wakes the libraries we do
 * not rewind and is therefore the last step of more than one crash. */
void savestate_note_focus(int state);

/* Called once per presented frame. Emits the watched addresses for a while
 * after a save or a restore when D3D9SW_WATCH_TRACE is set, so a value can be
 * judged by the shape of its motion rather than by two samples. */
void savestate_watch_tick(void);

/* Write the main module out as it exists in memory, for a disassembler. The
 * game's .text is encrypted on disk behind a Steam stub; in here it is not. */
int savestate_dump_image(void);

void savestate_soak_arm(void);
int savestate_soak_action(void);

/* Does the saved image at this heap offset look like one of our busy malloc
 * blocks? live_head is the address that byte would occupy in the live heap
 * (the heap does not move). remain is bytes from there to the end of the
 * region. Writes the header-plus-payload length to *n on success. */
int gameheap_saved_block(const void *saved, void *live_head, size_t remain, size_t *n);

/* Heapwalk without invoking the heap: the redirected allocator already sees
 * every busy block. Snapshot after suspend; restore walks that list. Empty
 * snapshot (cross-session, or the table never reserved) falls back to
 * gameheap_saved_block. */
unsigned gameheap_busy_snapshot(void);
unsigned gameheap_busy_saved_count(void);
int gameheap_busy_saved_at(unsigned i, void **head, size_t *total);
void gameheap_busy_rewind(void);

/* The texture pack this launch reads: contents and bytes. A save records it as
 * an expectation; a load from a launch with a smaller pack paints magenta. */
void savestate_note_texpack(unsigned contents, unsigned long long bytes);

/* Save slots held at once. Each slot's captured BYTES live in its own section or
 * slotfile (disk-backed under D3D9SW_SLOTFILE=1), so a slot costs ~1.8 MB of
 * control-block metadata plus one snapshot's worth of storage - not a gigabyte of
 * RAM apiece. Each slot restores independently because wholesale restore captures
 * the owned heap whole, so two saves are two byte images with no shared address
 * identity to chain them. Validate 2 before leaning on 3-4. Slot selection is by
 * key: save-slot-N on g_vk_save+N, load-slot-N on g_vk_load+N (keep the save and
 * load key bases at least SAVESTATE_SLOTS apart, or they overlap - the hotkey
 * code warns). */
#define SAVESTATE_SLOTS 4

#endif
