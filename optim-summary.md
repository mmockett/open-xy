# CPU optimisation hand-over (2026-10-31)

From the CPU audit session, for the OP-XY orchestrator. No PRs were opened,
nothing is tagged or merged, and nothing has been put on a device or heard.
Use the `optim/*` branches. The same commits are also on `cpu/*` branches and
on each machine repo's `claude/catalog-update-open-xy-e2pws0`; those copies
can be deleted.

Each branch:
- starts from its repo's current `main` (the SDK branches from etcetc000
  `staging`, or from `sdk/effect-screens` where noted);
- passes `sdk.py test --machine … --cxx clang++ --host-cxx g++` for every
  machine it touches (`sdk.py check` for the SDK branches).

The audit's write-ups, measurements and patch files are on
mmockett/OP-XY-Dev PR #1, `drafts/cpu-audit/` (opened earlier under the
session's rules; not merged).

## What the CPU figures are

- **Device measurements:** only today's catalog records, in the "Device now"
  column below (`sdk.py profile` on hardware). Nothing in this work has been
  measured on a device.
- **Simulated cycles:** counted by a Blackfin simulator running each machine
  compiled with the SDK's native flags.
  - The toolchain was built from source (`blackfin-plus-gnu`), not the pinned
    bundle.
  - The simulator uses classic Blackfin timing (3 cycles per 32-bit multiply,
    5/9 per branch) and models no cache or DDR.
  - The device's count runs about 0.75–2.5x the simulated one, depending on
    the machine.
- **"Device after" figures are estimates**, scaled from simulated cycles by
  each audit's method. Only `sdk.py profile` gives the real figure.
- **Host estimates** (the SDK's `sdk.py test` CPU line) weren't used. They
  proved unreliable: Sand read 12,300 against 19,900 simulated and 31,500 on
  the device.
- **Bit-identical** means every output sample equals the original's. It was
  checked on host builds (g++ or clang, `-fwrapv` for Lint) across controls
  at 0/25/50/75/100, presets, 1 and 8 voices, moving controls and glides,
  with matching output hashes in the simulator. The audit session re-ran a
  sample of these itself.

## Branches

### Bit-identical (the sound doesn't change)

| Repo | Branch | Commit | Machines | Device now | Simulated | Device after (est.) |
| --- | --- | --- | --- | ---: | --- | ---: |
| OP-XY-MNMD | `optim/mmsaw-mmvo6-mmpar` | `fd2c4ac44bec7d6ab44c637e95ec0c1302936ad2` | mmsaw, mmvo6, mmpar (+ mmpulse, mmens, mmstat, mmdyn share files, output unchanged) | 4.06 / 3.42 / 3.41% | −62 / −61 / −43% | 1.4–2.5% |
| OP-XY-Hexagon | `optim/hexagon-bit-identical` | `f257b6957f2d0e2ad80eb80be7f01a00a41f2f09` | Bloom, Hollow, Wane, Moth | 6.24 / 4.90 / 4.10 / 4.91% | −49 / −41 / −18 / −6% | 3.6–4.1 / 3.3–3.8 / 3.5–3.7 / 4.7–4.9% |
| OP-XY-Nonagon | `optim/nonagon-bit-identical` | `e530448cddfbc5ba084be63f163f8c61ff8cbe62` | Sand, Cocoa, Butterfly, Mach, Fermi | 5.42 / 5.68 / 5.55 / 4.48 / 3.82% | −16 / −28 / −32 / −13 / −11% | ~4.7 / 4.3–4.5 / 4.0–4.2 / ~4.0 / ~3.5% |
| OP-XY-ModFX | `optim/lint-chroma` | `762e2fe148b545f8c57d16b062e29d7e4716e98b` | Lint, Chroma (builds on `sdk/effect-screens`) | 7.75%; 7.23% (10.76% heavy) | −56%; −3% (−8% heavy) | ~3.4%; ~7.0% (9.9%) |

What changed:
- **MNMD:** each synth's sample loop held more state than the registers could
  and ran as a software loop. It now runs as small out-of-line passes over a
  stack buffer, plus caches keyed on exact inputs and branch-free clamps.
  `voiceBytes`: mmvo6 112 → 164, mmpar 68 → 144.
- **Hexagon:**
  - Bloom: skips partials silent for the whole block; its note-on `start()`
    ran twice, which halved eight-note restarts once fixed.
  - Hollow: a per-voice mode-plan cache (`voiceBytes` 448 → 644).
  - Wane: rare branches marked as rare.
  - Moth: skips its modulator at FM 0.
  - All four: 16×16 multiplies, hardware loops.
- **Nonagon:**
  - out-of-line loops for hardware loops, and 16×16 multiplies;
  - Sand's seed jump-ahead at note-on (initialize + start 6,700 → 3,500
    cycles);
  - Mach's sweeping-root loop (worst blocks 55–60k → 35–40k cycles) and its
    note-on clear (1,086 → 287 cycles).
