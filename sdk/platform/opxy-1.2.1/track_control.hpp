// Synths with track control: generated table (track_control_table.cpp.in);
// the GUI-tick overlay lives in track_control.cpp.
#pragma once
#include <opxy/track_control.hpp>

namespace opxy { namespace platform {

// In a browser kit an unselected machine's ready resolves to a function returning 0.
struct ControlMachine {
    const char* id;  // absolute symbol: the engine ID, supplied at link time
    int (*ready)();
    // Each copy's state: copy 0 for a unique machine, else the copy's track.
    void (*reset)(u32 copy);
    void (*control)(u32 copy, const control::Input*, control::Overlay*);
    // What it keeps with each project (project_companion.cpp): its machine ID,
    // at most saved_bytes, and save/load, or 0 and null when it keeps nothing.
    const char* name;
    u32 saved_bytes;
    u32 (*save)(u8* data);
    void (*load)(const u8* data, u32 bytes);
    bool unique;  // runs only while one track holds it
    // The aux tracks it may take over (aux_tracks.cpp), one bit per track
    // index, or 0; then its own screen page (an absolute symbol, or null) and
    // its eight controls' defaults, which it starts from on an aux track.
    u32 aux;
    const char* page;
    i32 defaults[8];
    u32 engine() const { return static_cast<u32>(reinterpret_cast<__UINTPTR_TYPE__>(id)); }
    u32 page_id() const { return static_cast<u32>(reinterpret_cast<__UINTPTR_TYPE__>(page)); }
};
// The engine whose machine runs on a track: stock's engine on an instrument
// track, the machine holding an aux track, else 0. GUI only (track_control.cpp).
u32 control_engine(u32 track);
// The aux service (aux_tracks.cpp), linked only for a machine that takes over aux
// tracks: its entry points, which its startup fills in; null without it.
struct AuxService {
    u32 (*engine)(u32 track);           // the engine of the machine holding an aux track, or 0
    void (*tick)();                     // once per GUI tick, before the machines run
    const char* (*holder)(u32 track);   // what to save: the holder's machine ID, or null
    const i32* (*kept)(u32 track);      // and the stock role's controls kept aside
    void (*restore)(u32 track, const char* id, u32 id_length, const i32* controls);  // after a load
    void (*forget)();                   // before a load and for a new project: every role stock
};
// The marks track control shows over every page (track_control_overlay.cpp.in):
// a machine captures edits, or the selected track holds an overlay from one that
// marks; `machine` indexes sdk_control_machines. GUI only.
const u32 kMarkCapture = 1, kMarkOverlaid = 2;
struct Marks { u32 kind, machine; };
Marks control_marks();
}}

extern "C" {
extern const opxy::platform::ControlMachine sdk_control_machines[] __asm__("sdk_control_machines");
extern const opxy::u32 sdk_control_machine_count __asm__("sdk_control_machine_count");
void sdk_gui_tick() __asm__("sdk_gui_tick");
// Registers the marks' screen slot on the GUI role, after the machines' screens.
void sdk_overlay_startup() __asm__("sdk_overlay_startup");
extern opxy::platform::AuxService sdk_aux __asm__("sdk_aux");
// The aux service's startup on the GUI role, from startup.cpp when it is linked.
void sdk_aux_startup() __asm__("sdk_aux_startup");
void sdk_param_edit_set(opxy::u32 project, opxy::u32 track, opxy::u32 index, opxy::i32 value)
    __asm__("sdk_param_edit_set");
// A turn event (stock's step first) of `track`'s encoder at parameter `slot`:
// true when the track's machine takes its own turns, which then edit nothing.
bool sdk_control_turn_offer(opxy::u32 track, opxy::u32 slot, const opxy::i32* event)
    __asm__("sdk_control_turn_offer");
// The value stock's displayed-value getter shows before its LFO amount, through
// the jump hook sdk_param_shown_hook (track_control_hooks.s).
opxy::i32 sdk_param_shown(opxy::u32 project, opxy::u32 track, opxy::u32 index, opxy::u32 playing)
    __asm__("sdk_param_shown");
}
