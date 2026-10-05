---
domain: present-pacing
workload: STALKER CoP capture29
title: "V4 fixed-input observer-light CPU route comparison"
type: experiment
status: measured-no-material-route-advantage
updated: 2026-10-03
source: experiments/output/stkcop-v4-controlled-cpu-13/verification.md; experiments/output/stkcop-v4-controlled-cpu-13/experiment-aa-r1/result.json; experiments/output/stkcop-v4-controlled-cpu-13/experiment-forward-r1/result.json; experiments/output/stkcop-v4-controlled-cpu-13/experiment-reverse-r1/result.json
related: specs/experiments/harness/replay/spec.md; specs/experiments/gap.md
---

# V4 fixed-input CPU route comparison

The public paired qualifier binds unchanged capture29 to matching 2,806
original-keyed logical items and four original-query/pixel GPU checks. The
subsequent execution-only CPU driver omits diagnostic hashing and observes
native body wall/process CPU separately from setup, teardown and authentication.
Each comparison interleaves two arms with two warmups and 16 measured fresh
processes per arm. All 108 executions and final reauthentication pass.

| Comparison | First arm wall / CPU median (ms) | Second arm wall / CPU median (ms) |
|---|---:|---:|
| Compatibility / Compatibility | 69.715 / 70.081 | 69.944 / 70.324 |
| Compatibility / ProductionSlot | 70.339 / 70.702 | 69.969 / 70.370 |
| ProductionSlot / Compatibility | 68.878 / 69.297 | 68.937 / 69.377 |

A/A wall median difference is 0.329%. ProductionSlot is lower by 0.526%
forward and 0.086% reverse. These runs do not establish a material route
advantage or a confidence interval. Absolute inter-run drift and the changing
effect size prevent treating the small difference as an optimization win.

Compatibility executes all 2,799 draws in 80 batches. ProductionSlot executes
534 Direct draws with 54 leases and 2,265 compatibility draws in 26 batches.
Both consume three sources and 512 commands. This is a mixed-route comparison,
not all-Direct execution or different source grouping.

The controlled consumer uses immediate FIFO CPU completion, not Metal encoding
or GPU completion. These timings neither explain the whole-game STALKER/D9VK
gap nor rank encoder parallelism. Original numeric occlusion coverage is absent;
the preservation contract remains conditional on legal NOOVERWRITE and closed
writers. Matching logical fingerprints is not an independent complete
scalar/uniform oracle.

Next: bind exact-fit/1 MiB/5 MiB retained-capacity policy identities to separate
CPU and GPU qualification under unchanged event/query cuts, then measure them.
Record actual provisioning/reuse/rotation rather than assuming headroom changes
grouping. Encoder partitioning is a separate authority and measurement axis.
