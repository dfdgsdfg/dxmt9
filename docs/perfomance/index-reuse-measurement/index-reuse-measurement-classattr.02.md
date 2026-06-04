---
domain: index-reuse-measurement
workload: 3DMark05 GT1
subcategory: classattr
order: 02
title: Large4096 Cross-Bucket Attribution
date: 2026-06-02
type: measurement
status: accepted
source: specs/perfomance.plan.md#L11165-L11250
---

# Large4096 Cross-Bucket Attribution

**Question / hypothesis.** A narrow `60/4` large-draw reverse probe was the first
clean signal to move the first-order VS-write counter, but `60/4` is
visibility-sensitive. Which `large4096` draws are production-safe (opaque
depth-write) versus diagnostic-only (depth-read/alpha)? Split the optimization
path from the diagnostic signal.

**Method.** Encoder breakdown extended with `large4096` × state-bucket
intersections:
`indexed_triangle_large_4096_{opaque_depth_write,depth_read,alpha_blend,scissor,textured}_draws/primitives/vertices`.
Validation run `large4096-cross-baseline-r1` (`--no-gputrace --measure-index-reuse
--encoder-breakdown-seq 60 --top 4 --hot-gpu-share 95`).

**Result.** Frame-60 `large4096` cross-bucket (draws):

| Row | large4096 d/p/v | opaque | depth-read | alpha | scissor | textured |
|---|---:|---:|---:|---:|---:|---:|
| `60/0` | `7/39,952/119,856` | `5` | `2` | `0` | `2` | `7` |
| `60/1` | `9/72,305/216,915` | `9` | `0` | `0` | `0` | `0` |
| `60/3` | `9/72,305/216,915` | `9` | `0` | `0` | `0` | `0` |
| `60/4` | `19/104,721/314,163` | `0` | `19` | `16` | `4` | `19` |

The direct positive `60/4 large4096` target is `0` opaque and entirely
depth-read (`19`), mostly alpha-blended (`16`), scissored (`4`), textured (`19`)
— not production-safe for primitive reordering. The correctness-preserving
candidate set is `60/1` + `60/3` opaque (`9 + 9 = 18`) plus the `5` opaque
draws in `60/0` = `23` opaque-large draws. Smoke run
`reverse-opaque-large4096-smoke-r1` hit exactly those `23` draws, left `60/4`
untouched, and stayed in shape-gate range (draws `716→710` `-0.84%`,
vertices `3,064,776→3,039,225` `-0.83%`).

**Verdict.** Accepted as the splitter between the diagnostic primitive-reorder
signal and the production-safe candidate. The earlier single-row `60/3` reverse
was negative, so opaque-large alone may not carry the same signal — the safe set
must be tested explicitly with Xcode before any production claim.

**Related.** [[index-reuse-measurement]] · follows
[[index-reuse-measurement-classattr.01]] · hands the safe opaque-large set to
[[index-cache-locality]] · diagnostic reorder signal owned by
[[primitive-reorder-diagnostics]] · width owner [[hidden-backend-storage]].
