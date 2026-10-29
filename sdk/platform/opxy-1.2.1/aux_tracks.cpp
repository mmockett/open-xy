// Track control machines on aux tracks (issue #220). A machine whose
// trackControl lists "aux" tracks can take over one of them, chosen with
// Shift + M1 on that track as an engine is on tracks 1-8; the list's first
// entry is the track's stock role. While a machine holds the track, its M1 page
// shows the machine's own page, its knobs edit the machine's controls
// (synth_controls.cpp), track control runs it there (track_control.cpp) and the
// stock role is kept silent. M2..M4 stay stock. Part of the control service.
//
// Stock OS 1.2.1, GUI side. The aux controller (method table 0803fb6c) serves
// tracks 8..15; the selected track is the byte at 840a0534 and its page tab
// (M1..M4 as 0..3) the word at 0801eae4.
// - Its shifted-key method (8c08f6b0) ignores Shift + M1..M4. Its Shift + M1
//   case (8c08f6ec) is hooked (aux_hooks.s): on a track a machine can take
//   over, it opens the chooser, titled "engine"; on the sends (14, 15), the
//   effect list, as their Shift + track key does (8c08f60c), so Shift + M1
//   chooses what runs on every aux track that has a choice.
// - Shift + track key reaches 8c08f60c(controller, track), which opens the
//   effect list on the sends only. The hook after its LINK (8c08f610) lists the
//   presets for the track from content/aux_presets, titled "preset", as Shift +
//   track key does on tracks 1-8 (below).
// Both lists show the track's number under the title, as stock's do.
// - Its page method 8c08f53c answers the screen page for the selected track
//   and tab; the hook at 8c08f540 answers the machine's page on tab 0.
// - The aux LFO names its four engine destinations from a fixed table
//   ("param1".."param4", 84056d50) in 8d53d160(track, group, slot); the hook at
//   8d53d2a0 names them after the machine holding the track instead, through
//   the engine record's names (8d506c4c), as tracks 1-8 name theirs.
//
// Taking a track over keeps the stock role's eight controls aside and gives the
// machine its defaults, through stock's set (8d5acf28), which stores and
// publishes them; giving it back restores them. The choice and the role's
// controls are kept with the project (track_control_project.cpp); a new project
// starts with every track stock.
//
// Silencing, audio side. GUI and audio share no memory: a held track carries
// kMark in its last live word, which stock never locks, and the GUI sets it
// each tick through stock's live set (8d5acea8), which publishes it. It is
// never a stored value: stock's base setter does not bound the index, and the
// project must not keep it. The audio hooks read it from the audio side's copy
// of the track's words (84eab39c + track * 0xe8). Stock resets live values now
// and then (when the sequencer starts, for one), so the audio side stays
// silent until the mark has been missing for kHeldCalls of its calls.
// - CV (track 12, index 11): the audio loop's CV output (8d50f1cc, from
//   8d512214) sets the voltage from word 0 and its mode from word 14, and a
//   note on raises the gate (8d5b5fe0, from 8d50e95c). Both are skipped while
//   held. When the track is taken, the audio side clears stock's held CV notes
//   (8d5b59a0) and lowers the gate (8d5b5f78(0)) once.
#include "track_control.hpp"
#include "stock.hpp"
#include "stock_browser.hpp"
#include "synth_controls.hpp"

extern "C" {
void sdk_aux_shift_m1() __asm__("sdk_aux_shift_m1");
opxy::u32 sdk_aux_shift_track(opxy::u32 controller, opxy::u32 track) __asm__("sdk_aux_shift_track");
opxy::i32 sdk_aux_page() __asm__("sdk_aux_page");
const char* sdk_aux_lfo_label(opxy::u32 track, opxy::u32 slot) __asm__("sdk_aux_lfo_label");
void sdk_cv_output(opxy::u32) __asm__("sdk_cv_output");
void sdk_cv_gate_on(opxy::u32, opxy::u32, opxy::u32) __asm__("sdk_cv_gate_on");
// For track_control_project.cpp: the machine ID holding each aux track, and
// the stock role's controls kept aside, to save; and their restoration on load.
}

