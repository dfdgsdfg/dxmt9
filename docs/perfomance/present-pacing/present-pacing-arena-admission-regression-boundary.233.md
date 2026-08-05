---
domain: present-pacing
workload: 3DMark05 GT2
title: "Present-Pacing #233 - Segmented Arena Admission Turns Capacity Pressure Into Session Fragmentation"
type: leaf
status: current
updated: 2026-08-05
source: experiments/output/app-d3d9-3dmark05-clean-boundary-0eaa-tape-on-gt2-r1-20260805/result.json; experiments/output/app-d3d9-3dmark05-clean-boundary-c8d8-tape-on-gt2-r1-20260805/result.json
related: specs/backend/encode-scheduling/gap.md; specs/backend/encode-scheduling/requirements.md; specs/backend/encode-scheduling/spec.md
---

# Present-Pacing #233 - Segmented Arena Admission Turns Capacity Pressure Into Session Fragmentation

## Question

Did the GT2 locality regression enter with the source-kind-neutral session join
at `0eaa0d27`, or with the segmented Arena/Present completion at `c8d8f83d`?
The previous gap entry quoted a 512-page run with 4.044 command buffers per
Present, but that result did not reproduce from clean current-master sources.

## Clean boundary comparison

Both revisions were checked out as detached clean worktrees and built with the
canonical release directories and matching Meson options. Each run used the
repository-required Sikarugir runtime, `DXMT9_CPU_READY_TAPE=1`, GT2,
`--no-gputrace`, frame sampling, and the same probe wrapper. The staged unix
provider hashes were `a464ccb990c21897` for `0eaa0d27` and
`2586f9230077ea94` for `c8d8f83d`.

| Counter | `0eaa0d27` | `c8d8f83d` | Structural delta |
|---|---:|---:|---:|
| Presents | 2,784 | 2,675 | diagnostic denominator only |
| Command buffers / Present | 3.9996 | 23.7357 | +493.4% / 5.93x |
| Render passes / Present | 11.2029 | 27.7779 | +147.9% / 2.48x |
| Resident pages peak | 143 | 512 | `c8d8` pins the arena limit |
| Resident sources peak | 17 | 17 | source descriptors are not the limit |
| Ready entries peak | 15 | 16 | Ready FIFO is not the limit |
| Admission waits | 0 | 28,782 | new dominant pressure path |
| Admission wait | 0 ms | 47,889.008 ms | wait conservation fails |
| Admission-pressure session releases | 0 | 45,902 | 17.16 / Present |
| Pending sessions started | 1,127 | 48,535 | 18.14 / Present |
| Legacy oversize bypasses | 59,747 | 0 | representation cutover |
| GPU command-buffer errors | 0 | 0 | both runs pass the error smoke |

These are locality and pressure results, not an FPS comparison. One run per
revision is sufficient to classify a 5.93x command-buffer change and exact
512-page saturation, but not to claim a small throughput delta.

## Regression boundary and mechanism

`0eaa0d27` is locality-safe in this comparison. It joins source kinds while
large candidates still take the oversize rollback path. `c8d8f83d` removes that
bypass by representing segmented sources whose total bound may consume the
entire 512-page Arena. Once one candidate occupies the available page budget,
the replay worker observes admission pressure. The current policy turns that
observation into a release fence, submits the parked session, and repeats. The
near equality between pressure releases and pending-session starts identifies
capacity-driven session destruction, not source-prefix selection, as the first
order regression.

The earlier 4.044-CB / 17.615-pass 512-page result was produced from a different
uncommitted state and cannot be used as evidence for clean `c8d8f83d`. It is
historical mechanism evidence only.

## Design consequence

Increasing the Arena alone cannot satisfy the locality gate. The streaming path
needs a deterministic capacity policy before another runtime promotion:

- acquire a fixed session-capacity lease before representing the first source;
- reserve successor headroom for one worst-case ordinary Direct reservation,
  including circular-wrap padding and non-page capacity dimensions;
- release at fixed source/page/byte/draw caps selected from ordered source
  summaries, leaving the first over-cap candidate Ready; and
- let older GPU-resident work delay lease acquisition or publication, never
  choose the membership or submission boundary of an open session.

The promotion signal is therefore zero pressure-created session releases plus
flat command-buffer/pass shape, not merely fewer admission waits.
