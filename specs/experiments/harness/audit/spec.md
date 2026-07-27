---
type: "Spec"
title: "Harness Audit Spec — Record Verification"
description: "Script inventory, Meson registration, and the audit_perf_docs_sources.py checks-versus-does-not-check table, verified against source and live reproduction."
tags: [specs, experiments, harness, audit, spec]
---

# Harness Audit Spec — Record Verification

Implements `specs/experiments/harness/audit/requirements.md`
(`R-HARN-AUDIT-*`). Instantiates the `audit` row of the domain map in
`specs/experiments/harness/spec.md` §1 and the terminal
`compare-gate → record` boundary in that spec's §2. Stage names,
boundary names, and envelope fields are cited from the parent spec
rather than redefined here.

Facts below were verified against the working tree at commit
`b447b79c` on 2026-07-27, using `wc -l`, direct `Read`/`grep -n`, and
by actually invoking three scripts in this domain against the live
repository state (§2, §3) rather than assuming their behavior from
source alone — following the same discipline
`specs/experiments/harness/gate/spec.md` §4 already applied to its own
domain's scripts. Filesystem mtimes, not `git log`, are used for any
path under `traces/`, since that tree is gitignored.

---

## 1. Script Inventory

| Script | Lines (`wc -l`) | Role |
|---|---:|---|
| `assert_perf_counters.py` | 417 | Runs a given native test executable with `DXMT_PERF_COUNTERS=1` set, greps its stderr for the single `[dxmt9-perf] ` line, and asserts an exact fixed set of counter key/value pairs plus the presence (not the value) of ~190 `encodeDraw()` sub-phase timer keys. Not a citation auditor — a native-test output assertion wrapper. |
| `audit_perf_counter_callsites.py` | 158 | Text-based audit: every `count*()` declared in `dxmt9_perf_counters.hpp` must have >=1 call site under `src/` outside the perf-counters translation unit itself. |
| `audit_perf_counter_table.py` | 98 | Text-based audit: every field declared in the `Counters` struct (`dxmt9_perf_counters.cpp`) must be referenced by at least one `kCounterTable` row (`&Counters::NAME`). |
| `audit_perf_docs_sources.py` | 107 | Audits newly added `docs/perfomance` leaf files for a frontmatter `source:` field that does not cite the retired `specs/perfomance.plan.md`. §2 gives the full checks-versus-does-not-check table. |
| `audit_winemetal_install_names.py` | 123 | Walks every `winemetal.so` under top-level `build*` directories and asserts `otool -L` shows no bare `winemac.so`/`ntdll.so` dependency (each must be `@rpath/`-prefixed or a system path). |
| `check_d3d9_conformance_manifest.sh` | 203 | Validates `tests/conformance/d3d9/MANIFEST.toml`: required fields, enum values, evidence lane/arch/status consistency against declared `lanes`/`arches`, requirement-ID pattern, license fields, and — the referent-existence case cited in requirements.md R-HARN-AUDIT-4.4 — that each evidence `source` (`path:line`) resolves to an existing file with the line in range, and that `source_file` (when set) exists and contains the named `function` text. |
| `check_d3d9_conformance_status.py` | 315 | Reads the same manifest and renders a text/Markdown/Mermaid status report (counts by status, next actions, per-status buckets); optionally exits non-zero via `--fail-if-full-support-missing`. |
| `check_drift.sh` | 6 | Thin forwarder: `python3 scripts/tools/shader_corpus_tool.py drift "$@"`. |
| `check_manifest.sh` | 138 | Validates the shader corpus `MANIFEST.toml`: the referent-existence case cited in requirements.md R-HARN-AUDIT-4.4 is its `comm -3` diff between the actual `*.shader_test` files under `tests/shader_runner/corpus/` and the `file = "..."` entries the manifest declares (lines 14-29), plus a TOML-level field/enum check and a per-file provenance-comment check. |
| `verify_tla.sh` | 40 | Runs the TLA+ model checker (`tlc`, or a downloaded `tla2tools.jar`) over every `.tla`/`.cfg` pair under `specs/verification/tla/`. |

