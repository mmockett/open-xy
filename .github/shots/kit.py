"""Fork-only: a minimal builder collection of the given machine folders, with their previews.

No firmware, objects or stock content: enough for the builder's machine pages and the
selected-machines dialog (as tests/web/builder.test.mjs's collection), not for installing.
"""
import base64
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tooling import browser_kit, screen  # noqa: E402
from tooling.machines import machine_at  # noqa: E402

out = Path(sys.argv[1])
out.mkdir(parents=True)
source = (ROOT / 'profiles/opxy-1.2.1.json').read_bytes()
profile = json.loads(source)
machines = [machine_at(Path(path)) for path in sys.argv[2:]]
entries = [browser_kit.machine_entry(m, profile, {}, out, 'clang++') for m in machines]
for entry in entries:
    print(entry['id'], entry['kind'], 'preview' in entry, entry.get('preview', {}).get('kit'))
kit = dict(format='opxy-browser-kit/1', version='0.0.0', profile=profile,
           profileSource=base64.b64encode(source).decode('ascii'), definitions={}, fixed={},
           machines=entries, objects=[], notices='', extensions=[], stockCatalog='',
           screenTemplate=screen.template().decode('utf-8'))
(out / 'kit.json').write_text(json.dumps(kit, separators=(',', ':')), encoding='utf-8')
