# mini-replay PSO per-draw render-state fix — report

## Defect verified

`scripts/tools/run_3dmark05_mini_replay.py`'s `render_source()` collapsed
every draw's blend/color-write/depth/cull state to `draws[0]["state"]` and
baked it once into a single `MTLRenderPipelineDescriptor` /
`MTLDepthStencilDescriptor` shared by all draws (old code, lines ~944-962 and
~1517-1550/1649 per the task description).

Verified against the real capture
`traces/app-d3d9-3dmark05-vertexremap-enc1-r1/analysis/frame60-mini-replay-manifest-enc1.json`:

```
229 draws total; color_write counts: {'0x0': 42, '0xf': 187}
draw 0 color_write: 0x0
```

Derived directly from the manifest JSON (`Counter` over
`draws[i]["state"]["color_write"]`), not estimated. Draw 0 is a depth-prepass
draw (`color_write=0x0`); pre-fix, the single shared PSO baked `writeMask =
colorWriteMask(0)`, so all 187 color-writing draws (`color_write=0xf`)
replayed with every channel masked off.

## RED evidence (real, not described)

The fix was already implemented in the working tree when this report was
written (a prior turn was cut off by a network error after the implementation
landed but before tests/report). To get genuine RED evidence without
reverting my own understanding, I:

1. Wrote `tests/scripts/test_mini_replay_draw_state.py` against the *fixed*
   code first and confirmed it passes (6/6 OK).
2. Ran `git stash push -- scripts/tools/run_3dmark05_mini_replay.py` to get
   the real pre-fix file back into the working tree (verified via `git
   status`: only the implementation file was stashed, the new test file
   stayed untracked).
3. Ran the new test file directly against that pre-fix module.
4. `git stash pop` to restore the fix, and re-ran to confirm GREEN.

### RED run (pre-fix code, via `git stash`)

```
$ git stash push -- scripts/tools/run_3dmark05_mini_replay.py
Saved working directory and index state WIP on master: 525e1b70 ...

$ python3 tests/scripts/test_mini_replay_draw_state.py -v
test_blend_state_differs_on_color_write ... ERROR
test_cull_mode_reads_per_draw_value ... ERROR
test_depth_state_differs_on_depth_func ... ERROR
test_color_writing_draw_does_not_share_masked_off_pipeline ... FAIL
test_distinct_color_write_masks_are_emitted ... FAIL
test_distinct_depth_compare_functions_are_emitted ... FAIL

======================================================================
ERROR: test_blend_state_differs_on_color_write
AttributeError: module 'run_3dmark05_mini_replay' has no attribute 'draw_blend_state'

======================================================================
ERROR: test_cull_mode_reads_per_draw_value
AttributeError: module 'run_3dmark05_mini_replay' has no attribute 'draw_cull_mode'

======================================================================
ERROR: test_depth_state_differs_on_depth_func
AttributeError: module 'run_3dmark05_mini_replay' has no attribute 'draw_depth_state'

======================================================================
FAIL: test_color_writing_draw_does_not_share_masked_off_pipeline
AssertionError: 0 not greater than or equal to 2 : expected at least 2 PSO build blocks, found []

======================================================================
FAIL: test_distinct_color_write_masks_are_emitted
AssertionError: 1 not greater than 1 : expected more than one distinct colorWriteMask(...) argument, got {'0'}

======================================================================
FAIL: test_distinct_depth_compare_functions_are_emitted
AssertionError: 1 not greater than 1 : expected more than one distinct compareFunction(...) argument, got {'2'}

----------------------------------------------------------------------
Ran 6 tests in 0.183s

FAILED (failures=3, errors=3)
```

