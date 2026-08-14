#!/usr/bin/env python3
"""Regression coverage for the bounded Render Tape v2 projection CLI."""

from __future__ import annotations

import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile


def digest_range(start: int) -> str:
    return bytes(range(start, start + 32)).hex()


def run(*args: object, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(arg) for arg in args],
        check=check,
        capture_output=True,
        text=True,
    )


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: test_render_tape_projection_cli.py <fixture> <tool>"
        )
    fixture = pathlib.Path(sys.argv[1])
    tool = pathlib.Path(sys.argv[2])
    with tempfile.TemporaryDirectory(prefix="dxmt9-render-tape-project-") as root:
        tape = pathlib.Path(root) / "events.bin"
        run(fixture, "--write-fixture", tape)
        before = hashlib.sha256(tape.read_bytes()).hexdigest()
        common = (
            "--verified-blob",
            f"{digest_range(0x40)}:4",
            "--verified-blob",
            f"{digest_range(0x10)}:4",
        )
        completed = run(
            tool,
            "project",
            tape,
            "--command-event-ordinal",
            6,
            "--first-record",
            1,
            "--record-count",
            2,
            *common,
        )
        artifact = json.loads(completed.stdout)
        assert artifact["schema"] == "dxmt9.render_tape.projection.v1"
        assert artifact["status"] == "projection-ready"
        assert artifact["scope"] == {
            "profile": "frame-tape",
            "claim": "structural-projection-readiness-only",
            "wire_bytes_rewritten": False,
            "legacy_mini_replay_manifest": False,
            "provider_replay": False,
            "gt2_replay": False,
        }
        assert artifact["source"]["sha256"] == before
        assert artifact["selector"] == {
            "command_event_ordinal": 6,
            "first_record_index": 1,
            "record_count": 2,
        }
        assert [(entry["event_ordinal"], entry["record_index"])
                for entry in artifact["draws"]] == [(6, 1), (6, 2)]
        assert artifact["boundaries"]["clear"]["record_index"] == 0
        assert artifact["boundaries"]["present"]["record_index"] == 3
        assert [entry["identity"]["generation"]
                for entry in artifact["objects"]] == [3, 7]
        assert [entry["digest"] for entry in artifact["blob_references"]] == [
            digest_range(0x40),
            digest_range(0x10),
        ]
        assert artifact["conservation"] == {
            "selected_draws": 2,
            "excluded_records": 2,
            "objects": 2,
            "blob_references": 2,
            "excluded_coordinator_events": 3,
        }
        encoded = completed.stdout
        for unsupported_key in (
            "frame_id",
            "source_ordinal",
            "source_seq_id",
            "logical_pass_id",
        ):
            assert unsupported_key not in encoded
        assert hashlib.sha256(tape.read_bytes()).hexdigest() == before

        rejected = run(
            tool,
            "project",
            tape,
            "--command-event-ordinal",
            6,
            "--first-record",
            0,
            "--record-count",
            2,
            *common,
            check=False,
        )
        assert rejected.returncode == 1
        assert rejected.stdout == ""
        assert "status=non-draw-record" in rejected.stderr
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
