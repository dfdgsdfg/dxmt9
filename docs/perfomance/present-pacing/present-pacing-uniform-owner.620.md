---
domain: present-pacing
workload: STALKER H502/r5 original captured input
title: "H620 — full uniform construction is not every uniform settlement"
type: experiment
status: bounded-attribution
updated: 2026-10-16
source: traces/stkcop-uniform-owner-h620/{contract.md,derivation.json,source-verified.json,native-tests.json,extra-tests.json,detail-tests.json,build.json,runs.json,analysis.json,uniform-verification.json}
related: present-pacing-uniform-fixed.619.md; present-pacing-replay-cache.617.md; present-pacing-replay-cache-live.618.md; present-pacing-encode-census.613.md
---

# H620 — qualify the real uniform path before selecting another copy change

The original captured input performs 1,112 binding-agnostic cache uniform
settlements, but only 190 full payload constructions and 584 constants-only
refreshes. Another 338 settlements reuse the full payload. All 1,112 still
project fixed fields. Consequently, treating every uniform scope as a full
10,840-byte rebuild overstates that path's invocation count by 5.85 times.
These counts describe the captured input, including provider reconstruction;
they do not establish the late-live path mix.

Fixed projection's entire observed wall is only 0.187–0.189ms per capture.
This is not a removal measurement, but it supplies no basis for attributing
H619's +2.941ms broad-interval median contrast to the removed fixed copy.
H619's failed stable-saving screen remains closed, not re-run or rehabilitated.

## Mechanism and qualification

Four private objects derive from authenticated H617. Four broad provider
dispatch detail phases are replaced by full construction, constants refresh,
fixed projection and hash processing. H620 has distinct trace magic; record
size remains 640 bytes and 20 phases. Parent admission permits full/refresh/
fixed only beneath the cache's uniform scope, and hash only beneath full or
refresh. Other uses of the uniform builders do not enter these child scopes.

Source owners are `src/d3d9/core_draw.cpp`:
`makeDrawUniformPayloadFromState`,
`refreshDrawUniformPayloadShaderConstantsFromState`, and
`Device::cachedBaseDrawStateForSubmissionBatch`. Hash processing includes its
options, hashing, result metadata and component-hash settlement, not just the
hash arithmetic. Full construction's exclusive remainder includes initialization,
constant copies, matrix/material/light/texture/clip construction. The outer
uniform remainder includes caller-side assignment and bookkeeping; it is not
a pure memcpy measurement.

No rendering policy, candidate, product source or shared ABI changes. H619 is
not enabled. This observation-only change uses native tests rather than a new
rendering transition model. ARM64/x86_64 pass twelve native controls, four
extra controls, six detail-switch controls and 32 parser corruption cases.
Native code also checks the complete 400-case phase-parent table and inactive/
wrong-parent rejection. All four source edits reverse to authenticated H617;
canonical core object parity, changed archive members and install names pass.

Xcode 27.0, Metal 32023.921 and Apple M1 are verified. The fixed same-image
order O0,D0,D1,O1 completes without replacement. O disables detail while retaining
root observation; D enables detail. All four original GPU controls preserve
source/output digest
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`,
670 created/released wrappers, seven mapped sources, normal restoration and
the 9/5/4/5 query transcript. This is the production-counter query profile,
not an actual-Metal occlusion oracle. No Wine staging or live run occurs.

The independent uniform verifier imports no trace parser. It re-reads 1,040
raw rows, performs 4,010 hash checks and verifies:

- full + refresh calls equal hash-processing calls;
- fixed-projection calls equal uniform-settlement calls;
- full/refresh/fixed inclusive durations fit the parent;
- exclusive children plus parent remainder exactly reconstruct uniform wall;
- identities, completed roots, loss/fault counters and endpoint bounds hold.

## Observed costs

Milliseconds per whole captured input, not per live frame. Rows below are
disjoint exclusive regions; the full-construction row excludes its nested hash.
CPU bounds use each complete root's self-thread CPU endpoints and complementary
wall. They are neither confidence intervals nor overhead-corrected estimates.

| Uniform region | Calls | D0 wall | D0 CPU bounds | D1 wall | D1 CPU bounds |
|---|---:|---:|---:|---:|---:|
| Full construction, excluding hash | 190 | 0.232 | 0.030–0.232 | 0.232 | 0.037–0.232 |
| Constants refresh, excluding hash | 584 | 0.140 | 0.023–0.140 | 0.141 | 0.018–0.141 |
| Fixed projection | 1,112 | 0.187 | 0.024–0.187 | 0.189 | 0.019–0.189 |
| Hash processing | 774 | 0.436 | 0.110–0.436 | 0.441 | 0.109–0.441 |
| Outer uniform remainder | 1,112 | 0.345 | 0.071–0.345 | 0.332 | 0.053–0.332 |

Hash processing has the largest observed exclusive wall, but its CPU lower
bound does not exceed every competitor's upper bound. No unique CPU winner
inside uniform work is proved. Per-root union bounds give all uniform work
0.614–1.340ms / 0.574–1.334ms. Full construction including its hash is
0.188–0.541ms / 0.194–0.546ms. These union bounds are recomputed, not sums of
independently uncertain child lower bounds.

Complete-root CPU is O0 60.364–61.733ms, D0 63.266–64.668ms,
D1 61.216–62.590ms and O1 61.562–62.955ms. Finite detail-ON minus detail-OFF
contrasts span -1.740 to +4.304ms. This population cannot calibrate a universal
observer tax or show that detail is free. Source shapes and all detailed call
counts match D0/D1. All 260 roots per run are complete, without loss or faults.

## Consequence for the default-live objective

Do not select another whole-uniform copy optimization solely from object size.
The full build is a minority path here, and this finer experiment establishes
neither a large removable budget nor the default late-live path frequencies.
It also does not reject a distinct live opportunity: H618 already demonstrates
that cold and live cache rankings differ.

The higher-priority unresolved comparison is now the large Replay regions
versus Encode's draw loop in the **same** late episode. H613 and H618 are
separate populations. Primary's renderer work and other-thread exclusions
must also be qualified for any global maximum claim; H580's earlier bounds
are not simultaneous current evidence. A common-episode comparison must
retain whole-thread OFF endpoints, phase coverage, boundary uncertainty and
observer burden. Large observed regions still require a separate semantics-
preserving removal experiment before they can be called saved CPU.
