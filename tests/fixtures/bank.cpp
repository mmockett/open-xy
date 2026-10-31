// Blackfin simulator bench: bit identity of the assembly against the portable
// reference, then cycles per partial-sample for the engines' GCC loops and
// for the assembly. Freestanding, bare metal (crt0.s, link.ld).
#include <opxy/bank.hpp>
#include "gcc_loops.hpp"
using namespace opxy;

#if !defined(WITH_QUADRATURE) && defined(__bfin__)
extern "C" void opxy_bank_quadrature(i32*, u32, const u32*, u32, bank::Pair*, u32) {}
extern "C" void opxy_bank_quadrature_held(i32*, u32, const u32*, u32, bank::Pair*, u32, u32) {}
#endif
#if defined(__bfin__)
static inline u32 cycles() { u32 c; __asm__ volatile("%0 = CYCLES;" : "=d"(c)); return c; }
extern "C" int sys_write(int fd, const void* buf, int len);
static void out(const char* s, int n) { sys_write(1, s, n); }
#else
#include <cstdio>
static inline u32 cycles() { return 0; }
static void out(const char* s, int n) { fwrite(s, 1, n, stdout); }
static void opxy_bank_quadrature_held(i32* acc, u32 frames, const u32* p, u32 shift, bank::Pair* q, u32 n, u32 hold) {
    bank::reference::quadrature(acc, frames * hold, p, 32 - shift, q, n, hold);
}
#endif
static void puts_(const char* s) { int n = 0; while (s[n]) ++n; out(s, n); }
static void putu(u32 v) { char b[12]; int n = 0; do { b[11 - n++] = '0' + v % 10; v /= 10; } while (v); out(b + 12 - n, n); }
static void puth(u32 v) { char b[8]; for (int i = 0; i < 8; ++i) { u32 d = (v >> (28 - 4 * i)) & 15; b[i] = d < 10 ? '0' + d : 'a' + d - 10; } out(b, 8); }

static u32 rng = 0x12345678u;
static u32 rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static i32 rnd16() { i32 v = static_cast<i32>(rnd() % 65535u) - 32767; return v; }  // [-32767, 32767]

static i16 table16[4096 + 256];
static u32 pairs[4096];
static i32 acc_a[64], acc_b[64];
static bank::Partial pa[40], pb[40];
static bank::Pair qa[40], qb[40];
static bank::Glide ga[40], gb[40];

