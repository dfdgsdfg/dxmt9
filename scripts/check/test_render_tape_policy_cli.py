#!/usr/bin/env python3
"""Regression coverage for authenticated structural policy exploration."""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile


def run(*args: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(arg) for arg in args], check=True, capture_output=True, text=True
    )


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_render_tape_policy_cli.py <fixture> <tool> <bundle-tool>"
        )
    fixture = pathlib.Path(sys.argv[1])
    tool = pathlib.Path(sys.argv[2])
    bundle_tool = pathlib.Path(sys.argv[3])
    with tempfile.TemporaryDirectory(prefix="dxmt9-render-tape-policy-") as root:
        root_path = pathlib.Path(root)
        events = root_path / "events.bin"
        identity = root_path / "identity.bin"
        shader = root_path / "shader.bin"
        texture = root_path / "texture.bin"
        bundle = root_path / "bundle"
        run(fixture, "--write-policy-fixture", events, identity)
        shader.write_bytes(bytes((1, 2, 3, 4)))
        texture.write_bytes(bytes((0xAA, 0xBB, 0xCC, 0xDD)))
        run(
            sys.executable,
            bundle_tool,
            "pack",
            "--events",
            events,
            "--identity",
            identity,
            "--blob",
            shader,
            "--blob",
            texture,
            "--output-dir",
            bundle,
            "--validator",
            tool,
        )
        first = json.loads(
            run(
                sys.executable,
                bundle_tool,
                "policy-explore",
                bundle,
                "--validator",
                tool,
            ).stdout
        )
        second = json.loads(
            run(
                sys.executable,
                bundle_tool,
                "policy-explore",
                bundle,
                "--validator",
                tool,
            ).stdout
        )
        assert first == second
        assert first["schema"] == "dxmt9.render_tape.parallel_policy.v1"
        assert first["status"] == "valid"
        assert first["authenticated_input"] is True
        assert first["structural_only"] is True
        assert first["proof_core_validated"] is False
        assert [entry["child_count"] for entry in first["candidates"]] == [
            2,
            4,
            8,
            16,
        ]
        for candidate in first["candidates"]:
            assert candidate["draw_total"] == 16
            assert candidate["primitive_total"] == 16
            assert candidate["pipeline_input_section_facts"] == 16
            assert candidate["uniform_section_facts"] == 0
            children = candidate["children"]
            assert sum(child["record_count"] for child in children) == 16
            assert children[0]["first_record"] == 0
            assert (
                children[-1]["first_record"]
                + children[-1]["record_count"]
                == 16
            )
        assert [entry["reason"] for entry in first["rejections"]] == [
            "coordinator-record",
            "non-draw-record",
        ]
        assert first["identity_validation"]["valid"] is True
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
