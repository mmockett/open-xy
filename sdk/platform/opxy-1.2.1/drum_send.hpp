// Per-key sends for synthesis drum kits (resources.padSend, SDK 1.10). Stock
// mixes a track's voices into one stereo sum and scales that sum into the two
// send buses, so on its own every key of a kit shares the track's sends. With
// padSend, the hooks (drum_send.cpp, drum_send_hooks.s) let each key pass its
// own share of FX I and of FX II: a send bus then reads a second sum, of each
// voice scaled by its key's share of that send, while the track's own mix keeps
// stock's sum. A send to which every voice on the track follows the track keeps
// stock's path for that block.
#pragma once
#include <opxy/audio.hpp>
#include <opxy/q31.hpp>
#include "drum_storage.hpp"

namespace opxy { namespace platform {
// A key's send codes live in stock's pad word +0x14, which reaches the audio
// side as the note payload's word 6 (8d5ad738). From bit 16 the word holds the
// kit's FILTER (5 bits) and SRR (4), then FX I's code in bits 25..27 and FX
// II's in 28..30. Code 0 follows the track's send exactly, so a pad saved
// before padSend, or one the kit has not marked, plays as before.
const u32 kDrumSendPayload = 6, kDrumSendShift = 25, kDrumSendBits = 3, kDrumSendCodes = 8, kDrumSends = 2;
// Both codes of a payload, FX I's in bits 0..2 and FX II's in 3..5.
inline u32 drum_send_codes(const i32* payload) {
    return payload && drum_marked_q31(payload[0]) && payload[kDrumSendPayload] > 0
        ? (static_cast<u32>(payload[kDrumSendPayload]) >> kDrumSendShift) & ((1u << (kDrumSends * kDrumSendBits)) - 1) : 0;
}
// One send's code (0 FX I, 1 FX II) of what drum_send_codes gives.
inline u32 drum_send_code(u32 codes, u32 send) { return (codes >> (send * kDrumSendBits)) & (kDrumSendCodes - 1); }
inline i32 drum_send_store(i32 word, u32 send, u32 code) {
    const u32 low = kDrumSendShift + send * kDrumSendBits, mask = (kDrumSendCodes - 1) << low;
    return static_cast<i32>((static_cast<u32>(word) & ~mask) | ((code & (kDrumSendCodes - 1)) << low));
}
// The share of the track's send each code passes, in Q31: code c of 1..7
// passes (c - 1) / 7 (0, 14, 29, 43, 57, 71 and 86 percent); 0 passes all of it.
const i32 kDrumSendGains[kDrumSendCodes] = {
    q31::maximum, 0, 306783378, 613566757, 920350135, 1227133513, 1533916891, 1840700270};
inline i32 drum_send_gain(u32 code) { return kDrumSendGains[code & (kDrumSendCodes - 1)]; }

// Stock's track sum and a voice's output: 64 interleaved left/right Q31 frames.
const u32 kDrumSendWords = 2 * block_frames;

// What a send's hook reads, in this order (drum_send_hooks.s): `track` is the
// stock track index plus one while that track's send takes `source`, else 0.
struct DrumSendBus {
    u32 track;
    i32* source;
};
// FX I's, then FX II's.
struct DrumSendState {
    DrumSendBus bus[kDrumSends];
};
// Each track's mix starts with no voices of its own.
inline void drum_send_begin(DrumSendState& state) {
    for (u32 s = 0; s < kDrumSends; ++s) {
        state.bus[s].track = 0;
        state.bus[s].source = nullptr;
    }
}
// Counted, pointer-stepping loops, kept out of line so the compiler sees
// plain pointers (not stock's constant addresses) and makes each a hardware
// loop. File-local in every unit, so no unit exports them.
static inline __attribute__((noinline)) void drum_send_copy(i32* __restrict to, const i32* __restrict from) {
    for (u32 n = kDrumSendWords; n; --n) *to++ = *from++;
}
static inline __attribute__((noinline)) void drum_send_add(i32* __restrict to, const i32* __restrict from) {
    for (u32 n = kDrumSendWords; n; --n, ++to) *to = q31::add(*to, *from++);
}
static inline __attribute__((noinline)) void drum_send_scale(i32* __restrict to, const i32* __restrict from, i32 gain) {
    for (u32 n = kDrumSendWords; n; --n, ++to) *to = q31::add(*to, q31::multiply(gain, *from++));
}
// Before stock adds a voice into the track's sum, for one send. While every
// voice so far has followed the track, nothing is kept and stock's sum feeds
// the send. The first voice with a share of its own starts `scratch` as stock's
// sum so far (with whatever stock started the sum with), and from then on each
// voice of the track goes into it scaled by its share, a follower unscaled,
// with stock's saturating add in stock's order, so a follower lands in both
// sums alike.
inline void drum_send_bus(DrumSendBus& bus, i32* scratch, u32 track, const i32* sum, const i32* voice, u32 code) {
    if (!bus.track) {
        if (!code) return;
        drum_send_copy(scratch, sum);
        bus.track = track + 1;
        bus.source = scratch;
    }
    if (!code) return drum_send_add(scratch, voice);
    const i32 gain = drum_send_gain(code);
    if (gain) drum_send_scale(scratch, voice, gain);
}
// Both sends, each into its own scratch sum.
inline void drum_send_voice(DrumSendState& state, i32 (*scratch)[kDrumSendWords], u32 track, const i32* sum,
                            const i32* voice, u32 codes) {
    for (u32 s = 0; s < kDrumSends; ++s)
        drum_send_bus(state.bus[s], scratch[s], track, sum, voice, drum_send_code(codes, s));
}
// What send `s`'s bus reads for `track`: the kept sum while it is that track's, else stock's.
inline const i32* drum_send_source(const DrumSendState& state, u32 s, u32 track, const i32* sum) {
    return state.bus[s].track == track + 1 ? state.bus[s].source : sum;
}

// A kit declaring padSend, for the voice hook to recognize its voices: its
// method table, and both send codes of the block one of its handles just rendered.
struct DrumSendKit {
    const void* vtable;
    u32 (*codes)(const void* handle);
};
}}

extern "C" {
// Generated per collection (drum_callbacks.cpp); a kit not in a browser
// selection names kit_absent, which no voice's method table is.
extern const opxy::platform::DrumSendKit sdk_drum_send_kits[];
extern const opxy::u32 sdk_drum_send_kit_count;
}
