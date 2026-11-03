// The bounce capture's ring, peaks, dither, WAV header and take paths, and the
// DSP render's chunk packing, alignment, levels, hash and frame arithmetic, with
// no stock call (bounce.hpp).
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

static u32 le32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<u32>(p[3]) << 24; }

// Phase 2a: the DSP render's pure parts.
static void render_checks() {
    // Blocks for the take: 20 s held, 5 s tail; rounded up to whole blocks.
    require(blocks_for_millis(20000) == 13782 && blocks_for_millis(5000) == 3446);
    require(blocks_for_millis(0) == 0 && blocks_for_millis(1) == 1 && blocks_for_millis(1000) == 690);
    require(blocks_for_millis(120000) == 82688);
    const u32 frames = (13782 + 3446) * kFrames;
    require(frames == 1102592);
    // No overflow at the longest a take could be asked for (two hours).
    require(blocks_for_millis(7200000u) == 4961250);
    // The diagnostic build's wait before the first take, and its 1.5 s wait after a step line.
    require(blocks_for_millis(1500) == 1034);

    // Step lines after blocks 1, 16 and 256, then every 2048; a take's 17,228 blocks log 11.
    require(!render_checkpoint(0) && render_checkpoint(1) && !render_checkpoint(2));
    require(render_checkpoint(16) && !render_checkpoint(17) && render_checkpoint(256) && !render_checkpoint(512));
    require(render_checkpoint(2048) && !render_checkpoint(2049) && render_checkpoint(4096) && render_checkpoint(16384));
    u32 checkpoints = 0;
    for (u32 b = 1; b <= 13782 + 3446; ++b) checkpoints += render_checkpoint(b) ? 1 : 0;
    require(checkpoints == 11);

    // A 32-byte-aligned chunk inside a slightly larger allocation, for any start.
    static unsigned char raw[kChunkBytes + 2 * kChunkAlign];
    for (u32 skew = 0; skew < kChunkAlign; ++skew) {
        unsigned char* p = align_up(raw + skew, kChunkAlign);
        typedef __UINTPTR_TYPE__ Address;
        require(reinterpret_cast<Address>(p) % kChunkAlign == 0);
        require(p >= raw + skew && p < raw + skew + kChunkAlign);
        require(p + kChunkBytes <= raw + sizeof raw);
    }
    unsigned char* aligned = align_up(raw, kChunkAlign);
    require(align_up(aligned, kChunkAlign) == aligned);

    // The file through the chunk: header first, every write but the last a whole
    // chunk; frames never straddle, and the byte count matches the header.
    static unsigned char file[600000];
    u32 size = 0, writes = 0, whole = 0;
    Chunk chunk;
    chunk.reset(aligned);
    chunk.begin();
    require(chunk.used == kHeaderBytes && chunk.frames() == 0);
    Dither dither;
    dither.state = 0x9e3779b9u;
    Dither check = dither;
    const u32 count = 140000;  // over two chunks
    for (u32 i = 0; i < count; ++i) {
        if (chunk.full()) {
            std::memcpy(file + size, chunk.base, chunk.used);
            size += chunk.used;
            ++writes;
            ++whole;
            chunk.drained();
        }
        require(chunk.used + 4 <= kChunkBytes);
        const u32 l = static_cast<u32>(i * 40503u) << 8, r = ~l;
        chunk.frame16(dither.sample(l), dither.sample(r));
    }
    require(chunk.frames() == count);
    require(whole == (kHeaderBytes + 4 * count) / kChunkBytes);
    std::memcpy(file + size, chunk.base, chunk.used);
    size += chunk.used;
    ++writes;
    chunk.drained();
    require(size == kHeaderBytes + 4 * count && chunk.frames() == count && chunk.written == size && writes == whole + 1);
    // The first chunk holds (65,536 - 44) / 4 = 16,373 frames exactly.
    require((kChunkBytes - kHeaderBytes) % 4 == 0 && (kChunkBytes - kHeaderBytes) / 4 == 16373);
    // The samples come out in order, as the dither made them.
    for (u32 i = 0; i < count; ++i) {
        const u32 l = static_cast<u32>(i * 40503u) << 8, r = ~l;
        const short el = check.sample(l), er = check.sample(r);
        const unsigned char* p = file + kHeaderBytes + 4 * i;
        require(static_cast<short>(p[0] | p[1] << 8) == el && static_cast<short>(p[2] | p[3] << 8) == er);
    }
    // The header patch: 44 bytes over the zeroed placeholder.
    for (u32 i = 0; i < kHeaderBytes; ++i) require(file[i] == 0);
    wav_header(file, chunk.frames(), 2, 16);
    require(le32(file + 40) == 4 * count && le32(file + 4) == size - 8);

    // Levels: peak magnitude in Q31; the energy of the top 16 bits as two words.
    Level level;
    level.reset();
    level.add(0x40000000u);
    level.add(static_cast<u32>(-0x20000000));
    require(level.peak == 0x40000000u && level.high == 0 && level.low == 0x10000000u + 0x4000000u);
    level.add(0x80000000u);
    require(level.peak == 0x7fffffffu && level.low == 0x10000000u + 0x4000000u + 0x40000000u);
    // A carry into the high word: full scale for 8 frames is 8 * 2^30 = 2^33.
    level.reset();
    for (u32 i = 0; i < 8; ++i) level.add(0x80000000u);
    require(level.high == 2 && level.low == 0);
    for (u32 i = 0; i < 4; ++i) level.add(0x80000000u);
    require(level.high == 3 && level.low == 0);

    // FNV-1a over the words' little-endian bytes.
    Hash hash;
    hash.reset();
    hash.add(0);
    require(hash.value == 0x4b95f515u);
    hash.reset();
    hash.add(0x12345678u);
    hash.add(0xdeadbeefu);
    require(hash.value == 0x2ca9bf8bu);

    // Clock arithmetic: 400 MHz is 580,500 cycles a block.
    require(cycles_per_milli(580500) == 400000 && cycles_per_micro(580500) == 400);
    require(cycles_per_milli(1451000) < 0xffffffffu / 441u * 640u);
    // 25% of a block over 100 blocks, in 256-cycle units.
    require(block_percent(100 * (580500 / 4 / 256), 100, 580500) == 24 ||
            block_percent(100 * (580500 / 4 / 256), 100, 580500) == 25);
    require(block_percent(5, 0, 580500) == 0 && block_percent(5, 1, 0) == 0);
    require(speed_x100(17228, 5743) == 299 && speed_x100(1, 0) == 0);

    // The halfword block counter, widened: steps add, a wrap past 0xffff carries.
    Counter16 counter;
    counter.reset();
    require(counter.read(0xfff0) == 0);            // the first read only primes
    require(counter.read(0xfff8) == 8);
    require(counter.read(0x0004) == 20);           // wrapped: 8 + 12
    require(counter.read(0x10004) == 20);          // only the low half counts
    require(counter.read(0x8003) == 20 + 0x7fff);
    counter.reset();
    require(counter.read(0x1234) == 0 && counter.read(0x1233) == 0xffff);  // a full lap less one
    // Ticks to blocks: 1 ms ticks of 400,000 cycles, 580,500-cycle blocks.
    require(blocks_for_ticks(1000, 400000, 580500) == 689);
    require(blocks_for_ticks(120000, 400000, 580500) == 82681);
    require(blocks_for_ticks(5, 400000, 0) == 0 && blocks_for_ticks(1000000, 1000000, 1451000) == 689253);
    require(nominal_per_block(400000) == 580499 && nominal_per_block(999999) == 1451246);
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
    render_checks();
    return 0;
}
