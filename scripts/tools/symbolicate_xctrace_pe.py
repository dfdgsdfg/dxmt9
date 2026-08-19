#!/usr/bin/env python3
"""Join an xctrace `time-profile` export against a dxmt9 PE module-map log.

Tier 1 of PE 32-bit symbolication (see agents/rules/metal_debugging.rules.md
and agents/rules/environment_variables_bridge.rules.md): xctrace samples of
the Wine game thread are largely opaque because the 32-bit PE code (game exe,
our PE `d3d9.dll`, `winemetal.dll`, and Wine's own PE-side DLLs) has no dyld
image xctrace can attribute to. `DXMT9_PE_MODULE_MAP=1` makes the PE side log
its own Toolhelp32 module snapshot (`[dxmt9-pe-module-map] module=... base=...
size=...`) once at device creation. This tool re-symbolicates the top frame of
each sample on a selected thread against that module map by address range,
turning "95% opaque" into a per-module CPU breakdown.

Parsing approach for the xctrace XML mirrors
scripts/tools/summarize_xctrace_cpu_threads.py: rows carry either an inline
element (`id="..."`) or a `ref="..."` back-reference into an interned pool of
previously-seen elements with the same tag name, so every lookup resolves refs
against an id->element map built once over the whole document.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Iterable, Optional


MODULE_LINE_RE = re.compile(
    r"\[dxmt9-pe-module-map\].*?\bmodule=(?P<module>\S+)\s+base=0x(?P<base>[0-9a-fA-F]+)\s+size=0x(?P<size>[0-9a-fA-F]+)"
)
PROBE_LINE_RE = re.compile(
    r"\[dxmt9-pe-module-map\].*?\bprobe=(?P<probe>\S+)\s+addr=0x(?P<addr>[0-9a-fA-F]+)"
    r"(?:\s+contained=(?P<contained>[01]))?"
)

# 4 GiB boundary used to separate "plausibly 32-bit PE address space" from
# "definitely 64-bit host/unix address space" when a top frame address falls
# outside every logged module range.
FOUR_GIB = 1 << 32

TID_IN_THREAD_FMT_RE = re.compile(r"\(0x([0-9a-fA-F]+)\)")

# A raw, unsymbolicated top frame from xctrace shows up as a bare hex address
# in the `name` attribute (with or without a leading "0x"). Anything else is
# treated as an already dyld-symbolicated frame.
HEX_ADDR_RE = re.compile(r"^(0x)?[0-9a-fA-F]+$")


class ModuleMap:
    """Sorted, non-overlapping-by-construction module base/size ranges."""

    def __init__(self) -> None:
        self._ranges: list[tuple[int, int, str]] = []  # (base, end, name)

    def add(self, name: str, base: int, size: int) -> None:
        if size <= 0:
            return
        self._ranges.append((base, base + size, name))

    def finalize(self) -> None:
        # Deterministic order: by base address, then name, so classify() is
        # reproducible and ties in overlapping (stale/reused) ranges resolve
        # the same way on every run.
        self._ranges.sort(key=lambda r: (r[0], r[2]))

    def classify(self, addr: int) -> str:
        for base, end, name in self._ranges:
            if base <= addr < end:
                return name
        if addr < FOUR_GIB:
            return "unknown_32bit"
        return "unknown_64bit"

    def contains_in_module_matching(self, name_substring: str, addr: int) -> bool:
        """True if `addr` falls inside a logged module whose name contains
        `name_substring` (case-insensitive). Used for the probe-address
        validation: the probe is our own d3d9.dll, so this checks containment
        by module identity rather than requiring an exact name match against
        the probe symbol's own name (which is unrelated to any module name)."""
        needle = name_substring.lower()
        for base, end, name in self._ranges:
            if needle in name.lower() and base <= addr < end:
                return True
        return False

    def __len__(self) -> int:
        return len(self._ranges)