`417 + 158 + 98 + 107 + 123 + 203 + 315 + 6 + 138 + 40 = 1,605` total
lines, matching the sum `wc -l` itself reports for the ten paths in
one invocation on 2026-07-27. `scripts/check/README.md` (27 lines) is
documentation and is excluded from this count, per R-HARN-AUDIT-1.1.

---

## 2. `audit_perf_docs_sources.py` — Checks Versus Does Not Check

Verified directly against `audit_paths()` (`:59-78`) and the
`git_new_perf_docs()`/`is_leaf()` selection logic (`:25-56`) that feed
it:

| Checks | Does not check |
|---|---|
| A frontmatter `source:` line is present (`SOURCE_RE.search`, `:22,67-70`) — failure: `"missing frontmatter source"`. | Whether any path named inside the `source:` value **exists on disk**. No filesystem `exists()`/`is_file()` call appears anywhere in `audit_paths()`. |
| The `source:` line's text does not contain the literal substring `specs/perfomance.plan.md` (`:21,71-77`) — failure names the retired path and suggests alternatives. | Whether a cited path's content matches what was recorded at citation time (no digest field exists to check against — parent spec.md §3's "second use of `inputs` digests" describes a mechanism that does not exist yet anywhere in this pipeline, this domain included). |
| — | Whether the `source:` value is even shaped as one or more paths at all — a `source:` line containing arbitrary prose with no `specs/perfomance.plan.md` substring passes with no shape check. |
| — | Any `docs/perfomance/*.md` file that is **not** git-new (an already-committed leaf edited in place) unless the caller passes explicit `--path` (§2's default-scope note below). |
| — | Any `docs/perfomance/*.md` file directly under the top-level directory (`is_leaf()`, `:25-30`, requires `len(rel.parts) > 1`) — `docs/perfomance/overview.md`, `index.md`, `log.md`, and the three `overview-3dmark05-gt{1,2,3}.md`/`overview-sfiv.md` files (7 files total, verified `find docs/perfomance -maxdepth 1 -name '*.md'` on 2026-07-27) can never be selected by this script's default scope, regardless of git status. |

**Default scope, verified.** Absent `--path`, `main()` calls
`git_new_perf_docs()` (`:95`), which runs `git status --porcelain --
docs/perfomance` and keeps only lines whose status is `??` (untracked)
or contains `A` (staged-added) (`:33-56`). A leaf that is already
committed and merely edited — the overwhelmingly common case for a
137-leaf-per-domain-sized corpus that was written months ago — is
invisible to a default invocation no matter what its `source:` field
says. Per R-HARN-AUDIT-1.1's inventory, R-HARN-AUDIT-2.3 requires this
default to be stated plainly rather than left to be discovered by
reading source, which is exactly what this paragraph does.

**Parent spec.md's own description of this boundary, checked against
source.** Parent spec.md §2's `compare-gate → record` section states:
"The `audit`-domain consumer — `scripts/check/audit_perf_docs_sources.py`
— is what makes this boundary checkable: it reads a `source:` citation
and confirms the cited path still resolves to an artifact whose digest
matches what was recorded at citation time, rather than only
confirming the path exists." Checked directly against `audit_paths()`
above, this describes an intended role for the mechanism the parent
spec's own §5 migration would eventually build, not the current
function: `audit_paths()` performs neither half of that sentence — it
does not confirm the path exists, and it does not compare any digest,
because no digest field exists anywhere upstream to compare against
(parent spec.md §5: "No migration step has been performed"). This
document does not treat that sentence as wrong to have written — it
states the target shape a fixed audit should reach — but a reader who
takes it as a description of *current* behavior would be misled, so
this section states the gap plainly rather than repeating the parent
prose as if it were already true.

---

## 3. Live Reproductions

Both reproductions below were run on 2026-07-27 against the actual
working tree at commit `b447b79c`, not fixture files, per
R-HARN-AUDIT-4.2's "live-reproduced instance" requirement.

### 3.1 Zero-file pass on a clean tree

```sh
$ git status --porcelain -- docs/perfomance | grep -cE '^(\?\?|A)'
0
$ python3 scripts/check/audit_perf_docs_sources.py
audit_perf_docs_sources: OK (0 new leaf file(s) checked)
$ echo "exit=$?"
exit=0
```

On the current tree, the registered Meson test
`dxmt9-perf-docs-source-audit` (§5) evaluates zero files and reports
`OK`. This is the sharpest illustration of R-HARN-AUDIT-2.3's default-
scope requirement: the audit passing carries no information about the
737 leaf files that actually exist under `docs/perfomance/` (`find
docs/perfomance -mindepth 2 -name '*.md' | wc -l` → `737` on
2026-07-27), because none of them is git-new right now.

### 3.2 A dangling citation passes when explicitly named

A pre-existing leaf, `docs/perfomance/baselines/baselines-frame60.02.md`
(committed at `913f0eac`, 2026-07-08, 113 lines), carries a `source:`
line whose first path segment is
`experiments/output/app-d3d9-3dmark05-post-visualfix-frame60-baseline-r1/3dmark05-perf-summary.md`
and whose second is
`traces/app-d3d9-3dmark05-post-visualfix-frame60-baseline-r1/analysis/frame60-counters-xcode.csv`.
The second path is gitignored (under `traces/`), so `git log` cannot
date its removal; checked by filesystem access on 2026-07-27, it does
not exist:

```sh
$ ls traces/app-d3d9-3dmark05-post-visualfix-frame60-baseline-r1/analysis/frame60-counters-xcode.csv
ls: ...: No such file or directory
$ python3 scripts/check/audit_perf_docs_sources.py \
    --path docs/perfomance/baselines/baselines-frame60.02.md
audit_perf_docs_sources: OK (1 new leaf file(s) checked)
$ echo "exit=$?"
exit=0
```

This is one concrete, live instance of the class of failure parent
spec.md §2/§3 report at 34-of-56 scale from a separate 2026-07-27 disk
audit; this document does not claim this one file is among that
original 34, only that the same mechanism gap reproduces against the
current tree independently of that earlier count.

### 3.3 The registered conformance-status test skips its own strict gate

```sh
$ python3 scripts/check/check_d3d9_conformance_status.py \
    --fail-if-full-support-missing >/dev/null 2>&1; echo "exit=$?"
exit=1
$ python3 scripts/check/check_d3d9_conformance_status.py \
    >/dev/null 2>&1; echo "exit=$?"
exit=0
```

The manifest currently has at least one non-`passing` case (the flag
exits `1`), yet the Meson-registered invocation
(`dxmt9-d3d9-conformance-status-report`, §5) passes no arguments and
therefore always takes the `exit=0` branch regardless of manifest
status, unless `load_cases()` itself raises (malformed TOML). This is
the verified case behind R-HARN-AUDIT-3.2.

---

## 4. Existence-Checking Already Exists in This Domain

Two scripts in this domain's own inventory already perform the
referent-existence check R-HARN-AUDIT-4.1 requires and
`audit_perf_docs_sources.py` (§2) lacks, for their own citation shapes:

**`check_manifest.sh` (:14-29).** Builds `actual_files` from `find
"$tests_dir" -name '*.shader_test' -type f` and `manifest_files` from
the manifest's own `file = "..."` entries, sorts both, and runs `comm
-3` on them (:27-29). Any filename present in exactly one list —
either a real `.shader_test` file the manifest never declares, or a
manifest entry naming a file that does not exist on disk — surfaces in
`$diff_output` and fails the script (`exit 1`, `"manifest
mismatch:"`). This is a directory-level referent check (the whole
corpus against the whole manifest at once), not a per-citation check,
but its net effect is exactly R-HARN-AUDIT-4.1's requirement: a
manifest entry naming a nonexistent file cannot pass this script.

**`check_d3d9_conformance_manifest.sh` (:130-141, :184-195).** For
each `evidence` entry's `source` field (a `path:line` string matched
by `evidence_source_re`), the script resolves `repo_root /
match.group("path")` and checks `evidence_path.is_file()` (:138,
failure: `"evidence source does not exist"`) and, only if that passes,
`int(match.group("line")) > len(evidence_path.read_text().splitlines())`
(:140-141, failure: `"evidence source line is out of range"`).
Separately, for each case's optional `source_file` field, it checks
`local_path.is_file()` (:190-191, failure: `"source_file does not
exist"`) and, if the file exists, that the case's declared `function`
string literally appears in that file's text (:193-195, failure:
`"function {function!r} not found in {source_file}"`). Both checks
resolve a named path to disk and fail the whole script if it does not
resolve — the pattern R-HARN-AUDIT-4.1 asks `audit_perf_docs_sources.py`
to adopt.

