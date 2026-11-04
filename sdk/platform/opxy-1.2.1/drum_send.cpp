// Per-key sends (drum_send.hpp): the C++ side of the stock mixer hooks in
// drum_send_hooks.s. Audio role only; nothing here allocates or waits.
#include "drum_send.hpp"

using opxy::i32;
using opxy::u32;
using namespace opxy::platform;

extern "C" {
void sdk_drum_send_begin() __asm__("sdk_drum_send_begin");
void sdk_drum_send_voice(const unsigned char* voice, u32 track) __asm__("sdk_drum_send_voice");
// Read by sdk_drum_send_source_1 and _2: per send, the track plus one and its source.
extern DrumSendState sdk_drum_send_state __asm__("sdk_drum_send_state");
}

DrumSendState sdk_drum_send_state;

namespace {
// The track sums the per-key shares feed FX I and FX II from. One pair is
// enough: the engine (8d510760) mixes its tracks one at a time, each voice loop
// (8d51c708, called only at 8d510bea) followed by that track's sends.
i32 scratch[kDrumSends][kDrumSendWords];
// Stock's stereo sum of the track's voices, which the engine hands the voice
// loop (8d510be2) and its send loops read (8d511088, 8d5110ca).
i32* const kTrackSum = reinterpret_cast<i32*>(0x11900000);
// The voice just rendered, as the loop adds it into the sum (8d51c7b2).
const i32* const kVoiceOutput = reinterpret_cast<const i32*>(0x11b01548);
// The stock voice's machine object, whose method table stock calls (8d51b53e).
const u32 kVoiceObject = 0x94;

u32 codes_of(const unsigned char* voice) {
    const void* object = *reinterpret_cast<const void* const*>(voice + kVoiceObject);
    if (!object) return 0;
    const void* table = *static_cast<const void* const*>(object);
    for (u32 i = 0; i < sdk_drum_send_kit_count; ++i)
        if (sdk_drum_send_kits[i].vtable == table) return sdk_drum_send_kits[i].codes(object);
    return 0;
}
}

// 8d510bea: a track's voice loop is about to run.
void sdk_drum_send_begin() { drum_send_begin(sdk_drum_send_state); }

// 8d51c7b2: before stock adds `voice` (its rendered block at 0x11b01548) into
// the sum of `track`.
void sdk_drum_send_voice(const unsigned char* voice, u32 track) {
    drum_send_voice(sdk_drum_send_state, scratch, track, kTrackSum, kVoiceOutput, codes_of(voice));
}
