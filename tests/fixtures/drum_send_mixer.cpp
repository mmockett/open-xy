// Per-key sends: drum_send.cpp's hooks inside a host model of stock 1.2.1's
// mixer (8d510760), as drum_send_hooks.s places them. The model follows the
// stock instructions: the voice loop's saturating add of each rendered voice
// into the track's 128-word stereo sum (8d51c7b0..8d51c7d8), then the main mix
// at a constant gain (8d511038..8d511084), FX I at a gain stepped every word
// (8d511088..8d5110c8) and FX II at a constant gain (8d5110ca..8d51110e). A
// skipped track (8d51237a) mixes nothing. The test substitutes host buffers
// for stock's 0x11900000 and 0x11b01548.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <opxy/q31.hpp>
// The stock buffers drum_send.cpp names, as the test substitutes them.
extern opxy::i32 host_sum[], host_voice[];
#include "drum_send.cpp"

using opxy::i32;
using opxy::u32;
namespace q31 = opxy::q31;

i32 host_sum[kDrumSendWords], host_voice[kDrumSendWords];

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (0)

namespace {
// A kit's voices as the hook sees them: a method table and both codes per handle.
const void* kit_table[11];
const void* stock_table[11];
struct Handle { const void* const* vtable; u32 codes; };
u32 kit_codes(const void* handle) { return static_cast<const Handle*>(handle)->codes; }
}
const DrumSendKit sdk_drum_send_kits[] = {{kit_table, kit_codes}};
const u32 sdk_drum_send_kit_count = 1;

