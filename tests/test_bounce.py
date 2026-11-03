"""Private bounce capture (phase 1) and DSP render (phase 2a): host logic, the reviewed
hook site and stock addresses, and their opt-in services."""
import os
import struct
from pathlib import Path
import unittest
from unittest import mock

from tooling import native
from tooling.firmware import absolute_target
from tooling.formats import ldr, mapped_offset, sha
from tooling.pipeline import config
from tooling.startup import render
from machine_fixtures import fixture
import test_sample_services as services


class Bounce(unittest.TestCase):
    run_fixture = services.SampleServices.run_fixture

    @unittest.skipUnless(services.HOST_CXX, 'No working host C++ compiler')
    def test_ring_peaks_dither_header_and_paths(self):
        self.run_fixture('bounce')

    def test_service_is_opt_in(self):
        machines = [fixture('probesynth')]
        profile = config()
        with mock.patch.dict(os.environ, {'OPXY_BOUNCE_CAPTURE': ''}):
            self.assertNotIn('bounce', native.active_services(profile, native.service_users(machines)))
            self.assertNotIn('sdk/bounce.o', [name for name, _m, _s in native.plan(machines, profile)])
        with mock.patch.dict(os.environ, {'OPXY_BOUNCE_CAPTURE': '1'}):
            self.assertIn('bounce', native.active_services(profile, native.service_users(machines)))
            self.assertIn('sdk/bounce.o', [name for name, _m, _s in native.plan(machines, profile)])
        hooks = [p for p in profile['patches'] if p.get('symbol') == 'sdk_bounce_block']
        self.assertEqual(len(hooks), 1)
        self.assertEqual(hooks[0]['service'], 'bounce')

    def test_startup_starts_the_writer_last_only_when_linked(self):
        self.assertNotIn('sdk_bounce_startup', render(['probe']))
        text = render(['probe'], control=True, aux=True, bounce=True)
        self.assertLess(text.index('    sdk_aux_startup();'), text.index('    sdk_bounce_startup();'))

    @unittest.skipUnless(os.environ.get('OPXY_TEST_STOCK_OS'), 'Private stock input not supplied')
    def test_hook_replaces_the_usb_audio_call_in_the_main_sport_callback(self):
        data = Path(os.environ['OPXY_TEST_STOCK_OS']).read_bytes()
        profile = config()
        self.assertEqual(sha(data), profile['stock_sha256'])
        (patch,) = [p for p in profile['patches'] if p.get('symbol') == 'sdk_bounce_block']
        offset = mapped_offset(ldr(data), int(patch['address'], 16), patch['length'])
        self.assertEqual(hex(offset), patch['file_offset'])
        self.assertEqual(absolute_target(data[offset:offset + 8]), 0x8d4d4e14)
        # R0 = R7 just before: the block is the call's only argument.
        self.assertEqual(data[offset - 2:offset], bytes.fromhex('0730'))


# Stock 1.2.1 call sites that reach each function bounce.cpp or bounce_render.cpp
# calls: an absolute (8-byte) or relative (4-byte) CALL at the site. Facts only,
# read from stock; drafts/bounce-stems.md §7.7 says what each one is.
CALLED = {
    0x8fed0348: 0x8c040a88,  # fopen
    0x8fed0780: 0x8c040af0,  # fwrite
    0x8fed00ae: 0x8c040b16,  # fclose
    0x8c0bd5bc: 0x8c0c1a28,  # mkdir, from the DSP's path-request dispatcher
    0x8c040bd8: 0x8d4ef704,  # rename wrapper
    0x8c040c24: 0x8c040c4e,  # remove wrapper
    0x8c048788: 0x8d4d0426,  # heap allocate
    0x8d4d2910: 0x8c041148,  # free
    0x8c03de70: 0x8c03df66,  # semaphore create
    0x8c03e6f4: 0x8c03d452,  # semaphore take
    0x8c03e9c8: 0x8c03d4bc,  # semaphore give
    0x8d5bdf44: 0x8c0b6828,  # task create
    0x8c042efc: 0x8c040b78,  # log
    0x8c03ee34: 0x8c03d234,  # vTaskSuspendAll, paired with the next in heap_4's malloc
    0x8c03f284: 0x8c03d244,  # xTaskResumeAll
    0x8c03ee20: 0x8c04060e,  # xTaskGetTickCount
    0x8c040188: 0x8c0ba506,  # vTaskDelay, a tick each pass of the DSP's main_task loop
    0x8d4ff0a8: 0x8d5aec36,  # set audio mode, from the 0xc8 command handler
    0x8d510760: 0x11a02b3a,  # engine render, from the audio block wrapper
    0x8d50e81c: 0x8d57c9ea,  # note-on, from the benchmark
    0x8d50e964: 0x8d57ca4a,  # note-off, from the benchmark
    0x8c03efc4: 0x8c0c18d0,  # vTaskPrioritySet(NULL, 8), stock's bridge write (diagnostic build)
    0x8c03f94c: 0x8c0c18c0,  # vTaskGetInfo(NULL, ...), the same (diagnostic build)
}
STOCK = os.environ.get('OPXY_TEST_STOCK_OS')


