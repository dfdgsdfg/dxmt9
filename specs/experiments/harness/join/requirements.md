---
type: "Spec Requirements"
title: "Harness Join Requirements — External Tool Joins"
description: "Requirements for the join domain: the manual Xcode/xctrace counter export as a declared contract, and detecting a missing or wrong-run export instead of joining it into misleading rows."
tags: [specs, experiments, harness, join, requirements]
---

# Harness Join Requirements — External Tool Joins

This is a domain document under `specs/experiments/harness/`. It
instantiates the `R-HARN-*` requirement groups in
`specs/experiments/harness/requirements.md` for the `join` domain
named in `specs/experiments/harness/spec.md` §1. The join domain
participates in the `external-join` pipeline stage (parent spec.md
§0) only: it merges an external tool's export — an Xcode "Export
Encoder Counters" CSV, an `xctrace` XML table — with dxmt9's own
attribution across the `offline-replay → external-join` and
`external-join → compare-gate` boundaries (parent spec.md §2).
Requirement IDs in this file use the prefix `R-HARN-JOIN-`.

This domain has a property none of the other six domains has: its
*primary* input for the Xcode route does not arrive from a script at
all. Xcode's "Export Encoder Counters" has no CLI, so a human opens
the `.gputrace`, navigates Summary → Show Performance → Counters,
waits for draw-counter profiling to finish, and clicks Export. Per
parent R-HARN-4.1, every artifact that crosses a domain boundary must
carry, in band, what a consumer needs to interpret it — that
requirement does not stop applying because the producer of one
particular artifact is a human following a documented procedure
instead of a script. §2 below states what that means concretely: the
procedure itself is out of this document's scope (it belongs to
`agents/rules/metal_debugging.rules.md` §2b), but the artifact the
procedure must produce — its path and its column shape — is this
domain's contract, checkable the same way any other domain's input
contract is checkable.

---

## 1. Scope

**R-HARN-JOIN-1.1** This document covers exactly the four scripts the
parent domain map's `join` row names:
`scripts/tools/finalize_3dmark05_perf_probe.sh`,
`scripts/tools/summarize_xcode_encoder_counters.py`,
`scripts/tools/summarize_xctrace_metal_intervals.py`, and
`scripts/tools/summarize_xctrace_cpu_threads.py`. Instantiates
R-HARN-1.1.

