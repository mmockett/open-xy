// Stock OS 1.2.0 project companions (docs/projects.md). Stock saves every project,
// the workspace included, through one call of save(project, path) (8d5973ac, from
// 8d584576). After a save that succeeds, the hook writes the .oxy beside the .xy: the
// saved file's size and CRC-32, then this firmware's machine numbers, in the canonical
// form tooling/projects.py writes. It runs where stock saves, never on the audio role.
// A failure leaves the previous companion, which no longer matches, or a partial
// one; readers ignore both.
#include "project_companion.hpp"
#include "stock.hpp"

using opxy::u32;
using opxy::platform::CompanionMachine;
using opxy::platform::stock;

extern "C" u32 sdk_project_save(void* project, const char* path) __asm__("sdk_project_save");

namespace {
typedef void File;
// newlib, as stock's project reader and writer call it (8d58f184, 8d58f230, 8d58f70c, 8d58f1aa).
File* file_open(const char* path, const char* mode) {
    return stock<File*(const char*, const char*)>(0x8fed0350)(path, mode);
}
u32 file_read(void* data, u32 size, u32 count, File* file) {
    return stock<u32(void*, u32, u32, File*)>(0x8fed05c8)(data, size, count, file);
}
u32 file_write(const void* data, u32 size, u32 count, File* file) {
    return stock<u32(const void*, u32, u32, File*)>(0x8fed078c)(data, size, count, file);
}
void file_close(File* file) { stock<int(File*)>(0x8fed00b6)(file); }
u32 save(void* project, const char* path) { return stock<u32(void*, const char*)>(0x8d5973ac)(project, path); }

const u32 kPathBytes = 256;
const u32 kTextBytes = 8192;
unsigned char chunk[4096];
char text[kTextBytes];
char companion[kPathBytes];
u32 crc_table[256];

u32 value(const char* symbol) { return static_cast<u32>(reinterpret_cast<__UINTPTR_TYPE__>(symbol)); }

// zlib's CRC-32.
u32 crc32(u32 crc, const unsigned char* data, u32 count) {
    if (!crc_table[1]) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int bit = 0; bit < 8; ++bit) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    }
    crc = ~crc;
    while (count--) crc = crc_table[(crc ^ *data++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

// The project's bytes as saved: false when it cannot be read back.
bool measure(const char* path, u32& bytes, u32& crc) {
    File* file = file_open(path, "rb");
    if (!file) return false;
    bytes = crc = 0;
    for (u32 count; (count = file_read(chunk, 1, sizeof chunk, file)) != 0;) {
        crc = crc32(crc, chunk, count);
        bytes += count;
    }
    file_close(file);
    return true;
}

// song.xy -> song.oxy; false for any other name.
bool companion_path(const char* path) {
    u32 length = 0;
    while (path[length]) ++length;
    if (length < 4 || length + 2 >= kPathBytes || path[length - 3] != '.' || path[length - 2] != 'x' ||
        path[length - 1] != 'y')
        return false;
    for (u32 i = 0; i < length - 2; ++i) companion[i] = path[i];
    companion[length - 2] = 'o';
    companion[length - 1] = 'x';
    companion[length] = 'y';
    companion[length + 1] = 0;
    return true;
}

class Writer {
public:
    u32 length = 0;
    bool full = false;
    void put(const char* s) { while (*s) byte(*s++); }
    void byte(char c) {
        if (length < kTextBytes) text[length++] = c;
        else full = true;
    }
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

void write_companion(u32 bytes, u32 crc) {
    Writer w;
    w.put("{\n  \"format\": \"open-xy-project\",\n  \"project\": {\n    \"bytes\": ");
    w.number(bytes);
    w.put(",\n    \"crc32\": \"");
    w.hex(crc);
    w.put("\"\n  },\n  \"sections\": {\n    \"open-xy.machines\": {\n      \"machines\": {");
    u32 listed = 0;
    for (const CompanionMachine* m = sdk_companion_machines; m->id; ++m) {
        const u32 number = value(m->number);
        if (!number) continue;  // a kit machine this selection left out
        w.put(listed++ ? ",\n        \"" : "\n        \"");
        w.put(m->id);
        w.put(m->effect ? "\": {\n          \"effect\": " : "\": {\n          \"engine\": ");
        w.number(number);
        w.put(",\n          \"page\": ");
        w.number(value(m->page));
        w.put("\n        }");
    }
    w.put(listed ? "\n      },\n" : "},\n");
    w.put("      \"profile\": \"opxy-1.2.0\",\n      \"version\": 1\n    }\n  },\n  \"version\": 1\n}\n");
    if (w.full) return;
    File* file = file_open(companion, "wb");
    if (!file) return;
    file_write(text, w.length, 1, file);
    file_close(file);
}
}

u32 sdk_project_save(void* project, const char* path) {
    const u32 saved = save(project, path);
    u32 bytes, crc;
    if (saved && path && companion_path(path) && measure(path, bytes, crc)) write_companion(bytes, crc);
    return saved;
}