This is exactly the collapse: the generated source had a single
`colorWriteMask(0)` (matching draw 0's `color_write=0x0`) and a single
`compareFunction(2)` (matching draw 0's `depth_func=2` in the synthetic
two-draw fixture), never the color-writing/second draw's values.

### GREEN run (fix restored, via `git stash pop`)

```
$ git stash pop
Dropped refs/stash@{0} (a8676c5544b2f2a8db724cc62be7844c61ab14ad)

$ python3 tests/scripts/test_mini_replay_draw_state.py -v
test_blend_state_differs_on_color_write ... ok
test_cull_mode_reads_per_draw_value ... ok
test_depth_state_differs_on_depth_func ... ok
test_color_writing_draw_does_not_share_masked_off_pipeline ... ok
test_distinct_color_write_masks_are_emitted ... ok
test_distinct_depth_compare_functions_are_emitted ... ok

----------------------------------------------------------------------
Ran 6 tests in 0.164s

OK
```

## Design chosen

Per-draw state is extracted by three small pure functions
(`draw_blend_state`, `draw_depth_state`, `draw_cull_mode` in
`run_3dmark05_mini_replay.py`), each returning a tuple/int from a single
draw's `state` dict. `separate_alpha` resolution is baked into
`draw_blend_state`'s returned tuple (the effective alpha src/dst/op factors),
so two draws that differ only in the `separate_alpha` flag but resolve to
identical Metal factors share one PSO combo instead of needlessly forking.

In `render_source()`, each draw is run through three small dedup tables
(`blend_states`, `depth_states`, `pso_combos`), assigning:

- `pipelineIndex` = index into a PSO table built from the *distinct*
  `(shaderIndex, blendIndex)` combinations that actually occur among the
  replayed draws (not the cartesian product of all shaders × all blend
  states).
- `depthStateIndex` = index into a table of distinct `(depth_enabled,
  depth_write, depth_func)` tuples.
- `cullMode` = the draw's own `cull` value, applied per draw at encode time.

**PSO/depth-state construction is unrolled, not table-driven at the C++
level.** I initially built `BlendStateEntry`/`DepthStateEntry`/`PsoComboEntry`
C-struct arrays consumed by a runtime loop (`psoDesc.colorAttachments[0]
.writeMask = colorWriteMask(blend.colorWrite)`), which is correct but hides
the actual numbers behind a runtime field read — the real-lane verification
this task requires greps the emitted **source text** for literal
`colorWriteMask(N)` / `compareFunction(N)` arguments, which a
variable-argument call site can never produce. I replaced that with
`pso_build_statement()` / `depth_build_statement()`, which emit one literal
C++ block per distinct combo/depth-tuple (`psoDesc.colorAttachments[0]
.writeMask = colorWriteMask(15);` etc., baked at Python codegen time), the
same way the pre-fix single-PSO path baked draws[0]'s values — just once per
combo instead of once globally. `DrawEntry.pipelineIndex` /
`.depthStateIndex` still select among the resulting `psos[]` / `depthStates[]`
runtime vectors at encode time; only the *construction* of each entry is
unrolled/literal.

`cullMode` did not need this treatment: it's read per draw at encode time
(`[encoder setCullMode:cullMode(draw.cullMode)];`), replacing the old
single `[encoder setCullMode:cullMode({cull})];` call that ran once before
the draw loop using only `draws[0]`'s cull state. `scissor` was already
per-draw via `scissorRect(draw, ...)` and was left untouched.

Dead code removed: the top-of-function `scissor`, `scissor_l`, `scissor_t`,
`scissor_r`, `scissor_b` locals were computed from `draws[0]` but never
referenced anywhere in the emitted template (grep-confirmed before editing);
removed as part of deleting the collapse block they lived in.

## Real-lane verification

```
$ T=$PWD/traces/app-d3d9-3dmark05-vertexremap-enc1-r1
$ python3 scripts/tools/run_3dmark05_mini_replay.py \
    "$T/analysis/frame60-mini-replay-manifest-enc1.json" \
    --output-dir /tmp/pso-state-check --primitive-order original --vertex-order original
(exit code 0, summary JSON printed, no errors)

$ grep -o "colorWriteMask([0-9]*)" /tmp/pso-state-check/dxmt9_3dmark05_mini_replay.mm | sort | uniq -c
   2 colorWriteMask(0)
  15 colorWriteMask(15)
```

Both `colorWriteMask(0)` and `colorWriteMask(15)` are present (pre-fix this
was a single `colorWriteMask(0)` line, per the task's own statement of
expected pre-fix behavior, and independently confirmed live via `git stash`
above on the synthetic fixture). The counts (2 and 15) are PSO-**combo**
occurrences, not draw counts — see below for the derivation of how many
combos exist and why.

### Distinct PSO combo count — derived, not estimated

```
$ grep -n "const unsigned psoComboCount" /tmp/pso-state-check/dxmt9_3dmark05_mini_replay.mm
398:    const unsigned psoComboCount = 17;
```

Cross-checked by an independent count of PSO-creation call sites in the same
file:

```
$ grep -c "newRenderPipelineStateWithDescriptor:psoDesc error:&error\];" \
    /tmp/pso-state-check/dxmt9_3dmark05_mini_replay.mm
17
```

Both agree: **17 distinct `(shaderIndex, blendIndex)` combinations** occur
among this frame's 229 draws (vs. a hypothetical single shared PSO pre-fix,
or up to `shaderCount x distinct-blend-count` if the combo table were a
naive cross product instead of "only combos that actually occur").

