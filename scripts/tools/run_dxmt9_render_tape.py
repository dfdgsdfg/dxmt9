#!/usr/bin/env python3

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
from typing import Any


SCHEMA = "dxmt9.render_tape.bundle.v1"
EVENTS_NAME = "events.bin"


def digest(path: pathlib.Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def run_validator(
    validator: pathlib.Path, command: str, events: pathlib.Path
) -> dict[str, Any]:
    completed = subprocess.run(
        [str(validator), command, str(events)],
        check=False,
        text=True,
        capture_output=True,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise SystemExit(f"render tape {command} failed: {detail}")
    try:
        return json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise SystemExit(
            f"render tape validator emitted invalid JSON: {error}"
        ) from error


def producer_revision() -> str:
    root = pathlib.Path(__file__).resolve().parents[2]
    completed = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        check=False,
        text=True,
        capture_output=True,
    )
    return completed.stdout.strip() if completed.returncode == 0 else "unknown"


def pack(args: argparse.Namespace) -> int:
    events = args.events.resolve()
    validator_result = run_validator(args.validator, "validate", events)
    output = args.output_dir.resolve()
    if output.exists():
        if not output.is_dir():
            raise SystemExit(f"output path is not a directory: {output}")
        if any(output.iterdir()):
            raise SystemExit(f"output directory is not empty: {output}")
    output.mkdir(parents=True, exist_ok=True)
    destination = output / EVENTS_NAME
    shutil.copyfile(events, destination)
    event_digest = digest(destination)
    manifest = {
        "schema": SCHEMA,
        "profile": "frame-tape",
        "producer": {
            "path": "scripts/tools/run_dxmt9_render_tape.py",
            "git_revision": producer_revision(),
        },
        "stage": "dump-extract",
        "domain": "replay",
        "inputs": [{"path": str(events), "sha256": digest(events)}],
        "env_snapshot": {},
        "validity": {
            "structural": True,
            "reference_replay": False,
            "validator": validator_result,
        },
        "components": {
            "events": {
                "path": EVENTS_NAME,
                "bytes": destination.stat().st_size,
                "sha256": event_digest,
            }
        },
        "scope": {
            "production_capture": False,
            "production_provider_replay": False,
            "output_oracle": False,
        },
    }
    manifest_path = output / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps({"bundle": str(output), "manifest": str(manifest_path)}))
    return 0


def load_bundle(bundle: pathlib.Path) -> tuple[dict[str, Any], pathlib.Path]:
    manifest_path = bundle.resolve() / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"cannot read render tape manifest {manifest_path}: {error}")
    if manifest.get("schema") != SCHEMA:
        raise SystemExit(f"unsupported render tape bundle schema: {manifest.get('schema')}")
    if manifest.get("profile") != "frame-tape":
        raise SystemExit(f"unsupported render tape profile: {manifest.get('profile')}")
    component = manifest.get("components", {}).get("events")
    if not isinstance(component, dict) or component.get("path") != EVENTS_NAME:
        raise SystemExit("render tape bundle must declare components.events.path=events.bin")
    events = manifest_path.parent / EVENTS_NAME
    if not events.is_file():
        raise SystemExit(f"render tape events component is missing: {events}")
    actual_bytes = events.stat().st_size
    if component.get("bytes") != actual_bytes:
        raise SystemExit(
            f"render tape events size mismatch: manifest={component.get('bytes')} actual={actual_bytes}"
        )
    actual_digest = digest(events)
    if component.get("sha256") != actual_digest:
        raise SystemExit(
            "render tape events digest mismatch: "
            f"manifest={component.get('sha256')} actual={actual_digest}"
        )
    return manifest, events


def validate_or_inspect(args: argparse.Namespace) -> int:
    manifest, events = load_bundle(args.bundle)
    result = run_validator(args.validator, args.command, events)
    result["bundle_schema"] = manifest["schema"]
    result["events_sha256"] = manifest["components"]["events"]["sha256"]
    result["scope"] = manifest.get("scope", {})
    print(json.dumps(result, sort_keys=True))
    return 0


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser(description="Pack and inspect dxmt9 Render Tape bundles")
    subparsers = value.add_subparsers(dest="command", required=True)
    pack_parser = subparsers.add_parser("pack", help="pack a validated v1 event tape")
    pack_parser.add_argument("--events", type=pathlib.Path, required=True)
    pack_parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    pack_parser.add_argument(
        "--validator", type=pathlib.Path,
        default=pathlib.Path("build/tools/dxmt9-render-tape"),
    )
    pack_parser.set_defaults(function=pack)
    for command in ("validate", "inspect"):
        command_parser = subparsers.add_parser(command)
        command_parser.add_argument("bundle", type=pathlib.Path)
        command_parser.add_argument(
            "--validator", type=pathlib.Path,
            default=pathlib.Path("build/tools/dxmt9-render-tape"),
        )
        command_parser.set_defaults(function=validate_or_inspect)
    return value


def main() -> int:
    args = parser().parse_args()
    return args.function(args)


if __name__ == "__main__":
    raise SystemExit(main())
