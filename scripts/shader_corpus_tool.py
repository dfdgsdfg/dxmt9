#!/usr/bin/env python3

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

import tomllib

DEFAULT_VKD3D_URL = "https://gitlab.winehq.org/wine/vkd3d"
DEFAULT_WINE_URL = "https://github.com/wine-mirror/wine"
DEFAULT_ORACLE_ENV = "Windows 11 / WARP"
DEFAULT_ORACLE = "shader_runner_d3d9"


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def default_manifest_path(root: Path) -> Path:
    return root / "MANIFEST.toml"


def load_manifest(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        raise FileNotFoundError(f"manifest missing: {path}")

    with path.open("rb") as handle:
        data = tomllib.load(handle)

    entries = data.get("test")
    if not isinstance(entries, list):
        raise ValueError(f"manifest has no [[test]] entries: {path}")

    result: list[dict[str, Any]] = []
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise ValueError(f"manifest entry #{index + 1} is not a table")
        if "file" not in entry:
            raise ValueError(f"manifest entry #{index + 1} is missing file")
        result.append(dict(entry))
    return result


def toml_string(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, str):
        return json.dumps(value)
    if isinstance(value, list):
        return "[" + ", ".join(toml_string(item) for item in value) + "]"
    raise TypeError(f"unsupported manifest value type: {type(value)!r}")


KEY_ORDER = (
    "file",
    "status",
    "source",
    "models",
    "opcodes",
    "oracle",
    "oracle-date",
    "upstream-commit",
)


def manifest_entry_to_text(entry: dict[str, Any]) -> str:
    lines = ["[[test]]"]
    emitted: set[str] = set()

    for key in KEY_ORDER:
        if key in entry and entry[key] is not None:
            lines.append(f"{key} = {toml_string(entry[key])}")
            emitted.add(key)

    for key in sorted(entry):
        if key in emitted or entry[key] is None:
            continue
        lines.append(f"{key} = {toml_string(entry[key])}")

    return "\n".join(lines)


def manifest_to_text(entries: list[dict[str, Any]]) -> str:
    ordered = sorted(entries, key=lambda entry: str(entry["file"]))
    return "\n\n".join(manifest_entry_to_text(entry) for entry in ordered) + "\n"


def atomic_write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", newline="\n", delete=False, dir=path.parent, prefix=path.name + ".") as handle:
        handle.write(text)
        handle.flush()
        os.fsync(handle.fileno())
        temp_path = Path(handle.name)
    os.replace(temp_path, path)


def split_provenance(text: str) -> tuple[dict[str, str], str]:
    lines = text.splitlines(keepends=True)
    prefix_end = 0
    while prefix_end < len(lines) and (lines[prefix_end].startswith(";") or not lines[prefix_end].strip()):
        prefix_end += 1

    provenance_lines = lines[:prefix_end]
    body_lines = lines[prefix_end:]
    while body_lines and not body_lines[0].strip():
        body_lines = body_lines[1:]

    provenance: dict[str, str] = {}
    for raw_line in provenance_lines:
        stripped = raw_line.lstrip(";").strip()
        if stripped == "[provenance]":
            provenance["[provenance]"] = "true"
            continue
        if ":" not in stripped:
            continue
        key, value = stripped.split(":", 1)
        provenance[key.strip()] = value.strip()

    return provenance, "".join(body_lines)


def build_provenance_block(fields: dict[str, str]) -> str:
    lines = ["; [provenance]"]

    def append_field(key: str, value: str | None) -> None:
        if value is not None and value != "":
            lines.append(f"; {key}: {value}")

    append_field("source", fields.get("source"))
    append_field("upstream-url", fields.get("upstream-url"))
    append_field("upstream-commit", fields.get("upstream-commit"))
    append_field("oracle", fields.get("oracle"))
    append_field("oracle-env", fields.get("oracle-env"))
    append_field("oracle-date", fields.get("oracle-date"))

    for key in sorted(fields):
        if key in {"[provenance]", "source", "upstream-url", "upstream-commit", "oracle", "oracle-env", "oracle-date"}:
            continue
        append_field(key, fields[key])

    return "\n".join(lines) + "\n\n"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def write_text(path: Path, text: str) -> None:
    atomic_write(path, text)


def get_upstream_commit(upstream_root: Path, explicit_commit: str | None) -> str:
    if explicit_commit:
        return explicit_commit

    result = subprocess.run(
        ["git", "-C", str(upstream_root), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    )
    commit = result.stdout.strip()
    if not commit:
        raise RuntimeError(f"failed to resolve HEAD for upstream checkout: {upstream_root}")
    return commit


def localize_path(root: Path, relative: str) -> Path:
    path = (root / relative).resolve()
    root_resolved = root.resolve()
    if root_resolved not in path.parents and path != root_resolved:
        raise ValueError(f"path escapes root: {relative}")
    return path


def load_corpus_manifest(manifest_path: Path) -> list[dict[str, Any]]:
    return load_manifest(manifest_path)


def update_manifest_entry(entry: dict[str, Any], upstream_commit: str, oracle_date: str) -> bool:
    changed = False
    if entry.get("source") == "vkd3d":
        if entry.get("upstream-commit") != upstream_commit:
            entry["upstream-commit"] = upstream_commit
            changed = True
        if entry.get("oracle-date") != oracle_date:
            entry["oracle-date"] = oracle_date
            changed = True
    return changed


def sync_corpus(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve()
    manifest_path = Path(args.manifest).resolve() if args.manifest else default_manifest_path(root)
    upstream_root = Path(args.upstream_root).resolve() if args.upstream_root else None

    if upstream_root is None:
        raise SystemExit("sync requires --upstream-root or DXMT9_UPSTREAM_ROOT")

    upstream_commit = get_upstream_commit(upstream_root, args.upstream_commit or os.environ.get("DXMT9_UPSTREAM_COMMIT"))
    oracle_date = args.oracle_date or os.environ.get("DXMT9_ORACLE_DATE") or dt.date.today().isoformat()
    upstream_url = args.upstream_url or os.environ.get("DXMT9_UPSTREAM_URL") or DEFAULT_VKD3D_URL

    entries = load_corpus_manifest(manifest_path)
    manifest_changed = False
    touched_files = 0

    for entry in entries:
        if entry.get("source") != "vkd3d":
            continue

        relative = str(entry["file"])
        local_path = localize_path(root, relative)
        upstream_path = localize_path(upstream_root, relative)
        if not local_path.exists():
            raise FileNotFoundError(f"local corpus file missing: {local_path}")
        if not upstream_path.exists():
            raise FileNotFoundError(f"upstream corpus file missing: {upstream_path}")

        local_fields, _ = split_provenance(read_text(local_path))
        if local_fields.get("source") not in {"vkd3d", None}:
            raise ValueError(f"refusing to sync non-vkd3d source file: {relative}")

        _, upstream_body = split_provenance(read_text(upstream_path))
        if upstream_body.strip() == "":
            raise ValueError(f"upstream corpus file has no shader body: {upstream_path}")

        merged_fields: dict[str, str] = {}
        merged_fields["source"] = "vkd3d"
        merged_fields["upstream-url"] = local_fields.get("upstream-url", upstream_url)
        merged_fields["upstream-commit"] = upstream_commit
        merged_fields["oracle"] = local_fields.get("oracle", DEFAULT_ORACLE)
        merged_fields["oracle-env"] = local_fields.get("oracle-env", DEFAULT_ORACLE_ENV)
        merged_fields["oracle-date"] = oracle_date

        for key, value in local_fields.items():
            if key in {"[provenance]", "source", "upstream-url", "upstream-commit", "oracle", "oracle-env", "oracle-date"}:
                continue
            merged_fields[key] = value

        new_text = build_provenance_block(merged_fields) + upstream_body.lstrip("\n")
        if not new_text.endswith("\n"):
            new_text += "\n"

        current_text = read_text(local_path)
        if current_text != new_text:
            if not args.dry_run:
                write_text(local_path, new_text)
            touched_files += 1
            print(f"sync: {relative}")

        if update_manifest_entry(entry, upstream_commit, oracle_date):
            manifest_changed = True

    if manifest_changed:
        if args.dry_run:
            print(f"manifest: would rewrite {manifest_path}")
        else:
            write_text(manifest_path, manifest_to_text(entries))
            print(f"manifest: updated {manifest_path}")
    elif not args.quiet:
        print(f"manifest: unchanged {manifest_path}")

    print(f"sync complete: {touched_files} file(s) touched")
    return 0


def read_provenance_from_file(path: Path) -> dict[str, str]:
    provenance, _ = split_provenance(read_text(path))
    return provenance


def drift_report(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve()
    manifest_path = Path(args.manifest).resolve() if args.manifest else default_manifest_path(root)
    upstream_root_arg = args.upstream_root or os.environ.get("DXMT9_UPSTREAM_ROOT")
    upstream_commit_arg = args.upstream_commit or os.environ.get("DXMT9_UPSTREAM_COMMIT")

    entries = load_corpus_manifest(manifest_path)

    print("shader corpus drift report")
    print(f"manifest: {manifest_path}")

    if upstream_root_arg:
        upstream_root = Path(upstream_root_arg).resolve()
        upstream_commit = get_upstream_commit(upstream_root, upstream_commit_arg)
        print(f"upstream checkout: {upstream_root}")
        print(f"upstream HEAD: {upstream_commit}")
        print()

        behind: list[str] = []
        current: list[str] = []
        mismatched: list[str] = []

        for entry in entries:
            if entry.get("source") != "vkd3d":
                continue
            relative = str(entry["file"])
            local_path = localize_path(root, relative)
            if not local_path.exists():
                mismatched.append(f"{relative} missing")
                continue

            file_provenance = read_provenance_from_file(local_path)
            file_commit = file_provenance.get("upstream-commit")
            manifest_commit = entry.get("upstream-commit")
            if file_provenance.get("source") != "vkd3d":
                mismatched.append(f"{relative} source={file_provenance.get('source', '<missing>')}")
                continue
            if file_commit != manifest_commit:
                mismatched.append(f"{relative} provenance/manifest mismatch file={file_commit} manifest={manifest_commit}")
                continue
            if file_commit != upstream_commit:
                behind.append(f"{relative} recorded={file_commit} head={upstream_commit}")
            else:
                current.append(relative)

        if current:
            print("up-to-date:")
            for item in current:
                print(f"  {item}")
            print()
        if behind:
            print("behind:")
            for item in behind:
                print(f"  {item}")
            print()
        if mismatched:
            print("mismatched:")
            for item in mismatched:
                print(f"  {item}")
            print()

        if not behind and not mismatched:
            print("report: all tracked vkd3d files are aligned with the configured upstream commit")
        else:
            print(f"report: {len(behind)} behind, {len(mismatched)} mismatched")
        return 0

    print("upstream checkout: not configured")
    print("report-only: no upstream comparison is available")
    print()

    for entry in entries:
        print("---")
        print(f"file: {entry.get('file')}")
        print(f"source: {entry.get('source')}")
        print(f"oracle: {entry.get('oracle')}")

    print()
    print("report-only: set DXMT9_UPSTREAM_ROOT or pass --upstream-root to compare provenance commits")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="dxmt9 shader corpus maintenance tool")
    subparsers = parser.add_subparsers(dest="command", required=True)

    sync = subparsers.add_parser("sync", help="sync vkd3d-sourced corpus files from upstream")
    sync.add_argument("--root", default=str(repo_root() / "tests" / "shader_tests"))
    sync.add_argument("--manifest")
    sync.add_argument("--upstream-root")
    sync.add_argument("--upstream-commit")
    sync.add_argument("--upstream-url")
    sync.add_argument("--oracle-date")
    sync.add_argument("--dry-run", action="store_true")
    sync.add_argument("--quiet", action="store_true")

    drift = subparsers.add_parser("drift", help="report provenance drift against an upstream checkout")
    drift.add_argument("--root", default=str(repo_root() / "tests" / "shader_tests"))
    drift.add_argument("--manifest")
    drift.add_argument("--upstream-root")
    drift.add_argument("--upstream-commit")

    return parser


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command == "sync":
        return sync_corpus(args)
    if args.command == "drift":
        return drift_report(args)
    raise AssertionError("unreachable")


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
