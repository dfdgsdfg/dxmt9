#!/usr/bin/env python3

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
from typing import Any


SCHEMA = "dxmt9.render_tape.bundle.v2"
EVENTS_NAME = "events.bin"


def digest(path: pathlib.Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def run_validator(
    validator: pathlib.Path,
    command: str,
    events: pathlib.Path,
    blob_refs: list[str],
) -> dict[str, Any]:
    arguments = [str(validator), command, str(events)]
    for reference in blob_refs:
        arguments.extend(("--verified-blob", reference))
    completed = subprocess.run(
        arguments,
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
    blob_sources = [path.resolve() for path in args.blob]
    blobs_by_digest: dict[str, tuple[pathlib.Path, int]] = {}
    for path in blob_sources:
        sha256 = digest(path)
        size = path.stat().st_size
        blobs_by_digest.setdefault(sha256, (path, size))
    blob_refs = [
        f"{sha256}:{size}"
        for sha256, (_, size) in blobs_by_digest.items()
    ]
    validator_result = run_validator(
        args.validator, "validate", events, blob_refs
    )
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
    blob_components = []
    blobs_directory = output / "blobs"
    if blobs_by_digest:
        blobs_directory.mkdir()
    for sha256, (source, size) in blobs_by_digest.items():
        blob_name = f"{sha256}.bin"
        shutil.copyfile(source, blobs_directory / blob_name)
        blob_components.append(
            {
                "path": f"blobs/{blob_name}",
                "bytes": size,
                "sha256": sha256,
            }
        )
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
            },
            "blobs": blob_components,
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


def load_bundle(
    bundle: pathlib.Path,
) -> tuple[dict[str, Any], pathlib.Path, list[str]]:
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
    blob_components = manifest.get("components", {}).get("blobs", [])
    if not isinstance(blob_components, list):
        raise SystemExit("render tape components.blobs must be an array")
    blob_refs = []
    seen_digests: set[str] = set()
    for blob in blob_components:
        if not isinstance(blob, dict):
            raise SystemExit("render tape blob component must be an object")
        claimed_digest = blob.get("sha256")
        if (
            not isinstance(claimed_digest, str)
            or len(claimed_digest) != 64
            or any(value not in "0123456789abcdef" for value in claimed_digest)
        ):
            raise SystemExit("render tape blob digest must be lowercase SHA-256")
        if claimed_digest in seen_digests:
            raise SystemExit(f"duplicate render tape blob digest: {claimed_digest}")
        seen_digests.add(claimed_digest)
        expected_path = f"blobs/{claimed_digest}.bin"
        if blob.get("path") != expected_path:
            raise SystemExit(
                "render tape blob path must be canonical: "
                f"expected={expected_path} actual={blob.get('path')}"
            )
        path = manifest_path.parent / expected_path
        if not path.is_file():
            raise SystemExit(f"render tape blob component is missing: {path}")
        actual_bytes = path.stat().st_size
        actual_digest = digest(path)
        if blob.get("bytes") != actual_bytes:
            raise SystemExit(
                f"render tape blob size mismatch: manifest={blob.get('bytes')} actual={actual_bytes}"
            )
        if claimed_digest != actual_digest:
            raise SystemExit(
                "render tape blob digest mismatch: "
                f"manifest={claimed_digest} actual={actual_digest}"
            )
        blob_refs.append(f"{actual_digest}:{actual_bytes}")
    return manifest, events, blob_refs


def validate_or_inspect(args: argparse.Namespace) -> int:
    manifest, events, blob_refs = load_bundle(args.bundle)
    result = run_validator(args.validator, args.command, events, blob_refs)
    result["bundle_schema"] = manifest["schema"]
    result["events_sha256"] = manifest["components"]["events"]["sha256"]
    result["scope"] = manifest.get("scope", {})
    print(json.dumps(result, sort_keys=True))
    return 0


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser(description="Pack and inspect dxmt9 Render Tape bundles")
    subparsers = value.add_subparsers(dest="command", required=True)
    pack_parser = subparsers.add_parser("pack", help="pack a validated v2 event tape")
    pack_parser.add_argument("--events", type=pathlib.Path, required=True)
    pack_parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    pack_parser.add_argument("--blob", type=pathlib.Path, action="append", default=[])
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