### Depth-state table — for completeness

```
$ grep -n "const unsigned depthStateTableCount" /tmp/pso-state-check/dxmt9_3dmark05_mini_replay.mm
696:    const unsigned depthStateTableCount = 2;
```

This capture's 229 draws all use `depth_enabled=1, depth_func=4`
(LessEqual) and differ only in `depth_write` (1 vs 0 — depth-writing opaque
draws vs depth-testing-only draws), so only 2 distinct depth-stencil states
occur here. The `depth_func`-disagreement case (which needed a genuinely
different `compareFunction(N)` literal) is covered by the synthetic
two-draw fixture in `test_distinct_depth_compare_functions_are_emitted`,
which is real and passing (see GREEN output above) — this real capture
simply doesn't happen to exercise a `depth_func` disagreement, only a
`depth_write`/`color_write` disagreement (the defect this task targets).

### Source-of-truth cross-check (manifest, independent of the generator)

```
$ python3 -c "
import json
from collections import Counter
d = json.load(open('.../frame60-mini-replay-manifest-enc1.json'))
draws = d['draws']
cw = Counter(str(dr['state'].get('color_write','0xf')) for dr in draws)
print(len(draws), 'draws total; color_write counts:', dict(cw))
print('draw 0 color_write:', draws[0]['state'].get('color_write'))
"
229 draws total; color_write counts: {'0x0': 42, '0xf': 187}
draw 0 color_write: 0x0
```

Matches the task's own stated numbers (42 prepass / 187 color-writing draws
out of 229) exactly, confirmed directly from the manifest rather than taken
on faith.

## Compile check

```
$ cd /tmp/pso-state-check
$ xcrun -sdk macosx clang++ -std=c++20 -fobjc-arc -O2 -Wall -Wextra \
    -framework Foundation -framework Metal \
    -o dxmt9-3dmark05-mini-replay dxmt9_3dmark05_mini_replay.mm
(no output — clean compile)
$ echo $?
0
$ ls -la dxmt9-3dmark05-mini-replay
.rwxr-xr-x dididi wheel 367 KB ... dxmt9-3dmark05-mini-replay
```

Same clean `-Wall -Wextra` compile (zero warnings, zero errors, exit 0) for
the real 229-draw lane and, separately, for the small synthetic two-draw
fixture used by the new unit test (compiled via `run_3dmark05_mini_replay.py
--compile`, also exit 0, binary produced). The generator change did not
break codegen correctness.

I did **not** attempt to run either binary against a live Metal device to
check pixel output — the task explicitly scopes the black-output problem as
unresolved and out of scope, and this fix is not expected to change it. This
fix only changes which PSO/depth-state each draw binds; it does nothing
about whatever separate issue causes the mini-replay's rendered output to be
black.

## Regression fallout fixed

`tests/scripts/test_run_3dmark05_mini_replay.py::test_prepare_rewrites_argbuf_slots_and_summarizes_payloads`
pinned five substrings of the old collapsed-state codegen shape:

- `[encoder setRenderPipelineState:psos[draw.shaderIndex]];` → now
  `psos[draw.pipelineIndex]` (PSOs are now selected per combo, not per
  shader variant alone).
- `sourceAlphaBlendFactor =\n        blendFactor(2, true);` /
  `alphaBlendOperation =\n        blendOperation(3);` → the per-combo build
  block emits these as single lines now (no line wrap), so the assertions
  were updated to the single-line form actually emitted (verified via a
  throwaway generation + `grep -n ... | cat -A` before editing the
  assertions, to paste the exact expected text rather than guess it).
