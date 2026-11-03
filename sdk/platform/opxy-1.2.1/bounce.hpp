// Bounce, private: the parts of the real-time mix capture (phase 1, bounce.cpp)
// and the DSP's offline render (phase 2a, bounce_render.cpp) that need no stock
// call, so the host can test them (tests/fixtures/bounce.cpp). Their stock calls
// are in bounce_stock.hpp.
#pragma once
#include <opxy/types.hpp>

namespace opxy { namespace bounce {
// File-local in each unit that includes it: the capture and the render may link
// together, and staging has no COMDAT groups to merge an inline left out of line.
namespace {

// The block the main board hands USB audio: 64 frames of 8 words. Slots 0..5
// carry audio; 6 and 7 carry the DSP's message words (stock 8c0c69e4).
const u32 kFrames = 64;
const u32 kSlots = 8;
const u32 kAudioSlots = 6;

// One producer (the block hook) and one consumer (the writer task). Frames are
// a left and a right word. The capacity is a power of two; the indices only
// grow, so head - tail is the fill even across a 32-bit wrap.
struct Ring {
    u32* words;
    u32 capacity;  // frames
    volatile u32 head;
    volatile u32 tail;
    u32 dropped;    // blocks the writer had no room for
    u32 high;       // most frames ever waiting

