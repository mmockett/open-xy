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
`open-xy-aux-samples` at `1f38077`, the tree behind `combined-001` and `combined-003`.

## Build

`module-probe-003` is `combined-003` (on the device from 2026-10-29 20:10)
plus the probe: the same layout, catalog, screens and machine objects. Only
`os.ldr` differs (sha256 `53bcd81b…`; combined-003 is `79a311ef…`), and
only `startup.o` and `project_companion.o` change, plus the two probe objects.
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
   read `os.ldr` back and compare its hash. Write the OS (track 2, track 5),
   power-cycle.
3. Boot normally, open a project, play a custom synth, save a project.
4. te-boot MTP; `get /probe-log.txt`; also the newest `/logs/log_*.txt`.
5. If the log shows reads at startup: replace `/probe.bin` with
   `probe-run.bin`, restart (no OS write), read the log again.
6. Undo: put the backed-up `os.ldr` back, write the OS, remove `/probe.bin`
   and `/probe-log.txt`.

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

Not verified: whether `/yaffs2` and `/fat32` are the mount names (they come
from strings in the stock OS), and whether `fopen` is safe on either role
during startup.
