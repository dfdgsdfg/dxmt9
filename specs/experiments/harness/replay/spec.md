---
type: "Spec"
title: "Harness Replay Spec — Unified Render Replay"
description: "Unified draw-slice, frame-tape, and sequence-tape architecture plus the implemented mini-replay contract."
tags: [specs, experiments, harness, replay, spec]
---

# Harness Replay Spec — Unified Render Replay

Implements `specs/experiments/harness/replay/requirements.md`
(`R-HARN-REPLAY-*`). Instantiates the `replay` row of the domain map in
`specs/experiments/harness/spec.md` §1 and the `dump-extract →
offline-replay` and `offline-replay → external-join` boundaries in
that spec's §2. Stage names, boundary names, and envelope fields are
cited from the parent spec rather than redefined here. The geometry
`.meta` slice contract this domain consumes is defined once,
authoritatively, in `specs/experiments/harness/probe/spec.md` §5; it
is cited below, not restated.

The implemented `draw-slice` profile renders a valid image as of
2026-07-28 (§7, Defect History). The bounded `frame-tape` profile has captured-
bundle production-provider identity, deterministic fresh-device repetition,
and closure-aware whole-command reduction/bisection. The bounded
`sequence-tape` profile replays exactly two Present intervals separated by one
complete texture mutation and now has an explicit opt-in PE capture/publication
owner plus captured production-provider identity; broader grammar remains a
tracked gap.
They reuse the replay domain rather than creating
a second, competing harness family. Where
current source still violates a contract — the 32-bit index-width rejection
of §6.3 remains unimplemented — this file says so explicitly, and
`specs/experiments/gap.md` tracks the shortfall.

Facts in this document were verified against
`scripts/tools/run_3dmark05_mini_replay.py` (2,093 lines) and
`scripts/tools/build_3dmark05_mini_replay_manifest.py` (666 lines) on
2026-07-27; line numbers are cited per fact and should be re-checked
against the live file before further citation, per the same discipline
`specs/experiments/harness/probe/spec.md` §3 states for its own line
citations.

---

## 0. Unified Replay Profiles

The replay domain has one evidence vocabulary and three scopes:

| Profile | Status | Captured scope | Reference execution |
|---|---|---|---|
| `draw-slice` | implemented | Selected draws from one encoder in one captured frame, with extracted geometry, shaders, constants, textures, and attachments | Generated standalone Metal program owned by the three scripts in §1 |
| `frame-tape` | bounded implementation | Structural v2 event tape, bounded PE capture owner, production-provider identity host, exact native offscreen oracles, captured-bundle identity for full-surface `Clear` and non-uniform textured-UP intervals, fresh-device repetition, whole-command reducer/bisect, and a structural draw-range projection planner | Broaden the fail-closed capture grammar and turn a validated projection into an executable draw-slice only after indexed/VB/IB/shader/attachment closure exists |
| `sequence-tape` | bounded two-interval implementation | Exactly two complete textured-UP Present intervals separated by one complete digest-backed texture mutation, with per-interval completion/output conservation and repeat identity; explicit PE capture/publication defers sealing and publication until interval 2; a captured Sikarugir bundle reproduces both ordered output digests through the production provider | Widen the interval count or grammar only with new closure and oracle evidence |

`draw-slice` remains deliberately small and shader/geometry-centric. It is
useful for code-path mutation, pixel isolation, and inexpensive GPU experiments,
but it does not reproduce a complete frame or the production queue. Full tapes
solve that larger problem; they do not retroactively strengthen evidence from an
old mini-replay manifest.

All profiles use the parent artifact envelope and the same four evidence words:
`validity`, `coverage`, `conservation`, and optional `benchmark`. Their contents
differ by scope, but their meanings do not. In particular, output equality is
not coverage, containment is not execution, and successful replay is not a
performance claim.

---

## 1. Script Inventory

| Script | Role |
|---|---|
| `scripts/tools/build_3dmark05_mini_replay_manifest.py` | Joins a `join`-domain shader-dump summary CSV, a `reduce`-domain indexed-probe-draw CSV, and `probe`-domain geometry `.meta`/`.bin` payloads into one `dxmt9.3dmark05.mini_replay_manifest.v1` JSON file (§2) for a filtered, row-local draw set. |
| `scripts/tools/plan_3dmark05_mini_replay.py` | Read-only readiness report: joins the same three artifact families and states which rows have shader source, geometry bytes, and index/state identity available, without claiming a replay is runnable. Does not write a manifest. |
| `scripts/tools/run_3dmark05_mini_replay.py` | The domain's core: rewrites dumped MSL for standalone binding (§6), applies primitive/vertex/draw-order and diagnostic transforms (§4), generates an Objective-C++ Metal replay source, optionally compiles and runs it. |

`plan_3dmark05_mini_replay.py` is a diagnostic report generator, not a
manifest producer; it is listed here because the parent domain map
(`specs/experiments/harness/spec.md` §1) assigns it to this domain,
not because it participates in the same manifest-build → replay
pipeline as the other two scripts.

---

## 2. Manifest Schema

`dxmt9.3dmark05.mini_replay_manifest.v1` is the schema string both
sides of the manifest boundary check verbatim:

```
$ grep -n "mini_replay_manifest.v1" scripts/tools/build_3dmark05_mini_replay_manifest.py
598:        "schema": "dxmt9.3dmark05.mini_replay_manifest.v1",
```

`build_3dmark05_mini_replay_manifest.py:598` writes this string;
`run_3dmark05_mini_replay.py`'s `load_manifest()` (line 38) checks
`data.get("schema") != "dxmt9.3dmark05.mini_replay_manifest.v1"` and
raises `SystemExit(f"unsupported manifest schema: {data.get('schema')}")`
naming the actual mismatched value on failure — this one check already
satisfies R-HARN-REPLAY-5.2's naming discipline and is cited here as a
positive existing pattern, not a defect.

A manifest's top-level shape is `{schema, sources, summary, draws}`.
Each entry in `draws` carries `row` (`"<seq>/<enc>"`), `seq`,
`encoder`, `encoder_draw_index`, `draw_ordinal`, and four nested
objects:

- `state` — primitive/index counts, `base_vertex`, `stream0_offset`/
  `stream0_stride`, blend/depth/scissor/cull/fill render state, and
  `texture_mask`/`color_write` bitfields.
- `shaders` — VS/PS hash, resolved `vs_file`/`ps_file` MSL paths (and
  whether each was matched by draw-hash or fell back to the row's own
  shader-summary file, `vs_file_source`/`ps_file_source`), and
  `vsout_fields`/`ps_vsout_read_fields` layout metadata.
- `geometry` — `index_file`/`stream0_file` paths and byte counts, a
  `streams` array (§3) covering stream0 and any extra streams 1-15,
  and index-reuse/cache-miss estimates carried through from the
  geometry `.meta` file.
- `uniforms` — per-cbuf-kind (`vsconsts`/`psconsts`/`ffpvs`/`ffpps`)
  file path and byte count, present only when the source dump wrote
  that cbuf.
- `textures` — up to 16 fragment-stage texture-slot entries with
  handle, format, and dimensions, consumed by
  `run_3dmark05_mini_replay.py`'s `--texture-input-dir`.
- `attachments` — up to 4 color surfaces plus one depth surface,
  each carrying `format`, `width`, `height`, and alias-texture fields.

`run_3dmark05_mini_replay.py`'s own `validate_payloads()` (lines
50-71) checks, for every draw, that `index_file`/`stream0_file` exist
and that `index_bytes`/`stream0_bytes` are positive, and for any
present cbuf kind, that its file exists — each failure raises
`SystemExit` naming the draw index and the missing key or path. This
is the domain's existing R-HARN-2.1-compliant behavior for payload
presence; it does not check payload *content* — R-HARN-REPLAY-3.1's
output-validity assertion is downstream of this check, not covered by
it.

---

## 3. Input Contract From `probe`