- **ModFX, Lint:**
  - The echo line is lowered as it's read when the working level falls,
    instead of the whole buffer at once. A fall block went from 147,735 to
    49,688 simulated cycles: this was the effects' restart spike.
  - Sections run over the block instead of a per-sample dispatch.
  - Lint's State is 192 → 408 bytes of 472, and `tables.h` and
    `make_tables.py` change layout.
- **ModFX, Chroma:** lowers only what each effect reads (fall blocks about 40%
  cheaper on the host), plus multiply and Gain changes.

### Sound-changing alternatives (each builds on the bit-identical branch above)

| Repo | Branch | Commit | What | Simulated | How much the sound changes |
| --- | --- | --- | --- | --- | --- |
| OP-XY-Hexagon | `optim/hollow-halfrate` | `61f5ab533202d439f19c25726cad8fa7db5ef272` | Every mode at half rate + a 19-tap half-band | a further −7 to −10%; est. 3.2% midpoint, 3.7% sweep | 48–61 dB below the signal (median 55); worst −43 dBFS on a Whisper chord's first block |
| OP-XY-Hexagon | `optim/moth-1x` | `b8b1f1bddf559840d41fa63491c4453302a73589` | 1× rate; governor holds bandwidth under 20 kHz | −51% midpoint, −48% heaviest; est. 2.9–3.4% | 90% of 520 cases within 0.5 dB per third octave. Presets darker high up, from A5 (Ping) to F#7 (Lantern). Fold, FM and ratio all high clearly darker (centroid 9–11 → 2–5 kHz) |
| OP-XY-Hexagon | `optim/moth-hybrid` | `9d725c39649194dff658fbfcca2363b4dfd4feca` | 1× until the bandwidth needs 2× | −41% midpoint; heaviest unchanged; sweep record likely ~4.9% | bands within 0.08 dB everywhere |
| OP-XY-Nonagon | `optim/sand-multirate` | `dec2fa6289283be69ac8a1fe37b79670d62fb38d` | Modes at 1/8, 1/4, 1/2 or full rate + three half-bands | −40% vs today; est. 3.2–4.3%; no 8-note restart spike | 42–53 dB below the signal, worst −36 dBFS; **output 1.13 ms later**; `voiceBytes` 1360 → 2264 |
| OP-XY-Nonagon | `optim/cocoa-multirate` | `0f57c5e5a88935717a91ac0f70ff5fd2dde8938b` | Same scheme; taps and pops at full rate | −44%; est. 3.2–4.3% | median 63 dB below; restruck chord 52 dB; **output 1.13 ms later**; `voiceBytes` 304 → 1132 |
| OP-XY-Nonagon | `optim/cocoa-silent-floor` | `172ad76ec65c56f63dcea92fb71cad47d03004b8` | Silent floor 1<<14 → 1<<16 | a further 2–4% | at most −90 dBFS |
| OP-XY-Nonagon | `optim/butterfly-alternate-spectrum` | `6bed9453de05c8ed15c31be4476aa2baf471f666` | Spectrum every other block, staggered (on the loop changes, without the 16×16 step) | a further 7.5% | partials lag ≤ 1 block; 25 dB signal-to-difference (phase drift); needs listening |
| OP-XY-ModFX | `optim/chroma-subtract` | `a4ba1a298f1b2bf51ac32ea3387a40da3a0cd052` | `q31::subtract`; **needs `optim/sdk-q31-subtract`** | a further 8% defaults, 7% heavy | identical at normal level; differs on 8 of 16 settings at loud clipped input |
| OP-XY-ModFX | `optim/chroma-subtract-exact` | `e658e15c7617a7312cd1cd39698a2f2e82219a1e` | Exact form; needs the SDK change | about 1% | bit-identical |
| OP-XY-ModFX | `optim/lint-subtract` | `5882264e4495ae6ab3f6f0c62127eb2412ccf840` | 12 sites; needs the SDK change | a further 1.7% | differs only on full-scale negative input, where the old code flips sign |

### SDK (mmockett/open-xy)

