// Bounce phase 1, private: the parts of the real-time mix capture that need no
// stock call, so the host can test them (tests/fixtures/bounce.cpp). The device
// side, its hook and writer task, is bounce.cpp.
#pragma once
#include <opxy/types.hpp>

namespace opxy { namespace bounce {

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

// Little-endian WAV header of 44 bytes for 16-bit PCM.
inline void put16(unsigned char* p, u32 v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
inline void put32(unsigned char* p, u32 v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }
const u32 kHeaderBytes = 44;
const u32 kRate = 44100;
inline void wav_header(unsigned char* h, u32 frames, u32 channels) {
    const u32 block = 2 * channels;
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
    put16(h + 34, 16);
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
inline u32 kib_per_second(u32 bytes, u32 millis) { return millis ? (bytes >> 10) * 1000u / millis : 0; }
}}
