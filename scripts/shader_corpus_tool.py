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
VALID_MODELS = {
    "ffp",
    "hlsl",
    "ps_1_1",
    "ps_1_2",
    "ps_1_3",
    "ps_1_4",
    "ps_2_0",
    "ps_3_0",
    "vs_1_1",
    "vs_2_0",
    "vs_3_0",
}
VALID_OPCODES = {
    "ABS", "ADD", "ALPHA_TEST", "BREAK", "BREAKC", "BREAKP", "CALL", "CALLNZ",
    "CLEAR", "CMP", "CND", "CRS", "DCL", "DEF", "DEFB", "DEFI", "DP2ADD",
    "DP3", "DP4", "DSX", "DSY", "ELSE", "ENDIF", "ENDLOOP", "ENDREP", "EXP",
    "EXPP", "FRC", "HLSL", "IF", "IFC", "LABEL", "LOG", "LOGP", "LOOP",
    "LRP", "M3x2", "M3x3", "M3x4", "M4x3", "M4x4", "MAD", "MAX", "MIN",
    "MOV", "MOVA", "MUL", "NOP", "NRM", "POW", "PROBE", "RCP", "REP",
    "RET", "RSQ", "SETP", "SGE", "SGN", "SINCOS", "SLT", "SUB", "TEX",
    "TEXBEM", "TEXBEML", "TEXCOORD", "TEXDEPTH", "TEXDP3", "TEXDP3TEX",
    "TEXKILL", "TEXLDD", "TEXLDL", "TEXM3x2DEPTH", "TEXM3x2PAD",
    "TEXM3x2TEX", "TEXM3x3", "TEXM3x3DIFF", "TEXM3x3PAD",
    "TEXM3x3SPEC", "TEXM3x3TEX", "TEXM3x3VSPEC", "TEXREG2AR",
    "TEXREG2GB", "TEXREG2RGB",
}
VALID_STATUSES = {"passing", "failing", "skipped"}
REQUIRED_PROVENANCE_FIELDS = {
    "source",
    "source_kind",
    "license",
    "license_scope",
    "oracle",
    "oracle-date",
}
D3DBC_OPCODE_NAMES = {
    0: "NOP",
    1: "MOV",
    2: "ADD",
    3: "SUB",
    4: "MAD",
    5: "MUL",
    6: "RCP",
    7: "RSQ",
    8: "DP3",
    9: "DP4",
    10: "MIN",
    11: "MAX",
    12: "SLT",
    13: "SGE",
    14: "EXP",
    15: "LOG",
    18: "LRP",
    19: "FRC",
    20: "M4x4",
    21: "M4x3",
    22: "M3x4",
    23: "M3x3",
    24: "M3x2",
    25: "CALL",
    26: "CALLNZ",
    27: "LOOP",
    28: "RET",
    29: "ENDLOOP",
    30: "LABEL",
    31: "DCL",
    32: "POW",
    33: "CRS",
    34: "SGN",
    35: "ABS",
    36: "NRM",
    37: "SINCOS",
    38: "REP",
    39: "ENDREP",
    40: "IF",
    41: "IFC",
    42: "ELSE",
    43: "ENDIF",
    44: "BREAK",
    45: "BREAKC",
    46: "MOVA",
    47: "DEFB",
    48: "DEFI",
    64: "TEXCOORD",
    65: "TEXKILL",
    66: "TEX",
    67: "TEXBEM",
    68: "TEXBEML",
    69: "TEXREG2AR",
    70: "TEXREG2GB",
    71: "TEXM3x2PAD",
    72: "TEXM3x2TEX",
    73: "TEXM3x3PAD",
    74: "TEXM3x3TEX",
    75: "TEXM3x3DIFF",
    76: "TEXM3x3SPEC",
    77: "TEXM3x3VSPEC",
    78: "EXPP",
    79: "LOGP",
    80: "CND",
    81: "DEF",
    82: "TEXREG2RGB",
    83: "TEXDP3TEX",
    84: "TEXM3x2DEPTH",
    85: "TEXDP3",
    86: "TEXM3x3",
    87: "TEXDEPTH",
    88: "CMP",
    90: "DP2ADD",
    91: "DSX",
    92: "DSY",
    93: "TEXLDD",
    94: "SETP",
    95: "TEXLDL",
    96: "BREAKP",
}
D3DBC_COMMENT_OPCODE = 0xFFFE
D3DBC_END_OPCODE = 0xFFFF


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
    "source_kind",
    "license",
    "license_scope",
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
    append_field("source_kind", fields.get("source_kind"))
    append_field("license", fields.get("license"))
    append_field("license_scope", fields.get("license_scope"))
    append_field("upstream-url", fields.get("upstream-url"))
    append_field("upstream-commit", fields.get("upstream-commit"))
    append_field("oracle", fields.get("oracle"))
    append_field("oracle-env", fields.get("oracle-env"))
    append_field("oracle-date", fields.get("oracle-date"))

    for key in sorted(fields):
        if key in {
            "[provenance]",
            "source",
            "source_kind",
            "license",
            "license_scope",
            "upstream-url",
            "upstream-commit",
            "oracle",
            "oracle-env",
            "oracle-date",
        }:
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


