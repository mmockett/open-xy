"""Private bounce capture (phase 1): host logic, the reviewed hook site and its opt-in service."""
import os
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


if __name__ == '__main__':
    unittest.main()
