---
domain: index-reuse-measurement
subcategory: geometry
order: 02
title: Geometry Signature / Dedup Result
date: 2026-06-01
type: measurement
status: rejected
source: specs/perfomance.plan.md#L3409-L3431
---

# Geometry Signature / Dedup Result

**Question / hypothesis.** Does the ~1.6 GiB top-frame VS-buffer-write bucket come
from many unique geometry submissions, or from repeated (deletable) geometry
shapes? i.e. is "redundant replay of the same geometry shape" the first-order
owner?

**Method.** Run `app-d3d9-3dmark05-geometry-signature-gputrace-r2` with the
encoder-local geometry signature from
[[index-reuse-measurement-geometry.01]] enabled and a matched Xcode export. Read
top-3 `draw_geometry_signature_unique` / `_duplicates` /
`_consecutive_duplicates` against the Xcode VS-write counters.

**Result.** Total GPU `33.688ms`; top-3 encoders `33.153ms` / `98.41%`. Top-3
Xcode buffer write `1628.008MiB`, of which `1627.192MiB` is VS buffer write.
Top-3 dxmt explicit CPU writer bytes `0.444MiB` → unexplained/Xcode ratio
`1.000x`. Top-3 geometry signatures: `330` unique / `55` duplicate over `385`
draw samples → aggregate duplicate ratio `0.143x` (`0.122x` consecutive). Hot
encoders individually `0.13x–0.17x` duplicate ratio while still writing `225MiB`,
`421MiB`, and `981MiB` of VS buffer traffic.

**Verdict.** Rejected as the owner. There are some repeated backend shapes but
nowhere near enough to explain a ~1.6 GiB hidden vertex-stage bucket. Owner stays
classified as real submitted geometry/primitive pressure interacting with Apple
GPU hidden vertex/tiler/parameter storage. Deleting duplicate draws is at most a
minor CPU/GPU improvement, not the first-order GPU bottleneck.

**Related.** [[index-reuse-measurement]] · follows
[[index-reuse-measurement-geometry.01]] · next is size histogram
[[index-reuse-measurement-geometry.03]] · confirms width owner
[[hidden-backend-storage]] · rejects visible-width ownership [[vsout-layout]].
