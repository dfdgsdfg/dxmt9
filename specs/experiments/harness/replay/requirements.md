---
type: "Spec Requirements"
title: "Harness Replay Requirements — Unified Render Replay"
description: "Requirements for draw-slice, frame-tape, and sequence-tape capture and replay profiles."
tags: [specs, experiments, harness, replay, requirements]
---

# Harness Replay Requirements — Unified Render Replay

This is a domain document under `specs/experiments/harness/`. It
instantiates the `R-HARN-*` requirement groups in
`specs/experiments/harness/requirements.md` for the `replay` domain
named in `specs/experiments/harness/spec.md` §1. The domain owns three
profiles under one contract: the implemented standalone `draw-slice`, the
bounded production-backed `frame-tape`, and the bounded two-interval
`sequence-tape` profile. The broader profiles add canonical backend command capture, a consistent
state/resource checkpoint, and production-path replay; they do not weaken
the existing mini replay's validity and execution-proof contracts.
Requirement IDs in this file use the prefix `R-HARN-REPLAY-`.

**The implemented `draw-slice` profile renders a valid image as of
2026-07-28.** It did not when these requirements were written; the six
defects that blocked it are fixed, and this domain's own `spec.md` §7
records each one with its fix from the implementation side. A seventh defect, found on
2026-07-29, was not a rendering fault at all: the domain rendered
correctly and reported a pixel-identical result that was read as
proof of a shader-translator change the replay had never executed
(§4, R-HARN-REPLAY-3.4). Its containment half is answered by
`coverage`; its execution half is answered by R-HARN-REPLAY-3.8's
mutation check, which generalizes the hand technique that found the
defect. Every requirement below still states the contract the domain
must satisfy rather than asserting the domain meets it. Where today's
source violates a requirement, this file says so in the requirement's
own rationale, and `specs/experiments/gap.md` tracks the shortfall.

---

## 1. Scope and Stage Participation

**R-HARN-REPLAY-1.1** The replay domain comprises the implemented scripts
the parent domain map (`specs/experiments/harness/spec.md` §1) assigns to
its `draw-slice` profile:
`scripts/tools/build_3dmark05_mini_replay_manifest.py`,
`scripts/tools/plan_3dmark05_mini_replay.py`, and
`scripts/tools/run_3dmark05_mini_replay.py`. The same domain owns the
implemented bounded `frame-tape`/`sequence-tape` validators, tools, and
production-path replayer, plus the frame-only production capture owner,
described in this document's §7 and its `spec.md`
§0/§8. Consuming a `join`- or `reduce`-domain artifact does not make replay
participate in `external-join` or `log-reduce`; its consumer stage remains
`offline-replay`. Instantiates R-HARN-1.1 and R-HARN-7.1.

**R-HARN-REPLAY-1.2** The implemented `draw-slice` profile is a read-only consumer of the
`dump-extract → offline-replay` boundary (parent spec.md §2): it must
never write into a `probe`-domain trace directory's `analysis/geometry/`,
`analysis/shaders/`, or `analysis/frame<N>-depth.bin` paths, only read
from them. A change that has this domain re-derive or re-dump geometry
or shader payloads itself, instead of consuming what `dump-extract`
already wrote, duplicates a responsibility the parent domain map
assigns to `probe`. This does not prohibit the frame-tape producer,
which writes a different sealed bundle during `run-capture`/`dump-extract`
and is governed by R-HARN-REPLAY-7.2–7.5. Instantiates R-HARN-1.1
(parent spec.md §1, "Why the domain axis is harness families, not stages").

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

**R-HARN-REPLAY-5.4** Replay-owned constant and volatile bindings must
satisfy the dumped MSL argument ABI before a draw is submitted. File-backed
constant payloads are zero-padded to the next 16-byte boundary so a captured
logical payload such as the 2632-byte `FfpVsConsts` is backed by the 2640-byte
MSL struct extent. Replay `DrawVolatile` includes the production
`streamInstanceDivisors[16]` tail even when every divisor is zero. A Metal
argument-length validation failure is a harness failure and must not be
reported as a valid all-black pixel oracle. Instantiates R-HARN-5.1/5.3.

The replay rasterizer sets clockwise front-face winding before applying the
captured D3D9 cull disposition. Relying on Metal's default winding changes the
meaning of `D3DCULL_CW` and can cull the entire captured draw while leaving a
valid command buffer.

The manifest's `state.cull` is the encoder observer's already-converted
`WMTCullMode` (`0=None`, `1=Front`, `2=Back`), not the D3D9 API ordinal. Replay
must consume that encoding directly and must not reinterpret it as
`D3DCULL_NONE/CW/CCW` (`1/2/3`).

---

## 3. Unsupported Attachment Formats Are Failures, Not Fallbacks

**R-HARN-REPLAY-2.1** This domain's color- and depth-pixel-format
resolvers each declare the finite `core::Format` set they accept and
fail, naming the unrecognized `format_value`, for any input outside
that set — they must not return a plausible-looking Metal pixel format
for a `core::Format` they do not recognize. Instantiates R-HARN-2.1/
2.2. The current declared color set is `{1, 2, 3, 4, 11}`; value 11
is A16B16G16R16F and resolves to `MTLPixelFormatRGBA16Float` for the
3DMark06 HDR mini-replay lane. Rationale (defect 3): the old
`color_pixel_format()` recognized only `core::Format` values 1-4 and
returned `MTLPixelFormatRGBA8Unorm` for every other value, including
R32F (`core::Format` 16), which row
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
actually resolves for a replay run must be recorded — in
`mini-replay-summary.json` today, or the artifact envelope once
adopted for this domain (parent spec.md §3) — so a downstream reader
can see which format was selected
without re-deriving it from the manifest's raw `format` integer. The
record must name each resolved `MTLPixelFormat` beside the
`core::Format` ordinal it was resolved from, so the mapping itself is
auditable and not only its result, and must state when a format came
from the legacy no-`attachments` default rather than from a declared
`core::Format`. The generated program and the recorded value must
come from one resolution, so the artifact can never name a format the
replay did not render with. Instantiates R-HARN-2.4.

The color readback layout must be derived from that same resolved
format. In particular, A16B16G16R16F uses eight bytes per pixel and
must be decoded from RGBA16Float before writing the 8-bit RGB oracle;
using the legacy four-byte row pitch is a hard harness failure, not a
valid all-black result.

---

## 4. Output Validity Self-Assertion

**R-HARN-REPLAY-3.1** A `run_3dmark05_mini_replay.py --run` invocation
that requested a color output (`--color-output`/
`DXMT9_MINI_REPLAY_COLOR_OUTPUT_PATH`) must read that image back, must
assert it is non-degenerate — more than one distinct RGB triple across
the whole image — must record the measured distinct-value and
non-background pixel counts in `mini-replay-summary.json` under
`validity`, and must exit non-zero when the assertion fails or the
image is missing, truncated, or otherwise unreadable. The threshold is
exactly the degenerate case; this requirement must not be extended to a
percentage-coverage gate, for which the domain has no evidence.
Instantiates R-HARN-3.1/3.3. Rationale (defect 4): every one of four
replay lanes in the vertex-remap experiment printed `mini replay
draws=229 repeat=1` and exited 0 while each wrote a 1024×768 PPM
containing exactly one distinct pixel value (fully black). The
generated program applies the same threshold to the same image
(R-HARN-REPLAY-3.7), so the guarantee does not depend on which of the
two entry points a maintainer used.

**R-HARN-REPLAY-3.2** A `mini replay draws=<N> repeat=<R>` line plus
exit code 0 must not, by itself, be treated by any downstream consumer
as evidence that the rendered image is valid; only the validity result
required by R-HARN-REPLAY-3.1 (recorded per parent spec.md §3's
`validity` envelope field once adopted) is that evidence. Instantiates
R-HARN-3.2/3.4 — this is the replay-domain instance of the parent
rationale that four identical black PPMs would also produce four
identical digests, which a digest-only or exit-code-only gate would
wrongly read as agreement.

**R-HARN-REPLAY-3.3** This domain's own `spec.md` §7 must state
plainly what the current code does and does not check — an unqualified
reader of this domain's docs must not come away believing `exit 0`
means "valid image" beyond what R-HARN-REPLAY-3.1's assertion
actually establishes. Instantiates R-HARN-3.3. As of 2026-07-29 the
harness does perform that assertion; before then it did not, and the
2026-07-28 defect-4 fix had removed the cause of one degenerate render
without adding any check that would catch the next one.

**R-HARN-REPLAY-3.4** A pixel comparison produced by this domain
certifies only the draw window the replayed manifest covers — the
draws of a single encoder from a single captured frame — and must not
be used as the correctness oracle for a shader-translator or codegen
change unless the changed code path is separately shown to execute
within that window. To make that judgement possible,
`mini-replay-summary.json` must carry a `coverage` block naming the
manifest rows and encoders replayed, the replayed draw count, the
shader-variant count, and the number of replayed draws per
`shaders.vs_hash` and per `shaders.ps_hash`, with the per-hash counts
summing to the replayed draw count. Instantiates R-HARN-3.2/3.4.
Rationale (defect 7): a translator change was validated by replaying
GT1 frame60 encoder 1 with the new emission applied by hand, produced
786,432 of 786,432 pixels identical to baseline, and was read as
correct; under the real runtime the same emission made every skinned
character in 3DMark05 GT1 disappear. Instrumenting the eight affected
vertex shaders so that taking the branch under test collapses the
vertex position still produced a byte-identical image — across 229
draws and roughly 795,000 vertex invocations that branch never
executed. The harness had reported nothing about which shaders the
replay contained, so nothing distinguished "the change is correct"
from "the change was never reached."

**R-HARN-REPLAY-3.5** The non-degeneracy result required by
R-HARN-REPLAY-3.1 and the coverage description required by
R-HARN-REPLAY-3.4 are independent claims and must be recorded,
reported, and consumed as independent claims. A `validity` pass must
never be presented or read as evidence that the replay exercised any
particular draw, shader, or code path, and a `coverage` block must
never be presented or read as evidence that the rendered image is
correct. Instantiates R-HARN-3.2/3.4. Rationale: defect 7's image was
non-degenerate — 12,231 distinct RGB values over 784,476 of 786,432
non-background pixels — and the shaders under test contributed only
15,134 pixels, 1.9% of the frame, so a validity check alone would have
passed the very run that produced the false negative.

**R-HARN-REPLAY-3.6** The `validity` field required by
R-HARN-REPLAY-3.1 must be present in `mini-replay-summary.json` for
every invocation, including invocations that produced no image to
assert on; in that case it must state that the assertion did not run
and why, and must not carry a degeneracy verdict or measured pixel
counts. A consumer must never have to infer a pass from an absent
field. Instantiates R-HARN-3.2/3.3.

**R-HARN-REPLAY-3.7** The replay program this domain generates must
apply R-HARN-REPLAY-3.1's threshold to the image it just wrote: it
must print the measured distinct-value count on its own summary line,
and must exit non-zero — naming the output path, the measured count,
and the threshold — when the count is below it. The threshold must be
emitted into the generated source from the same single constant the
wrapper assertion uses, so the two can never disagree. This closes the
guarantee for a maintainer who invokes the compiled
`dxmt9-3dmark05-mini-replay` executable directly rather than through
the wrapper, which is the only documented path and was therefore the
only guarded one. The wrapper's assertion is not made redundant by
this and must remain: it is the only side that can also record
`validity` in the artifact (R-HARN-REPLAY-3.6) and diagnose the
missing, truncated, or otherwise unreadable image. The generated
program's degenerate exit status must therefore be distinguishable
from a replay failure, and the wrapper must carry it through to its
own assertion rather than surfacing it as an opaque subprocess error.
Instantiates R-HARN-3.1/3.3.

**R-HARN-REPLAY-3.8** This domain must provide an opt-in check that
answers whether the replay *executes* a named construct, distinct from
the containment question R-HARN-REPLAY-3.4's `coverage` block answers.
Given a source-text substitution over the shader sources it generates,
the harness must replay both the unmutated and the mutated sources and
must exit non-zero when the two images are identical, because an
identical image proves the mutated construct was never executed by
this replay. It must report, in `mini-replay-summary.json` under
`execution_proof`, the substitution, how many generated shader sources
were scanned and mutated, how many sites were substituted, the
differing-pixel count, and a named verdict.

Two failing outcomes must be reported as distinct verdicts and
distinct messages, never collapsed:

- **not present** — the substitution matched no site. This is not an
  execution verdict; the construct is absent from the replayed shaders
  or the pattern is wrong, and the run establishes nothing about
  whether the construct would execute.
- **present but not executed** — the substitution matched sites and
  the images are identical. The construct is present and this replay
  never reaches it.

Instantiates R-HARN-3.2/3.4. Rationale (defect 7): the technique that
found the defect was exactly this substitution, applied by hand —
collapsing the vertex position on the branch under test produced a
byte-identical image, with an unconditional-marker control proving the
instrumentation was live. Nothing in the harness supported it, so the
judgement depended on a maintainer thinking to do it. A harness that
reports success without establishing that it ran the code under test
is worse than no harness, because it is believed.

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
`--depth-clear`) or a diagnostic bisection probe
(`--force-fragment-color`, `--force-fragment-primitive-id`,
`--primitive-order`, `--vertex-order`, `--draw-order`,
`--trim-vsout-to-fs-reads`).
Instantiates R-HARN-6.2.

