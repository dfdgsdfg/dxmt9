#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile


def run(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, check=check, text=True, capture_output=True)


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_render_tape_cli.py <fixture-generator> <validator> <bundle-tool>"
        )
    generator = pathlib.Path(sys.argv[1])
    validator = pathlib.Path(sys.argv[2])
    bundle_tool = pathlib.Path(sys.argv[3])
    with tempfile.TemporaryDirectory(prefix="dxmt9-render-tape-") as directory:
        tape = pathlib.Path(directory) / "frame.tape"
        run(str(generator), "--write-fixture", str(tape))

        validated = json.loads(run(str(validator), "validate", str(tape)).stdout)
        assert validated == {
            "schema": "dxmt9.render_tape.v1",
            "profile": "frame-tape",
            "valid": True,
            "events": 5,
            "presents": 1,
        }

        inspected = json.loads(run(str(validator), "inspect", str(tape)).stdout)
        assert inspected["valid"] is True
        assert inspected["events"] == 5
        assert inspected["creates"] == 1
        assert inspected["writes"] == 1
        assert inspected["write_bytes"] == 4
        assert inspected["chunks"] == 1
        assert inspected["records"] == 2
        assert inspected["handles"] == 1
        assert inspected["presents"] == 1
        assert inspected["last_present_ordinal"] == 1

        damaged = pathlib.Path(directory) / "damaged.tape"
        payload = bytearray(tape.read_bytes())
        payload[8:12] = (99).to_bytes(4, "little")
        damaged.write_bytes(payload)
        rejected = run(str(validator), "validate", str(damaged), check=False)
        assert rejected.returncode == 1
        assert "status=invalid-header" in rejected.stderr

        bundle = pathlib.Path(directory) / "bundle"
        run(
            sys.executable,
            str(bundle_tool),
            "pack",
            "--events",
            str(tape),
            "--output-dir",
            str(bundle),
            "--validator",
            str(validator),
        )
        bundle_validated = json.loads(
            run(
                sys.executable,
                str(bundle_tool),
                "validate",
                str(bundle),
                "--validator",
                str(validator),
            ).stdout
        )
        assert bundle_validated["bundle_schema"] == "dxmt9.render_tape.bundle.v1"
        assert bundle_validated["scope"] == {
            "production_capture": False,
            "production_provider_replay": False,
            "output_oracle": False,
        }
        bundle_inspected = json.loads(
            run(
                sys.executable,
                str(bundle_tool),
                "inspect",
                str(bundle),
                "--validator",
                str(validator),
            ).stdout
        )
        assert bundle_inspected["writes"] == 1
        event_component = bundle / "events.bin"
        component_bytes = bytearray(event_component.read_bytes())
        component_bytes[-1] ^= 1
        event_component.write_bytes(component_bytes)
        digest_rejected = run(
            sys.executable,
            str(bundle_tool),
            "validate",
            str(bundle),
            "--validator",
            str(validator),
            check=False,
        )
        assert digest_rejected.returncode != 0
        assert "events digest mismatch" in digest_rejected.stderr
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
