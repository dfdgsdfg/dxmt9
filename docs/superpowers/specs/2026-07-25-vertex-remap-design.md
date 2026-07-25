# Vertex Remap Locality Discriminator — Design

Date: 2026-07-25
Status: approved design, not yet implemented

## Context

The dominant GPU cost in 3DMark05 GT1/GT2 under dxmt9 is a hidden vertex-stage
device-write bucket that no source-visible shader property explains:

| Evidence | Value |
|---|---|
| GT1 frame60 whole-frame GPU (Xcode) | `35.919ms`, weighted vertex stage `96.28%` |
| GT1 frame60 device write | `1,838.868MiB/frame` = `53.7 GB/s` during the GPU interval, `79%` of M1's `68.25 GB/s` |
| GT2 frame279 device write | `8,855.654MiB/frame` = `62.0 GB/s` during the GPU interval, `91%` of peak |
| Write vs visible `184B` VSOut requirement | `8.6x` (GT1), `15.66x` (GT2 whole frame) |
| Partial renders | `0` across every encoder — not parameter-buffer overflow spill |

Everything source-visible has been ruled out as the owner: VSOut field width,
half precision, position-only, point-size, fragmentless routing, the `r[32]`
temp array, the `outTexcoord[8]` scratch array, AIR-level IR shape, alpha-test
index eligibility, draw merging, and CPU translation cost (GT2 survives native
Metal replay with all CPU removed).

One result stands out as unexplained headroom. In
`docs/perfomance/mini-replay-bisection/mini-replay-bisection-replay.03.md`, a
113-draw row-local replay of GT1 frame60 encoder2 was compared against a
`sort-min-index` control:

| Case | VS invocations | VS B / invocation | GPU |
|---|---:|---:|---:|
| original order | `668,929` | `1,710.0` | `18.115ms` |
| `sort-min-index` control | `667,944` | **`442.6`** | **`7.925ms`** |

Invocation count moved `-0.15%` while write density fell `3.86x` and GPU time
fell `2.29x`. This rejects "the measured density is an M1 hardware floor" and
makes primitive/parameter-buffer locality a real backend-efficiency lever.

The problem is that `sort-min-index` changes two things at once:

- **(a)** index references become monotonic, so parameter-buffer writes coalesce
- **(b)** primitive order changes, so triangles group spatially and tile /
  primitive-block binning locality changes

Every production reorder lane that changes **(b)** is blocked on a final-color /
final-writer oracle (`gate_status=blocked-final-color-oracle` in the indexed
state-class proxy). If **(a)** is the real driver, there is a semantically free
way to get it: reorder the *vertex buffer storage* and rewrite indices
accordingly, keeping triangle order and triangle composition byte-identical.
That is a pure permutation — same vertices, same triangles, same order — so it
needs no oracle at all.

## Goal

Determine whether **(a)** index/parameter-buffer write locality or **(b)**
primitive order owns the `3.86x` density delta, using an offline discriminator
that cannot regress the runtime.

This design is a **scout**. It is explicitly allowed to kill the vertex-remap
idea. If the verdict is **(b)**, vertex remap is abandoned and the work returns
to the oracle problem.

## Non-goals

- No runtime encoder change. Zero lines in `src/`.
- No new `DXMT9_*` environment variable and no new perf counter.
- No production reorder policy, cache, or promotion decision.
- No GT2 measurement in this step. GT1 first; GT2 is a separate later step.
- No vertex compaction (dropping unreferenced vertices). See Follow-ups.

## Scope

- Target row: **3DMark05 GT1 frame60 encoder2**, the same hot row as
  `replay.03`. It is the only row with both a lower bracket (`1,710 B/inv`) and
  an upper bracket (`442.6 B/inv`) already measured, which makes a single run
  per lane interpretable.
- Repeat budget: **one Xcode counter export per lane** to start. The bracketed
  effect is `3.86x`, far outside Xcode replay variance (typically `CV < 5%`), so
  a single run resolves the `0.15` / `0.7` recovery thresholds when the result is
  decisive. If any lane's delta versus A lands under `10%`, escalate that lane to
  five runs with `scripts/tools/analyze_xcode_replay_variance.py` before drawing
  a conclusion.
- Changes land in `scripts/tools/run_3dmark05_mini_replay.py`, a new script
  test, and `docs/`.

## Lane definitions

All four lanes replay the same manifest, same build, same machine, same depth
sidecar.

| Lane | Primitive order | Vertex layout | Role |
|---|---|---|---|
| A | original | original | baseline; reproduces `1,710 B/inv` |
| B | `sort-min-index` | original | upper-bracket control; reproduces `442.6 B/inv` |
| C | **original (preserved)** | first-reference permutation | the candidate; bit-exact |
| D | `sort-min-index` | deterministic scatter permutation | discriminator |

