---
domain: present-pacing
workload: 3DMark05 GT2
title: "PE admission identity reuse and active-prefix staging"
type: evidence
status: partial
updated: 2026-09-05
source: src/d3d9/d3d9_pe_semantic_owner.hpp; src/d3d9/d3d9_pe_semantic_recorder_state.hpp; tests/native/bridge/pe_semantic_owner_spec.cpp; tests/native/bridge/pe_semantic_batch_spec.cpp; experiments/output/app-d3d9-3dmark05-gt2-perf-admission-reuse-*-20260905
related: specs/d3d9/recorder/requirements.md; present-pacing-pe-direct-canonical.242.md
---

# PE admission identity reuse and active-prefix staging

The R-CORE-REC-7.12–7.13 representation refinement passes native, TLA, release
build, and GT2 OFF/ON direction checks. Some sampled local costs fall; the
single-run OFF FPS difference is only +0.82%, so a stable FPS gain is not
established. This does not close the post-v0.1.1 PE performance investigation.

## Implemented scope

1. Admission preserves the identity-set ordinal and resolves chunk-qualified
   pin indices once. Wire promotion directly addresses that ordinal rather
   than scanning the completed wire table again. Append consumes the bounded
   `PeSemanticPreparedPinMapping`; sparse binding rows keep the resolved local
   wire ordinal in existing private wire metadata. Canonical binding emission
   adds the destination handle-table base without another identity lookup.
2. Four Draw/UP callsites, three ApplyState callsites, and the shared six-kind
   constant emitter use family-specific `armSemanticRecord` overloads. Reuse
   overwrites active fields and family changes clear previous-family authority.
   Fixed owner record heads are copied only for the active producer family.
3. Prepared witnesses initialize scalar frontiers, not inactive arrays.
   Settlement revokes used prefixes rather than clearing records, pins,
   bindings, wire records/handles, and canonical payload. The twelve pin-index
   arrays still clear, and warm-retainer behavior and rollback remain unchanged.

No wire ABI, logical quota, chunk cadence, transport default, resource lifetime,
capture, or replay/encode policy changes. The mapping is call-local and never
crosses the PE/Unix boundary. It still performs a linear lookup over at most 80
prepared identities per non-null input occurrence; this replaces hash probes,
not all identity comparisons. Ten cold control callsites retain generic input
staging. Index clearing, warm-retainer work, and contiguous ExactFixed gather
remain costs.

## Verification and code generation

Orca run `run_8d3972ed369c` separated owner implementation, input staging,
native owner tests, stronger staging differential tests, and independent
read-only review. Main integrated the output, removed residual draft rescans,
corrected test fixtures, and ran verification. All five workers are released.

- Full builds pass in `build`, `build-x86_64-builtin`,
  `build-win32-x64-builtin`, and `build-win32-x86-builtin`.
- Thirteen focused native specs pass: owner, batch, semantic projection,
  chunk-record values/validation, diagnostics, producer differential,
  full-snapshot equivalence, retainer, recorder fault matrix, transition,
  commit, and Render Tape capture. The x86_64 owner/batch specs and
  `dxmt9-winemetal-install-name-audit` also pass.
- `dxmt9-verify-tla` passes in 150.73 seconds. No queue protocol changed;
  existing recorder models own acceptance/settlement transitions, while native
  differential tests establish canonical bytes and ownership effects.
- Added pins cover stale cold refs/spans, family A/B/A, accepted-source
  mutation, retained-only wire promotion, null binding, kind/generation/pointer
  collisions, pin-capacity duplicate/new distinction, partial sparse acquisition
  rollback, and failed settlement/retry. The independent review's early-return
  prepared-plan reset finding was restored and verified by the existing reused-
  witness test. Other initial failures were invalid test fixtures, not waived
  assertions: incorrect oracle retain counts, out-of-order retry ordinals,
  two vertex-input rows above the schema's one-row bound, and tests requiring
  an active constant header to be zero.

Release PE32 disassembly shows the two append instantiations no longer call
`memset` for the 1,028-byte prepared wire context. Each previously had two
alternative observed/unobserved callsites, not two executions per append.
The Draw staging overload copies its 252-byte sparse descriptor and 56-byte
fixed head instead of the generic 844-byte product; the constant overload has
no `memcpy` call. Generic cold staging still has its 844-byte copy. The append
stack allocation rises from `0x510` to `0xf18` bytes (1,296 to 3,864), without
a new stack-probe call in the inspected release functions. This is a real
stack-space tradeoff, not a zero-space abstraction.

