// Bounce phase 2a, private test firmware only (OP-XY-Dev drafts/bounce-stems.md §7):
// the DSP renders a held chord faster than real time, silently, and writes it to
// /fat32/samples/bounce/render NNN/mix.wav on its own storage.
//
// A task of its own at priority 3 (below spibridge_io_task at 5 and the comm
// tasks at 6) waits 120 s after it starts, measures the clock, then runs
// kTakes takes with the same seeds. Each take:
//   1. sets audio mode 2, in which the audio interrupt still counts its block and
//      sends silence but renders nothing, and waits a settle pause;
//   2. seeds the DSP's random generator;
//   3. plays a chord on track 0 through stock's note-on, as its benchmark does;
//   4. renders blocks itself with the scheduler suspended around each engine call,
//      so no task changes voices in the middle of a block (interrupts still run),
//      sleeping a tick every kYieldBlocks blocks;
//   5. packs each block to 16-bit TPDF stereo in a 32-byte-aligned 64 KiB chunk
//      that holds the WAV header first, and writes whole chunks to mix.tmp;
//   6. releases the chord after kHoldMillis and renders kTailMillis more;
//   7. restores mode 0 straight after the last block, then patches the header and
//      renames mix.tmp to mix.wav;
//   8. writes a 4 MiB burst in whole chunks and removes it.
// Nothing crosses the bridge but the log lines. The service links only with
// OPXY_BOUNCE_RENDER=1 and starts from the audio role's startup (sdk_startup_synth).
//
// Clocks (bounce-2a-003). bounce-2a-002 logged its first line and then nothing for
// 13 minutes, in its 120 s wait on stock's block counter. In stock that counter
// (84a50b98) is counted first thing by every call of the L1 block wrapper, in every
// mode, and every live block goes through it; but it is a halfword that wraps every
// 95 s, which 002 read as a word (a wrap would have ended its wait early, not held
// it). So the wait never saw the counter move, or the task never woke from its
// semaphore sleep; stock alone doesn't say which. Every wait now counts FreeRTOS
// ticks (the core timer's 400,000 cycles; 1 ms at 400 MHz) and sleeps with
// vTaskDelay, as the DSP's own main_task does each pass; the block counter is
// widened from 16 bits (Counter16) and only logged and measured. The idle window
// logs a line every 10 s with both clocks, so the next log tells the two apart.
//
// Diagnostics (bounce-2a-002): a "step" line goes out immediately before and after
// every risky step, and the task then waits kSettleMillis ticks before going on.
// Stock's log call only formats the line into a 64 KiB RAM ring (disk_log.cpp's
// sink 8c0cd7fa, interrupts masked); the DSP's main thread
// (main_thread_bfs.cpp, 8c0ba3d4) posts the ring's drain (8c0ba908 -> 8c0ce7c8:
// fwrite of the ring, then fflush) as a background job about once a second. A
// hard crash loses whatever is still in the ring, so the wait lets the line reach
// the file before the step it names runs. Calling the drain from this task
// instead would race the posted one on the ring's tail, so it isn't called. The
// waits are left out of the take's speed and its too-slow limit.
#include "bounce.hpp"
#include "bounce_stock.hpp"
#include "stock.hpp"

using opxy::i32;
using opxy::u16;
using opxy::u32;
using opxy::platform::at;
using opxy::platform::stock;
using opxy::platform::word;
using namespace opxy::bounce;

extern "C" {
void sdk_bounce_render_startup() __asm__("sdk_bounce_render_startup");
u32 sdk_benchmark_cycles() __asm__("sdk_benchmark_cycles");
}

