---
domain: present-pacing
workload: STALKER CoP Day
title: "H332: query-scope CB composition qualification"
type: evidence
status: partial-qualification
updated: 2026-09-20
source: experiments/output/query-scope-cb-validation-h332/scope-cb-final.txt; experiments/output/query-scope-cb-validation-h332/scope-cb-tla.txt; experiments/output/query-scope-cb-validation-h332/x86-final.txt; experiments/output/query-scope-cb-validation-h332/scope-cb-session-repeat.txt; experiments/output/query-scope-cb-validation-h332/scope-cb-midcb.txt; experiments/output/query-scope-cb-validation-h332/scope-cb-repeat.txt; experiments/output/query-scope-cb-validation-h332/scope-cap-counterexample.txt; experiments/output/query-scope-cb-validation-h332/scope-cap-fixed.txt; experiments/output/query-scope-cb-validation-h332/x86-cap-final.txt; experiments/output/query-scope-cb-validation-h332/scope-cap-tla.txt; experiments/output/query-scope-cb-validation-h332/scope-cap-isolated.txt; experiments/output/app-d3d9-stkcop-bench-scope-cb-reference-fixed-r666-day/result.json; experiments/output/app-d3d9-stkcop-bench-scope-cb-joined-r667-day/result.json; experiments/output/app-d3d9-stkcop-bench-scope-cb-reference-repeat-r668-day/result.json
related: present-pacing-stalker-scope-fragmentation.331.md
---

# H332: share command buffers, not query render passes

Base `c68254d3`; uncommitted implementation overlay. The
[contract](../../../specs/backend/query-scope-cb-composition.md) is
R-CORE-8.2(b). `DXMT9_QUERY_SCOPE_CB_JOIN=1` allows scoped FIFO source
admission into the existing CPU-ready EncodeSession coordinator. Source-tail
finalization closes encoders/clears but carries the CB and its owned callbacks.
Publication, END prefixes, resource pins, origin-qualified contributions and
ordered completion remain unchanged. Framegraph/DCE/parallel exclusions remain.
Default OFF; no README or production-default promotion.

## Deterministic qualification

- Native exhaustive value checker: 32,768 four-source scope/CB-partition/failure
  traces; shared production admission/closure predicates and result accumulation.
  The failure map is held equal between reference and candidate; this is not a
  claim that the driver fails identical CBs under different grouping.
- arm64 focused set: 8/8, including reference/candidate tile off/force, receipt
  oracle and existing session coordinator tests.
- x86_64: all seven scope/value/GPU tests and install-name audit pass. Existing
  broad session test fails once at its seven-admission-escape assertion under
  concurrent build/TLC load; isolated repeats pass 3/3. This is recorded as an
  unresolved scheduling-sensitive test failure, not erased by retries.
- x86_64 candidate GPU repeats pass 10/10. A separate per-record mid-CB split
  run passes 2/2, retaining origin-buffer contribution semantics.
- GPU fixture checks exact 1,024 visible / zero occluded samples, 4,096 per
  overlapping query, five independent passes, exact green framebuffer, gated
  S_FALSE with untouched bytes, FLUSH with no later source, reissue/release/reset.
  The all-scoped window asserts a nonzero actual session append count. It does
  not merely test the admission predicate while running the isolated path.
- Full existing TLA inventory passes in 264.70s. Session completion/progress
  models cover ordering/release; the new bounded value checker covers scope sums.
  Neither proves Metal bytes or all allocation/driver failures.

## Comparison boundary

H331 has zero session starts/appends. Enabling CPU-ready Tape is therefore an
independent change from admitting scoped sources. A valid comparison separates
Tape OFF, Tape ON with scoped join OFF, and Tape ON with scoped join ON.
Matched repeats use identical five staged images, actual occlusion/async fences/
Pending/publication dedup ON and END coalescing OFF. Query observers, PE sampler
and frame sampling are OFF; frame-time windows come from benchmark-owned
`.result` data. macOS `dasd` is near one CPU core before the first run, so small
FPS differences cannot be treated as interference-free non-regression evidence.

## First wild controls and a prerequisite failure

