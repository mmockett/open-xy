# Module probe (experiment, local only)

Can machines load from files instead of the boot partition? This probe answers
the two questions that decide it, on the device:

1. Can the module read a file when machines register at startup (synth
   initializer, audio and GUI roles; screen factory), and which path form
   reaches the files MTP shows?
2. Can the module copy code it read into its own memory, sync the caches and
   call it?

Code: `sdk/platform/opxy-1.2.1/module_probe.cpp` and `module_probe_cache.s`,
called from `startup.cpp.in` (stages 1–3) and `project_companion.cpp`
(stage 4, project save). Branch `exp/module-file-probe`, from
`open-xy-aux-samples` at `1f38077`, the tree behind the `combined-*` builds.

## Build

`module-probe-004` is `combined-004` (on the device as of 2026-10-29; Lint's
controls reordered) plus the probe: the same layout, catalog, screens and
machine objects. Only `os.ldr` differs (sha256 `942fc2d3…`; combined-004 is
`30914b34…`), and only `startup.o` and `project_companion.o` change, plus the
two probe objects. The Nonagon session built it and stages it; earlier probe
builds (002, 003) match firmware no longer on the device.
If the device moves on to another build, rebuild the probe from that build's
collection and layout.

```sh
python tools/module_probe/make_probe_files.py <out>   # probe-read.bin, probe-run.bin
```

| File | Flags | Does |
| --- | --- | --- |
| `probe-read.bin` | 1 | GUI stages read it; the audio role reads it once the GUI has found it |
| `probe-run.bin` | 3 | Also copies 16 bytes of code into the module, syncs caches, calls it with `11111111`; expects `0325476c` |

Without a probe file the GUI stages still try four `fopen` calls each, and
write nothing.

## On the device

Every step needs the player's go-ahead, and the device is shared: coordinate
with the session that owns the combined firmware first.

1. te-boot MTP; back up `/os.ldr` and `/layout.json` (docs/device.md in OP-XY-Dev).
2. Copy the probe's `os.ldr` to `/os.ldr` and `probe-read.bin` to `/probe.bin`;
   read `os.ldr` back and compare its hash. Write the OS: power off, hold
   COM while switching on, track 3 (dev menu), track 5; power-cycle. (Track 3
   then track 2 is MTP.)
3. Boot normally, open a project, play a custom synth, save a project.
4. te-boot MTP; `get /probe-log.txt`; also the newest `/logs/log_*.txt`.
5. If the log shows reads at startup: replace `/probe.bin` with
   `probe-run.bin`, restart (no OS write), read the log again.
6. Undo: put the backed-up `os.ldr` back, write the OS as in step 2, remove
   `/probe.bin` and `/probe-log.txt`.

The log is also tried at `/fat32/projects/probe-log.oxy` (normal-mode MTP)
when the probe was found as `/fat32/projects/probe.oxy`.

## Reading the log

`found:` is the path form that opened. Events list `sequence stage gui
opened-forms bytes crc32`; stages are 1 synth initializer on the audio role,
2 on the GUI role, 3 screen factory, 4 project save. `opened-forms` is a bit
mask over `probe.bin`, `/yaffs2/probe.bin`, `/probe.bin`,
`/fat32/projects/probe.oxy`. The CRC of `probe-read.bin` is `dd7f4962`.

| Log | Means |
| --- | --- |
| Stage 2 opened in the first events | Files can be read before any synth registers: a boot loader can work as designed |
| Only stage 3 or 4 opened | Files become readable later; a loader has to load at the screen factory or delay registration |
| No log; device boots | No path form reached the file. Try the other forms, or another location |
| Device halts at boot | `fopen` is unsafe that early. Undo from te-boot (it does not depend on the OS) |
| `run: state 1` and the device halted | Code written to module memory can't be fetched (CPLB or caches) |
| `run: state 2 … (match)` | Code read from a file runs in the module |

## Results: module-probe-004, 2026-10-29

Run by the Nonagon session with the player's approval. Logs:
`~/Library/Application Support/OPXY-SDK/private/device-backup-20261029-probe4/`
(`probe-log-read.txt`, `probe-log-run.txt`, device logs `dev-2302.txt`,
`dev-2307.txt`). The device booted normally both times and the custom synths
played.

| Test | Result |
| --- | --- |
| Read (`probe-read.bin`) | Found as `probe.bin` (relative; mask `1`), the root te-boot MTP shows. Read at stage 2, the synth initializer on the main board, then at the screen factory and on every save: 48 bytes, CRC `dd7f4962` |
| Run (`probe-run.bin`, restart only) | `run: state 2, bytes 16, result 0325476c, expected 0325476c (match)`, CRC `58876bad` |

**The OP-XY has two processors, each with its own copy of the module.** The
device log interleaves two streams: `bfm`, the main board (screen, USB, yaffs
and fat32, "dsp board revision is 5"), and `I`, the DSP board, which renders
audio. The SDK's audio role is the DSP board, so `synth-audio 0` doesn't mean
the audio role never ran. Its counters live in the DSP's memory, and the DSP
never found the file itself, because the probe let it read only after a find
in its own memory. Stage 1 is untested.

What the boot order shows (`timing_log`): on the main board the filesystem
starts at 2.6 s and engines at 7.6 s (stage 2), screens at 9.0 s (stage 3),
then "synchronize cpus". The DSP board starts its filesystem at 1.6 s and
engines at 8.0 s, and its own `filemanager` tries to open
`user/tunings/user*.scl` (relative paths). So the DSP has some file access
before its engines start, perhaps through the main board. Whether newlib
`fopen` reaches it from the module is not tested.

The `producer_factory: engine type <id> not registered` lines appear on both
boards at every boot here, while the machines work. They aren't proof of a
failed install on their own.

What this settles for a loader:

- Main board: proven. A file at the te-boot MTP root can be read before any
  synth registers, and code copied from it into module memory runs after a
  cache flush. Screens and GUI code could load from files today.
- DSP board: open. Voices render there, so a loader has to work there too.
  The next probe lets the DSP read on its own at stage 1 and report back. It
  can't write the log through the main board's memory, so it needs its own
  channel: its own `probe-log-dsp.txt` through newlib, or the stock logger
  that writes the `I` lines (address not yet found).

Not verified: whether `/yaffs2` and `/fat32` are mount names (they come from
strings in the stock OS); the forms other than `probe.bin` were not reached
because the first form opened.
