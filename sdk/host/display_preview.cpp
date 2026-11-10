// Main-thread WebAssembly drawing transport. Separate from audio scheduling.
#include "display_binding.hpp"
#include <opxy/sample_overview.hpp>
#include <opxy/control_interaction.hpp>
namespace {
opxy::binding::DisplayState state;
opxy::binding::DisplayInput input;
opxy::binding::Frame frame;
// The overview a sample engine's screen reads; the host writes it (empty until then).
// Its gain starts unavailable, as on the device while a track holds no sample.
opxy::draw::SampleOverview sample = {0, 0, 0, 0, 0, 0, 0, 0, 0, {}, {}, opxy::draw::gain_unavailable};
}
const opxy::draw::SampleOverview* opxy_sample_overview() { return &sample; }
extern "C" {
// A preview shows one machine on one track.
opxy::u32 sdk_control_copies(opxy::u32) { return 1; }
opxy::i32 sdk_control_display(opxy::u32) { return 0; }
opxy::u32 opxy_control_source(opxy::u32 available) {
    return available<=1 && opxy::control_interaction::configure_preview_source(available!=0);
}
opxy::u32 opxy_control_event(opxy::u32 track,opxy::u32 physical,opxy::u32 parameter,opxy::i32 value) {
    return opxy::control_interaction::submit_preview(track,physical,parameter,value);
}

// 6, or SDK 1.12's audio and encoder inputs (display.mjs).
opxy::u32 opxy_display_abi() { return opxy::binding::display_abi; }
opxy::u32 opxy_display_audio_words() { return opxy::binding::audio_words; }
opxy::u32 opxy_display_state_bytes() { return sizeof(state); }
opxy::u32 opxy_display_input_bytes() { return sizeof(input); }
opxy::u32 opxy_display_frame_bytes() { return sizeof(frame); }
opxy::binding::DisplayState* opxy_display_state() { return &state; }
opxy::binding::DisplayInput* opxy_display_input() { return &input; }
opxy::binding::Frame* opxy_display_frame() { return &frame; }
void opxy_display_reset() { opxy::binding::display_reset(&state, &frame); }
void opxy_display_render() { opxy::binding::display_render(&state, &input, &frame); }
opxy::u32 opxy_display_valid() { return opxy::binding::display_valid(&frame); }
opxy::draw::SampleOverview* opxy_display_sample() { return &sample; }
}
