#!/usr/bin/env python3
"""Offline verification of a vendored canonical snapshot (no network fetch)."""
import hashlib
import json
from pathlib import Path
root=Path(__file__).resolve().parent
pin=json.loads((root/'UPSTREAM.json').read_text())
assert pin['url']=='https://github.com/Yacine-Derder/lps-node-firmware.git'
assert len(pin['revision'])==40 and all(c in '0123456789abcdef' for c in pin['revision'])
assert pin['path']=='portable/mul_core'
files={p.relative_to(root).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
       for p in root.rglob('*') if p.is_file() and p.name!='UPSTREAM.json'}
if files!=pin['sha256']:
    raise SystemExit('MUL core vendor drift: re-export the explicit published canonical revision; do not edit vendor files')
print('MUL core pin verified: '+pin['revision'])
