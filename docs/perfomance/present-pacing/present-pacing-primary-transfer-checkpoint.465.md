---
domain: present-pacing
workload: STALKER Day tail, archived default policy and in-place diagnostic
title: "Primary attribution transfer remains unqualified at the commit checkpoint"
type: evidence
status: incomplete-three-of-four-runs-first-stability-gate-failed
updated: 2026-10-07
source: traces/stkcop-primary-decision-h465/{contract.md,forward.json,baseline-self.json}; traces/stkcop-primary-decision-h465/{default-r1,dxmt9-r1,dxmt9-r2}/receipt.json; traces/stkcop-primary-decision-h465/dxmt9-r1/conditional-bound.json
related: present-pacing-default-thread-identity.462.md; present-pacing-completed-workload-visibility.455.md; present-pacing-occlusion-edge-isolation.464.md
---

# A diagnostic-only ranking does not establish the default CPU owner

At this commit checkpoint, three of the four predeclared H465 runs have
completed. The final `default-r2` has not run, and no complete four-run verdict
is claimed. These measurements use archived H453 runtime images, not the newly
built H463 public oracle provider.

The first diagnostic's fixed 102–110 s interval gives a conditional Primary
renderer CPU upper bound of 3.331 s, below the same-run Encode lower bound of
3.724 s. The bound assumes complete ingress coverage, marker publication and
an exogenous observation schedule. It is not a default-path measurement.

The camera-qualified forward comparison fails the predeclared +/-5% stability
gate:

| Native CPU per completed frame | Default r1 | Diagnostic r1 endpoint range |
|---|---:|---:|
| Whole Primary, including game/Wine | 47.997 ms | 53.877–54.733 ms |
| Encode | 29.829 ms | 31.239–31.557 ms |

Primary increases approximately 12–14%; Encode increases approximately 5–6%.
This does not isolate marker overhead from run variation, but it prevents
transferring the diagnostic ranking under the declared contract. A final
reverse control cannot erase this valid failed gate. Do not add replacement
runs or widen the acceptance band until the experiment passes.

## Next decision

If resumed, complete only the outstanding control and close the finite
experiment. Do not start another open-ended Primary marker redesign to select
an optimization. H462's dedicated-worker evidence and H455's workload-changing
visibility candidate remain separate findings.

The most actionable candidate is real occlusion qualification: the conservative
provider returns positive results even for fully hidden draws; H455 observed
4.60–6.11 ms/frame less Encode CPU with real visibility. That is a candidate
effect, not a correctness-qualified saving or an FPS promise. Resolve the
bounded shared XYZRHW raster failure, then qualify and price real visibility
on one matched current build. No runtime default or README score changes here.

The largest default dxmt9 single-thread CPU owner and its validated removable
budget remain unresolved. This checkpoint does not mark that objective complete.
