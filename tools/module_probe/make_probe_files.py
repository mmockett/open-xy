"""Writes the module probe's files (sdk/platform/opxy-1.2.1/module_probe.cpp).

probe-read.bin  reads on the GUI role, then on the audio role once the GUI has found it
probe-run.bin   also copies the code into the module and calls it once

    python tools/module_probe/make_probe_files.py <out folder>
"""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
BIN = HERE.parents[1] / 'local' / 'toolchain' / 'bin'
ARGUMENT = 0x11111111
AUDIO_READ, RUN_CODE = 1, 2


def code():
    with tempfile.TemporaryDirectory() as tmp:
        obj, raw = Path(tmp) / 'probe_code.o', Path(tmp) / 'probe_code.bin'
        subprocess.run([str(BIN / 'bfin-elf-as'), '-mcpu=bf707', '-o', str(obj), str(HERE / 'probe_code.s')], check=True)
        subprocess.run([str(BIN / 'bfin-elf-objcopy'), '-O', 'binary', '-j', '.text', str(obj), str(raw)], check=True)
        return raw.read_bytes()


def probe_file(flags, body):
    expected = ((ARGUMENT ^ 0x12345678) + 3) & 0xffffffff
    return b'XYPROBE1' + struct.pack('<6I', flags, 32, len(body), ARGUMENT, expected, 0) + body


def main(out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    body = code()
    (out / 'probe-read.bin').write_bytes(probe_file(AUDIO_READ, body))
    (out / 'probe-run.bin').write_bytes(probe_file(AUDIO_READ | RUN_CODE, body))
    print(f'{len(body)} code bytes; expected result {((ARGUMENT ^ 0x12345678) + 3) & 0xffffffff:08x}; files in {out}')


if __name__ == '__main__':
    main(sys.argv[1])
