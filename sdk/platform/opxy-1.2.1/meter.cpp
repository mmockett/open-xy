// Test build only, never for release: each custom machine's live CPU.
//
// Audio board: the generated voice and effect adapters time each render with
// CYCLES and add it to their machine's total. Every kWindow cycles (16 blocks,
// about 23 ms) the totals close; one row, in turn, goes to the GUI through stock's
// telemetry id 25 (8d4d666a), which no stock code writes or reads. A word is one
// row, so a row arrives about every 23 ms and the whole list several times a second.
// A window's total is an average over those 16 blocks, so a peak here is the
// loudest 23 ms, not the worst single block.
//
// GUI board: whichever screen shows the rows polls the word once a frame.
//
// After the machines come two rows of the meter's own: every machine together, and
// what the timing itself costs, timed by running it once a window.
#include <opxy/types.hpp>
#include "stock.hpp"

using opxy::u32;
using opxy::platform::stock;
using opxy::platform::word;

extern "C" {
extern const char* const sdk_meter_names[] __asm__("sdk_meter_names");
extern const u32 sdk_meter_count __asm__("sdk_meter_count");
u32 sdk_benchmark_cycles() __asm__("sdk_benchmark_cycles");
void sdk_meter_add(u32 index, u32 start) __asm__("sdk_meter_add");
u32 sdk_meter_start() __asm__("sdk_meter_start");
void sdk_meter_poll(u32 hold_frames) __asm__("sdk_meter_poll");
u32 sdk_meter_rows() __asm__("sdk_meter_rows");
u32 sdk_meter_row(u32 index, const char** name, u32* now, u32* peak) __asm__("sdk_meter_row");
void sdk_probe_init(u32 index, u32 slot) __asm__("sdk_probe_init");
void sdk_probe_in(u32 index, u32 slot, const opxy::i32* left, const opxy::i32* parameters) __asm__("sdk_probe_in");
void sdk_probe_out(u32 index, const opxy::i32* left, const opxy::i32* right) __asm__("sdk_probe_out");
}

namespace {
const u32 kMachines = 40, kRows = kMachines + 2;
const u32 kBlockCycles = 580352, kWindow = 16 * kBlockCycles;
const u32 kTelemetry = 0x080376e4, kId = 25;

// Click probe, audio board: what a send effect sees that changes, as events in a
// ring, sent one per window. An event is 0xe0 + code in the top byte, then a value:
//   1 init: slot
//   2 a control word moved over 1%: word in bits 16..23, its new top 16 bits
//   3 the shift word changed: its top 16 bits (word 16)
//   4 the output stepped over 6% of full scale within a block or across blocks:
//     step in 1/4096 of full scale in bits 12..23, the input's peak in the low 12
//   5 the input reaching the effect stepped over 3%: the same fields
const u32 kRing = 64;
u32 ring[kRing], ring_in, ring_out;
opxy::i32 last_words[2][9], last_out[2], last_in[2], in_peak[2];
void event(u32 code, u32 value) {
    if (ring_in - ring_out < kRing) ring[ring_in++ % kRing] = (0xe0u + code) << 24 | (value & 0xffffffu);
}
inline u32 size(opxy::i32 x) { return x < 0 ? 0u - static_cast<u32>(x) : static_cast<u32>(x); }

// Audio board.
u32 totals[kMachines + 1], lasts[kRows], peaks[kRows];
u32 window_start, calls, cursor, spare;

u32 permille(u32 cycles, u32 elapsed) {
    const u32 scale = elapsed / 1000;
    const u32 value = scale ? cycles / scale : 0;
    return value > 0xfff ? 0xfff : value;
}

__attribute__((noinline)) u32 tally(u32 index, u32 start) {
    const u32 now = sdk_benchmark_cycles();
    totals[index] += now - start;
    return now;
}

void close_window(u32 now) {
    const u32 elapsed = now - window_start;
    window_start = now;
    const u32 count = sdk_meter_count;
    u32 all = 0;
    for (u32 i = 0; i < count; ++i) {
        all += totals[i];
        lasts[i] = totals[i];
        if (totals[i] > peaks[i]) peaks[i] = totals[i];
        totals[i] = 0;
    }
    lasts[count] = all;
    if (all > peaks[count]) peaks[count] = all;
    // The cost of one timed call, as a machine's adapter pays it, times the calls.
    const u32 calls_now = calls;
    const u32 before = sdk_benchmark_cycles();
    tally(kMachines, sdk_benchmark_cycles());
    const u32 own = (sdk_benchmark_cycles() - before) * calls_now;
    calls = 0;
    totals[kMachines] = 0;
    lasts[count + 1] = own;
    if (own > peaks[count + 1]) peaks[count + 1] = own;
    if (ring_out != ring_in) {
        // Events first: they carry the probe's answer, and rows can wait a window.
        stock<u32(u32, u32)>(0x8d4d666a)(kId, ring[ring_out++ % kRing]);
        return;
    }
    const u32 row = cursor;
    cursor = row + 1 >= count + 2 ? 0 : row + 1;
    stock<u32(u32, u32)>(0x8d4d666a)(kId, row << 24 | permille(peaks[row], elapsed) << 12 | permille(lasts[row], elapsed));
    peaks[row] = 0;
}

// GUI board.
u32 log_lines;
u32 last_seen;
u32 shown_now[kRows], shown_peak[kRows], peak_frame[kRows], frames;
}

