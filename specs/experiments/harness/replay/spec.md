---
type: "Spec"
title: "Harness Replay Spec — Offline Metal Replay"
description: "Script inventory, manifest schema, probe input contract, mode table, owned environment variables, declared engine-shape expectations, and known deviations for the replay domain."
tags: [specs, experiments, harness, replay, spec]
---

# Harness Replay Spec — Offline Metal Replay

Implements `specs/experiments/harness/replay/requirements.md`
(`R-HARN-REPLAY-*`). Instantiates the `replay` row of the domain map in
`specs/experiments/harness/spec.md` §1 and the `dump-extract →
offline-replay` and `offline-replay → external-join` boundaries in
that spec's §2. Stage names, boundary names, and envelope fields are
cited from the parent spec rather than redefined here. The geometry
`.meta` slice contract this domain consumes is defined once,
authoritatively, in `specs/experiments/harness/probe/spec.md` §5; it
is cited below, not restated.

**This domain does not currently render a valid image** (§7, Known
Deviations). Every section below states this domain's intended
contract. Where current source violates that contract, this file says
so explicitly in the section concerned; nothing below should be read
as a claim that today's replay output is correct.

Facts in this document were verified against
`scripts/tools/run_3dmark05_mini_replay.py` (2,093 lines) and
`scripts/tools/build_3dmark05_mini_replay_manifest.py` (666 lines) on
2026-07-27; line numbers are cited per fact and should be re-checked
against the live file before further citation, per the same discipline
`specs/experiments/harness/probe/spec.md` §3 states for its own line
citations.

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
presence; it does not check payload *content* (§5, R-HARN-REPLAY-3.1's
gap is downstream of this check, not covered by it).

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

`run_3dmark05_mini_replay.py --help` enumerates 18 `--`-prefixed
option lines as of 2026-07-27:

```
$ python3 scripts/tools/run_3dmark05_mini_replay.py --help | grep -oE '^\s+--[a-z-]+' | sort -u | wc -l
18
```

Per R-HARN-REPLAY-6.1, every one of them alters this domain's output —
the generated Objective-C++ replay source, the compiled binary's
runtime behavior, or a written artifact — so all 18 are tabled below;
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
| `--force-fragment-color` | Replaces the compiled `dxmt9_fs` function body with a bare `return float4(1.0f, 0.0f, 1.0f, 1.0f);` (`force_fragment_color_source()`, line 732). **Currently fails to compile against essentially every real captured `dxmt9_fs`** — translated per-draw shaders declare `FSOut` and FFP shaders declare `FfpFsOut`, neither of which is `float4` (§6, §7 defect 5). |
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
| `DXMT9_MINI_REPLAY_COLOR_OUTPUT_PATH` | `run_binary()` from `--color-output`, only when non-`None` | Generated replay binary, to gate the color-attachment blit-readback and `writePpm()` call | Output path for the replayed color attachment as a PPM image. |
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

`--force-fragment-color`'s rewritten body (`float4(1.0f, 0.0f, 1.0f,
1.0f)` returned bare) matches only the third, never-replayed shape.
Against the two shapes real captured shaders actually declare —
`FSOut` and `FfpFsOut` — it is a return-type mismatch and fails to
compile (§7, defect 5).

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

## 7. Known Deviations

Five defects blocked a vertex-remap experiment run against this
domain on 2026-07-25/27 (parent `requirements.md`'s introduction).
**One is fixed; four are not.** Nothing in this document, and nothing
in this domain's `requirements.md`, should be read as describing the
four open defects as acceptable current behavior — they are the
reason this domain's documents exist.

1. **Fixed in `12348666` (defect 2).** Sliced stream payloads
   double-counted `stream0_offset` as a payload-relative offset,
   producing negative slot capacities. `resolve_stream_payload_offset()`
   (§3) is the fix; it is now this domain's stated, correct behavior,
   not an open deviation.

2. **Unfixed — silent attachment-format fallback (defect 3).**
   `color_pixel_format()` recognizes `core::Format` values 1-4 and
   returns `MTLPixelFormatRGBA8Unorm` for every other value with no
   diagnostic naming the unrecognized format. Row `60/0` renders to
   R32F (`core::Format` 16), which falls into this silent-fallback
   branch; the replay renders into a wrong-format attachment with no
   indication the format was unrecognized. `depth_pixel_format()` has
   the identical shape for depth formats outside `{40, 41, 49, 42,
   46}` (§ requirements R-HARN-REPLAY-2.1), though no wild run has yet
   exercised an unrecognized depth format specifically.

3. **Unfixed — every replay lane renders fully black, cause unknown
   (defect 4).** All four lanes captured during the vertex-remap
   experiment reported `mini replay draws=229 repeat=1` and exited 0
   while each wrote a 1024×768 PPM containing exactly one distinct
   pixel value. **The R32F format gap (defect 3) does not explain
   this.** Row `60/1` renders to X8R8G8B8 — `core::Format` 2, a format
   `color_pixel_format()` handles natively, not through the
   unrecognized-value fallback — and its four lanes were **also**
   fully black, one distinct RGB value across all 786,432 pixels.
   Eliminated as causes, each by a direct check during this
   investigation: the depth sidecar (still black with `--depth-clear
   1.0`, i.e. independent of any depth-input upload path), the
   constant buffers (the shader's `VsConsts` is `float4[256] +
   int4[16] + uint[16]` = 4,416 bytes, and the dumped payload is
   exactly 4,416 bytes carrying real transform values, not zeros or
   truncated data), scissor (disabled, full-rect), cull and fill state,
   and draw issue itself (the replay reports `draws=229 repeat=1` and
   exits 0, i.e. every draw call is actually being issued to the
   encoder). **The cause of the black output is unresolved.** The tool
   that would normally bisect the failure between the geometry and
   fragment stages — `--force-fragment-color` — is itself broken (next
   item), so the usual next diagnostic step is unavailable.

4. **Unfixed — the fragment-stage bisection tool does not compile
   (defect 5), and against essentially all real captured shaders, not
   only FFP ones.** `--force-fragment-color` replaces `dxmt9_fs`'s
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
   only that third, never-replayed shape, so it fails to compile
   against essentially every real captured shader — translated and FFP
   alike, not an FFP-only subset. The one diagnostic flag built
   specifically to separate "the geometry reaches the fragment stage
   but the fragment shader is producing wrong color" from "the
   geometry never reaches the fragment stage at all" is unusable
   exactly when defect 4 needs it.

Because defect 5 leaves the fragment-stage bisection path unusable,
defect 4's root cause cannot currently be narrowed further with this
domain's own tooling. A future fix must restore
`--force-fragment-color`'s ability to compile against the two shapes
real captured shaders actually declare — `FSOut` and `FfpFsOut`
(§6.2) — not against
`float4`, which the flag already matches but which no captured shader
ever uses. Both emitters already provide a matching constructor helper
a rewrite could target instead of a bare `return float4(...)`:
`dxmt9_make_fs_out(float4, uint)`
(`dxmt9_shader_metal_ir.cpp:2607-2617`) for `FSOut` and
`dxmt9_make_ffp_fs_out(float4, uint)`
(`dxmt9_ffp_shaders.cpp:1057-1062`) for `FfpFsOut`. Before it can be
trusted to diagnose defect 4, the flag must compile against both.
