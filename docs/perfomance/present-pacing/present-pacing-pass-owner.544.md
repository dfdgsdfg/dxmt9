---
domain: present-pacing
workload: STALKER CoP H502/r5 original Normal input
title: "Pass preparation is smaller than encoder creation and outer transition work"
type: evidence
status: local-pass-owners-separated-removal-open
updated: 2026-10-15
source: traces/stkcop-pass-owner-h544/{contract.md,pass_observer.hpp,dxmt9_draw_encoder_chunk.mm,dxmt9_render_pass_encoder.mm,build.py,build.log,build.json,run.py,analyze.py,summary.json,verify.py,verification.json,parser-tests.log}; traces/stkcop-pass-owner-h544/{image,A0,B0,B1,A1}-receipt.json
related: present-pacing-run-milestones.543.md; present-pacing-encode-command-owner.542.md; present-pacing-query-proof-wild.429.md
---

# Store-proof traversal is not the dominant pass-entry cost

H543 left preparation versus actual Metal encoder creation unresolved. H544
observes these boundaries in a private copy of
`dxmt9_render_pass_encoder.mm::beginRenderPassWithStoreProofLookahead`.
The nested color/depth proof routines report their own self CPU and visited
command counts. Nothing changes load/store decisions or rendering semantics.

## Execution and authority

- Xcode 27.0, Metal compiler 32023.921, Apple M1/Metal 4; environment check passed.
- Frozen H537 A archive/provider/runner and original H502/r5 input. Two runtime
  objects are replaced in private images; canonical sources, objects and Wine
  deployment are not changed. Original H537 images remain byte-identical.
- H543's removal of nested draw clocks is retained. All four timing runs have
  identical command/run milestones; only B adds private per-pass/proof timers.
- Fixed order: image-OFF authentication, A0 OFF, B0 ON, B1 ON, A1 OFF. Every run
  has a fresh process/device. All five match original source/output pixels,
  seven source mappings, 670 balanced wrappers and query obligations.
- Source 5 maps to sequence 8. Both B runs record the same 80 pass-start command
  indices. Same-run joins place all pass endpoints inside the corresponding
  DrawRun pass-entry scopes on the same native thread. No missing endpoints,
  observer errors, overflow or nested draw events occur.
- Independent log reduction verifies 160 rows and 1,280 CPU/wall differences;
  existing supply-parser tests pass 42/42. A parser metadata string/integer
  assertion was corrected without changing or repeating any execution.
- H544 and domain-navigation provenance checks pass; `git diff --check`
  passes. The repository-wide provenance check still fails on 11 absent build
  log artifacts cited by three older leaves (capacity ownership recovery,
  GPU position invariance and query default promotion). Those unrelated
  historical evidence gaps were not rewritten as part of this experiment.

The observer uses bounded thread-local rows and executing-thread libproc CPU
queries, not H512's rejected external-counter endpoints. It exports rows only
after source work, outside the command body; whole Encode may include export.
No new TLA transition is needed for observer-only private copies. This is not
an optimization or a promotion of a rendering policy.

## Source 5 pass costs

All values below are **observer-inclusive ms per captured source**, not
ms/frame, warm-state costs, default-path ownership or removable CPU.

| Component | B0 CPU | B1 CPU | B0 wall | B1 wall |
|---|---:|---:|---:|---:|
| Prepare attachments, including proof | 0.717 | 0.783 | 0.680 | 0.734 |
| Metal encoder creation call | 2.501 | 2.571 | 2.534 | 2.626 |
| Commit/configure encoder | 0.137 | 0.157 | 0.110 | 0.113 |
| Output assignment/return | 0.051 | 0.052 | 0.037 | 0.039 |
| Helper total | 3.406 | 3.563 | 3.361 | 3.512 |

The creation scope includes the WMT wrapper and Metal call, not exclusively
driver instructions. CPU/wall clocks have distinct sampling endpoints and
microsecond granularity; small scopes are not per-call precision claims.

| Nested proof scope, already included in preparation | Calls | Commands visited | B0 CPU | B1 CPU |
|---|---:|---:|---:|---:|
| Depth/stencil | 85 | 1,461 | 0.120 | 0.125 |
| Color | 83 | 1,792 | 0.356 | 0.417 |
| Total | 168 | 3,253 | 0.476 | 0.542 |

Counts agree across both B runs. Proof CPU must **not** be added to preparation.
The roughly 0.5 ms proof scope does not explain the 6.573/6.802 ms whole
pass-entry scopes. The helper accounts for 3.406/3.563 ms; **3.167/3.239 ms
remains outside it**, including old-pass handling and surrounding start-pass
work. This residual is not assigned wholesale to `endEncoding`, locks,
attachment planning or argument-buffer construction.

## Observer limits and decision

Command-body CPU is 23.011/23.397 ms in A0/A1 and 22.937/23.778 ms in B0/B1.
Paired ON-minus-OFF differences are -0.074/+0.381 ms. Their opposite signs
do not establish negligible overhead or permit a calibrated subtraction.
The whole draw loop remains 13.481/13.965 ms in B, larger than any pass
subcomponent. No absolute private-image comparison with H543 is a product
performance result.

**Deprioritize a new store-proof summary/cache as the primary CPU fix.** The
measured scope is small, already includes observer cost, and has not been
shown redundant or safely removable. H428's query-preservation proof elision
already exists; the current production-counter input has Metal occlusion OFF.

Next distinguish the outer pass residual's lifecycle closure from post-open
labels and argument-buffer setup. Then isolate the whole draw loop's binding,
uniform and actual issue work without reinstating unconditional per-draw
timers. Driver creation itself cannot be declared removable without a
semantics-preserving pass-policy experiment. Original readbacks remain a local
oracle with `content_authority=unknown` and `timing_qualified=0`, not a real
visibility-query oracle. Default live largest-owner and correctness-qualified
removal-budget goals remain open.
