---
domain: present-pacing
workload: STALKER CoP H502/r5 late Normal input, x86 release
title: "Same-production-query replay reproduces the late input and exposes bounded CPU scopes"
type: evidence
status: bounded-original-readback-match-not-default-cost-qualified
updated: 2026-10-13
source: traces/stkcop-production-query-h506/{source_probe.cpp,run.py,summary.json,candidate-build-r2.json,run-r1.log,run-r2.log,run-r3.log,run-r4.log,run-negmetal.log,run-negsync.log,native-final.txt,tla.txt,build-final.txt}; traces/stkcop-production-query-h506/{run-r3,run-r4}/{supply.csv,analysis.json,caller.json}; traces/stkcop-closed-query-history-h505/build-production-query.txt; traces/stkcop-scene-request-h502/r5/receipt.json
related: present-pacing-closed-query-history.505.md; present-pacing-provider-phases.489.md; present-pacing-validation-reuse.492.md; present-pacing-root-game.477.md; ../../../specs/experiments/gap.md
---

# The same late input now reaches the original query and pixel oracles

H505's backend mismatch is closed for a private, explicitly declared
same-production-query diagnostic. Four fresh x86/Rosetta executions of the
unchanged H502/r5 Normal bundle reproduce all seven original source cuts,
nine query Issues, five polls and four defined results. Both source and output
readbacks match the original 3,686,400 bytes exactly. No recorded result is
injected, source boundary changed, or hardware occlusion enabled.

This is progress toward default-tail attribution, not completion of that goal.
The hardware-query oracle, every resource's initial content authority, default
game-thread ownership and a correctness-qualified removal budget remain distinct.

## Cold contract and authority boundary

R-HARN-REPLAY-9.25 adds `RenderTapeProductionQueryDiagnosticV1` to private V4
Normal preparation. It explicitly declares primitive-count occlusion with
asynchronous fences, binds the immutable source-plan digest, and records the
external capture-runtime receipt digest. The receipt is a **caller attestation**:
the provider does not parse or authenticate external JSON merely because its
digest is nonzero. The runner checks the frozen receipt, capture manifest,
supervisor snapshot and scrubbed DXMT environment; H505's image/request/source
joins remain the original-capture evidence. The frozen receipt SHA-256 is
`5b4d473fa9b6ae4021bd057271c28d916df328f42418fab3f36e35c03e5d6664`.

Preparation rejects foreign source digests, missing evidence, unknown versions,
unsupported profile bits, non-Normal use and non-exact publication mode. It
owns a value copy. Before setup, execution verifies the actual device has a
Metal backend, counter occlusion and asynchronous fences. Production Issue,
GetData, local completion and exact result/source/pixel comparisons are
unchanged. `productionCounterDiagnostic` is separate from `actualMetal`, and
`gpuResultOracle` stays false even on complete success. Public launchers and
the default provider still require hardware occlusion; no new runtime knob,
production query policy or Wine ABI is introduced.

Pure admission negatives and all eight local-backend Boolean combinations
bind the gates. Null-device tests grant no local authority. Actual hardware-
occlusion and synchronous-fence negative processes both reject before setup:
zero wrappers, zero Issue/poll execution, zero mapped sources and no pixels.
There is no new scheduling/lifetime transition; the existing full TLA suite is
regression coverage, not proof of the externally attested configuration.

## Original-input evidence

Two CPU-observer-OFF runs and two diagnostic trace runs each establish:

| Evidence | Every successful run |
|---|---:|
| Exact source mapping | 7 / 7 |
| Query Issue / recorded polls executed | 9 / 9; 5 / 5 |
| Readiness obligations / defined-result comparisons | 5 / 5; 4 / 4 |
| Original/replayed occlusion values | 160 / 160 for all four |
| Wrappers created / released | 670 / 670 |
| Restored surfaces / retained aliases activated | 16 / 12 |
| Source and output oracle | bit-exact, 3,686,400 bytes each |
| Hardware query-result qualification | false |

The original pixel hash remains
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
Normal construction/arm/finish stays unchanged. The binary, frozen runtime
images and input hashes are checked before each run; r1 dyld output confirms
the actual private images. All five shared Wine images remain unchanged.

## First CPU decomposition of this input

These are **observer-inclusive native CPU milliseconds over one captured
interval**, not live-game per-frame prices, steady-state performance, or a
removable amount. Both runs use fresh devices with shader archive/prewarm off.
Prefetch therefore includes cold-cache work; no claim is made that its entire
cost is compilation. Background host activity was present. No builds or TLA
checks overlap these two trace runs, but this is not an isolated timing trial.

| Scope | r3 | r4 |
|---|---:|---:|
| Encode, seven original sources total | 68.241 | 64.983 |
| Encode prefetch (inside preceding row) | 31.747 | 30.547 |
| FrameGraph planning/setup | 2.015 | 1.910 |
| Encode command body | 30.276 | 28.906 |
| Planned replay, including V4 qualification | 75.170 | 65.849 |
| Full-Slot validation (inside planned replay) | 9.763 | 9.548 |
| Provider preparation | 2.111 | 2.476 |
| Successful provider settlement | 5.895 | 5.405 |

Source 5 owns 591 commands and about 20.6 ms body CPU in r3; source 1 owns
228 commands and about 7.9 ms. Both strict reducers accept all 259 original
raws. The trace is closed with no overflow/incomplete scopes, and setup sources
are excluded from Encode totals. Repeated Slot validation performs 646 accepted
calls and 235,150 successful command visits in both runs. Stored parameter
extent sums are not actual parameter visits. Do not sum nested or overlapping
Replay/Encode scopes into a critical-path estimate.

This identifies a larger repeated-validation envelope than H491's earlier,
non-tail H481 input. It does **not** resurrect H492's rejected last-handle cache
or establish a 9.5 ms removal budget. Planned replay still includes V4 record
binding/qualification, and the large prefetch scope is not a default warm price.
The next useful comparison must separate those harness/cold-cache costs while
retaining the same original identity and output oracles, before choosing a
product-path removal. H477's mixed Primary and unmatched native-thread CPU
remain unranked; the offline Encode number does not supersede them.

## Validation

Full canonical x86 Unix build and six focused tests pass, including provider
V4, execution V4, query identity, capture and install-name audit. All 237 TLA
configurations meet their expected outcomes. The first private runner build
reported two multicharacter newline warnings; a corrected, separately archived
candidate-r2 build has no such warnings and supplies every execution here.
No PE rebuild is required by this cold provider-only API change.

H505's unrelated fixed textured-UP digest failure is not waived or cleared by
these six tests or this original-input oracle. No commit, default promotion,
FPS claim or global largest-owner/removal verdict follows from H506.
