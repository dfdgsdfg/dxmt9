---
domain: present-pacing
workload: 3DMark05 GT2
title: "PE prepared-witness duplicate reset"
type: evidence
status: partial
updated: 2026-09-05
source: src/d3d9/d3d9_pe_semantic_owner.hpp; tests/native/bridge/pe_semantic_owner_spec.cpp; experiments/output/app-d3d9-3dmark05-gt2-perf-prepared-reset-base-off-20260905; experiments/output/app-d3d9-3dmark05-gt2-perf-prepared-reset-base-on-20260905; experiments/output/app-d3d9-3dmark05-gt2-perf-prepared-reset-candidate-off-20260905; experiments/output/app-d3d9-3dmark05-gt2-perf-prepared-reset-candidate-on-20260905
related: docs/perfomance/present-pacing/present-pacing-pe-identity-frontier.240.md; specs/d3d9/recorder/gap.md
---

# PE prepared-witness duplicate reset

The next bounded change after `e1e86794` removes duplicate prepared-witness
initialization, not the semantic owner or its staging. Release i386 code confirms
the removed work; the sampled admission phase improves. One GT2 run per cell
does not establish a stable FPS gain.

## Mechanism and invariant

Both observed and unobserved append paths value-initialize their call-local
`PeSemanticPreparedRecord`. Previously `prepareAdmission()` then assigned a
second whole zero-valued witness. Release i386 code implements that assignment
with a 1,024-byte `_memset` of a temporary and a 1,028-byte `_memcpy`, in addition
to small metadata stores.

`PeSemanticPreparedRecord::resetForAdmission()` now invalidates the witness,
resets the active wire-handle count, retention deltas, and destination/role
scalars. The planner resets its small plan before computing it; early returns
that bypass the planner explicitly reset that plan. Identity entries outside
the active count are not consumed. Initial value initialization remains, so
this change does not introduce indeterminate array elements or a new implicit
copy/move hazard.

The candidate i386 `prepareAdmission` contains neither memory call. Its local
stack reservation falls from `0x42c` (1,068 bytes) to `0xc` (12 bytes); the
caller-side initial clear remains. These are function-local code-generation
facts, not a claim that the complete admission stack or all zero-fills vanish.
The exact patch and before/after recorder-object disassembly are preserved in
the candidate-ON artifact's `analysis/` directory.

First-seen qualified handle order, capacity classification, retention,
rollback, canonical ExactFixed/segmented bytes, ABI, resource lifetime, and
scheduling are unchanged. Planner-time malformed input can leave partial
diagnostic facts, and Capacity can leave a complete plan and active handles;
both already occurred in the baseline and remain non-admissible. Failure does
not promise an empty witness.

`preparedAdmissionReinitializesReusedWitness` compares fresh and reused active
facts field by field, without comparing padding or inactive tails. It covers
smaller/different valid identities, null-view rejection, partial planner failure
after a wire identity was admitted to the local witness, handle-bearing
Capacity, Unavailable, and recovery. Review initially questioned partial
failure metadata; main and reviewer checked the baseline and retained that
behavior, strengthening the tests instead of changing failure semantics.

This is a call-local value transform without changed concurrent transitions.
The distinguishing evidence is native reused-witness/byte-equivalence testing;
no new TLA concurrency model or Metal pixel-equivalence claim is needed.

## GT2 evidence

All four runs use Sikarugir `sikarugir-cx-24.0.7`, 1024x768, the `perf` profile,
info logging, frame sampling, contiguous transport, and default replay/encode
policies. The profile disables index reordering. There is no GPU trace or copy
ledger. Observer OFF sets `DXMT9_PE_STATS_DECIMATION=0` and
`DXMT9_PE_SEMANTIC_OWNER_PHASE_SPLIT=0`; ON uses `127` and `1` respectively.
FPS is the benchmark-owned `.3dr` result.

| Cell / artifact suffix | Presents | .3dr FPS | CB/P | Pass/P |
|---|---:|---:|---:|---:|
| Base OFF: `gt2-perf-prepared-reset-base-off-20260905` | 1,669 | 26.679 | 3.9994 | 15.7723 |
| Base ON: `gt2-perf-prepared-reset-base-on-20260905` | 1,619 | 25.869 | 3.9994 | 15.7696 |
| Candidate OFF: `gt2-perf-prepared-reset-candidate-off-20260905` | 1,716 | 27.435 | 3.9994 | 15.7622 |
| Candidate ON: `gt2-perf-prepared-reset-candidate-on-20260905` | 1,624 | 25.952 | 3.9994 | 15.7660 |

Each suffix has the `experiments/output/app-d3d9-3dmark05-` prefix. All runs
report `pass`, zero command-chunk rejects, GPU command-buffer errors, and
no-pipeline draw skips. The saved candidate-OFF scene was visually inspected;
that is a bounded smoke observation, not an exhaustive pixel oracle.

Both baseline cells use i386 `d3d9.dll` hash `46e9fdead064ffab`; both candidate
cells use `2592b9e8afe54f74`, each 1,764,352 bytes. Unix provider hash
`f7935d35db31eb12` and both bridge hashes are unchanged across all cells.
The ON runs use `--skip-stage` after their corresponding OFF run to retain
exact staged binaries; artifact hashes are still recorded and checked.
This matters because normal staging can invoke Ninja even without `--build`.

| Calibrated ON phase | Base ns/event | Candidate ns/event | Base ms/P | Candidate ms/P |
|---|---:|---:|---:|---:|
| Prepare admission | 440.6 | 400.9 | 1.156 | 1.080 |
| Fixed/direct pins | 114.0 | 108.3 | 0.299 | 0.292 |
| Sparse/variable copy | 396.0 | 401.9 | 1.039 | 1.083 |
| Canonical materialization/metrics | 337.6 | 343.5 | 0.886 | 0.926 |
| PendingDelta settlement | 180.7 | 162.5 | 0.474 | 0.438 |
| Settle/clear | 39,655.2 | 40,155.8 | 0.611 | 0.633 |
| ExactFixed inclusive | 6,384.8 | 6,210.6 | 0.098 | 0.098 |

The retained `analysis/summarize_runs.py` subtracts each run's null clock-pair
calibration from sampled means, then uses phase events and final Presents for
run-normalized estimates. The last baseline/candidate phase snapshots follow
decimated Present counts 1,560/1,620 while final totals are 1,619/1,624; these
are not identical reporting populations. Parent timings include children and
must not be added to them. `analysis/comparison.jsonl` retains exact values.

Prepare improves about 39.7 ns/event (9.0%). OFF FPS moves +2.84%, while ON
moves +0.32%. The same baseline binary's earlier OFF run in the previous leaf
was 27.338 FPS, already 2.5% above this baseline cell. The sequence is not
counterbalanced, so the full OFF delta cannot be assigned to the removed reset.
Keep the mechanism result; do not promote these single runs to an FPS claim
or rewrite README benchmark values from them.

## Verification and next owner

Main verification passed native, x86_64 Unix, and both PE builds, plus six
focused specs: semantic owner, semantic batch, semantic projection, chunk
record value, chunk validation, and PE diagnostics. Orca run
`run_c3adbc11d451` owns implementation, independent review, and test hardening.
All owned worker terminals were released or transferred to the completed
follow-up; `git diff --check` and the performance-source audit pass. The final
patch and native test log are retained beside the measurement analysis.

The larger remaining candidate is typed sparse/variable staging followed by
canonical materialization, including repeated pin/index lookup. This patch
does not fuse those stages or close the post-v0.1.1 producer regression.
ExactFixed remains about 0.1 ms/P here and is not the first target.