**R-HARN-REPLAY-6.2** A diagnostic flag that does not compile the
generated replay source is a contract violation, not an acceptable
degraded state, because a diagnostic path exists specifically to be
exercised once the primary replay path has already failed to produce
a useful image. Instantiates R-HARN-6.1/6.3. Rationale (defect 5):
`--force-fragment-color`'s `force_fragment_color_source` replaces the
body of `dxmt9_fs` with a bare `return float4(1.0f, 0.0f, 1.0f, 1.0f);`
without checking the function's declared return type. dxmt9 emits
`dxmt9_fs` with one of three declared return shapes depending on which
emitter produced it, not two: every real translated per-draw pixel
shader declares `FSOut`, a struct, because
`src/dxmt9/dxmt9_shader_metal_ir.cpp` hardcodes
`usesFragmentOutStruct = true` unconditionally
(`dxmt9_shader_metal_ir.cpp:2576`) — `FSOut` is therefore the dominant
real shape for exactly the per-draw shaders this harness dumps and
replays, not a secondary case; every fixed-function-pipeline (FFP)
shader declares a different struct, `FfpFsOut`
(`src/dxmt9/dxmt9_ffp_shaders.cpp:1109,1129,1150,1172,1181`); only
dxmt9's internal blit/gamma-apply/debug-fill utility shaders
(`src/dxmt9/dxmt9_shader_sources.cpp:124,152,178`) declare a bare
`float4`, and those utility shaders are never the per-draw shader this
harness dumps or replays. The rewritten body therefore fails to
compile against essentially every real captured shader — translated
and FFP alike — not only the FFP subset. The one diagnostic flag meant
to bisect a rendering failure between geometry and fragment stages is
itself unusable exactly when it would be needed to investigate
defect 4.

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

---

## 7. Unified Replay Profiles and Render Tape

**R-HARN-REPLAY-7.1** Every replay artifact declares exactly one profile:
`draw-slice`, `frame-tape`, or `sequence-tape`. `draw-slice` is the existing
single-encoder diagnostic projection. `frame-tape` contains one complete
Present interval. `sequence-tape` contains consecutive complete Present
intervals and may additionally declare a wall-clock selection duration. A
consumer must reject an unknown profile and must not infer a stronger scope
from an artifact's size or filename. Instantiates R-HARN-7.1.

**R-HARN-REPLAY-7.2** A full tape bundle is content-addressed and pointer-free.
Its manifest records the tape schema and version, profile, producer revision,
canonical wire ABI hash, platform/capability provenance, event and Present
ordinal ranges, every component digest, and the artifact-envelope fields from
the parent spec. Variable payloads are stored as immutable digest-named blobs;
events refer to blob digests and generation-qualified object identities, never
process addresses or live COM/Objective-C objects. Instantiates R-HARN-4.1,
R-HARN-7.2, and R-HARN-7.5.

**R-HARN-REPLAY-7.3** A full tape begins with exactly one `BootstrapState`.
It declares a versioned typed-default baseline for every backend-visible D3D9
state category, an exact all-known-category completeness mask, and one or more
canonical D9C v2 `APPLY_STATE` chunks using the existing `FULL_SNAPSHOT`
section grammar as overlays. The validator accepts only the one baseline
profile version it implements, requires `FULL_SNAPSHOT` on every overlay
record, rejects missing or unknown category bits, and rejects any bootstrap
chunk containing Draw, Clear, Present, resource, query, readback, or other
ordering records. Sparse overlay absence means the declared baseline default,
never inheritance from a live server shadow. Object descriptors,
shader/declaration bytes, and initial subresource contents remain
generation-qualified `ObjectDefine` and digest-backed `ResourceMutation`
events; shader and declaration definitions require an immutable verified
payload digest. Before any replay sink callback, validation builds a bounded,
value-owned ObjectDefine index and closes every handle in every bootstrap
overlay against an exact usable definition; later command chunks are closed
against the ordered live-generation set. The bootstrap replay callback is
journal-only and carries an explicit deferred-provider mode, so a provider
cannot apply state before definitions have been journaled. Instantiates
R-HARN-7.2/7.3.

**R-HARN-REPLAY-7.4** The v2 event journal has exactly these semantic classes:
`BootstrapState`, `ObjectDefine`/`ObjectDestroy`, digest-backed
`ResourceMutation`, byte-exact canonical `CommandChunk`, typed
`OrderedControl`, and terminal `PresentComplete`. Clear, Present,
UpdateTexture, UpdateSurface, QueryIssue, and Readback already belong to D9C v2
chunks and must not be duplicated as controls. `OrderedControl` is restricted
to calls that actually bypass chunks: QueryGetData, CPU-read observation,
flush/wait, Reset, and device-lost observation. It records the returned value,
typed disposition, and completion waterline. Every event carries a monotone
ordinal and every object reference carries kind, object ID, and generation.
Unknown kinds/dispositions, stale generations, missing or unverified blobs,
range overflow, completion regression, or duplicate representation are hard
validation failures. Instantiates R-HARN-2.1, R-HARN-7.2, and R-HARN-7.5.

Completeness includes resource-owned state and direct state calls, not only
the device-shadow category mask and resource content bytes. A capture must
either represent an effective mutation in replay order or reject the capture
with a named reason. In particular, an unrepresented effective managed-texture
LOD change must invalidate capture tracking, including before arming; a later
reset to zero must not silently erase that missing history. Clamped no-ops and
non-managed SetLOD calls do not change this state and must not reject capture.
Capture rejection must not change the application's SetLOD return value or
backend state update. Existing bundles without this evidence cannot certify
that resource-owned state stayed at its default.

The same completeness rule applies to explicit AUTOGEN mip generation outside
the recorded command path. Recording it must preserve the existing public
completion/failure contract and bind a canonical command to an authenticated
Unix source/completion receipt, without executing the operation twice. Until
that binding exists, capture tracking must reject a hidden-chain explicit
generation with a named diagnostic, including before arm. Capture rejection
must not suppress or defer the application's existing synchronous operation.

The v2 state contract additionally treats the 3-channel, 256-entry gamma
ramp as explicit state. `BootstrapState` carries the initial ramp as exactly
1536 owned bytes with a verified SHA-256 digest and recomputed identity flag;
an in-interval `SetGammaRamp` is represented by `OrderedControl::GammaRampSet`
with the same exact payload and monotone completion ordinal. A gamma mutation
must precede the interval's Present-bearing chunk; the provider applies both
forms before subsequent command work. The compact replay path is not allowed
to silently drop these events and therefore falls through to the general
typed provider when either form is present. Mismatched size, digest, identity,
ordering, or unsupported control kinds fail closed.
The gamma-bearing 64-byte bootstrap header was introduced in baseline profile
version 2. Profile 3 retains that layout and adds the ordered paged checkpoint
contract in R-HARN-REPLAY-9.7.
Pre-gamma profile-1 artifacts are Retired and fail closed; they are not
reinterpreted as profile 2 or used as production replay evidence.

For `frame-tape`, `PresentComplete` is unique and last. For `sequence-tape`,
one `PresentComplete` closes each interval and only the final completion is
last. Every completion names the ordinal of its interval's one Present-bearing
command event, records a monotone completion value and typed oracle attachment
identities, and explicitly distinguishes a captured SHA-256 oracle from
`not-captured`. A successful Reset terminates/aborts the frame world and cannot
be followed by successful `PresentComplete`; a failed Reset remains an ordered
observation.

**R-HARN-REPLAY-7.5** Capture sealing must prove resource closure and checkpoint
consistency before reporting success. Every referenced object exists at the
event that uses it; every read byte is supplied by the checkpoint or an earlier
journaled mutation; no resource generation is reused while an older reference
remains reachable; and each selected Present interval is complete. Capture
that cannot establish these predicates is invalid rather than partial full-tape
evidence. Instantiates R-HARN-3.1, R-HARN-4.4, and R-HARN-7.3.

Each resource `ObjectDefine` may declare a non-zero expected total byte extent
and subresource count; the two fields must be both zero or both non-zero. For
each identity independently, validation must close every non-zero expectation
against unique, zero-offset initial `ResourceMutation` subresources whose byte
sizes sum exactly to the declared extent and whose count matches exactly.
Unrelated events do not close another identity's expectation. A matching seed
mutation may arrive after unrelated commands, but only before that identity's
first command, identity-bearing control, or destroy use; once that identity is
complete, later mutations are ordinary interval traffic. The final journal
still requires every declared expectation to be complete. The ordered
live-generation registry remains identity/lifetime-only; seed accounting is a
separate bounded value table and cannot be satisfied by a mutation after the
identity's first use.

**R-HARN-REPLAY-7.6** Reference full-tape replay reconstructs objects and imports
wire chunks through production validation, queue, lifetime, and provider code.
Recorded Present events map to an explicit replay surface boundary: the
reference mode uses an offscreen target and records attachment hashes/readback;
an optional windowed mode may present to a drawable but is not the deterministic
oracle. Replaying by translating events into a second hand-written renderer is
not reference evidence. Instantiates R-HARN-7.4.

**R-HARN-REPLAY-7.7** Replay reports three independent evidence blocks:
`validity` for bundle and output non-degeneracy, `coverage` for exact event,
source, object, shader, draw, and Present containment, and `conservation` for
accepted/rejected counts, ordinal continuity, completion, and output hashes.
Benchmark timing is a fourth, optional block and must declare reset, warm-up,
repeat, and sampling policy. None of the four blocks substitutes for another.
Instantiates R-HARN-3.2/3.4 and R-HARN-7.5.

**R-HARN-REPLAY-7.8** The first implementation milestone is a complete
`frame-tape` identity replay. `sequence-tape` may reuse the same schema only
after two consecutive Present intervals replay with correct resource mutation,
completion, and output conservation. A nominal ten-second workload is a
selection policy over complete intervals, not a separate ad-hoc dump format.
Rolling eviction, random seek, and mid-interval checkpoints are outside the
initial contract and must not be implied by the profile name.

**R-HARN-REPLAY-7.9** `draw-slice` remains a supported diagnostic projection
with its current manifest, generated standalone Metal renderer, coverage, and
execution-proof rules. Until a conversion path is implemented, it is not a
view extracted from `frame-tape`; its result certifies only the selected draw
window. When conversion is added, both paths must share object/shader/blob
identity and must demonstrate byte- or semantic-equivalent projection before
the legacy manifest producer can be retired.

**R-HARN-REPLAY-7.10** Render Tape captures dxmt9's effective backend-semantic
stream. It does not reproduce exact application call timing, PE-side setter
frequency, state calls coalesced before draw-delta construction, Wine scheduling,
or producer/replay overlap. Experiments about those observables require a
separate producer trace. This limitation must appear in every full-tape
artifact's scope block. Instantiates R-HARN-7.6.

**R-HARN-REPLAY-7.11** The PE capture owner is opt-in and default-off. While the
gate is enabled, a device-owned registry tracks generation-qualified live
texture/surface/buffer/shader/declaration/query identities, exact value-owned
descriptors and immutable payloads, and supported CPU-owned resource contents
from creation onward. At the first successful Present boundary the device-owned
producer builds a canonical `APPLY_STATE|FULL_SNAPSHOT` from the actual PE
shadow, emits the registry as initial object/blob/mutation seeds with expected
extent/count closure, and arms the following Present interval. An injected
bootstrap producer may replace this owner only as an explicit test override.
The optional `DXMT9_RENDER_TAPE_PROFILE=sequence-tape` selector extends this
bounded owner to exactly two consecutive standard Present intervals: the first
completion remains journaled but unsealed, one complete digest-backed mutation
must follow it, and only the second completion may validate and publish.
The capture owner copies each successfully committed canonical D9C v2 chunk
once; live create/destroy, writable-lock mutation, and true chunk-bypass control
call sites feed the same owner without duplicating chunkized operations. It
derives blob digests from copied bytes and publishes events and verified blob
bytes as one value-owned bundle. Missing state or initial content, stale
generation, descriptor/payload mismatch, unsupported lock/content layout,
failed bridge commit, capacity failure, terminal Reset or device-lost control,
failed validation, missing/rejecting publisher, or producer failure aborts the
interval without exposing a partial artifact. The producer must not recover
bytes by retaining or dereferencing stale COM pointers.

Abort must release session-owned blob payloads, object descriptors, catalogue,
event builder and unpublished artifact storage before a retry can build its
next snapshot. Live byte/count accounting must describe retained storage, not
report zero while old payloads remain owned. Failure attribution may preserve
pre-release extents as value-only diagnostics; it must not keep those payloads
alive. Resetting the builder must preserve the selected capture profile.

Capture mutation failure evidence must bind the exact resource identity,
subresource, byte offset and incoming extent to the rejected stage. Blob
registration failures must be distinguishable from mutation-event validation,
event reservation and append failures. Explicit capacity denials and typed
exceptions must remain distinguishable, with pre-abort extents retained as
allocation-free values. These diagnostics must not relax admission, expose
partial publication, or retain borrowed payload/exception text.