Lane C isolates **(a)**. Lane D isolates **(b)**.

## Permutation rules

Both C and D are pure permutations of vertex storage. Neither changes vertex
byte content, vertex count, payload size, `base_vertex`, triangle composition,
or (for C) triangle order.

Let `stride` be the stream's vertex stride, `bv` the draw's `base_vertex`, and
`S = payloadBytes / stride` the dumped slot count. A vertex fetch reads slot
`index + bv`.

**Lane C — first-reference order**

1. Walk the index stream in triangle order. Collect `F`, the distinct fetch
   slots in order of first reference.
2. Assign the `k`-th element of `F` to new slot `bv + k`.
3. Assign the slots not referenced by the index stream to the remaining new
   slots in ascending order.
4. Rewrite every index that referenced original fetch slot `F[k]` to the value
   `k`. The fetch then resolves to slot `k + bv`, which is exactly where step 2
   wrote `F[k]`'s bytes.

Consequences: slot count and payload size are identical, `bv` is preserved, the
first-reference sequence is exactly `0, 1, 2, …`, and the new maximum index
`|F| - 1` is at most the original maximum index, so uint16 index payloads cannot
overflow.

`bv + |F| <= S` always holds and needs no fallback. Every index is
non-negative, so `bv <= min(F)`, and therefore
`bv + |F| <= max(F) + 1 <= S`. The implementation keeps the check as a
defensive assertion rather than a reachable branch, and `base_vertex` is never
rewritten.

**Lane D — scatter**

Same construction, but step 2 assigns `F` to new slots in the order produced by
sorting `F` on the deterministic key `(slot * 2654435761) mod 2^32`, ascending,
with the raw slot value as tiebreaker. A key sort is always a bijection, and the
multiplier makes the resulting order uncorrelated with both slot value and
first-reference order. This scatters index references while keeping lane B's
primitive order, and it is reproducible without depending on any language's RNG
implementation. Lane D may land worse than lane A; that is an acceptable and
informative outcome.

**All streams**

A D3D9 indexed draw uses the same vertex index for every stream, so one
permutation applies to `stream0` and each extra stream using that stream's own
`stride`. The manifest already carries `stream0_stride` and a per-stream
`stride`, so no new manifest field is required.

## Decision matrix

Lanes A and B bracket the effect, so each lane's result is scored as the
fraction of the bracket it recovers. Using `d(X)` for lane X's
`VS buffer write / invocation`:

```
recovery(X) = (d(A) - d(X)) / (d(A) - d(B))
```

`recovery(A) = 0` and `recovery(B) = 1` by construction. Values may fall outside
`[0, 1]`; lane D in particular may score negative if scattering is worse than
the original layout.

| Observation | Interpretation | Next action |
|---|---|---|
| `recovery(C) >= 0.7` and `recovery(D) <= 0.15` | **(a)** index/PB write locality owns it | design the runtime lane |
| `recovery(C) <= 0.15` and `recovery(D) >= 0.7` | **(b)** primitive order owns it | **abandon vertex remap**; return to the oracle problem |
| both fall in `(0.15, 0.7)` | both contribute | proceed partially; expected gain is C's measured value |
| `recovery(C) <= 0.15` and `recovery(D) <= 0.15` | lane B did not reproduce | harness/manifest defect; re-verify before interpreting |

Any combination not listed — for example both lanes above `0.7` — means the two
mechanisms are not separable by this experiment. Record the numbers and redesign
rather than forcing a verdict.

Lane B is also its own sanity check: if `d(B)` does not land near the
`442.6 B/inv` reference from `replay.03`, the manifest does not represent the
same row and no lane result should be interpreted.

## Gates

**Correctness gate.** Lane C's `--color-output` must be **bit-identical** to
lane A's, verified by comparing SHA-256 digests of the two output images. Lane C
is a data-equivalent permutation, so any pixel difference is a remap
implementation bug, not a finding. Lanes B and D are correctness-invalid
diagnostics and are exempt.

**Mechanism gate.** Report per lane: `VS buffer write / invocation`,
`VS invocations`, and `gpu_ms`. `VS invocations` must agree between A and C
within `1%` — a permutation does not change vertex reuse, so larger movement
indicates a defective permutation rather than a hardware response.
(`replay.03` saw `0.15%` between genuinely different orders, so `1%` is a loose
bound that still catches a broken permutation.)

## Components and data flow

