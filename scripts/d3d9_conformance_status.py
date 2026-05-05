#!/usr/bin/env python3

from __future__ import annotations

import argparse
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

import tomllib

STATUS_ORDER = ("failing", "partial", "scaffolded", "passing", "skipped", "todo")
ACTION_BY_STATUS = {
    "scaffolded": "run missing evidence",
    "partial": "complete missing lane/arch matrix",
    "failing": "fix implementation",
    "passing": "done",
    "skipped": "out of scope",
    "todo": "scaffold missing PE case",
}


@dataclass(frozen=True)
class CaseStatus:
    executable: str
    function: str
    status: str
    area: str
    owner: str
    lanes: tuple[str, ...]
    arches: tuple[str, ...]
    evidence: tuple[dict[str, Any], ...]

    @property
    def expected_matrix(self) -> set[tuple[str, str]]:
        return {(lane, arch) for lane in self.lanes for arch in self.arches}

    @property
    def evidence_matrix(self) -> set[tuple[str, str]]:
        result: set[tuple[str, str]] = set()
        for item in self.evidence:
            lane = item.get("lane")
            arch = item.get("arch")
            if isinstance(lane, str) and isinstance(arch, str):
                result.add((lane, arch))
        return result

    @property
    def missing_matrix(self) -> set[tuple[str, str]]:
        return self.expected_matrix - self.evidence_matrix


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def default_manifest_path() -> Path:
    return repo_root() / "tests" / "conformance" / "d3d9" / "MANIFEST.toml"


def load_cases(manifest: Path) -> list[CaseStatus]:
    if not manifest.is_file():
        raise FileNotFoundError(f"manifest missing: {manifest}")

    with manifest.open("rb") as handle:
        data = tomllib.load(handle)

    raw_cases = data.get("case")
    if not isinstance(raw_cases, list):
        raise ValueError(f"manifest has no [[case]] entries: {manifest}")

    cases: list[CaseStatus] = []
    for index, case in enumerate(raw_cases, 1):
        if not isinstance(case, dict):
            raise ValueError(f"case #{index}: entry is not a table")

        cases.append(
            CaseStatus(
                executable=required_string(case, "executable", index),
                function=required_string(case, "function", index),
                status=required_string(case, "status", index),
                area=string_or_unknown(case.get("area")),
                owner=string_or_unknown(case.get("owner")),
                lanes=required_string_tuple(case, "lanes", index),
                arches=required_string_tuple(case, "arches", index),
                evidence=tuple(required_dict_list(case.get("evidence", []), "evidence", index)),
            )
        )

    return cases


def required_string(case: dict[str, Any], key: str, index: int) -> str:
    value = case.get(key)
    if not isinstance(value, str) or not value:
        raise ValueError(f"case #{index}: {key} must be a non-empty string")
    return value


def string_or_unknown(value: Any) -> str:
    if isinstance(value, str) and value:
        return value
    return "unknown"


def required_string_tuple(case: dict[str, Any], key: str, index: int) -> tuple[str, ...]:
    value = case.get(key)
    if not isinstance(value, list) or not value or any(not isinstance(item, str) or not item for item in value):
        raise ValueError(f"case #{index}: {key} must be a non-empty string list")
    return tuple(value)


def required_dict_list(value: Any, key: str, index: int) -> list[dict[str, Any]]:
    if not isinstance(value, list):
        raise ValueError(f"case #{index}: {key} must be a list")
    for item_index, item in enumerate(value, 1):
        if not isinstance(item, dict):
            raise ValueError(f"case #{index}: {key} #{item_index} must be a table")
    return value


def status_counts(cases: Iterable[CaseStatus]) -> Counter[str]:
    return Counter(case.status for case in cases)


def bucket_cases(cases: Iterable[CaseStatus]) -> dict[str, list[CaseStatus]]:
    buckets: dict[str, list[CaseStatus]] = defaultdict(list)
    for case in cases:
        buckets[case.status].append(case)
    return buckets


def ordered_statuses(counts: Counter[str]) -> list[str]:
    ordered = list(STATUS_ORDER)
    ordered.extend(sorted(status for status in counts if status not in STATUS_ORDER))
    return ordered


def matrix_text(matrix: Iterable[tuple[str, str]]) -> str:
    values = [f"{lane}/{arch}" for lane, arch in sorted(matrix)]
    return ", ".join(values) if values else "-"


def case_label(case: CaseStatus) -> str:
    return f"{case.function} ({case.area}, {case.owner})"