When the bootstrap producer already owns its blob payloads, session admission
must transfer those payloads rather than create another full-size copy.
Transferred bytes still require exact duplicate detection, budget admission,
and recomputed digests; caller-supplied identity is not validation evidence.
Borrowed-input admission retains copying semantics. Both routes must produce
the same canonical events and blob catalogue, with no partial publication on
failure. Digest computation must use bounded scratch independent of input
extent. Arm snapshots needed by later JIT materialization remain owned until
their existing lifetime boundary; consuming bootstrap seeds must not drain them.

For an exact current-arm identity selected for bootstrap or JIT admission,
snapshot-owned payloads may instead transfer to the staged seed/session without
copying. Unselected snapshots and mutable durable-registry storage must remain
untouched. Snapshot identity, arm ordinal and descriptor authority survive
payload transfer, including completed-EVENT polling authority. The transfer
must either finish that identity's admission or abort the whole capture
interval; a partially drained snapshot must never authorize same-interval
retry or completeness. Repeated references to an admitted identity reuse its
session-owned bytes. A new arm reconstructs snapshots after abort. The actual
transfer predicate and failure transitions require bounded native coverage;
pointer conservation proves ownership behavior, not byte authenticity.

Durable capture CPU shadows have a different lifetime: subsequent partial
application mutations still require their previous contents. They must not
surrender their sole payload to session admission. A capture-local immutable
payload owner may share the exact backing between durable shadow, session and
publication, subject to unchanged digest, extent, deduplication and budgets.
Borrowed span inputs still require ownership before return. Shared ownership
must not substitute for byte authentication or cross the PE/Unix wire ABI.

Durable writes must use an explicit transactional mutation boundary. A full
replacement derives from the actual incoming bytes; a partial write detaches
when an immutable reader still owns the prior version. Failed validation or
allocation must not modify either published bytes or the committed shadow.
UpdateTexture must validate/stage all subresources before committing the
destination versions. Abort releases session leases without destroying the
registry's authoritative contents. Ordinary mutable data access must not
silently detach or expose published storage for modification. Snapshot-vector
transfer and EVENT descriptor authority retain their existing contracts.

Native coverage must bind buffer, block, linear and volume writes, atomic
UpdateTexture replacement, nonuniform A-to-B partial updates with retained A,
abort/rearm and allocation failures. Boundary-only ownership accounting must
distinguish logical bytes from unique retained backing allocations; unchanged
budget workload capture remains required to claim practical memory benefit.

GPU checkpoint bytes and the durable CPU mutation base are distinct authorities.
A successful GPU readback must not overwrite the CPU base merely to reuse its
allocation. In particular, a managed texture may retain CPU version A while a
GPU surface operation produces B; a later partial managed unlock can upload the
whole CPU level A-plus-patch. Substituting B as the CPU base would reconstruct
different bytes. Storage optimizations must preserve both required versions and
their authority, not silently turn supported partial updates into unknown seeds.

The capture owner must also enforce an overflow-safe total owned-blob byte
budget before hashing or copying a new blob. The capture-only production
default is 256 MiB (268,435,456 bytes). `DXMT9_RENDER_TAPE_MAX_BLOB_BYTES`
overrides it in decimal bytes only while capture is enabled; unset, invalid, or
zero resolves to the default, and a valid value above the hard 1 GiB
(1,073,741,824-byte) ceiling resolves to that ceiling. Exact duplicate bytes
may be admitted without a second charge; an over-budget blob is rejected as a
capture capacity failure, and this policy must not relax digest, descriptor,
generation, event, or replay-grammar validation. Capture-off must not read the
override or change the normal recorder/bridge path. GT2 r7 and r8 measurements
are lower bounds on the required owned bytes, not a proof that a complete GT2
bundle fits; that bundle-capacity claim remains unproven.

Event and unique-blob cardinality limits default independently to 4,096.
Capture-only `DXMT9_RENDER_TAPE_MAX_EVENTS` and
`DXMT9_RENDER_TAPE_MAX_BLOB_ENTRIES` may override those counts in decimal;
unset, invalid, zero or integer-overflow text resolves to the default, and a
valid value above 65,536 resolves to that hard ceiling. The capture owner must
also clamp directly supplied counts to the ceiling. These overrides must not
increase the 64 MiB event-byte budget or 1 GiB hard blob-byte budget, truncate
events, change original identities/cuts, or select a different renderer path.
Capture-off must not read them. Capacity rejection remains transactional and
must retain its first-failure evidence.

Frame-tape bootstrap materialization is an exact closure, not an all-live
snapshot. Its roots are the generation-qualified handles in the freshly
validated bootstrap overlay plus the required Present output; versioned
descriptor dependencies, including a texture-derived Surface's exact parent
texture/subresource owner, are added recursively. Unreferenced live objects,
including incomplete ones, are omitted. A referenced missing,
stale-generation, incomplete, or missing dependency seed rejects the tape with
a typed first reason. After arm, a complete pre-arm object omitted from that
closure may be materialized once, with its exact descriptor and complete
current seeds, immediately before its first command-chunk or identity-bearing
control reference. A missing or incomplete first-reference seed fails closed
before the event is recorded; mutations and destroys of unadmitted objects
remain registry-only. This per-identity JIT does not make an object available
after it has been destroyed and does not claim arbitrary deferred closure
across opaque producer-side references.

Sequence-tape retains a complete all-live arm snapshot. Its second interval
cannot admit `ObjectDefine`, so applying frame-tape JIT after the first
`PresentComplete` would violate the sequence grammar. Any unexpected
unadmitted sequence identity therefore rejects before an event is emitted.

For a generation-qualified buffer whose first writable lock is a partial
range and has no complete seed, the PE owner may, only while capture tracking
is enabled, relock that same `D9CBuffer` at offset zero for exactly
`D9CBufferDesc::size` bytes with `D3DLOCK_READONLY` after the application's
unlock succeeds. The returned bytes are copied exactly and recorded as one
zero-offset complete seed; allocation bytes, the partial write, and inferred
padding are never substituted. A stale or missing buffer identity, descriptor
extent mismatch, failed full lock/copy/unlock, or generation proof rejects
only the tape with a typed reason and preserves the application's `Unlock`
HRESULT. Capture-off performs no such relock or copy. The capture-only
read-lock may repeat backend synchronization or dirty-side effects and this
remains explicit performance/semantic debt; `D3DLOCK_DISCARD` is not promoted
by this closure.

**R-HARN-REPLAY-6.7** A writable buffer `CpuUnlock` mutation preserves a typed
lock disposition in the fixed v2 `ResourceMutation` header: `Plain`,
`NoOverwrite`, or `Discard`. The zero value is `Plain` for compatibility with
bundles written before the field was assigned. Provider replay must apply the
corresponding D3D9 lock flag at the mutation's recorded byte offset in both
the full frame-tape and executable-projection paths. Non-buffer mutations,
non-`CpuUnlock` mutations, and unknown disposition values fail closed before
provider effects. This requirement preserves dynamic-buffer backing-generation
semantics rather than treating every mutation as an unqualified plain upload.

Supported CPU-owned texture contents include uncompressed 2D locks and
block-compressed DXT1, DXT3, and DXT5 locks on 2D mip levels and cube face/mip
subresources. When an already-lockable 2D texture subresource receives its
first partial writable lock and has no complete seed, the PE owner may perform
one capture-only full CPU-visible lock through that exact texture handle after
the user unlock, strip row pitch, and record the resulting complete seed. This
closure admits only the exact bytes returned by a successful complete full lock;
it never admits inferred or otherwise unproven allocation bytes or the partial seed,
and aborts only the tape with a typed rejection when identity, generation,
extent, full-lock, copy, or unlock proof fails. It does not apply to standalone
Surface wrappers, cube or volume wrappers, or user-memory surfaces. A
texture-derived 2D Surface wrapper may use the same fallback only after proving
that its live container texture has the exact generation-qualified texture
identity and flattened subresource recorded by the mutation. The relock occurs
only after the application's Surface unlock succeeds and admits only bytes from
the successful exact-owner full lock. Because that capture-only readback may
repeat backend dirty or autogen work, it remains explicit side-effect and
performance debt and is not evidence for promotion.
Every supported full lock and partial rectangle validates pitch,
bounds, and all byte arithmetic, strips row padding, and maintains tightly
packed complete canonical subresource content; block-compressed rectangles are
D3D9-valid block aligned and use rounded terminal blocks for odd dimensions.
A partial write is admissible only after an exact-size complete seed already
exists or one of the exact-owner capture-only closure paths above succeeds;
each committed write then produces an ordered immutable full-subresource
mutation.
Texture-derived surface locks, whether uncompressed or block
compressed, mutate the owning texture identity at the exact
generation-qualified subresource; standalone surfaces remain surface-owned.
Repeated wrappers for one underlying surface identity are idempotent and do
not duplicate ObjectDefine/ObjectDestroy interval events. Versioned descriptor
metadata names the exact parent texture and flattened subresource for surface
aliases and distinguishes complete seed bytes from unavailable bytes. Capture
orders replacements between distinct surface-alias object IDs by their
ObjectDestroy/ObjectDefine events; generation numbers are monotone only within
one wire object ID. An alias and a standalone surface never share a logical
slot merely because their wire object IDs match. Capture rejection at this cold
path records a typed first-rejection reason; the capture-off path performs none of
the block layout or byte-copy work.

Texture and Surface `ObjectDefine` payloads have one production grammar:
`RenderTapeTextureDescriptorV2` followed by exactly one `D9CSurfaceDesc` for
each declared subresource, or exactly one `RenderTapeSurfaceDescriptorV2`.
Texture dimensions are self-describing: 2D and volume counts equal the mip
count, cube counts equal six times the mip count, every descriptor repeats the
exact dimension-appropriate resource type and mip extent, and volume entries
preserve the depth of each mip. Textures use `CompleteSeed` by default; texture
`Unavailable` remains invalid except for the bounded
`ProducedByCapturedPass` lane. That exception admits exactly one Texture2D
with one mip and one subresource, render-target usage (or the exact DF24
depth-only exception in R-HARN-REPLAY-7.37), and exactly one
generation-qualified `TextureSubresource` Surface alias at subresource 0. The
texture and alias definitions must precede the resolving command chunk. The
same retained command chunk must bind that exact alias and perform an
unrestricted full clear as its first terminal access; it resolves one
obligation exactly once, and no seed mutation is permitted. A
`PresentComplete` with an unresolved obligation is invalid. Partial clears,
draw/read-first access, identity mismatch, multiple Produced textures,
multiple matching aliases, multi-mip/cube/volume textures, and any ambiguous
or out-of-order definition fail closed. Independently owned render-target,
depth, and offscreen surfaces are `Standalone` plus `CompleteSeed`; ordinary
texture views are `TextureSubresource` plus `Unavailable` and name the exact
generation-qualified parent/subresource. The oracle target is
`SwapchainBackbuffer` plus `CompleteSeed` when its post-arm contents are
observable, and retains `ProducedPresentOutput` only for a structurally proven
output-only interval that does not read or preserve prior backbuffer contents.
The unversioned
texture level-0/count payload and raw
`D9CSurfaceDesc` Surface payload are Retired and must be rejected by capture
registration, bootstrap/JIT materialization, validation, inspection,
projection, and provider replay. A provider may return a typed unsupported
result for a structurally valid canonical dimension outside its runtime
capability, but must not reinterpret it through a retired grammar. These
descriptor bytes remain schema payload inside the existing `ObjectDefine`
event and do not change the event, chunk, bridge, or ABI/hash wire contracts.

**R-HARN-REPLAY-7.12** Production capture publication requires the explicit
`DXMT9_RENDER_TAPE_OUTPUT_ROOT` PE-visible absolute-root policy. Under Wine the
value is a Windows drive-qualified path such as `Z:\\...`; a host-only POSIX
path is not a PE absolute root. The internal PE fallback
publisher must reject relative/traversal/NUL roots, every symlink component,
digest mismatch, duplicate digest conflicts, and completed-name collisions.
It stages `events.bin`, digest-named blobs, and a v2-compatible
provenance/scope manifest on the output filesystem, flushes and closes the
components, and exposes them only through an atomic directory rename. A
publisher failure leaves no claimable partial bundle and does not alter the
D3D9 call result; capture remains inert when the root is unset or unsafe.

**R-HARN-REPLAY-7.13** A production Render Tape `PresentComplete` carries the
SHA-256 digest of the exact standard-`Present` presentation-output domain: the
row-major, tightly packed bytes of a one-shot offscreen mirror representing the
canonical logical output at the captured backbuffer descriptor extent, rendered
by the existing window `Presenter` with the same present PSO, source, sampler,
and gamma parameters in the same command buffer as the normal drawable present.
The PE owner reserves that mirror only immediately before committing the
captured canonical `PRESENT` chunk, then uses a typed capture-only bridge
finish to drain, read back, validate `(width,height,format,byteCount)`, copy
its fixed-POD result, and copy the same tight bytes into an exact-capacity
top-level bridge buffer. The publisher writes those bytes as `output.rgba`
beside `events.bin`, records its size and SHA-256 in the atomic bundle
manifest, and marks the capture-side output-oracle scope. The sidecar is
authoritative comparison evidence but never a replay input or a member of the
immutable resource-blob catalogue. Before publishing the one-shot reservation, the
bridge drains deferred replay and flushes the renderer queue; after the
captured chunk is accepted, finish drains that replay and flushes the renderer
queue before accepting the ticket as encoded. Therefore neither an older nor a
later Present may consume the ticket for the captured interval. Reservation,
cancellation, failure, a missing or mismatched result, and cleanup abort only
the tape: they preserve the D3D9 `Present` HRESULT and publish no artifact. The
feature is default-off and must add neither allocation nor bridge work while
capture is inactive. This requirement does not widen the accepted grammar,
support `PresentEx`.