```
run_3dmark05_perf_probe.sh --frame 60
  --dump-indexed-geometry --dump-indexed-geometry-cbufs --dump-shaders
  --dump-depth-attachment-{handle,seq,enc,path}
  (scoped to encoder2 via reverse-indexed row/class filters and
   DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MIN/MAX)
        |
plan_3dmark05_mini_replay.py -> build_3dmark05_mini_replay_manifest.py
        |  base manifest: index + stream payloads + strides + base_vertex + MSL
run_3dmark05_mini_replay.py, four lanes
  A: --primitive-order original       --vertex-order original
  B: --primitive-order sort-min-index --vertex-order original
  C: --primitive-order original       --vertex-order first-reference
  D: --primitive-order sort-min-index --vertex-order scatter
  each: --compile --run --depth-input <same sidecar> --capture-path --color-output
        |
Xcode manual export x4 (metal_debugging.rules.md section 2b)
  -> traces/<run>/analysis/lane{A,B,C,D}-counters-xcode.csv
        |
summarize_xcode_encoder_counters.py per lane + comparison table
A vs C color bit comparison
```

**Harness change — one place.** Add `--vertex-order {original,first-reference,
scatter}` to `scripts/tools/run_3dmark05_mini_replay.py`, implemented as
`transform_vertex_layout(geometry, state, vertex_order)` alongside the existing
`transform_index_payload`. It must run **after** the index transform so lane D
composes `sort-min-index` indices with a scatter layout. It writes new stream
and index payload files into a lane-specific directory, updates
`geometry["stream0_file"]`, `geometry["streams"][i]["file"]`, and
`geometry["index_file"]`, and records `geometry["vertex_order"]` for provenance.

**Harness precondition.** The existing index rewrite path is uint16-only
(`transform_index_payload` and `uint16_indices` both reject non-uint16-aligned
payloads). Verify that the dumped encoder2 draws use uint16 indices. If any use
uint32, either extend the harness to both widths or exclude those draws from the
manifest and record the exclusion.

## Testing

`tests/scripts/test_mini_replay_vertex_order.py`, a pure deterministic test with
no GPU or Wine dependency, next to the existing
`tests/scripts/test_build_3dmark05_mini_replay_manifest.py`.

The load-bearing invariant is fetch equivalence. For every index position `i`:

```
newStream[(newIdx[i] + bv) * stride ...] == oldStream[(oldIdx[i] + bv) * stride ...]
```

If this holds, the remap is data-equivalent by construction. Additional cases:

- the permutation is a bijection over `[0, S)`
- payload size is preserved and the byte multiset is preserved
- `base_vertex` is preserved
- lane C's first-reference sequence is monotonic
- lane D satisfies the same fetch invariant (data-exact, order scattered)
- the `bv + |F| > S` fallback path
- multiple streams with different strides are permuted consistently

## Artifacts

- Leaf document
  `docs/perfomance/mini-replay-bisection/mini-replay-bisection-vertexremap.01.md`,
  one experiment per file with `source:` provenance, matching the domain that
  owns row-local replay (`replay.03` lives there).
- If the verdict changes direction, update
  `docs/perfomance/hidden-backend-storage/overview.md`'s conclusions table and
  the GT1 root map `docs/perfomance/overview-3dmark05-gt1.md`.
- Add `--vertex-order` to the mini-replay flag row in
  `agents/rules/metal_debugging.rules.md` section 9.

No `agents/rules/environment_variables*.rules.md` change is needed because the
new flag is harness-only and sets no `DXMT9_*` variable.

## Risks and follow-ups

**Runtime shared-vertex-buffer cost (deferred).** The permutation depends on the
index traversal order, so it is per-draw. In the scout this is free because the
geometry dump already carries a separate stream payload per draw. A runtime lane
would need a remapped vertex buffer per `(VB contentRevision, IB
contentRevision, IB span)` triple — the same key shape as the existing
`ReorderedIndexBufferCacheKey` — which means several draws sharing one vertex
buffer hold several remapped copies. For static geometry this converges to a
one-time cost, but the memory bound must be designed explicitly before any
runtime lane is built. It is out of scope here.

**Vertex compaction (deferred).** Dropping unreferenced vertices could shrink
the read working set on top of the locality change, but it confounds locality
with size reduction and would defeat this scout's discriminating purpose. If
lane C succeeds, re-measure compaction as a separate follow-up question.

**Null result is a valid outcome.** The decision matrix includes an explicit
abandon branch. A `(b)` verdict is a successful scout, not a failure.

## References

- `docs/perfomance/mini-replay-bisection/mini-replay-bisection-replay.03.md` —
  the `1,710` vs `442.6 B/inv` bracket this design targets
- `docs/perfomance/hidden-backend-storage/overview.md` — the five-component
  attribution model and the full list of rejected owners
- `docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.30.md` —
  "GPU efficiency ceiling is separate from wall-clock FPS ownership"
- `docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.36.md` —
  GT2 native replay removes CPU from the timed path
- `agents/rules/metal_debugging.rules.md` — Xcode counter export discipline,
  replay variance tooling, trace artifact layout