R664 (`app-d3d9-stkcop-bench-scope-cb-baseline-r664-day`) uses Tape OFF and
join OFF: PASS, 2,346 frames / 131.461s = 17.846 FPS. The captured image shows
the game. Benchmark-owned windows are 5.30 FPS at 90–100s and 8.60 FPS at
120–130s. There are zero session starts, 38,899 CB creations and 73,376 render
pass begins across 2,435 encoded Presents. These whole-run populations include
loading and must not be treated as a scene-only cost budget.

R665 (`app-d3d9-stkcop-bench-scope-cb-session-reference-r665-day`) enables Tape
but keeps scoped join OFF. It fails during loading with `capacity lease
pre-representation charge`; capture shows loading, not a completed scene.
The exact owned process does not exit on SIGTERM and is terminated with
SIGKILL. No benchmark result exists; it is not an FPS sample.

The semantic work cap can admit more sources than the physical lease can hold
when payload retirement is blocked. Pre-representation `charge()` rejection
was incorrectly treated as fatal even though no candidate effect occurred.
A 31-source native fixture with real pending-clear retirement exclusion
reproduces SIGABRT. The fix keeps the uncharged candidate Ready, consumes the
already selected prefix, and posts the existing SessionCap submission boundary.
The same fixture now verifies a 30-source submission, credit release, retry
of the final source and exactly-once FIFO completion. Successor headroom and
the transactional charge predicate are unchanged.

The post-fix binary differs from R664/R665. The matched controls below use the
fixed binary; R664 is not a same-image Tape-OFF comparison.

## Final-image OFF / ON / OFF

R666/R667/R668 have identical five staged image receipts. All three runs pass
with no runner failure or GPU command-buffer error. Captures show plausible
game scenes; these are sampled checks, not continuous visual qualification.
Changed-path exact pixels and visibility results come from the native fixture.

| Run | Scoped join | Frames / benchmark seconds | FPS | Whole-run CBs | Whole-run passes | Encoded Presents |
|---|---|---:|---:|---:|---:|---:|
| R666 | OFF | 1,951 / 131.539 | 14.832 | 42,552 | 65,518 | 2,040 |
| R667 | ON | 1,964 / 132.696 | 14.801 | 22,923 | 65,735 | 2,053 |
| R668 | OFF repeat | 1,997 / 133.659 | 14.941 | 43,134 | 66,894 | 2,086 |

CBs per encoded Present fall from 20.859 / 20.678 in the references to 11.165
in the candidate (about 46%). Passes per Present remain 32.117 / 32.068 versus
32.019: this is the intended CB-only mechanism. Neither these whole-run
ratios nor CB reduction establish the critical-path cost or scene-only savings.
Candidate FPS is 0.21% below R666 and 0.94% below R668: no demonstrated gain
and no statistical non-regression claim from these three samples.

| Reconstructed benchmark window | R666 OFF FPS | R667 ON FPS | R668 OFF FPS |
|---|---:|---:|---:|
| 70–80s | 10.4 | 14.3 | 12.9 |
| 80–90s | 9.6 | 10.5 | 9.3 |
| 90–100s | 5.4 | 5.8 | 4.4 |
| 100–110s | 8.0 | 7.9 | 7.2 |
| 110–120s | 10.4 | 8.0 | 9.1 |
| 120–130s | 9.5 | 7.6 | 10.6 |

The late stall survives composition and varies between identical-policy runs.
Do not promote the candidate or update README FPS from this result. Tape
enablement still needs its own final-image OFF/ON comparison.

Post-cap-fix qualification: native focused 7/7; x86 focused 10 pass / one
unresolved broad-session pressure-test failure, then isolated broad-session
and physical-cap repeats 6/6. Full TLA passes in 198.60s. The failure already
occurred before the cap fix; this does not prove its cause or waive the gap.
Archives are `scope-cap-counterexample.txt`, `scope-cap-fixed.txt`,
`x86-cap-final.txt`, `scope-cap-tla.txt` and `scope-cap-isolated.txt` under
`experiments/output/query-scope-cb-validation-h332/`.

Pre-fragmentation wait attribution remains separate from CB composition.
