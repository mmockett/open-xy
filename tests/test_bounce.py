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
    0x8d4ff0a8: 0x8d5aec36,  # set audio mode, from the 0xc8 command handler
    0x8d510760: 0x11a02b3a,  # engine render, from the audio block wrapper
    0x8d50e81c: 0x8d57c9ea,  # note-on, from the benchmark
    0x8d50e964: 0x8d57ca4a,  # note-off, from the benchmark
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
