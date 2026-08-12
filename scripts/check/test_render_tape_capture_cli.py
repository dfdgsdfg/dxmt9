#!/usr/bin/env python3

import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile


def run(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, check=check, text=True, capture_output=True)


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: test_render_tape_capture_cli.py <fixture> <validator> <bundle-tool> <runner>"
        )
    fixture = pathlib.Path(sys.argv[1])
    validator = pathlib.Path(sys.argv[2])
    bundle_tool = pathlib.Path(sys.argv[3])
    runner = pathlib.Path(sys.argv[4])
    with tempfile.TemporaryDirectory(prefix="dxmt9-render-tape-capture-") as root:
        root_path = pathlib.Path(root)
        source = root_path / "source"
        bundle = root_path / "bundle"
        run(str(fixture), "--write-production-fixture", str(source))
        events = source / "events.bin"
        blob = source / "blobs" / (
            "8d70d691c822d55638b6e7fd54cd94170c87d19eb1f628b757506ede5688d297.bin"
        )
        assert events.is_file()
        assert blob.is_file()
        assert hashlib.sha256(blob.read_bytes()).hexdigest() == blob.stem

        run(
            sys.executable,
            str(bundle_tool),
            "pack",
            "--events",
            str(events),
            "--blob",
            str(blob),
            "--output-dir",
            str(bundle),
            "--validator",
            str(validator),
        )
        validated = json.loads(
            run(
                sys.executable,
                str(bundle_tool),
                "validate",
                str(bundle),
                "--validator",
                str(validator),
            ).stdout
        )
        assert validated["valid"] is True
        assert validated["events"] == 6
        assert validated["presents"] == 1
        assert validated["scope"] == {
            "production_capture": False,
            "production_provider_replay": False,
            "output_oracle": False,
        }

        inspected = json.loads(
            run(
                sys.executable,
                str(bundle_tool),
                "inspect",
                str(bundle),
                "--validator",
                str(validator),
            ).stdout
        )
        assert inspected == {
            "bootstraps": 1,
            "bundle_schema": "dxmt9.render_tape.bundle.v2",
            "chunks": 1,
            "controls": 1,
            "defines": 1,
            "destroys": 0,
            "events": 6,
            "events_sha256": inspected["events_sha256"],
            "handles": 0,
            "last_present_ordinal": 4,
            "mutations": 1,
            "mutation_bytes": 4,
            "present_completions": 1,
            "profile": "frame-tape",
            "records": 1,
            "schema": "dxmt9.render_tape.v2",
            "scope": {
                "production_capture": False,
                "production_provider_replay": False,
                "output_oracle": False,
            },
            "valid": True,
        }
        runner_bundle = root_path / "runner-bundle"
        runner_result = json.loads(
            run(
                sys.executable,
                str(runner),
                "--fixture",
                str(fixture),
                "--validator",
                str(validator),
                "--bundle-tool",
                str(bundle_tool),
                "--output-dir",
                str(runner_bundle),
            ).stdout
        )
        assert runner_result["inspect"] == inspected
        assert (runner_bundle / "events.bin").is_file()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
