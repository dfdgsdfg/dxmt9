---
domain: present-pacing
workload: STALKER CoP Day/high 1280x720, Sikarugir CX24.0.7
title: "In-place Primary observer: conditional bound and matched baseline"
type: evidence
status: diagnostic-conditional-no-default-transfer
updated: 2026-10-07
source: traces/stkcop-primary-inplace-h460/{generate.py,interposer.hpp,marker.hpp,reader.c,reader.dylib,sample_window.py,analyze.py,manifest.json}; traces/stkcop-primary-inplace-h460/{full-inplace-r3,full-cow-r4,full-baseline-r5,dxmt9-r1,default-r1}; traces/stkcop-primary-inplace-h460/perturbation-comparison.json
related: present-pacing-primary-interposer-rejection.458.md; present-pacing-completed-workload-visibility.455.md; present-pacing-occlusion-msaa-oracle.459.md
---

# Conditional Primary renderer bound; no default-path dominance claim

H460 preserves the original vptr identity rejected by H458's cloned-table
observer. Diagnostic thunks replace original table slots on process-private
image COW pages; original callables are saved before publication. Object vptrs,
production validation and original callable bodies are unchanged. This remains
implementation-specific diagnostic instrumentation, not a runtime feature.

## Qualification and runtime identity

Actual-object fixtures cover all 16 table bits and 12 exports, including
texture binding, container aliases, cached backbuffer release/reacquisition,
StateBlock and query objects. Baseline and diagnostic fixtures pass. A parent
mapping of the same DLL retains byte-identical `.rdata` after the child patches
and exercises actual objects. Three unload/reload cycles pass. The broad
fixture creates StateBlock before resource setup; this does not claim to fix
H458's archived failure with a different ordering.

| D3D9 image | SHA-256 |
|---|---|
| Baseline | `534ea6f17128354ea350b7d6000c750e82bfcc3556d827464ebf02e69e8f02d0` |
| In-place diagnostic | `6d004a59cfa2e975c8a5de74575c70dd77eff863a5e507ffa2acee8eb654dc38` |

Both STALKER runs finish normally and restore shared Wine staging to baseline.
Canonical and shared x86 D3D9 hashes match afterward. Real occlusion remains
OFF; perf counters and frame sampling are OFF. Settings retain max-threads=2,
snd_targets=16 and draw chunk command limit=0. Scene/tail screenshots are spot
evidence only, not whole-sequence or exact pixel equivalence.

## Conditional 102–110 second window

The observer predeclares 1,600 strata of 5 ms and one planned point per stratum.
All observations remain in the population; late or uncertain reads count as
renderer-possible. The clockless outer marker includes waits inside calls, so
residency is not CPU time. The upper bound is
`min(Primary CPU upper, window wall * residency upper)`, never occupancy times
Primary CPU.

| Quantity | Diagnostic result |
|---|---:|
| Renderer-possible planned points | 636 / 1,600 |
| Residency upper including nominal Hoeffding margin | 0.435436 |
| Primary total CPU, enclosing interval | 7.039138 s |
| Conditional Primary renderer CPU upper | 3.483485 s |
| Encode CPU, contained/enclosing intervals | 4.084206–4.256130 s |

Thus the conditional Primary renderer upper is below the contained Encode
lower in this diagnostic. Without the publication assumptions the renderer
upper returns to 7.039138 s and this ordering is unresolved.

Independent review confirms the interval orientation and arithmetic but adds
two qualifications to the archived analyzer's assumptions:

- The fixed `random.Random(460)` schedule must additionally be modeled as
  independent uniform draws. Exogeneity and predeclaration alone do not give a
  design-based confidence guarantee. The 99% level is nominal/model-conditional.
- The emitted ARM64 reader has DSB ISH before the clock call and ISB afterward.
  An additional ISB apparently present after a semicolon in the inline source
  is not emitted. Qualification concerns actual instructions, not that source
  spelling. Cross-architecture publication remains an assumption.

Complete outer ingress coverage, matching guest/native Primary identity and
an exogenous observer remain required. Other unnamed target threads have not
been fully attributed; this is not a process-wide ranking.

## Matched baseline and observer cost

The archived H455 camera/endpoint reducer is reused with the H460 run root.
Seven camera knots and four discrete candidate endpoint variants pass its
2 m / 3 degree and native CPU gates in all three windows. These are diagnostic
CPU values, not observer-OFF FPS results.

| Window | Baseline Primary ms/frame | Diagnostic Primary | Baseline Encode ms/frame | Diagnostic Encode |
|---|---:|---:|---:|---:|
| 40–55 s | 19.26 | 19.10–19.41 | 13.36 | 13.23–13.49 |
| 80–95 s | 44.02 | 41.59–42.16 | 25.64 | 24.39–24.83 |
| 102–110 s | 48.78 | 48.94–49.72 | 30.57 | 29.69–29.96 |

In the contained approximately 7.72-second tail observation, the diagnostic
observer uses 0.350546 CPU seconds (sampling thread 0.282807), and the helper
uses 0.118147. Baseline observer uses 0.067230 seconds over approximately 7.69 s.
These costs are not subtracted and do not bound scheduling perturbation. One
pair does not transfer the conditional ownership bound to default code.
Completed-frame sampling reports terminal offset discontinuities around
118.4 s in both runs; these are outside the reported windows and prohibit a
whole-scene counter census claim.

## Decision

Stop expanding Primary instrumentation for the next optimization decision.
H460 is sufficient to reject treating the entire mixed Primary CPU total as
PE cost, but not sufficient to declare Encode the largest default-path owner.

The concrete next candidate remains H455's real-occlusion path: repeated
camera-matched Encode reductions of 4.60–6.11 ms/frame motivate completing its
correctness and observer-OFF acceptance gates. H459 adds bounded MSAA evidence,
not full-scene qualification. Resolve the known sampler validation failure,
retain public query/lifetime/fault and attachment-preservation gates, then
accept or reject the candidate with a bounded OFF/ON comparison. Do not infer
the full backend performance gap or FPS gain from these CPU differences.
