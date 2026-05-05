# Verification Design

---

## 1. Approach

dxmt9 uses **TLA+** with the **TLC model checker** for formal verification.
TLA+ is a specification language for concurrent and distributed systems.
TLC exhaustively explores all reachable states of a finite model and
checks that every invariant holds and every liveness property is satisfied.

The verification layer sits alongside — not above — the English specs. Each
TLA+ module is the formal counterpart of a section in `backend/design.md`
or `d3d9/queries/design.md`. They describe the same system; TLA+ is more
precise and machine-checkable.

| English spec | Formal / deterministic evidence | C++ implementation |
|---|---|---|
| `archicture/design.md` §6 / `backend/design.md` §2 | `tla/CommandQueue.tla` | `src/dxmt9/dxmt9_queue.*`, `src/dxmt9/dxmt9_command_queue.*` |
| `archicture/design.md` §6 / `backend/design.md` §2.2 | `tla/QueueLifecycleRefinement.tla` | `QueueLifecycleController` in `src/dxmt9/dxmt9_queue.*` |
| `backend/design.md` §3 | `tla/EncoderLifecycle.tla` | `src/dxmt9/dxmt9_draw_encoder.*`, blit/readback encoder helpers |
| `backend/design.md` §5 / §7 | `tla/ResourceLifetime.tla` | `src/dxmt9/dxmt9_resource_pool.*` |
| `backend/design.md` §8 / §8.1 | `tla/PresentFrameLatency.tla` | `src/dxmt9/dxmt9_command_queue.*`, `src/dxmt9/dxmt9_presenter.*` |
| `d3d9/queries/design.md` §2-3 | `tla/QuerySeqId.tla` | `src/d3d9/core.cpp` |
| `backend/design.md` §2 and `tests/design.md` §0.1 | queue observer / fake backend tests | `QueueLifecycleController`, chunk importer replay path |

---

## 2. Module Structure

```
specs/verification/
├── requirements.md        What must be formally verified and why
├── design.md              This file: approach, binding, how to run
└── tla/
    ├── CommandQueue.tla   3-thread ring buffer
    ├── CommandQueue.cfg   TLC model parameters
    ├── QueueLifecycleRefinement.tla  QueueLifecycleController staging/refinement
    ├── QueueLifecycleRefinement.cfg
    ├── PresentFrameLatency.tla  Present token and frame-latency gating
    ├── PresentFrameLatency.cfg
    ├── ResourceLifetime.tla  Deferred GPU resource destruction
    ├── ResourceLifetime.cfg
    ├── EncoderLifecycle.tla  MTLCommandEncoder state machine
    ├── EncoderLifecycle.cfg
    ├── QuerySeqId.tla     D3D9 query seq-ID fence
    └── QuerySeqId.cfg
```

---

## 3. TLA+ Module Anatomy

Each module follows this structure:

```
CONSTANTS     — model parameters (ring size, limits, bounds)
VARIABLES     — the full system state
Init          — initial state predicate
Actions       — one named action per observable state transition
Next          — disjunction of all actions
Spec          — Init ∧ □[Next]_vars ∧ fairness conditions
TypeOK        — type invariant (always the first invariant checked)
Safety        — conjunction of all safety invariants
Liveness      — temporal properties (leads-to, eventually)
```

---

## 4. Binding to C++ Implementation

The TLA+ spec is an **abstraction** of the C++ implementation. The
relationship is **refinement**: every concrete execution must be a
behavior that the abstract spec permits.

### 4.1 Variable mapping

Each TLA+ variable corresponds to a concrete C++ field. The mapping must
be documented as a comment block at the top of the C++ class:

```cpp
// TLA+: CommandQueue module
// state[s]          → chunks_[s].state     (enum class SlotState)
// chunkSeqId[s]     → chunks_[s].seq_id    (uint64_t)
// currentSeqId      → current_seq_id_      (std::atomic<uint64_t>, Wine thread)
// completedSeqId    → completed_seq_id_    (std::atomic<uint64_t>, finish thread)
// writeIdx          → write_idx_           (size_t, Wine thread only)
// encodeIdx         → encode_idx_          (size_t, encode thread only)
class CommandQueue { ... };
```

### 4.2 Action mapping

Each TLA+ action corresponds to a C++ method. The method must carry a
comment referencing its abstract action:

```cpp
// TLA+: WineCommit — requires InflightCount < MAX_INFLIGHT
void CommandQueue::commitChunk() { ... }

// TLA+: EncodeBegin
void EncodeThread::dequeueChunk() { ... }

// TLA+: FinishComplete — advances completedSeqId
void FinishThread::onGPUCompletion(uint64_t seq_id) { ... }
```

### 4.3 Invariant assertions

