# Mini-replay Defects A & B — fix report

Branch: `master`. Two commits, one per defect:

- `fe673fd5` — Defect A (fragment-override rewrites)
- `e2d3ed0e` — Defect B (attachment format silent degradation)

## Defect A — `--force-fragment-color` emits non-compiling MSL

### What I verified before touching anything

- `src/dxmt9/dxmt9_shader_metal_ir.cpp:2576`: `const bool usesFragmentOutStruct = true;` is hardcoded (no
  false branch reachable) and `:2637`/`:2658` etc. emit `fragment FSOut dxmt9_fs(...)` for every
  D3D9-bytecode-translated pixel shader. Confirmed via `grep -n "fragmentReturnType\|dxmt9_fs"` — five call
  sites, all literal `"fragment " << fragmentReturnType << " dxmt9_fs(...`.
- `src/dxmt9/dxmt9_ffp_shaders.cpp:1109,1129,1150,1172,1181`: all five FFP fragment emission sites declare
  `fragment FfpFsOut dxmt9_fs(...)`. `struct FfpFsOut` (line 1053) has `float4 color [[color(0)]];` — the
  member is named `color`, **not** `color0`.
- `src/dxmt9/dxmt9_shader_sources.cpp:124,152,178`: the only three `fragment float4 dxmt9_fs(...)` (bare
  return) sites, confirmed as blit/gamma-apply/debug-fill utility shaders, never dumped/replayed by this
  harness.
- `struct FSOut` (`dxmt9_shader_metal_ir.cpp:2598-2606`) can have more than one `float4 colorN [[color(N)]]`
  member (`colorOutputCount` loop), plus an optional `float depth [[depth(any)]]` when `writesDepth`, plus
  always a `uint sampleMask [[sample_mask]]`.
- `force_fragment_primitive_id_source` shares the exact same defect: it also called `replace_function_body`
  with a bare `return float4(...)` body, with no return-type awareness. Fixed both functions through one
  shared helper.

### RED (before fix)

```
$ python3 tests/scripts/test_mini_replay_fragment_override.py -v
...
test_ffpfsout_color_member_is_not_named_color0 ... FAIL
test_fsout_multiple_color_attachments ... FAIL
test_fsout_single_color_and_sample_mask ... FAIL
test_fsout_with_depth_output ... FAIL
test_unrecognized_struct_member_attribute_fails_loudly ... FAIL
test_ffpfsout_color_member_name_respected ... FAIL
test_fsout_gets_primitive_id_parameter_and_struct_assignment ... FAIL
----------------------------------------------------------------------
Ran 9 tests in 0.001s
FAILED (failures=7)
```

(2 tests pass at RED because the bare-`float4` shape was already correct before the fix — those are
regression guards, not new-behavior assertions.)

### Design chosen

Added `fragment_entry_return_type()` (parses `fragment <Type> dxmt9_fs(` via a new module-level
`FRAGMENT_ENTRY_RE`), `struct_definition_body()` (brace-matched, like the existing `replace_function_body`),
`fragment_output_struct_members()` (regex `STRUCT_OUTPUT_MEMBER_RE` over the struct body, returning
`(member_name, attribute_text)` pairs — the member **name** is parsed, never assumed), and
`fragment_output_member_statement()` which maps each member to a value by its Metal attribute:
`color(N)` → the forced color expression, `depth(...)` → `0.0f`, `sample_mask` → `0xffffffffu`. Any other
attribute raises `SystemExit` naming the struct/member/attribute rather than leaving it uninitialized — this
extends the "no silent degradation" principle already governing Defect B to this defect too, since an
uninitialized `sampleMask` could silently discard every sample.

`fragment_output_body()` ties it together: bare `float4` keeps the historical `return <expr>;` shape;
a struct return builds `<Type> o; o.<member> = ...; ... return o;`. Both `force_fragment_color_source` and
`force_fragment_primitive_id_source` now call this one helper with their own color expression/preamble
(`force_fragment_primitive_id_source` also still calls the existing `add_fragment_primitive_id_parameter`
first, unchanged).

### GREEN (after fix)

```
$ python3 tests/scripts/test_mini_replay_fragment_override.py -v
test_bare_float4_return_unchanged_shape ... ok
test_ffpfsout_color_member_is_not_named_color0 ... ok
test_fsout_multiple_color_attachments ... ok
test_fsout_single_color_and_sample_mask ... ok
test_fsout_with_depth_output ... ok
test_unrecognized_struct_member_attribute_fails_loudly ... ok
test_bare_float4_unchanged_shape ... ok
test_ffpfsout_color_member_name_respected ... ok
test_fsout_gets_primitive_id_parameter_and_struct_assignment ... ok
----------------------------------------------------------------------
Ran 9 tests in 0.000s
OK
```

