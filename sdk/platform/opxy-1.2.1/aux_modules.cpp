// Stock OS 1.2.1 aux tracks 1..6 (tracks 8..13) with aux modules. Shift and
// an aux track's key reach the auxiliary controller's selector (8c08f60c),
// which stock opens only for the sends (tracks 14 and 15); for the other aux
// tracks it lists the track's stock module and the collection's aux modules.
// While a module is on a track, its page replaces M1 and the encoders on it
// edit the module's controls, the track's words 4..7, through stock's editor,
// which stores, locks and publishes them as any track parameter. Stock's own
// module keeps running beneath, on words 0..3, and M2..M4 stay stock's.
// GUI only; the choice is not saved with the project yet.
#include "aux_modules.hpp"
#include "stock.hpp"
#include "stock_browser.hpp"
#include "synth_controls.hpp"

extern "C" {
opxy::u32 sdk_aux_screen() __asm__("sdk_aux_screen");
opxy::u32 sdk_aux_turn(opxy::u32, opxy::u32) __asm__("sdk_aux_turn");
void sdk_aux_select(opxy::u32) __asm__("sdk_aux_select");
}

namespace {
using opxy::i32;
using opxy::u32;
using opxy::u8;
using namespace opxy::platform;
namespace browser = opxy::platform::stock_browser;

// The auxiliary controller's page: 0 on M1 .. 3 on M4 (8c08f53c reads it).
const u32 kAuxPage = 0x0801eae4;
// The GUI's project, as both controllers pass it to stock's editor.
const u32 kProject = 0x86bd027c;
const char* const kStockModules[kAuxTracks] = {"brain", "punch-in", "midi", "cv", "audio", "tape"};

// Each aux track's module, as its index + 1, or 0 for stock's.
u8 chosen[kAuxTracks];
// Each module control's value as last applied to the instrument tracks.
i32 applied[kAuxTracks][4];

const AuxModule* module(u32 track) {
    const u32 a = track - kAuxFirst;
    if (a >= kAuxTracks || !chosen[a]) return nullptr;
    const AuxModule& m = sdk_aux_modules[chosen[a] - 1];
    return m.ready() == 1 ? &m : nullptr;
}

// A track's current word (stock's editor reads and writes these, 8d5ad5bc)
// and the word the project stores (and saves).
i32 current(u32 track, u32 slot) {
    return *reinterpret_cast<const volatile i32*>(stock<u32(u32)>(0x8d587708)(track) + 0x3864 + slot * 4);
}
i32 stored(u32 track, u32 slot) {
    return *reinterpret_cast<const volatile i32*>(kProject + 0x4080 + track * 0xe8 + slot * 4);
}
// Stock's set: stores the word and sends it to the audio side (8d5acd9c),
// as for any edit.
void set(u32 track, u32 slot, i32 value) { stock<void(u32, u32, u32, i32)>(0x8d5acf28)(kProject, track, slot, value); }

// Once a frame (sdk_send_refresh): a macro control that moved sets its
// parameter on every instrument track that does not already hold the value.
void tick() {
    for (u32 a = 0; a < kAuxTracks; ++a) {
        const AuxModule* m = module(kAuxFirst + a);
        if (!m) continue;
        for (u32 i = 0; i < 4; ++i) {
            if (m->macro[i] < 0) continue;
            const i32 value = current(kAuxFirst + a, kAuxControlWord + i);
            if (value == applied[a][i]) continue;
            applied[a][i] = value;
            for (u32 track = 0; track < kTracks; ++track)
                if (stored(track, static_cast<u32>(m->macro[i])) != value)
                    set(track, static_cast<u32>(m->macro[i]), value);
        }
    }
}

// A module put on a track: each macro control picks up its parameter from
// instrument track 1, so the other tracks follow only once it is turned.
void place(u32 track, u32 choice) {
    const u32 a = track - kAuxFirst;
    if (a >= kAuxTracks) return;
    chosen[a] = static_cast<u8>(choice);
    const AuxModule* m = module(track);
    sdk_aux_engines[a] = m ? m->engine() : 0;
    sdk_aux_tick = tick;
    if (!m) return;
    for (u32 i = 0; i < 4; ++i) {
        if (m->macro[i] < 0) continue;
        const i32 value = stored(0, static_cast<u32>(m->macro[i]));
        set(track, kAuxControlWord + i, value);
        applied[a][i] = value;
    }
}

// The open selector: its track, and which module each row after the first is.
browser::StockSelector<kAuxModuleLimit + 1> selector = {};
u32 selecting;
u8 rows[kAuxModuleLimit + 1];

void choose(void*, u32 index) { place(selecting, rows[index]); }
}

// 8c08f624: the selector's exit for any track but a send; R4 holds the track.
void sdk_aux_select(u32 track) {
    const u32 a = track - kAuxFirst;
    if (a >= kAuxTracks) return;
    const char* labels[kAuxModuleLimit + 1] = {kStockModules[a]};
    rows[0] = 0;
    u32 count = 1, initial = 0;
    for (u32 i = 0; i < sdk_aux_module_count && count <= kAuxModuleLimit; ++i) {
        if (sdk_aux_modules[i].ready() != 1) continue;
        if (chosen[a] == i + 1) initial = count;
        labels[count] = sdk_aux_modules[i].label;
        rows[count++] = static_cast<u8>(i + 1);
    }
    if (count == 1) return;
    selecting = track;
    selector.open("aux module", labels, count, initial, browser::Callbacks{nullptr, choose, nullptr});
}

// 8c08f540: the auxiliary controller's page choice (its method 11). On M1 of
// a track with a module, the module's page; 0 leaves the choice to stock.
u32 sdk_aux_screen() {
    const AuxModule* m = word(kAuxPage) == 0 ? module(byte(kSelectedTrack)) : nullptr;
    return m ? m->screen() : 0;
}

// 8c08fa14: the auxiliary controller's turn (its method 6), with the encoder
// 0..3, or 4..7 with Shift. On M1 of a track with a module, the turn edits the
// module's control through stock's editor and the controller returns 1;
// Shift turns a continuous control in tenths, as on an owned synth's page.
// 0 leaves the turn to stock.
u32 sdk_aux_turn(u32 encoder, u32 event) {
    const u32 track = byte(kSelectedTrack);
    const AuxModule* m = word(kAuxPage) == 0 && encoder < 8 ? module(track) : nullptr;
    if (!m) return 0;
    const u32 slot = kAuxControlWord + (encoder & 3);
    const SynthControls* controls = sdk_synth_controls_of(m->engine());
    if (event && controls && (controls->continuous & (1u << (encoder & 3))) && shift_held()) {
        const i32* source = reinterpret_cast<const i32*>(event);
        const i32 fine[3] = {source[0] / 10, source[1], source[2]};
        stock<u32(u32, u32, u32, const i32*)>(0x8d5ad5bc)(kProject, track, slot, fine);
    } else {
        stock<u32(u32, u32, u32, u32)>(0x8d5ad5bc)(kProject, track, slot, event);
    }
    return 1;
}
