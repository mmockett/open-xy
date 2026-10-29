# Module probe (experiment, local only)

Can machines load from files instead of the boot partition? The probe answers
the questions that decide it, on the device, on both of its processors: the
main board (the SDK's GUI role) and the DSP board (the audio role, which
renders voices). Each runs its own copy of the module.

1. Can the module read a file when machines register at startup, and which
   path form reaches the files MTP shows?
2. Can the module copy code it read into its own memory, sync the caches and
   call it?

Code: `sdk/platform/opxy-1.2.1/module_probe.cpp` and `module_probe_cache.s`,
called from `startup.cpp.in` (stages 1–3) and `project_companion.cpp`
(stage 4, project save). Branch `exp/module-file-probe`, from
`open-xy-aux-samples` at `1f38077`, the tree behind the `combined-*` builds.

| Stage | Board | Where |
| --- | --- | --- |
| 1 | DSP | synth initializer ("init engines", about 8.0 s) |
| 2 | main | synth initializer (about 7.6 s) |
| 3 | main | screen factory (about 9.0 s) |
| 4 | main | project save |

Every stage tries each path form until one opens, then keeps to that form.
Each stage reports through stock's logger (`0x8c042efc`, as
`producer_factory.cpp` calls it), so both boards' results land in
`/logs/log_*.txt`. The main board also writes `probe-log.txt` beside the file;
the DSP board never writes files.

## Probe 2: module-probe-dsp-001

`module-probe-dsp-001` is `combined-004` plus probe 2: the same layout,
catalog, screens and machine objects. Only `os.ldr` differs (sha256
`698d4c77…`; combined-004 is `30914b34…`), and only `startup.o` and
`project_companion.o` change, plus the two probe objects. If the device's
firmware has moved on from combined-004, rebuild from that build's collection
and layout first.

```sh
python tools/module_probe/make_probe_files.py <out>
```

| File | Flags | CRC-32 | Does |
| --- | --- | --- | --- |
| `probe-read.bin` | 0 | `723bdb25` | Both boards read it |
| `probe-run.bin` | 2 | `f7c3f9ea` | The main board also copies 16 bytes of code into its module, syncs caches, calls it with `11111111`; expects `0325476c` |
| `probe-run-dsp.bin` | 6 | `2742ba35` | Both boards run the code |

Without a probe file each stage only tries four `fopen` calls. The DSP board's
stage 1 now tries them unconditionally: that is the test.

## On the device

Every step needs the player's go-ahead, and the device is shared: coordinate
with the session that owns device installs first.

1. te-boot MTP; back up `/os.ldr` and `/layout.json` (docs/device.md in OP-XY-Dev).
2. Copy the probe's `os.ldr` to `/os.ldr` and `probe-read.bin` to `/probe.bin`;
   read `os.ldr` back and compare its hash. Write the OS: power off, hold
   COM while switching on, track 3 (dev menu), track 5; power-cycle. (Track 3
   then track 2 is MTP.)
3. Boot normally, play a custom synth, save a project.
4. te-boot MTP; get the newest `/logs/log_*.txt` and `/probe-log.txt`;
   `grep module_probe` the log.
5. If the DSP read the file: replace `/probe.bin` with `probe-run-dsp.bin`,
   restart (no OS write), boot, play a synth, read the log again.
6. Undo: put the backed-up `os.ldr` back, write the OS as in step 2, remove
   `/probe.bin` and `/probe-log.txt`.

## Reading the results

Log lines: `module_probe[<stage>]: probe <board> stage start`, then
`probe <board> call <n>: opened <mask> bytes <n> crc <crc32> flags <f> run
<state> <result>/<expected> [match]`. `opened` is a bit mask over
`probe.bin`, `/yaffs2/probe.bin`, `/probe.bin`, `/fat32/projects/probe.oxy`.
Run state 0 is none, 1 about to call, 2 returned.

| DSP log (stage 1) | Means |
| --- | --- |
| `opened 00000001 … crc 723bdb25` | The DSP board reads files at engine start: a loader can work on both boards |
| `opened 00000000`, main board found it | newlib on the DSP doesn't reach the files. A loader has to get code to the DSP another way (the main board, over the bridge) |
| `stage start` and no `call` line; device halts | `fopen` hangs or faults on the DSP at that point. Undo from te-boot |
| No `module_probe[1]` line at all | The DSP's logger lines didn't arrive, or stage 1 didn't run: inconclusive |
| `probe about to run code` last, device halted | The DSP can't fetch code written to module memory |
| `run 2 0325476c/0325476c match` | Code read from a file runs on the DSP |

## Results, probe 2: module-probe-dsp-001, 2026-10-29

Run by the Nonagon session with the player's approval; combined-004 restored
afterwards. Logs: `~/Library/Application Support/OPXY-SDK/private/device-backup-20261029-probe-dsp/`
(`dev-read.txt`, `dev-run.txt`, `probe-log-read.txt`, `probe-log-run.txt`).
Both boots were normal.

| Test | Board | Log |
| --- | --- | --- |
| Read | DSP, stage 1 | `8.016.510: I module_probe[1]: probe dsp call 1: opened 00000001 bytes 48 crc 723bdb25`; its timing log puts "init engines" at 8.015530 s |
| Read | main, stages 2, 3, 4 | 7.635 s, 9.075 s, each save; form `00000001`, 48 bytes, `723bdb25` |
| Run | DSP, stage 1 | `probe about to run code`, then `run 2 0325476c/0325476c match` at 2.880 s (a faster boot; engines at 2.879 s) |
| Run | main, stages 2, 3, 4 | `match` at each |

**Both processors can read a file from the te-boot MTP root (`probe.bin`)
at the moment their engines start, before any synth registers, and run code
copied from it into module memory after a cache flush.** The mechanism a
loader needs works on the OP-XY as it is. Stock's logger at `0x8c042efc`
carries `I` (DSP) and `bfm` (main) lines from module code into `/logs`.

What a loader still has to settle (not tested here): loading the whole
machine code, not 16 bytes; whether file reads are fast enough at boot for
about 1 MB per board; hooks that keep stock behaviour when no module file is
present; and both boards loading the same file version.

## Results, probe 1: module-probe-004, 2026-10-29

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
  Probe 2 (above) lets the DSP read on its own at stage 1 and report
  through stock's logger.

Not verified: whether `/yaffs2` and `/fat32` are mount names (they come from
strings in the stock OS); the forms other than `probe.bin` were not reached
because the first form opened.
