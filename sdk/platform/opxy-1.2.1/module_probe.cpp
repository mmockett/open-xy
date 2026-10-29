// Experiment, never for release: can a module on stock OS 1.2.1 read files
// while machines register at startup, and run code it read? If both hold,
// machines could load from files instead of from the boot partition.
//
// The OP-XY has two processors, each running its own copy of the module: the
// main board (the GUI role) and the DSP board (the audio role), which renders
// voices. Stages: 1 synth initializer on the DSP board, 2 synth initializer on
// the main board, 3 screen factory (main), 4 project save (main). Every stage
// tries each path form until one opens, then keeps to that form.
//
// Reports: each stage logs one line through stock's logger, so both boards
// appear in /logs/log_*.txt as "module_probe[<stage>]: ...". The main board
// also writes probe-log.txt (probe-log.oxy under /fat32) beside the file found.
// The DSP board never writes files.
//
// File: 32-byte header "XYPROBE1", flags (2 run code on the main board, 4 run
// code on the DSP board), code offset, code bytes, argument, expected result,
// reserved; then code. Without a file, each stage only tries to open it.
#include <opxy/types.hpp>
#include "stock.hpp"

using opxy::u32;
using opxy::platform::stock;

extern "C" {
void sdk_module_probe(u32 stage) __asm__("sdk_module_probe");
void probe_sync_code(const void* start, u32 bytes) __asm__("probe_sync_code");
u32 stock_gui_role() __asm__("stock_gui_role");
}

