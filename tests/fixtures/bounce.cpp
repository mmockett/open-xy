// The bounce capture's ring, peaks, dither, WAV header and take paths, with no
// stock call (bounce.hpp).
#include "bounce.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define require(v) do { if (!(v)) { std::fprintf(stderr, "line %d\n", __LINE__); std::abort(); } } while (0)
using namespace opxy;
using namespace opxy::bounce;

static u32 storage[2 * 256];
static u32 block[kFrames * kSlots];

static void fill_block(u32 base) {
    for (u32 i = 0; i < kFrames; ++i)
        for (u32 s = 0; s < kSlots; ++s) block[kSlots * i + s] = base + 16 * i + s;
}

int main() {
    // Slots 0 and 1 of each frame, in order, across the wrap of a 256-frame ring.
    Ring ring;
    ring.reset(storage, 256);
    u32 expected = 0, read = 0;
    for (u32 b = 0; b < 40; ++b) {
        fill_block(b << 12);
        require(ring.push(block));
        ring.publish();
        while (ring.fill() > 100) {
            const u32* frame = ring.words + 2 * (ring.tail & 255);
            const u32 i = read % kFrames, base = (read / kFrames) << 12;
            require(frame[0] == base + 16 * i && frame[1] == base + 16 * i + 1);
            ring.tail = ring.tail + 1;
            ++read;
        }
        expected += kFrames;
    }
    require(ring.head == expected && ring.dropped == 0 && ring.high == 164);

    // A full ring drops whole blocks and keeps what it has.
    ring.reset(storage, 256);
    for (u32 b = 0; b < 5; ++b) {
        if (ring.push(block)) ring.publish();
    }
    require(ring.fill() == 256 && ring.dropped == 1);
    ring.tail = ring.tail + 63;  // one frame short of a block
    require(!ring.push(block) && ring.dropped == 2);
    ring.tail = ring.tail + 1;
    require(ring.push(block));
    ring.publish();
    require(ring.fill() == 256);

    // The indices only grow: the fill is right across a 32-bit wrap.
    ring.reset(storage, 256);
    ring.head = ring.tail = 0xffffffc0u;
    require(ring.push(block));
    ring.publish();
    require(ring.head == 0 && ring.fill() == 64);

    // Peaks per audio slot; the message slots are left out.
    Peaks peaks;
    peaks.reset();
    for (u32 i = 0; i < kFrames * kSlots; ++i) block[i] = 0;
    block[kSlots * 3 + 0] = static_cast<u32>(-1000);
    block[kSlots * 5 + 1] = 0x80000000u;
    block[kSlots * 7 + 5] = 77;
    block[kSlots * 9 + 6] = 0x7fffffff;
    peaks.add(block);
    require(peaks.slot[0] == 1000 && peaks.slot[1] == 0x7fffffff && peaks.slot[5] == 77 && peaks.slot[2] == 0);

    // TPDF dither: silence stays within one LSB, its mean near zero; full scale
    // saturates instead of wrapping; a mid-level word lands within one LSB.
    Dither dither;
    dither.state = 1;
    i32 sum = 0;
    for (u32 i = 0; i < 100000; ++i) {
        const short s = dither.sample(0);
        require(s >= -1 && s <= 1);
        sum += s;
    }
    require(sum > -2000 && sum < 2000);
    for (u32 i = 0; i < 1000; ++i) {
        require(dither.sample(0x7fffffffu) >= 32766);
        require(dither.sample(0x80000000u) <= -32767);
        const short m = dither.sample(0x40000000u);
        require(m >= 16383 && m <= 16385);
    }

    // 24-bit: rounded, saturating, little-endian.
    require(sample24(0x7fffffffu) == 0x7fffff && sample24(0x80000000u) == -0x800000);
    require(sample24(0x00000180u) == 2 && sample24(0x0000017fu) == 1 && sample24(static_cast<u32>(-0x180)) == -1);
    unsigned char b[3];
    put24(b, -2);
    require(b[0] == 0xfe && b[1] == 0xff && b[2] == 0xff);

    // The 44-byte header of 16-bit stereo at 44.1 kHz.
    unsigned char h[kHeaderBytes];
    wav_header(h, 44100, 2, 16);
    require(!std::memcmp(h, "RIFF", 4) && !std::memcmp(h + 8, "WAVEfmt ", 8) && !std::memcmp(h + 36, "data", 4));
    const u32 data = h[40] | h[41] << 8 | h[42] << 16 | static_cast<u32>(h[43]) << 24;
    const u32 riff = h[4] | h[5] << 8 | h[6] << 16 | static_cast<u32>(h[7]) << 24;
    require(data == 176400 && riff == 36 + 176400);
    require(h[20] == 1 && h[22] == 2 && h[32] == 4 && h[34] == 16);
    require((h[24] | h[25] << 8 | h[26] << 16) == 44100 && (h[28] | h[29] << 8 | h[30] << 16) == 176400);
    wav_header(h, 100, 2, 24);
    require((h[40] | h[41] << 8) == 600 && h[32] == 6 && h[34] == 24 && (h[28] | h[29] << 8 | h[30] << 16) == 264600);
    // 64 bars at 120 bpm are 128 s; no tempo, no limit; slow tempos fit 32 bits.
    require(bar_frames(64, 1200) == 64 * 88200 && bar_frames(64, 0) == 0);
    require(bar_frames(500, 200) == 500u * 529200u && bar_frames(1, 1333) == 79399);

    // Ticks: forward steps counted, jumps and resets (a pattern loop) only logged.
    TickWatch watch;
    watch.reset();
    watch.step(1000);
    watch.step(1001);
    watch.step(1003);
    watch.step(1500);  // a jump
    watch.step(10);    // a loop back
    watch.step(12);
    require(watch.counted == 5 && watch.jumps == 1 && watch.backwards == 1 && watch.largest == 497 && watch.last == 12);
    require(!std::strcmp(reason_name(kBarsDone), "bars done") && !std::strcmp(reason_name(9), "?"));

    // Take paths: three digits, bounded, nothing past the buffer.
    char path[40];
    require(take_path(path, sizeof path, "/fat32/samples/bounce/test ", 7, "/mix.wav") &&
            !std::strcmp(path, "/fat32/samples/bounce/test 007/mix.wav"));
    require(take_path(path, sizeof path, "a", 999, "") && !std::strcmp(path, "a999"));
    require(!take_path(path, sizeof path, "a", 1000, ""));
    char tiny[8];
    require(!take_path(tiny, sizeof tiny, "test ", 1, "/x") && take_path(tiny, sizeof tiny, "tes ", 1, ""));

    require(kib_per_second(1 << 20, 1000) == 1024 && kib_per_second(5, 0) == 0);
    require(kib_per_second(0xffffffffu, 1000) == 4194303);
    return 0;
}
