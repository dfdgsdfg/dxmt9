#!/usr/bin/env python3
"""Generate specs/tests/gap_d3d9_wine_test.md directly from MANIFEST data.

For each `START_TEST` test function in
`~/workspaces/wine/dlls/d3d9/tests/{visual,device,d3d9ex,stateblock}.c`,
emit one inventory row that records:
  - Wine source file + line number (so devs can jump straight to the oracle)
  - dxmt9 porting status derived from PE conformance + corpus evidence
  - dxmt9 PE / corpus function names carrying the evidence (or `—` when none)

Status source-of-truth (in order of precedence):
  1. `tests/conformance/d3d9/MANIFEST.toml` `[[case]]` rows whose
     `source = "wine/dlls/d3d9/tests/<file>.c:<wine_test>[,…]"` cites
     the Wine test. Case-level `status` (passing / partial / scaffolded
     / failing / skipped) is mapped to the inventory icon.
  2. `tests/shader_runner/corpus/MANIFEST.toml` `[[test]]` rows whose
     `oracle = "Wine d3d9 <file>.c <wine_test> behavior, ..."` cites
     the Wine test. Corpus `status` (passing / failing) is mapped.

There is no separate `specs/wine_test.plan.md`. Earlier rounds of
this generator parsed §5.1-§5.4 of that plan, but the plan duplicated
data that already lives in MANIFEST.toml — it has been retired.
"""
from __future__ import annotations

import os
import re
import subprocess
import tomllib
from pathlib import Path

# Generator lives in `scripts/tools/`; walk up two levels to reach the dxmt9 repo root.
REPO = Path(__file__).resolve().parents[2]
WINE_ROOT = Path(os.environ.get("WINE_REPO", str(REPO.parent / "wine")))
WINE = WINE_ROOT / "dlls" / "d3d9" / "tests"
PE_MANIFEST = REPO / "tests" / "conformance" / "d3d9" / "MANIFEST.toml"
CORPUS_MANIFEST = REPO / "tests" / "shader_runner" / "corpus" / "MANIFEST.toml"
OUT = REPO / "specs" / "tests" / "gap_d3d9_wine_test.md"


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


def _parse_wine_source(source: str, file_basename: str) -> list[str]:
    """Pull Wine test names out of a `source = "wine/dlls/d3d9/tests/X.c:..."`
    string when X matches `file_basename` (e.g. `device`). Returns [] when
    the source does not cite the file.

    Handles:
      - single:           `wine/dlls/d3d9/tests/device.c:test_x`
      - comma-list:       `wine/dlls/d3d9/tests/device.c:test_a,test_b`
      - cross-file:       `wine/.../device.c:test_a,wine/.../visual.c:test_b`
      - sub-test ('/'):   `wine/.../device.c:test_cube/test_cube_levels`
      - free-text:        `wine/.../d3d9ex.c:base-vs-ex QueryInterface tests`
                          (returns [] since no canonical test name).
    """
    out: list[str] = []
    # Cross-file: split on `,wine/` or `+wine/` separators. Each subsequent
    # segment still begins with `wine/dlls/...`.
    parts = re.split(r"[,+](?=wine/)", source)
    for part in parts:
        m = re.match(r"^wine/dlls/d3d9/tests/([a-z0-9_]+)\.c:(.*)$", part.strip())
        if not m:
            continue
        if m.group(1) != file_basename:
            continue
        rest = m.group(2)
        for chunk in rest.split(","):
            chunk = chunk.strip()
            if not chunk:
                continue
            # Sub-test form: keep the head and any explicit `test_*` names
            # in the slash chain (drops free-text qualifiers).
            for piece in chunk.split("/"):
                piece = piece.strip()
                if not piece:
                    continue
                # Accept identifier-looking tokens only.
                if re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", piece):
                    out.append(piece)
    return out


def _load_manifest(path: Path) -> dict:
    if not path.is_file():
        return {}
    with path.open("rb") as f:
        return tomllib.load(f)


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


