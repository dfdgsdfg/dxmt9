---
type: "Spec Requirements"
title: "Harness Replay Requirements — Offline Metal Replay"
description: "Requirements for the replay domain: standalone Metal replay of dumped 3DMark05 geometry/shader payloads outside Wine."
tags: [specs, experiments, harness, replay, requirements]
---

# Harness Replay Requirements — Offline Metal Replay

This is a domain document under `specs/experiments/harness/`. It
instantiates the `R-HARN-*` requirement groups in
`specs/experiments/harness/requirements.md` for the `replay` domain
named in `specs/experiments/harness/spec.md` §1. The replay domain
participates in the `offline-replay` pipeline stage only (parent
spec.md §0): it reads geometry, shader, and cbuf payloads a `probe`-
domain run already dumped and renders them in a standalone,
Wine-independent Metal process. Requirement IDs in this file use the
prefix `R-HARN-REPLAY-`.

**This domain does not currently render a valid image.** Every
requirement below states the contract the domain must satisfy; it
does not describe the domain's present behavior as meeting that
contract. Where today's source violates a requirement, this file says
so in the requirement's own rationale, and this domain's own
`spec.md` Known Deviations section states the same defect from the
implementation side.

---

## 1. Scope and Stage Participation

**R-HARN-REPLAY-1.1** The replay domain comprises exactly the scripts
the parent domain map (`specs/experiments/harness/spec.md` §1) assigns
to it: `scripts/tools/build_3dmark05_mini_replay_manifest.py`,
`scripts/tools/plan_3dmark05_mini_replay.py`, and
`scripts/tools/run_3dmark05_mini_replay.py`. This domain's own
`spec.md` names only the `offline-replay` pipeline stage (parent
spec.md §0) as a stage it participates in — even though the manifest
builder's own `--shader-summary` input crosses in from the `join`
domain and its `--probe-draws` input crosses in from the `reduce`
domain (parent spec.md §1, "reduce versus join"). A change to this
domain must not describe consuming those two inputs as participation
in `external-join` or `log-reduce`; consuming another domain's output
artifact is not the same as performing that domain's stage work.
Instantiates R-HARN-1.1.

**R-HARN-REPLAY-1.2** This domain is a read-only consumer of the
`dump-extract → offline-replay` boundary (parent spec.md §2): it must
never write into a `probe`-domain trace directory's `analysis/geometry/`,
`analysis/shaders/`, or `analysis/frame<N>-depth.bin` paths, only read
from them. A change that has this domain re-derive or re-dump geometry
or shader payloads itself, instead of consuming what `dump-extract`
already wrote, duplicates a responsibility the parent domain map
assigns to `probe`. Instantiates R-HARN-1.1 (parent spec.md §1, "Why
the domain axis is harness families, not stages").

---

## 2. Engine-Shape Dependencies Are Declared and Detectable

**R-HARN-REPLAY-5.1** This domain declares, in its own `spec.md`, the
finite set of Metal cbuf-binding signatures it accepts from dumped VS/
FS MSL: the legacy argument-buffer shape
(`constant ArgbufLayout& abuf [[buffer(30)]]`) and the current
direct-cbuf shape (`constant VsConsts& vsConsts [[buffer(0)]]` /
`constant PsConsts& psConsts [[buffer(0)]]`, each paired with a
`constant FfpVsConsts& ffpVs [[buffer(3)]]` /
`constant FfpPsConsts& ffpPs [[buffer(3)]]`). No third shape is
declared or accepted today. Instantiates R-HARN-5.1. Rationale: the
first shape is what dxmt9 emitted before `DXMT9_ARGBUF_DIRECT_CBUF`
was promoted default-on in commit `9eb02437`; the second is what it
emits now. Neither the requirement nor `transform_msl` may treat one
shape as a legacy special case of the other — they are declared as two
independent, fully-specified accepted inputs.

**R-HARN-REPLAY-5.2** When dumped MSL matches neither shape declared
by R-HARN-REPLAY-5.1, this domain's `transform_msl` fails with a
message naming the shader stage (`vs`/`fs`) and that neither the
argument-buffer nor the direct-cbuf shape was found, rather than
silently choosing a fallback binding or continuing with an unbound
cbuf parameter. Instantiates R-HARN-5.2/5.3. This requirement is
already met by current source (`find_direct_cbuf_slot` returning
`None` for both `vsConsts`/`ffpVs` or both `psConsts`/`ffpPs` raises
`SystemExit("mini replay cbuf rewrite found no buffer(30) argbuf and
no direct constant binding for <stage> stage")`); it is stated as a
requirement here because R-HARN-5.3's own rationale (defect 1) is what
happens when a shape's expectation is enforced only in code with no
declared contract to check a future engine change against — the fix
this domain must preserve is exactly "declare both shapes, fail by
name on a third."

**R-HARN-REPLAY-5.3** A future engine change that emits a third
cbuf-binding shape (neither buffer(30) argbuf nor buffer(0)/buffer(3)
direct-cbuf) is a hard failure for this domain until an explicit
branch recognizing that shape is added to `transform_msl` and the new
shape is added to R-HARN-REPLAY-5.1's declared set and this domain's
`spec.md` engine-shape table. A change must not silently widen an
existing regex to also match the new shape without updating both
documents — that reintroduces defect 1's exact failure mode (a
dependency enforced only in source, undeclared anywhere else).
Instantiates R-HARN-5.1/5.3.

