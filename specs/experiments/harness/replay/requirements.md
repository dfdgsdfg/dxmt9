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
The gamma-bearing 64-byte bootstrap header is baseline profile version 2.
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
with one mip and one subresource, render-target usage, and exactly one
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

**R-HARN-REPLAY-7.18** Production capture and production-provider replay use
one tape-exact attachment-action policy. While either operation is active, an
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