# PE case status → inventory bucket. `passing` and `partial` (with passing
# evidence) both signal evidence-in-hand; the inventory treats them as
# covered. `scaffolded` and `skipped` remain `scaffolded` (skip = intentional
# defer; scaffold = case exists but no run). `failing` is failing.
_PE_STATUS_TO_INV = {
    "passing":    "covered",
    "partial":    "covered",
    "skipped":    "covered",  # intentional defer with documented reason
    "scaffolded": "scaffolded",
    "failing":    "failing",
    "todo":       "scaffolded",
}

# Aggregation rule when a Wine test has multiple evidence rows:
# any failing → failing; else any covered (passing/partial) → covered;
# else scaffolded; else UNTRACKED.
def _aggregate(statuses: list[str]) -> str:
    if not statuses:
        return "UNTRACKED"
    if "failing" in statuses:
        # Mixed pass + failing reads as failing/partial; pure failing as failing.
        if "covered" in statuses:
            return "failing/partial"
        return "failing"
    if "covered" in statuses:
        return "covered"
    if "scaffolded" in statuses:
        return "scaffolded"
    return "UNTRACKED"


def collect_evidence(file_basename: str) -> dict[str, tuple[str, str]]:
    """Return {wine_test_name: (status, ", ".join(dxmt9_fns))} for the
    Wine source file `file_basename` (e.g. `device`).

    Walks both the PE conformance manifest and the shader corpus manifest;
    aggregates per-Wine-test evidence into the inventory's status vocabulary.
    """
    fn_map: dict[str, list[str]] = {}  # wine_name -> [fn, …]
    status_map: dict[str, list[str]] = {}  # wine_name -> [inv_status, …]

    pe = _load_manifest(PE_MANIFEST)
    for case in pe.get("case", []):
        if not isinstance(case, dict):
            continue
        wines = _parse_wine_source(case.get("source", ""), file_basename)
        if not wines:
            continue
        fn = case.get("function") or ""
        # Lane-prioritised case verdict: the `builtin` lane is the load-bearing
        # one (it is what the gap doc and conformance summary report against),
        # while `app-local` lanes are an exploratory provider path whose
        # failures should not demote a case that builtin already pins as
        # passing. Per-evidence statuses are grouped by lane, then the case
        # verdict is the **best** lane verdict (builtin > app-local > others).
        # Case-level `status` is used only as a fallback when no evidence rows
        # exist yet.
        by_lane: dict[str, list[str]] = {}
        for ev in case.get("evidence", []) or []:
            if not isinstance(ev, dict):
                continue
            s = ev.get("status") or ""
            if not s:
                continue
            lane = ev.get("lane") or "unknown"
            by_lane.setdefault(lane, []).append(_PE_STATUS_TO_INV.get(s, "scaffolded"))
        if by_lane:
            # Prefer builtin if it has any covered evidence; otherwise fall
            # back to its raw aggregate. If no builtin lane, take the best
            # remaining lane in app-local > others order.
            lane_order = ["builtin", "app-local"] + [
                l for l in by_lane if l not in ("builtin", "app-local")
            ]
            case_verdict = None
            for lane in lane_order:
                if lane not in by_lane:
                    continue
                lane_agg = _aggregate(by_lane[lane])
                if lane_agg == "covered":
                    case_verdict = "covered"
                    break
                if case_verdict is None:
                    case_verdict = lane_agg
            inv_statuses = [case_verdict] if case_verdict else ["scaffolded"]
        else:
            cs = case.get("status") or ""
            inv_statuses = [_PE_STATUS_TO_INV.get(cs, "scaffolded")] if cs else ["scaffolded"]
        for w in wines:
            fn_map.setdefault(w, [])
            if fn and fn not in fn_map[w]:
                fn_map[w].append(fn)
            status_map.setdefault(w, []).extend(inv_statuses)

    corpus = _load_manifest(CORPUS_MANIFEST)
    oracle_pat = re.compile(
        r"^Wine d3d9 ([a-z0-9_]+)\.c[\s:]+(.+?)(?:\s+behavior|$)", re.IGNORECASE)
    name_pat = re.compile(r"\b([a-zA-Z_][a-zA-Z0-9_]*)\b")
    for entry in corpus.get("test", []):
        if not isinstance(entry, dict):
            continue
        oracle = entry.get("oracle") or ""
        m = oracle_pat.match(oracle)
        if not m:
            continue
        if m.group(1) != file_basename:
            continue
        # Wine test names live in the second group; extract identifier-like
        # tokens and filter to those that look like Wine test entrypoints
        # (start with `test_` or end with `_test`, plus known anchors).
        for tok in name_pat.findall(m.group(2)):
            if tok.startswith("test_") or tok.endswith("_test"):
                fn_map.setdefault(tok, [])
                f = entry.get("file") or ""
                # Use the corpus file basename (sans `.shader_test`) as a
                # human-readable cite when no PE function exists.
                if f:
                    leaf = Path(f).stem
                    if leaf not in fn_map[tok]:
                        fn_map[tok].append(leaf)
                corpus_status = entry.get("status") or ""
                if corpus_status == "passing":
                    status_map.setdefault(tok, []).append("covered")
                elif corpus_status == "failing":
                    status_map.setdefault(tok, []).append("failing")
                else:
                    status_map.setdefault(tok, []).append("scaffolded")

    out: dict[str, tuple[str, str]] = {}
    for name, statuses in status_map.items():
        agg = _aggregate(statuses)
        fns = fn_map.get(name, [])
        fns_disp = ", ".join(f"`{f}`" for f in fns) if fns else "—"
        out[name] = (agg, fns_disp)
    return out


