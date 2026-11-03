// Bounce phase 1, private test firmware only (OP-XY-Dev drafts/bounce-stems.md):
// a real-time capture of the mix as USB audio receives it, written to
// /fat32/samples/bounce/test NNN/mix.wav. A take starts at play and ends after
// kBars bars at the project's tempo when it started, or at stop if that comes
// first (kBars 0: at stop only). Stop is the play flag clear for kQuietBlocks
// blocks in a row: on bounce-002 a take ended at every pattern loop of 4 bars.
//
// The main board's per-block SPORT callback (8c0c69e4) hands USB audio
// (8d4d4e14) the block the DSP sent, 64 frames of 8 words. The hook replaces
// that CALL: it calls USB audio as before, then copies slots 0 and 1 into a ring
// while a take is armed. A writer task of its own, below stock's I/O tasks,
// drains the ring to a .tmp file as 16-bit TPDF-dithered stereo, writes the
// header and renames the file to .wav at stop. Each take ends with a log line
// of its length, drops, ring high-water, write rate and slot peaks, then a
// 4 MiB burst write that measures what FAT32 sustains. At most kMaxTakes takes
// and kMaxBytes a boot, and none after a short write. Main board only:
// sdk_bounce_startup runs on the GUI role, and the hook does nothing until it has.
#include "barrier.hpp"
#include "bounce.hpp"
#include "stock.hpp"

using opxy::i32;
using opxy::u16;
using opxy::u32;
using opxy::platform::barrier;
using opxy::platform::byte;
using opxy::platform::kPlaying;
using opxy::platform::stock;
using namespace opxy::bounce;

extern "C" {
void sdk_bounce_block(const u32* block) __asm__("sdk_bounce_block");
void sdk_bounce_startup() __asm__("sdk_bounce_startup");
u32 sdk_benchmark_cycles() __asm__("sdk_benchmark_cycles");
}

