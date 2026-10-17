#!/usr/bin/env python3
"""Verify the private, unmodified Perfetto amalgamation against its source pin."""
from hashlib import sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PINS = {
    "sdk/perfetto.h": "01f81730839c1d833c4988fc2830ca9c9b0231b0476a4f9f87b5d63f80d77e69",
    "sdk/perfetto.cc": "33a93a2a1f02f3442b8384214d5bb466177fbbe75ba6324f09656b140d4efd19",
    "LICENSE": "73f3b0eee3d8d064888f928c52211f33bdcc9145925a9e1340d07cd78ed62351",
}
for name, expected in PINS.items():
    source = ROOT / "subprojects/perfetto" / name
    actual = sha256(source.read_bytes()).hexdigest()
    if actual != expected:
        raise SystemExit(f"Perfetto pin mismatch: {name}: {actual} != {expected}")
print("Private Perfetto SDK revision/digest/license pin: PASS")