namespace {
using opxy::i32;
using opxy::u32;
using namespace opxy::platform;
namespace control = opxy::control;
namespace browser = opxy::platform::stock_browser;

const u32 kProject = 0x86bd027c;
const u32 kAuxTab = 0x0801eae4;
const u32 kFirstAux = 8;
const u32 kControls = 8;
// The aux tracks a machine can take over: those whose stock role has a silencer.
const u32 kCv = 11;
const u32 kTakeable = 1u << kCv;
const char* role_name(u32 track) { return track == kCv ? "cv" : nullptr; }

// The last live word, which carries the mark, and the mark: "OXAU".
const u32 kMarkWord = control::track_parameters - 1;
const i32 kMark = 0x4f584155;

// Each aux track's holder, as 1 + its index in sdk_control_machines, or 0 for
// the stock role; and the role's controls while a machine holds it.
u32 holders[control::all_tracks];
i32 kept[control::all_tracks][kControls];
// Whether this GUI set a track's mark (so it clears only its own).
bool marked[control::all_tracks];

bool takeable(u32 track) { return track < control::all_tracks && (kTakeable >> track & 1); }
const ControlMachine* holder(u32 track) {
    if (!takeable(track) || !holders[track] || holders[track] > sdk_control_machine_count) return nullptr;
    const ControlMachine& m = sdk_control_machines[holders[track] - 1];
    return m.ready() == 1 ? &m : nullptr;
}

i32 base(u32 track, u32 index) {
    return static_cast<i32>(word(stock<u32(u32)>(0x8d587708)(track) + 0x3864 + index * 4));
}
void param_set(u32 track, u32 index, i32 value) {
    stock<void(u32, u32, u32, i32)>(0x8d5acf28)(kProject, track, index, value);
}
i32 live(u32 track, u32 index) {
    return *at<const volatile i32>(kProject + 0x4080 + (track * control::track_parameters + index) * 4);
}
void live_set(u32 track, u32 index, i32 value) {
    stock<void(u32, u32, u32, i32)>(0x8d5acea8)(kProject, track, index, value);
}

// Publishes a held track's mark before anything else reaches the audio side,
// so the role stays silent from the first word the machine sets.
void mark(u32 track) {
    if (live(track, kMarkWord) != kMark) live_set(track, kMarkWord, kMark);
    marked[track] = true;
}
void unmark(u32 track) {
    if (!marked[track]) return;
    if (live(track, kMarkWord) == kMark) live_set(track, kMarkWord, base(track, kMarkWord));
    marked[track] = false;
}

// Gives the track to machine `choice` (1 + its index) or back to its role (0);
// the next GUI tick clears the mark of a track given back.
void take(u32 track, u32 choice) {
    if (choice == holders[track]) return;
    if (!holders[track])
        for (u32 i = 0; i < kControls; ++i) kept[track][i] = base(track, i);
    holders[track] = choice;
    const ControlMachine* m = holder(track);
    if (m) mark(track);
    for (u32 i = 0; i < kControls; ++i) param_set(track, i, m ? m->defaults[i] : kept[track][i]);
}

// The chooser: the stock role, then each machine that may take the track over.
const u32 kChoices = 8;
browser::StockSelector<kChoices> chooser = {};
u32 chooser_track, chooser_machines[kChoices];
void choose(void*, u32 index) { take(chooser_track, chooser_machines[index]); }

bool open_chooser(u32 track) {
    const char* labels[kChoices] = {role_name(track)};
    u32 count = 1, initial = 0;
    chooser_machines[0] = 0;
    for (u32 i = 0; i < sdk_control_machine_count && count < kChoices; ++i) {
        const ControlMachine& m = sdk_control_machines[i];
        if (!(m.aux >> track & 1) || !m.page || m.ready() != 1) continue;
        if (holders[track] == i + 1) initial = count;
        chooser_machines[count] = i + 1;
        labels[count++] = m.name;
    }
    if (count == 1) return false;  // nothing to choose: M1 as stock
    chooser_track = track;
    return chooser.open("engine", labels, count, initial, browser::Callbacks{nullptr, choose, nullptr},
                        static_cast<i32>(track));
}

// Presets for aux tracks, as files: those the builder installs in
// content/aux_presets/<folder>/<name>.json, then the user's in
// /fat32/aux_presets/<folder>/<name>.json, which MTP shows (allowlist edits in the
// profile) and stock's instrument browser never reads: it lists any .json in
// /fat32/presets' folders. Each names what it is for in "type"
// (a machine's ID, or a stock role such as "cv") and its eight controls in
// "params" (0..32767, as instrument presets store them). Shift + track key on a
// track a machine can take over lists, as stock's preset browser does, the
// folders and in each the presets that can run on the track; choosing one gives
// the track to its machine (or role) and sets the controls.
//
// Stock's folder calls, as its preset scan (8d56db50) makes them: 8c0be54a(path)
// opens a folder (0 when it can't), 8c0be6a4(folder) gives the next entry (its
// name inline at +0xb) or 0, 8c0be7b2(folder) closes it; newlib's fopen family
// reads the files.
void* folder_open(const char* path) { return stock<void*(const char*)>(0x8c0be54a)(path); }
const char* folder_next(void* folder) {
    const char* entry = stock<const char*(void*)>(0x8c0be6a4)(folder);
    return entry ? entry + 0xb : nullptr;
}
void folder_close(void* folder) { stock<void(void*)>(0x8c0be7b2)(folder); }
void* file_open(const char* path) { return stock<void*(const char*, const char*)>(0x8fed0348)(path, "rb"); }
u32 file_read(void* data, u32 bytes, void* file) {
    return stock<u32(void*, u32, u32, void*)>(0x8fed05bc)(data, 1, bytes, file);
}
void file_close(void* file) { stock<int(void*)>(0x8fed00ae)(file); }

const u32 kFolders = 16, kPresets = 64, kNameBytes = 16, kPathBytes = 128, kPresetBytes = 2048;
const char* const kPresetRoots[] = {"content/aux_presets", "/fat32/aux_presets"};
struct Preset { u32 choice; i32 values[kControls]; };
browser::detail::Row folders[kFolders];
browser::detail::Item items[kPresets];
Preset presets[kPresets];
browser::detail::Session preset_session;
u32 preset_track, preset_count, picked = kPresets;
char text[kPresetBytes];

// a + "/" + b into path; false when it doesn't fit.
bool join(char* path, const char* a, const char* b) {
    u32 n = 0;
    for (const char* p = a; *p; ++p) { if (n + 2 >= kPathBytes) return false; path[n++] = *p; }
    path[n++] = '/';
    for (const char* p = b; *p; ++p) { if (n + 1 >= kPathBytes) return false; path[n++] = *p; }
    path[n] = 0;
    return true;
}
bool same(const char* a, const char* b, u32 n) {
    for (u32 i = 0; i < n; ++i) if (a[i] != b[i]) return false;
    return !b[n];
}

// What a preset's "type" names, for this track: 1 + a machine's index, 0 for the
// track's stock role, or -1 when it can't run here.
i32 choice_for(const char* type, u32 length, u32 track) {
    if (role_name(track) && same(type, role_name(track), length)) return 0;
    for (u32 i = 0; i < sdk_control_machine_count; ++i) {
        const ControlMachine& m = sdk_control_machines[i];
        if ((m.aux >> track & 1) && m.page && m.ready() == 1 && same(type, m.name, length))
            return static_cast<i32>(i + 1);
    }
    return -1;
}

// The span of the string or array after "key": in text[0, length), or false.
bool field(u32 length, const char* key, u32& at) {
    for (u32 i = 0; i + 1 < length; ++i) {
        if (text[i] != '"') continue;
        u32 k = 0;
        while (key[k] && i + 1 + k < length && text[i + 1 + k] == key[k]) ++k;
        if (key[k] || i + 1 + k >= length || text[i + 1 + k] != '"') continue;
        u32 j = i + 2 + k;
        while (j < length && (text[j] == ' ' || text[j] == ':' || text[j] == '\n' || text[j] == '\t' || text[j] == '\r')) ++j;
        if (j < length) { at = j; return true; }
    }
    return false;
}

// Reads one preset file: true with its choice and controls when it can run on track.
bool read_preset(const char* path, u32 track, Preset& preset) {
    void* file = file_open(path);
    if (!file) return false;
    const u32 length = file_read(text, kPresetBytes - 1, file);
    file_close(file);
    if (!length || length >= kPresetBytes - 1) return false;
    u32 at, end;
    if (!field(length, "type", at) || text[at] != '"') return false;
    for (end = at + 1; end < length && text[end] != '"'; ++end) {}
    const i32 choice = choice_for(text + at + 1, end - at - 1, track);
    if (choice < 0 || !field(length, "params", at) || text[at] != '[') return false;
    preset.choice = static_cast<u32>(choice);
    u32 n = 0, i = at + 1;
    while (n < kControls) {
        while (i < length && (text[i] == ' ' || text[i] == ',' || text[i] == '\n' || text[i] == '\t' || text[i] == '\r')) ++i;
        if (i >= length || text[i] < '0' || text[i] > '9') break;
        u32 v = 0;
        while (i < length && text[i] >= '0' && text[i] <= '9') v = v > 32767 ? v : v * 10 + static_cast<u32>(text[i++] - '0');
        if (v > 32767) return false;
        preset.values[n++] = static_cast<i32>(v << 16);
    }
    return n == kControls;
}

bool before(const browser::detail::String& a, const browser::detail::String& b) {
    for (u32 i = 0; i < a.length && i < b.length; ++i)
        if (a.text[i] != b.text[i]) return a.text[i] < b.text[i];
    return a.length < b.length;
}

// The folders of both roots with at least one preset that can run on this track,
// the installed ones first. Returns the folder count.
u32 scan_root(const char* root_path, u32 track, u32 count) {
    void* root = folder_open(root_path);
    if (!root) return count;
    char folder_path[kPathBytes], file_path[kPathBytes];
    for (const char* name; count < kFolders && (name = folder_next(root));) {
        if (name[0] == '.' || !join(folder_path, root_path, name)) continue;
        void* folder = folder_open(folder_path);
        if (!folder) continue;
        browser::detail::Row& row = folders[count];
        row = browser::detail::Row{};
        if (!browser::detail::name(row.name, name)) { folder_close(folder); continue; }
        row.id = static_cast<i32>(count);
        browser::detail::Item* first = items + preset_count;
        for (const char* file; preset_count < kPresets && (file = folder_next(folder));) {
            u32 n = 0;
            while (file[n]) ++n;
            if (file[0] == '.' || n < 6 || n - 5 >= kNameBytes || !same(file + n - 5, ".json", 5)) continue;
            if (!join(file_path, folder_path, file) || !read_preset(file_path, track, presets[preset_count])) continue;
            char label[kNameBytes];
            for (u32 i = 0; i < n - 5; ++i) label[i] = file[i];
            label[n - 5] = 0;
            browser::detail::Item& item = items[preset_count];
            item = browser::detail::Item{};
            if (!browser::detail::name(item.name, label)) continue;
            // Preset ids follow folder ids, so a highlight tells the two apart.
            item.id = static_cast<i32>(kFolders + preset_count);
            ++preset_count;
        }
        folder_close(folder);
        browser::detail::Item* last = items + preset_count;
        if (first == last) continue;  // nothing for this track in it
        row.items = browser::detail::Vector<browser::detail::Item>{0, first, last, last};
        ++count;
    }
    folder_close(root);
    return count;
}

u32 scan_presets(u32 track) {
    preset_count = 0;
    u32 count = 0;
    for (const char* root : kPresetRoots) count = scan_root(root, track, count);
    // By name, as stock lists its folders; each row keeps its own items.
    for (u32 i = 1; i < count; ++i)
        for (u32 j = i; j > 0 && before(folders[j].name, folders[j - 1].name); --j) {
            const browser::detail::Row row = folders[j];
            folders[j] = folders[j - 1];
            folders[j - 1] = row;
        }
    for (u32 i = 0; i < count; ++i) folders[i].id = static_cast<i32>(i);
    return count;
}

// Stock reports highlights in both columns; moving onto a folder highlights its
// first preset. Whatever the mode, the last preset highlighted is the one a press
// of knobs 2-4 applies.
void preset_highlight(void*, u32, i32 id) {
    if (id >= static_cast<i32>(kFolders) && id < static_cast<i32>(kFolders + preset_count))
        picked = static_cast<u32>(id) - kFolders;
}
void apply_preset(void*, u32) {
    if (picked >= preset_count) return;
    const Preset& preset = presets[picked];
    take(preset_track, preset.choice);
    for (u32 i = 0; i < kControls; ++i) param_set(preset_track, i, preset.values[i]);
}

bool open_presets(u32 track) {
    if (!takeable(track)) return false;
    const u32 count = scan_presets(track);
    if (!count) return false;
    preset_track = track;
    picked = kPresets;
    u32 initial = 0;
    if (const ControlMachine* m = holder(track))
        for (u32 i = 0; i < count; ++i)
            if (same(folders[i].name.text, m->name, folders[i].name.length)) initial = i;
    return browser::detail::open_rows(preset_session, folders, count, "preset", initial,
                                      browser::Callbacks{nullptr, apply_preset, nullptr, preset_highlight},
                                      static_cast<i32>(track), true);
}

// The audio side's copy of a track's words, as stock's audio loop reads them.
const volatile i32* audio_words(u32 track) { return at<const volatile i32>(0x84eab39c + track * 0xe8); }
// Audio side: CV output calls (one per audio block) since the CV track's mark
// was last seen, capped; it counts as held for kHeldCalls of them, far longer
// than the GUI takes to publish the mark again after a stock reset.
const u32 kHeldCalls = 512;
u32 cv_missing = kHeldCalls;
bool cv_held() {
    if (audio_words(kCv)[kMarkWord] == kMark) cv_missing = 0;
    else if (cv_missing < kHeldCalls) ++cv_missing;
    return cv_missing < kHeldCalls;
}
bool cv_silenced;  // audio side: the held CV notes were cleared and the gate lowered
}