The geometry `.meta`/`.bin` slice contract this domain consumes is
defined once in `specs/experiments/harness/probe/spec.md` §5 (in turn
citing `specs/experiments/harness/spec.md` §2's `dump-extract →
offline-replay` boundary). This document does not restate the slice
rule; it states only how this domain's own source names line up with
that contract.

`build_3dmark05_mini_replay_manifest.py`'s `stream_payload_metadata()`
(lines 96-126) copies each stream's declared fields verbatim from the
`.meta` `key=value` pairs into the manifest's `geometry.streams[]`
entries: `offset` (from `stream{N}_offset`), `stride`
(`stream{N}_stride`), `start_byte` (`stream{N}_start_byte`), and
`byte_count` (`stream{N}_byte_count`) — the same four fields probe
spec.md §5 names as load-bearing, for stream0 and, identically, for
every stream `1..15` that the `.meta` file recorded as written.

`run_3dmark05_mini_replay.py`'s `resolve_stream_payload_offset()`
(lines 254-274) is this domain's consumer-side derivation:

```python
payload_offset = offset - start_byte
if payload_offset < 0:
    raise SystemExit(
        f"draw {ordinal}: stream{stream_index} start_byte {start_byte} "
        f"exceeds offset {offset}"
    )
```

This is called from two sites: `remap_draw_vertex_layout()` (line 468,
for the `--vertex-order` transform) and `render_source()` (line 995,
for the stream0 offset baked into each generated `DrawEntry`). Both
sites pass the manifest's own declared `offset`/`start_byte` for the
stream in question — never an assumed `offset == 0` or an assumed
equality between the two fields — satisfying R-HARN-REPLAY-4.1/4.2.
This function and its two call sites are commit `12348666`'s fix for
defect 2 (§7); the fix is stated here as the domain's present,
correct behavior, not as a deviation.

---

## 4. Mode Table

`run_3dmark05_mini_replay.py --help` enumerates 19 `--`-prefixed
option lines as of 2026-07-29:

```
$ python3 scripts/tools/run_3dmark05_mini_replay.py --help | grep -oE '^  --[a-z-]+' | sort -u | wc -l
19
```

The anchor is exactly the two spaces argparse indents an option line
with. The looser `'^\s+--[a-z-]+'` this section used while the count
was 18 also matches a wrapped help-text continuation line that happens
to begin with a flag name, which `--prove-executed`'s help does, so it
over-counts.

Per R-HARN-REPLAY-6.1, every one of them alters this domain's output —
the generated Objective-C++ replay source, the compiled binary's
runtime behavior, or a written artifact — so all 19 are tabled below;
there is no exception bucket comparable to the `probe` domain's §7.2/
§7.2b (this script has no finalizer-passthrough flags and no flags
that set nothing).

| Flag | Effect on output |
|---|---|
| `manifest` (positional) | Selects the input manifest file; determines the whole draw/shader/attachment set replayed. |
| `--output-dir` (required) | Directory for the generated `.mm` source, compiled binary, per-shader-variant `.replay.metal` files, `index-order`/`vertex-order` subdirectories, and `mini-replay-summary.json`. |
| `--width` / `--height` | Fallback render-target dimensions (default `1024`/`768`) used only when the draw's own color/depth attachment metadata supplies neither (`pass_width`/`pass_height` in `render_source()`, lines 969-970). |
| `--primitive-order` | `original` / `reverse-triangles` / `sort-min-index` / `sort-max-index` / `cache-opt-lru32` / `cache-opt-lru64`; rewrites each draw's uint16 triangle-list index payload before replay (`transform_index_payload()`, lines 202-226). Diagnostic; changes primitive order. |
| `--draw-order` | `original` / `reverse`; reorders the manifest's draw list before replay (`materialize_replay_draws()`, lines 521-524). |
| `--vertex-order` | `original` / `first-reference` / `scatter`; permutes vertex storage across every stream and rewrites indices in place, preserving triangle order, vertex bytes, payload size, and `base_vertex` (`remap_draw_vertex_layout()`, lines 428-494). Diagnostic locality discriminator. |
| `--trim-vsout-to-fs-reads` | Trims the shared `VSOut` struct to only the fields the paired FS's `stage_in` reads (plus `position`/`texcoord0` fallbacks) via `apply_vsout_read_trim()` (lines 687-706). Diagnostic; changes VS/FS source shape. |
| `--force-fragment-color` | Replaces the compiled `dxmt9_fs` function body with a magenta constant. The rewrite is struct-aware as of `fe673fd5`: it targets whichever of the three return shapes the captured shader declares, via `dxmt9_make_fs_out` / `dxmt9_make_ffp_fs_out` for `FSOut` / `FfpFsOut` (§6, §7 defect 5). |
| `--force-fragment-primitive-id` | Adds a `[[primitive_id]]` parameter and replaces `dxmt9_fs`'s body with a primitive-id-encoded color (`force_fragment_primitive_id_source()`, lines 759-771). Not one of the five defects this document was scoped to verify, but its body replacement is the same `replace_function_body(source, "dxmt9_fs", "{ ... return float4(...); }")` shape as `--force-fragment-color` (line 761) against a function signature whose declared return type it does not inspect; this domain has not independently exercised this flag against a real captured shader. The more consequential unconfirmed case is an `FSOut`-returning translated shader — the dominant real shape for captured per-draw PS (§6) — not only an `FfpFsOut`-returning FFP shader, so whether it shares defect 5's compile failure is unconfirmed here, not ruled out. |
| `--compile` | Invokes `xcrun -sdk macosx clang++ -std=c++20 -fobjc-arc -O2 ...` (`compile_source()`, lines 1911-1925) to build the generated source into `<output-dir>/dxmt9-3dmark05-mini-replay`. |
| `--run` | After compiling, runs the binary via `subprocess.run` (`run_binary()`, lines 1954-1966), forwarding `DXMT9_MINI_REPLAY_*` env (§5). |
| `--repeat` | Sets `DXMT9_MINI_REPLAY_REPEAT` in the subprocess environment (default `1`); the binary re-issues every draw in the encoder that many times. |
| `--depth-clear` | Clear value baked directly into the generated source's `pass.depthAttachment.clearDepth` (default `1.0`); ignored when `--depth-input` is also given (load action becomes `MTLLoadActionLoad`, §6). |
| `--depth-input` | Path to a raw depth-attachment sidecar (from `DXMT9_DUMP_DEPTH_ATTACHMENT_PATH`) baked into the generated source and blit-uploaded before the render pass; switches `depth_load_action` from `Clear` to `Load`. Missing-file check is a bare `if depth_input and not depth_input.exists(): raise FileNotFoundError(depth_input)` (`prepare()`, lines 1756-1757) — it fails loudly (non-zero exit via uncaught exception) but with Python's default `FileNotFoundError` message rather than this domain's usual `SystemExit(f"...: {path}")` phrasing used elsewhere in `prepare()`/`validate_payloads()`. |
| `--texture-input-dir` | Directory of `texture-*.json` sidecars (from `--dump-draw-texture-handles`) loaded by `load_texture_sidecars()` (lines 860-915) and baked into the generated source's `TextureEntry`/`TextureSubresourceEntry` arrays and per-draw fragment-texture-slot indices. |
| `--capture-path` | Forwarded as `DXMT9_MINI_REPLAY_CAPTURE_PATH` (plus `MTL_CAPTURE_ENABLED=1`) to the replay binary's own subprocess environment when `--run` is also given; gated by `--min-capture-free-mb`'s free-space check (`check_capture_free_space()`, lines 1940-1951) before the binary is invoked. |
| `--color-output` | Forwarded as `DXMT9_MINI_REPLAY_COLOR_OUTPUT_PATH`; the binary blit-reads the color attachment back to a buffer and writes a PPM image (`writePpm()`, generated source lines ~1238-1263). |
| `--min-capture-free-mb` | Free-space guard (MiB) checked before `--capture-path` is used when `--run` is given; default `2048`, overridable by `DXMT9_MINI_REPLAY_MIN_CAPTURE_FREE_MB` (`parse_min_capture_free_mb()`, lines 1928-1937). |
| `--prove-executed` | `'REGEX=>REPLACEMENT'`. Prepares a second, mutated shader tree and replay binary under `<output-dir>/execution-proof/`, replays it, compares the two images, writes `execution_proof` into the summary, and exits non-zero on either failing verdict (§7, defect 7; R-HARN-REPLAY-3.8). Requires `--run` and `--color-output`. A pattern that matches no site is decided from the generated sources before anything is compiled, and is reported as `not-present`, never as an execution verdict. |

`build_3dmark05_mini_replay_manifest.py`'s own filter flags
(`--row`, `--vs`, `--ps`, `--payload-selection`,
`--encoder-draw-min`/`-max`/`-indices`, `--draw-ordinals`,
`--max-draws`) also alter output — which draws land in the built
manifest — but are not re-tabled here with the same per-flag detail as
the table above, because the brief this document was written against
scoped its grep-reproducible mode-table verification to
`run_3dmark05_mini_replay.py --help` only; a future revision that adds
an equivalent grep-verified count for the manifest builder should
extend this section rather than silently assume its flags are covered
by the table above.

---

## 5. Environment Variables

| Var | Set by | Read by | Purpose |
|---|---|---|---|
| `DXMT9_MINI_REPLAY_REPEAT` | `run_binary()` from `--repeat`, in the subprocess environment | Generated replay binary (`std::getenv`, generated source ~line 1621) | Number of times to re-issue every draw in the single render encoder. Default `1` when unset/unparseable (`std::max(1, std::atoi(env))`). |
| `DXMT9_MINI_REPLAY_CAPTURE_PATH` | `run_binary()` from `--capture-path`, only when non-`None` | Generated replay binary, to configure a `MTLCaptureDescriptor` with `MTLCaptureDestinationGPUTraceDocument` | Output path for a `.gputrace` capture of the single replayed command buffer. |
| `DXMT9_MINI_REPLAY_COLOR_OUTPUT_PATH` | `run_binary()` from `--color-output`, only when non-`None` | Generated replay binary, to gate the color-attachment blit-readback, the `writePpm()` call, and the binary's own degenerate-image check (§7, R-HARN-REPLAY-3.7) | Output path for the replayed color attachment as a PPM image. |
| `DXMT9_MINI_REPLAY_MIN_CAPTURE_FREE_MB` | Caller's process environment (this domain never sets it into a subprocess) | `run_3dmark05_mini_replay.py`'s own argument parser, as the default source for `--min-capture-free-mb` (line 2073) | Overrides the free-space guard threshold without requiring `--min-capture-free-mb` on every invocation. |

These four are documented identically in
`agents/rules/environment_variables_capture.rules.md` (Mini-replay /
payload capture section); this table adds only the set-by/read-by
direction that file does not itself state.

---

## 6. Declared Engine-Shape Expectations

### 6.1 Cbuf-binding shapes

This domain's `transform_msl()` (`run_3dmark05_mini_replay.py:590-638`)
recognizes exactly two dumped-MSL cbuf-binding shapes, per
R-HARN-REPLAY-5.1:

**Legacy argument-buffer shape** — present when
`has_argbuf_parameter()` (line 558) matches
`constant\s+ArgbufLayout&\s+abuf\s+\[\[buffer\(30\)\]\]` in the dumped
source. `replace_argbuf_parameter()` (lines 564-573) rewrites that one
parameter into two plain constant-buffer parameters allocated from the
free Metal buffer-slot range 0-29 excluding slot 30 and any slot the
shader already uses (`allocate_cbuf_slots()`, lines 546-555), and
strips the `constant VsConsts& vsConsts = *abuf.vsConsts;` /
`constant PsConsts& psConsts = *abuf.psConsts;` (and `Ffp*`)
local-alias lines the argbuf shape otherwise requires.

**Current direct-cbuf shape** — present when
`DXMT9_ARGBUF_DIRECT_CBUF` (default-on per
`agents/rules/environment_variables_encoder.rules.md`) causes the
engine to bind constants directly instead of through an argument
buffer. Verified against
`src/dxmt9/dxmt9_shader_metal_ir.cpp:1502,1505,2706-2739`: the emitted
VS signature carries `constant VsConsts& vsConsts [[buffer(0)]]` and
`constant FfpVsConsts& ffpVs [[buffer(3)]]`; the emitted FS signature
carries `constant PsConsts& psConsts [[buffer(0)]]` and
`constant FfpPsConsts& ffpPs [[buffer(3)]]`. `find_direct_cbuf_slot()`
(lines 576-587) locates these by name and slot without rewriting
anything — buffer 0 and buffer 3 do not conflict with this domain's
own reserved vertex-buffer slots 1 (stream0) and 5 (`DrawVolatile`,
`setVertexBytes:...atIndex:5`).

`transform_msl` raises when neither shape is found for a stage
(§2, `load_manifest`'s sibling failure-naming pattern). No third shape
is declared or handled; per R-HARN-REPLAY-5.3, recognizing one would
require both a new `transform_msl` branch and an update to this
section, not a broadened regex on either existing branch.

### 6.2 Fragment return-type shapes

Separately from the cbuf-binding shapes in §6.1, dxmt9 declares
`dxmt9_fs` with one of **three** distinct return types depending on
which emitter produced the dumped MSL. This domain's diagnostic
fragment-body rewrites (`--force-fragment-color`,
`--force-fragment-primitive-id`, §4) depend on this shape even though
`transform_msl` does not — they replace the function body but never
inspect or adapt to the declared return type, which is the exact gap
this section
exists to make explicit.

**`FSOut`** — the shape every real translated per-draw pixel shader
declares, because `src/dxmt9/dxmt9_shader_metal_ir.cpp` hardcodes
`const bool usesFragmentOutStruct = true;` unconditionally
(`dxmt9_shader_metal_ir.cpp:2576`; there is no code path where this is
`false`). The struct itself (`dxmt9_shader_metal_ir.cpp:2598-2606`) has
a per-variant field layout — `color0..colorN [[color(i)]]` for
`colorOutputCount` render targets, an optional `float depth
[[depth(any)]]`, and a `uint sampleMask [[sample_mask]]` — and the
emitter also always emits a matching constructor helper,
`inline FSOut dxmt9_make_fs_out(float4 color, uint sampleMask)`
(`dxmt9_shader_metal_ir.cpp:2607-2617`). `fragmentReturnType` is
assigned via
`const char* fragmentReturnType = usesFragmentOutStruct ? "FSOut" : "float4";`
at `dxmt9_shader_metal_ir.cpp:2637` — a ternary whose `"float4"` false
branch is dead code today, since `usesFragmentOutStruct` is hardcoded
`true` at `:2576` with no code path that sets it `false`. The
resolved value, `"FSOut"`, is used verbatim at every `dxmt9_fs`
emission site (`:2658`, `:2686`, `:2702`, `:2720`, `:2732`). Because
this is the shape for every translated per-draw
pixel shader — exactly what `probe`-domain geometry dumps capture and
this domain replays — `FSOut` is the **dominant real case**, not a
secondary one.

**`FfpFsOut`** — the shape every fixed-function-pipeline (FFP) shader
declares, unconditionally
(`src/dxmt9/dxmt9_ffp_shaders.cpp:1109,1129,1150,1172,1181`). A
distinct, simpler struct — `float4 color [[color(0)]]` plus `uint
sampleMask [[sample_mask]]` (`dxmt9_ffp_shaders.cpp:1053-1056`) — with
its own matching constructor helper, `inline FfpFsOut
dxmt9_make_ffp_fs_out(float4 color, uint sampleMask)`
(`dxmt9_ffp_shaders.cpp:1057-1062`).

**Bare `float4`** — confined to `src/dxmt9/dxmt9_shader_sources.cpp`'s
internal utility shaders: `makeGenericFragmentSource`
(debug-fill, line 124), `makeTexturedFragmentSource` (internal blit,
line 152), and `makeGammaApplyFragmentSource` (gamma-ramp apply, line
178). None of these is ever the per-draw pixel shader a `probe`-domain
geometry dump captures or this domain replays — they exist only for
dxmt9's own internal presenter/blit paths, which this harness has no
mechanism to dump or reach.

`--force-fragment-color` originally emitted `float4(1.0f, 0.0f, 1.0f,
1.0f)` returned bare, matching only the third, never-replayed shape;
against the two shapes real captured shaders actually declare —
`FSOut` and `FfpFsOut` — that was a return-type mismatch and failed to
compile. `fe673fd5` made the rewrite select the shape the captured
shader declares (§7, defect 5).

### 6.3 Declared index width

Per R-HARN-REPLAY-6.3, this domain declares which index width(s) its
replay binary supports, the same way §6.1 declares the accepted
cbuf-binding shapes. Today it supports exactly one: 16-bit. The
generated `drawIndexedPrimitives:` call
(`run_3dmark05_mini_replay.py:1703-1705`) is emitted with a literal
`indexType:MTLIndexTypeUInt16`; there is no branch, and no other
`indexType:` literal, for a 32-bit source index buffer anywhere in
this script.

R-HARN-REPLAY-6.3 also requires that a manifest draw whose original
D3D9 index format is 32-bit be rejected with a diagnostic naming the
unsupported width, rather than replayed as if it were 16-bit. That
check does not exist yet: nothing in `run_3dmark05_mini_replay.py`
inspects the manifest's `index_type` field before emitting the
hardcoded `MTLIndexTypeUInt16` call, so a 32-bit-indexed draw is
replayed with the index buffer bytes silently reinterpreted as
16-bit indices instead of being rejected. This is a declared
requirement without a matching implementation, not a description of
an existing check; `specs/experiments/gap.md` is where that
implementation shortfall is tracked, per this project's own
gap-tracking convention.

---

## 7. Defect History

Five defects blocked a vertex-remap experiment run against this
domain on 2026-07-25/27 (parent `requirements.md`'s introduction), and
a sixth was found while debugging them. A seventh — a verification
defect rather than a rendering one — was found on 2026-07-29 after the
domain was already rendering correctly. **All seven are now fixed**,
the last on 2026-07-29. They are the reason this domain's documents
exist, and each is recorded below with its fix so a future reader can
tell what the contract is protecting against.

1. **Fixed in `12348666` (defect 2).** Sliced stream payloads
   double-counted `stream0_offset` as a payload-relative offset,
   producing negative slot capacities. `resolve_stream_payload_offset()`
   (§3) is the fix; it is now this domain's stated, correct behavior,
   not an open deviation.

2. **Fixed in `e2d3ed0e` (defect 3).** `color_pixel_format()`
   recognized `core::Format` values 1-4 and returned
   `MTLPixelFormatRGBA8Unorm` for every other value with no diagnostic
   naming the unrecognized format. Row `60/0` renders to R32F
   (`core::Format` 16), which fell into that silent-fallback branch, so
   the replay rendered into a wrong-format attachment with no
   indication the format was unrecognized. `depth_pixel_format()` had
   the identical shape for depth formats outside `{40, 41, 49, 42,
   46}` (R-HARN-REPLAY-2.1). Both now fail loudly on an unrecognized
   value instead of substituting a fallback, which is what
   R-HARN-2.1's no-silent-degradation contract requires. The
   *resolved* formats are now recorded in `mini-replay-summary.json`
   under `attachment_formats` (R-HARN-REPLAY-2.3) — each
   `MTLPixelFormat` name beside the `core::Format` ordinal it came
   from, the attachment dimensions, and whether it came from a
   declared attachment or the legacy no-`attachments` default.
   `resolve_attachment_formats()` is the single resolution:
   `render_source()` bakes into the generated `.mm` the same record
   `prepare()` writes to the summary, so the artifact cannot name a
   format the replay did not render with.

3. **Fixed in `36a41ad5` (defect 4) — every replay lane rendered
   fully black.** All four lanes captured during the vertex-remap
   experiment reported `mini replay draws=229 repeat=1` and exited 0
   while each wrote a 1024×768 PPM containing exactly one distinct
   pixel value. The R32F format gap (defect 3) did not explain it: row
   `60/1` renders to X8R8G8B8 — `core::Format` 2, handled natively by
   `color_pixel_format()` rather than through the unrecognized-value
   fallback — and its four lanes were also fully black.

   **Root cause: the generated program bound nothing to fragment
   `buffer(5)`.** dxmt9 emits a `constant FsVolatile& fsVolatile
   [[buffer(5)]]` parameter alongside the generated alpha-test tail
   (`dxmt9_ffp_shaders.cpp:1098-1099`,
   `dxmt9_shader_metal_ir.cpp:2643`), and the shader drives from its
   contents both an alpha-test switch ending in `discard_fragment()`
   and the `[[sample_mask]]` output. All 17 shader variants in the GT1
   frame60 `enc1` capture declare it. Reading an unbound Metal buffer
   is undefined; with nothing bound, no fragment survived to write.
   The vertex stage had always bound its own `DrawVolatile` at the
   same index — only the fragment volatile was omitted. The fix emits
   a per-draw `FsVolatile` matching the engine struct
   (`src/dxmt9/dxmt9_draw_state.hpp:131-139`) and binds it with
   `setFragmentBytes ... atIndex:5`. `sampleMask` must be
   `0xffffffffu`: the replay is always single-sample, and because that
   field feeds `[[sample_mask]]` directly, a zero would mask out every
   fragment in turn.

   Eliminated as causes before the root cause was found, each by a
   direct check: the depth sidecar (still black with `--depth-clear
   1.0`), the constant buffers (the shader's `VsConsts` is
   `float4[256] + int4[16] + uint[16]` = 4,416 bytes, and the dumped
   payload is exactly 4,416 bytes carrying real transform values),
   scissor (disabled, full-rect), cull and fill state, the render pass
   and colour attachment, PSO creation, and draw issue itself. The
   decisive step was changing the clear colour to magenta with real
   shaders bound: 100% of the clear survived, proving fragments ran
   but never wrote.

   Verified end-to-end after the fix: replaying the GT1 frame60 `enc1`
   manifest renders the recognisable "Return to Proxycon" interior,
   12,231 distinct RGB values over 784,476 of 786,432 non-black
   pixels, untextured because the replay binds white dummy textures.

   **What this defect cost is the reason R-HARN-3.1 exists.** The
   harness reported success on a degenerate artifact, so the failure
   had to be found by bisecting generated Metal source rather than
   being reported. Fixing the cause did not prevent a recurrence; the
   output-validity self-assertion that would catch the next one landed
   separately, with defect 7 below.

4. **Fixed in `fe673fd5` (defect 5) — the fragment-stage bisection
   tool did not compile, and against essentially all real captured
   shaders, not only FFP ones.** `--force-fragment-color` replaced
   `dxmt9_fs`'s
   body with a bare `return float4(1.0f, 0.0f, 1.0f, 1.0f);` without
   checking the function's declared return type. dxmt9 declares
   `dxmt9_fs` with one of **three** shapes, not two (§6.2): every real
   translated per-draw pixel shader declares `FSOut` — dxmt9
   hardcodes `usesFragmentOutStruct = true` unconditionally
   (`src/dxmt9/dxmt9_shader_metal_ir.cpp:2576`), so `FSOut` is the
   dominant real case for exactly the shaders this harness dumps and
   replays, not a secondary one; every FFP shader declares a different
   struct, `FfpFsOut`
   (`src/dxmt9/dxmt9_ffp_shaders.cpp:1109,1129,1150,1172,1181`); only
   dxmt9's internal blit/gamma-apply/debug-fill utility shaders
   (`src/dxmt9/dxmt9_shader_sources.cpp:124,152,178`) declare a bare
   `float4`, and those are never the per-draw shader this harness
   dumps or replays. `--force-fragment-color`'s rewritten body matches
   only that third, never-replayed shape, so it failed to compile
   against essentially every real captured shader — translated and FFP
   alike, not an FFP-only subset. The one diagnostic flag built
   specifically to separate "the geometry reaches the fragment stage
   but the fragment shader is producing wrong color" from "the
   geometry never reaches the fragment stage at all" was unusable
   exactly when defect 4 needed it. Rather than call either emitter's
   constructor helper, the fix reads the declared return type, parses
   its members and their `[[...]]` attributes, and assigns each one
   explicitly — `color(N)` gets the forced colour, `depth(...)` gets
   `0.0f`, `sample_mask` gets `0xffffffffu` — leaving no member
   uninitialized and degrading to the historical bare `return` only
   for the `float4` utility shape. An unrecognized attribute is a hard
   error rather than a silent omission. That member-walk also avoids
   assuming member names: `FfpFsOut`'s colour member is `color`, not
   `color0`. The flag then did the job it was built for — forcing
   magenta produced full-viewport coverage while the real shaders
   produced none, which localized defect 4 to the fragment stage.

5. **Fixed in `07c39ecb` (defect 6) — every draw was encoded with
   draw 0's pipeline, depth state, and cull mode.** The generator
   collapsed blend, colour-write, depth, and cull state into a single
   `first_state` sampled from the first draw in the manifest and
   applied it to all of them. For the GT1 frame60 `enc1` row that is
   provably wrong: its 229 draws carry three distinct
   `(color_write, depth_write, alpha_blend)` combinations — 42
   depth-only (`0x0, 1, 0`), 145 blended colour (`0xf, 0, 1`), and 42
   unblended colour (`0xf, 0, 0`) — so no single shared state can
   represent them. The fix derives per-draw blend, depth, and cull tuples and
   emits one PSO per distinct combination, indexed per draw. This
   defect was found while debugging defect 4 and is not among the five
   in the parent `requirements.md` introduction; it is recorded here
   because the same no-silent-degradation contract covers it.

6. **Fixed on 2026-07-29 (defect 7) — the harness reported neither
   what it covered nor whether its output was valid, and a
   pixel-identical result from it was read as proof of a codegen
   change the replay had never executed.** A D3D9→MSL translator
   change (`959c848c`, reverted in `65a2d769`, corrected in
   `d63f7a65`) rewrote how relative constant reads overlay DEF
   literals. It was validated by replaying the GT1 frame60 `enc1`
   manifest through this domain with the new emission applied by hand:
   786,432 of 786,432 pixels identical to baseline. That was read as
   "the codegen is correct." It was not — under the real runtime the
   same emission made every skinned character in 3DMark05 GT1
   disappear, across six captured frames spanning 0:13–0:57 that
   showed the environment rendering correctly with weapons floating
   unheld and no human figure anywhere.

   **The replay could not have caught it, and the reason is worth
   stating exactly.** Instrumenting the eight affected vertex shaders
   so that *taking the DEF-select branch* collapses the vertex
   position produced an image byte-identical to baseline: across the
   row's 229 draws and roughly 795,000 vertex invocations, the branch
   under test was never executed. The verification was testing dead
   code. The control proves the instrumentation was live — forcing the
   same marker on unconditionally changed 15,134 pixels. Those same
   eight shaders account for only those 15,134 pixels, **1.9% of the
   frame**, so even a total failure of them would have been easy to
   miss in a whole-image comparison. The harness reported none of
   this: it printed `mini replay draws=229 repeat=1` and exited 0.

   This is a verification-fidelity defect, not a rendering one. The
   domain rendered exactly what it was asked to render; what was
   missing was any statement of what that covered. The fix adds three
   deliberately separate records to `mini-replay-summary.json`
   (R-HARN-REPLAY-3.5):

   - `coverage` (R-HARN-REPLAY-3.4) — the manifest rows and encoders
     replayed, the replayed draw count, the shader-variant count, and
     the per-`vs_hash`/per-`ps_hash` draw counts, which sum to the
     draw count. For the GT1 frame60 `enc1` manifest this reports row
     `60/1`, 229 draws, 17 shader variants, and the 17 vertex-shader
     hashes with their draw counts — so a reader who changed a shader
     can see whether it is present in the replay at all, and in how
     many draws. `scope` states in the artifact that this is a
     single-encoder slice of one frame and does not establish that any
     branch executed.
   - `validity` (R-HARN-REPLAY-3.1) — the color output is read back
     after the run, its distinct RGB triples and non-background pixels
     are counted, and a single-distinct-value image exits non-zero
     with the counts recorded before the exit. The threshold is two
     distinct values, a named constant; no percentage-coverage gate
     was added because the degenerate case is the only failure shape
     with evidence behind it (defect 4).
   - `execution_proof` (R-HARN-REPLAY-3.8) — present only when
     `--prove-executed` was passed, because proving execution costs a
     second replay and needs a pattern only the maintainer testing a
     specific change can write. It carries the substitution, the
     scanned/mutated file counts, the site count, the differing-pixel
     count, and one of three named verdicts.

   **`validity` must not be read as coverage, and `coverage` must not
   be read as execution.** Containment is what `coverage` answers:
   defect 7's replay contained all eight affected vertex shaders and
   still never took the branch. Execution is what `execution_proof`
   answers, and only when asked.

   **`validity` must not be read as coverage.** Defect 7's own image
   was thoroughly non-degenerate — 12,231 distinct RGB values over
   784,476 of 786,432 non-background pixels — so the validity check
   would have passed the exact run that produced the false negative.
   It removes one specific way a comparison can be vacuous; it proves
   nothing about what the replay reached. The two claims are kept
   apart in the summary, in the message text, and in
   R-HARN-REPLAY-3.5.

   Verified end-to-end: the GT1 frame60 `enc1` manifest passes
   validity (12,231 distinct values, 784,476 non-background) and
   reports coverage naming row `60/1`, 229 draws, 17 shader variants;
   the same manifest sliced to its 42 depth-only draws — which write
   no colour — now exits 1 naming the degeneracy, where it previously
   exited 0.

   **The generated program now carries the same gate**
   (R-HARN-REPLAY-3.7). It counts distinct RGB triples in the readback
   buffer `writePpm` just wrote from, prints `distinct_rgb=<n>` on the
   `mini replay draws=<N> repeat=<R>` line, and returns
   `REPLAY_DEGENERATE_EXIT_STATUS` (3) with the wrapper's own wording
   when the count is below `MIN_DISTINCT_RGB_VALUES`. The threshold is
   interpolated into the generated source from that one Python
   constant, so the two sides cannot drift. The status is a dedicated
   value rather than a generic failure so `run_binary()` can tell "the
   replay rendered a degenerate image" apart from "the replay failed"
   and carry the former through to the wrapper's assertion, which
   remains the only side that records `validity` and diagnoses a
   missing, truncated, or unreadable image. Verified: the 42
   depth-only-draw slice, invoked as
   `DXMT9_MINI_REPLAY_COLOR_OUTPUT_PATH=... ./dxmt9-3dmark05-mini-replay`
   with no wrapper, prints `mini replay draws=42 repeat=1
   distinct_rgb=1` and exits 3; the full `enc1` manifest prints
   `distinct_rgb=12231` and exits 0.

   **`--prove-executed` answers the execution half**
   (R-HARN-REPLAY-3.8). `coverage` says the changed shader is in the
   replay; only a mutation says the replay reaches it. Given
   `'REGEX=>REPLACEMENT'`, the harness prepares a second shader tree
   under `<output-dir>/execution-proof/` with `re.sub` applied to every
   generated `.metal` source, replays it, and compares the two images.
   Verdicts, and why they are three and not two:

   | Verdict | Meaning | Exit |
   |---|---|---|
   | `not-present` | the substitution matched no site — wrong pattern, or the construct is absent from these dumped shaders. **Not an execution verdict** | non-zero, decided from the generated sources before anything is compiled |
   | `present-but-not-executed` | matched sites, byte-identical image. Defect 7's exact shape | non-zero |
   | `executed` | matched sites, image moved | zero |

   Verified against defect 7's own manifest. The dumped GT1 frame60
   `enc1` shaders predate `d63f7a65`'s emission, so the literal
   `dxmt9_cdef<N> : ` pattern reports `not-present` — 0 sites across
   34 generated sources — which is the correct answer and not
   `not-executed`. Reconstructing the DEF select over the same
   inputs those dumps do carry (`cFloat[196] = float4(3.0f, ...)`
   overlays and `cFloat[clamp(a0.x + N, 0, 255)]` relative reads, in
   exactly eight vertex shaders) reproduces the defect: substituting
   the select's marker branch matches 48 sites in 8 of 34 sources and
   changes **0 of 786,432 pixels** — the branch is never taken. The
   unconditional-marker control over the same 48 sites changes
   **15,134 pixels**, independently reproducing the figure the original
   hand investigation measured, so the instrumentation is live and
   those eight shaders own 1.9% of the frame.

   The interface is a raw regex over emitted MSL with no shader
   awareness, which is the right shape for a maintainer testing a
   codegen change but is sharp: it matches inside comments and
   substrings, and a replacement that does not compile fails at
   compile time. The `sites` / `files_mutated` / `files_scanned`
   counts in `execution_proof` are what make a verdict checkable, and
   the flag's help text says so.

---

## 8. Full Render Tape Architecture

**Implementation status (2026-08-14).** The structural substrate is Render
Tape v2. `device_c_render_tape.*` owns the pointer-free event grammar described
by R-HARN-REPLAY-7.3/7.4, validates the fixed typed baseline plus canonical
`APPLY_STATE|FULL_SNAPSHOT` bootstrap completeness, object generations,
verified digest-backed mutations, exact D9C
v2 chunks, ordered-control dispositions, Present/completion closure, and all
allocation before replay callbacks. The validator first builds a bounded,
value-owned ObjectDefine index, closes Bootstrap FULL_SNAPSHOT handles against
definitions that may be journaled after event 1, and then applies the ordered
live-generation checks for later chunks. The replay sink receives an explicit
journal-only/deferred-provider bootstrap mode. Native tests cover a complete
trace and fail-closed bootstrap-handle, blob, generation, range, control,
completion, descriptor, and callback-conservation cases. `dxmt9-render-tape`
accepts only explicitly
verified blob references; `run_dxmt9_render_tape.py` builds the
`dxmt9.render_tape.bundle.v2` envelope, stores digest-named blobs, verifies their
size and SHA-256, and then supplies that catalogue to native validation.

The bounded capture owner and PE hook now provide the production bootstrap
producer: capture is default-off and allocates no registry while disabled. When
enabled, the device tracks generation-qualified live object identities plus
value-owned descriptors, immutable shader/declaration payloads, and supported
CPU-written resource contents from creation onward. At the first successful
Present boundary the member producer serializes the actual PE shadow as one
canonical `APPLY_STATE|FULL_SNAPSHOT`, sorts and seeds the live objects, emits
complete initial subresource mutations, and arms the next interval; an injected
producer remains only an explicit test override. Each recorder-flushed canonical
chunk is copied exactly once, and the following Present seals/publishes one
value-owned bundle containing events and digest-verified blob bytes. Live PE
object, writable-lock mutation, and true chunk-bypass control call sites feed
that owner; chunkized operations are not duplicated. PE object definitions
carry value-owned C-side buffer/surface/texture/query descriptors, vertex
declaration elements, exact validated shader bytecode, and resource expected
extent/count closure. Immediately before building the bootstrap, the PE producer
refreshes the complete binding view and validates the sealed overlay through the
canonical chunk validator; its exact `(kind,generation,objectId)` handle set is
used by frame-tape to form an exact bootstrap closure over those roots plus the
required Present output and recursive descriptor dependencies (in particular a
texture-derived Surface's exact parent texture/subresource owner). Unreferenced
live objects, including incomplete ones, are not materialized; referenced
missing, stale-generation, incomplete, or dependency seeds fail closed with a
typed diagnostic. A complete pre-arm object omitted from the closure may be
materialized once immediately before its first command-chunk or identity-bearing
control reference, with exact current seeds. A missing or incomplete
first-reference seed fails closed with the typed pre-arm-materialization reason;
mutations and destroys of unadmitted objects remain registry-only. This
per-identity JIT is not arbitrary deferred closure for opaque producer-side
references. Sequence-tape instead retains the complete all-live arm snapshot,
because its second interval cannot admit a new `ObjectDefine`; an unexpected
unadmitted sequence identity rejects before event emission. The capture seam
accepts bounds-checked uncompressed 2D
surface/texture, complete-buffer, and DXT1/DXT3/DXT5 block-compressed 2D/cube
face-and-mip lock layouts. Full locks establish tightly packed complete seeds;
partial locks strip row padding, require an existing exact-size seed, overlay
at the checked descriptor coordinates, and publish the resulting complete
content. For a generation-qualified buffer whose first writable lock is a
partial range with no complete seed, the PE owner may, only while capture
tracking is enabled, relock that same `D9CBuffer` at offset zero for exactly
`D9CBufferDesc::size` bytes with `D3DLOCK_READONLY` after the application
unlock succeeds. It copies exactly the returned bytes and records one
zero-offset complete seed; allocation bytes, the partial write, and inferred
padding are never substituted. Identity, descriptor extent, generation,
full-lock, copy, or unlock proof failure rejects only the tape with a typed
reason and preserves the application's `Unlock` HRESULT. Capture-off performs
no relock or copy. The capture-only read-lock may repeat backend
synchronization or dirty-side effects and remains explicit performance/semantic
debt; `D3DLOCK_DISCARD` is not promoted by this closure. For an
already-lockable 2D texture subresource whose first writable
lock is partial and unseeded, the PE owner may take one capture-only full
CPU-visible lock through the exact texture handle after user unlock, strip row
padding, and record that complete snapshot; only exact bytes returned by that
successful full lock are admissible, while inferred or otherwise unproven
allocation bytes and partial seeds remain inadmissible. Full-lock, copy, unlock, identity,
generation, or extent proof failure rejects the tape without changing the D3D9
HRESULT. This seam is not general GPU readback and does not claim indexed GT2
replay. Texture-derived 2D surface wrappers for both uncompressed and
block-compressed formats may take the same exact-owner full CPU-visible lock
after the original surface unlock, but only after proving the owner texture
identity and generation-qualified subresource; standalone, cube, volume, and
user-memory surfaces remain surface-owned and do not use this fallback. The
capture-only relock may repeat backend dirty/autogen work even though the
bytes are read-only; this remains an explicit side-effect/performance debt.
Texture-derived surface wrappers mutate the owning texture identity at the
exact generation-qualified subresource; repeated wrappers for one underlying
surface are idempotent. For block-compressed locks it validates 4x4-block
alignment (allowing the rounded terminal block at odd extents), pitch, bounds,
and overflow, copies rows without pitch padding, and maintains tightly packed
complete subresource bytes. An aligned partial rectangle requires an existing
complete seed and publishes the resulting full subresource as the next ordered
immutable mutation. Version-2 texture and surface descriptor payloads distinguish
complete from unavailable initial bytes and bind a surface alias to its exact
generation-qualified parent texture plus flattened subresource. Alias logical
slots are keyed by that parent/subresource metadata, not by the wrapper's wire
object ID: distinct alias object IDs are ordered by journal
events, and only generations within one object ID require numeric monotonicity.
An alias and an ordinary surface with a shared object ID remain distinct slots.
Invalid layouts,
stale aliases, incomplete partial-write seeds, volume locks, missing initial
bytes, and unavailable descriptor or bytecode data fail closed with a typed
first capture-rejection diagnostic before publication; this metadata does not
widen provider replay claims or alter canonical D9C v2 records or the bridge ABI.

The V2 payloads are now the single production representation. A texture
payload is one `RenderTapeTextureDescriptorV2` header plus an exact tail of
`D9CSurfaceDesc` values: one complete mip chain for 2D and volume textures and
six complete mip chains for cube textures. The validator checks the declared
dimension, mip/subresource relation, resource type, repeated format/storage
attributes, mip extents, and volume mip depth before any replay effect. Texture
initial content is `CompleteSeed` by default; texture `Unavailable` is
rejected except for the bounded `ProducedByCapturedPass` exception. That
exception is limited to one Texture2D mip and subresource, render-target
usage, one exact generation-qualified Surface alias at subresource 0, and a
single unresolved obligation. Both definitions must precede the resolving
command chunk, which must be retained and bind the exact alias before an
unrestricted full clear as the first terminal access. The obligation resolves
once; `PresentComplete` rejects it if unresolved. Partial or draw/read-first
access, identity mismatch, multiple Produced textures, multiple matching
aliases, multi-mip/cube/volume textures, ambiguous aliases, and out-of-order
definitions fail closed, and the Produced texture receives no seed mutation.
A surface payload is exactly one `RenderTapeSurfaceDescriptorV2`: independently
owned surfaces use `Standalone`/`CompleteSeed`, aliases use
`TextureSubresource`/`Unavailable` with an exact live parent and subresource,
and the oracle uses `SwapchainBackbuffer`/`ProducedPresentOutput`. The old
level-0/count texture descriptor and raw surface descriptor are Retired; there
is no production compatibility branch. Registry admission, bootstrap and JIT
materialization, publication validation, inspection, projection, and provider
preflight all consume the same V2 bytes. Unsupported provider capabilities,
including canonical cube or volume inputs outside the bounded provider slice,
return the typed unsupported result only after structural V2 validation.

Writable buffer/surface/texture mutations are copied before provider unlock,
and readonly buffer locks are journaled as controls. One bounded
`perf-d3d9-present-loop` production capture now closes the capture-time output
oracle through this provider. The 2026-08-13 bundle
`experiments/output/render-tape-oracle-final.0ZFP3y/frame-95919862787500-1`
has a 2992-byte `events.bin` with SHA-256
`ed6bb63659a72c60066c0653d4934669dd7f7081e7389f6a110f13a94eb5c7be`.
Its four events are one object definition, one bootstrap, one `Clear` +
`Present` command chunk, and one `PresentComplete`, with zero blobs or
mutations. Structural `validate` and `inspect` pass; `provider-replay` returns
`complete` (exit 0), with `production_capture=true`,
`production_provider_replay=true`, and `output_oracle=true`. The capture-time
and replay 256×256 format-21, 262144-byte outputs have the same SHA-256
`49843e277c6ce8246d199c69c77aba0e7791c50522ab16c6a926f1528bd7474c`, so
`expected_digest_captured=true` and `expected_digest_matched=true`; object
conservation is 1/1. The uniform clear makes `output_non_degenerate=false`,
which limits scene coverage but does not weaken the exact byte-equivalence
claim for this accepted interval.

The first digest-bearing wild run usefully failed this oracle: capture recorded
SHA-256 `7f2d0d283f559405932df39c2322375c60a577e9e6d462136661dea1a21b7b6a`
while replay produced `49843e277c6ce8246d199c69c77aba0e7791c50522ab16c6a926f1528bd7474c`.
Those hashes exactly identify the sample's first and second clear colours,
respectively. Deferred replay had allowed the prior Present to consume the
one-shot ticket. Draining deferred replay alone was insufficient because the
prior Present could already be pending in the renderer queue; conversely, the
captured Present reached that queue before its ticket was marked encoded. The
implemented ordering therefore flushes the renderer before publishing the
reservation and again after draining the captured replay but before accepting
the encoded ticket. The successful evidence above is from that corrected
ordering.

At the arm boundary, the implicit swap-chain backbuffer is lazily admitted as
the stable PE wire identity with `PresentOutput` role and
`initial-content-not-required` disposition. It is the output produced by the
captured interval, not an arbitrary live buffer or a CPU-seeded input. Proof
that a selected interval does not load prior backbuffer contents is not yet
implemented. Structurally valid captures may still be published with
`reference_replay=false` and `output_oracle=false`, but provider replay and
promotion must fail closed for intervals that require such a load until that
proof exists. The direct `PresentEx` path
currently calls the provider without emitting the canonical D9C `PRESENT`
record, so `presentChunkSeen=false` and frame completion fail closed; PresentEx
event support is a separate gap from canonical `Present` capture evidence.

That admission is a **capture-owned single-holder role**, not a property of the
D3D9 object. Exactly one live registry entry may hold `PresentOutput`, and the
admission that names a new holder hands the role back first. Handing it back is
a pure transition over the recorded holder, whether the holder is still in the
live registry, and its wrapper reference count there: re-admitting the same
exact identity retains the role; a holder that already left the registry needs
no transition; a **surface** holder the admission itself registered and that
still carries only the admission's own wrapper reference is **retired**
(tombstoned, so the generation rules keep applying to it); any other holder is
**demoted** back to its exact displaced initial-content state and stays
registered. Retirement is therefore scoped to the proven swap-chain output
handoff. A generic standalone or texture-derived alias the capture merely
re-roled is only ever demoted, so this policy never removes an entry the alias
replacement rules still own. The transition never inspects generations —
identity monotonicity stays owned by registration, so handing the role back can
never admit an identity registration would reject.
The role is also handed back whenever an arm attempt ends without an active
interval, and again at the start of the next attempt, because an admission
releases its PE wrapper before the arm returns and the C-side wire registry is
free to recycle that object id at a newer generation in the meantime.

Without that policy a retried arm accumulates holders. The GT2 frame-tape
evidence (`experiments/output/`
`app-d3d9-3dmark05-gt2-frame-tape-exact-closure-r6-20260814/dxmt9.log`) shows
both consequences: `producer aborted reason=present_output_count count=2`
through `count=8` across successive retries, and then a recycled wire object id
meeting its stale holder in the logical-slot replacement scan, where a
standalone surface is deliberately not an alias replacement candidate, so it
rejected as `prior_not_retained_alias` and marked the registry invalid for the
remaining life of the process. Retiring the stale holder removes the collision
without relaxing that alias predicate.

A CPU-unlock mutation on an already-admitted object that fails to append now
carries typed attribution instead of a single fused status. The owner drives
the two session steps separately — the blob registration and then the mutation
event, which is exactly the pair `resourceMutationBytes` performs, so nothing
about what is admitted or when the tape fails closed changes — and a rejection
emits `mutation_reject` with the failing step (`registry_entry_missing`,
`subresource_out_of_range`, `blob_register`, or `mutation_event`), the status,
the capture state, the identity/subresource/byte size, whether the identity is
still live in the tape, and each bounded counter against its limit
(`event_count`, `buffered_bytes`, `owned_blob_bytes`, `owned_blob_entries`).
Every field is already-owned session state exposed through const accessors;
no predicate is relaxed and no capacity is raised. This makes the fused
`InvalidInput` of a mutation event decidable — a non-live identity is now
distinguishable from an unverified blob — so an interval abort can be told
apart from a registry-shape or capacity failure without guessing.

The capture owner also bounds the total owned blob bytes (256 MiB by default,
268,435,456 bytes), with overflow-safe admission before hashing or copying.
`DXMT9_RENDER_TAPE_MAX_BLOB_BYTES` is a capture-only decimal-byte override:
unset, invalid, and zero values resolve to the default, while valid values
above the hard 1 GiB (1,073,741,824-byte) ceiling resolve to that ceiling. The
override is not read when capture is disabled. Exact duplicate blobs are
admitted without a second charge, and a rejected blob cannot relax digest,
descriptor, generation, event, or replay-grammar validation. GT2 r7 and r8
measurements establish lower bounds only; no complete GT2 bundle has yet
proved that this bounded policy is sufficient for the full capture.

Production publication is enabled only when `DXMT9_RENDER_TAPE_OUTPUT_ROOT`
names an explicit safe **PE-visible absolute** directory. Under Wine callers
must pass the Windows drive form (for example `Z:\\Users\\...`) after
resolving the host directory; a host-only `/private/...` or `/tmp/...` string
is not a PE absolute root. The PE fallback resolver rejects
relative or traversal roots, embedded NULs, symlink components, and completed
frame-name collisions. It writes `events.bin`, digest-named `blobs/`, and a
minimal provenance/scope manifest in same-filesystem staging, flushes and
closes the files, and atomically renames the complete frame directory; an
unset or unsafe root leaves the default-off capture inert.

For a standard canonical `PRESENT`, production capture also reserves one
one-shot offscreen mirror on the existing window `Presenter` immediately before
the captured chunk commit. The mirror is the canonical logical output at the
captured backbuffer descriptor extent; it is not a claim about a drawable that
may be resized by the window system. The ordinary drawable present remains
intact. The reserve bridge first drains deferred replay and flushes the renderer
queue before publishing the one-shot ticket, so a prior queued Present cannot
consume it. In the captured Present command buffer the presenter renders the
mirror with the identical present PSO, source, sampler, and gamma parameters.
After the commit, a typed capture-only bridge finish drains replay and flushes
the renderer queue before validating that the ticket was encoded; it then
production-reads back the mirror, tightly packs row bytes, and returns fixed
POD metadata plus SHA-256. PE
validates the dimensions, D3D format, and exact byte count before recording the
digest in `PresentComplete`. Mirror failure, cancellation, absent/mismatched
metadata, or cleanup failure aborts the tape alone and never changes the
application's Present result or publishes a bundle. The inactive capture gate
does not reserve, bridge, allocate, or add a render pass. The production PE
capture owner still emits only `frame-tape`; the replay tool's bounded sequence
and reducer operations do not extend capture grammar, support `PresentEx`, or
prove prior-output loads.

The structural v2 schema represents recorded initial/resource bytes as
digest-backed `ResourceMutation` events and adds only a total expected byte
extent plus subresource count to resource `ObjectDefine`. A separate bounded
seed-closure table rejects duplicate subresources and requires their zero-offset
mutation sizes to sum to the exact extent. Each identity's expectation closes
when its exact seeds are satisfied; unrelated events do not close another
identity's expectation. A matching mutation may arrive before that identity's
first command, control, or destroy use; later mutations for a complete identity
are ordinary interval traffic. The final journal still requires all
expectations complete. The ordered live-generation registry remains
identity/lifetime-only.

### 8.1 Capture boundary

Render Tape records the canonical semantics after PE-side state coalescing and
before unix-side import changes their scheduling. That boundary is intentional:
the canonical D9C wire in `include/dxmt9/device_c.h` is pointer-free,
bounds-checkable, versioned, and already carries generation-qualified object
references. It is therefore the command representation the full-tape event
journal preserves byte-for-byte.

The wire is delta-oriented, so it cannot start a replay alone. The capture owner
must first create an ordered checkpoint containing the complete effective state
shadow and all resource contents that the selected interval can read before a
captured write. Resource and control operations that bypass command chunks are
recorded in the same ordinal journal. Capture is armed for a future Present
boundary, obtains a consistent checkpoint only after older work can no longer
mutate it, then records complete Present intervals. A failed drain, missing
resource byte range, or generation mismatch aborts sealing.

This is a backend-semantic capture, not an exact D3D9 API trace. Multiple setters
already coalesced into one draw delta cannot be reconstructed, and CPU timing or
Wine scheduling before publication is not preserved.

### 8.2 Bundle layout

The logical bundle is:

```text
render-tape/
├── manifest.json       schema/profile/ABI/provenance/component digests
├── events.bin          bootstrap/object/mutation/command/control/completion journal
├── blobs/              digest-named resource, shader, declaration payloads
└── output.rgba         optional digest-authenticated Present output bytes
```

The physical container may later become one packed file, but these logical
components and their independent digests remain part of the schema. Event
payloads reference blob digests and `(kind, objectId, generation)` identities;
they never embed live pointers or depend on registry slot addresses from the
capturing process.

The implemented structural tools are invoked as:

```sh
build/tools/dxmt9-render-tape validate frame.tape \
  --verified-blob <sha256>:<bytes>
build/tools/dxmt9-render-tape inspect frame.tape \
  --verified-blob <sha256>:<bytes>
python3 scripts/tools/run_dxmt9_render_tape.py pack \
  --events frame.tape --blob mutation.bin --output-dir frame-tape-bundle
python3 scripts/tools/run_dxmt9_render_tape.py validate frame-tape-bundle
python3 scripts/tools/run_dxmt9_render_tape.py provider-replay frame-tape-bundle
```

The first pair operates on the canonical event component. The second pair adds
and verifies the artifact envelope. `inspect` dispatches every prevalidated
event to the structural replay sink and reports conservation counts; it remains
semantically distinct from the production provider replay. `provider-replay`
validates the bundle, passes the actual digest-named blob files to
`build/tools/dxmt9-render-tape-provider`, and reports machine-readable validity,
coverage, conservation, and output-oracle scope without rewriting the source
manifest.

The 2026-08-15 GT2 r57 bundle at
`experiments/render-tapes/gt2-output-oracle-r57-20260815/`
`frame-156258260414600-1` closes the full-tape production-provider milestone.
It contains 912 events, 32 command chunks, 269 object definitions, 607
mutations, 687 immutable blobs, 1,877 command records, and 1,256 draws. The
atomic bundle includes a 3,145,728-byte `output.rgba` whose SHA-256
`6c4705e6a7fd302038a4deb6aab505f93d80be5e6f5de452d051806632b83d01`
matches the captured `PresentComplete` oracle. Three isolated provider
processes (one warm-up plus two measured runs) replay all 687 blobs and
conserve 269 created/released objects. Their output SHA-256 is identically
`c82fc63f8c75dcf1453cfcf1251c96560b89389075f237c9d4b1442fb79fd052`.
Exact digest equality remains false and is reported as such; authenticated
pixel comparison finds 34 of 786,432 pixels different, maximum RGB delta 2,
total RGB delta 62, and zero alpha differences, which satisfies the bounded
R-HARN-REPLAY-7.19 envelope. The official runner therefore reports
`status=complete`, `oracle_mode=pixel-envelope`, `deterministic=true`, and
`production_provider_replay=true`. A single-process envelope run is rejected
as `insufficient-envelope-repeats`.

The journal distinguishes at least:

- canonical command-chunk wire bytes and their source/sequence identity;
- object define/destroy and digest-backed subresource content mutations;
- shader/declaration creation and immutable payload identity;
- byte-exact chunk-owned QueryIssue/readback/Present commands;
- direct QueryGetData, CPU-read, Flush/wait, Reset, and device-lost controls;
- bootstrap and terminal `PresentComplete` records.

### 8.3 Production-path replay

The reference replayer is a small host around existing production owners, not a
second renderer. It validates the manifest and component digests, reconstructs
the generation-qualified object registry from the checkpoint, applies journaled
mutations in ordinal order, imports recorded chunks through the production wire
validator, and submits them through the production queue and Metal provider.

For deterministic validation, Present is adapted to an offscreen attachment and
ordered readback. The adapter replaces only drawable acquisition/presentation;
it may not bypass pass actions, resource lifetime, completion, or command queue
semantics. A windowed mode and `.gputrace` capture of the replayer are useful
diagnostics but are downstream options, not the reference oracle.

The first production slice is implemented by
`device_c_render_tape_provider.*` and hosted by
`tools/dxmt9_render_tape_provider.cpp`. It admits exactly one uncompressed,
single-sample 2D colour output and a complete bootstrap. The legacy bounded
form is one full-surface `Clear` followed by one identity `Present`. The second
bounded form adds exactly one fixed-function `DrawPrimitiveUP` between them:
one triangle-list primitive, inline `XYZRHW|DIFFUSE|TEX1` vertices, one bound
uncompressed single-level A8R8G8B8 texture, and one complete level-0 seed.
Canonical records may occupy separate command-chunk events; admission flattens
them in journal order and requires exactly `Clear` → `DrawPrimitiveUP` →
standard `Present`. Admission validates
the whole tape and the actual blob digests before creating provider objects.
Recorded identities resolve to replay-owned wrappers in a side registry; the
canonical chunk bytes are passed unchanged to
`replayPrevalidatedResolvedCommandChunk`, which retains the wrappers and uses
the existing `DeviceReplaySink`, replay planner, drain ledger, resource marks,
queue, and completion paths. `OffscreenPresentOutput` replaces only drawable
acquisition/scheduling in `Presenter`; the existing present PSO/pass and
production readback remain shared. The host hashes each supplied blob itself,
obtains output requirements from provider preflight, constructs the matching
native D9C device, and returns non-zero for any non-Complete result. Result
values report validity, grammar coverage, and blob/object/ordinal conservation
separately and make no timing or benchmark claim. Queries, readback/control events, reset/device-lost,
`PresentEx` flags, partial rectangles/seeds, multiple/depth/MSAA/compressed
outputs, shaders, arbitrary declarations, vertex/index buffers, prior-output
loads, and all other records or descriptor families fail before effects. The
one declaration exception is production's byte-exact four-element
`XYZRHW|DIFFUSE|TEX1` FVF expansion; its descriptor, immutable payload digest,
bootstrap identity, factory creation, and release are all checked explicitly.
Texture and surface admission first parses the canonical V2 grammar shared
with the capture validator. The bounded provider then accepts only its
single-sample uncompressed 2D capability subset; a canonical cube, volume,
compressed, or otherwise unsupported resource reports `UnsupportedGrammar`
without a legacy descriptor fallback or provider effect.

The native provider fixture covers combined and three-event command-chunk
boundaries, negative near-misses, and production Metal replay. On the Apple M1
test host its 16×16 non-uniform output has tight SHA-256
`3dc6ca2708ccbb285106dea4b1cba42e6d67dd69ffe72ab003d87d7d8250b72e`;
the direct-FVF and production-declaration forms produce the same digest and
conserve 2/2 and 3/3 replay-owned objects respectively.

The 2026-08-13 Sikarugir `PRESENT_LOOP_TEXTURED=1` capture at
`experiments/output/render-tape-wild-textured-r2/tapes/frame-99975454225700-1`
contains 8 events, 3 records in 2 command chunks, 3 definitions, one 64-byte
texture mutation, and one 32-byte immutable generated declaration. Structural
validation and inspection pass. Provider replay returns `complete`, 2/2 blob
references, 3/3 object conservation, `output_non_degenerate=true`, and exact
capture/replay identity for the 262144-byte output SHA-256
`866e45bc5527c590f7cbf1deb9ca8fd5aa3ac2eddcd6746bdaf0572848a78c17`.

The same provider now admits one additional bounded `sequence-tape` shape:
exactly two copies of the textured-UP interval with one complete, digest-backed
level-0 texture mutation between their `PresentComplete` boundaries. Each
interval has its own expected digest and completion conservation. Native Metal
tests produce two distinct interval outputs after the mutation and reproduce
the ordered two-digest vector on a second fresh device. The CLI repeats either
profile with a `fresh-process-device` reset for every warm-up and measured run,
rejects `warmup + repeat` above 64, and requires exact validity, coverage,
conservation, output-oracle, and ordered-digest identity across all runs.
Production PE capture/publication of a sequence is opt-in through
`DXMT9_RENDER_TAPE_CAPTURE=1`, `DXMT9_RENDER_TAPE_PROFILE=sequence-tape`, and
the existing safe `DXMT9_RENDER_TAPE_OUTPUT_ROOT` policy. The PE owner keeps
the first completion in the journal without sealing or publishing, preserves
the next complete digest-backed mutation in order, and seals/validates and
publishes exactly once after interval 2.

The 2026-08-14 Sikarugir production bundle at
`experiments/output/perf-d3d9-present-loop-sequence-r3/tapes/sequence-16792924532000-1`
closes the captured wild sequence identity slice. It contains 12 events, four
command chunks, six records, three object definitions, and two complete
64-byte texture mutations. Structural inspection passes, and production
provider replay with one warm-up plus two measured fresh-device runs conserves
3/3 objects and 3/3 referenced blobs. Every run reproduces the ordered
256x256 output SHA-256 vector
`866e45bc5527c590f7cbf1deb9ca8fd5aa3ac2eddcd6746bdaf0572848a78c17`,
`937c9038b1c145c05b319ff71130fe7f46a999155c376dceea9eae63c77f6861`;
both capture digests match, both outputs are non-degenerate, and the mutation
therefore has an observable result. Prior-output-load proof and broader grammar
remain open.

For `frame-tape`, `reduce` retains selected whole `CommandChunk` events plus
their bootstrap, exact live generation definitions, complete initial seed/blob
closure, the selected Present, and terminal `PresentComplete`. It rewrites
ordinals and the content-addressed manifest, validates before provider effects,
and accepts only a candidate whose explicit provider oracle succeeds. `bisect`
performs deterministic delta minimization over that same whole-command domain.
Both operations reject controls, destruction, live post-seed mutations,
sequence input, and any other shape whose semantics the bounded closure cannot
preserve.

This is a provider API plus native evidence seam, not a new structural CLI.
The existing `validate`/`inspect` commands remain structural-only. The bounded
captured-bundle run above promotes this narrow slice from native identity
evidence to production artifact evidence; it does not claim general grammar
support or non-degenerate pixel equivalence.

The structural/replay tools have these modes:

| Mode | Purpose | Timing claim |
|---|---|---|
| `validate` / `inspect` | schema/resource/order validation and structural conservation; no provider execution | none |
| `provider-replay` | production-provider execution with declared fresh-process reset, warm-up, repetition, and exact run identity | identity only; no performance sampling |
| `reduce` / `bisect` | deterministic whole-command frame reduction with closure validation and explicit provider oracle | none |
| `project` | pure one-command-event contiguous Draw-range readiness projection; no provider execution or wire rewrite | none |
| future `benchmark` | repeated execution with a declared sampling policy beyond identity checks | explicit and profile-bound |

### 8.4 Draw-slice projection foundation

`device_c_render_tape_projection.*` implements the first bounded
R-HARN-REPLAY-7.16 transform. It calls the production Render Tape validator
with the supplied verified blob catalogue before constructing output, accepts
only `frame-tape`, and selects one non-empty contiguous record interval from
one `CommandChunk` named by canonical event ordinal. All selected records must
be Draw records. The first Draw must carry a command-validator-accepted
`FULL_SNAPSHOT`, which is the child state anchor; the frame bootstrap is not
silently inherited as child state. The planner locates a preceding Clear and a
following Present without adding them to the selected range. The end comparison
is exclusive, so a Present immediately after the last selected Draw in the same
command chunk is a valid outside boundary.

The planner walks selected record handle slices in serial order, deduplicates
only exact `(kind, objectId, generation)` identities, and resolves them against
the source's exact `ObjectDefine` events. It reports definitions in source
order, one blob entry per source reference, immutable shader/declaration
payloads, complete seed mutations, and subsequent digest-backed mutations for
those identities before the selected command event. Duplicate digests remain
separate references so conservation and source order stay observable. The
source validator proves per-identity initial-content closure; the planner additionally
checks each selected object's projected seed byte/count totals against its
definition. Any missing proof returns a status before invoking a replay sink,
provider, Metal owner, or writer. The capture/Draw hot path is unchanged; this
is a cold offline transform.

`dxmt9-render-tape project` exposes the transform. Its required selector is
`--command-event-ordinal`, `--first-record`, and `--record-count`, plus the same
explicit `--verified-blob <sha256>:<bytes>` catalogue syntax as structural
validation. On success it writes one
`dxmt9.render_tape.projection.v1` JSON object to stdout with:

- the unchanged source tape SHA-256, byte/event/record counts, and frame profile;
- canonical Draw, preceding-Clear, and following-Present locators;
- exact objects, definition locators, initial-content totals, and ordered blob
  references;
- excluded record and coordinator-event conservation; and
- explicit false claims for wire rewriting, legacy mini-replay compatibility,
  provider replay, and GT2 replay.

The command writes no JSON on rejection. The artifact is not a rewritten v2
tape and is not `dxmt9.3dmark05.mini_replay_manifest.v1`; the legacy manifest
builder remains authoritative for executable mini-replay input. Render Tape v2
has canonical event ordinals but no authoritative frame ID, application
source/sequence ID, or logical-pass/DAG identity. The selector and artifact
therefore use event ordinal plus record index and do not manufacture those
broader identities. Adding a captured mapping is a separate gap.

This pure selector/conservation transform neither changes replay order nor
executes stateful rendering, so the formal scheduling layer from
`agents/rules/rendering_correctness.rules.md` is not applicable to this
milestone. `dxmt9-render-tape-projection-spec` binds the production validator
and planner with accepted `Clear -> FULL_SNAPSHOT Draw range -> Present`,
same-chunk exclusive-end boundary, locator/order/object/blob conservation, and
fail-closed sequence, non-Draw/coordinator, snapshot, range, definition,
generation, and seed cases. `dxmt9-render-tape-projection-cli-spec` pins the JSON
schema/source digest, negative no-output behavior, and absence of unsupported
selector identities.

This projection transform deliberately does not change
`device_c_render_tape_provider.*` admission grammar. The projection artifact
remains a structural readiness plan rather than a standalone executable
mini-replay. The separate full-tape production provider now admits and replays
the indexed GT2 r57 bundle with the evidence described in §8.2; that result
does not by itself make a projected Draw slice executable or prove that a
projection preserves the full frame oracle.

### 8.5 Profile relationship and migration

The current mini replay is the `draw-slice` implementation, not deprecated
code. It owns a different execution adapter and deliberately narrow coverage.
The first full-tape milestone does not replace its shader-mutation or draw-order
tools. Instead, both profiles are managed under this replay domain, share the
parent artifact envelope, and report compatible evidence blocks.

The bounded tape-to-draw-slice planner now extracts one contiguous Draw range
and its content-addressed dependency references from a validated frame tape.
It does not yet translate that readiness artifact into standalone Metal replay
inputs, so the current manifest builder remains authoritative for mini-replay
inputs. Once indexed geometry, shader, and attachment capture closure is tested,
the useful legacy transforms can consume the projection without maintaining an
independent resource-dump format.

The completed bounded implementation order is one `frame-tape`, production-
provider identity, deterministic output/conservation repetition, whole-command
reducer/bisection, and an exactly two-interval `sequence-tape` with a mutation
across the boundary, production sequence capture, and captured identity. The
next increments are broader grammar/interval count, executable tape-to-
`draw-slice` conversion, and benchmark corpus management. A nominal ten-second
capture
remains a future corpus-selection policy over complete intervals, not a claim
provided by the current two-interval profile.

Random seek, rolling eviction, mid-interval checkpoints, and a raw D3D9 API
trace are not part of the first version.