---

## 3. Unsupported Attachment Formats Are Failures, Not Fallbacks

**R-HARN-REPLAY-2.1** This domain's color- and depth-pixel-format
resolvers each declare the finite `core::Format` set they accept and
fail, naming the unrecognized `format_value`, for any input outside
that set — they must not return a plausible-looking Metal pixel format
for a `core::Format` they do not recognize. Instantiates R-HARN-2.1/
2.2. Rationale (defect 3): `color_pixel_format()` recognizes
`core::Format` values 1-4 and returns `MTLPixelFormatRGBA8Unorm` for
every other value, including R32F (`core::Format` 16), which row
`60/0` actually uses; the replay rendered into a wrong-format
attachment with no diagnostic that the format was unrecognized. This
requirement covers `depth_pixel_format()` on the same terms:
recognizing `core::Format` values `{40, 41, 49}` and `{42, 46}` and
returning `MTLPixelFormatDepth32Float` for anything else has the
identical unnamed-fallback shape as the color resolver and is not
exempt from this requirement merely because no wild run has yet
exercised an unrecognized depth format.

**R-HARN-REPLAY-2.2** If a placeholder attachment format is ever
wanted for a genuinely unsupported `core::Format` — for example, to
let a replay proceed far enough to inspect geometry shape when the
exact color format cannot yet be represented — that fallback is
reachable only behind a named opt-in flag (e.g. an explicit
`--allow-unmapped-color-format`), never as the resolver's default
return path for an unrecognized value. Instantiates R-HARN-2.3.

**R-HARN-REPLAY-2.3** The color and depth pixel formats this domain
actually resolved for a replay run are recorded in
`mini-replay-summary.json` (or the artifact envelope once adopted for
this domain, parent spec.md §3) so a downstream reader can see which
format was selected without re-deriving it from the manifest's raw
`format` integer. Instantiates R-HARN-2.4.

---

## 4. Output Validity Self-Assertion

**R-HARN-REPLAY-3.1** Before the standalone replay binary prints its
`mini replay draws=<N> repeat=<R>` success line and exits 0, it asserts
that the color output it wrote (when `--color-output`/
`DXMT9_MINI_REPLAY_COLOR_OUTPUT_PATH` requested one) is non-degenerate
— not a single distinct pixel value across the whole image — and exits
non-zero if the assertion fails or cannot be executed. Instantiates
R-HARN-3.1/3.3. Rationale (defect 4): every one of four replay lanes
in the vertex-remap experiment printed `mini replay draws=229 repeat=1`
and exited 0 while each wrote a 1024×768 PPM containing exactly one
distinct pixel value (fully black). Nothing in the current binary
asserts the image carries content before declaring success — this
requirement exists precisely because the code has no such check today.

**R-HARN-REPLAY-3.2** A `mini replay draws=<N> repeat=<R>` line plus
exit code 0 must not, by itself, be treated by any downstream consumer
as evidence that the rendered image is valid; only the validity result
required by R-HARN-REPLAY-3.1 (recorded per parent spec.md §3's
`validity` envelope field once adopted) is that evidence. Instantiates
R-HARN-3.2/3.4 — this is the replay-domain instance of the parent
rationale that four identical black PPMs would also produce four
identical digests, which a digest-only or exit-code-only gate would
wrongly read as agreement.

**R-HARN-REPLAY-3.3** For as long as R-HARN-REPLAY-3.1's assertion
remains unimplemented, this domain's own `spec.md` Known Deviations
section states that plainly — an unqualified reader of this domain's
docs must not come away believing `exit 0` currently means "valid
image." Instantiates R-HARN-3.3.

