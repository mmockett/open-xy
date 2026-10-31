// The engines' sine loops as they stand (OP-XY-Hexagon / OP-XY-Nonagon main,
// read 2026-10-31), each driven over `count` partials the way its render()
// calls it. Copied verbatim apart from names; tables are passed in.
#pragma once
#include <opxy/types.hpp>
using opxy::i32; using opxy::u32; using opxy::i16;
#ifndef FRAMES_MAX
#define FRAMES_MAX 64
#endif
struct EP { u32 phase, inc; i32 a, b, da, db, h, dh; u32 glide; };

namespace bloom_orig {
template <u32 frames> inline u32 sound(const i16* sine_table, i32* mix, u32 phase, u32 increment, i32 amplitude, i32 step) {
    for (i32* out = mix; out != mix + frames; ++out) {
        *out += sine_table[phase >> 20] * (amplitude >> 16);
        phase += increment;
        amplitude += step;
    }
    return phase;
}
}
namespace bloom_on {  // /tmp/audit-hexagon/proto/bloom_on
inline opxy::i16 top_half(i32 amplitude) { return static_cast<opxy::i16>(static_cast<u32>(amplitude) >> 16); }
template <u32 frames> inline u32 sound(const i16* sine_table, i32* mix, u32 phase, u32 increment, i32 amplitude, i32 step) {
    for (i32* out = mix; out != mix + frames; ++out) {
        *out += sine_table[phase >> 20] * top_half(amplitude);
        phase += increment;
        amplitude += step;
    }
    return phase;
}
}
namespace hollow {
template <u32 frames> inline u32 sound(const i16* kSine, i32* acc, u32 phase, u32 increment, i32 a, i32 b, i32 da, i32 db) {
    for (i32* out = acc; out != acc + frames; ++out) {
        a += da;
        b += db;
        const opxy::i16* const at = kSine + (phase >> 22);
        *out += at[0] * (a >> 16) + at[256] * (b >> 16);
        phase += increment;
    }
    return phase;
}
}
namespace sand {
const u32 kStep = 8;
template <u32 frames> inline u32 sound(const i16* kSine, i32* acc, u32 phase, u32 increment, i32 a, i32 b, i32 da, i32 db) {
    a += da * (kStep / 2);
    b += db * (kStep / 2);
    for (i32* step = acc; step != acc + frames; step += kStep) {
        const i32 ah = a >> 16, bh = b >> 16;
        for (i32* out = step; out != step + kStep; ++out) {
            const opxy::i16* const at = kSine + (phase >> 22);
            *out += at[0] * ah + at[256] * bh;
            phase += increment;
        }
        a += da * kStep;
        b += db * kStep;
    }
    return phase;
}
}
namespace cocoa {
// sing() with split = block_frames (the untapped case) and the second loop empty.
template <u32 frames> inline void sing(const i16* sine, i32* sum, u32& phase, u32 inc, i32 glide, i32 amp, i32 slope, i32 after, u32 split) {
    u32 p = phase;
    i32 a = amp;
    for (u32 i = 0; i < split; ++i) {
        sum[i] += ((a >> 16) * sine[p >> 20]) >> 3;
        a += slope;
        p += inc;
        inc += static_cast<u32>(glide);
    }
    for (u32 i = split; i < frames; ++i) {
        sum[i] += ((a >> 16) * sine[p >> 20]) >> 3;
        a += after;
        p += inc;
        inc += static_cast<u32>(glide);
    }
    phase = p;
}
}
namespace butterfly {
// The bank's inner loop from render(): 32 half-rate samples, pre-incremented phase.
template <u32 half_frames> inline u32 bank(const i16* sine, i32* acc, u32 ph, u32 step_phase, i32 a, i32 slide) {
    i32 g = a;
    for (u32 i = 0; i < half_frames; ++i) {
        ph += step_phase;
        acc[i] += sine[ph >> (32 - 12)] * (g >> 10);
        g += slide;
    }
    return ph;
}
}

