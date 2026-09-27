---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H385: Streaming submit-summary fold"
type: implementation
status: implemented-no-runtime-gain-established
updated: 2026-09-26
source: src/dxmt9/dxmt9_queue.cpp; tests/native/backend/dod_replay_observer_spec.cpp; traces/app-d3d9-stkcop-submit-summary-h385/{run.py,summarize.py,receipt.jsonl,supply.csv,summary.json,submit.json,waits.json,supply-analysis.json}; experiments/output/app-d3d9-stkcop-bench-submit-summary-h385/{result.json,dxmt9.log}
related: present-pacing-submit-summary-family.384.md
---

# H385: Streaming submit-summary fold

Following [H384](present-pacing-submit-summary-family.384.md), remove the
temporary observation vector in `src/dxmt9/dxmt9_queue.cpp`,
`summarizeCommands`. This is the Unix queue's Encode-to-submit summary, not
PE serialization. Each command observation is folded immediately into the
fixed-size summary accumulator. Both the span API and the production Slot
API use `accumulateChunkObservation`.

## Preserved contract

- Preserve sequence/slot identity, default frame, category flags and flag OR.
- The last Draw-class observation owns all three shader hashes. Clear and
  ColorFill are Draw-class observations with zero hashes; they must overwrite
  earlier shader hashes. Present retains hashes while contributing flags.
- Preserve resource resolver order, invocation count and exception propagation.
- Do not alter submission validation, queue locks, resource lifetime,
  source/completion order, scheduling or GPU commands.

## Structural effect and verification

For a nonempty compatible Slot summary, remove one vector allocation and
deallocation, N stored observations and a second N-element traversal. The
summary accumulator is O(1) storage rather than O(N) temporary storage.
Command-view reconstruction, compatibility/hash computation and resource
resolution remain unchanged. There is no new knob or alternate runtime path.

`tests/native/backend/dod_replay_observer_spec.cpp` exhausts 3,906 sequences
of length zero through five over shader Draw, zero-hash Draw, Blit,
StretchRect and Present. An independent reverse last-writer oracle checks
hashes; all summary fields are checked. Actual Slot tests cover Draw/Clear/
ColorFill/Present ordering, resolver ordering and early exception propagation.
Focused native tests, the x86_64 observer test, the unix install-name audit
and `dxmt9-verify-tla` (197.10s) pass. Both PE staging builds are current;
the Unix provider was rebuilt through canonical `meson compile`.

This is a local pure summary transformation, not a stateful rendering policy
change: no new interleaving model or GPU pixel oracle is required for its
value equivalence. Existing queue TLA checks are still run as regression
coverage; they do not establish driver/pixel behavior.

## Measurement boundary

H384's 4.163ms summary total includes all summary work, not just the vector.
Removing this vector must not be reported as eliminating that entire cost.
No FPS improvement is claimed.

The rebuilt candidate completed the same catalogue Day/high 1280x720 lane,
`-max-threads 2`, `snd_targets 16`, perf profile, with PASS, no failures and
zero GPU command-buffer errors. The result records all staged binary hashes.
No build, test or stack sampler ran concurrently. Trace 2700–2720 closed
without overflow/incomplete writers; the interior 2702–2718 contains 16
scene intervals and no incomplete submit/holder endpoints.

| Interior-window metric | H384 vector | H385 streaming |
|---|---:|---:|
| Complete submit sources / legacy members | 38 / 38 | 32 / 32 |
| Safety validation wall | 0.330ms | 0.305ms |
| Summary wall | 4.163ms | 4.135ms |
| Summary thread CPU | 4.167ms | 4.132ms |
| Summary wall / Present | 0.2602ms | 0.2584ms |
| Driver commit wall | 0.764ms | 0.711ms |
| Window wall | 1087.527ms | 737.698ms |

The raw summary difference is only 0.028ms across 16 intervals; these runs
have different source composition and are not identical-input A/B evidence.
The much shorter window cannot be attributed to this summary change. The
structural allocation/traversal reduction is established, but meaningful
runtime savings are not. This integration result is not human visual/audio
certification or a GPU exact comparison.

H385 still measures 19.214ms across 1,409 mark-ticket acquisition attempts,
with only 1.108ms of observed other-thread holder overlap, and zero StateBlock
space waits in the interior. Replay is empty 56.58% of the window. Most of the
acquisition time remains unattributed; do not rename it scheduler overhead.

## Next discriminating test

If pursuing the remaining summary cost, split `commandAt` view construction,
compatibility/sampler scanning, resolver calls and shader hashing using a
bounded or same-input harness, not unconditional per-command clocks. Do not
remove safety validation or introduce persistent cached summaries without
resource identity and mutation invalidation evidence. H385 alone does not
establish summary as the dominant application bottleneck.
