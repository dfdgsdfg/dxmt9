#!/usr/bin/env python3
"""Generate specs/gap_wine_d3d9_test.md from Wine source + plan.md.

For each `START_TEST` test function in
`~/workspaces/wine/dlls/d3d9/tests/{visual,device,d3d9ex,stateblock}.c`,
emit one inventory row that records:
  - Wine source file + line number (so devs can jump straight to the oracle)
  - dxmt9 porting status (from `specs/wine_test.plan.md` §5.1-5.4)
  - dxmt9 PE function name(s) carrying the evidence (or `—` when none)
"""
from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

# Generator lives in `scripts/tools/`; walk up two levels to reach the dxmt9 repo root.
REPO = Path(__file__).resolve().parents[2]
WINE_ROOT = Path(os.environ.get("WINE_REPO", str(REPO.parent / "wine")))
WINE = WINE_ROOT / "dlls" / "d3d9" / "tests"
PLAN = REPO / "specs" / "wine_test.plan.md"
OUT = REPO / "specs" / "gap_d3d9_wine_test.md"


def _git_capture(root: Path) -> dict[str, str]:
    """Run a fixed set of `git` queries against `root` and return the
    captured fields. Empty / missing entries are reported as
    `"unknown"` so the rendered doc never has blank cells. A dirty
    work tree is flagged via the `dirty` field."""
    out: dict[str, str] = {
        "hash": "unknown", "short": "unknown",
        "date": "unknown", "subject": "unknown",
        "describe": "unknown", "dirty": "no",
        "root": str(root),
    }
    if not (root / ".git").exists():
        return out

    def _run(args: list[str]) -> str:
        try:
            return subprocess.check_output(
                ["git", "-C", str(root)] + args, text=True,
                stderr=subprocess.DEVNULL).strip()
        except subprocess.CalledProcessError:
            return ""

    out["hash"] = _run(["rev-parse", "HEAD"]) or out["hash"]
    out["short"] = _run(["rev-parse", "--short", "HEAD"]) or out["short"]
    out["date"] = _run(["show", "-s", "--format=%ad", "--date=short", "HEAD"]) or out["date"]
    out["subject"] = _run(["show", "-s", "--format=%s", "HEAD"]) or out["subject"]
    out["describe"] = _run(["describe", "--always", "--tags"]) or out["describe"]
    dirty_status = _run(["status", "--porcelain"])
    out["dirty"] = "yes" if dirty_status else "no"
    return out


def wine_provenance() -> dict[str, str]:
    """Capture the Wine reference commit so the inventory has stable
    provenance: any reader can `git -C $WINE_REPO checkout <hash>` and
    reproduce the line numbers in the table."""
    return _git_capture(WINE_ROOT)


def dxmt9_provenance() -> dict[str, str]:
    """Capture the dxmt9 commit at generation time. Pairs with
    `wine_provenance()` so the inventory is bidirectionally
    reproducible: which dxmt9 plan/manifest state was the source of
    the porting-status column, and against which Wine source those
    statuses were evaluated."""
    return _git_capture(REPO)

SOURCES = ["visual", "device", "d3d9ex", "stateblock"]

PLAN_SECTIONS = {
    "visual": r"^### 5\.1 ",
    "device": r"^### 5\.2 ",
    "d3d9ex": r"^### 5\.3 ",
    "stateblock": r"^### 5\.4 ",
}

HELPERS = {
    "memset", "ok", "skip", "trace", "win_skip", "HeapFree", "HeapAlloc",
    "CloseHandle", "DestroyWindow", "UnregisterClassA", "SetCursorPos",
    "GetCursorPos", "GetForegroundWindow", "GetSystemMetrics", "Sleep",
    "create_window", "wait_for_color", "wait_for_buffer_processed",
    "wait_for_event", "wait_for_processed_messages", "util_init",
    "init_test_window", "init_device", "color_match", "init_test",
    "destroy_test", "destroy_window", "init", "setup_event",
    "cleanup_device", "count_query_data", "wait_query",
}