def parse_module_map_log(path: Path) -> tuple[ModuleMap, Optional[tuple[str, int]], Optional[bool]]:
    """Returns (module_map, (probe_name, probe_addr) or None, self_reported_contained or None)."""
    modules = ModuleMap()
    probe: Optional[tuple[str, int]] = None
    self_reported: Optional[bool] = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "[dxmt9-pe-module-map]" not in line:
            continue
        probe_match = PROBE_LINE_RE.search(line)
        if probe_match and "probe=" in line:
            probe = (probe_match.group("probe"), int(probe_match.group("addr"), 16))
            contained_text = probe_match.group("contained")
            if contained_text is not None:
                self_reported = contained_text == "1"
            continue
        module_match = MODULE_LINE_RE.search(line)
        if module_match:
            modules.add(
                module_match.group("module"),
                int(module_match.group("base"), 16),
                int(module_match.group("size"), 16),
            )
    modules.finalize()
    return modules, probe, self_reported


# --- xctrace time-profile XML parsing -------------------------------------
# Mirrors summarize_xctrace_cpu_threads.py's id/ref resolution: build an
# id -> element map over the whole document once, then every row element
# either carries its own attributes (id="...") or points at a previously
# interned element of the same tag (ref="...").


def load_xml(path: Path) -> tuple[ET.Element, dict[str, ET.Element]]:
    root = ET.parse(path).getroot()
    ids: dict[str, ET.Element] = {}
    for elem in root.iter():
        elem_id = elem.get("id")
        if elem_id:
            ids[elem_id] = elem
    return root, ids


def resolve(elem: ET.Element, ids: dict[str, ET.Element]) -> ET.Element:
    ref = elem.get("ref")
    if ref:
        return ids.get(ref, elem)
    return elem


def value(elem: ET.Element | None, ids: dict[str, ET.Element]) -> str:
    if elem is None:
        return ""
    resolved = resolve(elem, ids)
    return resolved.get("fmt") or (resolved.text or "")


def child(row: ET.Element, tag: str) -> ET.Element | None:
    for elem in row:
        if elem.tag == tag:
            return elem
    return None


def parse_duration_ms(fmt_text: str, fallback_ns_text: str) -> float:
    text = (fmt_text or "").strip().replace(",", "")
    if text:
        parts = text.split()
        try:
            amount = float(parts[0])
        except (ValueError, IndexError):
            amount = 0.0
        unit = parts[1] if len(parts) > 1 else "ms"
        if unit.startswith("ns"):
            return amount / 1_000_000.0
        if unit.startswith("us") or unit.startswith("µs"):
            return amount / 1_000.0
        if unit.startswith("ms"):
            return amount
        if unit.startswith("s"):
            return amount * 1_000.0
        if amount:
            return amount
    try:
        return float((fallback_ns_text or "").strip() or 0.0) / 1_000_000.0
    except ValueError:
        return 0.0


def top_frame(row: ET.Element, ids: dict[str, ET.Element]) -> Optional[tuple[str, str]]:
    """Returns (name, binary) for the first frame of the row's tagged
    backtrace, or None if the row has no backtrace at all."""
    bt = child(row, "tagged-backtrace")
    if bt is None:
        return None
    resolved_bt = resolve(bt, ids)
    for frame in resolved_bt.iter("frame"):
        actual = resolve(frame, ids)
        name = actual.get("name") or ""
        binary = ""
        binary_elem = child(actual, "binary")
        if binary_elem is not None:
            binary_actual = resolve(binary_elem, ids)
            binary = binary_actual.get("name") or binary_actual.get("path") or ""
        return name, binary
    return None


def frame_address(name: str) -> Optional[int]:
    """A raw unsymbolicated xctrace frame shows its address as the `name`
    attribute, formatted as a bare hex string (with or without "0x"). Return
    None if `name` does not look like an address (i.e. it is an already
    dyld-symbolicated function name)."""
    text = name.strip()
    if not text:
        return None
    if not HEX_ADDR_RE.match(text):
        return None
    try:
        return int(text, 16)
    except ValueError:
        return None


class ThreadKey:
    __slots__ = ("fmt",)

    def __init__(self, fmt: str) -> None:
        self.fmt = fmt or "<unknown-thread>"

    def tid(self) -> Optional[int]:
        m = TID_IN_THREAD_FMT_RE.search(self.fmt)
        if not m:
            return None
        return int(m.group(1), 16)

    def __eq__(self, other: object) -> bool:
        return isinstance(other, ThreadKey) and self.fmt == other.fmt

    def __hash__(self) -> int:
        return hash(self.fmt)