- `depthStateDesc.depthCompareFunction =\n        1 ? compareFunction(4) ...`
  → indentation changed from 8 to 10 spaces (the block now lives one scope
  level deeper, inside a `{ ... }` per depth-state block); assertion updated
  to match exactly, confirmed via the same throwaway generation.
- `[encoder setCullMode:cullMode(2)];` → cull is now genuinely per-draw
  (`cullMode(draw.cullMode)`), so the literal-2 assertion was replaced with
  a mechanism assertion that the encode loop reads `draw.cullMode` rather
  than a single baked literal.

This file is not wired into `tests/meson.build` (pre-existing gap, confirmed
via `grep -n "test_run_3dmark05_mini_replay" tests/meson.build` returning
nothing) so it did not show up in the `meson test --suite scripts` run, but
it is a real test in the repo that my change broke, so I fixed it rather
than leaving it silently wrong. Grepped all other `tests/scripts/*.py` for
the same literal patterns (`psos[draw.`, `setCullMode:cullMode`,
`sourceAlphaBlendFactor`, `depthStateDesc`, `colorWriteMask(`,
`alphaBlendOperation`) — only these two files matched, both now fixed.

Ran standalone after the fix:

```
$ python3 tests/scripts/test_run_3dmark05_mini_replay.py -v
... (11 tests) ...
Ran 11 tests in 0.659s
OK
```

## meson test result

```
$ meson test -C build --suite scripts --print-errorlogs
1/5 scripts - dxmt9:dxmt9-wine-resolve                     OK   0.05s
2/5 scripts - dxmt9:dxmt9-run-experiment-wsi-acquisition   OK   0.11s
3/5 scripts - dxmt9:dxmt9-run-experiment-renderer-defaults OK   0.11s
4/5 scripts - dxmt9:dxmt9-mini-replay-vertex-order         OK   0.17s
5/5 scripts - dxmt9:dxmt9-mini-replay-draw-state           OK   0.21s

Ok:                5
Fail:              0
```

(5 tests total in the `scripts` suite both before and after registering the
new test is expected to read as "was 4, now 5" — confirmed by
`grep -c "suite: \['scripts'\]" tests/meson.build` returning `5` after my
edit, i.e. exactly one new entry added.)

## Files changed

- `scripts/tools/run_3dmark05_mini_replay.py` — the fix itself: added
  `draw_blend_state`, `draw_depth_state`, `draw_cull_mode`,
  `pso_build_statement`, `depth_build_statement`; reworked `render_source()`
  to dedup per-draw state into `pipelineIndex`/`depthStateIndex`/`cullMode`
  fields on `DrawEntry` and emit one literal PSO/depth-state build block per
  distinct combo instead of one collapsed descriptor baked from
  `draws[0]`.
- `tests/scripts/test_mini_replay_draw_state.py` — new regression test
  (6 tests): unit-level checks on the three extraction helpers, plus
  CLI-driven checks on the emitted source (distinct `colorWriteMask`
  values incl. 15, the color-writing draw's PSO literal is 15 not 0 via its
  own `pipelineIndex`, distinct `compareFunction` values when `depth_func`
  disagrees).
- `tests/meson.build` — registered `dxmt9-mini-replay-draw-state` following
  the `dxmt9-mini-replay-vertex-order` pattern exactly (`suite: ['scripts']`,
  `workdir: meson.project_source_root()`, `is_parallel: false`).
- `tests/scripts/test_run_3dmark05_mini_replay.py` — updated 5 stale
  assertions in `test_prepare_rewrites_argbuf_slots_and_summarizes_payloads`
  that pinned the old collapsed-state codegen shape (see "Regression
  fallout fixed" above).

## Concerns

- The black-output problem is untouched and unexplained by this change —
  explicitly out of scope, not claimed as fixed. Do not read the "compile
  check" section above as evidence about pixel correctness; it only proves
  the generator still emits valid Objective-C++.
- `tests/scripts/test_run_3dmark05_mini_replay.py` is not registered in
  `tests/meson.build` (pre-existing condition, not introduced by this
  change). I fixed its now-stale assertions because I broke them, but did
  not add it to meson.build since that's a separate, unrelated gap the task
  did not ask me to close.
- `blendStateCount` was an unused-but-harmless local removed along with the
  earlier struct-table approach when I switched to unrolled literal codegen
  after the coordinator's correction that the required grep-based
  verification needs literal argument text, not a runtime-indexed table.