---

## 5. In-Payload Offsets Are Computed From Declared Fields

**R-HARN-REPLAY-4.1** Every in-payload byte offset this domain
computes for a dumped vertex stream payload — stream0 or any of
streams 1-15 — is derived as `offset - start_byte` from that stream's
own declared `offset` and `start_byte` fields, per the slice rule
stated once, authoritatively, in
`specs/experiments/harness/probe/spec.md` §5. This domain's own
`spec.md` cites that section rather than restating the slice rule.
Instantiates R-HARN-4.2/4.3. Rationale (defect 2, fixed in
`12348666`): `run_3dmark05_mini_replay.py` previously read
`stream0_offset` directly as a payload-relative offset instead of
computing it from the declared slice origin, producing negative slot
capacities; `resolve_stream_payload_offset` is the fix this
requirement pins in place.

**R-HARN-REPLAY-4.2** The derivation in R-HARN-REPLAY-4.1 applies
uniformly to every stream index present in a manifest draw's
`geometry.streams` list, not only stream0. A change that special-cases
stream0's offset derivation while leaving streams 1-15 to read their
`offset` field directly as payload-relative reintroduces defect 2 for
the non-primary streams, even though stream0 itself would still be
correct. Instantiates R-HARN-4.3/4.4.

**R-HARN-REPLAY-4.3** This domain must not assume, for any stream on
any future manifest, that `start_byte == offset` holds; every draw's
`streams[].start_byte` is read from the manifest itself, never
inferred from an observed equality in past dumps. Instantiates
R-HARN-4.3/4.4 (parent spec.md's stated rationale: the equality
observed in every currently captured `.meta` file is a producer
coincidence of today's slicing choice, not a contract a consumer may
rely on).

---

## 6. Diagnostic Paths Are Covered by the Mode Table and Kept Compiling

**R-HARN-REPLAY-6.1** Every flag of `run_3dmark05_mini_replay.py` that
alters this domain's output — the generated Objective-C++ replay
source, the compiled binary's runtime behavior, or an artifact this
domain writes — appears in this domain's own `spec.md` mode table,
whether the flag is a routine shape control (`--width`, `--height`,
`--depth-clear`) or a diagnostic bisection probe (`--force-fragment-
color`, `--force-fragment-primitive-id`, `--primitive-order`,
`--vertex-order`, `--draw-order`, `--trim-vsout-to-fs-reads`).
Instantiates R-HARN-6.2.

**R-HARN-REPLAY-6.2** A diagnostic flag that does not compile the
generated replay source is a contract violation, not an acceptable
degraded state, because a diagnostic path exists specifically to be
exercised once the primary replay path has already failed to produce
a useful image. Instantiates R-HARN-6.1/6.3. Rationale (defect 5):
`--force-fragment-color`'s `force_fragment_color_source` replaces the
body of `dxmt9_fs` with a bare `return float4(1.0f, 0.0f, 1.0f, 1.0f);`
without checking the function's declared return type; for a captured
fixed-function-pipeline (FFP) shader, that declared return type is
`FfpFsOut`, a struct, not `float4` (dxmt9's own FFP shader emitter,
`src/dxmt9/dxmt9_ffp_shaders.cpp`, always declares
`fragment FfpFsOut dxmt9_fs(...)`), so the rewritten source fails to
compile. The one diagnostic flag meant to bisect a rendering failure
between geometry and fragment stages is itself unusable exactly when
it would be needed to investigate defect 4.

**R-HARN-REPLAY-6.3** This domain declares which index width(s) its
replay binary supports. Today it declares exactly 16-bit indices: the
generated `drawIndexedPrimitives:` call is emitted with a literal
`indexType:MTLIndexTypeUInt16`, with no branch for a 32-bit source
index buffer. A manifest draw whose original D3D9 index format is
32-bit must be rejected with a diagnostic naming the unsupported width
rather than replayed as if it were 16-bit — this is a requirement on
intended behavior, not a description of an existing check, since
nothing in current source inspects the manifest's `index_type` field
before emitting the hardcoded `MTLIndexTypeUInt16` call.
Instantiates R-HARN-2.2 (an out-of-classification index width must not
be silently reinterpreted) and R-HARN-5.1 (the accepted index-width
set must be declared, the same way the accepted cbuf-binding shapes
are).