namespace {
typedef void File;
// newlib as stock's project files call it (project_files.hpp).
File* file_open(const char* path, const char* mode) { return stock<File*(const char*, const char*)>(0x8fed0348)(path, mode); }
u32 file_write(const void* data, u32 size, u32 count, File* file) {
    return stock<u32(const void*, u32, u32, File*)>(0x8fed0780)(data, size, count, file);
}
void file_close(File* file) { stock<int(File*)>(0x8fed00ae)(file); }
// mkdir(path, mode) as stock's folder setup calls it (8c0409f0): -1 when it fails,
// which includes a folder that already exists.
i32 make_folder(const char* path) { return stock<i32(const char*, u32)>(0x8c0bd5bc)(path, 0); }
// Stock's rename wrapper (8c040bd8): 1 when it moved the file.
bool rename_file(const char* from, const char* to) { return stock<u32(const char*, const char*)>(0x8c040bd8)(from, to) != 0; }
// Stock's remove wrapper (8c040c24), as its replace-by-rename calls it first.
void remove_file(const char* path) { stock<u32(const char*)>(0x8c040c24)(path); }
// Stock's log, as sport.cpp calls it: level 3 prints an "I" line in /logs.
template <class... A> void log(u32 line, const char* format, A... args) {
    stock<void(u32, const char*, u32, const char*, ...)>(0x8c042efc)(3, "bounce.cpp", line, format, args...);
}
// The stock heap, as the benchmark diagnostic takes from it (benchmark.cpp.in).
void* allocate(u32 bytes) {
    const u32 heap = 0x08036aa4;
    stock<void(u32, i32)>(0x8c03e6f4)(opxy::platform::word(heap), -1);
    void* block = stock<void*(u32, u32)>(0x8c048788)(opxy::platform::word(heap + 4), bytes);
    stock<void(u32, u32, u32, u32)>(0x8c03e9c8)(opxy::platform::word(heap), 0, 0, 0);
    return block;
}
void release(void* block) { stock<void(void*)>(0x8d4d2910)(block); }
// A binary semaphore no one gives: taking it with a timeout is the task's sleep.
u32 create_semaphore() { return stock<u32(u32, u32, u32)>(0x8c03de70)(1, 0, 3); }
void sleep_ticks(u32 semaphore, u32 ticks) { stock<u32(u32, u32)>(0x8c03e6f4)(semaphore, ticks); }
// Stock's task creation, as spibridge_io_task is created (8c0c0cb8); 0 on success.
u32 create_task(u32* handle, void (*entry)(void*), u32 priority, u32 stack, const char* name) {
    return stock<u32(u32*, void (*)(void*), void*, u32, u32, u32, const char*, u32)>(0x8d5bdf44)(
        handle, entry, nullptr, priority, 0, stack, name, 1);
}
void usb_audio(const u32* block) { stock<void(const u32*)>(0x8d4d4e14)(block); }

bool playing() { return byte(kPlaying) || byte(kPlaying + 1); }
u32 ticks() { return opxy::platform::word(opxy::platform::kTicks96); }
const u32 kQuietBlocks = 16;                  // 23 ms of a clear play flag is a stop

// The test's settings, fixed at compile time.
const u32 kBars = 64;                         // 4/4 bars from play; about 2:08 at 120 bpm. 0: play to stop
const u32 kBits = 16;                         // 16 (TPDF dither, Max's default) or 24
const u32 kMaxTakes = 6;                      // takes a boot; then the capture stays off until restart
const u32 kMaxBytes = 512u << 20;             // audio bytes a boot, across takes
static_assert(kBits == 16 || kBits == 24, "16- or 24-bit takes only");
const u32 kFrameBytes = 2 * kBits / 8;

const u32 kRingFrames = 1u << 16;             // 1.49 s at 44.1 kHz, 512 KiB
const u32 kChunkFrames = 2048;                // 8 KiB of 16-bit stereo a write
const u32 kMaxFrames = 20u * 60u * kRate;     // a take stops by itself after 20 minutes, whatever kBars says
const u32 kBurstBytes = 4u << 20;
const u32 kCyclesPerMicro = 400;              // the main board's core clock (stock's µs clock divides by 400)
const u32 kPriority = 2;                      // below jack_task (4) and spibridge_io_task (5)

// Shared between the hook and the writer task. armed is set by the writer once
// the ring is ready; the hook clears capturing at the first block after stop.
Ring ring;
Peaks peaks;
volatile u32 started;    // 1 once the writer task runs
volatile u32 armed;      // the hook may push
volatile u32 stopped;    // the take ends: a Reason, set by the hook or the writer
u32 target_frames;       // frames in kBars bars at the take's tempo; 0: until stop
u32 tempo_x10;           // that tempo
u32 quiet;               // blocks in a row with the play flag clear
u32 flickers;            // runs of a clear play flag shorter than a stop
u32 start_ticks;         // the tick word at the first block
TickWatch watch;
u32 takes;               // takes this boot
u32 bytes_written;       // audio bytes this boot
bool full;               // a cap is reached: no more takes until restart
u32 task_handle;
u32 semaphore;

unsigned char chunk[kFrameBytes * kChunkFrames];
unsigned char header[kHeaderBytes];
char folder[96];
char temporary[112];
char final_path[112];

struct Take {
    u32 number;
    File* file;
    u32 frames;
    u32 write_micros;
    u32 worst_write_micros;
    u32 writes;
    Dither dither;
    bool failed;  // a write came up short (storage full?)
} take;

u32 micros_since(u32 start) { return (sdk_benchmark_cycles() - start) / kCyclesPerMicro; }

// The next free "test NNN" folder under /fat32/samples/bounce; 0 when none.
u32 next_folder() {
    make_folder("/fat32/samples/bounce");
    for (u32 n = 1; n <= 999; ++n) {
        if (!take_path(folder, sizeof folder, "/fat32/samples/bounce/test ", n, "")) return 0;
        if (make_folder(folder) >= 0) return n;
    }
    return 0;
}

bool begin() {
    void* storage = allocate(kRingFrames * 2 * sizeof(u32));
    if (!storage) {
        log(__LINE__, "no memory for the capture ring");
        return false;
    }
    take.number = next_folder();
    if (!take.number || !take_path(temporary, sizeof temporary, "/fat32/samples/bounce/test ", take.number, "/mix.tmp") ||
        !take_path(final_path, sizeof final_path, "/fat32/samples/bounce/test ", take.number, "/mix.wav") ||
        !(take.file = file_open(temporary, "wb"))) {
        log(__LINE__, "could not open a take file (take %d)", static_cast<int>(take.number));
        release(storage);
        return false;
    }
    wav_header(header, 0, 2, kBits);
    file_write(header, 1, kHeaderBytes, take.file);
    take.frames = take.write_micros = take.worst_write_micros = take.writes = 0;
    take.failed = false;
    ++takes;
    take.dither.state = 0x9e3779b9u ^ take.number;
    ring.reset(static_cast<u32*>(storage), kRingFrames);
    peaks.reset();
    tempo_x10 = opxy::platform::project_bpm_x10();
    target_frames = bar_frames(kBars, tempo_x10);
    quiet = flickers = 0;
    watch.reset();
    stopped = kRunning;
    barrier();
    armed = 1;
    log(__LINE__, "take %d recording to %s, %d-bit, %d bars at %d.%d bpm = %d frames", static_cast<int>(take.number),
        temporary, static_cast<int>(kBits), static_cast<int>(kBars), static_cast<int>(tempo_x10 / 10),
        static_cast<int>(tempo_x10 % 10), static_cast<int>(target_frames));
    return true;
}

// Writes what the ring holds, a chunk at a time; returns the frames written.
u32 drain() {
    u32 written = 0;
    for (;;) {
        u32 count = ring.fill();
        if (!count) return written;
        if (count > kChunkFrames) count = kChunkFrames;
        const u32 mask = ring.capacity - 1;
        for (u32 i = 0; i < count; ++i) {
            const u32* frame = ring.words + 2 * ((ring.tail + i) & mask);
            unsigned char* out = chunk + kFrameBytes * i;
            for (u32 c = 0; c < 2; ++c) {
                if (kBits == 24) {
                    put24(out + 3 * c, sample24(frame[c]));
                } else {
                    put16(out + 2 * c, static_cast<u16>(take.dither.sample(frame[c])));
                }
            }
        }
        barrier();
        ring.tail = ring.tail + count;
        const u32 start = sdk_benchmark_cycles();
        const u32 stored = file_write(chunk, kFrameBytes, count, take.file);
        const u32 spent = micros_since(start);
        take.write_micros += spent;
        if (spent > take.worst_write_micros) take.worst_write_micros = spent;
        ++take.writes;
        take.frames += stored;
        written += stored;
        bytes_written += stored * kFrameBytes;
        if (stored != count) {
            take.failed = true;
            stopped = kWriteShort;
            return written;
        }
        if (stopped) continue;
        if (take.frames >= kMaxFrames) stopped = kLengthCap;
        else if (bytes_written >= kMaxBytes) stopped = kBytesCap;
    }
}

// Sustained FAT32 writes, as fast as the task runs, of kBurstBytes in chunks.
void burst() {
    if (!take_path(temporary, sizeof temporary, "/fat32/samples/bounce/test ", take.number, "/burst.tmp")) return;
    File* file = file_open(temporary, "wb");
    if (!file) return;
    u32 micros = 0;
    for (u32 done = 0; done < kBurstBytes; done += sizeof chunk) {
        const u32 start = sdk_benchmark_cycles();
        file_write(chunk, 1, sizeof chunk, file);
        micros += micros_since(start);
    }
    const u32 start = sdk_benchmark_cycles();
    file_close(file);
    micros += micros_since(start);
    remove_file(temporary);
    log(__LINE__, "burst %d KiB in %d ms: %d KiB/s", static_cast<int>(kBurstBytes >> 10), static_cast<int>(micros / 1000),
        static_cast<int>(kib_per_second(kBurstBytes, micros / 1000)));
}

void finish() {
    armed = 0;
    barrier();
    drain();
    file_close(take.file);
    wav_header(header, take.frames, 2, kBits);
    // r+b opens at the start without truncating, so the header overwrites the placeholder.
    File* file = file_open(temporary, "r+b");
    bool headed = false;
    if (file) {
        headed = file_write(header, 1, kHeaderBytes, file) == kHeaderBytes;
        file_close(file);
    }
    const bool renamed = rename_file(temporary, final_path);
    const u32 bytes = take.frames * kFrameBytes;
    log(__LINE__, "take %d: %d frames (%d ms), header %d, renamed %d, write failed %d", static_cast<int>(take.number),
        static_cast<int>(take.frames), static_cast<int>(take.frames / 441 * 10), headed ? 1 : 0, renamed ? 1 : 0,
        take.failed ? 1 : 0);
    log(__LINE__, "take %d: dropped %d blocks, ring high %d of %d frames", static_cast<int>(take.number),
        static_cast<int>(ring.dropped), static_cast<int>(ring.high), static_cast<int>(ring.capacity));
    log(__LINE__, "take %d: %d writes, %d ms writing, worst %d us, %d KiB/s while writing", static_cast<int>(take.number),
        static_cast<int>(take.writes), static_cast<int>(take.write_micros / 1000), static_cast<int>(take.worst_write_micros),
        static_cast<int>(kib_per_second(bytes, take.write_micros / 1000)));
    log(__LINE__, "take %d: slot peaks %x %x %x %x %x %x", static_cast<int>(take.number), peaks.slot[0], peaks.slot[1],
        peaks.slot[2], peaks.slot[3], peaks.slot[4], peaks.slot[5]);
    log(__LINE__, "take %d: ended by %s; play-flag flickers %d", static_cast<int>(take.number), reason_name(stopped),
        static_cast<int>(flickers));
    log(__LINE__, "take %d: ticks %x to %x, %d counted, %d jumps, %d backwards, largest step %d", static_cast<int>(take.number),
        start_ticks, watch.last, static_cast<int>(watch.counted), static_cast<int>(watch.jumps),
        static_cast<int>(watch.backwards), static_cast<int>(watch.largest));
    release(ring.words);
    ring.words = nullptr;
    if (!take.failed) burst();
    if (takes >= kMaxTakes || bytes_written >= kMaxBytes || take.failed) {
        full = true;
        log(__LINE__, "capture off until restart: %d takes, %d KiB this boot", static_cast<int>(takes),
            static_cast<int>(bytes_written >> 10));
    }
}

// Until the transport is stopped: three polls in a row with the play flag clear,
// so a pattern loop's clear block does not start the next take.
void wait_for_stop() {
    for (u32 clear = 0; clear < 3;) {
        sleep_ticks(semaphore, 5);
        clear = playing() ? 0 : clear + 1;
    }
}

void writer(void*) {
    for (;;) {
        sleep_ticks(semaphore, 2);
        if (!armed) {
            if (!full && playing()) {
                if (!begin()) wait_for_stop();  // one attempt per play
            }
            continue;
        }
        drain();
        if (stopped) {
            finish();
            wait_for_stop();  // a take that ended on its bars or a cap waits for stop
        }
    }
}
}