| Branch | Commit | Base | What |
| --- | --- | --- | --- |
| `optim/sdk-voice-memory` | `b7b618a2754b05c9e645fa7a05f577da58ab246c` | staging `4b75792` | Word-wise voice clear in `voice.cpp.in`; word-wise `memset`/`memcpy`; `opxy::q15::product` (16×16, exact); api.md#cpu notes on loops, branches and dispatch. About 3.5× faster clears and copies in the simulator; up to ~8k cycles off an eight-note restart. Same output. |
| `optim/sdk-q31-subtract` | `de6b8e96b7860a80c3619eca2f5f925b1a600b48` | `sdk/effect-screens` `d7d3bd9` (#230) | `q31::subtract`, one instruction beside `q31::add` |
| `optim/sdk-sine-bank` | `8dabcb41723df3610948006c70a6f7307ba50aeb` | staging `4b75792` | `opxy/bank.hpp` + `sdk/platform/blackfin/bank.s` + `tests/test_bank.py`; `bank` is an SDK unit |

The sine-bank routine:
- runs at 6.8–9.4 simulated cycles a partial-sample, against GCC's 16–28;
- is bit-identical to its reference (0 mismatches in 13,000 simulator
  trials), and five engines adopt it with identical output;
- would save, in the simulator: Bloom −40% (−15% on top of its bit-identical
  branch), Hollow −19% (−16%), Cocoa −30%, Butterfly −21%, Sand −14%.

Engine adoptions are not on branches yet.

All three SDK branches pass `sdk.py check`. They still need:
- the native build with the pinned tools;
- release verification;
- a device profile before and after.

The sine-bank and voice-memory branches both edit api.md#cpu, so they'll
conflict there.

## Before any machine branch ships

1. `sdk.py test --firmware` with the pinned toolchain on the Mac. It
   couldn't run here (no linux-x86_64 download).
2. A profiling build and `sdk.py profile` on the device for the new CPU
   records. All these machines are published, so each needs a new tag and a
   new record. The MNMD siblings' files change too, with identical output.
3. Listening, for every sound-changing branch. A/B WAVs are in the audit
   session's container, not in any repo: `/home/user/moth-ab`,
   `/home/user/halfrate-ab`, `/home/user/multirate-ab`.
4. Watch the bigger `voiceBytes`, and MNMD's audio stack (280–390 bytes).

## Findings not turned into code

- **Effects reset at a sequencer restart:** a hypothesis, untested. A
  reset cuts tails (a click) and restarts the working level, setting off the
  spike above. Test by ear: give Lint a long echo, stop and restart the
  sequencer; if the tail vanishes, the effect is reset. If so, a ~5 ms
  declick through `initialize` is worth adding.
- **Idle send gate (both effects):** close once the input is silent and the
  output below about −90 dBFS for 16 blocks. That saves the 7–11% each
  effect costs on an idle send. Near-identical: it drops Lint's −93 dBFS hiss
  while closed.
- **Chroma, lowering on read:** as Lint does, to remove the rest of its
  level-fall spike. The cloud ring needs it per segment.
- **The released Lint overflows signed integers** on 3 of 12 tested settings
  (Madness with the phaser or springs in the echo loop, everything up).
  - Host compilers disagree on its output; the device's GCC wraps.
  - Whether it's audible on the device is unverified: a wrapping build of
    "everything up" peaks at −9.6 dBFS with no jump over 0.22 of full scale.
  - The branches keep the behaviour; fixing it changes the sound there.
- **Build the everyday combined firmware with `--profiling off`.** This skips
  stock's per-voice timing bookkeeping: an estimated 0.1% of a block per voice
  from the code's size, not measured. Already written into OP-XY-Dev
  `docs/device.md` on PR #1.
- **Half-rate Bloom** (`bloom_halfrate.patch` in the audit folder) isn't
  recommended: it's worse at the benchmark's midpoint and sweep. Bloom's
  lever is the sine bank.
- **Moth:** the hybrid keeps the sound, but its catalog record stays near
  4.9%. Plain 1× is Max's call by ear.
- **The coming sweep benchmark** reads higher than today's records. Fermi at
  `time` 0 is about 2x its midpoint; Mach at `echo` 1 about 1.9x.
- **Per-machine CPU and RAM on the PEN menu:**
  - Design research and issue text are in `drafts/cpu-audit/cpu-menu.md` and
    `issue-cpu-menu.md`.
  - The issue isn't posted: this session can't create issues on
    etcetc000/open-xy.
  - Measuring is cheap; getting the numbers from the DSP board to the screen
    needs firmware work.
- **Not redone:** MD KIT's heaviest voices (already lightened on OP-XY-MNMD
  `main` from #13; these branches are based on it), and the screen-only
  releases (Hexagon v1.2.0, Nonagon v0.3.0, ModFX v1.5.0, MNMD v1.1.7).
- **No firmware, stock content or ROM captures** are in any branch. The MNMD
  branch doesn't touch the ROM-based machines (mde12, mmbbox, mmddrw, mmdens,
  mmwave). Nothing used Elektron-derived code. OP-XY-Grains is unchanged.
