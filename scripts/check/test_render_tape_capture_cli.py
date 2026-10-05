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
        assert validated["events"] == 7
        assert validated["presents"] == 1
        legacy_action_profile = {
            "present": False,
            "evidence": "legacy-unknown",
            "stable_exact": False,
            "witness": None,
        }
        assert validated["action_profile_validation"] == legacy_action_profile
        assert validated["scope"] == {
            "production_capture": False,
            "production_provider_replay": False,
            "output_oracle": False,
            "source_oracle": False,
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
            "action_profile_validation": legacy_action_profile,
            "bootstraps": 1,
            "buffer_definitions": [],
            "buffer_destroys": [],
            "buffer_mutations": [],
            "bundle_schema": "dxmt9.render_tape.bundle.v2",
            "chunks": 1,
            "command_event_indices": [4],
            "controls": 1,
            "cpu_window_plan": {
                "bootstrap_state_only": True,
                "page_size": 4096,
                "schema": "dxmt9.render_tape.cpu_window_plan.v1",
                "sources": [{
                    "allowed_record_count": 0,
                    "compatibility_record_count": 0,
                    "coordinator_count": 1,
                    "eligible": False,
                    "event": 5,
                    "lease_eligible": False,
                    "lease_spans": [{
                        "final_lease_span": False,
                        "first_record_index": 0,
                        "owns_lease": False,
                        "record_count": 1,
                    }],
                    "ordered_control_count": 0,
                    "partitioned": True,
                    "record_count": 1,
                }],
            },
            "defines": 2,
            "destroys": 0,
            "dynamic_buffers": 0,
            "event_types": [1, 2, 2, 4, 5, 6, 7],
            "events": 7,
            "events_sha256": inspected["events_sha256"],
            "expected_output_sha256": None,
            "grammar": {
                "content_dispositions": {
                    "complete_seed": 1,
                    "produced_present_output": 1,
                },
                "control_kinds": {"flush_wait": 1},
                "descriptor_kinds": {"surface": 2},
                "destroy_object_kinds": {},
                "mutation_kinds": {"upload": 1},
                "mutation_object_kinds": {"surface": 1},
                "record_types": {"present": 1},
                "section_kinds": {},
                "surface_storages": {
                    "standalone": 1,
                    "swapchain_backbuffer": 1,
                },
                "texture_dimensions": {},
            },
            "handles": 1,
            "last_present_ordinal": 5,
            "mutations": 1,
            "mutation_bytes": 4,
            "present_completions": 1,
            "present_command_event": 4,
            "present_command_events": [4],
            "profile": "frame-tape",
            "query_transcript": {
                "schema": "dxmt9.render_tape.query_transcript.v2",
                "complete": True,
                "query_definitions": 0,
                "issue_records": 0,
                "recorded_polls": 0,
                "definitions": [],
                "issues": [],
                "polls": [],
            },
            "records": 1,
            "schema": "dxmt9.render_tape.v2",
            "surface_descriptors_v2": 2,
            "texture_descriptors_v2": 0,
            "scope": {
                "production_capture": False,
                "production_provider_replay": False,
                "output_oracle": False,
                "source_oracle": False,
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