**R-HARN-REPLAY-7.15** A frame-tape arm boundary snapshots the exact current
swapchain backbuffer after the arm Present has drained and before any command
of the selected interval is admitted. This is backend-state identity for the
current dxmt9 implementation, not a claim that `D3DSWAPEFFECT_DISCARD`
preserves contents in the D3D9 abstract semantics. The capture names the same
generation-qualified surface used by the later mapped Present, defines it as
`SwapchainBackbuffer/CompleteSeed`, and journals exactly one full tight-pixel
upload before the first command chunk. Replay uploads those bytes into the
real replay backbuffer, restores the queue's touched-color state, and only then
applies bootstrap state or command work. Missing, stale, partial, unsupported,
or late seeds fail closed. A fresh-process exact-output comparison is required
to promote this lane; clearing or substituting a newly allocated output is not
equivalent evidence when the captured interval preserves prior pixels.

Distinct logical surface identities that wrap the same primary backbuffer must
preserve that sharing through provider-authenticated storage classification.
Capture must not infer shared backing from equal descriptors or content. The
explicit Present source selects the logical output independently of definition
order; ambiguous implicit selection must reject. Replay must preserve separate
wrapper lifetimes over the shared backing. Unsupported alias classes or missing
original backing authority must fail closed, not be repaired from expected
pixels or by inserting interval snapshot work.

**R-HARN-REPLAY-7.14** Deterministic provider repetition must construct a fresh
device for every warm-up and measured run, validate before effects, cap both
counts, and report the declared reset, warm-up, and repeat policy. Every
measured run must conserve the same objects, blobs, Present intervals, and
completion state and must produce the same ordered output-digest vector; one
successful run does not establish repeat identity.

**R-HARN-REPLAY-7.15** A Render Tape reducer must preserve closure rather than
delete arbitrary bytes. The bounded first reducer operates on whole
`CommandChunk` events in `frame-tape`, retains the unique bootstrap, the exact
generation-qualified definitions and complete initial mutations reachable from
selected command handles, the selected Present command, and terminal
`PresentComplete`, then rewrites ordinals and component manifests canonically.
The low-level `reduceRenderTape()` API and `dxmt9-render-tape reduce` command
are structural only: they must validate the candidate before publication or
effects, but do not execute a provider or claim an oracle result. The
high-level `scripts/tools/run_dxmt9_render_tape.py reduce`/`bisect` wrapper
owns the separate production-provider preflight/replay gate and may accept a
candidate for provider-backed reduction only after that explicit oracle
succeeds. Deterministic bisection may search this same finite selection space;
it must not silently omit live mutations, controls, destruction, or other
semantics it cannot close.

**R-HARN-REPLAY-7.16** The first Render Tape v2 draw-slice projector must be a
pure, cold value transform over a structurally validated `frame-tape` and an
explicitly verified blob catalogue. Its selector names exactly one canonical
`CommandChunk` event by event ordinal and one non-empty contiguous record
interval within that event. Every selected record must be a Draw, the first
selected Draw's effective state must fold deterministically from the canonical
`BootstrapState` plus every preceding and selected sparse state/constant write,
and the selected interval must be preceded by a Clear and followed by the
frame's Present;
Clear, Present, `OrderedControl`, `ObjectDestroy`, and `PresentComplete` remain
outside the child interval. Successful output preserves selected Draw order as
`(event ordinal, segment index, record index)` locators, exact
generation-qualified identities,
definition locators, immutable-payload references, and every digest-backed
resource mutation for those identities before the selected event. Missing
source validity, complete state-category coverage, live generation-qualified
definition, verified digest, initial full-content closure, boundary, fold, or
range proof must reject before any
replay sink, provider, Metal, or artifact-write effect. This is an offline
transform and must add no per-Draw capture-path work.

**R-HARN-REPLAY-7.17** The native projection command emits
`dxmt9.render_tape.projection.v1`, a versioned pointer-free JSON readiness
artifact bound to the unchanged source `events.bin` by its SHA-256 and byte
count. The artifact must state that it is structural projection readiness only,
did not rewrite v2 wire bytes, is not
`dxmt9.3dmark05.mini_replay_manifest.v1`, and establishes neither provider nor
GT2 replay. `events.bin` alone does not carry authoritative frame ID,
production source/sequence ID, or logical-pass identity. Those values may be
used only after separately authenticating the R-HARN-REPLAY-7.21
`identity.bin`; without it, the canonical event ordinal plus record index is
the only admitted locator and carries no production source identity.

**R-HARN-REPLAY-7.18** TapeExact-qualified production capture and
production-provider replay use one tape-exact attachment-action policy. While
either operation is active, an
attachment restored by bootstrap or resource mutation must load its preserved
contents on first binding unless an explicit Clear or post-Present discard has
higher precedence, and a later serialized command event must not observe a
DontCare store selected only because capture and replay expose different
lookahead windows. The capture process enables this policy from renderer queue
construction so its pre-arm history and selected interval use the same
semantics; a dedicated replay device enables it before applying any event and
restores its prior value only after queued work drains. The policy is
diagnostic-only and inactive when Render Tape capture or provider replay is
not selected. Deterministic equality between two fresh replay processes is a
required intermediate gate, but promotion still requires equality with the
captured `PresentComplete` digest.

The separately selected Normal-action capture diagnostic in
R-HARN-REPLAY-9.19 is outside this qualification domain; it must not relax this
contract or be passed off as an Exact-policy input.

**R-HARN-REPLAY-7.19** Provider replay keeps exact SHA-256 equality as the
primary output result. A frame-tape bundle that also carries the
digest-authenticated `output.rgba` sidecar from R-HARN-REPLAY-7.13 may use a
second, fail-closed 8-bit pixel envelope only after exact digest comparison
fails: dimensions, format, byte count, and sidecar SHA-256 must match the tape;
alpha must be byte-identical; every RGB channel delta must be at most 3; no
more than one pixel per 8,192 output pixels (with a minimum allowance of one
and an absolute maximum of 96) may differ; and total absolute RGB delta must be
no greater than twice that pixel allowance. The result must report exact-digest equality, sidecar
comparison, differing pixels, maximum and total RGB delta, and alpha
differences independently. An envelope match may complete only a one-interval
`frame-tape`; it must not relax structural validation, resource-blob digests,
object/completion conservation, non-degeneracy, a `sequence-tape`, or
fresh-process determinism. An envelope result requires at least two isolated
provider processes whose output SHA-256 values and full evidence records are
identical. Missing, altered, or unauthenticated sidecar bytes retain the
original output-mismatch result.

**R-HARN-REPLAY-7.20** Generated Render Tape artifacts use a transactional,
content-addressed output contract. A bundle producer must validate all source
events and supplied blob digests before creating a final destination, must not
overwrite an existing destination, and must publish only after writing and
flushing a complete same-filesystem staging directory. A failed operation must
leave no claimable partial bundle; the input bundle and any existing curated
fixture remain unchanged. The final `dxmt9.render_tape.bundle.v2` manifest is
the control plane and may claim only these canonical components: `events.bin`,
digest-named `blobs/<sha256>.bin`, the optional `identity.bin` component
declared as `components.identity`, the optional `output.rgba` component
declared as `components.output_oracle`, and the optional `source.rgba`
component declared as `components.source_oracle`. Every component records its canonical
path, byte count, and lowercase SHA-256; no unlisted file is part of the bundle,
and neither sidecar is a resource blob or a replay input.

The `output.rgba` sidecar is the capture-side oracle bytes named by
`PresentComplete`: tightly packed canonical 8-bit RGBA rows at the recorded
output extent. Formats without application-visible alpha, currently
`D3DFMT_X8R8G8B8`, normalize every alpha byte to opaque before hashing or
comparison. It is copied and authenticated as a sidecar, not deduplicated into
the resource catalogue. The sidecar is optional for structural bundles, but a
manifest must not assert `output_oracle` or an oracle result when the sidecar
is absent or fails its byte-count/digest check. `pack`, `reduce`, and `bisect`
may generate new bundles, but their manifests must describe the components and
scope they actually emitted rather than inherit an oracle claim from an input.

When present, `source.rgba` is the tightly packed canonical primary-swapchain
source backbuffer read after the captured Present drained, with the same
opaque-alpha normalization as `output.rgba`. The current frame grammar does
not admit additional-swapchain or direct-Present source identities. The
sidecar is diagnostic, never a replay input. Provider replay may read the
replay source backbuffer after Present and reports `source_readback`,
`source_sha256`, `source_oracle_exact_matched`, `source_oracle_matched`, and
`source_oracle_mode` separately from the output oracle. A non-exact source may
use the R-HARN-REPLAY-7.19 result only when the authenticated captured source
bytes equal the authenticated captured output bytes, the replay source bytes
equal the replay output bytes, and the output pixel envelope already matched;
the mode is then `pixel-envelope-equivalent`. It must not run a second,
independently widened source envelope.
A source mismatch therefore identifies a pre-Presenter draw/resource
divergence; an output-only mismatch leaves the source comparison independent.
Missing or unsupported source readback is fail-closed when
`components.source_oracle` is declared. `pack --source-rgba` must copy and
re-authenticate this sidecar rather than silently dropping its scope claim.

The `project` command is a different generated artifact: it emits one
`dxmt9.render_tape.projection.v1` JSON object to stdout, bound to the unchanged
source `events.bin` digest and byte count. It is structural readiness only and
must state that it did not rewrite wire bytes and did not perform provider or
GT2 replay. If a caller persists that JSON, it must use the same new-file,
flush, and atomic-rename rule; projection rejection emits no artifact. The
strict SHA-256 result and the R-HARN-REPLAY-7.19 envelope remain the only
permitted output-oracle decisions; transactionality must never turn a partial,
unhashed, or projection artifact into replay evidence.

**R-HARN-REPLAY-7.21** A Render Tape identity component is the sole authority
for mapping the canonical event journal to production scheduling and
FrameGraph identity. Version 1 is the retired one-source-per-event grammar and
must be rejected rather than reinterpreted by the current reader. The current
segmented grammar uses the versioned,
pointer-free `dxmt9.render_tape.identity.v2` schema at the canonical
`identity.bin` path and is declared by `components.identity` with its schema,
byte count, and lowercase SHA-256. Its header binds the exact `events.bin`
byte count and digest, frame ID, Present ordinal, and one non-zero bounded
capture token. Each canonical `CommandChunk` event has one or more ordered
identity-segment entries naming the event ordinal, production `sourceOrdinal`,
production `seqId`, and an event-local non-empty record range. A segment's
zero-based index and group count are derived canonically from its position in
the contiguous same-event row group; they are not duplicated wire fields.
Flattened source ordinals and sequence IDs are strictly monotone, while
repeated event ordinals are permitted only for adjacent segments in one
complete group.

The segment ranges must partition every event's records exactly once in record
order. Each segment carries an exact range of pass-membership pieces naming the
frozen post-proof `logicalPassId`, optimized DAG pass index, and pass kind.
EventSerial and SegmentSerial use the same event-wide graph and pass-coalesce
proof. A logical pass may cross a segment edge only through contiguous adjacent
pieces with identical proven pass identity; the edge itself is not a pass
boundary. Missing,
altered, stale-token, non-monotone, partial, overlapping, inferred,
process-local, or cross-edge-inconsistent identity rejects the bundle before
projection or provider effects.

The capture join must copy value identity from the actual
`ResolvedPublishedSource` and `CpuReadySourceMetadata` plus the event-wide
FrameGraph that owns the command after the pass-coalesce proof. It joins that owned value to the PE event
ordinal only through the bounded capture token; it must not retain pointers,
borrowed spans, slot indices, or registry addresses. Capture-disabled execution
must gate before metadata collection or allocation. The PE must finish any
capture-only object/materialization preflight before assigning the final event
ordinal and submitting the chunk, so later inserted journal events cannot move
an ordinal already copied by the provider. Capture may select a larger bounded
Arena profile: EventSerial/default capture retains its 512-page queue admission
bound, while only an authenticated token+event-ordinal capture event in the
queue-immutable `DXMT9_RENDER_IDENTITY_MODE=segment` profile applies the
call-local 64-page-per-source SegmentSerial planner bound over the same
2,048-page (8 MiB) Arena. Startup/non-capture raws and capture-off remain
512 pages/source and one source. Events exceeding that planner bound are
admitted only as an ordered atomic Arena
batch, with all payload, descriptor, control, and Ready entries published
together or aborted together; an indivisible over-bound layout is rejected
rather than widened. This profile must be selected only when both capture and
the publisher are active and must not change normal renderer admission. A
planner, descriptor, or builder rejection during admission or replay setup,
before the command loop's first semantic effect, must abort the complete batch
and select the v2 EventSerial fallback exactly once. This includes a failed
capture-identity arm after source ranges were predeclared. A typed
`RecoverableFailure` after replay has begun is no longer a
fallback boundary: it must fail-stop without recursively replaying EventSerial,
so semantic effects and Presenter ownership remain exactly once. A
provider-replay sidecar may be labelled
authoritative only for the exact replay process that emitted it; an offline
ordinal-derived mapping is not authoritative and must not satisfy this
requirement.

