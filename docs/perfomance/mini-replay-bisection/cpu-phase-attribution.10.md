---
domain: mini-replay-bisection
workload: 3DMark05 GT2 fixed Render Tape
title: "GT2 CPU phase attribution: Direct saves replay work, 1MiB gain is not stable"
type: experiment
status: diagnostic-attribution
updated: 2026-09-16
source: experiments/output/gt2-cpu-phase-attribution-20260916-r623.noindex/campaign.json; experiments/output/gt2-cpu-phase-attribution-20260916-r623.noindex/campaign-complete.json; experiments/output/gt2-cpu-phase-attribution-20260916-r623.noindex/analysis.json; experiments/output/gt2-cpu-phase-attribution-20260916-r623.noindex/run_campaign.py; experiments/output/gt2-cpu-phase-attribution-20260916-r623.noindex/analyze_campaign.py
related: docs/perfomance/mini-replay-bisection/gt2-qualified-policy-matrix.09.md; docs/perfomance/mini-replay-bisection/attachment-authority.08.md; specs/experiments/harness/replay/fixed-input-experiment.md
---

# GT2 CPU phase attribution

## Verdict

The Direct Slot route's benefit is in the **replay execution envelope**, not
setup/admission: Compatibility costs 18.869ms CPU there versus 16.075ms for
ProductionSlot exact, a 2.794ms (14.81%) diagnostic reduction. All four ordered
blocks show the same direction. Independent ordinary measurement brackets
repeat a smaller whole-iteration benefit of 3.05–3.08%.

The earlier 1MiB/64MiB CPU gain does **not** repeat reliably. Ordinary brackets
show -0.52% and +0.16% versus exact; diagnostic replay differs by only -0.033ms
on average. Keep it an opt-in capacity candidate, not a demonstrated CPU/FPS
improvement or default promotion. No runtime code, policy or oracle changes
were needed for this attribution.

## Measurement contract and evidence

- HEAD `782f2b5d`, with the five measured/qualifying images byte-identical to
  [r622](gt2-qualified-policy-matrix.09.md). Same r405 original, manifest,
  source boundaries and source-plan SHA-256
  `cc6b8312ea6abfc0a571741faca56aec729d8dac57e5585a3589bce29aac34f6`.
- Policies: Compatibility exact, ProductionSlot exact, ProductionSlot
  1MiB/64MiB. x86_64 release/O3 through Rosetta; fresh device/resource state
  per iteration; controlled immediate source completion; no actual encoding,
  GPU submissions, Wine producer or arrival pacing in CPU timing.
- Order: full verification C/E/H; ordinary C/E/H; diagnostic C/E/H, H/E/C,
  C/E/H, H/E/C; ordinary H/E/C; full verification H/E/C. Every timed process
  retains two warm-ups plus sixteen samples. Totals: **12 verification,
  96 ordinary measured, 192 diagnostic measured, 36 warm-up iterations**.
- The independent analyzer reauthenticates original/staged input components,
  policies, provider/validator and actual loaded images, source settlements,
  ordered draw observations, resource/source conservation and exclusive
  phase sums. All twelve fresh verification iterations match qualified r622.
- Existing r622 route/headroom GPU evidence is reauthenticated, not rerun:
  same images/input/policies and exact original source/output RGBA
  `d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`.
- `diagnose` receipts remain `accepted=false`, `diagnostic=true`,
  `execution_valid=true` and fail the ordinary measurement acceptance gate.
  No diagnostic receipt is relabelled as ranking or semantic evidence.
- No concurrent build/test/sampler/trace was launched. Host/thermal snapshots
  are retained, but do not prove fixed CPU residency/frequency or an idle OS.
  No sample or slow process is discarded. A controller-only verify/verify
  binding assertion was corrected after timing had finished; retained samples
  were reauthenticated, not rerun. `controller-resume.md` preserves the detail.

## What each phase actually measures

The existing controller-only `CpuDiagnosticLedger` charges nested phases
exclusively. Process CPU includes all process threads; it is not a replay
thread clock. The outer iteration excludes file staging, initial import and
the first authenticated plan construction; its admission phase repeats binding
checks for each fresh executor.

| Phase | Owner and included work | Important exclusion/qualification |
|---|---|---|
| Setup/admission | Fixture creation, `CpuGeneralTapeExecutor::admit`, `validateSourceOracle`, `admitCaptureSourcePlan`, harness storage setup | Rebuilds/authenticates source plan, checks emission ranges/capacity and reserves receipts; **not production queue admission or GPU waiting** |
| Resource materialization | 294 object definitions and 721 seed mutations, resource allocation/lock/upload and logical receipt work | Fresh-input reconstruction, not a game's steady-state per-frame creation/upload workload |
| Replay | `replayChunk`: import view, quiescence checks, handle resolution, source mapping/settlement, `replayPrevalidatedResolvedCommandChunkBySources`, draw accounting | Includes production planning/Slot admission and construction plus wrapper work; **not an isolated production-function timer** |
| Publication | `publishReady`: publication checks, controlled dequeue/submission, command-summary and source-proof bookkeeping | Source-driver publication also occurs inside replay before settlement; this phase is not all production publish cost |
| Completion | `releaseOne`: controlled acknowledgement, finish, reclaim and identity checks | No real GPU wait; immediate completion does not model asynchronous owner pressure |
| Reset/bookkeeping | Object/core/cache release, pool reclamation, reset checks, fixture teardown and outer bookkeeping | Not replay service work |

