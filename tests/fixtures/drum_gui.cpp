#include <cstdio>
#include "stock.hpp"
#include "drum_machines.hpp"
#include "drum_storage.hpp"
#include "synth_controls.hpp"
#include <cstdlib>
#include <cstring>

extern "C" {
void sdk_synth_press(opxy::u32) __asm__("sdk_synth_press");
opxy::u32 sdk_synth_turn(opxy::u32, opxy::u32, opxy::u32, opxy::u32) __asm__("sdk_synth_turn");
}
namespace {
using namespace opxy;
using namespace opxy::platform;
u32 track, key = 53, engine = 34, page, controller = 0x8400dff8;
u32 publications, dirty, clicks, fallback, last_key, turned_slot;
int ownership = 1;
bool shifted, shifted_octave;
i32 regions[24][32];
void require_at(bool ok, int line) { if (!ok) { std::fprintf(stderr, "line %d\n", line); std::abort(); } }
#define require(ok) require_at(ok, __LINE__)
int ready() { return ownership; }
void defaults(u32 note, u8* model, u8* values) {
    *model = static_cast<u8>((note - 53) % 2);
    for (u32 i = 0; i < 8; ++i) values[i] = static_cast<u8>(20 + i);
    values[7] = static_cast<u8>(note);
}
// The full model leaves slots 2 and 7 unnamed: gaps inside its count.
const char* label(u32 model, u32 slot) { return model ? "short" : slot == 2 || slot == 7 ? "" : "full"; }
const char* name(u32 model) { return model ? "short" : "full"; }
u32 count(u32 model) { return model ? 3 : 8; }
bool format(u32, i32, char*) { return false; }
u32 get_engine(u32 selected) { require(selected == track); return engine; }
u32 get_key(u32 selected) { require(selected == track); return key; }
u32 get_track() { return track; }
u32 get_shift() { return shifted; }
u32 canonical(u32 selected, u32 note) { require(selected == track); return shifted_octave && note == 53 ? 65 : note; }
const u32* top() { return &controller; }
void press(u32) { ++clicks; }
void* region(u32 selected, u32 note, u32* scratch, u32 nearest) {
    note = canonical(selected, note);
    require(selected == track && note >= 53 && note < 77 && !nearest);
    *scratch = note - 53;
    return regions[note - 53];
}
void changed() { ++dirty; }
void publish(u32 note) { ++publications; last_key = note; }
u32 stock_edit(u32, u32, const u32*, u32) { ++fallback; return 1234; }
u32 turn(u32, u32 selected, u32 slot, u32) { require(selected == track); turned_slot = slot; return slot; }
const char* title(u32, u32) { return "stock"; }
u32 text(u32, const char*) { return 0; }
void shown_at(u32 a, u32 b, u32 c, u32 d, int line) {
    u8 slots[4]; sdk_synth_shown(track, slots);
    require_at(slots[0] == a && slots[1] == b && slots[2] == c && slots[3] == d, line);
}
#define shown(a, b, c, d) shown_at(a, b, c, d, __LINE__)
}

extern "C" {
// Every drum kit's press pages its whole row, as the generated table marks it.
// Engine 37 groups its five models as two and three; engine 38 has 33, so it
// stores its keys in the prefix code.
static const opxy::u8 group_sizes[] = {2, 3};
static const opxy::u8 pad_bits[] = {5, 3, 4};
static const char* const pad_labels[] = {"FILTER", "RES", "SRR"};
static bool pad_text(opxy::u32, opxy::i32, char*) { return false; }
static const char* const group_names[] = {"ONE", "TWO"};
const opxy::platform::DrumMachine sdk_drum_machines[] = {
    {reinterpret_cast<const char*>(34), ready, 2, defaults, label, name, count},
    {reinterpret_cast<const char*>(37), ready, 5, defaults, label, name, count, 2, group_sizes, group_names},
    {reinterpret_cast<const char*>(38), ready, 33, defaults, label, name, count},
    // padMix: engine 39 with groups, engine 40 without.
    {reinterpret_cast<const char*>(39), ready, 5, defaults, label, name, count, 2, group_sizes, group_names, 1},
    {reinterpret_cast<const char*>(40), ready, 2, defaults, label, name, count, 0, nullptr, nullptr, 1},
    // padMix with groups and three pad controls: FILTER (5 bits, signed), RES (3), SRR (4).
    {reinterpret_cast<const char*>(41), ready, 5, defaults, label, name, count, 2, group_sizes, group_names, 1,
     3, pad_bits, 1, pad_labels, pad_text}
};
const opxy::u32 sdk_drum_machine_count = 6;
const opxy::platform::SynthControls sdk_synth_controls[] = {
    {reinterpret_cast<const char*>(34), ready, {4,5,6,7,0,1,2,3}, 0, {}, format, false, true},
    {reinterpret_cast<const char*>(37), ready, {4,5,6,7,0,1,2,3}, 0, {}, format, false, true},
    {reinterpret_cast<const char*>(38), ready, {4,5,6,7,0,1,2,3}, 0, {}, format, false, true},
    {reinterpret_cast<const char*>(39), ready, {4,5,6,7,0,1,2,3}, 0, {}, format, false, true},
    {reinterpret_cast<const char*>(40), ready, {4,5,6,7,0,1,2,3}, 0, {}, format, false, true},
    {reinterpret_cast<const char*>(41), ready, {4,5,6,7,0,1,2,3}, 0, {}, format, false, true}
};
const opxy::u32 sdk_synth_control_count = 6;
}