def select_thread(
    path: Path,
    ids: dict[str, ET.Element],
    root: ET.Element,
    *,
    process_re: re.Pattern[str],
    explicit_tid: Optional[int],
) -> Optional[str]:
    """Auto-select the top-CPU (highest summed weight) thread of the matched
    process, or the thread whose embedded tid matches --thread-tid."""
    weight_by_thread: dict[str, float] = {}
    for row in root.iter("row"):
        process = value(child(row, "process"), ids)
        thread = value(child(row, "thread"), ids)
        if not (process_re.search(process) or process_re.search(thread)):
            continue
        weight_elem = child(row, "weight")
        weight_actual = resolve(weight_elem, ids) if weight_elem is not None else None
        weight_ms = parse_duration_ms(
            weight_actual.get("fmt") if weight_actual is not None else "",
            weight_actual.text if weight_actual is not None else "",
        )
        weight_by_thread[thread] = weight_by_thread.get(thread, 0.0) + weight_ms

    if explicit_tid is not None:
        for thread in weight_by_thread:
            if ThreadKey(thread).tid() == explicit_tid:
                return thread
        return None

    if not weight_by_thread:
        return None
    # Deterministic tie-break: highest weight first, then lexical thread fmt.
    return max(weight_by_thread.items(), key=lambda kv: (kv[1], kv[0]))[0]


def summarize(
    time_profile: Path,
    modules: ModuleMap,
    *,
    process_re: re.Pattern[str],
    explicit_tid: Optional[int],
) -> tuple[str, dict[str, dict[str, float]], int, int]:
    root, ids = load_xml(time_profile)
    selected_thread = select_thread(
        time_profile, ids, root, process_re=process_re, explicit_tid=explicit_tid
    )

    buckets: dict[str, dict[str, float]] = {}
    total_rows = 0
    no_backtrace_rows = 0

    if selected_thread is None:
        return "", buckets, total_rows, no_backtrace_rows

    for row in root.iter("row"):
        process = value(child(row, "process"), ids)
        thread = value(child(row, "thread"), ids)
        if not (process_re.search(process) or process_re.search(thread)):
            continue
        if thread != selected_thread:
            continue
        total_rows += 1

        weight_elem = child(row, "weight")
        weight_actual = resolve(weight_elem, ids) if weight_elem is not None else None
        weight_ms = parse_duration_ms(
            weight_actual.get("fmt") if weight_actual is not None else "",
            weight_actual.text if weight_actual is not None else "",
        )

        frame = top_frame(row, ids)
        if frame is None:
            no_backtrace_rows += 1
            bucket_name = "no_backtrace"
        else:
            name, binary = frame
            addr = frame_address(name)
            if addr is None:
                # Already dyld-symbolicated: name is a real symbol, not a
                # bare address. Bucket by the owning image.
                bucket_name = f"host:{binary or '<unknown-image>'}"
            else:
                bucket_name = modules.classify(addr)

        bucket = buckets.setdefault(bucket_name, {"samples": 0, "weight_ms": 0.0})
        bucket["samples"] += 1
        bucket["weight_ms"] += weight_ms

    return selected_thread, buckets, total_rows, no_backtrace_rows