static u32 fnv(const void* p, u32 n, u32 h) { const u8* b = static_cast<const u8*>(p); for (u32 i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u; return h; }
static bool same(const void* a, const void* b, u32 n) { const u8* x = static_cast<const u8*>(a); const u8* y = static_cast<const u8*>(b); for (u32 i = 0; i < n; ++i) if (x[i] != y[i]) return false; return true; }

// A random amplitude with |top| bounded by limit, and a step that keeps the
// ramp inside it over 64 samples (or wraps freely when limit is 0: any value).
static i32 amp(i32 limit) { if (!limit) return static_cast<i32>(rnd()); return static_cast<i32>(rnd() % (2u * limit << 16)) - (limit << 16); }

static u32 check_sine(u32 trials, u32& hash) {
    u32 bad = 0;
    for (u32 t = 0; t < trials; ++t) {
        const u32 frames = 1 + rnd() % 64, count = 1 + rnd() % 33, bits = 1 + rnd() % 12;
        for (u32 n = 0; n < 64; ++n) acc_a[n] = acc_b[n] = static_cast<i32>(rnd());
        for (u32 k = 0; k < count; ++k) {
            pa[k].phase = rnd(); pa[k].increment = rnd(); pa[k].amplitude = static_cast<i32>(rnd()); { const i32 r = static_cast<i32>(rnd()); pa[k].step = r >> (rnd() % 32); }
            pb[k] = pa[k];
        }
        bank::reference::sine(acc_a, frames, table16, bits, pa, count);
        bank::sine(acc_b, frames, table16, bits, pb, count);
        if (!same(acc_a, acc_b, sizeof acc_a) || !same(pa, pb, count * sizeof pa[0])) ++bad;
        hash = fnv(acc_a, sizeof acc_a, fnv(pa, count * sizeof pa[0], hash));
    }
    return bad;
}
static u32 check_glide(u32 trials, u32& hash) {
    u32 bad = 0;
    for (u32 t = 0; t < trials; ++t) {
        const u32 frames = 1 + rnd() % 64, count = 1 + rnd() % 17, bits = 1 + rnd() % 12;
        for (u32 n = 0; n < 64; ++n) acc_a[n] = acc_b[n] = static_cast<i32>(rnd());
        for (u32 k = 0; k < count; ++k) {
            ga[k].phase = rnd(); ga[k].increment = rnd(); ga[k].amplitude = static_cast<i32>(rnd());
            { const i32 r = static_cast<i32>(rnd()); ga[k].step = r >> (rnd() % 32); }
            { const u32 r = rnd(); ga[k].glide = r >> (rnd() % 32); }
            gb[k] = ga[k];
        }
        const u32 scale = rnd() % 16;
        bank::reference::sine_glide(acc_a, frames, table16, bits, ga, count, scale);
        bank::sine_glide(acc_b, frames, table16, bits, gb, count, scale);
        if (!same(acc_a, acc_b, sizeof acc_a) || !same(ga, gb, count * sizeof ga[0])) ++bad;
        hash = fnv(acc_a, sizeof acc_a, fnv(ga, count * sizeof ga[0], hash));
    }
    return bad;
}
static u32 check_quadrature(u32 trials, u32 hold_mode, u32& hash) {
    u32 bad = 0;
    for (u32 t = 0; t < trials; ++t) {
        const u32 bits = 10 + rnd() % 3;
        const u32 hold = hold_mode == 1 ? 1 : (hold_mode == 0 ? (1u << (rnd() % 7)) : hold_mode);
        const u32 frames = hold * (1 + rnd() % (64 / hold)), count = 1 + rnd() % 33;
        for (u32 n = 0; n < 64; ++n) acc_a[n] = acc_b[n] = static_cast<i32>(rnd());
        for (u32 k = 0; k < count; ++k) {
            // |a>>16| + |b>>16| <= 32767 along the whole ramp
            const i32 la = 1 + static_cast<i32>(rnd() % 16383), lb = 1 + static_cast<i32>(rnd() % 16383);
            const i32 a0 = amp(la), b0 = amp(lb), a1 = amp(la), b1 = amp(lb);
            const u32 steps = frames / hold;
            qa[k].phase = rnd(); qa[k].increment = rnd();
            qa[k].a = a0; qa[k].b = b0;
            qa[k].step_a = (a1 - a0) / static_cast<i32>(steps); qa[k].step_b = (b1 - b0) / static_cast<i32>(steps);
            qb[k] = qa[k];
        }
        bank::reference::quadrature(acc_a, frames, pairs, bits, qa, count, hold);
        bank::quadrature(acc_b, frames, pairs, bits, qb, count, hold);
        if (!same(acc_a, acc_b, sizeof acc_a) || !same(qa, qb, count * sizeof qa[0])) ++bad;
        hash = fnv(acc_a, sizeof acc_a, fnv(qa, count * sizeof qa[0], hash));
    }
    return bad;
}


// Boundary inputs: amplitudes and table values at the ends of their ranges,
// frames 1 and 64, hold 1 and hold = frames, the held routine called directly.
static const i32 kEdgeAmp[] = {0x7fffffff, static_cast<i32>(0x80000000u), 0x7fff0000, static_cast<i32>(0x80010000u), 0, -1, 0x0000ffff, 0x00010000};
static u32 check_edges(u32& hash) {
    u32 bad = 0;
    i16 saved[4];
    for (u32 i = 0; i < 4; ++i) saved[i] = table16[i];
    table16[0] = 32767; table16[1] = -32767; table16[2] = 32767; table16[3] = -32767;
    for (u32 t = 0; t < 400; ++t) {
        const u32 frames = (t & 1) ? 64 : 1 + (t % 3), count = 1 + t % 9, bits = 12 - t % 3;
        for (u32 n = 0; n < 64; ++n) acc_a[n] = acc_b[n] = (t & 2) ? 0x7fffffff - static_cast<i32>(n) : static_cast<i32>(0x80000000u + n);
        for (u32 k = 0; k < count; ++k) {
            // phases at the first table entries so the edge values are read
            pa[k].phase = (rnd() & 3) << (32 - bits); pa[k].increment = (t & 4) ? (1u << (32 - bits)) : rnd();
            pa[k].amplitude = kEdgeAmp[rnd() % 8]; pa[k].step = (t & 8) ? 0 : static_cast<i32>(rnd());
            pb[k] = pa[k];
        }
        bank::reference::sine(acc_a, frames, table16, bits, pa, count);
        bank::sine(acc_b, frames, table16, bits, pb, count);
        if (!same(acc_a, acc_b, sizeof acc_a) || !same(pa, pb, count * sizeof pa[0])) ++bad;
        hash = fnv(acc_a, sizeof acc_a, fnv(pa, count * sizeof pa[0], hash));
    }
    for (u32 i = 0; i < 4; ++i) table16[i] = saved[i];
    u32 saved_pairs[4];
    for (u32 i = 0; i < 4; ++i) saved_pairs[i] = pairs[i];
    pairs[0] = 0x7fff7fffu; pairs[1] = 0x80018001u; pairs[2] = 0x7fff8001u; pairs[3] = 0x80017fffu;
    for (u32 t = 0; t < 600; ++t) {
        const u32 bits = 10 + t % 3, hold = (t % 5 == 0) ? 64 : (t % 5 == 1 ? 1 : 1u << (t % 7));
        const u32 frames = hold * (1 + (t & 1 ? 0 : (64 / hold - 1))), count = 1 + t % 7;
        for (u32 n = 0; n < 64; ++n) acc_a[n] = acc_b[n] = (t & 2) ? 0x7fffffff - static_cast<i32>(n) : static_cast<i32>(0x80000000u + n);
        for (u32 k = 0; k < count; ++k) {
            // |a >> 16| + |b >> 16| = 32767 at the extremes, held (steps 0) so the ramp stays there
            const i32 split = static_cast<i32>(rnd() % 32768u);
            const i32 sa = (rnd() & 1) ? 1 : -1, sb = (rnd() & 1) ? 1 : -1;
            qa[k].phase = (rnd() & 3) << (32 - bits); qa[k].increment = (t & 4) ? (1u << (32 - bits)) : rnd();
            qa[k].a = sa * split * 65536; qa[k].b = sb * (32767 - split) * 65536;
            qa[k].step_a = qa[k].step_b = 0;
            if (t & 8) { qa[k].a = qa[k].a | 0xffff; qa[k].step_a = 1; }  // carries into the top half mid-block
            qb[k] = qa[k];
        }
        bank::reference::quadrature(acc_a, frames, pairs, bits, qa, count, hold);
        if (t % 5 == 1 && (t & 16)) opxy_bank_quadrature_held(acc_b, frames, pairs, 32 - bits, qb, count, 1);  // held path at hold 1
        else bank::quadrature(acc_b, frames, pairs, bits, qb, count, hold);
        if (!same(acc_a, acc_b, sizeof acc_a) || !same(qa, qb, count * sizeof qa[0])) ++bad;
        hash = fnv(acc_a, sizeof acc_a, fnv(qa, count * sizeof qa[0], hash));
    }
    for (u32 i = 0; i < 4; ++i) pairs[i] = saved_pairs[i];
    return bad;
}

typedef void (*Run)(const i16*, i32*, EP*, u32);
#ifdef WITH_TIMING  // the simulator timing runs, unused in the host check
static EP ep[40];
static void prep_ep(u32 n) {
    for (u32 k = 0; k < n; ++k) {
        ep[k].phase = rnd(); ep[k].inc = rnd() >> 4; ep[k].a = amp(4000); ep[k].b = amp(4000);
        ep[k].da = static_cast<i32>(rnd()) >> 14; ep[k].db = static_cast<i32>(rnd()) >> 14; ep[k].glide = rnd() >> 20;
    }
}
static void time_gcc(const char* name, Run run, const i16* t, u32 frames) {
    puts_(name); puts_(" frames="); putu(frames);
    for (u32 n = 1; n <= 32; ++n) {
        prep_ep(n);
        run(t, acc_a, ep, n);  // warm (no cache in the sim; for the same path)
        const u32 c0 = cycles();
        run(t, acc_a, ep, n);
        const u32 c = cycles() - c0;
        puts_(" "); putu(c);
    }
    puts_("\n");
}
static void time_sine(u32 frames) {
    puts_("asm_sine frames="); putu(frames);
    for (u32 n = 1; n <= 32; ++n) {
        for (u32 k = 0; k < n; ++k) { pb[k].phase = rnd(); pb[k].increment = rnd() >> 4; pb[k].amplitude = amp(4000); pb[k].step = static_cast<i32>(rnd()) >> 14; }
        const u32 c0 = cycles();
        bank::sine(acc_b, frames, table16, 12, pb, n);
        const u32 c = cycles() - c0;
        puts_(" "); putu(c);
    }
    puts_("\n");
}
static void time_quadrature(u32 frames, u32 hold) {
    puts_(hold == 1 ? "asm_quadrature frames=" : "asm_quadrature_held8 frames="); putu(frames);
    for (u32 n = 1; n <= 32; ++n) {
        for (u32 k = 0; k < n; ++k) { qb[k].phase = rnd(); qb[k].increment = rnd() >> 4; qb[k].a = amp(4000); qb[k].b = amp(4000); qb[k].step_a = static_cast<i32>(rnd()) >> 14; qb[k].step_b = static_cast<i32>(rnd()) >> 14; }
        const u32 c0 = cycles();
        bank::quadrature(acc_b, frames, pairs, 10, qb, n, hold);
        const u32 c = cycles() - c0;
        puts_(" "); putu(c);
    }
    puts_("\n");
}
static void time_glide(u32 frames) {
    puts_("asm_sine_glide frames="); putu(frames);
    for (u32 n = 1; n <= 32; ++n) {
        for (u32 k = 0; k < n; ++k) { gb[k].phase = rnd(); gb[k].increment = rnd() >> 4; gb[k].amplitude = amp(4000); gb[k].step = static_cast<i32>(rnd()) >> 14; gb[k].glide = rnd() >> 20; }
        const u32 c0 = cycles();
        bank::sine_glide(acc_b, frames, table16, 12, gb, n, 3);
        const u32 c = cycles() - c0;
        puts_(" "); putu(c);
    }
    puts_("\n");
}
static void time_ref_sine(u32 frames) {
    puts_("gcc_reference_sine frames="); putu(frames);
    for (u32 n = 1; n <= 32; ++n) {
        for (u32 k = 0; k < n; ++k) { pb[k].phase = rnd(); pb[k].increment = rnd() >> 4; pb[k].amplitude = amp(4000); pb[k].step = static_cast<i32>(rnd()) >> 14; }
        const u32 c0 = cycles();
        bank::reference::sine(acc_b, frames, table16, 12, pb, n);
        const u32 c = cycles() - c0;
        puts_(" "); putu(c);
    }
    puts_("\n");
}

#endif
int main() {
    for (u32 i = 0; i < 4096 + 256; ++i) table16[i] = static_cast<i16>(rnd16());
    for (u32 i = 0; i < 4096; ++i) pairs[i] = (static_cast<u32>(rnd16()) & 0xffffu) | (static_cast<u32>(rnd16()) << 16);
    const u32 empty = 0;
    (void)empty;
#ifndef TRIALS
#define TRIALS 3000
#endif
    u32 h = 2166136261u;
    u32 bad = check_sine(TRIALS, h);
    puts_("identity sine: trials="); putu(TRIALS); puts_(" mismatches="); putu(bad); puts_(" hash="); puth(h); puts_("\n");
    h = 2166136261u; bad = check_glide(TRIALS, h);
    puts_("identity sine_glide: trials="); putu(TRIALS); puts_(" mismatches="); putu(bad); puts_(" hash="); puth(h); puts_("\n");
#ifdef WITH_QUADRATURE
    h = 2166136261u; bad = check_quadrature(TRIALS, 1, h);
    puts_("identity quadrature hold=1: trials="); putu(TRIALS); puts_(" mismatches="); putu(bad); puts_(" hash="); puth(h); puts_("\n");
    h = 2166136261u; bad = check_quadrature(TRIALS, 0, h);
    puts_("identity quadrature hold=2^k: trials="); putu(TRIALS); puts_(" mismatches="); putu(bad); puts_(" hash="); puth(h); puts_("\n");
    h = 2166136261u; bad = check_edges(h);
    puts_("identity edges: trials=1000 mismatches="); putu(bad); puts_(" hash="); puth(h); puts_("\n");
#endif
#ifdef WITH_TIMING
    const u32 fr[2] = {64, 32};
    for (u32 f = 0; f < 2; ++f) {
        const u32 frames = fr[f];
#define G(name) time_gcc("gcc_" #name, frames == 64 ? run_##name<64> : run_##name<32>, table16, frames);
        G(bloom_orig) G(bloom_on) G(cocoa) G(cocoa_noglide) G(butterfly) G(hollow) G(sand) G(bloom_on_ool) G(cocoa_ool) G(butterfly_ool) G(hollow_ool) G(sand_ool)
        time_ref_sine(frames);
        time_sine(frames);
        time_glide(frames);
#ifdef WITH_QUADRATURE
        time_quadrature(frames, 1);
        time_quadrature(frames, 8);
#endif
    }
#endif
    return 0;
}