u32 sdk_meter_start() { return sdk_benchmark_cycles(); }

void sdk_meter_add(u32 index, u32 start) {
    const u32 now = tally(index, start);
    ++calls;
    if (now - window_start >= kWindow) close_window(now);
}

void sdk_meter_poll(u32 hold_frames) {
    ++frames;
    const u32 w = word(kTelemetry + kId * 4);
    const u32 row = w >> 24;
    if (row >= 0xe0 && w != last_seen && log_lines < 800) {
        ++log_lines;
        stock<void(u32, const char*, u32, const char*, ...)>(0x8c042efc)(3, "../src/sdk/clickprobe.cpp", log_lines,
                                                                         "clickprobe code=%u value=%06x", row - 0xe0, w & 0xffffffu);
    }
    last_seen = w;
    if (row >= sdk_meter_count + 2) return;
    const u32 peak = (w >> 12) & 0xfff;
    shown_now[row] = w & 0xfff;
    if (peak >= shown_peak[row] || frames - peak_frame[row] > hold_frames) {
        shown_peak[row] = peak;
        peak_frame[row] = frames;
    }
}

u32 sdk_meter_rows() { return sdk_meter_count + 2; }

u32 sdk_meter_row(u32 index, const char** name, u32* now, u32* peak) {
    const u32 count = sdk_meter_count;
    if (index >= count + 2) return 0;
    *name = index < count ? sdk_meter_names[index] : index == count ? "all" : "meter";
    *now = shown_now[index];
    *peak = shown_peak[index];
    return 1;
}

void sdk_probe_init(u32, u32 slot) {
    event(1, slot);
    const u32 side = slot & 1;
    last_out[side] = 0;
    for (u32 i = 0; i < 9; ++i) last_words[side][i] = -1;
}

void sdk_probe_in(u32, u32 slot, const opxy::i32* left, const opxy::i32* parameters) {
    const u32 side = slot & 1;
    opxy::i32* last = last_words[side];
    for (u32 i = 0; i < 8; ++i) {
        const opxy::i32 w = parameters[i];
        const opxy::i32 d = w - last[i];
        if (last[i] != -1 && (d > 21474836 || d < -21474836)) event(2, i << 16 | static_cast<u32>(w) >> 16);
        last[i] = w;
    }
    // The shift row's word, as the adapter reads it (shift_row.hpp).
    const opxy::i32 packed = static_cast<opxy::i32>(word(0x84eab39c + slot * 0xe8 + 36 * 4));
    if (last[8] != -1 && packed != last[8]) event(3, static_cast<u32>(packed) >> 16);
    last[8] = packed;
    u32 peak = 0, worst = 0;
    opxy::i32 previous = last_in[side];
    for (u32 i = 0; i < 64; ++i) {
        const u32 a = size(left[i]);
        if (a > peak) peak = a;
        const u32 step = size((left[i] >> 1) - (previous >> 1));
        if (step > worst) worst = step;
        previous = left[i];
    }
    last_in[side] = previous;
    in_peak[side] = static_cast<opxy::i32>(peak);
    if (worst > 0x02000000u) event(5, (worst >> 18) << 12 | peak >> 19);
}

void sdk_probe_out(u32 index, const opxy::i32* left, const opxy::i32*) {
    const u32 side = index & 1;
    opxy::i32 previous = last_out[side];
    u32 worst = 0;
    for (u32 i = 0; i < 64; ++i) {
        const opxy::i32 d = (left[i] >> 1) - (previous >> 1);
        const u32 step = size(d);
        if (step > worst) worst = step;
        previous = left[i];
    }
    last_out[side] = previous;
    if (worst > 0x04000000u) event(4, (worst >> 18) << 12 | static_cast<u32>(in_peak[side]) >> 19);
}