#define NOINLINE __attribute__((noinline))
template <u32 frames> NOINLINE void run_bloom_orig(const i16* t, i32* acc, EP* p, u32 n) {
    for (u32 k = 0; k < n; ++k) p[k].phase = bloom_orig::sound<frames>(t, acc, p[k].phase, p[k].inc, p[k].a, p[k].da);
}
template <u32 frames> NOINLINE void run_bloom_on(const i16* t, i32* acc, EP* p, u32 n) {
    for (u32 k = 0; k < n; ++k) p[k].phase = bloom_on::sound<frames>(t, acc, p[k].phase, p[k].inc, p[k].a, p[k].da);
}
template <u32 frames> NOINLINE void run_hollow(const i16* t, i32* acc, EP* p, u32 n) {
    for (u32 k = 0; k < n; ++k) p[k].phase = hollow::sound<frames>(t, acc, p[k].phase, p[k].inc, p[k].a, p[k].b, p[k].da, p[k].db);
}
template <u32 frames> NOINLINE void run_sand(const i16* t, i32* acc, EP* p, u32 n) {
    for (u32 k = 0; k < n; ++k) p[k].phase = sand::sound<frames>(t, acc, p[k].phase, p[k].inc, p[k].a, p[k].b, p[k].da, p[k].db);
}
template <u32 frames> NOINLINE void run_cocoa(const i16* t, i32* acc, EP* p, u32 n) {
    for (u32 k = 0; k < n; ++k) cocoa::sing<frames>(t, acc, p[k].phase, p[k].inc, static_cast<i32>(p[k].glide), p[k].a, p[k].da, 0, frames);
}
template <u32 frames> NOINLINE void run_cocoa_noglide(const i16* t, i32* acc, EP* p, u32 n) {
    for (u32 k = 0; k < n; ++k) cocoa::sing<frames>(t, acc, p[k].phase, p[k].inc, 0, p[k].a, p[k].da, 0, frames);
}
template <u32 frames> NOINLINE void run_butterfly(const i16* t, i32* acc, EP* p, u32 n) {
    for (u32 k = 0; k < n; ++k) p[k].phase = butterfly::bank<frames>(t, acc, p[k].phase, p[k].inc, p[k].a, p[k].da);
}

// GCC's best case: the same loops out of line (noinline, one call a partial),
// so each sample loop gets a hardware loop, as the Nonagon audit's patches do.
template <u32 frames> NOINLINE u32 ool_bloom_on(const i16* t, i32* acc, u32 ph, u32 inc, i32 a, i32 da) { return bloom_on::sound<frames>(t, acc, ph, inc, a, da); }
template <u32 frames> NOINLINE u32 ool_hollow(const i16* t, i32* acc, u32 ph, u32 inc, i32 a, i32 b, i32 da, i32 db) { return hollow::sound<frames>(t, acc, ph, inc, a, b, da, db); }
template <u32 frames> NOINLINE u32 ool_sand(const i16* t, i32* acc, u32 ph, u32 inc, i32 a, i32 b, i32 da, i32 db) { return sand::sound<frames>(t, acc, ph, inc, a, b, da, db); }
template <u32 frames> NOINLINE void ool_cocoa(const i16* t, i32* acc, u32& ph, u32 inc, i32 g, i32 a, i32 da) { cocoa::sing<frames>(t, acc, ph, inc, g, a, da, 0, frames); }
template <u32 frames> NOINLINE u32 ool_butterfly(const i16* t, i32* acc, u32 ph, u32 inc, i32 a, i32 da) { return butterfly::bank<frames>(t, acc, ph, inc, a, da); }
template <u32 frames> NOINLINE void run_bloom_on_ool(const i16* t, i32* acc, EP* p, u32 n) {
    for (u32 k = 0; k < n; ++k) p[k].phase = ool_bloom_on<frames>(t, acc, p[k].phase, p[k].inc, p[k].a, p[k].da);
}
template <u32 frames> NOINLINE void run_hollow_ool(const i16* t, i32* acc, EP* p, u32 n) {
    for (u32 k = 0; k < n; ++k) p[k].phase = ool_hollow<frames>(t, acc, p[k].phase, p[k].inc, p[k].a, p[k].b, p[k].da, p[k].db);
}
template <u32 frames> NOINLINE void run_sand_ool(const i16* t, i32* acc, EP* p, u32 n) {
    for (u32 k = 0; k < n; ++k) p[k].phase = ool_sand<frames>(t, acc, p[k].phase, p[k].inc, p[k].a, p[k].b, p[k].da, p[k].db);
}
template <u32 frames> NOINLINE void run_cocoa_ool(const i16* t, i32* acc, EP* p, u32 n) {
    for (u32 k = 0; k < n; ++k) ool_cocoa<frames>(t, acc, p[k].phase, p[k].inc, static_cast<i32>(p[k].glide), p[k].a, p[k].da);
}
template <u32 frames> NOINLINE void run_butterfly_ool(const i16* t, i32* acc, EP* p, u32 n) {
    for (u32 k = 0; k < n; ++k) p[k].phase = ool_butterfly<frames>(t, acc, p[k].phase, p[k].inc, p[k].a, p[k].da);
}
