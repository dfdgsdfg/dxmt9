---
domain: present-pacing
workload: STALKER H502/r5 actual inputs, post-oracle owned preparation
title: "Exact render-state prefix shortcut reduces local preparation by about one tenth"
type: evidence
status: local-cpu-candidate-qualified-production-transfer-open
updated: 2026-10-16
source: traces/stkcop-pair-shortcut-h606/{contract.md,candidate.hpp,derive_candidate.py,candidate-derivation.json,predicate_native.cpp,prepare.py,derivation.json,build.py,build.json,run.py,runs.json,analyze.py,analysis.json,codegen.py,codegen.json,verify.py,verification.json}; traces/stkcop-pair-shortcut-h606/{O0,P0,P1,O1}.log; traces/stkcop-pair-shortcut-h606/{P0,P1}.csv
related: present-pacing-prepare-deferred.605.md; present-pacing-prepare-local.604.md; present-pacing-queue-partition.603.md
---

# Exact-prefix candidate has a local CPU effect, not a live FPS claim

H606 tests one small pure refinement of
`include/dxmt9/core_snapshots.hpp`, `flatRenderStatesEqualExceptAlphaTest`.
For non-overflow sets with equal counts, exact equality of the clamped active
entry prefixes immediately proves compatibility. Otherwise the original merge
predicate runs unchanged. Overflow retains its original full equality rule.
Hash equality is not trusted; inactive tail bytes are not used as authority.

Identical prefixes cause the original merge walk to advance both pointers on
every iteration, even with duplicate or unordered keys. The candidate therefore
does not need a new validity or generation assumption. No state, ownership,
ordering or synchronization changes; pure differential tests apply instead of
a new interleaving model.

The complete candidate batch predicate is generated from the hash-bound
production function with only its render-state helper substituted. It exists
only in the private experiment. Production rendering still uses the reference.

## Correctness and measurement boundary

- Each of ARM64 and x86_64 passes 1,468,900 render-state checks and 50,000 full
  draw-state checks. The bounded enumeration contains 585 short sequences and
  all overflow combinations; seeded larger cases include alpha-only changes,
  unordered/duplicate keys, hash/tail changes, clamped counts and `UINT32_MAX`.
- H605's owning/deferred fixture is reused: 84 batches, 2,343 entries and 2,828
  draws. Candidate and reference prepared outputs are compared after every
  scratch sample. Caller-destruction and injected-work-failure controls pass.
- Fixed O0,P0,P1,O1 processes all pass original source/output pixels, query
  transcript, seven source boundaries, 670 wrapper lifetimes and normal-state
  restoration. This validates the capture observer, **not candidate rendering**;
  the candidate executes only on scratch after the original oracle passes.
- Both arms exclude per-entry clocks. Two warmups, sixteen alternating pairs
  and 64 whole-batch repetitions match the predeclared contract. P1 reverses
  first-arm and mode order. No replacement or additional runs occur.

## Local CPU contrast

| Process | Reference, ns/entry | Candidate, ns/entry | Reference-first reduction | Candidate-first reduction |
|---|---:|---:|---:|---:|
| P0 | 548.11–553.32 | 490.83–495.75 | 52.36–61.93 | 52.90–62.49 |
| P1 | 551.59–557.07 | 493.45–498.47 | 53.99–63.62 | 53.12–62.68 |

All four reduction lower bounds exceed aggregate sham 5.52ns/entry. Endpoint
allowances and original-population weighting are the same as H605; these are
not statistical confidence intervals. The reduction is roughly 9.5–11.5% of
this warmed preparation loop, or 0.123–0.149ms across the captured population.
Neither value is a live frame or FPS estimate. H605 clock removal and H606
shortcut reductions are not a measured combined production treatment.

Independent verification reproduces 16,128 raw rows, order/count/weighting and
contrast arithmetic without importing the analyzer. Eleven malformed-trace
controls pass. Image and source hashes, both native architectures and four
original effect controls are verified. Optimized code retains the 64-repeat
loop, both reference/candidate calls, the prefix shortcut and original fallback;
there are no per-entry clock/counter calls in this scratch comparison.

## Decision

Keep the candidate as a measured private implementation. No product change,
runtime switch, Wine staging, promotion or commit is made. This removes only
about one tenth of the local preparation loop, not its remaining state/layout
work and not H603's much larger Replay CPU outside the queue envelope.

The next gate is production-call-site integration with focused native tests,
candidate-path GPU exact verification and a bounded default-live CPU transfer
comparison. Keep app pacing and Encode nonregression separate. If that gate
does not establish transfer, do not extend FPS repetitions or claim a large
single-thread win from this local result. Global largest CPU owner and a live
removable budget remain open.