Code owners: `tests/native/backend/render_tape_cpu_diagnostic.hpp`,
`CpuDiagnosticLedger`; `tests/native/backend/render_tape_cpu_general_executor.hpp`,
`CpuGeneralTapeExecutor::run` and `admitCaptureSourcePlan`;
`tests/native/backend/cpu_ready_production_routing_spec.cpp`,
`runGeneralCpuTapeExperiment`. Publication/completion scopes pause their enclosing
replay scope, so they are not charged twice.

## Additive CPU decomposition

Milliseconds per complete diagnostic iteration, **arithmetic means across 64
retained samples per policy**. Groups are summed within each iteration first;
the table is not a sum of unrelated medians. Resource setup tails remain in
the means. These numbers describe attribution, not low-observer policy ranking.

| Exclusive cost group | Compatibility | Direct exact | Direct 1MiB |
|---|---:|---:|---:|
| Setup/admission | 19.606 | 19.590 | 19.683 |
| Resource materialization | 27.812 | 27.388 | 27.998 |
| Replay envelope | 18.869 | 16.075 | 16.042 |
| Controlled publication | 7.380 | 7.366 | 7.367 |
| Controlled completion/reclaim | 0.838 | 0.829 | 0.834 |
| Reset/bookkeeping | 3.028 | 3.007 | 3.034 |
| Outer clock residual | 0.001 | 0.001 | 0.001 |
| Total | 77.534 | 74.255 | 74.960 |

Admission alone is 19.270/19.270/19.296ms. Mutation is
15.559/15.442/15.583ms; object setup is 12.253/11.946/12.416ms. Most of the
whole-iteration total is therefore outside replay. Removing setup/admission
from a report must not be described as making production rendering faster.

Direct exact reduces mean replay by 2.679–2.964ms across the four process
blocks; phase-median differences are 2.653–2.890ms. Its balanced admission
difference is only +0.0004ms and controlled publication/completion differ by
less than 0.025ms combined. This supports a replay-local route benefit.

For 1MiB versus exact, block mean replay changes are -0.075, +0.006, -0.078
and +0.015ms. Phase medians are 0.030–0.111ms lower, but that tiny signal does
not establish a repeatable whole-iteration gain. The aggregate diagnostic
total is 0.705ms higher, driven mainly by resource-setup variation; it must
not be promoted into a causal policy regression either.

## Ordinary brackets, observer cost and tails

Each cell is a separate process's median after its two warm-ups. Reverse is
executed H/E/C; comparisons below normalize to the same baseline/candidate.

| Policy | Forward CPU ms | Reverse CPU ms |
|---|---:|---:|
| Compatibility | 72.389 | 72.197 |
| Direct exact | 70.157 | 69.9955 |
| Direct 1MiB | 69.7905 | 70.104 |

Compatibility→exact is -3.08%/-3.05%; exact→1MiB is -0.52%/+0.16%.
Diagnostic process medians are 74.193–74.582ms, 71.381–72.094ms and
71.526–71.806ms respectively. Mean diagnostic process medians exceed their
ordinary brackets by 2.90%, 2.21% and 2.40%. This estimates observed diagnostic
perturbation plus process variation, not a precisely removable timer tax.

Fresh object setup has medians around 9.29–9.35ms but maxima of
37.36–38.06ms. Its per-process mean shifts by several milliseconds and can
reverse a tiny policy comparison. All tails are retained. Attribution identifies
the phase, not the allocator/driver mechanism behind the tail. Diagnostic wall
and process CPU means are close (largest block gap 0.022ms); this controlled
lane does not supply evidence of a GPU wait or a live queue/pacing bottleneck.

## Conserved mechanisms and next gate

Every iteration preserves 1,866 draws, 656 source-summary commands and 185
submitted/completed/reclaimed sources. Direct policies execute 1,856 Direct
and ten compatibility draws, with 182 leases. Exact grows SoA once per
iteration; 1MiB grows zero times and provisions/reuses 3/179 times. Neither
has optional capacity denials, rotations or rejections in this CPU lane.
This proves the headroom mechanism ran, not that its cost benefit is large.
The different asynchronous GPU retained capacity/denial results remain in
[r622](gt2-qualified-policy-matrix.09.md); CPU retained gauges cannot replace them.

The first-level separation is complete. Next, target the remaining ~16ms
replay envelope: distinguish import/resolve/settlement wrapper work from the
production source driver, then partition actual planning/materialization/
resource-closure costs using cold diagnostic scopes or external sampling.
Keep total, setup/admission, resource reconstruction and replay costs separate.
A future pre-admitted or warm-resource repetition lane must bind immutable
input identity, reseed resources, reset state and preserve the full semantic
and GPU oracle; simply skipping admission/reset is not an acceptable speedup.
Asynchronous residency/pacing, broader tapes and repeated wild A/B remain
required before any 1MiB default or FPS claim.
