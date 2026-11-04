// Owned synthesis kits using stock's per-key Drumline region transport.
#pragma once
#include <opxy/types.hpp>

namespace opxy { namespace platform {
struct DrumMachine {
    const char* id;
    int (*ready)();
    u32 model_count;
    void (*defaults)(u32 key, u8* model, u8* controls);
    const char* (*label)(u32 model, u32 slot);
    const char* (*model_name)(u32 model);
    u32 (*count)(u32 model);
    // Optional named groups of consecutive models (resources.modelGroups):
    // Shift and the first encoder choose a group, the second a model in it.
    u32 group_count;
    const u8* group_sizes;
    const char* const* group_names;
    // resources.padMix (SDK 1.10): Shift's third and fourth encoders turn the
    // key's stock pan and level, and a kit with groups takes its choke group to
    // Shift's second row.
    u32 pad_mix;
    // resources.padControls (SDK 1.10): the key's own small fields, on Shift's
    // second row after the choke group, each with its bits, a signed mask, a
    // label and the kit's caption for a value (null: the number).
    u32 pad_count;
    const u8* pad_bits;
    u32 pad_signs;
    const char* const* pad_labels;
    bool (*pad_text)(u32 field, i32 value, char* text);
    u32 engine() const { return static_cast<u32>(reinterpret_cast<__UINTPTR_TYPE__>(id)); }
    // A model's group (0 without groups) and the group's first model.
    u32 group(u32 model) const {
        u32 g = 0;
        for (u32 first = 0; g + 1 < group_count && model >= first + group_sizes[g]; ++g) first += group_sizes[g];
        return g;
    }
    u32 group_first(u32 g) const {
        u32 first = 0;
        for (u32 i = 0; i < g && i < group_count; ++i) first += group_sizes[i];
        return first;
    }
    // What Shift and the encoder at a position (0..3) choose for the selected
    // key, on the encoders' first row or (second) their second. A kit with
    // groups: the group, the model in it and the choke group, then nothing;
    // with padMix, the group, the model, pan and level on the first row and
    // the choke group on the second. A kit without groups: the model and the
    // choke group, then (padMix) pan and level, on either row.
    enum Shift { kShiftNone, kShiftGroup, kShiftModel, kShiftChoke, kShiftPan, kShiftLevel,
                 kShiftPad, kShiftPad1, kShiftPad2 };   // kShiftPad + i: pad control i
    // A second Shift row: the choke group first in a padMix kit with groups,
    // then the kit's pad controls on positions 1..3.
    bool second_shift_row() const { return (pad_mix && group_count) || pad_count; }
    // No tables: an inline function's statics would be defined in every
    // translation unit that uses it.
    Shift shift(u32 position, bool second) const {
        if (position >= 4) return kShiftNone;
        if (second && second_shift_row()) {
            if (position == 0) return pad_mix && group_count ? kShiftChoke : kShiftNone;
            return position - 1 < pad_count ? static_cast<Shift>(kShiftPad + position - 1) : kShiftNone;
        }
        if (group_count && position < 2) return position == 0 ? kShiftGroup : kShiftModel;
        if (!group_count && position < 2) return position == 0 ? kShiftModel : kShiftChoke;
        if (pad_mix) return position == 2 ? kShiftPan : kShiftLevel;
        return group_count && position == 2 ? kShiftChoke : kShiftNone;
    }
    // A slot the model uses: below its count and named. An unnamed slot inside
    // the count is a gap in the machine's own layout: blank and inert.
    bool used(u32 model, u32 slot) const {
        const char* name = slot < count(model) ? label(model, slot) : nullptr;
        return name && name[0];
    }
};
}}

extern "C" {
extern const opxy::platform::DrumMachine sdk_drum_machines[] __asm__("sdk_drum_machines");
extern const opxy::u32 sdk_drum_machine_count __asm__("sdk_drum_machine_count");
const opxy::platform::DrumMachine* sdk_drum_machine(opxy::u32 engine) __asm__("sdk_drum_machine");
opxy::u32 sdk_drum_alias(opxy::u32 engine) __asm__("sdk_drum_alias");
// GUI projection: the selected key's model and stored controls; track offsets
// (step locks, modulation) remain separate.
bool sdk_drum_values(opxy::u32 track, opxy::u8* model, opxy::u8* controls) __asm__("sdk_drum_values");
const char* sdk_drum_title(opxy::u32 engine, opxy::u32 slot) __asm__("sdk_drum_title");
// The selected key's canonical pad, 53..76, or 0 when the track is not an owned kit.
opxy::u32 sdk_drum_selected_pad(opxy::u32 track) __asm__("sdk_drum_selected_pad");
// The canonical pad a played note sounds on a track, 53..76, or 0. GUI only.
opxy::u32 sdk_drum_pad(opxy::u32 track, opxy::u32 note) __asm__("sdk_drum_pad");
// The selected key's choke group, as the Shift page names it.
const char* sdk_drum_choke(opxy::u32 track) __asm__("sdk_drum_choke");
// The same as a choice, 0 (none) to 3.
opxy::u32 sdk_drum_choke_choice(opxy::u32 track) __asm__("sdk_drum_choke_choice");
// The selected key's stock pan, -100..100 (0 centre), and its level in whole
// dB as a padMix kit plays it, -30..0; 0 when the track is not an owned kit.
opxy::i32 sdk_drum_pan(opxy::u32 track) __asm__("sdk_drum_pan");
opxy::i32 sdk_drum_level(opxy::u32 track) __asm__("sdk_drum_level");
// The selected key's pad controls (resources.padControls), each 0 when the key
// is not marked; returns their count.
opxy::u32 sdk_drum_pad_values(opxy::u32 track, opxy::i32* values) __asm__("sdk_drum_pad_values");
opxy::u32 sdk_drum_edit(opxy::u32 key, opxy::u32 control, const opxy::u32* event, opxy::u32 fine)
    __asm__("sdk_drum_edit");
}