opxy::platform::Address opxy::platform::host_stock_address(Address address) {
    switch (address) {
    case kSelectedTrack: return reinterpret_cast<Address>(&track);
    case 0x080238bc: return reinterpret_cast<Address>(&page);
    case 0x8d58781c: return reinterpret_cast<Address>(&get_engine);
    case 0x8d4d6c98: return reinterpret_cast<Address>(&get_key);
    case 0x8d4d6f14: return reinterpret_cast<Address>(&get_track);
    case 0x8d51f184: return reinterpret_cast<Address>(&get_shift);
    case 0x8c0944b0: return reinterpret_cast<Address>(&top);
    case 0x8d51f1fc: return reinterpret_cast<Address>(&press);
    case 0x8d58926c: return reinterpret_cast<Address>(&region);
    case 0x8d589210: return reinterpret_cast<Address>(&canonical);
    case 0x8d585ee8: return reinterpret_cast<Address>(&changed);
    case 0x8d4d5f18: return reinterpret_cast<Address>(&publish);
    case 0x8d4d5f60: return reinterpret_cast<Address>(&stock_edit);
    case 0x8d5ad5bc: return reinterpret_cast<Address>(&turn);
    case 0x8d506c4c: return reinterpret_cast<Address>(&title);
    case 0x8d6011f0: return reinterpret_cast<Address>(&text);
    default: std::abort();
    }
}

