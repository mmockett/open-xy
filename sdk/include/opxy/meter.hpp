#pragma once
#include "types.hpp"

// Test build only: each custom machine's live CPU, for a screen to show. On the
// device the audio board times every machine's render, and the numbers reach the
// GUI one machine at a time. Elsewhere there is nothing to read.
namespace opxy { namespace meter {
struct Row {
    const char* name;
    u32 now, peak;  // tenths of a percent of the block
};
#if defined(__bfin__)
extern "C" void sdk_meter_poll(u32 hold_frames) __asm__("sdk_meter_poll");
extern "C" u32 sdk_meter_rows() __asm__("sdk_meter_rows");
extern "C" u32 sdk_meter_row(u32 index, const char** name, u32* now, u32* peak) __asm__("sdk_meter_row");
// Once a frame: takes in the row the audio board last sent.
inline void poll(u32 hold_frames) { sdk_meter_poll(hold_frames); }
inline u32 rows() { return sdk_meter_rows(); }
inline bool row(u32 index, Row& out) { return sdk_meter_row(index, &out.name, &out.now, &out.peak) != 0; }
#else
inline void poll(u32) {}
inline u32 rows() { return 0; }
inline bool row(u32, Row&) { return false; }
#endif
}}  // namespace opxy::meter
