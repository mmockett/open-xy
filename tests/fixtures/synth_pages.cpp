// Host boundary double for the real hidden-pages check in synth_pages.cpp:
// stock's page words and setters (instrument and aux), the selected track,
// track engines and the machines holding aux tracks.
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <synth_pages.cpp>

namespace {
using opxy::u32;
using opxy::platform::Address;

void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
u32 page, sets, selected, engines[16];
void stock_set_page(u32 value) { page = value; ++sets; }
// The aux controller's tab word and its setter, and the machine holding each aux track.
u32 aux_tab, aux_sets, aux_engines[16];
void stock_set_aux_tab(u32 controller, u32 value) {
    require(controller == 0x08014db4, "Aux tab set on another controller");
    aux_tab = value;
    ++aux_sets;
}
u32 stock_selected() { return selected; }
u32 stock_engine(u32 track) { return track < 16 ? engines[track] : 0; }
// Each track's stock LFO on/off byte, and how many times stock's setter ran.
u32 lfo[16], lfo_sets;
u32 stock_lfo_on(u32 track) { require(track < 8, "LFO read off an instrument track"); return lfo[track]; }
void stock_lfo_set(u32 track, u32 value) {
    require(track < 8 && value == 0, "LFO set other than off");
    lfo[track] = value;
    ++lfo_sets;
}
// Stock's held keys by number (M1..M4 are 6..9) and Shift.
u32 held[56], shift;
u32 stock_key_held(u32 key) { require(key < 56, "Key outside stock's list"); return held[key]; }
u32 stock_shift() { return shift; }
// Holds page key M<n> (1..4) for a tick, then lets it go for one.
void press(u32 n) {
    held[5 + n] = 1;
    sdk_synth_pages();
    held[5 + n] = 0;
    sdk_synth_pages();
}
int ready() { return 1; }
}

extern "C" {
// Engine 40 hides the envelope and filter pages; engine 41 hides nothing; 42
// hides every page but the engine page.
const opxy::platform::SynthControls sdk_synth_controls[] = {
    {reinterpret_cast<const char*>(40), ready, {0, 1, 2, 3, 4, 5, 6, 7}, 0, {0, 0, 0, 0, 0, 0, 0, 0}, nullptr, false, false, 6},
    {reinterpret_cast<const char*>(41), ready, {0, 1, 2, 3, 4, 5, 6, 7}, 0, {0, 0, 0, 0, 0, 0, 0, 0}, nullptr, false, false, 0},
    {reinterpret_cast<const char*>(42), ready, {0, 1, 2, 3, 4, 5, 6, 7}, 0, {0, 0, 0, 0, 0, 0, 0, 0}, nullptr, false, false, 14},
    {nullptr, nullptr, {0, 1, 2, 3, 4, 5, 6, 7}, 0, {0, 0, 0, 0, 0, 0, 0, 0}, nullptr, false, false, 0}};
const u32 sdk_synth_control_count = 3;
}

u32 opxy::platform::control_engine(u32 track) {
    return track < 8 ? engines[track] : track < 16 ? aux_engines[track] : 0;
}

Address opxy::platform::host_stock_address(Address address) {
    if (address == 0x080238bc) return reinterpret_cast<Address>(&page);
    if (address == 0x0801eae4) return reinterpret_cast<Address>(&aux_tab);
    if (address == 0x8c08f74c) return reinterpret_cast<Address>(&stock_set_aux_tab);
    if (address == 0x8c0b19c4) return reinterpret_cast<Address>(&stock_set_page);
    if (address == 0x8d4d6f14) return reinterpret_cast<Address>(&stock_selected);
    if (address == 0x8d58781c) return reinterpret_cast<Address>(&stock_engine);
    if (address == 0x8d5879c0) return reinterpret_cast<Address>(&stock_lfo_on);
    if (address == 0x8d587984) return reinterpret_cast<Address>(&stock_lfo_set);
    if (address == 0x8d5b7d7c) return reinterpret_cast<Address>(&stock_key_held);
    if (address == 0x8d51f184) return reinterpret_cast<Address>(&stock_shift);
    std::abort();
}