namespace {
typedef void File;
// newlib, as project_companion.cpp calls it.
File* file_open(const char* path, const char* mode) {
    return stock<File*(const char*, const char*)>(0x8fed0348)(path, mode);
}
u32 file_read(void* data, u32 size, u32 count, File* file) {
    return stock<u32(void*, u32, u32, File*)>(0x8fed05bc)(data, size, count, file);
}
u32 file_write(const void* data, u32 size, u32 count, File* file) {
    return stock<u32(const void*, u32, u32, File*)>(0x8fed0780)(data, size, count, file);
}
void file_close(File* file) { stock<int(File*)>(0x8fed00ae)(file); }
// Stock's logger, as producer_factory.cpp calls it at 8d506d9a on both boards:
// level 3 (info), source path, line, format, arguments. The log shows the
// path's base name and the line: "module_probe[<stage>]: <text>".
void log_line(u32 stage, const char* text) {
    stock<void(u32, const char*, u32, const char*, ...)>(0x8c042efc)(3, "../src/sdk/module_probe.cpp", stage, "%s",
                                                                     text);
}

const u32 kForms = 4;
const char* const kProbePaths[kForms] = {"probe.bin", "/yaffs2/probe.bin", "/probe.bin", "/fat32/projects/probe.oxy"};
const char* const kLogPaths[kForms] = {"probe-log.txt", "/yaffs2/probe-log.txt", "/probe-log.txt",
                                       "/fat32/projects/probe-log.oxy"};
const u32 kRunMain = 2, kRunDsp = 4;
const u32 kHeaderBytes = 32, kFileBytes = 1024, kCodeBytes = 256, kEvents = 48, kLogWrites = 64, kLogLines = 32;

struct Event {
    u32 sequence, stage, gui, opened, bytes, crc;
};

unsigned char file_bytes[kFileBytes] __attribute__((aligned(4)));
unsigned char code_area[kCodeBytes] __attribute__((aligned(32)));
Event events[kEvents];
u32 event_count, sequence, calls[5], found_form, flags, log_writes, log_lines;
u32 run_state, run_result, run_expected, run_bytes;  // run_state: 0 none, 1 about to call, 2 returned
char text[4096];
char line[160];
u32 crc_table[256];

u32 word_at(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<u32>(p[3]) << 24; }

u32 crc32(const unsigned char* data, u32 count) {
    if (!crc_table[1]) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int bit = 0; bit < 8; ++bit) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    }
    u32 crc = ~0u;
    while (count--) crc = crc_table[(crc ^ *data++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

class Writer {
public:
    Writer(char* out, u32 capacity) : out(out), capacity(capacity) {}
    char* out;
    u32 capacity;
    u32 length = 0;
    void put(const char* s) { while (*s) byte(*s++); }
    void byte(char c) { if (length + 1 < capacity) out[length++] = c; }
    void number(u32 v) {
        char digits[10];
        u32 count = 0;
        do { digits[count++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        while (count) byte(digits[--count]);
    }
    void hex(u32 v) {
        for (int shift = 28; shift >= 0; shift -= 4) byte("0123456789abcdef"[(v >> shift) & 15]);
    }
    const char* finish() { out[length] = 0; return out; }
};

void say(u32 stage, const char* text) {
    if (log_lines >= kLogLines) return;
    ++log_lines;
    log_line(stage, text);
}

void write_log() {
    if (!found_form || log_writes >= kLogWrites) return;
    ++log_writes;
    Writer w(text, sizeof text);
    w.put("open-xy module probe 2\nfound: ");
    w.put(kProbePaths[found_form - 1]);
    w.put("\nflags: ");
    w.number(flags);
    w.put("\nrun: state ");
    w.number(run_state);
    w.put(", bytes ");
    w.number(run_bytes);
    w.put(", result ");
    w.hex(run_result);
    w.put(", expected ");
    w.hex(run_expected);
    w.put(run_state == 2 && run_result == run_expected ? " (match)" : "");
    w.put("\ncalls (this board): synth-dsp ");
    w.number(calls[1]);
    w.put(", synth-main ");
    w.number(calls[2]);
    w.put(", screen ");
    w.number(calls[3]);
    w.put(", save ");
    w.number(calls[4]);
    w.put("\nlog writes: ");
    w.number(log_writes);
    w.put("\nevents (sequence stage gui opened-forms bytes crc32):\n");
    for (u32 i = 0; i < event_count; ++i) {
        const Event& e = events[i];
        w.number(e.sequence);
        w.byte(' ');
        w.number(e.stage);
        w.byte(' ');
        w.number(e.gui);
        w.byte(' ');
        w.hex(e.opened);
        w.byte(' ');
        w.number(e.bytes);
        w.byte(' ');
        w.hex(e.crc);
        w.byte('\n');
    }
    File* file = file_open(kLogPaths[found_form - 1], "wb");
    if (!file) return;
    file_write(text, w.length, 1, file);
    file_close(file);
}

// One line per stage call: "<board> call <n>: opened <mask> bytes <n> crc <crc>
// run <state> <result>/<expected>".
void report(u32 stage, u32 gui, const Event& e) {
    Writer w(line, sizeof line);
    w.put(gui ? "probe main call " : "probe dsp call ");
    w.number(calls[stage]);
    w.put(": opened ");
    w.hex(e.opened);
    w.put(" bytes ");
    w.number(e.bytes);
    w.put(" crc ");
    w.hex(e.crc);
    w.put(" flags ");
    w.number(flags);
    w.put(" run ");
    w.number(run_state);
    w.byte(' ');
    w.hex(run_result);
    w.byte('/');
    w.hex(run_expected);
    w.put(run_state == 2 && run_result == run_expected ? " match" : "");
    say(stage, w.finish());
}

// Copies the file's code into the module, makes it visible to instruction
// fetch and calls it once.
void run_code(u32 stage, u32 bytes_read) {
    const u32 offset = word_at(file_bytes + 12), bytes = word_at(file_bytes + 16);
    if (offset < kHeaderBytes || bytes == 0 || bytes > kCodeBytes || offset > bytes_read || bytes > bytes_read - offset)
        return;
    for (u32 i = 0; i < bytes; ++i) code_area[i] = file_bytes[offset + i];
    run_bytes = bytes;
    run_expected = word_at(file_bytes + 24);
    run_state = 1;
    say(stage, "probe about to run code");  // a crash in the call leaves this as the last line
    if (stock_gui_role()) write_log();
    probe_sync_code(code_area, bytes);
    run_result = reinterpret_cast<u32 (*)(u32)>(code_area)(word_at(file_bytes + 20));
    run_state = 2;
}
}

void sdk_module_probe(u32 stage) {
    const u32 gui = stock_gui_role() ? 1 : 0;
    ++sequence;
    if (stage < 5) ++calls[stage];
    Event scratch;
    Event& e = event_count < kEvents ? events[event_count++] : scratch;
    e.sequence = sequence;
    e.stage = stage;
    e.gui = gui;
    e.opened = e.bytes = e.crc = 0;
    if (calls[stage] == 1) say(stage, gui ? "probe main stage start" : "probe dsp stage start");

    const bool first_find = !found_form;
    const u32 ran_before = run_state;
    for (u32 form = found_form ? found_form - 1 : 0; form < kForms; ++form) {
        File* file = file_open(kProbePaths[form], "rb");
        if (!file) {
            if (found_form) break;
            continue;
        }
        const u32 n = file_read(file_bytes, 1, kFileBytes, file);
        file_close(file);
        e.opened |= 1u << form;
        e.bytes = n;
        e.crc = crc32(file_bytes, n);
        if (!found_form) {
            const bool probe = n >= kHeaderBytes && file_bytes[0] == 'X' && file_bytes[1] == 'Y' &&
                               file_bytes[2] == 'P' && file_bytes[7] == '1';
            found_form = form + 1;
            flags = probe ? word_at(file_bytes + 8) : 0;
        }
        if ((flags & (gui ? kRunMain : kRunDsp)) && !run_state) run_code(stage, n);
        break;
    }
    if (calls[stage] <= 2 || run_state != ran_before) report(stage, gui, e);
    if (gui && (first_find || calls[stage] == 1 || stage == 4 || run_state != ran_before)) write_log();
}
