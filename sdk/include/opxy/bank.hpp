#pragma once
#include "types.hpp"

// Sine banks: many table oscillators summed into one block, each ramping its
// own amplitude. On the Blackfin these run hand-scheduled assembly in the
// SDK's platform layer (sdk/platform/blackfin/bank.s); elsewhere, and for
// table sizes the assembly does not cover, the portable reference below. Both
// give the same output, bit for bit, for inputs inside the documented range.
//
// Audio-thread safe: no allocation, I/O, locks or global state. The routines
// read the table and partials and write only acc and the partials.
namespace opxy { namespace bank {

// A partial of `sine`. Phase and increment are full-turn Q32. Amplitude is a
// Q31 level whose top half (amplitude >> 16, a signed 16-bit value) scales
// the table value; step is added to it after every sample. Both are updated
// in place: after a call, phase and amplitude are where the next block starts.
struct Partial {
    u32 phase;
    u32 increment;
    i32 amplitude;
    i32 step;
};

// A partial of `sine_glide`: as Partial, and its increment moves by glide
// after every sample (a pitch glide, linear in frequency).
struct Glide {
    u32 phase;
    u32 increment;
    i32 amplitude;
    i32 step;
    u32 glide;
};

// A partial of `quadrature`: two waves read at one phase from a table of
// pairs (the low half of each word is the first wave, usually sine; the high
// half the second, usually cosine), scaled by the top halves of a and b.
// a and b hold for `hold` samples, then step_a and step_b are added.
struct Pair {
    u32 phase;
    u32 increment;
    i32 a, b;
    i32 step_a, step_b;
};

// The top half of a word as a signed 16-bit value (x >> 16 on every compiler
// the SDK uses, written without implementation-defined behaviour).
inline i32 top(i32 x) {
    const u32 h = static_cast<u32>(x) >> 16;
    return static_cast<i32>(h) - static_cast<i32>((h & 0x8000u) << 1);
}
inline i32 low(u32 w) {
    const u32 h = w & 0xffffu;
    return static_cast<i32>(h) - static_cast<i32>((h & 0x8000u) << 1);
}

namespace reference {
// For n in [0, frames), for each partial in order:
//   acc[n] += table[phase >> (32 - bits)] * (amplitude >> 16)
//   phase += increment; amplitude += step
// All sums wrap modulo 2^32. Range: table values in [-32767, 32767]; any
// amplitude. bits in [1, 16]; the table holds 2^bits entries.
inline void sine(i32* acc, u32 frames, const i16* table, u32 bits, Partial* partials, u32 count) {
    const u32 shift = 32 - bits;
    for (u32 k = 0; k < count; ++k) {
        Partial& p = partials[k];
        u32 phase = p.phase, amplitude = static_cast<u32>(p.amplitude);
        const u32 step = static_cast<u32>(p.step);
        for (u32 n = 0; n < frames; ++n) {
            const i32 product = table[phase >> shift] * top(static_cast<i32>(amplitude));
            acc[n] = static_cast<i32>(static_cast<u32>(acc[n]) + static_cast<u32>(product));
            phase += p.increment;
            amplitude += step;
        }
        p.phase = phase;
        p.amplitude = static_cast<i32>(amplitude);
    }
}

// As sine, but each product is shifted right by scale (arithmetically, so
// rounded toward minus infinity) before it is added, and after every sample
// increment += glide (modulo 2^32). scale in [0, 15].
inline i32 shifted(i32 x, u32 scale) {
    // x >> scale for negative x too, without implementation-defined shifts.
    return x >= 0 ? x >> scale : -static_cast<i32>((static_cast<u32>(-(x + 1)) >> scale) + 1);
}
inline void sine_glide(i32* acc, u32 frames, const i16* table, u32 bits, Glide* partials, u32 count, u32 scale = 0) {
    const u32 shift = 32 - bits;
    for (u32 k = 0; k < count; ++k) {
        Glide& p = partials[k];
        u32 phase = p.phase, increment = p.increment, amplitude = static_cast<u32>(p.amplitude);
        const u32 step = static_cast<u32>(p.step);
        for (u32 n = 0; n < frames; ++n) {
            const i32 product = shifted(table[phase >> shift] * top(static_cast<i32>(amplitude)), scale);
            acc[n] = static_cast<i32>(static_cast<u32>(acc[n]) + static_cast<u32>(product));
            phase += increment;
            increment += p.glide;
            amplitude += step;
        }
        p.phase = phase;
        p.increment = increment;
        p.amplitude = static_cast<i32>(amplitude);
    }
}

// For n in [0, frames), for each partial in order, w = pairs[phase >> (32 - bits)]:
//   acc[n] += low(w) * (a >> 16) + high(w) * (b >> 16)
//   phase += increment; and after every hold samples, a += step_a, b += step_b
// frames is a multiple of hold (hold >= 1). Range: both halves of every word
// in [-32767, 32767]; |a >> 16| + |b >> 16| <= 32767 for every partial and
// sample. Sums wrap modulo 2^32.
inline void quadrature(i32* acc, u32 frames, const u32* pairs, u32 bits, Pair* partials, u32 count, u32 hold = 1) {
    const u32 shift = 32 - bits;
    for (u32 k = 0; k < count; ++k) {
        Pair& p = partials[k];
        u32 phase = p.phase, a = static_cast<u32>(p.a), b = static_cast<u32>(p.b);
        for (u32 n = 0; n < frames; n += hold) {
            const i32 ah = top(static_cast<i32>(a)), bh = top(static_cast<i32>(b));
            for (u32 j = 0; j < hold; ++j) {
                const u32 w = pairs[phase >> shift];
                const i32 sum = low(w) * ah + top(static_cast<i32>(w)) * bh;
                acc[n + j] = static_cast<i32>(static_cast<u32>(acc[n + j]) + static_cast<u32>(sum));
                phase += p.increment;
            }
            a += static_cast<u32>(p.step_a);
            b += static_cast<u32>(p.step_b);
        }
        p.phase = phase;
        p.a = static_cast<i32>(a);
        p.b = static_cast<i32>(b);
    }
}
}  // namespace reference

#if defined(__bfin__)
extern "C" void opxy_bank_sine(i32* acc, u32 frames, const i16* table, u32 shift, Partial* partials, u32 count);
extern "C" void opxy_bank_sine_glide(i32* acc, u32 frames, const i16* table, u32 shift, Glide* partials, u32 count,
                                     u32 scale);
extern "C" void opxy_bank_quadrature(i32* acc, u32 frames, const u32* pairs, u32 shift, Pair* partials, u32 count);
extern "C" void opxy_bank_quadrature_held(i32* acc, u32 chunks, const u32* pairs, u32 shift, Pair* partials, u32 count,
                                          u32 hold);
inline void sine(i32* acc, u32 frames, const i16* table, u32 bits, Partial* partials, u32 count) {
    if (frames && count) opxy_bank_sine(acc, frames, table, 32 - bits, partials, count);
}
inline void sine_glide(i32* acc, u32 frames, const i16* table, u32 bits, Glide* partials, u32 count, u32 scale = 0) {
    if (frames && count) opxy_bank_sine_glide(acc, frames, table, 32 - bits, partials, count, scale);
}
inline void quadrature(i32* acc, u32 frames, const u32* pairs, u32 bits, Pair* partials, u32 count, u32 hold = 1) {
    // The assembly covers tables of 2^10, 2^11 and 2^12 pairs.
    if (bits < 10 || bits > 12) return reference::quadrature(acc, frames, pairs, bits, partials, count, hold);
    if (!frames || !count) return;
    if (hold == 1) opxy_bank_quadrature(acc, frames, pairs, 32 - bits, partials, count);
    else opxy_bank_quadrature_held(acc, frames / hold, pairs, 32 - bits, partials, count, hold);
}
#else
inline void sine(i32* acc, u32 frames, const i16* table, u32 bits, Partial* partials, u32 count) {
    reference::sine(acc, frames, table, bits, partials, count);
}
inline void sine_glide(i32* acc, u32 frames, const i16* table, u32 bits, Glide* partials, u32 count, u32 scale = 0) {
    reference::sine_glide(acc, frames, table, bits, partials, count, scale);
}
inline void quadrature(i32* acc, u32 frames, const u32* pairs, u32 bits, Pair* partials, u32 count, u32 hold = 1) {
    reference::quadrature(acc, frames, pairs, bits, partials, count, hold);
}
#endif

}}  // namespace opxy::bank