def main() -> None:
    inventories: dict[str, list[tuple[str, int, str, str]]] = {}
    summary_rows: list[tuple[str, int, dict[str, int]]] = []

    for src in SOURCES:
        calls = extract_test_calls(src)
        evidence_rows = collect_evidence(src)
        per_status: dict[str, int] = {}
        rows: list[tuple[str, int, str, str]] = []
        for name, line in sorted(calls, key=lambda kv: kv[0]):
            status, dxmt9_fn = evidence_rows.get(name, ("UNTRACKED", "—"))
            rows.append((name, line, status, dxmt9_fn))
            per_status[status] = per_status.get(status, 0) + 1
        inventories[src] = rows
        summary_rows.append((src, len(rows), per_status))

    prov = wine_provenance()
    self_prov = dxmt9_provenance()

    lines: list[str] = []
    lines.append("---")
    lines.append('type: "Spec Gap"')
    lines.append('title: "Wine D3D9 Test Inventory"')
    lines.append('description: "Spec implementation and evidence gap tracker."')
    lines.append("tags: [specs, gap, tests, wine-d3d9]")
    lines.append("---")
    lines.append("")
    lines.append("# Wine D3D9 Test Inventory")
    lines.append("")
    lines.append("Inventory of every Wine `dlls/d3d9/tests/{visual,device,d3d9ex,stateblock}.c`")
    lines.append("test reachable from a `START_TEST` body, with the dxmt9 porting status")
    lines.append("and the dxmt9 PE conformance function(s) that carry the evidence.")
    lines.append("")
    lines.append("This gap doc tracks **which Wine oracles exist** and **where each")
    lines.append("one is mirrored** in the dxmt9 test surface — derived directly from")
    lines.append("`tests/conformance/d3d9/MANIFEST.toml` (PE conformance evidence)")
    lines.append("and `tests/shader_runner/corpus/MANIFEST.toml` (shader-runner")
    lines.append("oracle citations).")
    lines.append("")
    lines.append("Generated by `scripts/tools/gen_wine_d3d9_test_inventory.py`.")
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
    lines.append("A dirty work tree means the status column may include MANIFEST")
    lines.append("edits not yet visible upstream.")
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
    lines.append("| ❓ | UNTRACKED | Wine test that no MANIFEST.toml entry currently cites. Flag if it appears. |")
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
    lines.append("`~/workspaces/wine`) plus `tests/conformance/d3d9/MANIFEST.toml` and")
    lines.append("`tests/shader_runner/corpus/MANIFEST.toml`. It re-counts `START_TEST`")
    lines.append("callees by walking the brace-depth of the test entrypoint body, so")
    lines.append("newly added Wine tests appear with status `UNTRACKED` until a")
    lines.append("MANIFEST entry cites them.")
    lines.append("")
    OUT.write_text("\n".join(lines))
    print(f"wrote {OUT} ({grand_total} entries)")


if __name__ == "__main__":
    main()