def list_field(value: Any) -> list[str]:
    if not isinstance(value, list):
        return []
    return [item for item in value if isinstance(item, str)]


def d3dbc_model_from_version(token: int) -> str | None:
    shader_type = (token >> 16) & 0xFFFF
    major = (token >> 8) & 0xFF
    minor = token & 0xFF
    if shader_type == 0xFFFF:
        return f"ps_{major}_{minor}"
    if shader_type == 0xFFFE:
        return f"vs_{major}_{minor}"
    return None


def decode_d3dbc_model_opcodes(words: list[int]) -> tuple[str | None, set[str]]:
    if not words:
        return None, set()

    model = d3dbc_model_from_version(words[0])
    if model is None:
        return None, set()

    opcodes: set[str] = set()
    index = 1
    while index < len(words):
        token = words[index]
        opcode_value = token & 0xFFFF
        if opcode_value == D3DBC_END_OPCODE:
            break
        if opcode_value == D3DBC_COMMENT_OPCODE:
            index += 1 + ((token >> 16) & 0x7FFF)
            continue

        opcode_name = D3DBC_OPCODE_NAMES.get(opcode_value)
        if opcode_name in VALID_OPCODES:
            opcodes.add(opcode_name)

        length = (token >> 24) & 0xF
        index += 1 + length

    return model, opcodes


def parse_d3dbc_hex_blocks(text: str) -> list[list[int]]:
    blocks: list[list[int]] = []
    current_words: list[int] | None = None

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith("["):
            if current_words is not None:
                blocks.append(current_words)
                current_words = None
            if line in {"[pixel shader d3dbc-hex]", "[vertex shader d3dbc-hex]"}:
                current_words = []
            continue
        if current_words is None or not line or line.startswith(";"):
            continue
        for word in line.split():
            try:
                current_words.append(int(word, 16))
            except ValueError:
                current_words = None
                break

    if current_words is not None:
        blocks.append(current_words)

    return blocks


def manifest_model_opcode_pairs(root: Path, relative: str, models: list[str], opcodes: list[str]) -> set[tuple[str, str]]:
    path = localize_path(root, relative)
    decoded_pairs: set[tuple[str, str]] = set()
    if path.exists():
        for block in parse_d3dbc_hex_blocks(read_text(path)):
            model, block_opcodes = decode_d3dbc_model_opcodes(block)
            if model is None:
                continue
            decoded_pairs.update((model, opcode) for opcode in block_opcodes)

    if decoded_pairs:
        return decoded_pairs

    if len(models) == 1:
        return {(models[0], opcode) for opcode in opcodes}

    return set()