**R-HARN-REPLAY-7.22** Executable projection consumes a structurally valid
`dxmt9.render_tape.bundle.v2` bundle and an authenticated R-HARN-REPLAY-7.21
identity component before effects. It selects one non-empty contiguous Draw
range wholly contained in one sidecar-proven pass. The range may cross
identity-segment edges only when every covered pass piece is contiguous and has
the same authenticated logical-pass identity; output retains each covered
segment's source identity and never infers one merged source. The materializer
emits
another atomic `dxmt9.render_tape.bundle.v2`, never a legacy mini-replay
manifest: it derives a complete bootstrap overlay by folding the canonical
`BootstrapState` and every ordered sparse state/constant write through the
selected first Draw, retains the coordinator Clear and Present outside the
child range, conserves selected Draw order and payload bytes, remaps only
canonical handle-table indices required by the new chunk layout, and includes
exactly the surviving generation-qualified definitions, descriptor
dependencies, immutable payloads, and complete pre-effect resource mutations
reachable from the retained records and derived bootstrap. Incomplete category
coverage, a stale generation, or missing closure must reject before output.
The final candidate must pass the production tape and chunk validators before
the existing production
provider is invoked.

An offline materializer accepts only capture-authority identity. A
provider-replay identity is process-local and may authorize projection only
inside the exact provider process that emitted it; serializing it into a bundle
or passing it to a fresh process must fail before materialization.

The projected output oracle is generated from the projected bundle itself. The
producer must run at least two deterministic fresh-process provider replays,
persist and authenticate their byte-identical tight RGBA output as the new
bundle's `output.rgba`, write that digest into the projected
`PresentComplete`, and then repeat strict provider validation against the final
bundle. It must not copy the source full-frame digest or pixels as an equivalent
projection oracle. Strict SHA-256 equality is the default and required result;
R-HARN-REPLAY-7.19 may be considered only when separately authenticated
capture-side pixels have identical projected semantics. Any sidecar,
materialization, conservation, output, determinism, or final-provider failure
removes staging and leaves no destination.

**R-HARN-REPLAY-7.23** The production Render Tape parallel-refinement join is
a cold, fail-closed provider operation over an unchanged authenticated bundle.
The provider must expose a typed partition request with distinct `identity`,
`serial`, and `parallel` values, resolve it through the production partition
configuration, and report the requested and resolved values. Its result must
also report typed unsigned evidence for selected parallel passes, accepted
children and Draws, worker batches/tasks/active peak, pre-effect fallbacks, and
GPU command-buffer errors. Worker CPU/wall time may be reported for diagnosis
but is not proof of work because clock resolution may produce zero.

The official parallel verifier must first validate the complete bundle and
then invoke at least two separate fresh provider processes per mode over that
exact input: identity and ExplicitParallel. Every run must complete the strict
production output oracle. Replay identity—including status, requirements,
validity/output digest, grammar coverage, conservation, and interval
evidence—must be exactly equal within the identity repetitions, within the
ExplicitParallel repetitions, and across both modes. Acceptance additionally
requires the non-timing parallel evidence (`selected`, children, Draws,
pre-effect fallbacks, GPU errors, and worker batches/tasks) to be equal across
the ExplicitParallel repetitions; worker timing and active peak remain
scheduling diagnostics and are gated only as stated below. Acceptance also
requires every ExplicitParallel run to report `selected > 0`, at
least two accepted children, `draws > 0`, worker batches/tasks/active peak all
greater than zero, and zero GPU command-buffer errors. A pre-effect fallback
count remains typed diagnostic evidence; a fallback that prevents these gates
cannot be accepted as parallel evidence. Any malformed counter, mode mismatch,
provider failure, oracle failure, equality mismatch, vacuous selection, or GPU
error rejects the joined result. The operation produces one machine-readable
result only after all child processes finish and writes no replay artifact.

Every run in the official parallel verifier over a production-capture or
output-oracle bundle must also report `validity.output_non_degenerate=true`. A
structurally valid uniform or black bundle remains useful for schema,
conservation, and generic fixture `provider-replay`/parallel tests, but it is
not promotion evidence for the parallel production join.
The generic `provider-replay` command retains its existing policy for
non-production fixtures and may accept such a bundle when its declared oracle
is exact.

**R-HARN-REPLAY-7.24** A `CompleteSeed` for a resource whose backend-visible
contents may differ from its durable PE CPU shadow must use an exact
generation-qualified arm-boundary GPU snapshot when the resource matches a
declared supported shape. The snapshot is taken after the arm Present and all
older replay have drained, before any selected-interval command is admitted,
and replaces the PE content only in the value-owned arm overlay. The first
managed shader-input shape is a single-level, single-subresource,
single-sample `D3DFMT_A8R8G8B8` 2D texture with `D3DPOOL_MANAGED` and zero
usage; its meaningful alpha byte is preserved. Existing X8 render-target
snapshots continue to canonicalize the unused alpha byte to opaque. A stale
generation, descriptor mismatch, unsupported shape, failed readback, partial
result, or extent mismatch aborts capture before publication; the producer
must not substitute the PE bytes, zero-fill, or widen the supported set. This
is a concrete resource-content claim: descriptor truth tables and production
readback tests bind the policy, while a same-run GPU dump and source oracle
provide the applicable GPU-visible evidence. It does not claim that TLA+ can
prove Metal texture bytes.

The deterministic native/CLI acceptance fixture must contain a coordinator
Clear, a canonical `FULL_SNAPSHOT` anchor, one DrawRun whose production
partition has at least two non-empty children and satisfies the unchanged
child-boundary economics policy, and a coordinator Present. This requirement
closes the bounded implementation join; it does not claim a GT2 capture,
performance result, default promotion, or wild correctness evidence. A
non-vacuous GT2 capture/ExplicitParallel replay remains a separate evidence
gate.

**R-HARN-REPLAY-7.25** A v2 identity event-group join is atomic. Before any
projection, provider, or artifact effect, validation must authenticate every
segment identity, exact event-local record partition, and pass-piece
continuation. A missing segment, stale generation, range gap/overlap,
unsupported control, or resource-identity failure rejects the whole event
group; it must not publish a partial source list or infer a compatibility
identity. Capture and provider replay must report segment count, flattened
record coverage, per-segment completion, and final event settlement separately.
The legacy `dxmt9.render_tape.identity.v1` component is Retired and rejected
before staging, projection, or provider invocation; its one-source-per-event
bytes must not be reinterpreted as current identity. The v2 EventSerial
one-source-per-event compatibility lane remains valid and reachable, and is the
required complete-event fallback when v2 SegmentSerial cannot be authenticated
or proved before effects. v1 is never a fallback.

**R-HARN-REPLAY-7.26** Coordinator-capable capture identity must use an
explicitly versioned grammar; existing v2 bytes retain their original meaning.
Every canonical record must have exactly one authenticated disposition:
GPU-command provenance, state-only replay, or coordinator execution. Actual
published sources are unique rows referenced by provenance, not duplicated
per event. A writer spanning events may reference the same actual source.
Coordinator/state-only records may settle replay without creating a source or
pass. Such settlement must not imply submission or GPU completion. Source
rows and completion evidence must originate at actual successful publication
and completion edges; capture must not introduce extra flushes or publications
to satisfy its grammar. Capacity, cancellation or stale provenance failures
must reject the capture, not synthesize coverage or replay work again.
Physical command provenance must permit multiple, even nonadjacent, canonical
draw records to share one actual DrawRun across intervening state records.
Unique physical command rows own actual command kind, draw extent and pass
membership; draw dispositions reference disjoint ordered subdraw slices.
Physical command coverage/completion is counted once, while canonical records
remain exactly covered. Capture must not split a production DrawRun solely to
fit its evidence format. Missing/overlapping subdraws, unsupported mappings and
unrepresented generated commands fail closed.

**R-HARN-REPLAY-7.27** Query coordinator provenance must bind the capture
epoch, event/record, raw replay ordinal, wire identity/generation, issue cycle,
flags and actual dispatch outcome. A deferred END has an explicit Unbound
prefix until its generation-qualified marker binds at successful publication.
Zero sequence is not completed or an implicit empty prefix. An END with no
new publication may reference an authenticated existing source or arm-baseline
prefix; a zero-work baseline requires its own explicit authenticated kind and
must not be inferred from sequence zero. BEGIN need not create any prefix.
Pending observations may precede binding; Ready/Completed require current-issue
publication and completion
evidence. The exact END command cursor must be retained at dispatch and checked
against ordered provenance; merely bounding it by final source size is
insufficient. Younger commands must not expand the END's logical prefix.
The initializer/upload dependency has independent publication and completion
evidence. An empty END may publish a new initializer prefix while retaining an
already-completed render sequence; render completion alone must not admit
Ready. Post-arm initializer work must not be treated as baseline-completed or
represented by a fabricated render source.

**R-HARN-REPLAY-7.28** Coordinator-capable source-policy authentication must
bind immutable events, the versioned identity sidecar, actual publication cuts,
record dispositions, coordinator positions and prefix dependencies. CPU and
Metal consumers must validate the same plan before effects. Publication cuts
must follow authenticated original boundaries, not event count or an invented
one-source-per-control convention. Bounded model-code tests must cover empty
END, Draw/END/younger Draw, deferred binding, reissue/reset, cross-event writers,
stale generations, capacity failure and exactly-once settlement. These tests
do not replace original query-result and GPU-byte qualification.

**R-HARN-REPLAY-7.29** A runtime identity snapshot transferred from Unix to
PE must use a distinct bounded transport grammar, not impersonate a completed
identity sidecar. The receiver must match the capture token and epoch against
its independently retained arm key, bind the snapshot to its actual sealed
events and verified blob catalogue, and perform complete v3 validation before
publication. A size-query/copy pair must observe one frozen snapshot. Malformed
extent, stale key, failed completion or incomplete evidence must leave no
published identity. Detaching the observer must revoke previously copied
callbacks before releasing their capture owner; a stale callback must not
write into a subsequent capture incarnation.

The v3 coordinator and settlement `buildGeneration` fields identify the same
capture-qualified replay-attempt incarnation allocated at the actual replay
transaction begin. They must not alias a raw FIFO ordinal or an Arena
source-build ticket. Settlement requires both the successful production
transaction commit and its successful replay-ledger acknowledgment; record
walker exhaustion alone is not settlement. Failed commit or ledger publication
must invalidate the capture without manufacturing a settlement row.

**R-HARN-REPLAY-7.30** Publication authenticates source identity and immutable
physical command extents, not the eventual Encode pass plan. Executed-topology
evidence must originate at the selected production plan's acceptance boundary,
after frontier, ownership and completion-registration checks. Candidate graphs
that are rolled back or rejected must not contribute authenticated topology.
Natural-order execution, planner bypass, accepted optimized order and proven
command omission must be distinguishable; an omitted command must not be
represented as an executed no-pass command. Cross-source pass equivalence must
come from the accepted combined plan, never from equal source-local pass indices.
Logical plan membership must not claim physical Metal encoder equivalence.
Missing or unsupported topology evidence must reject qualification without
changing the production route or introducing a capture-only publication.

**R-HARN-REPLAY-7.31** Neutral physical provenance uses a new identity v4
grammar with a required execution-topology v1 companion. Existing v3 pass
membership semantics remain unchanged; conversion must not invent pass values.
The companion authenticates the exact identity bytes and digest, event bytes
and digest, capture token/epoch and queue identity. Bundle publication must
authenticate both components without circular digest dependencies.
Every physical command has exactly one terminal placement: executed in an
accepted DAG, executed without a selected DAG, or proven omitted. Execution
ordinals are distinct from canonical physical-command order. DAG equivalence
requires the same accepted plan and group, not equal local indices. Earlier
encoded prefixes and later suffixes retain separate accepted invocations;
a later graph cannot retroactively authorize earlier execution. Missing
submission evidence for a fully omitted source must not be replaced with a
synthetic GPU submission or completion.

**R-HARN-REPLAY-7.32** An Encode invocation may execute before its source's
completion ownership is registered. Its selected topology and successful
execution observation must remain distinct from accepted topology. Capture
must retain bounded owned candidates across such prefix/suffix invocations,
preserve their exact execution order, and promote them only after the actual
registration receipt validates the complete source closure. A suffix may use
an earlier successfully encoded prefix as an execution witness, but selection
alone is insufficient. Rejecting an already executed invocation invalidates
capture rather than erasing the observed effect. Batch acceptance validates
all identities, generations, dependencies, capacity and order before exposing
any accepted placement. Missing evidence must not alter production execution.
Registration acceptance and successful execution are independent facts: a
combined plan may be accepted before its fragments execute, while a speculative
prefix may execute before registration. Final executed-topology qualification
requires both facts; neither order may be fabricated to fit the observer.

