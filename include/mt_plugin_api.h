/* Motor Town native plugin API -- what the loader (version.dll) gives a plugin.
 *
 * A plugin is Binaries\Win64\plugins\mt_plugin_<Name>.dll exporting
 *
 *     extern "C" __declspec(dllexport) int PluginInit(const MtApi* api);
 *
 * returning 0 on success. The loader calls it once, from its own thread, in
 * alphabetical order of file name. Do the work on a thread of your own;
 * PluginInit should return quickly.
 *
 * The loader owns everything invasive or shared -- the ProcessEvent hook and
 * engine discovery happen exactly once, there. A plugin never patches vtables
 * itself: two hooks on the same slots break one another.
 *
 * Compatibility: fields are only ever appended. Check api->version (and
 * api->size before touching a field added later) and refuse to start if the
 * loader is older than what you need.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define MT_API_VERSION 8

/* Offsets the loader detected at runtime (nothing here is hardcoded). */
typedef struct MtLayout {
    /* UObjectBase */
    int32_t obj_flags, obj_index, obj_class, obj_name, obj_outer;
    /* UStruct: super struct, ChildProperties (FField*); PropertiesSize follows ChildProperties */
    int32_t struct_super, struct_child_props;
    /* FField / FProperty */
    int32_t field_next, field_name, field_offset;
    /* FUObjectItem size in the object table */
    int32_t object_stride;
} MtLayout;

typedef void (*MtGameThreadFn)(void* ctx);

/* Called on the game thread, inside the loader's one ProcessEvent hook, when a
   watched UFunction fires. parms is the function's parameter block: read it, or
   edit it in place. Return non-zero to block the original call (0 = let it
   proceed). */
typedef int (*MtPeHookFn)(uintptr_t obj, uintptr_t func, void* parms, void* ctx);

