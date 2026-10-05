#!/usr/bin/env python3
"""Exercise the production publisher with genuine versioned identity bytes."""

import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    fixture_tool, publisher_test = map(pathlib.Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="dxmt9-publisher-identity-") as root:
        fixtures = pathlib.Path(root) / "fixtures"
        subprocess.run([str(fixture_tool), "--write-inspection-fixture",
                        str(fixtures)], check=True)
        subprocess.run([str(publisher_test), str(fixtures)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