namespace {
// The service's entry points (track_control.hpp's AuxService).
u32 aux_engine(u32 track) {
    const ControlMachine* m = track >= kFirstAux ? holder(track) : nullptr;
    return m ? m->engine() : 0;
}

void aux_tick() {
    for (u32 t = kFirstAux; t < control::all_tracks; ++t) {
        if (!takeable(t)) continue;
        if (holder(t)) mark(t);
        else unmark(t);
    }
}
}

// 8c08f6ec: Shift + M1 on an aux track, which stock ignores. A track a
// machine can take over opens the chooser; a send opens its effect list
// through stock's Shift + track key (8c08f60c, from 8c091e24 with the aux
// controller), whose hook passes the sends on to stock.
const u32 kAuxController = 0x08014db4;
void sdk_aux_shift_m1() {
    const u32 track = byte(kSelectedTrack);
    if (takeable(track)) open_chooser(track);
    else if (track == 14 || track == 15) stock<void(u32, u32)>(0x8c08f60c)(kAuxController, track);
}

// 8c08f610: Shift + track key on an aux track. On one a machine holds, its
// presets; 1 when the list opened, else 0 and stock goes on (the sends' effects).
u32 sdk_aux_shift_track(u32, u32 track) { return open_presets(track) ? 1 : 0; }