// Replaces 8c0c6a3a's CALL 8d4d4e14 (USB audio) in the main board's SPORT
// callback; block is that call's only argument.
void sdk_bounce_block(const u32* block) {
    usb_audio(block);
    if (!armed || stopped) return;
    // A clear play flag for a block or two (a pattern loop) is not a stop: keep
    // the audio, and stop only after kQuietBlocks in a row.
    if (!playing()) {
        if (++quiet >= kQuietBlocks) {
            stopped = kStopped;
            return;
        }
    } else if (quiet) {
        ++flickers;
        quiet = 0;
    }
    if (!watch.begun) start_ticks = ticks();
    watch.step(ticks());
    if (target_frames && ring.head + ring.dropped * kFrames >= target_frames) {
        stopped = kBarsDone;
        return;
    }
    peaks.add(block);
    if (ring.push(block)) {
        barrier();
        ring.publish();
    }
}

void sdk_bounce_startup() {
    if (started) return;
    semaphore = create_semaphore();
    if (!semaphore) return;
    if (create_task(&task_handle, writer, kPriority, 0x4000, "bounce_task") != 0) {
        log(__LINE__, "could not start the bounce task");
        return;
    }
    started = 1;
    log(__LINE__, "bounce capture ready: %d-bit, %d bars from play (or to stop), up to %d takes a boot",
        static_cast<int>(kBits), static_cast<int>(kBars), static_cast<int>(kMaxTakes));
}
