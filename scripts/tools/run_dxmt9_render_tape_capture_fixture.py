#!/usr/bin/env python3

"""Generate and transactionally publish one deterministic frame-tape fixture."""

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, check=True, text=True, capture_output=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", type=pathlib.Path, required=True)
    parser.add_argument("--validator", type=pathlib.Path, required=True)
    parser.add_argument("--bundle-tool", type=pathlib.Path, required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()
    output = args.output_dir.resolve()
    if output.exists():
        if not output.is_dir() or any(output.iterdir()):
            raise SystemExit(f"output directory must be empty: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=f".{output.name}.", dir=output.parent
    ) as temporary:
        staging = pathlib.Path(temporary)
        source = staging / "source"
        bundle = staging / "bundle"
        run(str(args.fixture), "--write-production-fixture", str(source))
        events = source / "events.bin"
        blobs = sorted((source / "blobs").glob("*.bin"))
        if not events.is_file() or len(blobs) != 1:
            raise SystemExit("fixture must publish events.bin and exactly one blob")
        run(
            sys.executable,
            str(args.bundle_tool),
            "pack",
            "--events",
            str(events),
            "--blob",
            str(blobs[0]),
            "--output-dir",
            str(bundle),
            "--validator",
            str(args.validator),
        )
        validated = json.loads(
            run(
                sys.executable,
                str(args.bundle_tool),
                "validate",
                str(bundle),
                "--validator",
                str(args.validator),
            ).stdout
        )
        inspected = json.loads(
            run(
                sys.executable,
                str(args.bundle_tool),
                "inspect",
                str(bundle),
                "--validator",
                str(args.validator),
            ).stdout
        )
        if validated.get("valid") is not True or validated.get("presents") != 1:
            raise SystemExit("fixture validation did not close exactly one Present")
        expected = {
            "events": 7,
            "bootstraps": 1,
            "defines": 2,
            "mutations": 1,
            "mutation_bytes": 4,
            "chunks": 1,
            "records": 1,
            "controls": 1,
            "present_completions": 1,
            "last_present_ordinal": 5,
        }
        if any(inspected.get(key) != value for key, value in expected.items()):
            raise SystemExit(f"fixture inspect counts are not deterministic: {inspected}")
        if output.exists():
            output.rmdir()
        bundle.rename(output)
        print(json.dumps({"bundle": str(output), "inspect": inspected}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