**R-HARN-REPLAY-7.33** V4 must distinguish a raw event's replay-ledger
acknowledgment from the actual replay transactions used to execute its spans.
Every admitted span attempt retains its actual generation, original record
range, terminal outcome and destination receipt. Committed spans must partition
the original raw's records exactly in order before that raw may settle.
Pre-effect rollback contributes no committed coverage; a retry has a new real
attempt identity. A coordinator binds to the attempt that dispatched it, not
an invented raw-wide generation. Post-effect failure invalidates capture.
Receipt fields retain their production meaning: a compatibility transaction's
sequence is not independently authenticated GPU submission evidence. Source
publication/submission/completion remain separately observed facts. Legacy V3
keeps its original single-transaction settlement contract; supporting V4 spans
must not alter renderer route selection or construct synthetic transactions.

**R-HARN-REPLAY-7.34** The PE owner must retain the capture token, epoch and
queue identity returned by the successful Unix arm operation independently of
the transferred sidecars. Size-query, copy, pair decoding and final attachment
must agree with that retained binding and Present ordinal. A frozen size-query
must not expose a different pair on the copy call. Missing, mismatched or
orphan identity/topology components must abort publication atomically. A valid
pair alone does not grant query-readiness, GPU/pixel or timing qualification.

**R-HARN-REPLAY-7.35** V4 coordinator admission must distinguish an actual
acknowledged session release from an actual ordinary serial dispatch with no
CPU-ready session. The ordinary witness must originate at the production
coordinator admission boundary, positively establish the absent session, and
bind the capture, queue, event, raw, attempt and original record identity.
The V4 header must retain the observed immutable queue session domain, and
every coordinator's authority must agree with that domain, including after
paired transport and source-plan authentication.
A missing release callback alone must not authorize ordinary admission.
Ordinary admission must not invent a release ordinal, generation, posted
request, acknowledgment, source publication or completion. Session-enabled
coordinators still require their real release receipt, including when their
writer is empty. Both coordinator-only whole-raw replay and coordinator spans
within mixed replay must be covered. Existing query END/prefix, dispatch,
exactly-once and raw-acknowledgment obligations remain independent and mandatory.
Legacy V3's strict session-release grammar must remain unchanged; V4's distinct
admission meaning must be explicit in its versioned evidence and validators.

**R-HARN-REPLAY-7.36** A bounded descriptor-only NULL render-target lane may
admit the exact supported `D3DFMT_NULL` Texture2D render-target descriptor and its
generation-qualified Surface alias without an initial color payload. This is
an explicit no-meaningful-color-content authority, not a complete seed,
produced-by-pass proof or general permission for unavailable texture content.
Replay must recreate the original descriptors through the ordinary factories;
the lane must not substitute another format, alter renderer routes or assume
that every physical implementation allocates no backing.

This lane is limited to a DEFAULT-pool, render-target-only, single-sample
texture with one mip and one subresource and its subresource-0 alias; standalone
surfaces remain outside this authority. The descriptor/usage shape and alias
parent, generation, level, dimensions and
format must remain exact. Every use, including later references to an already
admitted object and repeated references within one chunk, must remain in the
allowed render-target binding/write-only domain. Sampler bindings, content
reads, copies, CPU mutations, readbacks and oracle attachments remain outside
this lane and must reject capture/replay qualification before their effects.
Unknown formats, non-render-target NULL objects and invented content bytes
must not acquire this authority. PE capture, wire validation and provider
preadmission must share the classification and access contract, with native
positive/negative parity tests before original-workload GPU qualification.

**R-HARN-REPLAY-7.37** Produced content authority may also admit an exact DF24
Texture2D with DEFAULT pool, DEPTHSTENCIL-only usage, positive canonical
extent, one mip and subresource, and no multisampling. Its physical format is
Depth32Float and its only meaningful contents aspect is depth. It must retain
the exact generation-qualified subresource-zero Surface alias and all existing
first-terminal-access, definition ordering, per-identity obligation and no-seed
mutation constraints. PE admission, canonical validation and General-provider
preadmission must use the same typed descriptor and witness policy. Each
parent owns one independently settled obligation and an unambiguous alias;
several independent DF24 parents may coexist only when each is separately
proved. The legacy bounded color provider's singleton restriction is not a
tape-global restriction on this General-provider extension.

The accepted producer must be the exact alias in the DEPTH_STENCIL binding
section of a canonical Clear, with flags exactly ZBUFFER and an empty, valid
rectangle tail. The authority refines the current captured backend stream,
whose empty-rectangle depth clear overwrites the whole attachment. It does not
certify D3D9 API viewport/scissor Clear conformance. Mixed color/depth clears,
stencil-only clears, partial rectangles, unsupported depth formats/topologies,
read-before-clear and ambiguous/stale identities must not acquire this
authority. A later clear cannot repair a rejected first terminal access.

Restoration must use ordinary texture/alias creation and ordinary Clear
execution, without an invented initial seed, format substitution, route
override or new depth checkpoint encoding. The bounded color provider must
remain color-only; its unsupported result may select the existing General
provider. Before original-input qualification, bounded native trace/flag/
identity checks must bind this contract to production predicates, and a real
Metal oracle must prove whole-image independence from distinct prior depth
contents, including deferred-clear and attachment-mismatch flush paths.
Original STALKER source/query/pixel and policy-performance qualification
remain separate mandatory gates.

## 8. Fixed-Input Policy Experiment Loop

**R-HARN-REPLAY-8.1** A rapid experiment consumes an authenticated immutable
Render Tape bundle, not a newly interpreted PE API trace or a captured Slot
that bypasses the policy under test. The input owner preserves command order,
complete bootstrap/resource closure, immutable blob contents and qualified
identities across every candidate and repetition. Preparation, resource setup,
replay, encode, completion, oracle and reset costs must be distinguished; a
timer excluding a phase must not be labelled end-to-end. Existing capture,
projection and fresh-process provider contracts remain unchanged.

**R-HARN-REPLAY-8.2** The experiment must execute production import/replay,
planner, Slot construction and resource-lifetime operations for the tested
scope. A CPU-only mode may substitute backend execution/completion but must
not substitute an unrelated planner or lifetime simulator. A Metal mode must
use the actual provider/encoder and real completion before readback. Requested
and resolved policies, actual direct/compatibility work and fallback reasons
must be reported; unsupported or vacuous candidates cannot pass a claimed
execution gate. Policy exploration never relaxes a production eligibility,
generation, first-draw, capacity or ordered-control proof.

**R-HARN-REPLAY-8.3** Fast repetitions execute in one process with separately
declared cold-device and warm-device/cache policies. Each iteration restores
all effective state and every mutable resource it may read, drains or aborts
its outstanding work, and retires its CPU pins and final-use stamps before
reset/reuse. An unchanged input digest alone does not prove reset. An explicit
reset oracle and a changed-state/mutation A-to-B-to-A negative control must
detect cross-iteration contamination. Shared immutable input and deliberately
warm compilation caches are not implicit permission to retain live resources
through all iterations. Warm-up iterations are excluded from measured totals
and individually checked. Failure stops the experiment without publishing a
successful partial aggregate.

**R-HARN-REPLAY-8.4** Completion exploration uses a bounded explicit schedule
over qualified submissions, not sleeps or assumed OS timing. The controlled
backend may delay and release completion to exercise pin/stamp transfer,
retirement, capacity pressure and generation/backing reuse through production
lifetime code. A completion cannot precede its corresponding submission,
advance over an unfinished prerequisite, or be acknowledged twice. A real
Metal backend may delay acknowledgment only after actual GPU completion;
synthetic completion must never authorize reclaim of in-flight Metal storage.
Exhausted schedules, unavailable progress, malformed actions and unresolved
work at termination produce named failures, not hangs or implicit drains that
erase the tested condition.

**R-HARN-REPLAY-8.5** Reference/candidate equality covers ordered command and
draw semantics, first-draw state/uniform values, exact generation-qualified
resource/backing identities, coordinator effects and completion obligations.
Different legal Slot/run layouts are compared through a documented semantic
projection, not raw pointers or only final pixels. Bounded lifetime cases must
include mutation/rename, destroy before completion, delayed completion,
capacity cuts, generation reuse and injected pre-effect allocation failure.
Shared production predicates must bind the bounded temporal model to native
transitions. Negative controls must demonstrate sensitivity to premature
reclaim, wrong backing/generation, missing/duplicate effects and bad reset.
Metal readback complements, and does not replace, this layer.

Policy equivalence must be a paired comparison, not two independently accepted
runs. The reference and candidate must bind the same authenticated Tape,
provider images, repeat count, resource backend and completion/publication
policy. Every iteration must pass its own execution, conservation and reset
checks before comparing ordered semantic evidence. Per-draw cumulative
semantic observations may identify the first differing logical draw; their
physical publication event must not be mislabelled as the original capture
draw locator. Physical batching counts are observations, not equality criteria.

**R-HARN-REPLAY-8.6** The experiment produces a versioned machine-readable
receipt containing immutable input and executable identities, effective policy,
reset/cache/backend mode, schedule identity, per-iteration coverage/oracles,
actual execution/fallback counts and separated wall/native-CPU measurements
where supported. Repeat bounds and resource limits are explicit and validated
before effects. Raw results retain all failed and slow iterations. A reduced
counterexample remains an authenticated Tape plus schedule, not an unbound
text log. A fast fixed-input result is backend cost/safety evidence, never a
game FPS or default-promotion claim. Qualification includes a real captured
GT2 input and a small lifecycle-sensitive sequence; a Clear-only fixture does
not establish draw-policy or lifetime completion.
The receipt must preserve inherited Metal API/shader-validation settings
alongside renderer knobs without dumping unrelated process secrets. Enabled
or unknown nonempty validation settings make the run diagnostic and ineligible
for policy-performance acceptance even when its pixel oracle passes. Failed
diagnostic receipts and actual readbacks remain preserved.

**R-HARN-REPLAY-8.7** CPU policy experiments must separate semantic
qualification from low-perturbation measurement. Both modes must execute the
same admitted production replay, Slot construction, resource mutations,
publication, completion and reset policy. Measurement must not hash draw or
resource contents or collect per-draw semantic observations. It must retain
production safety checks and cheap execution/conservation counters. A measured
receipt must not claim a fresh semantic or pixel oracle. Acceptance requires
successful detailed qualification bound to the same immutable input, loaded
images, effective policy and reset/backend configuration. Changed or missing
bindings invalidate acceptance. Failed and warm-up iterations are retained
and never silently folded into successful measured totals.

**R-HARN-REPLAY-8.8** A versioned CPU policy interface must permit independent
batching and capacity choices on an unchanged authenticated Tape. Policies
must be bounded and validated before runtime effects. Requested and effective
values must be recorded, including any production clamp. Commands,
coordinator effects and qualified lifetimes must remain ordered across physical
batch changes. Unsupported combinations must fail explicitly rather than
silently substituting the baseline. Actual publication/source counts,
direct/fallback work and capacity outcomes must identify the executed policy.

**R-HARN-REPLAY-8.9** Cross-policy CPU qualification must compare ordered
semantic observations independent of physical publication layout. The
measurement aggregate must bind each measured policy to its own qualification
and require reference/candidate semantic equality before reporting a qualified
cost comparison. Phase labels must distinguish observer-heavy verification
from low-perturbation execution; process CPU and wall time remain separate.
CPU costs exclude real encoder/GPU execution, Wine supply and production
concurrency unless those boundaries are explicitly executed and qualified.
An optional diagnostic-only CPU mode may partition wall and process CPU time
with exclusive nested scopes. It must retain measurement's execution and
conservation checks, reject unavailable/regressing clocks or inconsistent
phase totals, and remain ineligible for policy ranking and semantic
qualification. Diagnostic phase evidence must not be relabelled as ordinary
measurement. CPU-only attachment shadow work not performed by the normal
GPU-backed provider must be identified before claiming representative
production replay cost.

Attachment content ownership must be independent of GPU readback capability.
The general CPU executor must use backend-command-owned attachment contents,
preserve every Clear/draw/publication and qualified lifetime, and keep GPU
readback unavailable. Immutable seed writes remain permitted; readback,
CPU read locks and commands requiring prior CPU attachment pixels must be
rejected at grammar admission before effects. Seed storage must never be
reported as post-render pixels. Receipts must identify this ownership contract;
missing/legacy ownership receipts are not comparable with corrected timings.
A bounded exhaustive seed/Clear/read refinement, native production-predicate
binding and the independent real-GPU exact oracle are required before using
the corrected CPU cost for policy comparisons.

**R-HARN-REPLAY-8.10** When `--gpu-qualify` is requested, no CPU measurement
child may start until both detailed CPU verification receipts and their
authenticated input/provider/validator/image identity pair have passed. The
gate launches a fresh real-Metal `experiment` child for each canonical CPU
policy with warmup zero, two qualification iterations, identity partitioning,
an explicit cold- or warm-device reset, and all three policy environment knobs
set exactly from that policy. CPU and Metal provider executables are separate
roles, but validator/input identity, shared loaded runtime modules, and the
non-policy renderer environment are bound before measurement. A failed GPU or
pre-measurement identity gate starts no measurement child. Final
reauthentication failure rejects the aggregate while retaining any measurement
artifacts already produced as unqualified evidence.

