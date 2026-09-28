---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H408: Real visibility changes work but does not close the FPS gap"
type: evidence
status: single-pair-no-large-gain-scope-publication-candidate
updated: 2026-09-29
source: traces/stkcop-occlusion-h408/{run.py,analyze.py,summary.json}; traces/stkcop-occlusion-h408/{off,on,off-counts,on-counts}/{invocation.json,completion.json}; experiments/output/app-d3d9-stkcop-bench-occlusion-h408-{off,on,off-counts,on-counts}/result.json
related: present-pacing-stalker-baseline-recheck.407.md; present-pacing-stalker-query-split-census.402.md; present-pacing-query-scope-cb-composition.332.md
---

# H408: Visibility ON is not a standalone performance fix

## Method

Sequential OFF / ON perf runs, followed by separate OFF / ON diagnostics.
All four use the same five binary receipts and requested/final/control hashes
as H407 and H403. Sikarugir CX24.0.7, Day/high, 1280x720 windowed, two game
threads, 16 sound targets. Only `DXMT9_METAL_OCCLUSION` changes between
policies; covered-Pending remains OFF and no other policy is enabled.

The perf pair has runtime observers/frame counters OFF and no external CPU
or GPU trace. Diagnostic runs add frame/perf counters, split and backend
count observers, and bounded query timeline capture (skip 5,000 ENDs, stride
31). Diagnostic FPS is not used as performance evidence. All four exit 0,
runner PASS, without timeout. No builds, runtime changes or concurrent runs.
Metal skill preflight confirms Xcode 27, Metal compiler and Apple M1 support;
no expensive trace capture is needed for this experiment.

All four screenshots fail to locate the onscreen game window and fall back
to desktop capture. Therefore no new visual or foreground qualification is
claimed. The user accepted H407's 27.405689 FPS as a working baseline despite
this limitation. The present pair is exploratory performance evidence under
that convention, not a promotion gate. It is one order, without reversed
repeats or exact camera matching; small differences are not significant.

## Observer-OFF performance

| Policy | Engine FPS | Reconstructed scene seconds |
|---|---:|---:|
| Primitive-count default | 28.179667 | 119.128396 |
| Real visibility | 28.399157 | 119.052911 |

ON differs by +0.78%, not a demonstrated speedup. Simply enabling this
provider does not close the large historical D9VK gap in this pair. Do not
combine these runs with old D9VK scores to produce a new qualified ratio.

Selected engine-relative windows show nonuniform movement:

| Seconds | OFF FPS | ON FPS |
|---|---:|---:|
| 30–40 | 35.7 | 30.2 |
| 40–50 | 47.4 | 46.1 |
| 60–70 | 25.6 | 29.7 |
| 70–80 | 23.5 | 30.1 |
| 80–90 | 16.4 | 18.5 |
| 90–100 | 11.9 | 15.3 |
| 100–110 | 15.6 | 18.3 |

Some late windows improve while some earlier windows lose FPS. These are
reconstructed frame-time windows, not paired draw/camera identities or proof
that visibility causes the late improvement.

## Separate diagnostic work shape

| Observation | OFF | ON |
|---|---:|---:|
| Sampled Presents, including loading | 2,932 | 3,104 |
| Draws / sampled Present | 2,899.82 | 2,624.02 |
| CB creations / sampled Present | 7.72 | 16.20 |
| Mid-source sub-CBs / sampled Present | 4.30 | 3.66 |
| Render-pass begins / sampled Present | 30.44 | 30.39 |
| Sampled GPU CB errors | 0 | 0 |
| Qualified retained occlusion results | 56 | 95 |
| Zero results in that selected population | 0 | 93 |
| Whole-run source publications | 10,024 | 38,929 |
| SemanticBoundary publications | 0 | 32,748 |
| QueryEnd publications | 7,091 | 3,075 |

Draws per Present fall approximately 9.51%; CBs more than double while pass
counts remain similar. Loading, query frequency and observer perturbation
differ; these ratios are not matched-frame causal savings. The timeline is
bounded and selected, deduplicated by queue/query/issue/epoch. 93/95 is not
the full-run zero-result rate and does not prove equivalence to D9VK results.

ON scope-to-nonempty publication extent is 2,685 and scope-to-empty extent
is 30,063: **2,685 + 30,063 = 32,748**, accounting for all SemanticBoundary
publications. Empty END count is 30,063. Each publication has one observed
completion-bearing tail CB in both modes. CB creation, source-tail submission
and frame snapshots are different populations and need not match exactly.

## Interpretation and bounded next target

This confirms the H401/H402 mechanism with a separate observer-OFF pair:
visibility changes the workload, but the current provider also changes its
execution shape. It does not isolate which added cost offsets which saving.
The following are implementation restrictions, not necessarily unavoidable
costs of a GPU visibility result:

- `src/dxmt9/dxmt9_command_queue.cpp` / `setOcclusionScope` publishes populated
  writers on membership replacement; Arena admission rejects active scopes.
- `publishEarlyCpuReadyPrefix` suppresses early publication provider-wide
  under real visibility to avoid an incompatible successor wait.
- `src/dxmt9/render/session_source_policy.cpp` / `sessionSourceCanBeHead`
  and `sessionSourceCanAppendToPending` exclude scopes unless the separate
  scoped CB-join policy is enabled.
- `src/dxmt9/render/framegraph_backend.cpp` excludes scoped multi-source
  planning and bypasses scoped backend planning.
- `src/dxmt9/dxmt9_render_pass_encoder.mm` /
  `prepareRenderPassWithStoreProofLookahead` preserves attachments provider-wide.
  This safeguard addresses known depth-content failures; do not delete it.

Decision: reject **visibility ON alone** as the demonstrated large-gap fix;
do not reject actual visibility semantics as unnecessary. Keep default OFF
because this experiment adds no visual/GPU-exact qualification. Do not repeat
CB-only joining as a newly discovered optimization: H332 already reduced CBs
about 46% without a demonstrated FPS gain.

The next bounded design question is whether immutable query membership can
be represented as ranges inside an owned source/Slot, instead of requiring a
new source publication at each scope replacement. This could remove repeated
publication/admission work rather than only joining CBs afterward, but its
performance benefit is unproved. Before production implementation, specify
range ownership, origin-qualified result contribution, END-prefix submission,
overlap/reissue/reset and completion/failure behavior; test a small query-heavy
same-input fixture with CPU cost and exact visibility/pixels. A source boundary
must not simply be deleted without replacing its lifetime/progress contract.

The full D9VK differential remains unattributed. No runtime/default/README
change, commit or push. Raw results and failed captures are preserved.
