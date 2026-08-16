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
        blob = pathlib.Path(directory) / "mutation.bin"
        blob.write_bytes(bytes((0xAA, 0xBB, 0xCC, 0xDD)))
        run(str(generator), "--write-fixture", str(tape))

        blob_ref = (
            "8d70d691c822d55638b6e7fd54cd94170c87d19eb1f628b757506ede5688d297:4"
        )

        validated = json.loads(
            run(
                str(validator), "validate", str(tape),
                "--verified-blob", blob_ref,
            ).stdout
        )
        assert validated == {
            "schema": "dxmt9.render_tape.v2",
            "profile": "frame-tape",
            "valid": True,
            "events": 6,
            "presents": 1,
        }

        inspected = json.loads(
            run(
                str(validator), "inspect", str(tape),
                "--verified-blob", blob_ref,
            ).stdout
        )
        assert inspected["valid"] is True
        assert inspected["events"] == 6
        assert inspected["defines"] == 1
        assert inspected["texture_descriptors_v2"] == 0
        assert inspected["surface_descriptors_v2"] == 1
        assert inspected["mutations"] == 1
        assert inspected["mutation_bytes"] == 4
        assert inspected["chunks"] == 1
        assert inspected["records"] == 1
        assert inspected["handles"] == 1
        assert inspected["present_completions"] == 1
        assert inspected["last_present_ordinal"] == 4

        damaged = pathlib.Path(directory) / "damaged.tape"
        payload = bytearray(tape.read_bytes())
        payload[8:12] = (99).to_bytes(4, "little")
        damaged.write_bytes(payload)
        rejected = run(
            str(validator), "validate", str(damaged),
            "--verified-blob", blob_ref, check=False,
        )
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
            "--blob",
            str(blob),
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
        assert bundle_validated["bundle_schema"] == "dxmt9.render_tape.bundle.v2"
        assert bundle_validated["scope"] == {
            "production_capture": False,
            "production_provider_replay": False,
            "output_oracle": False,
            "source_oracle": False,
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
        assert bundle_inspected["mutations"] == 1

        manifest_path = bundle / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        canonical_blob_path = manifest["components"]["blobs"][0]["path"]
        manifest["components"]["blobs"][0]["path"] = "../mutation.bin"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        escaped_blob = run(
            sys.executable,
            str(bundle_tool),
            "validate",
            str(bundle),
            "--validator",
            str(validator),
            check=False,
        )
        assert escaped_blob.returncode != 0
        assert "blob path must be canonical" in escaped_blob.stderr
        manifest["components"]["blobs"][0]["path"] = canonical_blob_path
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

        extra_blob = bundle / "blobs" / ("0" * 64 + ".bin")
        extra_blob.write_bytes(b"unreachable")
        extra_rejected = run(
            sys.executable,
            str(bundle_tool),
            "validate",
            str(bundle),
            "--validator",
            str(validator),
            check=False,
        )
        assert extra_rejected.returncode != 0
        assert "blob directory does not match" in extra_rejected.stderr
        extra_blob.unlink()

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