def rows_for_output(buckets: dict[str, dict[str, float]], total_rows: int) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for module, stats in buckets.items():
        samples = int(stats["samples"])
        weight_ms = float(stats["weight_ms"])
        share = (samples / total_rows) if total_rows else 0.0
        rows.append(
            {
                "module": module,
                "samples": samples,
                "weight_ms": round(weight_ms, 3),
                "share_of_thread": round(share, 6),
            }
        )
    # Deterministic ordering: highest weight first, then module name.
    rows.sort(key=lambda r: (-float(r["weight_ms"]), str(r["module"])))
    return rows


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(
            fh, fieldnames=["module", "samples", "weight_ms", "share_of_thread"]
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def write_md(
    path: Path,
    *,
    selected_thread: str,
    total_rows: int,
    no_backtrace_rows: int,
    probe_name: str,
    probe_addr: Optional[int],
    probe_verdict: str,
    module_count: int,
    rows: list[dict[str, object]],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines: list[str] = []
    lines.append("# PE module-map symbolication (Tier 1)")
    lines.append("")
    lines.append(f"- Selected thread: `{selected_thread}`")
    lines.append(f"- Total samples on thread: {total_rows}")
    no_bt_share = (no_backtrace_rows / total_rows) if total_rows else 0.0
    lines.append(
        f"- Samples with no backtrace: {no_backtrace_rows} ({no_bt_share:.1%})"
    )
    lines.append(f"- Modules in map: {module_count}")
    lines.append(
        f"- Probe `{probe_name}` addr=0x{probe_addr:x} containment: **{probe_verdict}**"
        if probe_addr is not None
        else "- Probe: not found in module-map log"
    )
    lines.append("")
    lines.append("| module | samples | weight_ms | share_of_thread |")
    lines.append("|---|---|---|---|")
    for row in rows:
        lines.append(
            f"| {row['module']} | {row['samples']} | {row['weight_ms']} | "
            f"{float(row['share_of_thread']):.1%} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--time-profile", required=True, type=Path)
    parser.add_argument("--module-map-log", required=True, type=Path)
    parser.add_argument(
        "--process-regex",
        default=r"\.exe\b",
        help="Regex matched against the xctrace process/thread fmt text "
        "(default matches a typical Wine game process by its .exe name).",
    )
    parser.add_argument(
        "--thread-tid",
        default=None,
        help="Explicit tid (decimal or 0x-prefixed hex) to select instead of "
        "auto-selecting the top-CPU thread of the matched process.",
    )
    parser.add_argument("--output-csv", required=True, type=Path)
    parser.add_argument("--output-md", required=True, type=Path)
    parser.add_argument(
        "--allow-probe-failure",
        action="store_true",
        help="Do not fail (exit nonzero) when the probe address is not "
        "contained in its own module's logged range.",
    )
    args = parser.parse_args(argv)

    process_re = re.compile(args.process_regex)

    explicit_tid: Optional[int] = None
    if args.thread_tid is not None:
        text = str(args.thread_tid).strip()
        explicit_tid = int(text, 16) if text.lower().startswith("0x") else int(text)

    modules, probe, self_reported = parse_module_map_log(args.module_map_log)

    selected_thread, buckets, total_rows, no_backtrace_rows = summarize(
        args.time_profile,
        modules,
        process_re=process_re,
        explicit_tid=explicit_tid,
    )

    if not selected_thread:
        print(
            f"error: no thread matched --process-regex {args.process_regex!r} "
            "in the time-profile export",
            file=sys.stderr,
        )
        return 1

    rows = rows_for_output(buckets, total_rows)
    write_csv(args.output_csv, rows)

    probe_name = probe[0] if probe else ""
    probe_addr = probe[1] if probe else None
    probe_ok = False
    if probe is not None and probe_addr is not None:
        # The probe is a marker function inside our own PE d3d9.dll, so
        # containment means the address falls inside a module whose name
        # matches "d3d9" -- not a match against the probe symbol's own name.
        probe_ok = modules.contains_in_module_matching("d3d9", probe_addr)
        # Cross-check against the PE side's own self-reported verdict when it
        # logged one -- disagreement means either this tool's re-derivation
        # or the PE-side DXMT_ASSERT-guarded check disagree with the module
        # ranges, which is itself worth surfacing.
        if self_reported is not None and self_reported != probe_ok:
            print(
                "warning: PE-reported probe containment "
                f"({self_reported}) disagrees with recomputed containment "
                f"({probe_ok})",
                file=sys.stderr,
            )
    probe_verdict = "PASS" if probe_ok else "FAIL"

    write_md(
        args.output_md,
        selected_thread=selected_thread,
        total_rows=total_rows,
        no_backtrace_rows=no_backtrace_rows,
        probe_name=probe_name,
        probe_addr=probe_addr,
        probe_verdict=probe_verdict,
        module_count=len(modules),
        rows=rows,
    )

    print(f"selected_thread={selected_thread}")
    print(f"total_samples={total_rows} no_backtrace={no_backtrace_rows}")
    print(f"modules={len(modules)} probe_verdict={probe_verdict}")

    if probe is None:
        print("error: no probe= line found in --module-map-log", file=sys.stderr)
        return 1 if not args.allow_probe_failure else 0

    if not probe_ok and not args.allow_probe_failure:
        print(
            "error: probe address containment FAILED (pass --allow-probe-failure "
            "to continue anyway)",
            file=sys.stderr,
        )
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
