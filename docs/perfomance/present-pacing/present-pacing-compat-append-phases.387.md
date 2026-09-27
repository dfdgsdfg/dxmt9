---
domain: present-pacing
workload: 3DMark05 GT2 fixed Render Tape compatibility route
title: "H387: Fixed-input compatibility append is dominated by uniform work"
type: evidence
status: diagnostic-attribution-stalker-transfer-open
updated: 2026-09-27
source: traces/compat-append-h387/{run.py,analyze.py,campaign.json,summary.json}; traces/compat-append-h387/{verify,verify_split,off0,parent0,split0,split1,parent1,off1}/result.json
related: present-pacing-mark-ticket-coverage.386.md
---

# H387: Separate compatibility append owners on identical input

## Scope and method

H386 identified compatible Slot construction as an unobserved queue-mutex
holder. This follow-up measures the same production compatibility route, but
uses the preserved, admitted **GT2 r405 Tape**, not a STALKER late-frame Tape.
No equivalent admitted STALKER input is available for this experiment. Do not
transfer the numerical proportions to STALKER or claim its contention fixed.

Use the existing CPU Render Tape harness, x86_64 release under Rosetta, fresh
device/resources each iteration, capture-source boundaries and source-plan
SHA-256 `cc6b8312ea6abfc0a571741faca56aec729d8dac57e5585a3589bce29aac34f6`.
Completion is controlled and immediate, with no GPU submission, Wine producer
or asynchronous pacing. Provider, validator, loaded images and all input
component fingerprints are identical across blocks.

`DXMT9_COMPAT_APPEND_DIAGNOSTICS` modes are OFF (0), enclosing scopes (1),
and split phases (2). The sequence is 0/1/2/2/1/0, each two warmups and sixteen
retained samples: **96 measured iterations and 12 warmups**. Full CPU semantic
verification runs two iterations before (OFF) and after (split); the existing
verification comparator accepts their equivalence. Diagnostic receipts remain
`accepted=false`, `diagnostic=true`, `execution_valid=true`; they are not
policy-ranking evidence or GPU exact certification.

All measured iterations have 1,866 compatibility draws, zero Direct draws,
198 ordinary batches/reserve calls, 1,866 append calls and 646 new state
records in split mode. The remaining 1,220 appends extend existing DrawRuns.
Source submission/completion/reclaim counts are 185 each, with successful
reset and unchanged counts in every iteration. The analyzer rejects work-count
drift and checks that disjoint append phases fit inside the append envelope.

OFF means **this new observer OFF**, not every pre-existing perf timer OFF.
The CPU harness keeps existing runtime perf counters and controller diagnostics.
All builds/tests finish before the measurement campaign; no concurrent wild
run, sampler or build is launched.

## Results

Elapsed milliseconds per complete captured input, arithmetic means across
each block's sixteen retained iterations. These scopes are not native CPU
service and include instrumentation. Indented rows are inside append, not
additional work to add to it.

| Scope | Split block 0 | Split block 1 |
|---|---:|---:|
| Batch reserve/capacity/provision | 0.068 | 0.067 |
| Append envelope | 3.023 | 3.033 |
| ↳ Uniform lookup | **1.088** | **1.098** |
| ↳ Uniform append/intern/storage | **1.109** | **1.112** |
| ↳ State/PSO construction and state SoA append | 0.245 | 0.245 |
| ↳ Final DrawParam/payload/header/record writes | 0.171 | 0.170 |
| ↳ Preflight checks | 0.135 | 0.134 |
| ↳ Local reserve branch | 0.071 | 0.070 |

Uniform lookup plus append is approximately **73% of the observed append
envelope** in both blocks. This is the strongest local owner finding, not a
claim that 73% is removable. Final byte/vector emission is relatively small.

### Observer cost

Parent-only append means are **2.505/2.501ms**, versus **3.023/3.033ms** with
split clocks: approximately **0.518/0.532ms** extra observation envelope.
Do not subtract that from individual phases using an invented distribution.

| Mode | Replay native process-CPU median, forward | Reverse |
|---|---:|---:|
| New observer OFF | 16.052ms | 16.009ms |
| Parent only | 16.327ms | 16.208ms |
| Split | 16.768ms | 16.803ms |

The split mode adds roughly 0.72–0.79ms relative to the corresponding OFF
block's median replay CPU. Whole-iteration CPU includes resource setup tails
and varies more, so it is not used to assign the append phases. No FPS claim.

## Exact ownership boundaries

`src/dxmt9/dxmt9_backend_types.hpp`:

- `reserveDrawRunBatchStorage`: checks, vector capacity reservation and lookup
  provisioning; includes allocation/relocation when needed. It is **not an
  allocator-only timer** and does not capture every possible internal allocation.
- `appendDrawRun` / `appendCompatibleDrawRunEntryUnchecked`: preflight,
  `findDrawUniformPayload`, local reserve branch, `appendDrawUniformPayload`,
  then state and final command/payload construction.
- Local reserve is predominantly the `alreadyReserved=true` branch in this
  production batch path, not proof of 0.07ms of allocations.
- State includes `makeDrawPsoSubview` and `appendDrawState`. The latter writes
  hot/shader/debug SoA arrays; the separate final SoA row covers draw parameters,
  payload bytes and command records. These categories deliberately do not mean
  "all SoA bytes" versus "all computation".
- The by-value `CanonicalDrawState` copy occurs before entering `appendDrawRun`
  and is outside its envelope. Batch also includes snapshots, resource marking,
  locking, dispatch, argument materialization and possible publication. Its
  residual cannot be assigned entirely to that copy.
- Uniform append includes component lookup/dedup and storage writes; further
  splitting is required before deciding that copying, hashing or dedup dominates.

The observer uses bounded thread-local tokens and aggregate counters. Only an
ordinary compatibility-batch root admits child observations. No command order,
reserve policy, resource ownership, lock boundary or emitted payload changes.
No per-phase file output or native CPU syscall is added.

## Verification and next decision

Native arm64 and x86_64 observer/routing tests pass. OFF/parent/split mode tests
preserve the same command fixtures; non-rooted calls produce no diagnostic
counts. Unix canonical build/install-name audit and TLA regression (218.79s)
pass. CPU verification comparator accepts OFF versus split on the same Tape.
GPU contents and real asynchronous contention are outside this experiment.

The next local candidate is uniform identity lookup/interning and reusable
component selection, not general allocator replacement or moving final memcpy
alone. Before a structural off-lock design, reproduce this split on a retained
STALKER-compatible batch input, or validate a low-perturbation bounded live
observer there. Any reused handle must remain Slot/generation-qualified.
No performance optimization or runtime policy promotion is implemented here.