namespace {
template <class... A> void log(u32 line, const char* format, A... args) {
    log_from("sdk/bounce_render.cpp", line, format, args...);
}

// Stock 1.2.1 on the DSP (drafts/bounce-stems.md §7.2, §7.7).
const u32 kEngine = 0x84c696fc;        // the audio engine object every render and note call takes
const u32 kMode = 0x842e2a00;          // audio mode: 0 plays, 2 sends silence and renders nothing
const u32 kBlocks = 0x84a50b98;        // the audio interrupt's block counter, in every mode: a halfword
const u32 kTrackWords = 0x11b01d48;    // a track's instrument word; kNoInstrument: note-on returns at once
const u32 kNoInstrument = 0x1d;
const u32 kRandom = 0x11b01e50;        // the DSP's LCG state (x * 1664525 + 1013904223)
void set_mode(u32 mode) { stock<void(u32)>(0x8d4ff0a8)(mode); }
void render(i32* out, i32* in) { stock<void(void*, i32*, i32*)>(0x8d510760)(at<void>(kEngine), out, in); }
void note_on(u32 track, u32 note) {
    stock<void(void*, u32, u32, u32, u32, u32)>(0x8d50e81c)(at<void>(kEngine), track, note, 127, 1, 0);
}
void note_off(u32 track, u32 note) { stock<void(void*, u32, u32, u32)>(0x8d50e964)(at<void>(kEngine), track, note, 1); }
// FreeRTOS 10.0.0's vTaskSuspendAll, xTaskResumeAll, xTaskGetTickCount and vTaskDelay
// (the DSP's main_task sleeps a tick with it each pass, 8c0ba506).
void suspend_tasks() { stock<void()>(0x8c03ee34)(); }
void resume_tasks() { stock<u32()>(0x8c03f284)(); }
u32 tick_count() { return stock<u32()>(0x8c03ee20)(); }
void delay_ticks(u32 ticks) { stock<void(u32)>(0x8c040188)(ticks); }
u32 raw_blocks() { return *at<const volatile u16>(kBlocks); }
Counter16 counter;
// The block counter widened to 32 bits; read at least every 65,536 blocks (95 s).
u32 blocks() { return counter.read(raw_blocks()); }
u32 cycles() { return sdk_benchmark_cycles(); }

// How each engine call is kept whole against tasks. kSuspend (the design) stops
// the scheduler and leaves interrupts on; kMask is the documented fallback,
// interrupts off for one block's render as stock's benchmark masks its 1,500.
enum Atomic { kSuspend, kMask };
const Atomic kAtomic = kSuspend;

// The test's settings, fixed at compile time. Waits are in ticks, a tick taken as
// a millisecond; the take's length is in audio.
const u32 kStartMillis = 120000;       // after the task starts: a window to play the base with the task idle
const u32 kIdleLogMillis = 10000;      // a line with both clocks this often in that window
const u32 kSettleMillis = 1500;        // after each step line, so stock's ~1 s log drain writes it out
const u32 kClockTicks = 300;           // the clock measurement's span
const u32 kHoldMillis = 20000;         // the chord held
const u32 kTailMillis = 5000;          // rendered after its release
const u32 kGapMillis = 5000;           // between takes
const u32 kTakes = 2;
const u32 kTrack = 0;                  // the device's track 1
const u32 kChord[] = {48, 55, 60, 64, 67};
const u32 kNotes = sizeof kChord / sizeof kChord[0];
const u32 kYieldBlocks = 16;           // a tick's sleep after this many blocks
const u32 kMaxElapsedMillis = 120000;  // a take slower than 0.2x real time stops, releasing the chord
const u32 kSeed = 0x0b0a2ce5u;         // the DSP's LCG, the same for every take
const u32 kDitherSeed = 0x9e3779b9u;
const u32 kBurstBytes = 4u << 20;
const u32 kPriority = 3;
const u32 kStack = 0x4000;             // the engine's depth from a task is unmeasured; the benchmark runs in 8 KiB

enum Ended { kDone = 0, kTooSlow, kWriteShort, kNoBlocks, kBusy, kNoFile };
const char* ended_name(u32 ended) {
    const char* names[] = {"done", "too slow", "short write", "no blocks", "mode busy", "no file"};
    return ended < 6 ? names[ended] : "?";
}

u32 task_handle;
volatile u32 started;
unsigned char* storage;   // allocation: chunk buffer, then the engine's blocks
Chunk chunk;
i32* out;                 // one engine block, interleaved left and right
i32* in;                  // its input, zero
u32 per_block;            // measured cycles a block (nominal when the counter doesn't move)
u32 per_tick;             // measured cycles a tick
u32 paused;               // ticks spent waiting for step lines to reach /logs, since boot
u32 hashes[kTakes];
char folder[96];
char temporary[112];
char final_path[112];
unsigned char header[kHeaderBytes];

struct Take {
    u32 number, ended, engine_word, mode_before, mode_after;
    bool headed, renamed;
    u32 rendered, elapsed, settle, sleeps, paused;   // elapsed and paused in ticks
    u32 mode2_ticks, mode2_blocks;   // mode 2's span, and the counter's move over it
    u32 mib_logged;       // whole MiB of the file already announced
    u32 render_units, worst_render;   // cycles / 256, and cycles
    u32 write_units, worst_write, writes, short_writes;
    Level left, right;
    Hash hash;
    Dither dither;
    File* file;
    // Field by field: a whole-struct copy could become a memset call.
    void reset() {
        number = ended = engine_word = mode_before = mode_after = 0;
        headed = renamed = false;
        rendered = elapsed = settle = sleeps = paused = mib_logged = 0;
        mode2_ticks = mode2_blocks = 0;
        render_units = worst_render = write_units = worst_write = writes = short_writes = 0;
        left.reset();
        right.reset();
        hash.reset();
        dither.state = kDitherSeed;
        file = nullptr;
    }
} take;

// Lets stock's log drain (about once a second) write the lines so far before
// the next step: kSettleMillis ticks, in mode 0 or 2 alike. It reads the block
// counter on each side, which keeps the widened count current. Never called with
// the scheduler suspended.
void settle() {
    const u32 from = tick_count();
    blocks();
    delay_ticks(kSettleMillis);
    blocks();
    paused += tick_count() - from;
}

// One diagnostic line, then the wait that gets it into /logs.
template <class... A> void step(u32 line, const char* format, A... args) {
    log(line, format, args...);
    settle();
}

// The next block edge: spins on the counter, at most about two blocks.
u32 edge(u32* at_cycles) {
    const u32 from = blocks();
    const u32 start = cycles();
    u32 now = from;
    while ((now = blocks()) == from && cycles() - start < 4u * per_block + 2000000u) {
    }
    *at_cycles = cycles();
    return now;
}

// Cycles a tick, and a block, measured between two block edges about kClockTicks
// ticks apart: a fact on the device rather than an assumed clock or tick rate.
// The ticks pace every wait, so a tick clock that isn't there stops the render;
// a block counter that doesn't move is logged and a nominal block taken instead,
// since mode 2, not the counter, keeps the interrupt off the engine.
bool measure_clock() {
    per_block = per_tick = 0;
    u32 c0, c1;
    const u32 b0 = edge(&c0);
    const u32 t0 = tick_count();
    delay_ticks(kClockTicks);
    const u32 b1 = edge(&c1);
    const u32 t1 = tick_count();
    const u32 n = b1 - b0, t = t1 - t0;
    per_tick = t ? (c1 - c0) / t : 0;
    // The core timer's 400,000 cycles: 100 MHz to 1 GHz brackets any tick this could be.
    if (per_tick < 100000 || per_tick > 1000000) {
        log(__LINE__, "render: no tick clock (%d cycles a tick over %d ticks); not rendering", static_cast<int>(per_tick),
            static_cast<int>(t));
        return false;
    }
    if (n >= 2 && n < 100000) {
        per_block = (c1 - c0) / n;
    } else {
        per_block = nominal_per_block(per_tick);
        log(__LINE__, "render clock: the block counter moved %d in %d ticks (raw %x); a nominal block", static_cast<int>(n),
            static_cast<int>(t), raw_blocks());
    }
    log(__LINE__, "render clock: %d cycles a block, %d cycles a tick (%d ticks, %d blocks)", static_cast<int>(per_block),
        static_cast<int>(per_tick), static_cast<int>(t), static_cast<int>(n));
    // A block is 1.45 ms: 100 MHz to 1 GHz brackets any core clock this could be.
    return per_block > 145000 && per_block < 1451000;
}

// The next free "render NNN" folder under /fat32/samples/bounce; 0 when none.
u32 next_folder() {
    make_folder("/fat32/samples/bounce");
    for (u32 n = 1; n <= 999; ++n) {
        if (!take_path(folder, sizeof folder, "/fat32/samples/bounce/render ", n, "")) return 0;
        if (make_folder(folder) >= 0) return n;
    }
    return 0;
}

// Writes what the chunk holds; false on a short write.
bool flush() {
    if (!chunk.used) return true;
    const u32 start = cycles();
    const u32 stored = file_write(chunk.base, 1, chunk.used, take.file);
    const u32 spent = cycles() - start;
    take.write_units += spent >> 8;
    if (spent > take.worst_write) take.worst_write = spent;
    ++take.writes;
    const bool whole = stored == chunk.used;
    if (!whole) ++take.short_writes;
    chunk.drained();
    const u32 mib = chunk.written >> 20;
    if (mib > take.mib_logged) {
        take.mib_logged = mib;
        step(__LINE__, "step take %d: %d MiB written (%d writes, short %d)", static_cast<int>(take.number),
             static_cast<int>(mib), static_cast<int>(take.writes), static_cast<int>(take.short_writes));
    }
    return whole;
}

void release_chord() {
    for (u32 i = 0; i < kNotes; ++i) {
        step(__LINE__, "step take %d: note-off %d, before", static_cast<int>(take.number), static_cast<int>(kChord[i]));
        note_off(kTrack, kChord[i]);
        step(__LINE__, "step take %d: note-off %d, after", static_cast<int>(take.number), static_cast<int>(kChord[i]));
    }
}

// One engine block, whole with respect to tasks.
void render_block() {
    for (u32 i = 0; i < kBlockWords; ++i) out[i] = in[i] = 0;
    if (kAtomic == kSuspend) {
        suspend_tasks();
        render(out, in);
        resume_tasks();
    } else {
        u32 mask;
        __asm__ __volatile__("CLI %0;" : "=d"(mask));
        render(out, in);
        __asm__ __volatile__("STI %0;" : : "d"(mask));
    }
}

// Steps 1-7. Returns with mode 0 whenever it set mode 2.
void render_take(u32 total, u32 hold) {
    take.mode_before = word(kMode);
    take.engine_word = word(kTrackWords + 4 * kTrack);
    if (take.mode_before != 0) {
        take.ended = kBusy;
        return;
    }
    const int n = static_cast<int>(take.number);
    step(__LINE__, "step take %d: mode 2, before (track word %x)", n, take.engine_word);
    set_mode(2);
    // The step's settle pause (kSettleMillis ticks, about a thousand blocks) lets any
    // block the interrupt began in mode 0 finish before the engine is touched; the
    // counter's move over it is logged, not waited on.
    const u32 mode2_from = tick_count();
    const u32 before = blocks();
    step(__LINE__, "step take %d: mode 2 set, now %d; settling", n, static_cast<int>(word(kMode)));
    take.settle = blocks() - before;
    step(__LINE__, "step take %d: mode 2 settled, %d blocks counted in %d ticks", n, static_cast<int>(take.settle),
         static_cast<int>(tick_count() - mode2_from));
    step(__LINE__, "step take %d: seeding, before", n);
    *at<volatile u32>(kRandom) = kSeed;
    take.dither.state = kDitherSeed;
    step(__LINE__, "step take %d: seeded", n);
    for (u32 i = 0; i < kNotes; ++i) {
        step(__LINE__, "step take %d: note-on %d, before (track word %x)", n, static_cast<int>(kChord[i]),
             word(kTrackWords + 4 * kTrack));
        note_on(kTrack, kChord[i]);
        step(__LINE__, "step take %d: note-on %d, after", n, static_cast<int>(kChord[i]));
    }
    bool released = false;
    // Speed and the too-slow limit count ticks from here, without the waits for step lines.
    const u32 from = tick_count();
    const u32 paused_from = paused;
    step(__LINE__, "step take %d: first render call, before", n);
    for (u32 b = 0; b < total; ++b) {
        if (b == hold) {
            release_chord();
            released = true;
        }
        const u32 start = cycles();
        render_block();
        const u32 spent = cycles() - start;
        take.render_units += spent >> 8;
        if (spent > take.worst_render) take.worst_render = spent;
        for (u32 f = 0; f < kFrames; ++f) {
            const u32 l = static_cast<u32>(out[2 * f]), r = static_cast<u32>(out[2 * f + 1]);
            take.left.add(l);
            take.right.add(r);
            take.hash.add(l);
            take.hash.add(r);
            if (chunk.full() && !flush()) take.ended = kWriteShort;
            chunk.frame16(take.dither.sample(l), take.dither.sample(r));
        }
        ++take.rendered;
        if (render_checkpoint(take.rendered))
            step(__LINE__, "step take %d: block %d rendered, worst %d cycles, sleeps %d", n,
                 static_cast<int>(take.rendered), static_cast<int>(take.worst_render), static_cast<int>(take.sleeps));
        if (take.ended) break;
        if ((b + 1) % kYieldBlocks == 0) {
            delay_ticks(1);
            ++take.sleeps;
            blocks();
            if (tick_count() - from - (paused - paused_from) > kMaxElapsedMillis) {
                take.ended = kTooSlow;
                break;
            }
        }
    }
    // A take cut short leaves its chord to ring out live rather than held.
    if (!released) release_chord();
    take.paused = paused - paused_from;
    take.elapsed = tick_count() - from - take.paused;
    step(__LINE__, "step take %d: mode 0, before (%d blocks rendered)", n, static_cast<int>(take.rendered));
    take.mode2_ticks = tick_count() - mode2_from;
    take.mode2_blocks = blocks() - before;
    set_mode(0);
    take.mode_after = word(kMode);
    step(__LINE__, "step take %d: mode 0 set, now %d", n, static_cast<int>(take.mode_after));
}

// Sustained writes on the DSP's own storage: kBurstBytes in whole chunks.
void burst() {
    if (!take_path(temporary, sizeof temporary, "/fat32/samples/bounce/render ", take.number, "/burst.tmp")) return;
    const int n = static_cast<int>(take.number);
    step(__LINE__, "step take %d: burst open, before", n);
    File* file = file_open(temporary, "wb");
    if (!file) {
        step(__LINE__, "take %d: burst file did not open", n);
        return;
    }
    step(__LINE__, "step take %d: burst open, after", n);
    u32 units = 0, written = 0;
    for (u32 done = 0; done < kBurstBytes; done += kChunkBytes) {
        const u32 start = cycles();
        written += file_write(chunk.base, 1, kChunkBytes, file);
        units += (cycles() - start) >> 8;
        if (((done + kChunkBytes) & ((1u << 20) - 1)) == 0)
            step(__LINE__, "step take %d: burst %d MiB written", n, static_cast<int>((done + kChunkBytes) >> 20));
    }
    step(__LINE__, "step take %d: burst close, before", n);
    const u32 start = cycles();
    file_close(file);
    units += (cycles() - start) >> 8;
    step(__LINE__, "step take %d: burst close, after; remove, before", n);
    remove_file(temporary);
    step(__LINE__, "step take %d: burst removed", n);
    const u32 millis = units / (cycles_per_milli(per_block) >> 8);
    log(__LINE__, "take %d: burst %d KiB in %d ms: %d KiB/s", static_cast<int>(take.number),
        static_cast<int>(written >> 10), static_cast<int>(millis), static_cast<int>(kib_per_second(written, millis)));
}

void report() {
    const int n = static_cast<int>(take.number);
    const u32 micro = cycles_per_micro(per_block);
    const u32 milli_units = cycles_per_milli(per_block) >> 8;
    const u32 write_ms = milli_units ? take.write_units / milli_units : 0;
    const u32 bytes = chunk.written + chunk.used;
    log(__LINE__, "take %d: %d frames, track word %x, ended by %s, header %d, renamed %d", n,
        static_cast<int>(chunk.frames()), take.engine_word, ended_name(take.ended), take.headed ? 1 : 0, take.renamed ? 1 : 0);
    const u32 elapsed_blocks = blocks_for_ticks(take.elapsed, per_tick, per_block);
    log(__LINE__, "take %d: rendered %d blocks in %d ticks (%d blocks), speed x100 %d, engine pct %d, worst %d us, sleeps %d",
        n, static_cast<int>(take.rendered), static_cast<int>(take.elapsed), static_cast<int>(elapsed_blocks),
        static_cast<int>(speed_x100(take.rendered, elapsed_blocks)),
        static_cast<int>(block_percent(take.render_units, take.rendered, per_block)),
        static_cast<int>(micro ? take.worst_render / micro : 0), static_cast<int>(take.sleeps));
    log(__LINE__, "take %d: wrote %d KiB in %d writes, %d ms, worst %d us, %d KiB/s, short %d", n,
        static_cast<int>(bytes >> 10), static_cast<int>(take.writes), static_cast<int>(write_ms),
        static_cast<int>(micro ? take.worst_write / micro : 0), static_cast<int>(kib_per_second(bytes, write_ms)),
        static_cast<int>(take.short_writes));
    log(__LINE__, "take %d: left %x %x %x, right %x %x %x", n, take.left.peak, take.left.high, take.left.low,
        take.right.peak, take.right.high, take.right.low);
    log(__LINE__, "take %d: settle %d blocks, mode %d then %d, hash %x, step waits %d ticks", n,
        static_cast<int>(take.settle), static_cast<int>(take.mode_before), static_cast<int>(take.mode_after),
        take.hash.value, static_cast<int>(take.paused));
    log(__LINE__, "take %d: mode 2 for %d ticks, block counter moved %d (about %d expected)", n,
        static_cast<int>(take.mode2_ticks), static_cast<int>(take.mode2_blocks),
        static_cast<int>(blocks_for_ticks(take.mode2_ticks, per_tick, per_block)));
    settle();
}

// One take, start to finish, with its file.
void run_take(u32 index) {
    const u32 hold = blocks_for_millis(kHoldMillis);
    const u32 total = hold + blocks_for_millis(kTailMillis);
    take.reset();
    chunk.reset(chunk.base);
    step(__LINE__, "step take %d: folder, before", static_cast<int>(index + 1));
    take.number = next_folder();
    const int n = static_cast<int>(take.number);
    step(__LINE__, "step take %d: folder render %d; file open, before", static_cast<int>(index + 1), n);
    if (!take.number || !take_path(temporary, sizeof temporary, "/fat32/samples/bounce/render ", take.number, "/mix.tmp") ||
        !take_path(final_path, sizeof final_path, "/fat32/samples/bounce/render ", take.number, "/mix.wav") ||
        !(take.file = file_open(temporary, "wb"))) {
        step(__LINE__, "render: take %d could not open a file (folder %d)", static_cast<int>(index + 1), n);
        take.ended = kNoFile;
        return;
    }
    step(__LINE__, "step take %d: file open, after", n);
    chunk.begin();
    render_take(total, hold);
    if (!take.ended || take.ended == kTooSlow) flush();
    step(__LINE__, "step take %d: close, before (%d bytes)", n, static_cast<int>(chunk.written + chunk.used));
    file_close(take.file);
    step(__LINE__, "step take %d: close, after; header, before", n);
    wav_header(header, chunk.frames(), 2, 16);
    // r+b opens at the start without truncating, so the header overwrites the placeholder.
    File* file = file_open(temporary, "r+b");
    if (file) {
        take.headed = file_write(header, 1, kHeaderBytes, file) == kHeaderBytes;
        file_close(file);
    }
    step(__LINE__, "step take %d: header %d; rename, before", n, take.headed ? 1 : 0);
    take.renamed = rename_file(temporary, final_path);
    step(__LINE__, "step take %d: rename %d", n, take.renamed ? 1 : 0);
    hashes[index] = take.hash.value;
    report();
    if (take.ended != kWriteShort) burst();
}

void renderer(void*) {
    // kStartMillis ticks from the task's start, a second's sleep at a time, with a
    // line every kIdleLogMillis giving both clocks: whether the task wakes, and
    // whether the block counter moves in mode 0.
    const u32 first = tick_count();
    const u32 counted = blocks();
    step(__LINE__, "render: waiting %d ticks before the first take (block counter raw %x, word %x)",
         static_cast<int>(kStartMillis), raw_blocks(), word(kBlocks));
    u32 next_line = kIdleLogMillis / 10;
    for (;;) {
        const u32 waited = tick_count() - first;
        if (waited >= kStartMillis) break;
        if (waited >= next_line) {
            next_line += kIdleLogMillis;
            log(__LINE__, "render: idle %d ticks, block counter moved %d (raw %x), cycles %x", static_cast<int>(waited),
                static_cast<int>(blocks() - counted), raw_blocks(), cycles());
        }
        delay_ticks(1000);
        blocks();
    }
    step(__LINE__, "render: wait over after %d ticks, block counter moved %d; measuring the clock",
         static_cast<int>(tick_count() - first), static_cast<int>(blocks() - counted));
    if (!measure_clock()) {
        settle();
        return;
    }
    settle();
    step(__LINE__, "step allocate, before");
    void* block = allocate(kChunkBytes + kChunkAlign + 2 * kBlockWords * sizeof(i32));
    if (!block) {
        step(__LINE__, "render: no memory for the %d KiB chunk", static_cast<int>(kChunkBytes >> 10));
        return;
    }
    step(__LINE__, "step allocate, after");
    storage = static_cast<unsigned char*>(block);
    chunk.reset(align_up(storage, kChunkAlign));
    out = reinterpret_cast<i32*>(chunk.base + kChunkBytes);
    in = out + kBlockWords;
    step(__LINE__, "render: %d takes of %d notes on track %d, %d ms held, %d ms tail, %s", static_cast<int>(kTakes),
         static_cast<int>(kNotes), static_cast<int>(kTrack + 1), static_cast<int>(kHoldMillis),
         static_cast<int>(kTailMillis), kAtomic == kSuspend ? "scheduler suspended a block" : "interrupts masked a block");
    for (u32 i = 0; i < kTakes; ++i) {
        if (i) {
            step(__LINE__, "render: gap of %d ms before take %d", static_cast<int>(kGapMillis), static_cast<int>(i + 1));
            delay_ticks(kGapMillis);
        }
        run_take(i);
        if (take.ended && take.ended != kTooSlow) break;  // no second take after a fault
    }
    step(__LINE__, "render: done; take hashes %x %x, same %d", hashes[0], hashes[kTakes - 1],
         hashes[0] == hashes[kTakes - 1] ? 1 : 0);
    release(storage);
    storage = nullptr;
}

// Bisect (bounce-diag-001, OP-XY-Dev drafts/bounce-stems.md §7.8). bounce-2a-001 and 003
// stopped the DSP's log drain (and later MTP and project loading) a few seconds after the
// task began waking; 002, whose task never woke again after its first line, did not. One
// boot runs the suspects in turn, least like 003 first, each long enough (observed stalls
// came 3-31 s after the task began waking) that the time /logs stops says which one it was
// (the main board's RTC line arrives once a minute even when the OP-XY is idle):
//   1. 0-90 s     blocked in one vTaskDelay, no log            (002's case)
//   2. 90-270 s   priority 2, below main_task: wakes every 100 ticks, reads the clocks, no log
//   3. 270-450 s  priority 3, main_task's: the same                (001's case)
//   4. 450-570 s  priority 3, the same and a log line every second (003's case)
//   5. then       ten takes with a 100-tick timeout of a semaphore no one gives, logged
//                 before and after                              (002's never-returning sleep)
// Nothing else: no mode change, note, render, allocation or file.
const bool kDiagnose = true;
const u32 kDiagBlocked = 90000, kDiagBelow = 270000, kDiagEqual = 450000, kDiagLogged = 570000;
const u32 kDiagWake = 100;
u32 semaphore;   // created at startup, as bounce-2a-002 did; never given

// FreeRTOS 10.0.0's vTaskPrioritySet (asserts tasks.c:1472, priority < 16; NULL is the caller;
// stock's bridge write calls it so, 8c0c18d0) and vTaskGetInfo (stock 8c0c18c0): the stack's
// unused words land at +0x20.
void set_priority(u32 priority) { stock<void(void*, u32)>(0x8c03efc4)(nullptr, priority); }
u32 stack_unused_words() {
    u32 status[10];
    for (u32 i = 0; i < 10; ++i) status[i] = 0;
    stock<void(void*, u32*, u32, u32)>(0x8c03f94c)(nullptr, status, 1, 0);
    return status[8] & 0xffff;
}

struct Wakes {
    u32 count, late, worst;   // wakes; those over 2 ticks late; the longest sleep, in ticks
};

// Wakes every kDiagWake ticks until tick `until` from `first`, reading both clocks; logs a
// line every second only when `logging`.
void wake_until(u32 first, u32 until, bool logging, Wakes* wakes) {
    u32 next_line = tick_count() - first;
    for (;;) {
        const u32 before = tick_count();
        const u32 waited = before - first;
        if (waited >= until) return;
        if (logging && waited >= next_line) {
            next_line = waited + 1000;
            log(__LINE__, "diag 4: %d ticks, %d wakes (%d late, longest %d), block counter %d", static_cast<int>(waited),
                static_cast<int>(wakes->count), static_cast<int>(wakes->late), static_cast<int>(wakes->worst),
                static_cast<int>(blocks()));
        }
        delay_ticks(kDiagWake);
        const u32 slept = tick_count() - before;
        ++wakes->count;
        if (slept > kDiagWake + 2) ++wakes->late;
        if (slept > wakes->worst) wakes->worst = slept;
        blocks();
    }
}

void diagnose() {
    const u32 first = tick_count();
    const u32 counted = blocks();
    delay_ticks(kDiagBlocked);   // 1
    const u32 woke = tick_count() - first;
    Wakes below = {0, 0, 0}, equal = {0, 0, 0}, logged = {0, 0, 0};
    set_priority(2);             // 2
    wake_until(first, kDiagBelow, false, &below);
    set_priority(kPriority);     // 3
    wake_until(first, kDiagEqual, false, &equal);
    // 4: the task's first line, at 450 s.
    log(__LINE__, "diag 1: woke after %d ticks; diag 2 (priority 2): %d wakes, %d late, longest %d ticks",
        static_cast<int>(woke), static_cast<int>(below.count), static_cast<int>(below.late), static_cast<int>(below.worst));
    log(__LINE__, "diag 3 (priority 3): %d wakes, %d late, longest %d ticks; block counter moved %d since the start",
        static_cast<int>(equal.count), static_cast<int>(equal.late), static_cast<int>(equal.worst),
        static_cast<int>(blocks() - counted));
    log(__LINE__, "diag 4: stack %d of %d words never used; FreeRTOS heap %d bytes free, %d at least",
        static_cast<int>(stack_unused_words()), static_cast<int>(kStack), static_cast<int>(word(0x11800044)),
        static_cast<int>(word(0x11800048)));
    wake_until(first, kDiagLogged, true, &logged);
    // 5
    log(__LINE__, "diag 5: semaphore takes, before (%d ticks)", static_cast<int>(tick_count() - first));
    const u32 from = tick_count();
    u32 returned = 0;
    for (u32 i = 0; i < 10; ++i) {
        sleep_ticks(semaphore, kDiagWake);
        ++returned;
    }
    log(__LINE__, "diag 5: %d of 10 semaphore takes returned in %d ticks (1000 expected); done", static_cast<int>(returned),
        static_cast<int>(tick_count() - from));
}

void task_entry(void* argument) {
    if (kDiagnose)
        diagnose();
    else
        renderer(argument);
    // The task has no stock delete to call; it sleeps out the boot.
    for (;;) delay_ticks(60000);
}
}

// From sdk_startup_synth on the audio role only.
void sdk_bounce_render_startup() {
    if (started) return;
    started = 1;
    counter.reset();
    if (kDiagnose && !(semaphore = create_semaphore())) return;
    if (create_task(&task_handle, task_entry, kPriority, kStack, "bounce_render") != 0)
        log(__LINE__, "render: could not start the bounce_render task");
}
