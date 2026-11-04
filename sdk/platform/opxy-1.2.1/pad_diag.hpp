// padtest diagnostic (local test builds only, never for release): while Shift is
// held on a padMix drum kit's track, log where encoder presses and turns go, so a
// device log shows why a first Shift turn or a Shift press did nothing. Lines go
// through stock's logger (0x8c042efc, level 3: "I" lines in /logs/log_*.txt,
// "bfm" from the main board), at most kPadDiagLines a boot from each file.
#pragma once
#include "stock.hpp"
#include "drum_machines.hpp"

namespace opxy { namespace platform { namespace {
const u32 kPadDiagLines = 120;
u32 pad_diag_lines;
// A padMix kit on the selected instrument track.
inline const DrumMachine* pad_diag_kit(u32 track) {
    const DrumMachine* kit = track < kTracks ? sdk_drum_machine(track_engine(track)) : nullptr;
    return kit && kit->pad_mix ? kit : nullptr;
}
template <class... A> void pad_diag(const char* file, u32 line, const char* format, A... args) {
    if (pad_diag_lines >= kPadDiagLines) return;
    if (++pad_diag_lines == kPadDiagLines) format = "pad diag: line limit reached, quiet until reboot";
    stock<void(u32, const char*, u32, const char*, ...)>(0x8c042efc)(3, file, line, format, args...);
}
// The per-key editor's four conditions in stock's encoder dispatcher (8c0b12e4),
// as it tests them before 8c0b147a: the instrument page word, 8d5b7e34 (0 to
// edit), the flag byte 0x860c9b3c (0 to edit) and the drum check 8d5b79f0.
inline void pad_diag_checks(u32& page, u32& step, u32& flag, u32& drum, u32 engine) {
    page = word(0x080238bc);
    step = stock<u32()>(0x8d5b7e34)();
    flag = byte(0x860c9b3c);
    drum = stock<u32(u32)>(0x8d5b79f0)(engine);
}
}}}