typedef struct MtApi {
    uint32_t version;  /* MT_API_VERSION of the loader */
    uint32_t size;     /* sizeof(MtApi) of the loader */

    /* This plugin's log: plugins\mt_plugin_<Name>.log, one line per call. */
    void (*log)(const struct MtApi* self, const char* fmt, ...);

    /* Blocks until engine discovery is done (or timeout; 0xFFFFFFFF = forever).
       Returns 1 when ready. Everything below needs it, except read/write. */
    int (*wait_engine)(uint32_t timeout_ms);
    const MtLayout* layout;

    /* Game memory, SEH-guarded: a bad pointer returns 0, never crashes. */
    int (*read)(uintptr_t addr, void* out, size_t n);
    int (*write)(uintptr_t addr, const void* in, size_t n);

    /* Names. Return the string length written (0 if none), always NUL-terminated. */
    int (*name)(uint32_t name_index, char* buf, int cap);  /* FName pool entry */
    int (*fname)(uintptr_t at, char* buf, int cap);        /* FName at an address, with _N suffix */

    /* Objects */
    int (*num_objects)(void);
    uintptr_t (*object)(int index);
    uintptr_t (*find_object)(const char* cls, const char* name, const char* outer);
    int (*is_a)(uintptr_t cls, uintptr_t target);  /* walks supers */

    /* Reflection: the FProperty called `name` on cls or a super (0 / -1 if absent). */
    uintptr_t (*field)(uintptr_t cls, const char* name);
    int (*field_offset)(uintptr_t cls, const char* name);

    /* Game thread: fn(ctx) runs on the thread that loaded the game (inside the one
       ProcessEvent hook), soon after the hook is installed. Returns 1 if queued. */
    int (*run_on_game_thread)(MtGameThreadFn fn, void* ctx);
    /* Call a UFunction. Only from the game thread (inside run_on_game_thread). */
    void (*process_event)(uintptr_t obj, uintptr_t func, void* parms);
    /* 1 once the hook is in (world loaded): queued work then runs within a frame. */
    int (*game_thread_ready)(void);

    void* plugin;  /* the loader's handle for this plugin; pass `api` to log */

    /* Watch UFunctions by name: fn is called on the game thread, inside the one
       ProcessEvent hook, whenever the invoked UFunction's name matches one of
       names[0..count). fn may read or modify parms in place; return non-zero to
       block the original call. Returns 1 if at least one name was registered.
       A name that the game has not loaded yet is not registered -- retry later. */
    int (*watch_functions)(const char* const* names, int count, MtPeHookFn fn, void* ctx);

    /* The pool index of a name (0 if not in the pool). The reverse of name(). */
    int (*name_index)(const char* name);

    /* Overwrite bytes in the game's own code -- e.g. neutralising a branch.
       Makes the page writable for the write, restores the protection, and
       flushes the instruction cache. Returns 1 on success. */
    int (*patch_bytes)(uintptr_t addr, const void* bytes, int len);

    /* The game exe's load address (its image base). Static addresses from a
       disassembler are converted with: runtime = static - 0x140000000 + this. */
    uintptr_t (*module_base)(void);

    /* ---- ini files (v6) ----
     * The loader owns the .ini files the plugins use, so two plugins that set
     * different keys of the same file do not overwrite each other: every write
     * goes through the loader, one key at a time, and the rest of the file --
     * other keys, other sections, comments -- is left alone.
     *
     * ini_set declares a value for a key:
     *   file/section/key/value are wide strings. file is a path relative to
     *   the game folder (Binaries\Win64), where these files have always lived.
     *   force = 0: a default. If the file already holds a value for that key,
     *           the file wins -- the user edits these by hand and keeps what
     *           they typed. Otherwise the value is written now.
     *   force = 1: write it through and always win (live state).
     *   Returns 1 if your value is the one that applies. 0 means it is not:
     *   the file already had a value, or another plugin declared that key
     *   first and keeps it (the plugin that loaded first wins; the loader logs
     *   the conflict). Read the value that applies with ini_get.
     * ini_get reads the merged value: what was declared for the key, or what
     *   the file holds. Returns the length written, 0 if there is none.
     */
    int (*ini_set)(const struct MtApi* self, const wchar_t* file, const wchar_t* section,
                   const wchar_t* key, const wchar_t* value, int force);
    int (*ini_get)(const wchar_t* file, const wchar_t* section, const wchar_t* key, wchar_t* buf, int cap);
    /* The engine's name pool: every name the game registered, walked one by
       one. fn is called with each name; return non-zero to stop the walk.
       This is where the console variables are: they are registered by FName,
       so their names are pool entries and not strings in the binary.
       Returns how many names were handed over. */
    int (*pool_names)(int (*fn)(const char* name, void* ctx), void* ctx);
    /* ---- v7 ----
       Keep an object for the rest of the session (AddToRoot): for assets a plugin loads and gives to engine
       components, which nothing in the engine references, so a GC would free them while still in use.
       Returns 1 when done, 0 when the loader could not learn the object flags (then keep the object some other way).
       Since v7 object() also returns 0 for an object the game destroyed but has not collected yet. */
    int (*keep)(uintptr_t obj);
    /* ---- v8 ----
       The game is on its way out. Returns 1 once the engine has started to shut down, and stays 1.
       The loader knows it because the world is gone and no new one comes: a map travel brings a new
       world within a second or two, the exit brings none.
       While this is 1 the loader does not run any plugin work (no game-thread queue, no watched
       function) and every api->process_event does nothing -- so a plugin cannot touch the engine
       while the engine is taking itself apart. That is what the game hung on: a plugin's tick ran
       inside the teardown and waited for a lock one of its own threads held while it was inside
       the engine (the user, 2026-11-17: "whenever I'm closing the game it gets stuck on stopping").
       A plugin's own thread should watch this and end itself: if (api->stopping()) return 0;
     */
    int (*stopping)(void);
} MtApi;

typedef int (*MtPluginInitFn)(const MtApi* api);