int main() {
    engines[2] = 40;
    engines[3] = 41;
    // A stock engine and a synth hiding nothing keep every page.
    for (u32 track : {0u, 3u})
        for (u32 p = 0; p < 4; ++p) {
            selected = track;
            page = p;
            sdk_synth_pages();
            require(page == p && sets == 0, "Page changed for a synth hiding nothing");
        }
    // A synth hiding every page but the engine page stays there.
    engines[4] = 42;
    selected = 4;
    page = 3;
    sdk_synth_pages();
    require(page == 0 && sets == 1, "LFO page not hidden");
    sets = 0;
    // On the hiding synth, M4 then M2: back to M4; M3 from M1: back to M1.
    selected = 2;
    page = 3;
    sdk_synth_pages();
    page = 1;
    sdk_synth_pages();
    require(page == 3 && sets == 1, "Envelope page not replaced by the page before");
    page = 0;
    sdk_synth_pages();
    page = 2;
    sdk_synth_pages();
    require(page == 0 && sets == 2, "Filter page not replaced by the page before");
    require(sdk_synth_page_key(2) == 0, "A page change taken for a key press");
    // Selecting the track while on another track's envelope page: the engine page.
    selected = 3;
    page = 1;
    sdk_synth_pages();
    selected = 2;
    sdk_synth_pages();
    require(page == 0 && sets == 3, "Hidden page kept after selecting the track");
    // Each page key pressed on the selected track is kept for its machine,
    // once, whether its page is hidden or shown, and held keys count once.
    for (u32 n = 1; n <= 4; ++n) {
        press(n);
        require(sdk_synth_page_key(3) == 0 && sdk_synth_page_key(2) == n && sdk_synth_page_key(2) == 0,
                "Page key not kept once for its track");
    }
    held[8] = 1;
    sdk_synth_pages();
    sdk_synth_page_key(2);
    sdk_synth_pages();
    held[8] = 0;
    require(sdk_synth_page_key(2) == 0, "A held page key taken for another press");
    // Shift + a page key is stock's; a synth hiding nothing gets no keys.
    shift = 1;
    press(1);
    shift = 0;
    require(sdk_synth_page_key(2) == 0, "Shift + M1 taken for a page key");
    selected = 3;
    press(2);
    require(sdk_synth_page_key(3) == 0, "Page key kept for a synth hiding nothing");
    selected = 2;
    // An aux track nothing holds is left alone, whatever its stock engine word says.
    const u32 sets_before = sets;
    selected = 9;
    engines[9] = 40;
    aux_tab = 1;
    sdk_synth_pages();
    require(aux_tab == 1 && sets == sets_before && aux_sets == 0, "Auxiliary track's page changed");
    // One a machine holds hides its pages on the aux controller's tab, and keeps a
    // press of a hidden page's key for the machine, as on an instrument track.
    selected = 11;
    aux_engines[11] = 42;
    aux_tab = 0;
    sdk_synth_pages();
    aux_tab = 3;
    sdk_synth_pages();
    require(aux_tab == 0 && aux_sets == 1 && sets == sets_before, "Held aux track's hidden tab kept");
    // M1 and M2 leave CV's tab as it was; they still reach the machine.
    for (u32 n = 1; n <= 4; ++n) {
        press(n);
        require(sdk_synth_page_key(11) == n && sdk_synth_page_key(11) == 0, "Page key on a held aux track not kept once");
    }
    aux_engines[11] = 0;
    sdk_synth_pages();

    // A stock LFO left on is switched off, once, on tracks whose synth hides
    // the LFO page (engine 42, track 4), whether or not they are selected;
    // other synths and stock engines keep theirs.
    lfo[0] = lfo[2] = lfo[3] = lfo[4] = 1;
    selected = 0;
    page = 0;
    lfo_sets = 0;
    sdk_synth_pages();
    require(lfo[4] == 0 && lfo[0] == 1 && lfo[2] == 1 && lfo[3] == 1 && lfo_sets == 1, "LFO not switched off alone");
    sdk_synth_pages();
    require(lfo_sets == 1, "LFO switched off again while off");
    // Moving the synth to a track whose LFO is on switches that one off too.
    lfo[5] = 1;
    engines[5] = 42;
    sdk_synth_pages();
    require(lfo[5] == 0 && lfo_sets == 2, "LFO kept on the track the synth moved to");
    return 0;
}
