// Stock OS 1.2.1 hidden instrument pages (definition "hidePages"): part of the
// control service, so only builds with a machine that hides pages or uses track
// control link it. Track control's GUI tick (track_control.cpp) checks the page
// once per tick, and switches off the stock LFO of any instrument track whose
// synth hides the LFO page. Pages, not the LFO switch, work the same on an aux
// track a machine took over (aux_tracks.cpp), whose page is the aux controller's.
#include "synth_controls.hpp"
#include "stock.hpp"
#include "track_control.hpp"

// 8c0b19c4 sets the page word M1..M4 select, wherever stock does; the page
// keys, track changes and menus all reach it. A tick after it shows a page the
// selected synth hides, the page shown before it comes back.
//
// On aux tracks the page word is 0801eae4 (M1..M4 as 0..3), set by the aux
// controller's 8c08f74c(controller, tab), which uses only the tab. A held
// track's hidden tab shows the machine's own page (aux_tracks.cpp), so nothing
// flashes; the tick sets the tab back as above.
//
// Page keys reach a machine hiding pages from stock's held-key list, not from
// the page word: M1 and a shown page's key leave the word as it was, and
// stock's M2 selects no tab on tracks without a second page (CV among them).
// Each tick keeps the first of M1..M4 to go down since the last, without
// Shift (Shift + M1 is the aux engine list), for the selected track's machine
// (sdk_synth_page_key).
namespace {
using opxy::u32;
using opxy::platform::SynthControls;
using namespace opxy::platform;

// The instrument track controller's page word: 0 on the engine page, 1..3 on the others.
const u32 kInstrumentPage = 0x080238bc;
const u32 kAuxPage = 0x0801eae4, kAuxController = 0x08014db4;
const u32 kAllTracks = 16;
u32 page_word(u32 track) { return word(track < kTracks ? kInstrumentPage : kAuxPage); }
void set_page(u32 track, u32 page) {
    if (track < kTracks) stock<void(u32)>(0x8c0b19c4)(page);
    else stock<void(u32, u32)>(0x8c08f74c)(kAuxController, page);
}

// A track's stock LFO is on while the byte at track data + 0x24 is set: stock's
// LFO update (8d58a9b4) reads it through 8d5879c0, and the M4 page's on/off sets
// it through 8d587984, which also marks the project changed.
const u32 kLfoPage = 3;
bool lfo_on(u32 track) { return stock<u32(u32)>(0x8d5879c0)(track) != 0; }
void lfo_off(u32 track) { stock<void(u32, u32)>(0x8d587984)(track, 0); }

const SynthControls* owned(u32 engine) {
    for (u32 i = 0; i < sdk_synth_control_count; ++i) {
        const SynthControls& m = sdk_synth_controls[i];
        if (m.engine() == engine && !m.effect) return m.ready() == 1 ? &m : nullptr;
    }
    return nullptr;
}

// The engine whose machine runs on a track: the machine holding an aux track.
u32 engine_on(u32 track) { return track < kTracks ? track_engine(track) : control_engine(track); }

// The last stock page shown that the selected synth does not hide.
u32 page_before;
// Stock's key numbers for M1..M4, and those held at the last tick, a bit each.
const u32 kPageKeys = 6;
u32 keys_down;
u32 key_track = kAllTracks, key_number;

// The page keys that went down since the last tick.
u32 page_keys_pressed() {
    u32 down = 0;
    for (u32 k = 0; k < opxy::page_controls; ++k)
        if (key_held(kPageKeys + k)) down |= 1u << k;
    const u32 pressed = down & ~keys_down;
    keys_down = down;
    return pressed;
}
}

void sdk_synth_pages() {
    // A synth that hides the LFO page leaves no way to reach stock's LFO, so a
    // stock LFO left on by the track's previous engine is switched off.
    // Instrument tracks only: that byte is measured there, not on aux tracks.
    for (u32 t = 0; t < kTracks; ++t) {
        const SynthControls* hiding = owned(track_engine(t));
        if (hiding && (hiding->hidden_pages >> kLfoPage & 1) && lfo_on(t)) lfo_off(t);
    }
    const u32 pressed = page_keys_pressed();
    const u32 track = selected_track();
    if (track >= kAllTracks) return;
    const u32 engine = engine_on(track);
    const SynthControls* m = engine ? owned(engine) : nullptr;
    if (m && m->hidden_pages && pressed && !shift_held()) {
        key_track = track;
        key_number = 1;
        while (!(pressed >> (key_number - 1) & 1)) ++key_number;
    }
    const u32 page = page_word(track);
    if (page >= opxy::page_controls) return;
    if (!m || !(m->hidden_pages >> page & 1)) {
        page_before = page;
        return;
    }
    set_page(track, m->hidden_pages >> page_before & 1 ? 0 : page_before);
}

u32 sdk_synth_page_key(u32 track) {
    if (track != key_track) return 0;
    key_track = kAllTracks;
    return key_number;
}
