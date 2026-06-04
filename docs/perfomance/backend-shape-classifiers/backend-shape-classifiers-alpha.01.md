---
domain: backend-shape-classifiers
workload: 3DMark05 GT1
subcategory: alpha
order: 01
title: Probe Disable Alpha Blend (broad)
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L2183-L2196
---

# Probe Disable Alpha Blend (broad)

**Question / hypothesis.** Does the global alpha-blend render state own the
hidden ~1.6 GiB "VS Buffer Device Memory Bytes Written" bucket on the top GT1
frame60 encoders? This is a correctness-invalid state classifier, not a fix.

**Method.** `DXMT9_PROBE_DISABLE_ALPHA_BLEND=1` (broad, all draws). Captured
`app-d3d9-3dmark05-probe-disable-alpha-blend-gputrace-r1`, exported Xcode
encoder counters, finalized against the normal-source baseline. The user-observed
output is a solid yellow frame, confirming the correctness failure.

**Result.**

| Metric | Normal | Disable alpha blend | Delta |
|---|---:|---:|---:|
| Total GPU | `35.456ms` | `36.010ms` | `+1.56%` |
| Top-3 GPU | `34.837ms` | `35.438ms` | `+1.72%` |
| Top-3 VS buffer write | `1627.240MiB` | `1627.268MiB` | `+0.00%` |
| Top-3 unexplained write | `1627.596MiB` | `1627.599MiB` | unchanged |

**Verdict.** Rejected. Broad alpha-blend state is not the first-order owner of
the hidden VS-write bucket — GPU time regresses while VS write is effectively
unchanged. Must not be used as a baseline or promoted as an optimization. The
scoped class-filtered probe ([[backend-shape-classifiers-alpha.03]]) later showed
alpha blend *is* a significant factor when isolated to the large4096+alpha class.

**Related.** [[backend-shape-classifiers]] · first in the alpha sequence, followed by [[backend-shape-classifiers-alpha.02]] · confirms [[hidden-backend-storage]] owner survives · refutes broad alpha state · related [[backend-shape-classifiers-alphatest.01]].
