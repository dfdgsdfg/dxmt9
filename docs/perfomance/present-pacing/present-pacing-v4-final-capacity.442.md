---
domain: present-pacing
workload: STALKER CoP capture29
title: "Final-image cold capacity matrix after immutable preparation"
type: experiment
status: measured-no-material-capacity-advantage
updated: 2026-10-04
source: experiments/output/stkcop-v4-final-policy-25/verification.md; experiments/output/stkcop-v4-final-policy-25/analysis.jsonl; experiments/output/stkcop-v4-final-policy-25/{exact-aa-r1,exact-1mib-r2,1mib-exact-r1,exact-5mib-r1,5mib-exact-r1}/result.json; experiments/output/stkcop-v4-final-policy-25/{paired-1mib-r1,paired-5mib-r1}/result.json; experiments/output/stkcop-v4-immutable-preparation-24/paired-l0-r1/result.json
related: docs/perfomance/present-pacing/present-pacing-v4-immutable-preparation.441.md; docs/perfomance/present-pacing/present-pacing-v4-capacity-comparison.432.md
---

# Final-image capacity comparison

The final immutable-preparation image qualifies exact-fit, 1 MiB and 5 MiB
ProductionSlot candidates against paired ordered CPU effective semantics and
original GPU pixels/EVENT readiness. Positive policies retain the 64 MiB
aggregate ceiling and zero grouping limit. Input, plan, endpoint executables,
shared runtime modules and qualification launcher identities match.

Five sequential comparisons pass 180 fresh execution-only CPU children:
two warm-ups and 16 measured samples per arm, 20 warm-ups and 160 measured
samples total. Main's artifact analysis revalidates qualifications, native
receipts, final image identities, lifecycle/conservation, observer omissions,
per-arm policy and published summaries. No concurrent analysis or agent work
was run during the four final capacity comparisons.

| Ordered arms | Body wall medians ms | Process CPU medians ms |
|---|---:|---:|
| Exact / Exact | 69.477 / 69.224 | 69.628 / 69.467 |
| Exact / 1 MiB | 69.430 / 69.120 | 69.655 / 69.310 |
| 1 MiB / Exact | 69.032 / 69.449 | 69.210 / 69.581 |
| Exact / 5 MiB | 69.302 / 69.598 | 69.579 / 69.821 |
| 5 MiB / Exact | 69.443 / 69.443 | 69.762 / 69.768 |

Relative to exact-fit in each comparison, 1 MiB lowers median body process CPU
by 0.495% forward and 0.533% reverse, with wall reductions 0.446% and 0.601%.
The 5 MiB CPU differences are +0.348% and -0.009%; wall is +0.427% and +0.001%.
A/A itself differs by -0.231% CPU and -0.365% wall. These A/A values are not
confidence intervals or universal noise bounds. The small 1 MiB observation
does not establish a material production advantage or justify a default change.

Every policy preserves 2,799 draws and 87 original raw acknowledgments.
Direct coverage is 534/781/648 draws for exact/1 MiB/5 MiB, respectively.
Process-exit retained-capacity reuse is zero in every measured row. The cold
input therefore cannot establish a persistent-slot reuse benefit. Process-exit
provisioning/retained gauges are not body deltas or RSS; the artifact analysis
reports their scope and missing observations explicitly.

## Evidence boundaries

The original forward `exact-1mib-r1` also passed qualification but overlapped a
read-only analysis process near its final samples. It remains preserved as
diagnostic evidence and is excluded from this matrix; `exact-1mib-r2` replaces
it. No failed or slow iteration is removed from any accepted aggregate.

This is the declared fresh-device, legal-NOOVERWRITE/closed-writers CPU domain
with immediate-FIFO controlled acknowledgment. Body timing excludes setup and
teardown and does not execute Metal encoding. The independent paired GPU lane
supplies original pixels/query qualification, not GPU timing. External system
load and driver caches remain uncontrolled. No FPS, warm-device, numeric
occlusion, unconditional future-preservation or independent original scalar
reconstruction authority is claimed.

Current evidence supports using the completed cold policy harness for further
bounded experiments. It does not identify capacity headroom as the outstanding
STALKER performance fix. H441's remaining resource-setup service cost and
exclusive actual-encoder attribution are separate investigations.