**R-HARN-REPLAY-8.11** GPU policy qualification requires both original
authenticated source and output RGBA oracles. Production evidence must carry
`scope.production_capture=true`; synthetic/test fixtures must be explicitly
marked `scope.synthetic=true` or `scope.test_input=true` and are reported as
synthetic evidence. Each provider result must carry top-level strict oracle
mode, `expected_digest_matched=true`, and `source_oracle_exact_matched=true`;
every interval must match the original expected digest and source bytes, and
retain an independently authenticated provider readback artifact whose bytes
equal those original bytes. Pixel-envelope-only evidence, diagnostic validation runs, A/B equality
without original oracles, missing/tampered readbacks, wrong route/policy/env,
zero selected-route draws, GPU errors, incomplete completion, reset or
conservation are failures. The coupled gate admits single-interval frame tapes
only; sequence tapes require per-interval original oracles and remain rejected.
Qualification is readback evidence, not a GT2 promotion claim by itself.

**R-HARN-REPLAY-8.12** CPU-only policy receipts remain explicitly CPU-only.
The coupled receipt records each GPU child path and hash, the qualified input
scope, strict oracle/reset evidence and final binary reauthentication without
relabeling a synthetic fixture as a production capture. The shared acceptance
predicates used by the coupled GPU gate must be exercised by
native-free negative tests for missing original oracles, envelope-only output,
source mismatch, missing/tampered RGBA, diagnostics, wrong route or zero path,
GPU error, policy/environment/hash mismatch, and a strict A/A success.

**R-HARN-REPLAY-8.13** A capture-source-boundary replay diagnostic must validate
the sidecar against the immutable tape and verified blob catalogue before any
tape effect. Only Capture authority with complete event settlement and exact,
nonempty, nonoverlapping source-record coverage is admissible. Each command
record executes once in original order through the production replay sink;
source boundaries request explicit queue publication without inventing captured queue
identities or adding per-source GPU waits. A failure after a visible prefix
must fail-stop rather than replay that prefix. Receipts must distinguish
validated source cuts, completed cut requests, and observed physical pass
boundaries: none implies either of the others or original-pixel equality.
This diagnostic is excluded from policy qualification and performance ranking.

An optional batch-cut discriminator may flush a DrawRun before an explicitly
located DrawPrimitive/DrawIndexedPrimitive record strictly inside a source.
Its bounded, sorted, unique event/record locators must all validate against the
same authenticated input before effects. Batch cuts must not request queue
publication, encoder termination, or GPU waits; only the original source ends
may request publication. Receipts must preserve requested/completed batch cuts
separately from source cuts. Exact serial coverage and unchanged publication
edges require native exhaustive pins; physical pass equality still requires
observed trace evidence. Batch-cut diagnostics must never qualify a policy.

**R-HARN-REPLAY-8.14** A GPU-qualified capture-source reference policy must
explicitly preserve the authenticated source-publication cuts through the
same record-range plan in CPU and real-Metal execution. The versioned policy
and receipts must bind the boundary mode, source-plan digest, and selected
replay route. Original canonical chunk bytes, full handle/payload tables,
retained wrappers and buffer snapshots remain one raw owner; range planning
must not repack wire data or turn captured source/sequence labels into local
queue authority. ProductionSlot must enter the ordinary Direct ChunkSlot
admission/assembler path, with source-limited compatibility work reported
separately, rather than substituting an Arena implementation.

All range plans and ordered-control legality must be checked before effects.
The cold source-policy driver admits at most 4096 records per canonical raw;
its per-range publication receipt allocation is bounded by twice that record
count plus one. CPU no-worker execution must additionally prove its narrower
in-flight queue bound before replay; receipt capacity is not queue credit.
A fallback may replay only an as-yet-unexecuted source range, never the full
raw after an earlier source has executed. A failure after effects in that
range must fail-stop. Source settlement must distinguish actual publication
with a queue-issued source/storage/sequence receipt, publication performed by
the source's Present, and an empty state-only settlement without GPU work.
The private raw transaction identity remains separate from publication
metadata: repeated local source publications must not reuse a nonzero raw
ordinal in Tape's strictly increasing raw-publication high water. The drain
journal publishes raw replay completion once, after all its ranges settle.
Native exact-cover, stale-source, empty, Present and failure-prefix pins bind
the reused queue lifecycle refinement. Strict original GPU oracles and actual
route coverage remain required; preserving requested cuts alone cannot
qualify a policy or prove physical pass equivalence.

**R-HARN-REPLAY-8.15** Original/replay GPU compilation equivalence must include
the explicit runtime compile profile from R-BACK-4.10, not only generated MSL
or a nil-options receipt. Equivalent source strings compiled under different
host-selected language versions are not an equivalent reference. Native replay
and Wine rendering must share the production profile. SDK-header modifications
may be used as retained causal diagnostics but are not the product fix or a
substitute for normal-provider qualification on the unchanged original bundle.

## 9. Query-aware captured policy experiments

**R-HARN-REPLAY-9.1** A query-aware capture must preserve query type,
generation-qualified object identity, issue-cycle identity, ordered BEGIN/END,
and each observed GetData call's FLUSH disposition, output-presence/size,
HRESULT and defined result bytes. Pending or failed calls must not copy
undefined caller memory. Captured queue labels are provenance, never replay
submission or completion authority. Legacy payloads missing this information
must not qualify a query-policy comparison.

**R-HARN-REPLAY-9.2** Admission must establish the query state at the capture
cut. A query begun before the cut, an END whose contributing work is absent,
or a result depending on an uncaptured issue cycle must have an authenticated
complete checkpoint or be rejected with an identity-qualified reason before
effects. Device-shadow completeness alone does not establish query or resource
completeness. Capture failures must preserve normal application HRESULTs and
must not silently strip queries, invent results, or publish a partial transcript
as a complete experiment input.

**R-HARN-REPLAY-9.3** CPU query replay must execute the production query,
publication and lifetime path with an explicitly controlled backend completion
schedule. Real-Metal replay must use actual queries and actual GPU completion.
Both modes must bind END to the local replay issue cycle and real local
submission identity. Captured Pending/Ready observations are semantic evidence,
not permission to sleep for captured wait durations or acknowledge unfinished
work. A bounded progress policy must report exhaustion rather than hang.

**R-HARN-REPLAY-9.4** Replay must preserve every recorded polling call and its
submission obligation, but must distinguish captured HRESULTs from newly
observed replay readiness. Different legal completion timing may change Pending
counts. Successful defined results must be compared for the same issue cycle;
missing, duplicate, stale or mismatched results fail qualification. Any extra
oracle polling or final drain must be separately reported, not inserted into
the measured transcript without disclosure. The CPU controlled backend cannot
claim a GPU occlusion result oracle.

**R-HARN-REPLAY-9.5** Query-aware policy comparisons must bind unchanged input,
source boundaries, images and reset policy, preserve production route
eligibility, and report actual route/fallback coverage. Diagnostic attribution
must separate replay/Slot construction, pass preparation, actual encoder
creation/configuration, draw encoding and completion waits. Overlapping thread
times must not be added into a critical-path estimate. CPU-only results must
not be labelled Metal Encode cost. Qualification requires both captured
query-bearing workload evidence and deterministic negative controls for stale
cycles, early completion, reissue, release/reset and incomplete bootstrap,
followed by actual Metal query-result and original-pixel oracles.

**R-HARN-REPLAY-9.6** A captured workload using volume textures requires exact
generation-qualified mip contents, including depth, before either executor may
admit it. CPU lock pitch and slice pitch are layout metadata, not canonical
content. Capture and replay must convert each checked row and slice to/from the
complete canonical mip blob; block-compressed rows must use their block-row
count, not logical height. Partial writes may establish a complete seed only
through an exact-owner full snapshot or an already complete checked overlay.
Failed snapshots reject capture without changing the application's operation
result. CPU upload evidence must include depth and both strides, and the Metal
oracle must exercise distinct z slices and mip levels. Supporting query grammar
does not waive these resource-closure requirements.

**R-HARN-REPLAY-9.7** Capture bootstrap must preserve the complete live keyed
device state even when it exceeds one canonical sparse section's capacity.
Baseline profile 3 must contain one initial FULL_SNAPSHOT APPLY_STATE anchor,
followed only by nonempty delta APPLY_STATE records in a bounded ordered
overlay sequence. Later full resets, missing anchors and empty overlay chunks
must reject before execution. Pagination must preserve explicit null bindings,
constant bytes and every keyed entry without consuming live pending state or
enlarging the ordinary recorder's scratch or wire section capacities. State
folding and projected bootstrap emission must preserve the same accumulated
state; accepting a paged capture while truncating or rejecting its legal folded
state does not complete this contract. Legacy profile 2 retains its existing
validation semantics. Native bounded refinement must compare reconstructed
state with the unsliced reference across capacity boundaries; actual captured
query and pixel qualification remain separate requirements.

**R-HARN-REPLAY-9.8** A captured standalone single-sample D24S8 attachment must
preserve both depth and stencil at the capture cut. A depth-only clear or
checkpoint must not establish complete initial contents. The version-1 dual
plane encoding consists of tight little-endian float32 depth followed by tight
uint8 stencil, with checked total size `5 * width * height`. Its explicit
content disposition requires actual Depth32Float_Stencil8 backing on both
capture and replay. Depth24Unorm_Stencil8, depth-only fallback and multisample
backings reject; no quantization to D3D packed 24-bit depth is permitted.
Capture must validate generation, descriptor, encoding, both pitches, plane
offset, byte count and physical format before publishing the checkpoint.
Replay must restore both aspects before any consuming draw. Independent
nonuniform depth/stencil byte and pixel oracles must establish capture and
inverse restoration, including wrong-plane and warm A-to-B-to-A cases. The
existing D24X8 and produced-by-pass admission contracts remain unchanged.
The cold checkpoint must retain the exact backing through transfer completion,
reject active borrowed producers before inspecting their payloads, and prove
ordinary queue and initializer completion. Exclusive producer admission must
remain held from that proof through transfer completion; a drained watermark
alone does not authorize a later unprotected transfer. Terminal, failed upload
or transfer, and incomplete drain must reject checkpoint completeness. Native
bounded transition tests must distinguish early transfer, producer reopening,
and signal-only completion from the accepted sequence.

**R-HARN-REPLAY-9.9** A completed pre-cut EVENT query may be represented by an
explicit versioned initial-state checkpoint, not by a fabricated captured
QUERY_ISSUE record or an unconditional Ready result. Capture must bind the
exact wire identity and absolute issue cycle to the current closed EVENT,
device incarnation, retained published END authority and actual successful
completion. A stale retained snapshot from an earlier public issue, cancellation,
terminal state, incomplete publication, Pending result or failed completion
must not establish this checkpoint. Checkpoints are capture-arm-local and must
not change the durable registry or the application's query state/HRESULTs.
Absolute Issue ordinal and logical issue cycle are distinct: BEGIN/END history
must not be collapsed into an assumed numeric equality. Capture must compare
both against independently maintained current Unix Issue history; echoing a
PE-supplied cycle alongside an unrelated valid snapshot is insufficient.

The authenticated checkpoint authorizes restoration of initial EVENT state,
not adoption of captured queue labels. CPU and Metal replay must establish a
new local production END/publication identity and complete its real local
obligations before reporting Ready. The CPU lane uses its declared controlled
completion backend; only the Metal lane may claim actual GPU completion.
Checkpoint setup issues, publications and waits must be reported separately
from captured command/source counts and measured transcript work. Recorded
GetData calls, FLUSH obligations, absolute cycle matching, reissue/release/reset
semantics and EVENT's defined-result-byte contract remain intact.
An empty local END prefix may lawfully publish no source and have sequence
zero; replay must not invent commands or GPU work to make that receipt
nonzero. It must still establish the new local issued-query incarnation.
Checkpoint query setup must occur before the measured transcript. A query
materialized later by a recorded ObjectDefine must activate the same retained
setup owner without issuing another END or adding an artificial mid-transcript
completion fence. Failure cleanup must cover both hidden and activated owners.

Legacy Unissued and ClosedWithoutReplayResult descriptors retain their current
meaning; the latter cannot authorize old-cycle polls. The completed-EVENT
checkpoint must not admit an open BEGIN or an occlusion result without its
independently complete contributing history. Native model/code negatives must
cover stale cycle/epoch/identity, early completion, missing local END and
double restoration. Actual Metal tests must cover checkpoint polling followed
by reissue and warm recovery, and the original captured query/pixel oracles
remain required for workload qualification.

