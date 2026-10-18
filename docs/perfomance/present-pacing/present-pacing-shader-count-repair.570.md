---
domain: present-pacing
workload: STALKER Day/high 1280x720 diagnostic late-window episode
title: "Legacy pixel-shader length repair unblocks the font2 creation path"
type: evidence
status: live-correctness-verified
updated: 2026-10-16
source: traces/stkcop-shader-failure-live-h569/{layout-before.json,native-before.txt}; traces/stkcop-shader-failure-live-h569/A0/{dxmt9.log,xray_engine.log,receipt.json}; traces/stkcop-shader-count-fix-h570/{build-v2.json,related-native-tests.txt,capture-verification.json,capture-off-receipt.json,capture-on-receipt.json,live-verification.json}; traces/stkcop-shader-count-fix-h570/A0/{receipt.json,catalogue-result.json,workers.bin,post-tail.png,dxmt9.log,xray_engine.log,restore/restored.json,recovered.json}
related: present-pacing-owned-worker-live.567.md; present-pacing-owned-worker-preflight.566.md; present-pacing-owner-live.564.md
---

# A shader-length bug, not evidence of a CPU bottleneck

H568 adds failure-only private PE/Unix shader-creation diagnostics while
preserving the earlier worker observers. H569 reaches all seven scene knots
but exits 1. The engine reports `font2` / `CreatePixelShader 0x8876086c`;
the Unix diagnostic identifies `failure=provider-null`, version `ffff0101`.
This observed call passed PE validation and the bridge terminal guard.

`src/d3d9/device_c_shader_dump.cpp::computeShaderBytecodeWordCount` used two
operands for TEX/TEXCOORD/TEXKILL regardless of pixel-shader version. For
ps_1_1 TEX, it consumes the following MAD opcode as a second operand and
loses the actual END boundary. The cached shader has a four-byte cache CRC
followed by 104 DWORDs, including comments, DEF, TEX, MAD and END.

## Deterministic evidence and scoped repair

The harness calls the actual frozen production function through `dlsym`.
Zero-padding to the existing 65,536-word scan limit avoids out-of-buffer
access in the deliberately failing test; it is not a product workaround.

| Input | H568 unchanged function | H570 repaired function |
|---|---|---|
| Actual cached ps_1_1 `font2` | Rejects | Accepts exactly 104 DWORDs |
| Handcrafted TEX/MAD/END | Rejects | Accepts exactly 9 DWORDs |

The production repair changes only legacy pixel-shader operand counts:
TEX/TEXCOORD use one operand in ps_1_1–1_3 and two in ps_1_4; TEXKILL uses
one. Other instruction handling, including unknown-opcode encoded lengths,
is unchanged. This is not a complete SM2/SM3 decoder audit or a fix for the
pointer-only API's general malformed-buffer limits.

`dxmt9-shader-bytecode-word-count-spec` fails before and passes after the
repair. Its twelve version/opcode combinations also cover MAD desynchronization;
additional cases cover VS MOV, COMMENT/DEF embedded END values, PHASE,
SM2 MOV, unknown encoded lengths, missing END and null arguments. Four
related shader validation/translator/transform/opcode tests pass.

The isolated x86_64 diagnostic provider differs in exactly one archive
member, `device_c_shader_dump.cpp.o`; all 48 other members are identical.
PE images and existing observers remain unchanged. Both linked images and
the Unix install names are recorded and checked. Native Meson regeneration
required the explicit current Xcode `SDKROOT`; no toolchain configuration
or ABI schema was changed.

## Original capture checks

The existing source capture passes worker-observer OFF and ON:

- Seven sources; 670 created/released wrappers.
- Query issues 9/9, polls 5/5, results 4/4, readiness 5/5.
- Original source and output pixels match exactly, SHA-256
  `1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
- ON has ten worker instances, 188 rows and all observed bodies closed.

These are finite CPU/GPU correctness checks, not FPS measurements. H569's
14 shared-file restorations and four recovered temporary services were
independently hash-checked before the next episode. The original failed
episode is retained, not retroactively accepted.

## Live repair episode

H570 reaches all seven spatial knots and exits **0**, without timeout or the
`font2` creation error. The post-tail image shows coherent building geometry;
this single screenshot is not a complete-run pixel oracle. The catalogue
wrapper returns 1 only for the predeclared `missing_capture` condition: its
600-second capture delay intentionally exceeds this bounded run. Game exit,
spatial gates and the independently captured post-tail image pass.

Normal shutdown also closes the eight compiler-worker instances missing from
H567. The structural reader accepts 15 instances and 662 rows; all 15 observed
worker bodies now have finite enclosing/closed-body bounds for knots 1–6.
There are **zero unbounded observed instances**. No forced worker wake or
in-window observer change was needed. This closes this episode's missing
compiler endpoints, not whole-thread startup/destruction or shared-driver
coverage. The run was predeclared correctness-only, so it is not retroactively
admitted as a timing comparison.

All 14 shared files were restored and all four temporary services recovered;
the collector independently verifies hashes and that owned Wine processes
have exited. The canonical Wine staging is restored, not left on the private
diagnostic image.

Remaining performance gates are work outside observed bodies, observer-to-default
transfer, and qualification of the largest renderer thread with its removable
cost. H564 remains the current accepted live self/root timing evidence; this
shader repair does not itself establish a new CPU maximum or FPS improvement.