class Stock:
    def __init__(self, data):
        self.data, self.blocks = data, ldr(data)

    def halves(self, address, count):
        return struct.unpack_from(f'<{count}H', self.data, mapped_offset(self.blocks, address, 2 * count))

    def call_target(self, site):
        w = self.halves(site, 4)
        if w[0] == 0xde00 and w[3] == 0:
            return w[1] << 16 | w[2]
        if w[0] & 0xff00 == 0xe300:  # CALL pcrel25m2
            offset = (w[0] & 0xff) << 16 | w[1]
            return site + 2 * (offset - (1 << 24) if offset & 0x800000 else offset)
        return None

    def absolute(self, site):
        """(store, address) of a 64-bit absolute load or store."""
        w = self.halves(site, 4)
        if w[0] >> 9 != 0x6c or w[3] & 0xf0ff:
            return None
        return bool(w[3] & 0x200), w[1] << 16 | w[2]

    def code(self):
        """(address, half-words) of each stored block of code: L1 and the 8c-8f image."""
        import numpy as np
        for b in self.blocks:
            if b['fill'] or b['target'] & 1 or not (0x11a00000 <= b['target'] < 0x11b00000 or
                                                     0x8c000000 <= b['target'] < 0x90000000):
                continue
            yield b['target'], np.frombuffer(self.data, '<u2', b['stored'] // 2, b['offset'] + 16)

    def absolute_sites(self, address):
        """Sites of every 64-bit absolute load or store of address."""
        import numpy as np
        sites = []
        for base, w in self.code():
            hit = ((w[:-3] & 0xfe00) == 0xd800) & (w[1:-2] == address >> 16) & (w[2:-1] == address & 0xffff)
            sites += [base + 2 * int(i) for i in np.flatnonzero(hit & ((w[3:] & 0xf0ff) == 0))]
        return sites

    def absolute_calls(self, function):
        """Sites of every absolute CALL to function."""
        import numpy as np
        sites = []
        for base, w in self.code():
            hit = (w[:-3] == 0xde00) & (w[1:-2] == function >> 16) & (w[2:-1] == function & 0xffff) & (w[3:] == 0)
            sites += [base + 2 * int(i) for i in np.flatnonzero(hit)]
        return sites

    def absolute_alignments(self):
        """{width bits: {address % 4: count}} of R0's absolute loads and stores of 08, 11 and 84 addresses."""
        import numpy as np
        out = {}
        for _base, w in self.code():
            hit = (w[:-3] == 0xd800) & ((w[3:] & 0xf0ff) == 0) & np.isin(w[1:-2] >> 8, (0x08, 0x11, 0x84))
            for i in np.flatnonzero(hit):
                counts = out.setdefault(int(w[i + 3]) & 0x0f00, {})
                counts[int(w[i + 2]) & 3] = counts.get(int(w[i + 2]) & 3, 0) + 1
        return out

    def immediate(self, site):
        """(register, value) of a 32-bit immediate load."""
        w = self.halves(site, 4)
        return (w[0] & 15, w[1] << 16 | w[2]) if w[0] >> 8 == 0xda and w[3] == 0 else None


@unittest.skipUnless(STOCK, 'Private stock input not supplied')
class RenderStock(unittest.TestCase):
    """Each stock address the DSP render calls, reads or writes, checked in stock 1.2.1."""

    @classmethod
    def setUpClass(cls):
        cls.stock = Stock(Path(STOCK).read_bytes())

    def test_is_stock_1_2_1(self):
        self.assertEqual(sha(self.stock.data), config()['stock_sha256'])

    def test_every_called_function_has_its_stock_caller(self):
        for function, site in CALLED.items():
            self.assertEqual(self.stock.call_target(site), function, hex(site))

    def test_scheduler_suspension(self):
        s = self.stock
        # vTaskSuspendAll: ++uxSchedulerSuspended (11800030), nothing else.
        self.assertEqual(s.halves(0x8c03ee34, 2), (0xe800, 0))  # LINK 0
        self.assertEqual(s.absolute(0x8c03ee38), (False, 0x11800030))
        self.assertEqual(s.absolute(0x8c03ee42), (True, 0x11800030))
        # xTaskResumeAll: asserts it non-zero (tasks.c line 2112), then decrements it.
        self.assertEqual(s.absolute(0x8c03f28c), (False, 0x11800030))
        self.assertEqual(s.absolute(0x8c03f2a6), (True, 0x11800030))
        self.assertEqual(s.immediate(0x8c03f3a8), (0, 0x84000120))
        self.assertTrue(s.data[mapped_offset(s.blocks, 0x84000120, 72):].startswith(
            b'../blackfin-common/adi-freertos/FreeRTOSv10.0.0/FreeRTOS/Source/tasks.c'))
        # xTaskGetTickCount reads one word; the tick is the core timer's 400,000 cycles.
        self.assertEqual(s.absolute(0x8c03ee24), (False, 0x11800014))
        self.assertEqual(s.immediate(0x8c03d940), (0, 0x61a80))
        self.assertEqual(s.absolute(0x8c03d950), (True, 0x1fc03004))

    def test_audio_block_wrapper(self):
        s = self.stock
        # Counts every block, then mode 2 (842e2a24 - 0x24) skips the render.
        self.assertEqual(s.absolute(0x11a02888), (False, 0x84a50b98))
        self.assertEqual(s.absolute(0x11a02892), (True, 0x84a50b98))
        self.assertEqual(s.immediate(0x11a0289a), (13, 0x842e2a24))
        # The render: engine object in R0, the L1 output block in R1.
        self.assertEqual(s.immediate(0x11a02b2a), (1, 0x11b00948))
        self.assertEqual(s.immediate(0x11a02b32), (0, 0x84c696fc))
        # Setting the mode only stores the word.
        self.assertEqual(s.absolute(0x8d4ff0ac), (True, 0x842e2a00))

    def test_block_counter_is_a_halfword_every_live_block_counts(self):
        # What bounce-2a-003's clocks rely on (bounce_render.cpp, "Clocks").
        s = self.stock
        # The wrapper loads the counter 16 bits wide (0x0c00), adds one, stores 16 bits (0x0600).
        self.assertEqual(s.halves(0x11a02888, 4), (0xd800, 0x84a5, 0x0b98, 0x0c00))
        self.assertEqual(s.halves(0x11a02890, 1), (0x6408,))  # R0 += 1
        self.assertEqual(s.halves(0x11a02892, 4), (0xd800, 0x84a5, 0x0b98, 0x0600))
        # Those width bits, over every absolute access in stock's code: words only at
        # multiples of 4, halfwords (0x0400, 0x0600, 0x0c00) never at an odd address,
        # bytes (0x0a00) at odd ones too.
        by_width = s.absolute_alignments()
        self.assertEqual(set(by_width[0x0000]), {0})
        for width in (0x0400, 0x0600, 0x0c00):
            self.assertEqual(set(by_width[width]), {0, 2}, hex(width))
        self.assertTrue({1, 3} <= set(by_width[0x0a00]))
        # Nothing else in stock names the counter or its upper half.
        self.assertEqual(s.absolute_sites(0x84a50b98), [0x11a02888, 0x11a02892])
        self.assertEqual(s.absolute_sites(0x84a50b9a), [])
        # The count comes before the mode-2 test (842e2a24 - 0x24), so it runs in every mode.
        self.assertEqual(s.immediate(0x11a0289a), (13, 0x842e2a24))
        self.assertEqual(s.halves(0x11a028a6, 1), (0x0c10,))  # CC = R0 == 2
        # On the DSP (stock_gui_role 0) the SPORT callback is 8c0c6f8c (8c0c69e4 on the main board).
        self.assertEqual(s.call_target(0x8c0c6cbc), 0x8d5c3eac)
        self.assertEqual(s.immediate(0x8c0c6cc6), (1, 0x8c0c69e4))
        self.assertEqual(s.immediate(0x8c0c6cce), (0, 0x8c0c6f8c))
        # It reads the mode and calls the wrapper unless the mode is 1.
        self.assertEqual(s.absolute(0x8c0c6fe0), (False, 0x842e2a00))
        self.assertEqual(s.halves(0x8c0c6fe8, 1), (0x0c08,))  # CC = R0 == 1
        self.assertEqual(s.call_target(0x8c0c6ffe), 0x11a02864)
        # The wrapper has no other absolute caller, and the engine render none but the
        # wrapper (the benchmark's are relative, in CALLED): every live block is counted.
        self.assertEqual(s.absolute_calls(0x11a02864), [0x8c0c6ffe])
        self.assertEqual(s.absolute_calls(0x8d510760), [0x11a02b3a])

    def test_render_sleeps_with_vtaskdelay_as_the_dsp_main_task_does(self):
        s = self.stock
        # main_task (priority 3) runs the DSP's loop 8c0ba32c when stock_gui_role is 0.
        self.assertEqual(s.call_target(0x8c0b6722), 0x8d5c3eac)
        self.assertEqual(s.call_target(0x8c0b673e), 0x8c0ba32c)
        self.assertEqual(s.immediate(0x8c0b6818), (1, 0x8c0b66e8))
        self.assertEqual(s.halves(0x8c0b6806, 1), (0x6819,))  # P1 = 3, the priority
        self.assertTrue(s.data[mapped_offset(s.blocks, 0x8400e978, 10):].startswith(b'main_task\0'))
        self.assertEqual(s.call_target(0x8c0b6828), 0x8d5bdf44)
        # vTaskDelay(1) each pass.
        self.assertEqual(s.halves(0x8c0ba504, 1), (0x6008,))  # R0 = 1
        # FreeRTOS's vTaskDelay: asserts the scheduler isn't suspended, then suspends it,
        # adds the task to the delayed list, resumes, and yields (RAISE 14) unless that did.
        self.assertEqual(s.absolute(0x8c0401a8), (False, 0x11800030))
        self.assertEqual(s.call_target(0x8c0401b4), 0x8c03ee34)
        self.assertEqual(s.call_target(0x8c0401bc), 0x8c0400ac)
        self.assertEqual(s.call_target(0x8c0401c0), 0x8c03f284)
        self.assertEqual(s.halves(0x8c04019e, 1), (0x009e,))

    def test_note_on_and_random_generator(self):
        s = self.stock
        # Note-on reads the track's instrument word.
        self.assertEqual(s.immediate(0x8d50e866), (9, 0x11b01d48))
        # The DSP's LCG: x * 1664525 + 1013904223 at 11b01e50, seeded from CYCLES at boot.
        self.assertEqual(s.absolute(0x8d51af52), (False, 0x11b01e50))
        self.assertEqual(s.immediate(0x8d51af5a), (1, 1664525))
        self.assertEqual(s.immediate(0x8d51af64), (1, 1013904223))
        self.assertEqual(s.absolute(0x8d51af6e), (True, 0x11b01e50))
        self.assertEqual(s.absolute(0x8c0b7ab8), (True, 0x11b01e50))

    def test_log_lines_reach_the_file_through_a_ring_drained_about_once_a_second(self):
        # What the diagnostic build's wait after each step line relies on.
        s = self.stock
        # The log call formats the line (8c042cbc) and hands it to each sink in the table at 080024e8.
        self.assertEqual(s.call_target(0x8c042f1c), 0x8c042cbc)
        self.assertEqual(s.immediate(0x8c042eb2), (13, 0x080024e8))
        # disk_log.cpp's sink (8c0cdc04), registered through 8c042c54, only copies the line into
        # the writer object 08024cf4's RAM ring (8c0cd7fa).
        self.assertEqual(s.immediate(0x8c0ba0f4), (0, 0x8c0cdc04))
        self.assertEqual(s.call_target(0x8c0ba0fc), 0x8c042c54)
        self.assertEqual(s.immediate(0x8c0cdc1a), (0, 0x08024cf4))
        self.assertEqual(s.call_target(0x8c0cdc22), 0x8c0cd7fa)
        self.assertTrue(s.data[mapped_offset(s.blocks, 0x84017a0c, 22):].startswith(b'../src/bf/disk_log.cpp'))
        self.assertEqual(s.immediate(0x8c0ce67a), (1, 0x84017a0c))
        # The drain (8c0ce7c8 on 08024cf4) fwrites the ring and fflushes the file.
        self.assertEqual(s.immediate(0x8c0ba90c), (0, 0x08024cf4))
        self.assertEqual(s.call_target(0x8c0ba914), 0x8c0ce7c8)
        self.assertEqual(s.call_target(0x8c0ce81c), 0x8fed0780)
        self.assertEqual(s.call_target(0x8c0ce83e), 0x8fed019a)
        # main_thread_bfs.cpp posts that drain (8c0ba908) as a job once its timer passes 1000.
        self.assertEqual(s.halves(0x8c0ba3d4, 2), (0xe122, 1000))
        self.assertEqual(s.immediate(0x8c0ba6c0), (2, 0x8c0ba908))
        self.assertEqual(s.call_target(0x8c0ba6cc), 0x8d4dd728)
        self.assertEqual(s.immediate(0x8c0ba8f2), (1, 0x8400f280))
        self.assertTrue(s.data[mapped_offset(s.blocks, 0x8400f280, 31):].startswith(b'../src/main/main_thread_bfs.cpp'))

    def test_diagnostic_build_reads_freertos_state(self):
        s = self.stock
        # heap_4's malloc keeps its state at 11800040: +4 bytes free, +8 the least ever free.
        self.assertEqual(s.immediate(0x8c03d60e), (12, 0x11800040))
        # vTaskPrioritySet asserts the priority below 16 (tasks.c line 1472, 0x5c0).
        self.assertEqual(s.halves(0x8c03efcc, 1), (0x6085,))  # R5 = 16
        self.assertEqual(s.halves(0x8c03f0c4, 2), (0xe121, 0x05c0))
        # vTaskGetInfo stores the stack's unused words as a halfword at +0x20.
        self.assertEqual(s.call_target(0x8c03f9b8), 0x8c03ed2c)
        # xTaskCreate takes the stack in words: it allocates depth << 2 (8c03f8d4) from heap_4.
        self.assertEqual(s.call_target(0x8c03f8da), 0x8c03d600)
        self.assertEqual(s.call_target(0x8d5bdf8e), 0x8c03f8c4)

    def test_only_the_benchmark_command_sets_the_audio_mode(self):
        # The setter's only stock callers are the 0xc8 command handler's, so mode 2 is the render's alone.
        s = self.stock
        self.assertEqual(s.call_target(0x8d5aec36), 0x8d4ff0a8)
        self.assertEqual(s.call_target(0x8d5aec72), 0x8d4ff0a8)

    def test_fwrite_hands_large_writes_to_write(self):
        # fwrite's unbuffered path calls write() once for the whole length.
        self.assertEqual(self.stock.call_target(0x8fed0856), 0x8c0bcbf6)


class Render(unittest.TestCase):
    def test_service_is_opt_in(self):
        machines = [fixture('probesynth')]
        profile = config()
        with mock.patch.dict(os.environ, {'OPXY_BOUNCE_RENDER': '', 'OPXY_BOUNCE_CAPTURE': ''}):
            self.assertNotIn('bounce_render', native.active_services(profile, native.service_users(machines)))
            self.assertNotIn('sdk/bounce_render.o', [name for name, _m, _s in native.plan(machines, profile)])
        with mock.patch.dict(os.environ, {'OPXY_BOUNCE_RENDER': '1', 'OPXY_BOUNCE_CAPTURE': ''}):
            services = native.active_services(profile, native.service_users(machines))
            self.assertIn('bounce_render', services)
            self.assertNotIn('bounce', services)
            names = [name for name, _m, _s in native.plan(machines, profile)]
            self.assertIn('sdk/bounce_render.o', names)
            self.assertNotIn('sdk/bounce.o', names)
        # No stock patch: the render starts from startup and calls stock directly.
        self.assertFalse([p for p in profile['patches'] if p.get('service') == 'bounce_render'])

    def test_diagnostic_build_waits_two_minutes_and_after_every_step_line(self):
        source = (Path(native.ROOT) / 'sdk/platform/opxy-1.2.1/bounce_render.cpp').read_text()
        self.assertIn('const u32 kStartMillis = 120000;', source)
        self.assertIn('const u32 kSettleMillis = 1500;', source)
        # Every step line waits for the drain; none is logged with the scheduler suspended.
        body = source[source.index('template <class... A> void step('):]
        self.assertIn('settle();', body[:body.index('\n}\n')])
        # The pause is ticks, so it lasts in mode 0 whether or not the block counter moves.
        body = source[source.index('void settle() {'):]
        self.assertIn('delay_ticks(kSettleMillis);', body[:body.index('\n}\n')])
        block = source[source.index('void render_block() {'):]
        self.assertNotIn('step(', block[:block.index('\n}\n')])

    def test_bisect_runs_its_steps_in_order_and_touches_nothing_else(self):
        source = (Path(native.ROOT) / 'sdk/platform/opxy-1.2.1/bounce_render.cpp').read_text()
        self.assertIn('const bool kDiagnose = true;', source)
        body = source[source.index('void diagnose() {'):]
        body = body[:body.index('\n}\n')]
        order = ['delay_ticks(kDiagBlocked);', 'set_priority(2);', 'wake_until(first, kDiagBelow, false',
                 'set_priority(kPriority);', 'wake_until(first, kDiagEqual, false', 'log(__LINE__, "diag 1:',
                 'wake_until(first, kDiagLogged, true', 'sleep_ticks(semaphore, kDiagWake);']
        at = [body.index(step) for step in order]
        self.assertEqual(at, sorted(at))
        # No log before step 4; no mode, note, render, allocation or file in the bisect.
        self.assertNotIn('log(', body[:body.index('// 4:')])
        for call in ('set_mode(', 'note_on(', 'render(', 'allocate(', 'file_open(', 'make_folder(', 'step('):
            self.assertNotIn(call, body)
        wake = source[source.index('void wake_until('):]
        self.assertIn('if (logging && waited >= next_line)', wake[:wake.index('\n}\n')])

    def test_waits_count_ticks_and_the_block_counter_is_read_as_a_halfword(self):
        source = (Path(native.ROOT) / 'sdk/platform/opxy-1.2.1/bounce_render.cpp').read_text()
        # bounce-2a-002 waited on the counter read as a word, with a semaphore sleep.
        # Only the bisect's step 5 (diagnose) still takes a semaphore, on purpose.
        render = source[:source.index('// Bisect (bounce-diag-001')]
        for gone in ('wait_blocks(', 'sleep_ticks(', 'create_semaphore(', 'while (blocks()', 'word(kBlocks); }'):
            self.assertNotIn(gone, render)
        self.assertIn('u32 raw_blocks() { return *at<const volatile u16>(kBlocks); }', source)
        self.assertIn('u32 blocks() { return counter.read(raw_blocks()); }', source)
        self.assertIn('void delay_ticks(u32 ticks) { stock<void(u32)>(0x8c040188)(ticks); }', source)
        renderer = source[source.index('void renderer(void*) {'):]
        renderer = renderer[:renderer.index('\n}\n')]
        # The idle window and the gap count ticks; the window logs both clocks.
        self.assertIn('const u32 waited = tick_count() - first;', renderer)
        self.assertIn('if (waited >= kStartMillis) break;', renderer)
        self.assertIn('render: idle %d ticks, block counter moved %d', renderer)
        self.assertIn('delay_ticks(kGapMillis);', renderer)
        # The take's speed and too-slow limit count ticks; mode 2 waits on no counter.
        take = source[source.index('void render_take(u32 total, u32 hold) {'):]
        take = take[:take.index('\n}\n')]
        self.assertIn('const u32 from = tick_count();', take)
        self.assertIn('if (tick_count() - from - (paused - paused_from) > kMaxElapsedMillis) {', take)
        self.assertIn('take.elapsed = tick_count() - from - take.paused;', take)
        self.assertNotIn('kNoBlocks', take)
        # A static counter is logged, and a nominal block taken; only a missing tick stops it.
        clock = source[source.index('bool measure_clock() {'):]
        clock = clock[:clock.index('\n}\n')]
        self.assertIn('per_block = nominal_per_block(per_tick);', clock)
        self.assertIn('not rendering', clock)
        self.assertNotIn('no audio blocks', clock)

    def test_startup_starts_the_render_on_the_audio_role_after_the_engines(self):
        self.assertNotIn('sdk_bounce_render_startup', render(['probe']))
        text = render(['probe'], bounce_render=True)
        synth = text[text.index('void sdk_startup_synth() {'):]
        synth = synth[:synth.index('\n}\n')]
        self.assertLess(synth.index('if (stock_gui_role()) return;'), synth.index('    probe_register();'))
        self.assertLess(synth.index('    probe_register();'), synth.index('    sdk_bounce_render_startup();'))
        screen = text[text.index('void sdk_startup_screen() {'):]
        self.assertNotIn('sdk_bounce_render_startup', screen[:screen.index('\n}\n')])


if __name__ == '__main__':
    unittest.main()