**R-HARN-REPLAY-9.10** A single-level, single-sample DEFAULT-pool 2D
A16B16G16R16F render-target texture may carry an exact arm-boundary initial
checkpoint. Its explicit version-2 color encoding must preserve the raw
little-endian RGBA16Float bits, with checked pitch `8 * width` and byte count
`8 * width * height`. Version-1 four-byte color encoding must retain its
existing meaning. Capture and replay must use matching physical RGBA16Float
storage without quantization or float-value conversion. Identity, descriptor,
encoding, pitch and extent must be validated before admitting completeness;
surface aliases must resolve to the authenticated parent texture/subresource.
Binding alone does not prove a captured pass overwrites missing initial
contents. Unsupported shapes, stale identities, transfer failures and
incomplete drains remain fail-closed. Native layout/encoding negatives and
actual Metal nonuniform bit-exact checkpoint/restoration evidence are required;
synthetic tests do not replace original-workload query and pixel qualification.
Capture itself must publish and complete pending initializer uploads even when
the ordinary queue has no source. It must hold exclusive checkpoint producer
admission through raw transfer completion; retaining the physical texture alone
does not exclude intervening writes. A caller-side upload flush/wait must not
be a hidden precondition of the accepted checkpoint oracle.

**R-HARN-REPLAY-9.11** The version-1 four-byte arm checkpoint also supports
single-level, single-subresource, non-MSAA DEFAULT-pool 2D render-target
textures with `D3DFMT_A8R8G8B8` and render-target usage only. All four bytes,
including meaningful non-opaque alpha, must survive snapshot and restoration
exactly. X8 alpha normalization remains exclusive to `D3DFMT_X8R8G8B8`.
The parent texture owns the seed; subresource aliases retain the authenticated
parent rather than receiving independent initial contents. This extension
does not admit standalone A8 surfaces, multi-mip, cube, volume, MSAA, or other
pool/usage combinations. Missing initial contents still require an actual
terminal-write proof; binding alone remains insufficient. Native shape/layout
negatives and actual Metal nonuniform A/B/A snapshot/restoration tests must
cover alpha preservation and parent/alias ownership before workload retry.
The exact DEFAULT A8 checkpoint must reuse exclusive producer admission from
pending-initializer publication through GPU transfer completion and host copy,
as required by R-HARN-REPLAY-9.10. It must validate physical BGRA8 storage and
must not require an external preflush, an ordinary source, or an oracle-only
upload wait. This does not change ordinary application readback policy.

**R-HARN-REPLAY-9.12** Capture payload storage may spill immutable versions
to owner-scoped scratch storage without retaining their full contents in PE
address space. Reads, hashes, equality checks and publication must use bounded
windows and propagate I/O failure. CPU shadow contents and authenticated GPU
arm contents remain distinct authorities; a GPU checkpoint must not replace
the CPU base used by later partial mutations. A partial write stages a new
version and exposes it only after all copying and patching succeeds. Retained
leases prevent extent reuse, including after capture cancellation. Failed
staging must preserve prior bytes and retire tentative storage. Live scratch
bytes, reusable extents, physical high-water and resident heap capacity must
not be conflated. Scratch storage is not a durability or GPU-byte oracle.

**R-HARN-REPLAY-9.13** The version-1 four-byte arm checkpoint may also admit
single-level, single-subresource, non-MSAA DEFAULT-pool Texture2D render targets
with `D3DFMT_R32F` and render-target usage only. Capture and restoration must
preserve raw little-endian float32 bits in matching physical `R32Float`
storage, with checked pitch `4 * width` and extent `4 * width * height`.
No color swizzle, alpha normalization, quantization or float-value conversion
is permitted. The exact parent identity, generation, descriptor and subresource
remain authoritative for its Surface aliases.

Capture must hold the existing exclusive checkpoint producer lease from
pending-initializer publication through GPU completion and host copy, including
when there is no ordinary queued source. The capture entry must reject failed
identity, physical-format, extent, command-status or completion checks; generic
application readback is not a fallback. Initial contents remain mandatory for
shader-read-first use and must not be replaced by fabricated produced-pass
authority. Native layout/shape negatives and real Metal raw-bit A/B/A
snapshot/restoration tests, including pending-initializer-only setup, are
required before retrying original-workload qualification. This does not widen
standalone, managed, multi-mip, cube or volume checkpoint admission.

**R-HARN-REPLAY-9.14** Policy-qualified production input must carry a versioned,
digest-authenticated witness of the queue's actual tape-exact attachment-action
profile from construction through the captured interval. The capture binding
must observe this state and its change revision under the exact capture token,
epoch and queue identity; the publisher must not infer authority from environment
variables. An ABA policy change must invalidate stability even if arm and finish
values match. Revision exhaustion, replacement, stale binding or an unstable
interval must not produce a qualified witness. Frozen size-query/copy transfer
must preserve the same witness and transactional publication must bind it to
the unchanged event, identity and topology components. Missing legacy evidence
means unknown, not preservation-disabled or policy-neutral. Such inputs may
remain inspectable and usable by explicitly labelled private diagnostics, but
must not qualify policy-performance comparison. This witness does not replace
query, semantic mapping, original-pixel, reset or repetition gates.

**R-HARN-REPLAY-9.15** GPU arm-checkpoint payload admission must reserve retained
and transient host-byte budgets before allocating the PE destination or calling
the snapshot bridge. The retained aggregate limit is the normalized capture
blob-byte limit (at most 1 GiB), independently accounted before deduplication;
the per-subresource limit is the smaller of that limit and 64 MiB. Additional
host staging is conservatively reserved as twice the subresource byte count
against the smaller of twice the retained limit and 128 MiB. Zero, overflow,
unsupported sizes, allocation failures and exhausted credit must fail only
capture admission, without fabricating checkpoint bytes or changing app results.
The retained reservation must follow snapshot, seed and immutable publication
ownership without double charging or release before the associated bytes are
destroyed. Shared immutable references retain one charge until the final owner
releases it. Independent alias snapshots are charged independently; equal
dimensions or hashes do not establish shared physical backing. Abort/rearm must
not reset live reservations. The Unix arm-snapshot entry points must reject
over-limit resource sizes before their own materialization/readback allocations.
This contract bounds admitted GPU arm payload and explicit staging requests,
not allocator overhead, driver memory, process RSS, CPU scratch storage or
Present-oracle storage.

**R-HARN-REPLAY-9.16** A V4 policy candidate may change physical source and
DrawRun grouping only through a checked correspondence to the unchanged
canonical event/raw/record/subdraw stream. Correspondence must use actual
candidate attempts, replay acknowledgments, command origins and completed
source lifecycles; captured physical indices are not candidate identities.
Every original effect must map exactly once, with unchanged semantic order,
state-only/coordinator disposition and qualified query identity/issue history.
Query END prefixes must cover the same original pre-END work, excluding younger
work, even when source partitions differ. Generated command expansion requires
an explicit origin correspondence, not count-only or pixel-only acceptance.
Recorded publication expectations may remain deferred while a candidate writer
spans their original cut. Traversal acknowledgment then means only that a bounded
obligation was registered; success requires every obligation to be discharged
against actual candidate publications. No artificial flush, fabricated receipt,
or forced reference partition may discharge it. Unsupported initializer or
command correspondence must reject explicitly. Exact-source diagnostic mode
retains its strict original-cut contract. This correspondence alone establishes
neither query readiness, pixel equality, reset safety nor timing qualification.

**R-HARN-REPLAY-9.17** CPU General replay must use one explicit input admission
budget across grammar and execution, including body and bootstrap work,
resource/blob extents and query evidence. Publication capacity and progress
limits are separate dimensions: event count must not stand in for source count.
Extended workload profiles must be explicitly selected and bounded before
effects; default admission must not silently grow. Arithmetic, index and
budget inconsistencies must reject before tape resource construction or replay.
Runtime publication accounting must count actual production publications and
fail explicitly when the admitted capacity/progress envelope is exhausted.
Admission does not authorize a different input, reduced transcript, renderer
route substitution or synthetic completion.

**R-HARN-REPLAY-9.18** CPU-only V4 policy execution must authenticate the same
original event, identity, topology and action-witness components as its paired
Metal qualification, while reporting a distinct controlled-queue evidence
domain. A queue-bound, lifetime-qualified test capability must establish
exclusive CPU-fixture ownership before this domain can be armed. Worker,
offload, device/root replacement or identity mismatch must invalidate that
authority. CPU submission and acknowledgment must consume actual published
sources through the queue lifecycle; neither captured completion watermarks
nor expected source rows authorize a completion. All raw/record coverage,
query identity/prefix and semantic correspondence obligations still apply.

Controlled CPU completion must never create Encode invocation/placement rows,
GPU completion, numeric occlusion results or pixel authority. The production
V4 finish ABI and Metal qualification must retain their full topology and
lifecycle gates. CPU provenance-only finalization must have a separately typed
result, not a caller-selected relaxation of the production finish validator.
Negative tests must reject cross-domain promotion, stale capability and root,
missing acknowledgment, uncompleted sources and forged END coverage.

CPU replay may drain genuinely published work only at a proven unlocked
boundary or through its explicitly controlled consumer. Observer callbacks
under the queue lock must not reenter the queue. It must not publish merely
because a raw or original reference cut ended. Admission must bound the work
between consumer progress opportunities against real queue headroom, so an
unsupported single-thread progress envelope rejects before effects rather
than hanging. Setup checkpoints may complete before the arm boundary; they
must retain CPU-shadow and GPU-checkpoint payload authorities separately and
must not move interval mutations or readiness events into setup.

The controlled consumer must join before capability revocation, fixture reset
or wrapper destruction. Failure after dequeue or submission must preserve the
actual identity and lifecycle phase. Queue-owned quarantined work is not
completion, retirement or successful cleanup; cleanup evidence must remain
incomplete until the corresponding owner is genuinely retired or torn down.
No cleanup path may fabricate an acknowledgment for unsubmitted work.

**R-HARN-REPLAY-9.19** Capture enablement and attachment-action selection must
be independently selectable for a capture-only Normal diagnostic. With
`DXMT9_RENDER_TAPE_CAPTURE` enabled, absent or empty
`DXMT9_RENDER_TAPE_ACTION_PROFILE`, or explicit `tape-exact`, must preserve
the existing Exact policy from queue construction. Explicit `normal` must
retain ordinary attachment actions from construction without toggling Exact at
arm or freeze. Unknown action names must disable PE capture and must not enable
the Unix Exact override. When capture is disabled the selector must have no
effect. PE and Unix must use the same closed selection predicate.

Normal capture must publish the actual queue-observed action history using the
existing authenticated witness, not an environment-derived attestation. Existing
seed budgets, missing-content, open-query-cut, generation, publication and
failure gates must remain unchanged. Successful byte readback alone must not be
claimed as proof of semantically defined attachment contents. A Normal bundle
may be inspected, but must remain ineligible for the existing stable-Exact
CPU/GPU policy qualification. It grants neither Normal replay authority nor
unperturbed application timing. No `.gputrace` replay time may be attributed to
dxmt9 Replay/Encode CPU work.

**R-HARN-REPLAY-9.20** Production Normal capture must observe bounded initial
attachment history at the same quiescent queue admission as its identity
baseline. Each row must preserve the generation-qualified surface identity,
storage/subresource alias, color-touched bit and current-backbuffer membership.
Post-Present discard and queue/initializer completion watermarks belong to the
same observation. Registry census must retain the complete admitted set before
observing queue state; capacity or allocation failure must acquire no partial
set. Releases must occur outside registry and queue locks.

History is diagnostic evidence, not content validity: every current row must
explicitly carry unknown content authority. Missing rows and legacy absence
must remain unknown, never implicitly untouched. Replay setup must not mint
original history. A versioned transfer and published byte-digest binding must
join history to the actual capture token/epoch/queue and baseline. Invalid or
partial history must fail capture rather than silently omit the observation.
No hot-path signature or per-draw observation is required. This read-only cold
slice changes no load/store decision, lifetime waterline or scheduling policy;
native bounded truth tables and real admission tests bind its observation and
failure predicates. Existing lifecycle models remain regression gates. Normal
restoration, content authority and original GPU comparison remain separate
prerequisites for replay qualification.

**R-HARN-REPLAY-9.21** A cold Normal attachment-history restoration primitive
must consume a one-use, bounded plan joined to exact original surface
generations and independently resolved, retained local handles. Required
surface coverage, current-backbuffer membership, logical alias equivalence and
touch agreement must be checked before mutation. Captured sequence labels must
never authorize local completion: the plan must bind a local queue incarnation
and exact published queue/initializer prefix, and restoration must independently
observe completion and writer quiescence at that prefix.

Restoration must reject active capture, borrowed builders, live queued work,
pending/failed initialization, terminal state, dead handles, policy changes and
current-backbuffer mismatch. Allocation or validation failure must leave all
touch/discard state unchanged. Success restores original touched bits and
post-Present discard independently of seed writes, without publishing, waiting,
changing policy or claiming defined contents. This primitive is not Normal
replay admission; authenticated provider integration, initial-content proof and
original GPU comparison remain required.

**R-HARN-REPLAY-9.22** A cold initial-setup planner may accept an explicit,
bounded list of exact late produced-texture generations required for Normal
attachment-history resolution. Such a request must be checked against the
authenticated complete source, not caller-supplied descriptor claims. It may
produce only allocation ownership with zero seed promises; the defining
clear/write, original logical definition and source publication cuts must remain
unchanged. Absent, stale, duplicate, unsupported or already seeded requests must
reject transactionally. Allocation footprint must be bounded independently of
seed bytes. An empty request must preserve the historical setup plan. This
metadata admission does not qualify Normal policy, local completion, defined
contents or GPU output, and must not silently enable a runtime preparation lane.