def extract_test_calls(src: str) -> list[tuple[str, int]]:
    """Return [(test_name, line_number), …] for START_TEST(src) body."""
    path = WINE / f"{src}.c"
    text = path.read_text()

    m = re.search(r"START_TEST\([^)]+\)\s*\{", text)
    if not m:
        return []
    # Walk braces to find the block end.
    pos = m.end() - 1
    depth = 0
    block_start = pos + 1
    block_end = pos
    while pos < len(text):
        c = text[pos]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                block_end = pos
                break
        pos += 1

    block = text[block_start:block_end]
    base_line = text[:block_start].count("\n") + 1
    calls: list[tuple[str, int]] = []
    seen: set[str] = set()
    for i, line in enumerate(block.splitlines()):
        match = re.match(r"^\s*([a-z_][a-z_0-9]*)\s*\(\s*\)\s*;", line)
        if not match:
            continue
        name = match.group(1)
        if name in HELPERS or name in seen:
            continue
        seen.add(name)
        # Look up the test function's definition line, not the call site.
        def_match = re.search(rf"^static void {re.escape(name)}\s*\(void\)",
                              text, re.MULTILINE)
        if def_match:
            def_line = text[:def_match.start()].count("\n") + 1
        else:
            def_line = base_line + i
        calls.append((name, def_line))
    return calls


def parse_plan_rows(section_re: str) -> dict[str, tuple[str, str]]:
    """Return {wine_entry: (status, dxmt9_fn)} for the matching plan section."""
    text = PLAN.read_text()
    lines = text.splitlines()
    rows: dict[str, tuple[str, str]] = {}
    in_sec = False
    section_pat = re.compile(section_re)
    next_pat = re.compile(r"^### ")
    for line in lines:
        if section_pat.search(line):
            in_sec = True
            continue
        if in_sec and next_pat.match(line) and not section_pat.search(line):
            break
        if not in_sec or not line.startswith("| `"):
            continue
        # | `name` | Route | Status | dxmt9 PE function(s) | Item DAG |
        cols = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cols) < 5:
            continue
        name = cols[0].strip("`")
        status = cols[2]
        dxmt9_fn = cols[3]
        if name and name != "Wine entry":
            rows[name] = (status, dxmt9_fn)
    return rows


STATUS_ICON = {
    "covered": "✅",
    "scaffolded": "📐",
    "partial": "🟡",
    "covered/partial": "🟢",
    "failing": "🔴",
    "failing/partial": "🟠",
    "partial/deferred": "⏸️",
    "deferred": "⏸️",
}


def status_icon(status: str) -> str:
    return STATUS_ICON.get(status, "❓")


