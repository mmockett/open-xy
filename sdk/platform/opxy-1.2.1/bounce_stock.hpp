// Bounce, private: the stock calls the capture (bounce.cpp, main board) and the
// render (bounce_render.cpp, DSP) share. Device only; the host tests bounce.hpp.
//
// Every helper is static inline (or a template in an unnamed namespace), so each
// unit keeps its own copy: staging links without COMDAT groups, and an unused one
// costs nothing. The file calls branch on the board's role inside stock: on the
// main board they cross the bridge, on the DSP they run on its own FAT32
// (OP-XY-Dev drafts/bounce-stems.md §7.0).
#pragma once
#include <opxy/types.hpp>
#include "stock.hpp"

namespace opxy { namespace bounce {
typedef void File;

// The libc's fopen, fwrite and fclose, as stock's project files call them
// (project_files.hpp). fwrite hands a whole write of 512 bytes or more straight to
// write() when the file's buffer is empty (8fed0780), so a 64 KiB write is one call.
static inline File* file_open(const char* path, const char* mode) {
    return platform::stock<File*(const char*, const char*)>(0x8fed0348)(path, mode);
}
static inline u32 file_write(const void* data, u32 size, u32 count, File* file) {
    return platform::stock<u32(const void*, u32, u32, File*)>(0x8fed0780)(data, size, count, file);
}
static inline void file_close(File* file) { platform::stock<int(File*)>(0x8fed00ae)(file); }
// mkdir(path, mode) as stock's folder setup calls it (8c0409f0): -1 when it fails,
// which includes a folder that already exists.
static inline i32 make_folder(const char* path) { return platform::stock<i32(const char*, u32)>(0x8c0bd5bc)(path, 0); }
// Stock's rename wrapper (8c040bd8): 1 when it moved the file.
static inline bool rename_file(const char* from, const char* to) {
    return platform::stock<u32(const char*, const char*)>(0x8c040bd8)(from, to) != 0;
}
// Stock's remove wrapper (8c040c24), as its replace-by-rename calls it first.
static inline void remove_file(const char* path) { platform::stock<u32(const char*)>(0x8c040c24)(path); }

// The stock heap, as the benchmark diagnostic takes from it on the DSP (benchmark.cpp.in).
static inline void* allocate(u32 bytes) {
    const u32 heap = 0x08036aa4;
    platform::stock<void(u32, i32)>(0x8c03e6f4)(platform::word(heap), -1);
    void* block = platform::stock<void*(u32, u32)>(0x8c048788)(platform::word(heap + 4), bytes);
    platform::stock<void(u32, u32, u32, u32)>(0x8c03e9c8)(platform::word(heap), 0, 0, 0);
    return block;
}
static inline void release(void* block) { platform::stock<void(void*)>(0x8d4d2910)(block); }

// A binary semaphore no one gives: taking it with a timeout is the task's sleep.
static inline u32 create_semaphore() { return platform::stock<u32(u32, u32, u32)>(0x8c03de70)(1, 0, 3); }
static inline void sleep_ticks(u32 semaphore, u32 ticks) { platform::stock<u32(u32, u32)>(0x8c03e6f4)(semaphore, ticks); }
// Stock's task creation, as spibridge_io_task is created (8c0c0cb8); 0 on success.
static inline u32 create_task(u32* handle, void (*entry)(void*), u32 priority, u32 stack, const char* name) {
    return platform::stock<u32(u32*, void (*)(void*), void*, u32, u32, u32, const char*, u32)>(0x8d5bdf44)(
        handle, entry, nullptr, priority, 0, stack, name, 1);
}

namespace {
// Stock's log, as sport.cpp calls it: level 3 prints an "I" line in /logs (on the
// DSP it writes the line itself; the main board's lines arrive marked "bfm"). The
// line's tag is the file name after its last '/', and loses its first character
// when there is none ("ounce" on bounce-002), so pass a path as stock does.
template <class... A> void log_from(const char* file, u32 line, const char* format, A... args) {
    platform::stock<void(u32, const char*, u32, const char*, ...)>(0x8c042efc)(3, file, line, format, args...);
}
}
}}