namespace {
// FX I's code in bits 0..2, FX II's in 3..5, as drum_send_codes gives them.
u32 pair(u32 fx1, u32 fx2) { return fx1 | fx2 << kDrumSendBits; }

// A stock voice record: its machine object at +0x94 (8d51b53e).
struct StockVoice { unsigned char bytes[0x94 + sizeof(void*)]; Handle handle; i32 audio[kDrumSendWords]; };
void bind(StockVoice& voice, bool kit, u32 codes) {
    voice.handle.vtable = kit ? kit_table : stock_table;
    voice.handle.codes = codes;
    const void* object = &voice.handle;
    std::memcpy(voice.bytes + 0x94, &object, sizeof object);
}

struct Gains { i32 main, fx1, step, fx2; };
struct Buses { i32 main[kDrumSendWords], fx1[kDrumSendWords], fx2[kDrumSendWords]; };

u32 seed = 12345;
i32 noise() { seed = seed * 1664525u + 1013904223u; return static_cast<i32>(seed); }

// One track of one block. `hooks` places the SDK's calls; `skip` takes stock's
// path that leaves the track out of every bus. `start` is what stock's sum
// holds before the first voice.
void mix(Buses& buses, u32 track, StockVoice* voices, u32 count, const i32* start, const Gains& g, bool hooks, bool skip) {
    if (hooks) sdk_drum_send_begin();                         // 8d510bea
    for (u32 i = 0; i < kDrumSendWords; ++i) host_sum[i] = start[i];
    for (u32 v = 0; v < count; ++v) {
        std::memcpy(host_voice, voices[v].audio, sizeof host_voice);   // the renderer's output
        if (hooks) sdk_drum_send_voice(voices[v].bytes, track);       // 8d51c7b2
        for (u32 i = 0; i < kDrumSendWords; ++i) host_sum[i] = q31::add(host_sum[i], host_voice[i]);
    }
    if (skip) return;
    for (u32 i = 0; i < kDrumSendWords; ++i) buses.main[i] = q31::add(buses.main[i], q31::multiply(g.main, host_sum[i]));
    // 8d511088: FX I's source; word k at the gain stepped k + 1 times.
    const i32* source = hooks ? drum_send_source(sdk_drum_send_state, 0, track, host_sum) : host_sum;
    i32 gain = g.fx1;
    for (u32 i = 0; i < kDrumSendWords; ++i) {
        gain = q31::add(gain, g.step);
        buses.fx1[i] = q31::add(buses.fx1[i], q31::multiply(gain, source[i]));
    }
    // 8d5110ca: FX II's source.
    source = hooks ? drum_send_source(sdk_drum_send_state, 1, track, host_sum) : host_sum;
    for (u32 i = 0; i < kDrumSendWords; ++i) buses.fx2[i] = q31::add(buses.fx2[i], q31::multiply(g.fx2, source[i]));
}

bool same(const Buses& a, const Buses& b) { return !std::memcmp(&a, &b, sizeof a); }
void fill(i32* words, i32 scale) { for (u32 i = 0; i < kDrumSendWords; ++i) words[i] = scale ? noise() / scale : noise(); }
bool idle() { return !sdk_drum_send_state.bus[0].track && !sdk_drum_send_state.bus[1].track; }

// A sum worked out apart from the hooks: each voice at its share of one send.
// No word here comes near saturation.
void expected_sum(i32* out, const i32* start, const StockVoice* voices, u32 count, u32 send) {
    for (u32 i = 0; i < kDrumSendWords; ++i) {
        int64_t sum = start[i];
        for (u32 v = 0; v < count; ++v) {
            const u32 code = drum_send_code(voices[v].handle.vtable == kit_table ? voices[v].handle.codes : 0, send);
            const int64_t product = int64_t(kDrumSendGains[code]) * voices[v].audio[i];
            sum += code ? (product >= 0 ? product >> 31 : -((-product + ((int64_t(1) << 31) - 1)) >> 31)) : voices[v].audio[i];
        }
        CHECK(sum > -(int64_t(1) << 31) && sum < (int64_t(1) << 31));
        out[i] = static_cast<i32>(sum);
    }
}

// Random voices, gains and bus contents, from quiet to saturating: stock
// voices and a kit's followers mix exactly as stock does.
void stock_and_followers_are_unchanged() {
    static StockVoice voices[8];
    static i32 start[kDrumSendWords];
    for (u32 round = 0; round < 300; ++round) {
        const i32 scale = round % 4 == 0 ? 0 : 1 << (round % 7);   // 0: full-scale, saturating sums
        const u32 count = 1 + round % 8;
        for (u32 v = 0; v < count; ++v) {
            fill(voices[v].audio, scale);
            // Stock voices carrying stray codes, a kit's followers, or both on one track.
            const bool kit = round % 3 != 0 && (v + round) % 2 == 0;
            bind(voices[v], kit, kit ? 0 : pair(round % 8, (round + v) % 8));
        }
        fill(start, round % 5 == 0 ? 1 << 4 : 1 << 30);
        const Gains g = {noise(), noise() & 0x7fffffff, noise() >> 10, noise() & 0x7fffffff};
        static Buses stock_buses, hooked;
        fill(stock_buses.main, 4); fill(stock_buses.fx1, 4); fill(stock_buses.fx2, 4);
        hooked = stock_buses;
        const u32 track = round % 8;
        mix(stock_buses, track, voices, count, start, g, false, round % 11 == 0);
        mix(hooked, track, voices, count, start, g, true, round % 11 == 0);
        CHECK(same(stock_buses, hooked));
        CHECK(idle());
    }
}

// Overlapping voices of one kit track with their own shares of each send, in
// mixed settings: the main mix keeps stock's sum, each send gets every voice at
// its share of that send, and a send all the voices follow stays stock's.
void shares_route_each_voice() {
    static StockVoice voices[5];
    static i32 start[kDrumSendWords];
    static Buses stock_buses, hooked, before;
    const Gains g = {0x60000000, 0x10000000, 0x00100000, 0x50000000};
    // FX I: dry, follow, 3/7, 6/7, follow; FX II: follow, 1/7, dry, follow, 5/7.
    const u32 codes[5] = {pair(1, 0), pair(0, 2), pair(4, 1), pair(7, 0), pair(0, 6)};
    for (u32 v = 0; v < 5; ++v) { fill(voices[v].audio, 1 << 4); bind(voices[v], true, codes[v]); }
    fill(start, 1 << 6);
    std::memset(&stock_buses, 0, sizeof stock_buses);
    std::memset(&hooked, 0, sizeof hooked);
    mix(stock_buses, 2, voices, 5, start, g, false, false);
    mix(hooked, 2, voices, 5, start, g, true, false);
    CHECK(!std::memcmp(stock_buses.main, hooked.main, sizeof hooked.main));
    CHECK(sdk_drum_send_state.bus[0].track == 3 && sdk_drum_send_state.bus[1].track == 3);
    static i32 fx1[kDrumSendWords], fx2[kDrumSendWords];
    expected_sum(fx1, start, voices, 5, 0);
    expected_sum(fx2, start, voices, 5, 1);
    i32 gain = g.fx1;
    for (u32 i = 0; i < kDrumSendWords; ++i) {
        gain = q31::add(gain, g.step);
        CHECK(hooked.fx1[i] == q31::multiply(gain, fx1[i]));
        CHECK(hooked.fx2[i] == q31::multiply(g.fx2, fx2[i]));
    }
    // Shares of FX II only: FX I is stock's, word for word.
    for (u32 v = 0; v < 5; ++v) bind(voices[v], true, pair(0, 1 + v % 7));
    hooked = stock_buses;
    before = stock_buses;
    mix(stock_buses, 4, voices, 5, start, g, false, false);
    mix(hooked, 4, voices, 5, start, g, true, false);
    CHECK(!std::memcmp(stock_buses.main, hooked.main, sizeof hooked.main));
    CHECK(!std::memcmp(stock_buses.fx1, hooked.fx1, sizeof hooked.fx1));
    CHECK(!sdk_drum_send_state.bus[0].track && sdk_drum_send_state.bus[1].track == 5);
    expected_sum(fx2, start, voices, 5, 1);
    for (u32 i = 0; i < kDrumSendWords; ++i) CHECK(hooked.fx2[i] == q31::add(before.fx2[i], q31::multiply(g.fx2, fx2[i])));
    // A key dry to both sends alone sends nothing while the main mix is stock's.
    bind(voices[0], true, pair(1, 1));
    std::memset(&hooked, 0, sizeof hooked);
    std::memset(start, 0, sizeof start);
    mix(hooked, 5, voices, 1, start, g, true, false);
    for (u32 i = 0; i < kDrumSendWords; ++i) CHECK(!hooked.fx1[i] && !hooked.fx2[i]);
    CHECK(hooked.main[0] == q31::multiply(g.main, voices[0].audio[0]));
    // A stock voice on a track whose kit voice kept a share goes in unscaled.
    bind(voices[1], false, pair(5, 5));
    bind(voices[0], true, pair(2, 3));
    std::memset(&hooked, 0, sizeof hooked);
    mix(hooked, 6, voices, 2, start, g, true, false);
    const i32 kept = q31::add(q31::multiply(kDrumSendGains[3], voices[0].audio[9]), voices[1].audio[9]);
    CHECK(hooked.fx2[9] == q31::multiply(g.fx2, kept));
}

// A kept sum belongs to one track of one block: the next track, or the same
// track after a block stock skipped, reads stock's sum again.
void state_ends_with_its_track() {
    static StockVoice voices[2];
    static i32 start[kDrumSendWords];
    std::memset(start, 0, sizeof start);
    fill(voices[0].audio, 1 << 4); bind(voices[0], true, pair(3, 6));
    fill(voices[1].audio, 1 << 4); bind(voices[1], false, 0);
    const Gains g = {0x40000000, 0x20000000, 0, 0x30000000};
    static Buses stock_buses, hooked;
    std::memset(&stock_buses, 0, sizeof stock_buses);
    std::memset(&hooked, 0, sizeof hooked);
    mix(hooked, 1, voices, 1, start, g, true, true);       // a kit track stock skipped
    CHECK(sdk_drum_send_state.bus[0].track == 2 && sdk_drum_send_state.bus[1].track == 2);
    mix(stock_buses, 1, voices + 1, 1, start, g, false, false);
    mix(hooked, 1, voices + 1, 1, start, g, true, false);  // the same track, next block, a stock voice
    CHECK(same(stock_buses, hooked));
    // A stale state from another track never redirects this one's sends.
    sdk_drum_send_state.bus[1].track = 4;
    sdk_drum_send_state.bus[1].source = nullptr;
    CHECK(drum_send_source(sdk_drum_send_state, 1, 2, host_sum) == host_sum);
    CHECK(drum_send_source(sdk_drum_send_state, 0, 3, host_sum) == host_sum);
    CHECK(drum_send_source(sdk_drum_send_state, 1, 3, host_sum) == nullptr);
}

void codes_and_gains() {
    // Payload word 6 carries FX I's code in bits 25..27 and FX II's in 28..30,
    // only on a marked pad; FILTER and SRR below them stay as they are.
    const i32 marked = i32(42u * 32537631u + 16268815u);
    i32 payload[8] = {marked, 0, 0, 0, 0, 0, 0, 0};
    for (u32 fx1 = 0; fx1 < kDrumSendCodes; ++fx1)
        for (u32 fx2 = 0; fx2 < kDrumSendCodes; ++fx2) {
            payload[6] = drum_send_store(drum_send_store(0x01ab0000, 0, fx1), 1, fx2);
            CHECK(payload[6] > 0 || (!fx1 && !fx2 && payload[6] == 0x01ab0000));
            CHECK(drum_send_codes(payload) == pair(fx1, fx2));
            CHECK(drum_send_code(drum_send_codes(payload), 0) == fx1 && drum_send_code(drum_send_codes(payload), 1) == fx2);
            CHECK((payload[6] & 0x01ffffff) == 0x01ab0000);
            CHECK((static_cast<u32>(payload[6]) >> 25 & 7) == fx1 && (static_cast<u32>(payload[6]) >> 28 & 7) == fx2);
        }
    payload[0] = 0;
    CHECK(drum_send_codes(payload) == 0);
    CHECK(drum_send_codes(nullptr) == 0);
    payload[0] = marked;
    payload[6] = 0x01ff0000;           // FILTER and SRR set, both sends follow
    CHECK(drum_send_codes(payload) == 0);
    // Shares rise with the code, 0 for dry, all for following.
    CHECK(drum_send_gain(0) == q31::maximum && drum_send_gain(1) == 0);
    for (u32 code = 2; code < kDrumSendCodes; ++code) {
        const double share = double(drum_send_gain(code)) / 2147483648.0;
        CHECK(share > (code - 1) / 7.0 - 1e-9 && share < (code - 1) / 7.0 + 1e-9);
    }
}
}

int main() {
    codes_and_gains();
    stock_and_followers_are_unchanged();
    shares_route_each_voice();
    state_ends_with_its_track();
    std::puts("drum sends ok");
    return 0;
}