    void reset(u32* storage, u32 frames) {
        words = storage;
        capacity = frames;
        head = tail = dropped = high = 0;
    }
    u32 fill() const { return head - tail; }
    // One block's slots 0 and 1; false when there was no room for all of it.
    bool push(const u32* block) {
        const u32 at = head;
        if (capacity - (at - tail) < kFrames) {
            ++dropped;
            return false;
        }
        const u32 mask = capacity - 1;
        for (u32 i = 0; i < kFrames; ++i) {
            u32* frame = words + 2 * ((at + i) & mask);
            frame[0] = block[kSlots * i];
            frame[1] = block[kSlots * i + 1];
        }
        return true;
    }
    // Published by the caller after its barrier, so the writer never reads a
    // frame before its words are stored.
    void publish() {
        head = head + kFrames;
        const u32 waiting = head - tail;
        if (waiting > high) high = waiting;
    }
};

// Each audio slot's largest magnitude over a take, so one take shows which slots
// carry the mix.
struct Peaks {
    u32 slot[kAudioSlots];
    void reset() { for (u32 i = 0; i < kAudioSlots; ++i) slot[i] = 0; }
    void add(const u32* block) {
        for (u32 i = 0; i < kFrames; ++i)
            for (u32 s = 0; s < kAudioSlots; ++s) {
                const i32 v = static_cast<i32>(block[kSlots * i + s]);
                const u32 m = v < 0 ? (v == static_cast<i32>(0x80000000u) ? 0x7fffffffu : static_cast<u32>(-v))
                                    : static_cast<u32>(v);
                if (m > slot[s]) slot[s] = m;
            }
    }
};

// 16-bit with TPDF dither: two uniform values of one 16-bit LSB each, their
// difference triangular over +-1 LSB. The words are Q31 (or 24-bit left
// justified, which reads the same).
struct Dither {
    u32 state;
    u32 next() {
        state = state * 1664525u + 1013904223u;
        return state >> 16;  // 0..65535: one LSB of the 16-bit result in Q31
    }
    short sample(u32 word) {
        const i32 noise = static_cast<i32>(next()) - static_cast<i32>(next());
        const i32 v = static_cast<i32>(word);
        i32 sum;
        if (__builtin_add_overflow(v, noise + 0x8000, &sum)) sum = v < 0 ? static_cast<i32>(0x80000000u) : 0x7fffffff;
        return static_cast<short>(sum >> 16);
    }
};

// 24-bit: the top 24 bits, rounded, saturating at full scale. No dither: the
// rounding error is below the DAC's noise floor.
inline i32 sample24(u32 word) {
    const i32 v = static_cast<i32>(word);
    i32 sum;
    if (__builtin_add_overflow(v, 0x80, &sum)) sum = 0x7fffffff;
    return sum >> 8;
}
// Little-endian 24-bit sample into three bytes.
inline void put24(unsigned char* p, i32 v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
}

// Little-endian WAV header of 44 bytes for 16- or 24-bit PCM.
inline void put16(unsigned char* p, u32 v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
inline void put32(unsigned char* p, u32 v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }
const u32 kHeaderBytes = 44;
const u32 kRate = 44100;
inline void wav_header(unsigned char* h, u32 frames, u32 channels, u32 bits) {
    const u32 block = bits / 8 * channels;
    const u32 data = frames * block;
    const char* tags[] = {"RIFF", "WAVE", "fmt ", "data"};
    const u32 at[] = {0, 8, 12, 36};
    for (u32 t = 0; t < 4; ++t)
        for (u32 i = 0; i < 4; ++i) h[at[t] + i] = static_cast<unsigned char>(tags[t][i]);
    put32(h + 4, 36 + data);
    put32(h + 16, 16);
    put16(h + 20, 1);
    put16(h + 22, channels);
    put32(h + 24, kRate);
    put32(h + 28, kRate * block);
    put16(h + 32, block);
    put16(h + 34, bits);
    put32(h + 40, data);
}

// "<prefix><three digits><suffix>" into out (at most size bytes, terminated);
// false when it does not fit or take is over 999.
inline bool take_path(char* out, u32 size, const char* prefix, u32 take, const char* suffix) {
    if (take > 999) return false;
    u32 n = 0;
    for (const char* p = prefix; *p; ++p) { if (n + 1 >= size) return false; out[n++] = *p; }
    const char digits[] = {static_cast<char>('0' + take / 100), static_cast<char>('0' + take / 10 % 10),
                           static_cast<char>('0' + take % 10)};
    for (u32 i = 0; i < 3; ++i) { if (n + 1 >= size) return false; out[n++] = digits[i]; }
    for (const char* p = suffix; *p; ++p) { if (n + 1 >= size) return false; out[n++] = *p; }
    out[n] = 0;
    return true;
}

// KiB per second from bytes and milliseconds, rounded down; 0 for no time.
// Whole KiB times 1000 fits 32 bits up to 4 GiB.
// Frames in n bars of 4/4 at a tempo in tenths of a BPM; 0 for no tempo. A bar
// is 4 * 60 * 10 / bpm_x10 seconds, so 105,840,000 / bpm_x10 frames, rounded
// down (under a frame a bar). 500 bars at 20 bpm stay inside 32 bits.
inline u32 bar_frames(u32 bars, u32 bpm_x10) { return bpm_x10 ? bars * (4u * 60u * 10u * kRate / bpm_x10) : 0; }

// What the transport's tick word does during a take, for the log only: the
// forward steps it took, how many steps jumped (more than kStep a block) and
// how many went backwards (a reset or a loop), and the largest forward step.
struct TickWatch {
    static const u32 kStep = 64;
    u32 last, counted, jumps, backwards, largest;
    bool begun;
    void reset() { last = counted = jumps = backwards = largest = 0; begun = false; }
    void step(u32 ticks) {
        if (!begun) {
            begun = true;
            last = ticks;
            return;
        }
        const u32 d = ticks - last;
        last = ticks;
        if (d >= 0x80000000u) {
            ++backwards;
            return;
        }
        if (d > largest) largest = d;
        if (d <= kStep) counted += d; else ++jumps;
    }
};

// Why a take ended.
enum Reason { kRunning = 0, kStopped, kBarsDone, kLengthCap, kBytesCap, kWriteShort };
inline const char* reason_name(u32 reason) {
    const char* names[] = {"running", "transport stop", "bars done", "20-minute cap", "boot byte cap", "short write"};
    return reason < 6 ? names[reason] : "?";
}

inline u32 kib_per_second(u32 bytes, u32 millis) { return millis ? (bytes >> 10) * 1000u / millis : 0; }

// ---- Phase 2a: the DSP's offline render (bounce_render.cpp) ----

// One engine block: 64 frames of left and right Q31 words, interleaved, as the
// audio block wrapper reads its output (11a02b4e).
const u32 kBlockWords = 2 * kFrames;

// Blocks covering a time in milliseconds, rounded up: ms * 44,100 / 1,000 / 64
// = ms * 441 / 640. Exact in 32 bits up to about 2.7 hours.
inline u32 blocks_for_millis(u32 millis) { return (millis * 441u + 639u) / 640u; }

// The first address at or after p that is a multiple of to (a power of two).
inline unsigned char* align_up(unsigned char* p, u32 to) {
    typedef __UINTPTR_TYPE__ Address;
    const Address a = reinterpret_cast<Address>(p);
    return reinterpret_cast<unsigned char*>((a + (to - 1)) & ~static_cast<Address>(to - 1));
}

// The file as it goes out: whole 64 KiB chunks from one 32-byte-aligned buffer,
// the WAV header first. Every write but the last is then a whole chunk at a
// chunk-aligned file offset, which FAT32 takes in whole sectors straight from the
// buffer (fat_diskio needs 32-byte alignment to skip its bounce copy). The header
// (44 bytes) and a 16-bit stereo frame (4) both divide the chunk, so a frame never
// straddles two chunks.
const u32 kChunkBytes = 64u << 10;
const u32 kChunkAlign = 32;
struct Chunk {
    unsigned char* base;
    u32 used;     // bytes in the buffer now
    u32 written;  // bytes handed out in whole or final chunks so far
    void reset(unsigned char* buffer) { base = buffer; used = written = 0; }
    bool full() const { return used == kChunkBytes; }
    // The header's place, zeroed, at the start of the file.
    void begin() {
        for (u32 i = 0; i < kHeaderBytes; ++i) base[i] = 0;
        used = kHeaderBytes;
    }
    // One frame; the caller writes a full chunk out first.
    void frame16(short left, short right) {
        unsigned char* p = base + used;
        put16(p, static_cast<u16>(left));
        put16(p + 2, static_cast<u16>(right));
        used += 4;
    }
    // After the caller wrote used bytes out.
    void drained() { written += used; used = 0; }
    // Frames in the file so far, written or buffered.
    u32 frames() const { return (written + used - kHeaderBytes) / 4; }
};

// Peak and energy of one channel over a take. The energy is the sum of the
// squares of the top 16 bits, kept as two words (no 64-bit runtime on the
// device); the log reader turns it into RMS dBFS.
struct Level {
    u32 peak;      // largest magnitude, Q31
    u32 low, high; // sum of squares of (word >> 16)
    void reset() { peak = low = high = 0; }
    void add(u32 word) {
        const i32 v = static_cast<i32>(word);
        const u32 m = v < 0 ? (v == static_cast<i32>(0x80000000u) ? 0x7fffffffu : static_cast<u32>(-v)) : static_cast<u32>(v);
        if (m > peak) peak = m;
        const i32 top = v >> 16;
        const u32 square = static_cast<u32>(top * top);  // at most 2^30
        const u32 sum = low + square;
        if (sum < low) ++high;
        low = sum;
    }
};

// FNV-1a over the engine's words, so two takes of the same seeds can be compared
// from the log alone.
struct Hash {
    u32 value;
    void reset() { value = 2166136261u; }
    void add(u32 word) {
        for (u32 i = 0; i < 4; ++i) {
            value ^= (word >> (8 * i)) & 0xff;
            value *= 16777619u;
        }
    }
};

// Cycles in a microsecond and a millisecond, from cycles in a block measured
// against the SPORT's block counter (a block is 64 / 44,100 s = 1,451.25 us).
// Fits 32 bits up to 9.7 million cycles a block.
inline u32 cycles_per_milli(u32 per_block) { return per_block * 441u / 640u; }
inline u32 cycles_per_micro(u32 per_block) { return cycles_per_milli(per_block) / 1000u; }
// Percent of a block that `units` of 256 cycles over `blocks` blocks took.
inline u32 block_percent(u32 units, u32 blocks, u32 per_block) {
    if (!blocks || per_block < 256) return 0;
    return units / blocks * 100u / (per_block >> 8);
}
// Real-time factor times 100: rendered blocks over the blocks the SPORT counted meanwhile.
inline u32 speed_x100(u32 rendered, u32 elapsed) { return elapsed ? rendered * 100u / elapsed : 0; }
}
}}