def render_text(cases: list[CaseStatus], manifest: Path) -> str:
    counts = status_counts(cases)
    buckets = bucket_cases(cases)
    total = len(cases)
    non_passing = total - counts.get("passing", 0)

    lines = [
        "D3D9 Wine-oracle conformance status",
        f"Manifest: {manifest}",
        f"Total cases: {total}",
        f"Full support missing: {non_passing}",
        "",
        "Status counts:",
    ]

    for status in ordered_statuses(counts):
        lines.append(f"  {status}: {counts[status]}")

    lines.extend(["", "Next actions:"])
    for status in ordered_statuses(counts):
        action = ACTION_BY_STATUS.get(status, "review manifest status")
        lines.append(f"  {action}: {counts[status]} {status}")

    lines.extend(["", "Buckets:"])
    for status in ordered_statuses(counts):
        action = ACTION_BY_STATUS.get(status, "review manifest status")
        lines.append(f"  {action} ({status}, {len(buckets[status])}):")
        for case in buckets[status]:
            suffix = ""
            if status in {"partial", "scaffolded"}:
                suffix = f" missing: {matrix_text(case.missing_matrix)}"
            elif status == "failing":
                suffix = f" evidence: {matrix_text(case.evidence_matrix)}"
            lines.append(f"    - {case_label(case)}{suffix}")

    return "\n".join(lines) + "\n"


def render_markdown(cases: list[CaseStatus], manifest: Path) -> str:
    counts = status_counts(cases)
    buckets = bucket_cases(cases)
    total = len(cases)
    non_passing = total - counts.get("passing", 0)

    lines = [
        "# D3D9 Wine-oracle conformance status",
        "",
        f"- Manifest: `{manifest}`",
        f"- Total cases: {total}",
        f"- Full support missing: {non_passing}",
        "",
        "## Status counts",
        "",
        "| Status | Count |",
        "| --- | ---: |",
    ]

    for status in ordered_statuses(counts):
        lines.append(f"| {status} | {counts[status]} |")

    lines.extend(
        [
            "",
            "## Next actions",
            "",
            "| Action | Status | Count |",
            "| --- | --- | ---: |",
        ]
    )
    for status in ordered_statuses(counts):
        action = ACTION_BY_STATUS.get(status, "review manifest status")
        lines.append(f"| {action} | {status} | {counts[status]} |")

    lines.extend(["", "## Buckets", ""])
    for status in ordered_statuses(counts):
        action = ACTION_BY_STATUS.get(status, "review manifest status")
        lines.extend([f"### {action} ({status}, {len(buckets[status])})", ""])
        for case in buckets[status]:
            detail = ""
            if status in {"partial", "scaffolded"}:
                detail = f"; missing: `{matrix_text(case.missing_matrix)}`"
            elif status == "failing":
                detail = f"; evidence: `{matrix_text(case.evidence_matrix)}`"
            lines.append(f"- `{case.function}` ({case.area}, {case.owner}){detail}")
        lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def mermaid_id(status: str) -> str:
    return "status_" + "".join(ch if ch.isalnum() else "_" for ch in status)


def render_mermaid(cases: list[CaseStatus], manifest: Path) -> str:
    counts = status_counts(cases)
    total = len(cases)
    non_passing = total - counts.get("passing", 0)
    lines = [
        "flowchart TD",
        f'  manifest["D3D9 Wine-oracle manifest<br/>{escape_mermaid(str(manifest))}"]',
        f'  total["Total cases: {total}<br/>Full support missing: {non_passing}"]',
        "  manifest --> total",
    ]

    for status in ordered_statuses(counts):
        action = ACTION_BY_STATUS.get(status, "review manifest status")
        node = mermaid_id(status)
        lines.append(f'  {node}["{escape_mermaid(action)}<br/>{status}: {counts[status]}"]')
        lines.append(f"  total --> {node}")

    return "\n".join(lines) + "\n"


def escape_mermaid(text: str) -> str:
    return text.replace("&", "&amp;").replace('"', "&quot;").replace("<", "&lt;").replace(">", "&gt;")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Report Wine-oracle D3D9 conformance status from tests/conformance/d3d9/MANIFEST.toml.",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=default_manifest_path(),
        help="Path to the D3D9 conformance manifest.",
    )
    parser.add_argument(
        "--format",
        choices=("text", "markdown", "mermaid"),
        default="text",
        help="Output format.",
    )
    parser.add_argument(
        "--fail-if-full-support-missing",
        action="store_true",
        help="Exit nonzero unless every manifest case is passing.",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    try:
        cases = load_cases(args.manifest)
    except (OSError, ValueError) as exc:
        print(f"d3d9 conformance status: {exc}", file=sys.stderr)
        return 2

    if args.format == "text":
        output = render_text(cases, args.manifest)
    elif args.format == "markdown":
        output = render_markdown(cases, args.manifest)
    else:
        output = render_mermaid(cases, args.manifest)

    print(output, end="")

    if args.fail_if_full_support_missing and any(case.status != "passing" for case in cases):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