// Whether the machine holding a track hides one of its pages ("hidePages").
bool hides(const ControlMachine& m, u32 tab) {
    for (u32 i = 0; i < sdk_synth_control_count; ++i) {
        const SynthControls& c = sdk_synth_controls[i];
        if (c.engine() == m.engine() && !c.effect) return c.hidden_pages >> tab & 1;
    }
    return false;
}

// 8c08f540: the selected aux track's page on tab 0 while a machine holds it, and
// on a tab it hides until the GUI tick sets the tab back (synth_pages.cpp); or
// -1 for stock's.
i32 sdk_aux_page() {
    const u32 tab = word(kAuxTab);
    const ControlMachine* m = holder(byte(kSelectedTrack));
    return m && (tab == 0 || (tab < 4 && hides(*m, tab))) ? static_cast<i32>(m->page_id()) : -1;
}

// 8d53d2a0: the name of the aux LFO's engine destination `slot` (0..3, the
// track's words 0..3) while a machine holds `track`: that parameter's name in
// the machine's engine record, as an instrument track's LFO shows it; else 0
// for stock's "param1".."param4".
const char* sdk_aux_lfo_label(u32 track, u32 slot) {
    const ControlMachine* m = slot < opxy::page_controls ? holder(track) : nullptr;
    return m ? stock<const char*(u32, u32)>(0x8d506c4c)(m->engine(), slot) : nullptr;
}