def manifest_list_files(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve()
    manifest_path = Path(args.manifest).resolve() if args.manifest else default_manifest_path(root)
    entries = load_corpus_manifest(manifest_path)
    for entry in entries:
        if args.status and entry.get("status") != args.status:
            continue
        print(entry["file"])
    return 0


def manifest_gaps(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve()
    manifest_path = Path(args.manifest).resolve() if args.manifest else default_manifest_path(root)
    entries = load_corpus_manifest(manifest_path)

    missing_metadata: list[str] = []
    covered_models: set[str] = set()
    covered_opcodes: set[str] = set()
    covered_model_opcodes: set[tuple[str, str]] = set()
    status_counts: dict[str, int] = {}
    non_passing: list[str] = []

    for entry in entries:
        relative = str(entry.get("file", "<missing>"))
        models = list_field(entry.get("models"))
        opcodes = list_field(entry.get("opcodes"))
        status = str(entry.get("status", ""))
        status_counts[status] = status_counts.get(status, 0) + 1

        if not models:
            missing_metadata.append(f"{relative}: missing models")
        if not opcodes:
            missing_metadata.append(f"{relative}: missing opcodes")
        if status not in VALID_STATUSES:
            missing_metadata.append(f"{relative}: invalid status {status!r}")

        unknown_models = sorted(model for model in models if model not in VALID_MODELS)
        unknown_opcodes = sorted(opcode for opcode in opcodes if opcode not in VALID_OPCODES)
        if unknown_models:
            missing_metadata.append(f"{relative}: unknown models {', '.join(unknown_models)}")
        if unknown_opcodes:
            missing_metadata.append(f"{relative}: unknown opcodes {', '.join(unknown_opcodes)}")

        missing_provenance = sorted(field for field in REQUIRED_PROVENANCE_FIELDS if not entry.get(field))
        if missing_provenance:
            missing_metadata.append(f"{relative}: missing provenance {', '.join(missing_provenance)}")

        if status == "passing":
            model_opcode_pairs = manifest_model_opcode_pairs(root, relative, models, opcodes)
            covered_model_opcodes.update(model_opcode_pairs)
            covered_models.update(model for model, _ in model_opcode_pairs)
            covered_opcodes.update(opcode for _, opcode in model_opcode_pairs)
        else:
            non_passing.append(f"{relative}: {status or '<missing>'}")

    tracked_models = sorted(model for model in VALID_MODELS if model not in {"ffp", "hlsl"})
    missing_models = sorted(model for model in tracked_models if model not in covered_models)
    tracked_opcodes = sorted(opcode for opcode in VALID_OPCODES if opcode not in {"ALPHA_TEST", "CLEAR", "HLSL", "PROBE"})
    missing_opcodes = sorted(opcode for opcode in tracked_opcodes if opcode not in covered_opcodes)
    total_matrix_pairs = len(tracked_models) * len(tracked_opcodes)
    covered_matrix_pairs = sum(
        1 for model in tracked_models for opcode in tracked_opcodes if (model, opcode) in covered_model_opcodes
    )
    missing_opcodes_by_model = {
        model: [opcode for opcode in tracked_opcodes if (model, opcode) not in covered_model_opcodes]
        for model in tracked_models
    }

    print("shader corpus gap report")
    print(f"manifest: {manifest_path}")
    print(f"entries: {len(entries)}")
    print("status counts: " + ", ".join(f"{status}={status_counts[status]}" for status in sorted(status_counts)))
    print()
    print(f"covered passing models ({len(covered_models)}): {', '.join(sorted(covered_models)) or '<none>'}")
    print(f"missing models ({len(missing_models)}): {', '.join(missing_models) or '<none>'}")
    print()
    print(f"covered passing opcodes ({len(covered_opcodes)}): {', '.join(sorted(covered_opcodes)) or '<none>'}")
    print(f"missing opcodes ({len(missing_opcodes)}): {', '.join(missing_opcodes) or '<none>'}")
    print()
    print(f"covered model x opcode pairs ({covered_matrix_pairs}/{total_matrix_pairs})")
    print("covered opcodes by model:")
    printed_model_coverage = False
    for model in tracked_models:
        covered_for_model = [opcode for opcode in tracked_opcodes if (model, opcode) in covered_model_opcodes]
        if not covered_for_model:
            continue
        printed_model_coverage = True
        print(f"  {model} ({len(covered_for_model)}): {', '.join(covered_for_model)}")
    if not printed_model_coverage:
        print("  <none>")
    print()
    print("missing opcodes by model:")
    printed_model_gap = False
    for model in tracked_models:
        missing_for_model = missing_opcodes_by_model[model]
        if not missing_for_model:
            continue
        printed_model_gap = True
        if len(missing_for_model) == len(tracked_opcodes):
            print(f"  {model} ({len(missing_for_model)}): <all tracked opcodes>")
        else:
            print(f"  {model} ({len(missing_for_model)}): {', '.join(missing_for_model)}")
    if not printed_model_gap:
        print("  <none>")
    print()
    if non_passing:
        print("non-passing tests:")
        for item in non_passing:
            print(f"  {item}")
        print()

    if missing_metadata:
        print("metadata gaps:")
        for item in missing_metadata:
            print(f"  {item}")
    else:
        print("metadata gaps: none")

    return 1 if args.fail_on_metadata_gaps and missing_metadata else 0


def update_manifest_entry(entry: dict[str, Any], upstream_commit: str, oracle_date: str) -> bool:
    changed = False
    if entry.get("source") == "vkd3d":
        if entry.get("source_kind") != "third-party-fixture":
            entry["source_kind"] = "third-party-fixture"
            changed = True
        if entry.get("license") != "LGPL-2.1-or-later":
            entry["license"] = "LGPL-2.1-or-later"
            changed = True
        if entry.get("license_scope") != "third-party-fixture":
            entry["license_scope"] = "third-party-fixture"
            changed = True
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
        raise SystemExit("sync requires --upstream-root or DXMT_UPSTREAM_ROOT")

    upstream_commit = get_upstream_commit(upstream_root, args.upstream_commit or os.environ.get("DXMT_UPSTREAM_COMMIT"))
    oracle_date = args.oracle_date or os.environ.get("DXMT_ORACLE_DATE") or dt.date.today().isoformat()
    upstream_url = args.upstream_url or os.environ.get("DXMT_UPSTREAM_URL") or DEFAULT_VKD3D_URL

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
        merged_fields["source_kind"] = local_fields.get("source_kind", "third-party-fixture")
        merged_fields["license"] = local_fields.get("license", "LGPL-2.1-or-later")
        merged_fields["license_scope"] = local_fields.get("license_scope", "third-party-fixture")
        merged_fields["upstream-url"] = local_fields.get("upstream-url", upstream_url)
        merged_fields["upstream-commit"] = upstream_commit
        merged_fields["oracle"] = local_fields.get("oracle", DEFAULT_ORACLE)
        merged_fields["oracle-env"] = local_fields.get("oracle-env", DEFAULT_ORACLE_ENV)
        merged_fields["oracle-date"] = oracle_date

        for key, value in local_fields.items():
            if key in {
                "[provenance]",
                "source",
                "source_kind",
                "license",
                "license_scope",
                "upstream-url",
                "upstream-commit",
                "oracle",
                "oracle-env",
                "oracle-date",
            }:
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
    upstream_root_arg = args.upstream_root or os.environ.get("DXMT_UPSTREAM_ROOT")
    upstream_commit_arg = args.upstream_commit or os.environ.get("DXMT_UPSTREAM_COMMIT")

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
    print("report-only: set DXMT_UPSTREAM_ROOT or pass --upstream-root to compare provenance commits")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="dxmt9 shader corpus maintenance tool")
    subparsers = parser.add_subparsers(dest="command", required=True)

    sync = subparsers.add_parser("sync", help="sync vkd3d-sourced corpus files from upstream")
    sync.add_argument("--root", default=str(repo_root() / "tests" / "shader_runner" / "corpus"))
    sync.add_argument("--manifest")
    sync.add_argument("--upstream-root")
    sync.add_argument("--upstream-commit")
    sync.add_argument("--upstream-url")
    sync.add_argument("--oracle-date")
    sync.add_argument("--dry-run", action="store_true")
    sync.add_argument("--quiet", action="store_true")

    drift = subparsers.add_parser("drift", help="report provenance drift against an upstream checkout")
    drift.add_argument("--root", default=str(repo_root() / "tests" / "shader_runner" / "corpus"))
    drift.add_argument("--manifest")
    drift.add_argument("--upstream-root")
    drift.add_argument("--upstream-commit")

    list_files = subparsers.add_parser("list-files", help="list corpus files from the manifest")
    list_files.add_argument("--root", default=str(repo_root() / "tests" / "shader_runner" / "corpus"))
    list_files.add_argument("--manifest")
    list_files.add_argument("--status", choices=sorted(VALID_STATUSES))

    gaps = subparsers.add_parser("gaps", help="report corpus model, opcode, and provenance gaps")
    gaps.add_argument("--root", default=str(repo_root() / "tests" / "shader_runner" / "corpus"))
    gaps.add_argument("--manifest")
    gaps.add_argument("--fail-on-metadata-gaps", action="store_true")

    return parser


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command == "sync":
        return sync_corpus(args)
    if args.command == "drift":
        return drift_report(args)
    if args.command == "list-files":
        return manifest_list_files(args)
    if args.command == "gaps":
        return manifest_gaps(args)
    raise AssertionError("unreachable")


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
