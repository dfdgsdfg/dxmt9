---
domain: present-pacing
workload: STALKER CoP H481 original Normal source, x86 release
title: "Planned replay dominates the observed provider call, not raw setup or settlement"
type: evidence
status: bounded-provider-partition-not-exclusive-renderer-cost
updated: 2026-10-11
source: traces/stkcop-provider-phases-h489/{contract.md,run.py,build.log,build-receipt.json,probe-build.log,probe-build-link-failed.log,tests.log,native-build.log,native-tests.log,trace-r1.txt,trace-r2.txt,off.txt,off.stderr}; traces/stkcop-provider-phases-h489/{trace-r1,trace-r2}/{supply.csv,caller.json,analysis.json}
related: present-pacing-prepared-replay-cpu.488.md; present-pacing-root-game.477.md; present-pacing-default-thread-identity.462.md
---

# Narrow the remaining replay cost before changing ownership

H488's observed replay call is now split at four cold `runProviderReplay`
boundaries. In two fresh executions of the same original Normal input,
preparation costs about 1.32 ms CPU and successful settlement 1.37–1.47 ms;
the planned-replay call costs 51.14–51.63 ms. Raw setup and settlement are
therefore small parts of this observed provider total. Their removal would
not explain most of it even if that removal were valid; no such removal is
implemented or qualified here.

Planned replay still includes the per-record V4 runtime/provider hooks. This
is not exclusive production renderer CPU, and it does not establish the
largest live default-game single-thread owner or an FPS/removable-cost bound.

## Scope and strict reduction

`src/d3d9/device_c_chunk_replay.cpp`, `runProviderReplay`, records
`ProviderReplayPhase` only when its V4 observation is present and the existing
CPU supply sink is enabled. There is no new public signature, per-record clock,
resource reference, drain, policy selector or lifecycle transition. Formal
refinement is not newly required: this edit observes existing boundaries and
does not change rendering/state/ownership semantics.

| Endpoints | Scope |
|---|---|
| 0 → 1 | Raw construction/copy, wrappers, capture bindings, prior drain, ledger publication, V4 admission and dispatch preparation |
| 1 → 2 | Selected replay function, including ordinary planning/Slot work and embedded V4 qualification |
| 2 → 3 | Successful post-replay publication, observer use accounting, ledger ACK and V4 settlement/snapshot/coverage |
| Outer call minus 0 → 3 | Entry validation/import and scope/raw resource destruction, plus boundary observer work |

The key is observer context plus original event ordinal. It is not a queue
sequence or H488's plan-raw index. The reducer joins each phase set to exactly
one enclosing same-thread raw call by its CPU/wall endpoints, rejects duplicate,
missing, cross-thread, non-monotone and escaped sets, and retains both identities.
It checks diagnostic scope correspondence, not independent wire provenance.
The original V4 oracle remains the identity/coverage authority.

Use `scripts/tools/analyze_prepared_replay_cpu.py TRACE --expected-raw-count 92
--require-provider`. Old H488 traces remain readable without that option, with
provider partition unavailable rather than invented zero costs. Failed calls
do not have a complete successful phase set and cannot be priced by this mode.

## Original-input CPU results

All numbers below are native caller CPU milliseconds over 92 original raws,
not per-Present game measurements. The first four rows partition the fifth.

| Scope | trace-r1 | trace-r2 |
|---|---:|---:|
| Provider preparation | 1.317 | 1.319 |
| Planned replay including V4 hooks | 51.140 | 51.629 |
| Successful settlement | 1.470 | 1.374 |
| Call boundary residual | 0.274 | 0.289 |
| Observed replay call total | 54.201 | 54.611 |
| Enclosing raw envelope | 62.017 | 62.277 |
| Original interval outside raws/readback | 60.915 | 61.100 |
| Readback/oracle | 48.867 | 49.748 |

The planned call owns roughly 94% of this observed replay-call CPU. That
percentage is scope attribution, not a removable fraction. The event-loop
residual still contains mutations, definitions, controls and event-close
qualification. H488's cumulative-snapshot finding is real code structure,
but the new evidence does not support calling raw settlement the dominant
cost. Event-close snapshots lie outside this call and remain unpriced.

The first interval is 917.930 ms elapsed versus 252.385 ms in the second;
caller CPU is 171.799/173.125 ms. No causal attribution is assigned to this
wall-time difference. These fresh-cache diagnostic runs are neither an A/B
optimization experiment nor a steady-state distribution. H488-to-H489 CPU
differences are not a calibrated observer tax and are not subtracted.

## Verification and provenance

All three fresh processes (two traced, one OFF) reproduce the original source
and output SHA-256
`f99e419861d3c4127e96d8ab56dca62d56421919a5962084ae4ecd95ee42ce5a`,
3,686,400 BGRX bytes each. Normal policy, 16 restored surfaces, 12 aliases,
779/779 wrappers and original source cuts mapping to local sequences 3/4/5
are preserved. Both traces have all 92 provider phase sets and pass the strict
caller and original-source reducers. All-resource content authority remains
unknown; exact final/source readback does not fill that gap.

The Metal skill's environment check passes (Xcode 27, Apple M1 Metal 4).
Readback is the oracle; no gputrace, external UI action or Wine staging occurs.
Full x86 release compilation precedes the runs, and the original H486 runner
is rebuilt against the new static provider. Its first link omitted the direct
winemetal dependency and failed before execution; that log is retained. The
corrected link uses the same explicit provider dependency as the Meson native
provider test, recorded in `build-receipt.json`. Therefore this is not a
byte-identical H488 executable comparison. OFF dyld paths confirm both actual
build-tree runtime images. No build overlaps any of the three runs.

Six focused x86 tests pass, including provider V4, provider observation V4,
attachment restore, the 12-case prepared CPU reducer, 42-case supply reducer
and install-name audit.
The full native build and five corresponding native tests also pass (the
install-name audit is x86-specific here). Both measured x86 runtime hashes and
the runner hash still match the pre-run receipt after verification.

## Consequence for the full objective

Do not optimize raw retention/import or settlement merely because they look
structurally redundant: this bounded call attributes most CPU elsewhere.
The next useful distinction is ordinary replay/Slot work versus the V4
record-level authority hooks inside planned replay, without disabling the
identity oracle or adding native-clock calls to every draw.

The historical owner evidence was rechecked as well. H462 has a same-run
guest/native join identifying its unnamed leader as an X-Ray helper; H477
does not contain the same join for its own leader. Repeated handle values
cannot transfer that identity. Neither the H477 all-call-clock rejection nor
the global Primary attribution gap is repaired by this captured-input result.
The original largest-owner and correctness-qualified removal objective remains
active, not replaced by successful harness accounting.