// 8d512214, audio side: CV output, unless a machine holds the CV track.
void sdk_cv_output(u32 argument) {
    if (!cv_held()) {
        cv_silenced = false;
        stock<void(u32)>(0x8d50f1cc)(argument);
        return;
    }
    if (cv_silenced) return;
    stock<void()>(0x8d5b59a0)();
    stock<void(u32)>(0x8d5b5f78)(0);
    cv_silenced = true;
}

// 8d50e95c, audio side: a note on the CV track raises the gate, unless held.
void sdk_cv_gate_on(u32 a, u32 b, u32 c) {
    if (cv_missing >= kHeldCalls && audio_words(kCv)[kMarkWord] != kMark)
        stock<void(u32, u32, u32)>(0x8d5b5fe0)(a, b, c);
}

namespace {
const char* aux_holder(u32 track) {
    const ControlMachine* m = holder(track);
    return m ? m->name : nullptr;
}
const i32* aux_kept(u32 track) { return kept[track]; }

// After a project loads: the machine named `id` holds `track` again, with the
// role's controls as kept; a machine this build lacks leaves the role in place.
// Called from inside stock's project load, while the project is not yet in
// use (stock asserts if its track data is touched then, data.cpp:985): it only
// records the holder; the next GUI tick marks the track.
void aux_restore(u32 track, const char* id, u32 id_length, const i32* controls) {
    if (!takeable(track)) return;
    for (u32 i = 0; i < sdk_control_machine_count; ++i) {
        const ControlMachine& m = sdk_control_machines[i];
        u32 n = 0;
        while (n < id_length && m.name[n] == id[n]) ++n;
        if (n != id_length || m.name[n] || !(m.aux >> track & 1) || m.ready() != 1) continue;
        holders[track] = i + 1;
        for (u32 c = 0; c < kControls; ++c) kept[track][c] = controls[c];
        return;
    }
}

// Before a project loads, and for a new one: every aux track has its stock role.
// Records only, as aux_restore does.
void aux_forget() {
    for (u32 t = 0; t < control::all_tracks; ++t) holders[t] = 0;
}
}

// On the GUI role at startup (startup.cpp): track control, the project
// companion and synth_controls.cpp, a shared unit, reach this service through
// these pointers only.
void sdk_aux_startup() {
    sdk_aux = AuxService{aux_engine, aux_tick, aux_holder, aux_kept, aux_restore, aux_forget};
    sdk_aux_engine = aux_engine;
}
