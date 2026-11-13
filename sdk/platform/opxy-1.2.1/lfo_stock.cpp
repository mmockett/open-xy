#include "lfo.hpp"
#include "stock.hpp"

namespace opxy { namespace platform { namespace lfo {
u32 gui_engine(u32 track) { return track_engine(track); }
u32 audio_engine(u32 track) { return word(0x11b01c48 + track * 4); }
u32 type(u32 track) { return stock<u32(u32)>(0x8d585fb8)(track); }
u32 selected_track() { return stock<u32()>(0x8d4d78cc)(); }
u32 animate(u32 controller, u32 position, i32 target, u32 duration) {
    return stock<u32(u32, u32, i32, u32)>(0x8d5fffa4)(controller, position, target, duration);
}
const char* name(u32 group, u32 slot, u32 flag) {
    return stock<const char*(u32, u32, u32)>(0x8d4d8940)(group, slot, flag);
}
void show(u32 widget) { *reinterpret_cast<volatile u8*>(widget + 0x5c) = 0; }
u32 group_value(u32 track) {
    return stock<u32(u32, u32, u32)>(0x8d5aac40)(0x86bd9f38, track, 26);
}
i32 step(u32 track, u32 index, i32 value, const i32* event) {
    return stock<i32(u32, u32, i32, const i32*)>(0x8d5ab618)(track, index, value, event);
}
i32 value(u32 project, u32 track, u32 index) {
    return stock<i32(u32, u32, u32)>(0x8d5aac40)(project, track, index);
}
u32 publish(u32 project, u32 track, u32 index, i32 value) {
    return stock<u32(u32, u32, u32, i32)>(0x8d5ab258)(project, track, index, value);
}
void modulation_slot(u32 index) {
    // 11a03f10 stores the destination here; 11a042d8 loads it for the
    // existing 8d4d7260 publication. Keep stock's timing and amount intact.
    *reinterpret_cast<volatile u32*>(0x84044a58) = index;
}
void modulate(u32 project, u32 track, u32 index, i32 amount) {
    stock<void(u32, u32, u32, i32)>(0x8d5182d4)(project, track, index, amount);
}
}}}
