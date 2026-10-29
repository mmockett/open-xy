// Experiment, never for release: can a module on stock OS 1.2.1 read files
// while machines register at startup, and run code it read? If both hold,
// machines could load from files instead of from the boot partition.
//
// Inert without a probe file. Stages: 1 synth initializer on the audio role,
// 2 synth initializer on the GUI role, 3 screen factory (GUI), 4 project save.
// The GUI stages try each path form until one opens; the audio role reads
// only once a GUI stage has found the file and the file asks for it. Results
// go to probe-log.txt (probe-log.oxy under /fat32) beside the file found;
// the audio role never writes.
//
// File: 32-byte header "XYPROBE1", flags (1 read on audio role, 2 run code),
// code offset, code bytes, argument, expected result, reserved; then code.
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

const u32 kForms = 4;
const char* const kProbePaths[kForms] = {"probe.bin", "/yaffs2/probe.bin", "/probe.bin", "/fat32/projects/probe.oxy"};
const char* const kLogPaths[kForms] = {"probe-log.txt", "/yaffs2/probe-log.txt", "/probe-log.txt",
                                       "/fat32/projects/probe-log.oxy"};
const u32 kAudioRead = 1, kRunCode = 2;
const u32 kHeaderBytes = 32, kFileBytes = 1024, kCodeBytes = 256, kEvents = 48, kLogWrites = 64;

struct Event {
    u32 sequence, stage, gui, opened, bytes, crc;
};

unsigned char file_bytes[kFileBytes] __attribute__((aligned(4)));
unsigned char code_area[kCodeBytes] __attribute__((aligned(32)));
Event events[kEvents];
u32 event_count, sequence, calls[5], found_form, flags, log_writes;
u32 run_state, run_result, run_expected, run_bytes;  // run_state: 0 none, 1 about to call, 2 returned
char text[4096];
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
    u32 length = 0;
    void put(const char* s) { while (*s) byte(*s++); }
    void byte(char c) { if (length < sizeof text) text[length++] = c; }
    void number(u32 v) {
        char digits[10];
        u32 count = 0;
        do { digits[count++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        while (count) byte(digits[--count]);
    }
    void hex(u32 v) {
        for (int shift = 28; shift >= 0; shift -= 4) byte("0123456789abcdef"[(v >> shift) & 15]);
    }
};

void write_log() {
    if (!found_form || log_writes >= kLogWrites) return;
    ++log_writes;
    Writer w;
    w.put("open-xy module probe 1\nfound: ");
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
    w.put("\ncalls: synth-audio ");
    w.number(calls[1]);
    w.put(", synth-gui ");
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

// Copies the file's code into the module, makes it visible to instruction
// fetch and calls it once.
void run_code(u32 bytes_read) {
    const u32 offset = word_at(file_bytes + 12), bytes = word_at(file_bytes + 16);
    if (offset < kHeaderBytes || bytes == 0 || bytes > kCodeBytes || offset > bytes_read || bytes > bytes_read - offset)
        return;
    for (u32 i = 0; i < bytes; ++i) code_area[i] = file_bytes[offset + i];
    run_bytes = bytes;
    run_expected = word_at(file_bytes + 24);
    run_state = 1;
    write_log();  // a crash in the call leaves "state 1" on the device
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
    if (!gui && !(found_form && (flags & kAudioRead))) return;

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
        if (gui && (flags & kRunCode) && !run_state) run_code(n);
        break;
    }
    if (gui && (first_find || calls[stage] == 1 || stage == 4 || run_state != ran_before)) write_log();
}
