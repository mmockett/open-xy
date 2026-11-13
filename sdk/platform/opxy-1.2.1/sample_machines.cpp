// Stock sample dispatch for every registered SDK sample engine. A matching
// engine number is not enough: the machine's registration must still be ours.
// The stubs in sample_hooks.s call these from inside stock functions.
#include "sample_machines.hpp"
#include "drum_machines.hpp"

using opxy::u32;
using opxy::platform::SampleMachine;

extern "C" {
u32 sdk_sample_note_route(u32) __asm__("sdk_sample_note_route");
u32 sdk_sample_alias(u32) __asm__("sdk_sample_alias");
}

const SampleMachine* sdk_sample_machine(u32 engine) {
    for (u32 i = 0; i < sdk_sample_machine_count; ++i) {
        const SampleMachine& m = sdk_sample_machines[i];
        if (m.engine() == engine) return m.ready() == 1 ? &m : nullptr;
    }
    return nullptr;
}

// Note-region hook, decoded by extensions_sample_note_region (sample_hooks.s).
// An owned engine borrows the multisampler payload and keeps stock's
// nearest-region test, except on the drum page (7), where notes map to its
// canonical pads.
enum NoteRoute : u32 { kStockRoute = 0, kOwnedNearest = 1, kOwnedDrumPads = 2 };
u32 sdk_sample_note_route(u32 engine) {
    if (sdk_drum_machine(engine)) return 3;
    const u32 page = opxy::platform::sample_page(engine);
    return !page ? kStockRoute : page == 7 ? kOwnedDrumPads : kOwnedNearest;
}

// Stock's sample predicates know only stock engine IDs. Owned SDK engines
// borrow the multisampler's (30) payload; the track keeps the real engine.
u32 sdk_sample_alias(u32 engine) { return opxy::platform::sample_page(engine) ? 30 : engine; }