Neither script checks a content digest — both stop at "the referent
exists" (and, for the conformance manifest, "the line is in range" /
"the text is present"), not "the referent's content matches what was
recorded at citation time." Parent spec.md §3's digest mechanism
(R-HARN-AUDIT-4.1's fuller citation) is therefore unimplemented
everywhere in this domain, not only in `audit_perf_docs_sources.py`;
this section records existence-checking as already present precedent,
not digest-checking.

---

## 5. Meson Registration

All ten scripts named in §1 are wired into Meson somewhere in the
`tests/` tree; none is unregistered. Verified via `grep -n
"scripts/check" tests/meson.build` plus `grep -rn
"assert_perf_counters" tests/` on 2026-07-27:

| Script | Registered as | File | Notes |
|---|---|---|---|
| `verify_tla.sh` | `dxmt9-verify-tla` | `tests/meson.build:126` | `is_parallel: false`. |
| `check_manifest.sh` | `dxmt9-manifest-check` | `tests/meson.build:127-128` | `is_parallel: false`. |
| `check_d3d9_conformance_manifest.sh` | `dxmt9-d3d9-conformance-manifest-check` | `tests/meson.build:136-138` | `is_parallel: false`. |
| `check_d3d9_conformance_status.py` | `dxmt9-d3d9-conformance-status-report` | `tests/meson.build:139-141` | `is_parallel: false`; no arguments passed (§3.3). |
| `check_drift.sh` | `dxmt9-drift-report` | `tests/meson.build:142-143` | `is_parallel: false`. |
| `audit_perf_counter_table.py` | `dxmt9-perf-counter-table-audit` | `tests/meson.build:144-146` | `is_parallel: false`. |
| `audit_perf_counter_callsites.py` | `dxmt9-perf-counter-callsite-audit` | `tests/meson.build:147-149` | `is_parallel: false`. |
| `audit_perf_docs_sources.py` | `dxmt9-perf-docs-source-audit` | `tests/meson.build:150-152` | `is_parallel: false`; no arguments passed, so CI always runs it in its default git-new-only scope (§2). |
| `audit_winemetal_install_names.py` | `dxmt9-winemetal-install-name-audit` | `tests/meson.build:158-161` | `is_parallel: false`; `depends: dxmt9_winemetal_unix_install_name_fixup`. |
| `assert_perf_counters.py` | `dxmt9-allocation-counter-spec` (as the test *executable*, not a script argument) | `tests/meson.build:38-41` (`find_program`), `tests/native/backend/meson.build:293-296` (`test(...)`) | Wired via `find_program()` in `tests/meson.build` and invoked as `test('dxmt9-allocation-counter-spec', assert_perf_counters, args: [allocation_counter_spec], env: dxmt9_native_test_env, depends: dxmt9_winemetal_unix_install_name_fixup)` — the one script in this domain not invoked as `test(name, python3/bash, args: [script_path])`. |

`scripts/check/README.md` states "All entries here are wired into
Meson tests under `tests/meson.build`" (`:3-4`); checked against the
table above this is accurate for 9 of the 10 scripts directly, and
accurate for `assert_perf_counters.py` only in the sense that its
`find_program()` declaration lives in `tests/meson.build` — the
`test()` call that actually uses it lives in a file that
`tests/meson.build` pulls in via `subdir('native/backend')`
(`tests/meson.build:55`), not in `tests/meson.build` itself.

The README's own bullet list (`:6-27`) is incomplete relative to the
ten-script inventory in §1, in two distinct ways, counted directly
against the file rather than estimated: it has exactly 8 bullets for
10 scripts, and of those 8, 6 name a Meson test in parentheses.

- **No bullet at all** (2 scripts): `audit_perf_counter_callsites.py`
  and `audit_winemetal_install_names.py` are absent from the README
  entirely — not merely missing a named test, but missing as an entry.
- **Bullet present, no test named** (2 scripts): `assert_perf_counters.py`
  and `audit_perf_docs_sources.py` — the latter being the script this
  document's §2/§3/§4 are largely about — each have a bullet but no
  `(test ...)` parenthetical, unlike the other 6 bulleted scripts.
- **Bullet present, test named** (6 scripts): `check_drift.sh`,
  `check_manifest.sh`, `check_d3d9_conformance_manifest.sh`,
  `check_d3d9_conformance_status.py`, `verify_tla.sh`, and
  `audit_perf_counter_table.py`.

`8 = 2 + 6` and `6 + 2 + 2 = 10` reconcile the count above against the
full inventory. This document's table above is the place that names
all ten scripts' registration state; the README is not currently a
substitute for it.

---

## 6. Mode Table

Per R-HARN-AUDIT-5.1. Most scripts in this domain accept no flags at
all — `audit_perf_counter_table.py`, `audit_perf_counter_callsites.py`,
`audit_winemetal_install_names.py`, `check_manifest.sh`,
`check_d3d9_conformance_manifest.sh`, and `verify_tla.sh` take no
command-line arguments that change behavior (verified: none defines an
`argparse`/manual-flag surface beyond a bare invocation; `verify_tla.sh`
reads the non-`DXMT`-prefixed `TLA2TOOLS_JAR` env var as an optional
local-jar override, out of scope for R-HARN-AUDIT-6.1's `DXMT9_*`/
`DXMT_*` claim).

| Script | Flag | Effect | Exercised by Meson registration? |
|---|---|---|---|
| `audit_perf_docs_sources.py` | `--path PATH` (repeatable) | Audits exactly the given leaf path(s) instead of the git-new-only default (§2); still subject to `is_leaf()`'s "must be under a subdirectory of `docs/perfomance`" filter. | No — `dxmt9-perf-docs-source-audit` passes no arguments (§5). |
| `check_d3d9_conformance_status.py` | `--manifest PATH` | Overrides the manifest path (default `tests/conformance/d3d9/MANIFEST.toml`). | No. |
| `check_d3d9_conformance_status.py` | `--format {text,markdown,mermaid}` | Selects the rendered report shape; does not change pass/fail. | No — default `text`. |
| `check_d3d9_conformance_status.py` | `--fail-if-full-support-missing` | Exits `1` if any case's `status != "passing"` (verified live in §3.3: exits `1` on the current manifest when passed, `0` when omitted). | No — `dxmt9-d3d9-conformance-status-report` passes no arguments (§5), so this domain's CI membership for this script proves only "the manifest parses and renders," never "every case is passing." |
| `check_drift.sh` | `"$@"` (forwarded verbatim) | Passes through to `shader_corpus_tool.py drift`; that script's own flags are out of scope for this document (it lives under `scripts/tools/`, not `scripts/check/`). | N/A — `dxmt9-drift-report` invokes with no extra arguments. |
| `assert_perf_counters.py` | `argv[1:]` (positional) | The executable (and its own arguments) to run under `DXMT_PERF_COUNTERS=1` before asserting its `[dxmt9-perf]` line; not a flag in the usual sense, but the one input this script's behavior is parameterized by. | Yes — `dxmt9-allocation-counter-spec` passes `[allocation_counter_spec]` (§5). |

---

## 7. Environment Variables

Per R-HARN-AUDIT-6.1, verified `grep -n "DXMT\|os.environ\|getenv"
scripts/check/*.py scripts/check/*.sh` on 2026-07-27 across all ten
scripts named in §1: the only match anywhere in this domain is
`scripts/check/assert_perf_counters.py:21-22`:

```python
env = os.environ.copy()
env["DXMT_PERF_COUNTERS"] = "1"
```

This sets `DXMT_PERF_COUNTERS=1` only in the environment of the
subprocess this one script directly launches and immediately asserts
against (a Meson-built native test binary passed as `argv[1]`), never
into this domain's own process environment or into any experiment run
another domain coordinates. Per R-HARN-AUDIT-6.2,
`specs/experiments/harness/reduce/spec.md`'s own environment-variable
table already records `DXMT_PERF_COUNTERS` as `runner`-owned for a
real catalogue/Wine run (`experiments/launchers/common.sh:146-147,163-164`);
that ownership and this domain's narrow, single-subprocess use do not
conflict because the two never share a run. No other script in this
domain sets, reads, or forwards any `DXMT9_*`/`DXMT_*` variable.
