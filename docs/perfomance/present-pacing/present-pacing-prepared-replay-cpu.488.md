---
domain: present-pacing
workload: STALKER CoP H481 original Normal source, x86 release
title: "Prepared caller CPU separates replay from its harness and readback costs"
type: evidence
status: bounded-caller-attribution-not-default-worker-cost
updated: 2026-10-11
source: traces/stkcop-replay-cpu-h488/{commands.md,images-sha256.txt,build.log,probe-build.log,final-tests.log,trace-r1.txt,trace-r2.txt,off.txt,off.stderr}; traces/stkcop-replay-cpu-h488/{trace-r1,trace-r2}/{supply.csv,caller.json,analysis.json}; traces/stkcop-normal-finish-h481/tape/frame-10699344711800-1/manifest.json
related: present-pacing-draw-loop-scratch.487.md; present-pacing-normal-source-cost.486.md; present-pacing-root-game.477.md
---

# Native caller CPU is now distinct from replay elapsed time

The H486/H487 replay path does not pass through `ReplayOffloadWorker`: the
prepared provider imports, resolves and replays original raws synchronously on
the harness caller. Consequently its absence of `ReplayBegin/End` is expected,
not evidence that Replay has zero cost. New cold V4 scopes measure this thread
using the existing native-ID/libproc CPU clock. No Rosetta POSIX-clock scaling,
per-record observer callback or per-draw clock is introduced.

Two fresh executions of the same original Normal input put the 92 observed
renderer replay calls at 52.094/52.089 ms caller CPU. Their enclosing raw
import/resolve/replay scopes are 59.715/59.787 ms. But original interval work
outside those raws is another 58.624/58.778 ms, and the readback/oracle costs
48.623/50.838 ms. These are not all production Replay work. In particular the
V4 identity/qualification callbacks still execute, and the outer loop owns
original mutations, definitions, controls and harness bookkeeping.

This closes the missing caller-clock observation, **not** the largest default
game-thread owner or a removable budget. The data warns against treating the
entire replay-harness time as the application Replay stage.

## Exact scopes and identities

`PreparedReplayPhaseBegin/End` append to the existing CPU-supply vocabulary.
Only cold `replayGeneralPlan` V4 execution emits them. The owner is the local
V4 execution instance, and every paired scope must retain that owner and TID:

| Phase | Identity | Included work |
|---|---|---|
| Interval | Original prefix event index | Successful arm through original event-loop return; excludes pre-arm setup, final settlement verification and cleanup |
| Raw envelope | Immutable plan command-chunk index | Import, wrapper resolution, Present shape checks, binding of observation and observed replay call |
| Raw replay call | Same raw index | `replayPrevalidatedResolvedCommandChunkObservedV4`, including ordinary provider admission/ownership and V4 observation |
| Readback oracle | Original PresentComplete event index | Completion drain, original output/source readback, copies and oracle processing |

The raw indices are not queue sequence numbers. The unchanged V4 source map
independently joins captured sources 0/1/2 to local queue sequences 3/4/5;
the caller numbers cover all original raws, not just the large middle source.
Bootstrap overlays have no original-raw scopes. `analyze_prepared_replay_cpu.py`
requires one interval, one terminal readback, the complete expected dense raw
index set, one replay call enclosed by each raw envelope, non-overlapping
siblings and monotonic same-thread CPU/wall endpoints. Missing, duplicate,
out-of-range, cross-thread or clipped scopes reject rather than become zero.

There is no new rendering policy, cache, retain, drain, source cut or lifetime
transition, and therefore no new TLA transition. The existing bounded sink
stores events and writes outside the observed interval. Disabling the supply
trace prevents clock reads; no public hot-path signature or PE ABI changes.

## Same-input CPU evidence

Units are native caller CPU ms; the envelope contains the replay-call row,
and the interval contains the other rows. They must not be added again.

| Scope | trace-r1 | trace-r2 |
|---|---:|---:|
| Entire armed interval | 166.962 | 169.403 |
| Readback/oracle | 48.623 | 50.838 |
| Interval excluding readback | 118.339 | 118.565 |
| Raw envelope, 92 calls | 59.715 | 59.787 |
| Observed renderer replay, inside envelope | 52.094 | 52.089 |
| Envelope residual | 7.621 | 7.698 |
| Interval remainder outside envelope/readback | 58.624 | 58.778 |

The replay call is not exclusive `replayPlannedChunk` time. In
`src/d3d9/device_c_chunk_replay.cpp`, `runProviderReplay` first owns the raw,
retains wrappers, persists capture bindings, drains prior replay, publishes a
ledger identity and binds V4 qualification; after replay it settles those
obligations. Record-level V4 observation remains inside the call. The envelope
residual similarly includes more than memcpy or resolve alone. None of these
totals is a correctness-qualified amount that can simply be removed.

The large source's concurrent Encode CPU is 69.336/53.064 ms, with
32.049/32.997 ms cold prefetch and 35.466/18.253 ms command body. Its sibling
control/Present sources add small separate Encode scopes. The first run's
armed interval lasts 769.802 ms wall versus 249.559 ms in the second while
caller CPU is similar. No specific cause is assigned to that elapsed-time or
Encode-body variation; it is not a measured observer tax, pure GPU wait or
steady-state distribution. Do not add CPU from the caller and Encode threads
to infer frame latency, or rank a whole-input caller scope against only one
Encode source.

## Verification and remaining boundary

Both traced runs and a separate observer-OFF image-provenance run reproduce
the original source and output bytes exactly, preserve Normal policy, restore
16 surfaces, activate 12 aliases, balance 779/779 wrappers and retain all three
source cuts. Each readback is 3,686,400 BGRX bytes with SHA-256
`f99e419861d3c4127e96d8ab56dca62d56421919a5962084ae4ecd95ee42ce5a`.
Actual dyld paths and executable/provider hashes are retained. The H486 runner
was recompiled against the modified frontend-provider static archive; merely
replacing the runtime dylib would not exercise the new caller scopes.

The x86 release build and five focused Meson tests pass, including the
eight-case prepared-scope reducer suite, the 42-case supply schema/analysis
suite, V4 provider, attachment restore and install-name audit. The native build
and four focused tests also pass (both analyzers, V4 provider and attachment
restore). Final review additionally rejects a readback preceding an original
raw; the existing traces satisfy that terminal-order check. No build overlaps the three diagnostic
processes; no Wine installation or PE image is staged. The runtime still has
H487's baseline encoder, not its archived scratch-construction candidate.
The Metal skill's original-readback workflow is used; no gputrace or FPS claim
is made. All-resource first-read content authority remains unknown.

Next separate ordinary Replay execution from V4 capture/qualification and
original mutation service before assigning a default worker budget. The
global decision also still needs Primary's dxmt9 share and the unnamed large
native thread from H477. H477's rejected live all-call-clock method remains
closed; this diagnostic is not a replacement default-transfer proof.