## Matched GT2 direction checks

Baseline is clean `4f479b19`; candidate is its uncommitted implementation diff
retained with the artifacts. All four qualified cells use Sikarugir
`sikarugir-cx-24.0.7`, GT2, 1024x768, `perf`, `DXMT_LOG_LEVEL=info`, and
`DXMT9_PE_SEGMENTED_TRANSPORT=0`. OFF uses stats decimation/phase split `0/0`;
ON uses `127/1`. No copy ledger or GPU trace is enabled. FPS is benchmark-owned
`.3dr` FPS, not process-elapsed FPS. Builds and TLA do not overlap these runs.

| Output suffix | Presents | .3dr FPS | CB/P | Pass/P |
|---|---:|---:|---:|---:|
| `gt2-perf-admission-reuse-base-off-info-20260905` | 1,708 | 27.304 | 3.9994 | 15.7652 |
| `gt2-perf-admission-reuse-base-on-info-20260905` | 1,623 | 25.929 | 3.9994 | 15.7702 |
| `gt2-perf-admission-reuse-candidate-off-20260905` | 1,722 | 27.527 | 3.9994 | 15.7753 |
| `gt2-perf-admission-reuse-candidate-on-20260905` | 1,656 | 26.463 | 3.9994 | 15.7705 |

All suffixes have the `experiments/output/app-d3d9-3dmark05-` prefix.
All cells pass with zero command-chunk rejects, GPU command-buffer errors, and
no-pipeline skips. Main inspected the saved candidate OFF forest scene without
an obvious visual defect; this is one-frame smoke, not an exhaustive pixel
oracle. Small pass/P changes across unequal frame populations do not establish
a changed pass boundary. OFF is +0.82% and ON +2.06%; neither is a repeated
promotion result.
Candidate ON could not locate the named window at capture time and used a
full-screen fallback. Its benchmark result and runtime counters remain usable,
but that fallback is not additional visual correctness evidence.

Both baseline cells load PE32 `3ae1a1a336295310` (1,763,840 bytes), PE64
`6feff32ba15d2f9c` (1,629,696 bytes). Both candidate cells load PE32
`e414289ac684d7c5` (1,756,672 bytes), PE64 `af8fe021a08fa6c6` (1,624,064 bytes).
Unix stays `f7935d35db31eb12` (9,779,896 bytes), bridge PE32 stays
`e31242c3d6c67e9a` (140,800 bytes), and bridge PE64 stays
`cc46aabe66969513` (145,920 bytes).

Two earlier `base-{off,on}-20260905` cells used the default `warn` log level.
The ON observer did not print phase rows. Those artifacts are retained but
excluded from the matched comparison above. The launcher left bounded focus
helpers after process exit; only the exact helper PIDs belonging to these
completed runs were terminated during cleanup. No benchmark process was killed.

## Calibrated phase evidence

| Phase | Base ns/event | Candidate ns/event |
|---|---:|---:|
| Prepare admission | 402.5 | 394.9 |
| Fixed/direct pins | 196.8 | 125.6 |
| Sparse/variable copy | 412.6 | 351.6 |
| Canonical materialization/metrics | 384.9 | 382.6 |
| PendingDelta settlement | 152.3 | 160.0 |
| Settle/clear | 38,698.4 | 28,947.5 |
| ExactFixed inclusive | 5,822.2 | 5,785.8 |

The empty clock-pair means are 192.7/184.8ns, subtracted from each sampled
mean. Sampled append counts are 34,461/34,648. The observed inclusive append
parent falls from 3,712.3 to 3,412.1ns/event, but still includes child observer
overhead; do not add it to child costs or call its difference pure product
CPU savings. Hot input staging is outside that owner parent.

The final phase snapshots are near decimated Present 1,620 in both runs,
whereas full-run Presents are 1,623/1,656. Their append-event populations are
4,376,671/4,400,408, versus full wire-record populations 4,382,727/4,474,172.
Prefer ns/event above; the retained summary's run-normalized ms/P estimates
mix those endpoints and undercount the unreported tail, especially candidate.
These samples support a local mechanism improvement, not complete additive
CPU attribution or an FPS promotion. Canonical construction and admission
dedup remain candidates; ExactFixed alone remains small in this observer.

Evidence scripts, source patch, before/after selected release disassembly,
baseline object files, native/TLA logs, and `comparison.jsonl` are retained in
`experiments/output/app-d3d9-3dmark05-gt2-perf-admission-reuse-base-off-20260905/analysis`.
