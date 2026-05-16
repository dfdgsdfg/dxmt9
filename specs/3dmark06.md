# 3DMark06 Shader Translator Status

Last audited: 2026-05-16

## Current Evidence

3DMark06 is currently process-level useful but not yet trustworthy rendering
evidence. Native Microsoft `d3dx9_28`, `d3dcompiler_43`, and
`d3dcompiler_47` avoid the Wine/vkd3d-shader SM1 HLSL compiler failure. The
previous captured dxmt9 visual blocker was later in the shader translator:

```text
unsupported D3D opcode: opcode_65
```

`opcode_65` is `D3DSIO_TEXKILL`. Current working-tree code now has `TEXKILL`
decode/lowering and passing source/corpus evidence. Follow-up 3DMark06
re-validation on 2026-05-16 with `texkill-cf1` and `texkill-cf2` no longer
shows `unsupported D3D opcode`, Wine compiler errors, or unhandled page faults.
Both runs still fail the harness as `black_screen` with an all-black internal
dump, so the active blocker moved past D3DBC opcode support into render output
or capture/present behavior. The dumped shader that originally exposed the
blocker is:

```text
experiments/output/3dmark06-shader-dump-cf2-shaders/shader-4803115731238309921.bin
```

It is a `ps_3_0` shader and contains token `0x01000041` at word 47.

## Audit Inputs

This table reflects:

- opcode constants in `src/dxmt9/dxmt9_d3d9_bytecode.hpp`
- fixed/fallback operand decode in `src/dxmt9/dxmt9_shader_decoder.cpp`
- vertex/pixel MSL lowering in `src/dxmt9/dxmt9_shader_metal_ir.cpp`
- native source-contract coverage in `tests/native/shader/shader_transform_spec.cpp`
- `python3 scripts/tools/shader_corpus_tool.py gaps`

The corpus report currently has 61 entries: 60 passing and 1 failing
(`render_state/dxmt9_alpha_test_readback.shader_test`). It reports 64 covered
passing opcodes, 18 missing opcodes, and 84/711 covered model/opcode pairs.

## Model Status

| Model scope | Status | Evidence / gap |
|---|---|---|
| `ps_2_0`, `ps_3_0`, `vs_2_0`, `vs_3_0` | implemented, partial evidence | Decoder accepts SM2/SM3 and corpus has passing coverage, including a runtime `vs_2_0` color-triangle probe. Model/opcode pair coverage remains sparse, so a missing corpus pair is not automatically a missing lowering. |
| `ps_1_1`, `ps_1_2`, `ps_1_3`, `ps_1_4`, `vs_1_1` | policy-rejected | `translateD3DBytecodeToSpirv()` rejects `module.major < 2` with `only SM 2.x and 3.x bytecode is supported`. Native D3DX is therefore required to avoid Wine/vkd3d-shader's SM1 compiler blocker for current 3DMark06 runs. |

## Opcode Status

| Classification | Opcodes | Current evidence | Missing evidence / action |
|---|---|---|---|
| implemented, corpus covered | `ABS`, `ADD`, `BREAK`, `BREAKC`, `BREAKP`, `CALL`, `CALLNZ`, `CMP`, `CND`, `CRS`, `DCL`, `DEF`, `DEFB`, `DEFI`, `DP2ADD`, `DP3`, `DP4`, `DSX`, `DSY`, `ELSE`, `ENDIF`, `ENDLOOP`, `ENDREP`, `EXP`, `EXPP`, `FRC`, `IF`, `IFC`, `LABEL`, `LOG`, `LOGP`, `LOOP`, `LRP`, `M3x2`, `M3x3`, `M3x4`, `M4x3`, `M4x4`, `MAD`, `MAX`, `MIN`, `MOV`, `MOVA`, `MUL`, `NOP`, `NRM`, `POW`, `RCP`, `REP`, `RET`, `RSQ`, `SETP`, `SGE`, `SGN`, `SINCOS`, `SLT`, `SUB`, `TEX`, `TEXKILL`, `TEXLDD`, `TEXLDL` | Fixed operand decode and real MSL emission exist. Native source-contract tests cover decode/classification, arithmetic, transcendental, matrix, texture LOD, flow control, constants, modifiers, semantics, indexed constants, `TEXKILL`, `BREAKC`, and `CALLNZ`; corpus has at least one passing entry for each listed opcode. `texkill-cf1`/`texkill-cf2` and `shader-complete-cf1` provide app-level evidence that shader opcode support no longer trips in the current 3DMark06 path. | Broaden model/opcode pair coverage. Continue black-screen investigation outside the current shader opcode surface. |
| deterministically unsupported | `TEXCOORD`, `TEXBEM`, `TEXBEML`, `TEXREG2AR`, `TEXREG2GB`, `TEXM3x2PAD`, `TEXM3x2TEX`, `TEXM3x3PAD`, `TEXM3x3TEX`, `TEXM3x3DIFF`, `TEXM3x3SPEC`, `TEXM3x3VSPEC`, `BEM`, `TEXDEPTH`, `TEXREG2RGB`, `TEXDP3TEX`, `TEXM3x2DEPTH`, `TEXDP3`, `TEXM3x3` | Opcode names are stable and all legacy texture opcodes now throw `unsupported legacy texture opcode: <name>` instead of silently no-oping. `opcode_audit_spec` keeps the legacy family out of fully-supported texture/sample classification. | Implement only when app evidence requires them. For current 3DMark06 native-D3DX runs, SM1 legacy texture direct-bytecode support remains out of scope. |
| metadata / terminators | `COMMENT`, `END`, `PHASE` | Decoder skips comments, stops at `END`, and treats `PHASE` as no-op metadata. | No app blocker known. Keep parser tests covering comments and instruction alignment. |

## 3DMark06 Re-validation

The `TEXKILL` implementation was re-tested in the app with:

```sh
python3 scripts/run_apps/run_experiment.py run 3dmark06 --timeout 180 --capture-frames 1 --output-suffix texkill-cf1
python3 scripts/run_apps/run_experiment.py run 3dmark06 --timeout 180 --capture-frames 2 --output-suffix texkill-cf2
```

Observed result for both runs:

- no `unsupported D3D opcode: opcode_65`
- no `D3DCompile2`, `E5017`, `gametest_proxycon`, or unhandled page fault
- process exited through the launcher watchdog with return code 0
- internal dump source was selected and remained fully black

The broader shader batch (`BREAKC`, `CALLNZ`, ps_3_0 arithmetic corpus) was
re-tested with:

```sh
python3 scripts/run_apps/run_experiment.py run 3dmark06 --timeout 180 --capture-frames 1 --output-suffix shader-complete-cf1
```

Observed result:

- `status=fail` solely due `black_screen`, `returncode=0`, `timed_out=false`
- `capture_source=internal_dump`, mean luma and variance both `0.0`
- internal dump hash stayed
  `sha256:20f08885b32fd899b85bcdfc5d390a99015ecd71de0242214234ec1efba549e7`
- no `unsupported D3D opcode`, `D3DCompile2`, `E5017`,
  `gametest_proxycon`, or page fault in the output logs
- logs show sustained draw/present traffic: 4655
  `device_draw_indexed_primitive` lines and 12 `device_present` lines

The next useful renderer gate is therefore no longer opcode translation. It is
to determine why the captured internal frame is black despite sustained
`DrawIndexedPrimitive` traffic after the previous shader translator blocker was
removed.