int main() {
    using namespace opxy;
    using namespace opxy::platform;
    for (u32 pad = 0; pad < 24; ++pad) {
        regions[pad][0] = 123456;
        regions[pad][5] = 987654;
        reinterpret_cast<u8*>(regions[pad])[26] = static_cast<u8>(53 + pad);
    }
    u32 presses = 0;
    shown(0,1,2,3);
    sdk_synth_press(1); ++presses; shown(4,5,6,7);   // a press pages the whole row
    const u32 event[3] = {1000, 3, 2};
    sdk_drum_edit(53, 1, event, 0);   // the second encoder's second row, slot 5
    u8 model = 99, raw[8];
    const u8* record = reinterpret_cast<const u8*>(regions[0]);
    require(drum_marked(record[24]) && drum_unpack(regions[0] + 1, &model, raw));
    require(model == 0 && raw[5] == 28 && raw[1] == 21 && raw[0] == 20 && raw[7] == 53);
    require(publications == 1 && dirty == 1 && last_key == 53);
    shifted = true;
    // Shift and the second encoder choose the choke group: none, then 1..3.
    // The edit above kept stock's play mode 0, gate, which reads as none.
    require(drum_choke(record[24]) == 0 && !std::strcmp(sdk_drum_choke(0), "NONE"));
    sdk_drum_edit(53, 5, event, 0);
    require(publications == 2 && (record[24] >> 4) == 5 && !std::strcmp(sdk_drum_choke(0), "3"));
    require(sdk_drum_choke_choice(0) == 3);
    sdk_drum_edit(53, 5, event, 0);   // mute3 is the limit
    require(publications == 2);
    const u32 none[3] = {0, static_cast<u32>(-5), 0};
    sdk_drum_edit(53, 5, none, 0);
    require(publications == 3 && drum_marked(record[24]) && (record[24] >> 4) == 1);
    require(!std::strcmp(sdk_drum_choke(0), "NONE"));
    // Shift and the third and fourth encoders turn nothing.
    sdk_drum_edit(53, 6, event, 0);
    sdk_drum_edit(53, 7, event, 0);
    require(publications == 3);
    require(drum_unpack(regions[0] + 1, &model, raw) && raw[5] == 28 && raw[1] == 21 && model == 0);
    require(regions[0][0] == 123456 && regions[0][5] == 987654 && record[26] == 53);
    sdk_synth_turn(0, 0, 5, 0);
    require(turned_slot == 5);
    // A gap: the third encoder's slot 2 is blank and inert; the second row
    // pages in whole, its gap (slot 7) included.
    shifted = false;
    sdk_synth_press(3); ++presses; shown(0,1,2,3);
    sdk_drum_edit(53, 2, event, 0);
    require(publications == 3 && drum_unpack(regions[0] + 1, &model, raw) && raw[2] == 22);
    sdk_synth_press(2); ++presses; shown(4,5,6,7);
    sdk_drum_edit(53, 3, event, 0);   // slot 7, the gap: inert
    require(publications == 3);
    sdk_synth_press(0); ++presses; shown(0,1,2,3);

    // Shift and the first encoder choose the model, as on Drumline; the
    // controls stay.
    const u32 one[3] = {0, 1, 0};
    sdk_drum_edit(53, 4, one, 0);
    require(sdk_drum_values(0, &model, raw) && model == 1 && raw[5] == 28 && publications == 4);
    shown(0,1,2,3);
    sdk_drum_edit(53, 4, event, 0);   // the last model is the limit
    require(publications == 4);
    shifted = false;

    key = 54; shown(0,1,2,3);
    sdk_synth_press(1); ++presses; shown(0,1,2,3);   // the shorter model has no second row: a press stays
    sdk_drum_edit(54, 3, event, 0);   // nor a slot 3: inert
    require(publications == 4);
    require(!std::strcmp(sdk_drum_title(34, 1), "short"));
    key = 53;
    const u32 back[3] = {0, static_cast<u32>(-1), 0};
    shifted = true;
    sdk_drum_edit(53, 4, back, 0);
    shifted = false;
    require(sdk_drum_values(0, &model, raw) && model == 0 && publications == 5);
    sdk_synth_press(1); ++presses; shown(4,5,6,7);
    key = 54; shown(0,1,2,3);   // a key without a second row returns to the first
    key = 53;
    sdk_synth_press(1); ++presses; shown(4,5,6,7);
    sdk_synth_press(1); ++presses; shown(0,1,2,3);

    const u32 negative[3] = {0, static_cast<u32>(-100000), 0};
    sdk_drum_edit(53, 0, negative, 0);   // the first encoder edits slot 0 now
    require(sdk_drum_values(0, &model, raw) && raw[0] == 0 && model == 0);
    const u32 zero[3] = {};
    sdk_drum_edit(53, 1, zero, 0);
    require(publications == 6 && dirty == 6);

    // Parent note 53 can select canonical pad 65 in the other octave. GUI
    // defaults use the selected record's key, matching the audio mapper.
    shifted_octave = true;
    u8 expected_model, expected[8];
    defaults(65, &expected_model, expected);
    require(sdk_drum_values(0, &model, raw) && model == expected_model && !std::memcmp(raw, expected, 8));
    // A missing record key uses the same stock mapping as playback.
    reinterpret_cast<u8*>(regions[12])[26] = 0;
    require(sdk_drum_values(0, &model, raw) && !std::memcmp(raw, expected, 8));
    // A pad stored by the earlier eight-value layout (tag 10, unmarked) and a
    // marked pad with a model the kit lacks both keep the key's defaults.
    regions[12][1] = 0x2a000000; regions[12][2] = 0x15550000; regions[12][3] = 0x0aaa0000; regions[12][4] = 0x50550000;
    reinterpret_cast<u8*>(regions[12])[24] = 0x22;
    require(sdk_drum_values(0, &model, raw) && model == expected_model && !std::memcmp(raw, expected, 8));
    const u8 seven[8] = {7, 7, 7, 7, 7, 7, 7, 7};
    require(drum_pack(9, seven, regions[12] + 1));
    reinterpret_cast<u8*>(regions[12])[24] = drum_mark(0x22);
    require(sdk_drum_values(0, &model, raw) && model == expected_model && !std::memcmp(raw, expected, 8));
    require(drum_pack(1, seven, regions[12] + 1));
    require(sdk_drum_values(0, &model, raw) && model == 1 && !std::memcmp(raw, seven, 8));
    require((reinterpret_cast<u8*>(regions[12])[24] >> 4) == 2);   // stock's choke group kept
    require(sdk_drum_selected_pad(0) == 65);   // the screen's pad is the audio's
    require(sdk_drum_pad(0, 53) == 65 && sdk_drum_pad(0, 54) == 54);   // a played note's, the same way
    require(!sdk_drum_pad(0, 30) && !sdk_drum_pad(0, 128) && !sdk_drum_pad(8, 53));
    shifted_octave = false;
    require(sdk_drum_selected_pad(0) == 53);

    {
        // Model groups: Shift's first encoder chooses the group, keeping the
        // model's place in it; the second the model in the group; the third
        // the choke group; the fourth nothing.
        engine = 37; key = 54; shifted = true;
        const u32 up[3] = {0, 1, 0}, many[3] = {0, 3, 0}, back5[3] = {0, static_cast<u32>(-5), 0};
        u8 m = 99, v[8];
        require(sdk_drum_values(0, &m, v) && m == 1);   // group ONE, its second
        sdk_drum_edit(54, 4, up, 0);
        require(sdk_drum_values(0, &m, v) && m == 3);   // group TWO, its second
        const u32 before = publications;
        sdk_drum_edit(54, 4, up, 0);   // TWO is the last group
        require(publications == before && sdk_drum_values(0, &m, v) && m == 3);
        sdk_drum_edit(54, 5, many, 0);
        require(sdk_drum_values(0, &m, v) && m == 4);   // the group's last model is the limit
        sdk_drum_edit(54, 5, back5, 0);
        require(sdk_drum_values(0, &m, v) && m == 2);   // and its first
        sdk_drum_edit(54, 4, back5, 0);
        require(sdk_drum_values(0, &m, v) && m == 0);   // ONE, its first
        const u8* pad = reinterpret_cast<const u8*>(regions[1]);
        require(drum_choke(pad[24]) == 0);
        sdk_drum_edit(54, 6, up, 0);
        require(drum_choke(pad[24]) == 1 && !std::strcmp(sdk_drum_choke(0), "1"));
        const u32 still = publications;
        sdk_drum_edit(54, 7, up, 0);
        require(publications == still);

        // A paged kit: a press moves the whole row, slot 7 too though this
        // model leaves it unnamed. A 33-model kit keeps every control at 7
        // bits: a detent is one unit, to 127, and slot 7 keeps its value.
        engine = 38; key = 55; shifted = false;
        shown(0,1,2,3);
        sdk_synth_press(2); ++presses; shown(4,5,6,7);
        sdk_drum_edit(55, 2, up, 0);
        require(sdk_drum_values(0, &m, v) && m == 0 && v[6] == 27 && v[7] == 55 && v[5] == 25);
        const u32 far[3] = {0, 100, 0};
        sdk_drum_edit(55, 2, far, 0);
        require(sdk_drum_values(0, &m, v) && v[6] == 127);
        sdk_drum_edit(55, 2, back5, 0);
        require(sdk_drum_values(0, &m, v) && v[6] == 122);
        sdk_synth_press(0); ++presses; shown(0,1,2,3);
        engine = 34; key = 53;
    }

    {
        // padMix with groups: Shift's first row is the group, the model, the
        // key's stock pan and its level; the second row's first encoder the
        // choke group, the rest nothing. Pan moves as stock's editor does,
        // (event[2] + 1) * event[1] a turn, within -100..100; level one dB a
        // detent, a cut within -30..0. Neither touches the stored words or mark.
        engine = 39; key = 56; shifted = true;
        u8* pad = reinterpret_cast<u8*>(regions[3]);
        const DrumMachine* kit = sdk_drum_machine(39);
        require(kit && kit->second_shift_row() && !sdk_drum_machine(37)->second_shift_row());
        require(kit->shift(0, false) == DrumMachine::kShiftGroup && kit->shift(1, false) == DrumMachine::kShiftModel &&
                kit->shift(2, false) == DrumMachine::kShiftPan && kit->shift(3, false) == DrumMachine::kShiftLevel);
        require(kit->shift(0, true) == DrumMachine::kShiftChoke && kit->shift(1, true) == DrumMachine::kShiftNone &&
                kit->shift(2, true) == DrumMachine::kShiftNone && kit->shift(3, true) == DrumMachine::kShiftNone);
        const DrumMachine* plain = sdk_drum_machine(40);
        for (u32 second = 0; second < 2; ++second)
            require(plain->shift(0, second) == DrumMachine::kShiftModel && plain->shift(1, second) == DrumMachine::kShiftChoke &&
                    plain->shift(2, second) == DrumMachine::kShiftPan && plain->shift(3, second) == DrumMachine::kShiftLevel);
        // Without padMix nothing moves: the third choke (groups) or nothing, the fourth nothing.
        require(sdk_drum_machine(37)->shift(2, true) == DrumMachine::kShiftChoke && sdk_drum_machine(34)->shift(2, false) == DrumMachine::kShiftNone);
        i32 words[5];
        std::memcpy(words, regions[3], sizeof(words));
        const u8 mark = pad[24];
        require(sdk_drum_pan(0) == 0 && sdk_drum_level(0) == 0);
        u32 before = publications;
        const u32 fast[3] = {0, 3, 2}, slow[3] = {0, 1, 0}, down[3] = {0, static_cast<u32>(-1), 0};
        sdk_drum_edit(56, 6, fast, 0);
        require(static_cast<signed char>(pad[0x1e]) == 9 && sdk_drum_pan(0) == 9 && publications == before + 1);
        require(!std::memcmp(words, regions[3], sizeof(words)) && pad[24] == mark);
        const u32 left[3] = {0, static_cast<u32>(-100), 3};
        sdk_drum_edit(56, 6, left, 0);
        require(sdk_drum_pan(0) == -100 && publications == before + 2);
        sdk_drum_edit(56, 6, left, 0);   // the left end is the limit
        require(publications == before + 2);
        const u32 far[3] = {0, 127, 99};
        sdk_drum_edit(56, 6, far, 0);
        require(sdk_drum_pan(0) == 100);
        const u32 back[3] = {0, static_cast<u32>(-100), 0};
        sdk_drum_edit(56, 6, back, 0);
        require(sdk_drum_pan(0) == 0 && pad[0x1e] == 0);   // 0 is the centre
        before = publications;
        sdk_drum_edit(56, 7, slow, 0);   // 0 dB is the top: no boost
        require(publications == before && pad[0x1d] == 0);
        sdk_drum_edit(56, 7, down, 0);
        require(static_cast<signed char>(pad[0x1d]) == -1 && sdk_drum_level(0) == -1 && publications == before + 1);
        const u32 many[3] = {0, static_cast<u32>(-50), 7};   // one dB a detent, whatever the speed
        sdk_drum_edit(56, 7, many, 0);
        require(sdk_drum_level(0) == -30 && static_cast<signed char>(pad[0x1d]) == -30);
        // A boost stock stored (up to +20 dB) shows and plays as 0 dB; turning
        // up keeps it, turning down cuts from 0 dB.
        pad[0x1d] = 12;
        require(sdk_drum_level(0) == 0);
        before = publications;
        sdk_drum_edit(56, 7, slow, 0);
        require(pad[0x1d] == 12 && publications == before);
        sdk_drum_edit(56, 7, down, 0);
        require(static_cast<signed char>(pad[0x1d]) == -1);
        require(!std::memcmp(words, regions[3], sizeof(words)) && pad[24] == mark);
        // The first row's choke encoder is now pan: the choke group stays.
        require(drum_choke(pad[24]) == 0);
        // The second row: every key has one, a short model's too (key 54).
        shifted = false;
        key = 54;
        sdk_synth_press(0); ++presses; shown(4,5,6,7);
        shifted = true;
        u8* other = reinterpret_cast<u8*>(regions[1]);
        const u32 up[3] = {0, 1, 0};
        before = publications;
        const u32 group = drum_choke(other[24]);   // 1, from engine 37's edit above
        sdk_drum_edit(54, 4, up, 0);
        require(group == 1 && drum_choke(other[24]) == 2 && publications == before + 1 && !std::strcmp(sdk_drum_choke(0), "2"));
        for (u32 c = 5; c < 8; ++c) sdk_drum_edit(54, c, up, 0);   // the second row's free encoders turn nothing
        require(publications == before + 1 && other[0x1e] == 0 && other[0x1d] == 0);
        shifted = false;
        sdk_synth_press(0); ++presses; shown(0,1,2,3);
        // padMix without groups: the model and the choke group, then pan and level.
        engine = 40; key = 53; shifted = true;
        u8* first = reinterpret_cast<u8*>(regions[0]);
        const u32 choke_before = drum_choke(first[24]);
        sdk_drum_edit(53, 6, up, 0);
        sdk_drum_edit(53, 7, down, 0);
        require(sdk_drum_pan(0) == 1 && sdk_drum_level(0) == -1 && drum_choke(first[24]) == choke_before);
        sdk_drum_edit(53, 5, up, 0);
        require(drum_choke(first[24]) == choke_before + 1);
        first[0x1e] = first[0x1d] = 0;
        engine = 34; key = 53; shifted = false;
        require(sdk_drum_pan(8) == 0 && sdk_drum_level(8) == 0);   // no instrument track
    }

    {
        // Pad controls: the second Shift row's encoders 2..4, packed from bit 16
        // of the pad word +0x14 (FILTER 16..20, RES 21..23, SRR 24..27). An
        // unmarked pad reads neutral whatever stock left there, and its first
        // edit clears the fields; bits 28..30 and stock's low half stay.
        engine = 41; key = 57; shifted = true;
        const DrumMachine* kit = sdk_drum_machine(41);
        require(kit->second_shift_row());
        require(kit->shift(0, true) == DrumMachine::kShiftChoke && kit->shift(1, true) == DrumMachine::kShiftPad &&
                kit->shift(2, true) == DrumMachine::kShiftPad + 1 && kit->shift(3, true) == DrumMachine::kShiftPad + 2);
        require(kit->shift(2, false) == DrumMachine::kShiftPan && kit->shift(3, false) == DrumMachine::kShiftLevel);
        i32* word = regions[4] + 5;
        u8* pad = reinterpret_cast<u8*>(regions[4]);
        require(!drum_marked(pad[24]));
        *word = 0x7a5a1234;   // a value stock left: bit 31 clear, its fields nonzero
        i32 values[3] = {9, 9, 9};
        require(sdk_drum_pad_values(0, values) == 3 && !values[0] && !values[1] && !values[2]);
        shifted = false;
        sdk_synth_press(0); ++presses; shown(4,5,6,7);
        shifted = true;
        const u32 left[3] = {0, static_cast<u32>(-3), 0}, up[3] = {0, 1, 0}, far[3] = {0, 100, 0}, back[3] = {0, static_cast<u32>(-100), 0};
        u32 before = publications;
        sdk_drum_edit(57, 5, left, 0);   // FILTER three steps low-pass
        require(drum_marked(pad[24]) && publications == before + 1);
        require((static_cast<u32>(*word) & kDrumPadMask) == ((32u - 3u) << 16) && (*word & 0xffff) == 0x1234);
        require(sdk_drum_pad_values(0, values) == 3 && values[0] == -3 && !values[1] && !values[2]);
        sdk_drum_edit(57, 5, back, 0);
        require(sdk_drum_pad_values(0, values) && values[0] == -16);
        sdk_drum_edit(57, 5, far, 0);
        require(sdk_drum_pad_values(0, values) && values[0] == 15);
        sdk_drum_edit(57, 6, far, 0);
        sdk_drum_edit(57, 7, far, 0);
        require(sdk_drum_pad_values(0, values) && values[0] == 15 && values[1] == 7 && values[2] == 15);
        before = publications;
        sdk_drum_edit(57, 7, up, 0);     // the last step is the limit
        require(publications == before);
        // Bits 28..30 are the SDK's: an edit keeps them.
        *word |= 5 << 28;
        sdk_drum_edit(57, 6, left, 0);
        require(sdk_drum_pad_values(0, values) && values[1] == 4 && ((static_cast<u32>(*word) >> 28) & 7) == 5);
        // A project keeps the whole record; a preset that stores the word's top
        // half (>> 16, restored << 16) keeps every field too.
        const i32 restored = static_cast<i32>((static_cast<u32>(*word) >> 16) << 16);
        for (u32 f = 0; f < 3; ++f)
            require(drum_pad_get(restored, f, pad_bits, 1) == drum_pad_get(*word, f, pad_bits, 1));
        // The controls never touch the stored words or the choke group.
        u8 m, v[8];
        require(sdk_drum_values(0, &m, v));
        // Another kit's edits leave the word alone.
        engine = 37; key = 57;
        const i32 kept = *word;
        sdk_drum_edit(57, 6, up, 0);
        require(*word == kept);   // another engine's track starts on its first row again
        shifted = false;
        engine = 34; key = 53;
    }

    ownership = 0;
    require(sdk_drum_edit(53, 0, one, 0) == 1234 && fallback == 1);
    require(!sdk_drum_values(0, &model, raw));
    require(!sdk_drum_selected_pad(0));
    shown(0,1,2,3);
    sdk_synth_press(0); ++presses; shown(0,1,2,3);
    require(clicks == presses);
    return 0;
}
