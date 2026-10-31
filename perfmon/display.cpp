#include "display.h"

namespace perfmon {
// Test build only: a list of the build's machines with their live CPU. M1 scrolls
// it, M2 sets how long a peak holds, from a second to a minute. Text changes four
// times a second, so the page itself adds little to the screen's work.
using opxy::i32;
using opxy::u32;
using namespace opxy::draw;

inline void display_reset(DisplayState& state) { state.frames = state.first = 0; }

inline void percent(char* out, u32 tenths) {
    char d[6];
    u32 k = 0;
    d[k++] = static_cast<char>('0' + tenths % 10);
    d[k++] = '.';
    u32 whole = tenths / 10;
    do { d[k++] = static_cast<char>('0' + whole % 10); whole /= 10; } while (whole && k < 5);
    while (k) *out++ = d[--k];
    *out = 0;
}

inline void display(DisplayState& state, const Input& input, Frame& frame) {
    const u32 hold = 60 + (clamp(input.parameters[1], 0, 0x7fffffff) >> 20) * 3540 / 2047;
    opxy::meter::poll(hold);
    const u32 total = opxy::meter::rows();
    const u32 most = total > kRows ? total - kRows : 0;
    const u32 first = (clamp(input.parameters[0], 0, 0x7fffffff) >> 20) * (most + 1) / 2048;
    if (state.frames++ % 15 && first == state.first) return;
    state.first = first;
    place(frame, 0, 0, 0, unit, 0, keep);
    for (u32 r = 0; r < kRows; ++r) {
        opxy::meter::Row row;
        const u32 at = r * 3;
        char now[8] = "", peak[8] = "";
        const char* name = "";
        if (opxy::meter::row(first + r, row)) {
            name = row.name;
            percent(now, row.now);
            percent(peak, row.peak);
        }
        text(frame, at, 30 * unit, (62 + static_cast<i32>(r) * 16) * unit, unit, name);
        text(frame, at + 1, 300 * unit, (62 + static_cast<i32>(r) * 16) * unit, unit, now);
        text(frame, at + 2, 380 * unit, (62 + static_cast<i32>(r) * 16) * unit, unit, peak);
    }
    text(frame, 30, 300 * unit, 46 * unit, unit, "now, peak % of block");
    text(frame, 31, 30 * unit, 46 * unit, unit, "live, not a benchmark");
}
}
