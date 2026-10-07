---
domain: present-pacing
workload: STALKER Day tail, archived default policy and in-place diagnostic
title: "The finite four-run Primary attribution experiment rejects default transfer"
type: evidence
status: completed-four-runs-default-transfer-rejected
updated: 2026-10-07
source: traces/stkcop-primary-decision-h465/{contract.md,forward.json,reverse.json,baseline-repeat.json,decision.json,decide.py}; traces/stkcop-primary-decision-h465/{default-r1,dxmt9-r1,dxmt9-r2,default-r2}/receipt.json; traces/stkcop-primary-decision-h465/{dxmt9-r1,dxmt9-r2}/conditional-bound.json
related: present-pacing-default-thread-identity.462.md; present-pacing-completed-workload-visibility.455.md; present-pacing-occlusion-edge-isolation.464.md
---

# A diagnostic-only ranking does not establish the default CPU owner

All four predeclared runs have completed normally, in order default-r1,
diagnostic-r1, diagnostic-r2, default-r2. This supersedes the three-run commit
checkpoint. These measurements use archived H453 runtime images, not the newly
built H463 public oracle provider. The final run restores baseline x86 D3D9
SHA-256 `534ea6f17128354ea350b7d6000c750e82bfcc3556d827464ebf02e69e8f02d0`.

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
transferring the diagnostic ranking under the declared contract.

The second diagnostic also has a conditional renderer upper below Encode:
3.426 s versus 4.002 s. The reverse camera-matched comparison nevertheless
fails the Primary stability gate: diagnostic 52.171 ms/frame versus default
48.791–49.391 ms/frame (ratios 0.9352–0.9467). Encode is 31.266 versus
30.192–30.340 ms/frame (ratios 0.9657–0.9704).

Default-repeat controls pass: Primary ratios 1.0165–1.0240 and Encode
1.0122–1.0204. All three comparisons pass the camera gate. Both diagnostic
comparisons fail stability; the final reducer reports `accepted=false`.

The separately declared transfer sensitivity also fails. Conservative CPU/frame
bounds give maximum diagnostic renderer upper 27.077 ms, minimum default Encode
lower 28.238 ms, and maximum whole-Primary upper 53.318 ms. The explicit 5% of
whole-Primary allowance is 2.666 ms, larger than the 1.161 ms ranking margin.
These conservative fixed-window bounds are not the camera-endpoint point
estimates in the table. Joint 99% coverage applies only to the two conditional
diagnostic sampling bounds, not to transfer or all-thread ownership.

Do not add replacement runs, widen the band, or subtract the observed Primary
increase to manufacture a default-path bound. This rejects the attribution
method's transfer claim, not the hypothesis that Encode might be largest.

## Next decision

The finite experiment is closed. Do not start another open-ended Primary
marker redesign to select an optimization. H462's dedicated-worker evidence
and H455's workload-changing visibility candidate remain separate findings.

The most actionable candidate is real occlusion qualification: the conservative
provider returns positive results even for fully hidden draws; H455 observed
4.60–6.11 ms/frame less Encode CPU with real visibility. That is a candidate
effect, not a correctness-qualified saving or an FPS promise.
[H466](present-pacing-positiont-pixel-center.466.md) subsequently closes the
bounded shared XYZRHW failure. Qualify and price real visibility using the
same corrected provider in both arms. No runtime default or README score
changes here.

The largest default dxmt9 single-thread CPU owner and its validated removable
budget remain unresolved. This checkpoint does not mark that objective complete.
