---
domain: present-pacing
workload: Actual PE32/PE64 D3D9 SM3 offscreen constant-state oracle
title: "Inline constants: real PE mode receipts and GPU pixels"
type: evidence
status: correctness-prerequisite
updated: 2026-09-08
source: tests/conformance/d3d9/evidence/inline-constant-pixels-20260908.json
related: present-pacing-inline-constant-replay.264.md; present-pacing-pe-retain-fault.263.md; present-pacing-inline-constant-current.258.md
---

# Inline constants through actual PE and GPU execution

This test-only increment supplies bounded pixel evidence for R-BACK-2.52,
complementing [H264's](present-pacing-inline-constant-replay.264.md) native
producer/replay/Slot byte oracle. No production code, default, ABI, chunk
policy or scheduling transition changes. Inline remains default-off; neither
test runtime nor the diagnostic clocks establish CPU savings.

## Fixture and independent oracle

`tests/conformance/d3d9/d3d9_inline_constants.cpp` creates a HAL hardware-VP
device through the real PE COM API. Authored executable VS/PS 3.0 bytecode
uses float constants directly, integer constants as REP counts and bool
constants as IF conditions. Shader creation does not use D3DCompiler or
scanner-only placeholder bytecode, and failed hardware setup is a failure,
not a silent software-VP fallback.

Six isolated VS/PS float/int/bool A, repeated A, B, A sequences run through
DrawPrimitive, DrawIndexedPrimitive and DrawPrimitiveUP: 72 draws in distinct
cells of one 64x288 A8R8G8B8 target. Static clip-space POSITION geometry makes
each sample interior to its triangle. There is no intervening viewport,
scissor or readback between the draws, and one final GetRenderTargetData plus
LockRect checks independent expected ARGB values. The fixture accepts a
three-code-value per-channel tolerance; the recorded positive matrix matches
every expected sampled pixel exactly. This is 72 pixel samples per process,
not a whole-texture or whole-scene image comparison.

Ordinary/indexed draws exercise the inline fold. UP is deliberately a
**standalone-drain control**, not an inline-UP implementation. Constants use
c0/i0/b0, plus fixed c4/c5 scales; broader register/layout/consumer coverage is
not inferred from this bounded SM3 domain. PE environment text never selects
the expected colors and is not accepted as proof that the mode executed.

## Real mode and lifetime receipts

The sequential Sikarugir cx-24.0.7 builtin matrix covers PE32/PE64, inline
OFF/ON and replay offload OFF/ON: eight fresh processes and 576 draw/pixel
checks. `matrix-qualified.json` requires matching source/fixture hashes,
actual staged-image receipts, equal pixel signatures, device Release zero,
eight zero renderer-error counters and these exact records per process:

| Receipt | Inline OFF | Inline ON |
|---|---:|---:|
| Ordinary / indexed / UP draws | 24 / 24 / 24 | 24 / 24 / 24 |
| Standalone VS / PS constant records | 19 / 19 | 6 / 6 |
| Total imported records, including setup/cleanup | 118 | 92 |
| GPU draw counter | 72 | 72 |

The tracked `tests/conformance/d3d9/evidence/inline-constant-pixels-20260908.json`
preserves compact verdicts, exact pixel signatures, image/source hashes and
negative-control results. Raw logs and reproducible collection scripts remain
under `experiments/output/pe-inline-constant-pixels-20260908/analysis`; the
compact receipt is not a replacement for those local raw artifacts.

Decimation is 1 and Info logging is enabled, so PE teardown reports all
append types; `command_chunk_records` independently matches their sum.
Positive worker queue-depth sample counts versus zero identify offloaded
versus synchronous replay. These executions exercise both runtime choices;
they do not enumerate worker interleavings. CPU-ready Tape stays OFF.

`run_pixels.py`, `run_remaining_matrix.py` and `qualify_matrix.py` in the
analysis root preserve the exact recipes and checks. The runner is the
canonical `scripts/tools/run_d3d9_conformance.py`, with `--pe-arch`, matching
build/executable directories and `--aux-exe d3d9_inline_constants_<arch>.exe`.
The recipes set `DXMT9_PE_INLINE_CONST_DELTA=0|1`,
`DXMT9_OFFLOAD_COMMIT_REPLAY=0|1`, `DXMT9_PE_STATS_DECIMATION=1`,
`DXMT_LOG_LEVEL=info`, `DXMT_LOG_PATH=none`, `DXMT_PERF_COUNTERS=1` and
`WINEDEBUG=-all`. Bare process success or the selected-env line is insufficient:
the canonical runner's JSON verdict must pass and the exact receipt matrix
must agree. App-local deployment has not been run; manifest status is partial.

## Production negative control and restoration

Main temporarily made the sparse `DeviceReplaySink::setConstants` overload
in `src/d3d9/device_c_chunk_replay.cpp` return success without applying a
VS_BOOL section starting at b0. `negative-inline-vs-bool.patch` preserves the
exact mutation. Real PE32, inline ON, worker replay then fails exactly the
ordinary and indexed VS-bool B samples: stale `0xff293d52` instead of
`0xff295752`. The UP bool sample and all other families remain correct.
The same mutated provider with inline OFF passes all 72 samples. This is an
actual production replay mutation, not a test expectation or shader edit.

The negative fixture exits with failure and its JSON verdict is fail even
though the aggregate conformance runner itself returns zero. The evidence
driver correctly rejects that run. Its PE append counts are unchanged,
demonstrating why counts alone cannot prove pixels.

Source restoration recovers SHA-256
`02a8ff6594ffd4b8a7e26394fb8052f1c4d7fc1b61f7322274e688accb4fda4d`;
the provider returns byte-identically to
`e4543457d6992f1a2a8a088a590e75170360d8938e4c4aeda582ab3b1da4a09a`.
No production diff remains. The restored real PE32 ON check passes all 72
samples. Focused arm64 5/5 and x86_64 6/6 tests pass, including the final-Slot
oracle, producer differential, shadow, record validation, manifest and x86_64
install-name checks. These are retained separately from the deliberate failure. Existing TLA
evidence remains in H264/H263; no new concurrency proof is claimed.

## Teardown discovery and exclusions

Earlier r1-r4 probes are not qualified positive matrix rows. Their pixels
passed, but they lacked the PE destructor receipt; r2-r4 explicitly returned
device Release 2 even after Reset. Cleanup had called GetBackBuffer merely
to restore the implicit target. Code inspection finds the cached swapchain
owns a backbuffer wrapper which retains its container, with both wrappers
also retaining the device. Reset releases device cache references but does
not break that surface/container cycle. The separate production lifetime
residual is not fixed by this test.

CreateDevice still creates its implicit swapchain/backbuffer; the final fixture
does not acquire the extra cached PE wrappers through GetBackBuffer. It unbinds its
shader/buffer resources, releases caller-owned references, then Resets to
detach the still-bound explicit target before releasing the device. All eight
r5 rows reach device Release zero and emit final PE statistics. It neither
forces extra Release calls nor accepts an outstanding device as success.

Remaining promotion gates include wider register/cap/layout, FFP and
ProcessVertices/full-snapshot/StateBlock/capture/failure boundaries,
cross-source and CPU-ready variants, and repeated matched low-observer
CPU/FPS economics. The native H264 and PE fault H263 domains remain
complementary. This evidence does not promote the lane or update README FPS.