def main() -> None:
    inventories: dict[str, list[tuple[str, int, str, str]]] = {}
    summary_rows: list[tuple[str, int, dict[str, int]]] = []

    for src in SOURCES:
        calls = extract_test_calls(src)
        plan_rows = parse_plan_rows(PLAN_SECTIONS[src])
        per_status: dict[str, int] = {}
        rows: list[tuple[str, int, str, str]] = []
        for name, line in sorted(calls, key=lambda kv: kv[0]):
            status, dxmt9_fn = plan_rows.get(name, ("UNTRACKED", "—"))
            rows.append((name, line, status, dxmt9_fn))
            per_status[status] = per_status.get(status, 0) + 1
        inventories[src] = rows
        summary_rows.append((src, len(rows), per_status))

    prov = wine_provenance()
    self_prov = dxmt9_provenance()

    lines: list[str] = []
    lines.append("# Wine D3D9 Test Inventory")
    lines.append("")
    lines.append("Inventory of every Wine `dlls/d3d9/tests/{visual,device,d3d9ex,stateblock}.c`")
    lines.append("test reachable from a `START_TEST` body, with the dxmt9 porting status")
    lines.append("and the dxmt9 PE conformance function(s) that carry the evidence.")
    lines.append("")
    lines.append("This file is the long-lived companion to the gitignored")
    lines.append("`specs/wine_test.plan.md`. The plan tracks per-round implementation")
    lines.append("staging; this gap doc tracks **which Wine oracles exist** and")
    lines.append("**where each one is mirrored** in the dxmt9 test surface.")
    lines.append("")
    lines.append("Generated from Wine source + plan tables by")
    lines.append("`scripts/tools/gen_wine_d3d9_test_inventory.py` (see commit history).")
    lines.append("")
    lines.append("## Provenance")
    lines.append("")
    lines.append("The inventory is reproducible from two pinned commits: the dxmt9")
    lines.append("commit whose plan / manifest fed the status column, and the Wine")
    lines.append("commit whose source provided the line numbers. Capture both before")
    lines.append("relying on the table for any cross-reference.")
    lines.append("")
    lines.append("### dxmt9 generation revision")
    lines.append("")
    self_dirty = (" — **work tree dirty**" if self_prov["dirty"] == "yes" else "")
    lines.append("Captured from `git -C <repo>` at the moment this file was rendered.")
    lines.append("A dirty work tree means the porting-status column may include")
    lines.append("changes not yet visible upstream.")
    lines.append("")
    lines.append("| Field | Value |")
    lines.append("|-------|-------|")
    lines.append(f"| Commit | `{self_prov['hash']}`{self_dirty} |")
    lines.append(f"| Short  | `{self_prov['short']}` |")
    lines.append(f"| Tag / describe | `{self_prov['describe']}` |")
    lines.append(f"| Author date | `{self_prov['date']}` |")
    lines.append(f"| Subject | {self_prov['subject']} |")
    lines.append("")
    lines.append("### Wine reference revision")
    lines.append("")
    lines.append("The `Wine line` column below points into the Wine commit captured at")
    lines.append("generation time. To reproduce the line numbers verbatim, check out")
    lines.append("the same commit in the Wine checkout before opening the source.")
    lines.append("")
    lines.append("| Field | Value |")
    lines.append("|-------|-------|")
    lines.append(f"| Commit | `{prov['hash']}` |")
    lines.append(f"| Short  | `{prov['short']}` |")
    lines.append(f"| Tag / describe | `{prov['describe']}` |")
    lines.append(f"| Author date | `{prov['date']}` |")
    lines.append(f"| Subject | {prov['subject']} |")
    lines.append(f"| Upstream | https://gitlab.winehq.org/wine/wine/-/commit/{prov['hash']} |")
    lines.append("")
    lines.append("```sh")
    lines.append("# Reproduce the inventory verbatim from a clean tree:")
    lines.append(f"git -C \"$DXMT9_REPO\" checkout {self_prov['hash']}")
    lines.append(f"git -C \"$WINE_REPO\"  checkout {prov['hash']}")
    lines.append("python3 scripts/tools/gen_wine_d3d9_test_inventory.py")
    lines.append("```")
    lines.append("")
    lines.append("## Legend")
    lines.append("")
    lines.append("| Icon | Status meaning | Where evidence lives |")
    lines.append("|------|----------------|----------------------|")
    lines.append("| ✅ | `covered` | Full evidence in at least one lane (PE conformance, shader-runner readback, EXP probe, or native unit test). |")
    lines.append("| 📐 | `scaffolded` | PE conformance scaffold registered in `tests/conformance/d3d9/MANIFEST.toml` and executed by `dxmt9-d3d9-conformance.exe`. |")
    lines.append("| 🟢 | `covered/partial` | Covered in one lane; another lane has only partial evidence. |")
    lines.append("| 🟡 | `partial` | Some evidence exists; matrix / breadth coverage still pending. |")
    lines.append("| 🟠 | `failing/partial` | Mixed evidence; some sub-cases pass, others record failing readbacks. |")
    lines.append("| 🔴 | `failing` | Test exists in the dxmt9 surface but records a deliberate failing readback. |")
    lines.append("| ⏸️ | `deferred` / `partial/deferred` | Acknowledged gap; not yet fixable from the dxmt9 PE side alone. |")
    lines.append("| ❓ | UNTRACKED | Wine test discovered after the last plan refresh. None today — flag if it appears. |")
    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.append("| Wine source | Tests | covered | scaffolded | partial | failing | other |")
    lines.append("|-------------|-----:|--------:|----------:|--------:|--------:|------:|")
    grand_total = 0
    grand_buckets = {"covered": 0, "scaffolded": 0, "partial": 0, "failing": 0, "other": 0}
    for src, total, per in summary_rows:
        grand_total += total
        covered = per.get("covered", 0)
        scaffolded = per.get("scaffolded", 0)
        partial = per.get("partial", 0) + per.get("covered/partial", 0)
        failing = per.get("failing", 0) + per.get("failing/partial", 0)
        other = total - (covered + scaffolded + partial + failing)
        grand_buckets["covered"] += covered
        grand_buckets["scaffolded"] += scaffolded
        grand_buckets["partial"] += partial
        grand_buckets["failing"] += failing
        grand_buckets["other"] += other
        lines.append(f"| `{src}.c` | {total} | {covered} | {scaffolded} | {partial} | {failing} | {other} |")
    lines.append(f"| **TOTAL** | **{grand_total}** | "
                 f"**{grand_buckets['covered']}** | "
                 f"**{grand_buckets['scaffolded']}** | "
                 f"**{grand_buckets['partial']}** | "
                 f"**{grand_buckets['failing']}** | "
                 f"**{grand_buckets['other']}** |")
    lines.append("")
    lines.append("`other` rolls up `deferred`, `partial/deferred`, and any UNTRACKED rows.")
    lines.append("")

    section_titles = {
        "visual": "Visual / Rendering Tests (`visual.c`)",
        "device": "Device / Resource Tests (`device.c`)",
        "d3d9ex": "D3D9Ex-Specific Tests (`d3d9ex.c`)",
        "stateblock": "StateBlock Tests (`stateblock.c`)",
    }
    for src in SOURCES:
        rows = inventories[src]
        lines.append(f"## {section_titles[src]}")
        lines.append("")
        lines.append(f"Source: [`dlls/d3d9/tests/{src}.c`]"
                     f"(https://gitlab.winehq.org/wine/wine/-/blob/master/dlls/d3d9/tests/{src}.c)")
        lines.append("")
        lines.append("| Wine entry | Wine line | Status | dxmt9 PE function(s) / evidence |")
        lines.append("|------------|----------:|:------:|---------------------------------|")
        for name, line, status, dxmt9_fn in rows:
            icon = status_icon(status)
            # Strip backticks already present; the column wraps in backticks itself.
            dxmt9_disp = dxmt9_fn if dxmt9_fn else "—"
            lines.append(f"| `{name}` | {line} | {icon} | {dxmt9_disp} |")
        lines.append("")

    lines.append("## Maintenance")
    lines.append("")
    lines.append("Regenerate this file with:")
    lines.append("")
    lines.append("```sh")
    lines.append("python3 scripts/tools/gen_wine_d3d9_test_inventory.py")
    lines.append("```")
    lines.append("")
    lines.append("The generator reads Wine source from `$WINE_REPO` (default")
    lines.append("`~/workspaces/wine`) and the plan from `specs/wine_test.plan.md`.")
    lines.append("It re-counts `START_TEST` callees by walking the brace-depth of the")
    lines.append("test entrypoint body, so newly added Wine tests appear with status")
    lines.append("`UNTRACKED` until the plan picks them up.")
    lines.append("")
    lines.append("Cross-reference: per-round implementation staging lives in")
    lines.append("`specs/wine_test.plan.md` (gitignored) and")
    lines.append("`specs/wine_test_failures.plan.md`; once a Wine row reaches a stable")
    lines.append("`covered` / `scaffolded` state, update both this file and the plan.")
    lines.append("")
    OUT.write_text("\n".join(lines))
    print(f"wrote {OUT} ({grand_total} entries)")


if __name__ == "__main__":
    main()
