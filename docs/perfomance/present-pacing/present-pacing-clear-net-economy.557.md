---
domain: present-pacing
workload: STALKER original captured source 5, serial Encode
title: "Full private Clear candidate including failure repair saves 1.9955 ms local Encode CPU"
type: evidence
status: positive-local-net-saving-not-default-live-transfer
updated: 2026-10-15
source: traces/stkcop-clear-net-economy-h557/{contract.md,run.py,rows.jsonl,summary.json,verify_raw.py,independent-verification.json}; traces/stkcop-clear-backed-null-h551/build.json; traces/stkcop-clear-failure-drain-h556/{build.json,verification.json}
related: present-pacing-clear-failure-drain.556.md; present-pacing-clear-economy.552.md; present-pacing-prefix-live-result.540.md
---

# Reprice the complete private candidate, not just the fusion branch

H557 compares the H551 serial image (extension OFF) with H556 (extension ON,
three failure relays and the ordinary-source cold failure drain). This is a
two-image net comparison, unlike H552's same-binary policy contrast. The
successful-path effects of adding failure handling are included; injected
failure waits are not part of these successful-run timings.

The fixed population is two warmups per arm, four A/A pairs and eight
alternating AB/BA pairs: 28 fresh-process/device executions. No retries,
exclusions or sample extension occur. The same original H502/r5 input, source
boundaries, source-probe binary and coarse supply/planning observer are used.
Per-command/per-draw clocks and fusion witness logging are disabled.

| Scope | A median ms | B median ms | Paired median saving ms | Maximum A/A ms | Positive pairs |
|---|---:|---:|---:|---:|---:|
| Source 5 whole Encode — primary | 36.0870 | 34.1290 | **1.9955** | 0.4020 | 8/8 |
| Seven original sources, Encode sum | 59.7975 | 58.0340 | 1.8385 | 1.0100 | 8/8 |
| Source 5 command body — nested secondary | 19.4615 | 17.4425 | 1.9215 | 0.4030 | 8/8 |

The paired median is not the difference of the two arm medians. Primary
paired savings range from 1.579 to 2.192 ms. The predeclared finite screen
passes: paired median exceeds maximum A/A difference and at least seven pairs
are positive. This is not a statistical confidence interval. Nested body and
whole Encode savings must not be added.

All 28 executions preserve original source/output BGRX SHA-256
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`,
seven-source mapping, production-counter query transcript and balanced wrapper
lifetimes. `verify_raw.py` independently decodes the raw same-thread
`encode_begin`/`encode_end` CPU cursors for source 5 / local sequence 8,
recomputes all primary pairs, verifies execution order and checks image,
script, contract, result and pixel hashes. Loader-path evidence comes from
the separate original-capture preflight; timing runs use the pinned package
and explicit library-search environment without loader logging.

## What is now established

The larger private candidate, including its currently implemented failure
handling, still removes approximately 2 ms of observer-inclusive Encode CPU
from this captured source. H552's earlier positive result is not erased by
the added bookkeeping. The new figure is independently measured rather than
inherited from H552.

It is **not** 2 ms per live frame, an FPS improvement, or proof of the largest
default-live CPU owner. Capture replay has a different preparation and cache
history. H540's earlier failure to transfer a Replay saving to live workload
is a direct reason not to perform that conversion.

## Next decision boundary

H556 still has two deferred-terminal-suffix calls outside its ordinary-source
failure-drain boundary. Close that concrete coverage gap before live candidate
use; do not add more identical local timing runs to justify promotion.
Then measure baseline/candidate whole-thread CPU on the same late live camera
interval, with matched low-frequency self reporters and independent Primary
attribution. Residual observer transfer and other-thread coverage must be
explicit before declaring a default/global maximum. Broader hardware-query
and recovery claims remain outside this production-counter capture result.

No canonical runtime edit, Wine staging, commit, push or README FPS update is
made by H557. Host process snapshots are retained; this is not a clean live
FPS benchmark.
