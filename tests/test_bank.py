"""Sine banks (opxy/bank.hpp): the portable reference against golden hashes
recorded from the Blackfin assembly. The assembly itself is assembled by the
pinned assembler in every native build ('bank' is an SDK unit). Draft for the
SDK pull request; paths are the SDK's."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tooling.formats import require
from tooling.pipeline import ROOT
from test_sample_services import HOST_CXX

# Hashes of the same random trials run through the assembly in the Blackfin
# simulator (bit-identical to the reference there), 3000 trials each.
GOLDEN = {
    'identity sine': 'e30dda7b',
    'identity sine_glide': '8f3c1ecd',
    'identity quadrature hold=1': '0dea2b50',
    'identity quadrature hold=2^k': 'd93f1135',
    'identity edges': '3e428649',
}


class Bank(unittest.TestCase):
    @unittest.skipUnless(HOST_CXX, 'No working host C++ compiler (set OPXY_TEST_HOST_CXX)')
    def test_reference_reproduces_the_assembly(self):
        with tempfile.TemporaryDirectory(prefix='bank-') as directory:
            binary = Path(directory) / ('bank.exe' if os.name == 'nt' else 'bank')
            built = subprocess.run([HOST_CXX, '-std=c++11', '-O2', '-Wall', '-Werror', '-DTRIALS=3000',
                                    '-DWITH_QUADRATURE', '-I' + str(ROOT / 'sdk/include'),
                                    '-I' + str(ROOT / 'tests/fixtures'), str(ROOT / 'tests/fixtures/bank.cpp'),
                                    '-o', str(binary)], capture_output=True, text=True, timeout=120)
            require(built.returncode == 0, built.stdout + built.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            require(run.returncode == 0, run.stderr)
            seen = {}
            for line in run.stdout.splitlines():
                name, _, rest = line.partition(':')
                fields = dict(f.split('=') for f in rest.split())
                seen[name] = fields['hash']
                require(fields['mismatches'] == '0', line)
            require(seen == GOLDEN, 'Reference hashes changed: %r' % seen)