No regression in the pre-existing mini-replay suite:
`test_mini_replay_draw_state.py` (6/6 OK), `test_mini_replay_vertex_order.py` (38/38 OK).

### Real-capture verification (`--force-fragment-color`)

Manifest: `traces/app-d3d9-3dmark05-vertexremap-enc1-r1/analysis/frame60-mini-replay-manifest-enc1.json`
(229 draws, confirmed 17 distinct shader variants via the generated `mini-replay-summary.json`
`shader_variant_count`).

```
$ python3 scripts/tools/run_3dmark05_mini_replay.py \
    traces/app-d3d9-3dmark05-vertexremap-enc1-r1/analysis/frame60-mini-replay-manifest-enc1.json \
    --output-dir <tmp>/mini-replay-a --force-fragment-color --compile
compile_cmd: xcrun -sdk macosx clang++ -std=c++20 -fobjc-arc -O2 -Wall -Wextra \
  -framework Foundation -framework Metal -o <tmp>/mini-replay-a/dxmt9-3dmark05-mini-replay \
  <tmp>/mini-replay-a/dxmt9_3dmark05_mini_replay.mm
# (no compiler errors/warnings; exit 0, binary produced)

$ python3 scripts/tools/run_3dmark05_mini_replay.py ... --force-fragment-color --run \
    --color-output <tmp>/mini-replay-a/actual.ppm
mini replay draws=229 repeat=1
exit=0
```

Sanity-checked the PPM is not degenerate-by-accident of a different bug: it decodes as a valid
`P6 1024 768 255` PPM, 786,432 pixels, **all** magenta (`0xff,0x00,0xff`) — which is the *correct* result
for this diagnostic mode (every fragment forced to magenta), not a sign of failure. All one of the 17
compiled `dxmt9_fs_NN.replay.metal` files inspected (`dxmt9_fs.replay.metal`, an `FfpFsOut` variant) shows
the exact expected rewrite:

```metal
fragment FfpFsOut dxmt9_fs(VSOut in [[stage_in]], constant PsConsts& psConsts [[buffer(0)]], constant FfpPsConsts& ffpPs [[buffer(3)]], constant FsVolatile& fsVolatile [[buffer(5)]]) {
  FfpFsOut o;
  o.color = float4(1.0f, 0.0f, 1.0f, 1.0f);
  o.sampleMask = 0xffffffffu;
  return o;
}
```

Note for later: since the frame is uniformly magenta end-to-end, geometry/rasterization/depth are covering
the whole viewport — so once anyone picks the black-output problem back up, `--force-fragment-color` is now
a working bisection tool that rules out "nothing is being rasterized" and points at the fragment/output
stage instead. I did not investigate the black-output problem itself, per the out-of-scope instruction.

### `--force-fragment-primitive-id`

Confirmed it shares the exact same defect (`replace_function_body(source, "dxmt9_fs", "{\n  return
float4(...)\n}")` with no return-type check) and applied the identical fix via the shared
`fragment_output_body()` helper. Verified against the same real 17-variant capture with `--compile`: exit 0,
binary produced, no compiler errors.

## Defect B — `color_pixel_format` / `depth_pixel_format` silent degradation

### What I verified

- `include/dxmt9/core_constants.hpp:292` `enum class Format : u32` starts at `Unknown = 0`. Counting ordinals
  by hand: `1 A8R8G8B8, 2 X8R8G8B8, 3 A8B8G8R8, 4 X8B8G8R8, 5 R5G6B5, 6 A1R5G5B5, 7 X1R5G5B5, 8 A4R4G4B4,
  9 A8, 10 R8G8B8, 11 A16B16G16R16F, 12 A32B32G32R32F, 13 G16R16F, 14 R16F, 15 G32R32F, 16 R32F`. **R32F =
  16**, confirming the task description and `specs/experiments/harness/requirements.md` R-HARN-2.2's
  rationale text exactly.