**R-HARN-JOIN-1.2** Per parent spec.md §1's reduce-versus-join origin
test ("the distinction is the origin of the input artifact, not the
shape of the output"), a script belongs to this domain only when its
*primary* input is an external tool's export — an Xcode "Export
Encoder Counters" CSV or an `xctrace` XML/TSV table — not a
dxmt9-produced log or JSON dump. `spec.md` §2 verifies this test
against each of the four named scripts rather than assuming it from
the parent's own classification. Instantiates R-HARN-1.1.

**R-HARN-JOIN-1.3** `finalize_3dmark05_perf_probe.sh` also invokes,
inline, scripts that belong to other domains — `gate`-domain
`compare_3dmark05_perf_counters.py` and
`compare_xcode_dxmt_bottlenecks.py`, plus two scripts
(`analyze_indexed_probe_classes.py`, `analyze_shader_dumps.py`) whose
own domain this document does not assert. Per parent spec.md §1 ("the
domain axis is harness families, not stages"), this does not make
those other scripts part of this domain, and a change to this
document must not describe their contracts as owned here. Instantiates
R-HARN-1.1.

---

## 2. The Human Export Step Is a Declared Contract, Not an Out-of-Band Assumption

**R-HARN-JOIN-2.1** The file a human produces by exporting Xcode
encoder counters has a declared expected path
(`<trace-dir>/analysis/frame<N>-counters-xcode.csv`, § derivation in
`spec.md` §3) that this domain's own scripts compute the same way on
every invocation, so a consumer never has to guess where the export
should land. Instantiates parent R-HARN-4.1 applied to a
human-produced artifact instead of a script-produced one.

**R-HARN-JOIN-2.2** The column shape the human-produced export must
carry is declared in this domain's own `spec.md` as a named, counted
list (not left implicit in a regex or a comment), per parent R-HARN-5.1
applied to an external tool's shape rather than an internal one.
`spec.md` §4 states this list and its verified count.

**R-HARN-JOIN-2.3** The GUI procedure that produces the export — Open,
Show Performance, Counters, wait for draw-counter profiling, Export —
is specified by reference to
`agents/rules/metal_debugging.rules.md` §2b and is not restated here;
this document's scope is the artifact the procedure must produce
(R-HARN-JOIN-2.1/2.2), not the click sequence that produces it.
Restating the GUI steps in two places would let them drift out of
sync with no mechanism to detect it.

**R-HARN-JOIN-2.4** The absence of the expected export file is a hard
failure of this domain's harness, not a state it silently proceeds
past: `finalize_3dmark05_perf_probe.sh` checks for the file before
doing any work and, per R-HARN-JOIN-1.3's own naming discipline, its
diagnostic names both the missing path and the specific human action
that produces it (`spec.md` §5 records the literal message).
Instantiates parent R-HARN-2.1/5.2 applied to a human-produced input.

**R-HARN-JOIN-2.5** A caller that only wants to see the derived paths
and commands this domain's harness would run — without checking
whether the human export step has actually happened — has an explicit,
named way to do that (a dry-run mode) that must not be confused with
having verified the export exists. `spec.md` §5 records which check
the dry-run path skips.

---

## 3. A Missing or Wrong-Run Export Must Be Detected, Not Silently Joined

**R-HARN-JOIN-3.1** This domain's harness must not join an export from
the wrong run, the wrong frame, or a malformed/empty export into a
"joined" artifact that reports success — per parent R-HARN-2.1, exiting
zero after producing a misleading joined row is exactly the kind of
silent degradation §2 of the parent spec forbids applied to this
domain's boundary. `spec.md` §6 names the checks
(`--require-xcode-counter-coverage`, `--require-dxmt-join-coverage`,
and the xctrace-route equivalents) that exist to catch this today, and
states plainly whether they run by default.

**R-HARN-JOIN-3.2** A caller who wants R-HARN-JOIN-3.1's protection
must be able to name it as an explicit, discoverable flag rather than
relying on default behavior alone; `spec.md` §6 states, for each named
gate, whether omitting it leaves a wrong-run export undetected. This
document does not assert that the current default invocation enforces
R-HARN-JOIN-3.1 by itself — that claim is checked, not assumed, in
`spec.md`.

**R-HARN-JOIN-3.3** When a coverage check in R-HARN-JOIN-3.1 fails, its
diagnostic names the specific counts involved (rows joined vs. rows
labeled, or matches vs. total) rather than a bare pass/fail, per parent
R-HARN-5.2. `spec.md` §6 cites the literal diagnostic strings.

---

## 4. Join Coverage Is Reported as a Number, Not Only Asserted

**R-HARN-JOIN-4.1** The fraction of top-ranked encoder rows that
successfully joined to dxmt9 attribution must be surfaced to a reader
as a numeric value in the harness's own output — report, CSV column,
or both — regardless of whether a `--require-*-coverage` gate is
active and regardless of whether that gate passes. Instantiates parent
R-HARN-3.1/3.2's validity-is-recorded-not-only-checked principle,
applied to join coverage instead of artifact non-degeneracy.

**R-HARN-JOIN-4.2** This document does not assert that every one of
this domain's four scripts satisfies R-HARN-JOIN-4.1 today; `spec.md`
§7 records, per script, whether the coverage number is unconditionally
surfaced or only computed inside a failing gate's diagnostic and
otherwise discarded. Per the honesty discipline parent R-HARN-1.2
requires of this whole spec family, a gap here is recorded as a gap,
not implied to be closed.

---

## 5. The External Tool's Column Names Are a Declared Engine-Shape Dependency

**R-HARN-JOIN-5.1** Every Xcode or `xctrace` column name this domain's
scripts pattern-match against — including the `RenderPass[seq=...,
enc=...]` encoder-label format used as the join key across both the
Xcode and `xctrace` routes — is an engine/tool-shape dependency this
domain's own `spec.md` declares by name and count, per parent
R-HARN-5.1. This applies even though the shape being pattern-matched
originates from Apple's tooling rather than from dxmt9's own emitted
output, because parent R-HARN-5.1's obligation is stated in terms of
"engine-generated output" a harness pattern-matches against, and an
Xcode/`xctrace` export of dxmt9's own GPU work is exactly that class
of output once it has been transformed by the external tool.

**R-HARN-JOIN-5.2** A failure to find a declared required column, or
to find any row carrying the `RenderPass[seq=...,enc=...]` label, is a
hard failure naming the missing column(s) or the absent label pattern,
not a partial join that silently treats the missing shape as zero
rows. Instantiates parent R-HARN-5.2/2.2. `spec.md` §4 records,
per check, whether this holds today.

**R-HARN-JOIN-5.3** The ordered column list produced by this domain's
own joined-output artifact (the CSV `compare-gate` consumes across the
`external-join → compare-gate` boundary) is defined in exactly one
place in the owning script, and this domain's `spec.md` states the
verified column count derived by importing the script rather than by
hand-counting prose, mirroring
`specs/experiments/harness/reduce/spec.md`'s own column-count
discipline (R-HARN-REDUCE-5.2) applied to this domain's output instead
of `reduce`'s.

---

## 6. This Domain Sets No Contract-Relevant Environment Variable

**R-HARN-JOIN-6.1** No script in this domain sets, exports, or
forwards any `DXMT9_*`/`DXMT_*` environment variable that changes what
a dxmt9-produced measurement means into a subprocess it launches. A
wrapper-local `DXMT_3DMARK05_*` variable that only selects this
domain's own CLI-flag defaults (an output path, a gate threshold) is
not contract-relevant in the parent spec.md §4 sense, because it does
not change what a dxmt9 counter, image, or geometry payload means —
only which of this domain's own paths or thresholds are used.
`spec.md` §8 verifies this distinction against the actual script
source rather than assuming it from variable naming alone. Instantiates
parent spec.md §4 Rule 1, mirrored from R-HARN-REDUCE-6.1.

---

## 7. Diagnostic Paths Carry the Primary Contract

**R-HARN-JOIN-7.1** Every command-line flag accepted by any script in
this domain that alters its emitted output or gating behavior — a
coverage requirement, a threshold, an output-path override, a dry-run
mode — appears in this domain's own `spec.md` mode table, whether the
flag is meant for routine finalization or for a diagnostic/comparison
run. Instantiates parent R-HARN-6.2.

**R-HARN-JOIN-7.2** A flag whose entire purpose is to add or narrow a
coverage or attribution check is bound by parent R-HARN-3.3/6.1 the
same as this domain's primary path: it must actually execute the check
it advertises and exit non-zero with a diagnostic naming the checked
quantity on failure, not merely accept the flag and otherwise proceed
as if it were absent. Instantiates parent R-HARN-6.1, mirrored from
R-HARN-REDUCE-7.2.