Every TLA+ safety invariant must appear as a `DXMT_ASSERT` in the
C++ implementation, executed on every relevant code path in debug builds:

```cpp
// SeqIdSafety: completedSeqId < currentSeqId
DXMT_ASSERT(completed_seq_id_.load(std::memory_order_relaxed)
            < current_seq_id_.load(std::memory_order_relaxed));

// BoundedInflight: InflightCount <= MAX_INFLIGHT
DXMT_ASSERT(inflight_count_.load() <= kMaxInflight);

// RingSafety: state[writeIdx] ∈ {Free, Writing}
DXMT_ASSERT(chunks_[write_idx_].state == SlotState::Free ||
            chunks_[write_idx_].state == SlotState::Writing);

// NoUseAfterFree (ResourceLifetime)
DXMT_ASSERT(res->state != ResourceState::Freed ||
            completed_seq_id_.load() >= res->last_used_seq_id);
```

---

## 5. Model Parameters

TLC requires finite models. The `.cfg` files use small parameters that
exercise all structural behaviors while keeping the state space tractable.

| Module | TLC parameter | TLC value | Production |
|---|---|---|---|
| CommandQueue | `RING_SIZE` | 4 | 32 |
| CommandQueue | `MAX_INFLIGHT` | 2 | 3 |
| CommandQueue | `MAX_SEQID` | 6 | unbounded |
| QueueLifecycleRefinement | `RING_SIZE` | 3 | 32 |
| QueueLifecycleRefinement | `MAX_INFLIGHT` | 2 | `kMaxQueuedChunks` back-pressure limit |
| QueueLifecycleRefinement | `MAX_SEQID` | 4 | unbounded |
| PresentFrameLatency | `MAX_SEQID` | 5 | unbounded |
| PresentFrameLatency | `MAX_FRAME_LATENCY` | 2 | `SetMaximumFrameLatency()` / backend latency cap |
| ResourceLifetime | `Resources` | `{r1,r2,r3}` | dynamic |
| ResourceLifetime | `MAX_SEQID` | 5 | unbounded |
| EncoderLifecycle | `RenderTargets` | `{rt1,rt2}` | dynamic |
| EncoderLifecycle | `MAX_OPS` | 8 | unbounded |
| QuerySeqId | `MAX_QUERIES` | 3 | dynamic |
| QuerySeqId | `MAX_SEQID` | 5 | unbounded |

**Why small parameters are sufficient:** The structural behaviors (ring
wrap-around, back-pressure blocking, hazard-triggered splits, flush-commit
sequencing) all occur within these bounds. Increasing parameters does not
reveal new behaviors — it only generates more repetitions of the same
patterns.

---

## 6. Refinement Spec (Advanced)

For stronger assurance, a concrete-level TLA+ module can be written that
models the C++ fields directly and proves it **refines** the abstract
module using TLA+'s `INSTANCE` operator:

```tla
---- MODULE CommandQueueImpl ----
VARIABLES chunks, current_seq_id, completed_seq_id, write_idx, encode_idx

CQ == INSTANCE CommandQueue WITH
  state          <- [s \in Slots |-> chunks[s].state],
  chunkSeqId     <- [s \in Slots |-> chunks[s].seq_id],
  currentSeqId   <- current_seq_id,
  completedSeqId <- completed_seq_id,
  writeIdx       <- write_idx,
  encodeIdx      <- encode_idx

\* Refinement obligation: Impl behaviors ⊆ Abstract behaviors
THEOREM ImplSpec => CQ!Spec
====
```

TLC checks this by verifying `CQ!Spec` is satisfied whenever `ImplSpec`
steps occur. This is the strongest form of binding short of a full proof
in a proof assistant (Lean 4, Coq).

Concrete refinement specs should be added once the implementation state is
stable enough to name. `QueueLifecycleRefinement.tla` is the first such model:
it binds the queue's abstract lifecycle to `QueueLifecycleController` staging
fields (`readySlots`, `pendingCompletion`, `completedSeqQueue`, inline
completion, empty commits, and `waitForSequence`).

---

## 7. How to Run TLC

### Run all specs

```sh
bash scripts/verify_tla.sh
```

This is the current verification path in the repository. Remote CI is not
configured yet; when it is added, it should invoke the same script.

### Expected output

```
Model checking completed. No error has been found.
  Estimates of the probability that TLC did not check all reachable states
  because two distinct states had the same fingerprint:
  calculated (optimistic):  ...
```

Any `Invariant ... is violated` or `Temporal properties were violated`
output is a spec defect and must be resolved before implementation proceeds.

The checked-in `.cfg` files set `CHECK_DEADLOCK FALSE` so TLC focuses on the
bounded-model invariants and liveness properties that matter for these specs.