- Read `specs/experiments/harness/requirements.md` §2 ("No Silent Degradation", R-HARN-2.1..2.4) and
  `specs/experiments/harness/replay/requirements.md` §3 ("Unsupported Attachment Formats Are Failures, Not
  Fallbacks", R-HARN-REPLAY-2.1..2.3). R-HARN-REPLAY-2.1 explicitly states: *"This requirement covers
  `depth_pixel_format()` on the same terms: recognizing `core::Format` values `{40, 41, 49}` and `{42, 46}`
  and returning `MTLPixelFormatDepth32Float` for anything else has the identical unnamed-fallback shape as
  the color resolver and is not exempt from this requirement merely because no wild run has yet exercised an
  unrecognized depth format."* So the depth-format audit is not optional — the spec already decided it.
  Continuing the enum count from `D16_LOCKABLE = 46`: `47 D15S1, 48 D24X4S4, 49 D24FS8, 50 S8_LOCKABLE,
  51 INTZ, 52 DF16`. `DF16` is documented in the same header as backed by `MTLPixelFormatDepth16Unorm` — the
  old fallback would have silently mapped it to `Depth32Float` instead, the exact same defect shape as
  color/R32F. I used `DF16 = 52` as the depth test's "unsupported" exemplar for this reason.
- R-HARN-REPLAY-2.3 (record the resolved format in `mini-replay-summary.json`) is explicitly marked in the
  spec itself as *"intended contract, not current behavior"* and is **not** part of what the task asked me
  to fix (the task's "minimum acceptable fix" is the hard failure). I left it unimplemented; see Concerns.

### Design decision: the empty-`attachments` legacy path

Both pre-existing mini-replay tests (`test_mini_replay_draw_state.py`, `test_mini_replay_vertex_order.py`)
build manifests whose draws never include an `attachments` key at all. `first_color_attachment`/
`first_depth_attachment` return `{}` for those, and `.get("format", 0)` — a *Python dict default*, not a
value from the manifest — resolves to `0`. Making `color_pixel_format`/`depth_pixel_format` reject every
value outside their declared set, including `0`, would have made *every existing manifest-driven test*
(and every diagnostic manifest of that shape) hard-fail, which is not this defect: those manifests carry no
render-target format information to classify at all, unlike a real capture row that *does* declare an
attachment with an out-of-range `core::Format`.

I kept `color_pixel_format`/`depth_pixel_format` themselves strict (no fallback branch inside either
function — `0`/Unknown included) and moved the "no attachment metadata at all" case up into
`render_source()`: when `first_color_attachment`/`first_depth_attachment` return an empty dict, `render_source`
uses the historical `MTLPixelFormatRGBA8Unorm`/`MTLPixelFormatDepth32Float` default explicitly and by name,
never calling the strict resolver. This is not a hidden fallback inside the resolver (R-HARN-REPLAY-2.2's
concern) — it is a different, named input shape handled at the call site, and it is exercised and pinned by
`test_manifest_without_attachments_still_succeeds_with_legacy_default`.

### RED (before fix)

```
$ python3 tests/scripts/test_mini_replay_attachment_format.py -v
test_r32f_is_a_hard_failure_not_a_silent_rgba8_fallback ... ERROR (TypeError: unexpected keyword argument 'width')
test_unrecognized_format_zero_is_also_a_hard_failure ... ERROR (TypeError: unexpected keyword argument 'width')
test_df16_is_a_hard_failure_not_a_silent_depth32float_fallback ... ERROR (TypeError: unexpected keyword argument 'width')
test_r32f_color_format_fails_the_whole_run_naming_ordinal_and_dims ... FAIL (0 == 0, expected non-zero exit)
test_unsupported_depth_format_fails_the_whole_run ... FAIL (0 == 0, expected non-zero exit)
----------------------------------------------------------------------
Ran 11 tests in 0.217s
FAILED (failures=2, errors=3)
```

(6 tests pass at RED: the four "recognized format" checks and the "legacy default" CLI checks, which were
already correct.)

### GREEN (after fix)

```
$ python3 tests/scripts/test_mini_replay_attachment_format.py -v
...
----------------------------------------------------------------------
Ran 11 tests in 0.220s
OK
```

No regression: `test_mini_replay_draw_state.py` (6/6), `test_mini_replay_vertex_order.py` (38/38),
`test_mini_replay_fragment_override.py` (9/9) all still pass. Also re-ran the real 229-draw capture through
plain `prepare()` (no override flags): exit 0, generated `.mm` contains
`MTLPixelFormatBGRA8Unorm`/`MTLPixelFormatDepth32Float_Stencil8` as before (its declared color format is `2`
= X8R8G8B8 for every draw in this particular capture, not R32F — R32F is the historical defect row cited
by the spec text, not necessarily present in this specific manifest file; the fix's correctness does not
depend on this file containing it, and the unit/CLI tests cover the R32F=16 case directly with a synthetic
manifest).

### `depth_pixel_format` audit — decision

Yes, it has the identical silent-fallback shape as `color_pixel_format`, and the spec (R-HARN-REPLAY-2.1)
explicitly says it is not exempt. Fixed it the same way, same commit, using `DF16 = 52` as the
verified-unsupported exemplar (see enum count above).

## meson counts

Before any change: `5/5` (verified via `meson test -C build --suite scripts --print-errorlogs`).
After both fixes + both new tests registered: `7/7`.

```
1/7 scripts - dxmt9:dxmt9-wine-resolve                     OK
2/7 scripts - dxmt9:dxmt9-run-experiment-wsi-acquisition   OK
3/7 scripts - dxmt9:dxmt9-run-experiment-renderer-defaults OK
4/7 scripts - dxmt9:dxmt9-mini-replay-vertex-order         OK
5/7 scripts - dxmt9:dxmt9-mini-replay-draw-state           OK
6/7 scripts - dxmt9:dxmt9-mini-replay-fragment-override    OK
7/7 scripts - dxmt9:dxmt9-mini-replay-attachment-format    OK
Ok: 7   Fail: 0
```

## Files changed

- `scripts/tools/run_3dmark05_mini_replay.py` — both fixes (Defect A: `FRAGMENT_ENTRY_RE`,
  `STRUCT_OUTPUT_MEMBER_RE`, `fragment_entry_return_type`, `struct_definition_body`,
  `fragment_output_struct_members`, `fragment_output_member_statement`, `fragment_output_body`, rewritten
  `force_fragment_color_source`/`force_fragment_primitive_id_source`. Defect B: `color_pixel_format`/
  `depth_pixel_format` signatures gained optional `width`/`height` and now raise `SystemExit` instead of
  falling back; `render_source`'s two call sites gained the empty-attachment legacy branch).
- `tests/scripts/test_mini_replay_fragment_override.py` (new, Defect A, 9 tests).
- `tests/scripts/test_mini_replay_attachment_format.py` (new, Defect B, 11 tests).
- `tests/meson.build` (registered both new tests, following the `dxmt9-mini-replay-draw-state` entry's
  shape: `suite: ['scripts']`, `workdir: meson.project_source_root()`, `is_parallel: false`).

Nothing under `src/` was touched. `transform_msl`'s direct-cbuf detection, `resolve_stream_payload_offset`,
and the per-draw PSO/depth/cull state work were not touched — confirmed by `git diff --stat` on both commits
showing only the four files above. No environment variable was added.

## Concerns

- R-HARN-REPLAY-2.3 (record the resolved `color_format`/`depth_format` in `mini-replay-summary.json`) is
  still unmet — the spec itself already says so today, and the task's minimum acceptable fix didn't ask for
  it. Left as-is to avoid scope creep into `render_source`'s summary-building code, which the task also
  asked me not to disturb unnecessarily.
- The black-output problem is unresolved, as expected; not investigated per instructions. The one relevant
  new fact is that `--force-fragment-color` on the real capture now produces a uniformly magenta full-frame
  image (all 786,432 pixels), which is the *correct* signal for that mode and rules out "nothing is
  rasterized" as an explanation — useful context for whoever picks that investigation up next.
- I added one guard beyond the letter of Defect A's description: an unrecognized struct-member Metal
  attribute (neither `color(N)`, `depth(...)`, nor `sample_mask`) now raises `SystemExit` naming the
  struct/member/attribute instead of silently leaving that member uninitialized. This wasn't explicitly
  requested, but it's cheap, it's consistent with the project's own "no silent degradation" principle
  applied one level down, and it's covered by a dedicated test
  (`test_unrecognized_struct_member_attribute_fails_loudly`). Flagging it in case a reviewer would rather
  I'd kept the fix narrower.
- Defect B's `0`-is-a-hard-failure decision assumes a manifest that *declares* an `attachments.colors[0]`
  dict always carries a real, meaningful `format` field on real captures (verified true for the one real
  manifest I have access to — every one of its 229 draws declares `format: 2`). If some other producer ever
  emits a declared-but-`format: 0` attachment as a legitimate "no known format" sentinel (as opposed to a
  bug), this fix would now hard-fail that case; I did not find such a manifest in this repo to test against,
  and no domain spec I read describes that as a legitimate shape.
