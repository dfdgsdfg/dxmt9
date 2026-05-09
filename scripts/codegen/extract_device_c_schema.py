#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import re
import sys


PROTOTYPE_RE = re.compile(
    r"([A-Za-z_][A-Za-z0-9_\s\*]*?\s+dxmt9c_[A-Za-z0-9_]+\s*\([^;]*?\)\s*;)",
    re.S,
)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: extract_device_c_schema.py <device_c.h> <out>", file=sys.stderr)
        return 2

    source = pathlib.Path(argv[1])
    dest = pathlib.Path(argv[2])

    text = source.read_text()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    protos = [match.group(1).strip() for match in PROTOTYPE_RE.finditer(text)]

    lines = [
        "// Generated from include/dxmt9/device_c.h by scripts/codegen/extract_device_c_schema.py.",
        "// This header is generator input only; it is not compiled directly.",
    ]
    lines.extend(protos)
    lines.append("")
    dest.write_text("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
