---
type: "Spec Requirements"
title: "Producer Concurrency Requirements"
description: "Bridge entry synchronicity classification and thread-ownership contracts for state shared among the producer, replay worker, encode thread, and completion path."
tags: [specs, backend, producer-concurrency, requirements]
---

# Producer Concurrency Requirements

This topic owns two contracts that previous performance work applied
case-by-case and this spec makes uniform: **why a PE→unix bridge entry is
allowed to block the game thread**, and **which thread owns each piece of
queue/pool state**. It does not change wire semantics, D3D9 command order, or
Presenter ownership; it constrains how synchronization for existing semantics
is expressed and evidenced.

Motivating evidence: the producer measurably lost ~1.0 ms/present to a coarse
shared mutex whose protection largely duplicated natural single-writer
ownership (`docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.{28,29}.md`),
and each recovery (offload, T2a', scan-hint) had to re-derive its safety
argument from scratch because no standing contract existed
(`docs/superpowers/specs/2026-08-20-producer-queue-concurrency-design.md`).

## Bridge entry synchronicity classification

- **R-BACK-43.1** Every `dxmt9c_*` bridge entry point must carry exactly one
  synchronicity class from the closed taxonomy below, recorded in the
  classification table in `spec.md`. A new entry point must be classified in
  the same change that adds it.

  | Class | Meaning |
  |---|---|
  | `app-return-value` | Returns data the D3D9 contract makes immediately app-visible. Inherently synchronous. |
  | `visibility-wait` | Must observe prior GPU/replay effects before returning (e.g. Wine draw-then-lock visibility). Synchronous, and the wait target must be stated. |
  | `ordering-fence` | Must serialize against deferred replay before proceeding; the drain form (global / resource-scoped / bypassable) must be stated. |
  | `state-mutation-ack` | Mutates unix state where the producer requires the resulting HRESULT. |
  | `record-only` | No app-visible result and no ordering obligation beyond chunk order; synchronous today only by calling convention. |

- **R-BACK-43.2** A `record-only` entry must not acquire `CommandQueue::mutex_`
  and must not wait on any cross-thread condition. Migrating an entry INTO
  `record-only` (removing a wait it has today) is a semantic change and
  requires the R-BACK-43.6 evidence ladder.

- **R-BACK-43.3** An entry's classification is a ceiling, not a description:
  it states the strongest blocking the entry is PERMITTED to perform. Adding
  blocking beyond an entry's class (a new drain, a new shared-mutex acquire on
  a classified-weaker entry) is a contract violation and must either change
  the classification (with justification) or be rejected in review.

## Thread-ownership contract

- **R-BACK-43.4** State reachable from more than one of {producer, replay
  worker, encode thread, completion path} must carry exactly one ownership
  class from the closed taxonomy below, declared at the owning
  struct/field/module in a comment adjacent to the declaration:

  | Class | Meaning | Synchronization |
  |---|---|---|
  | `producer-owned` | Written and read only on the game thread. | None; debug thread-affinity assert. |
  | `worker-owned` | Written and read only on the owning worker thread. | None; debug thread-affinity assert. |
  | `owner-published` | Written by one owner; read by others only through an explicit publication point (snapshot copy, release-store, publish-under-lock). | The publication mechanism must be named. |
  | `arena-protected` | Serialized by a component's own internal lock (e.g. `HandleArena`'s mutex). | The component's lock; never assumed covered by `CommandQueue::mutex_`. |
  | `queue-shared` | Genuinely multi-writer/multi-reader under `CommandQueue::mutex_`. | The queue mutex; each new member of this class needs a stated reason it cannot be one of the classes above. |
  | `immutable-after-init` | Written once before any concurrent access. | None; the initialization point must be named. |

- **R-BACK-43.5** `producer-owned` and `worker-owned` state must be guarded by
  a debug-only thread-affinity assertion that compiles out of release builds
  (the `recorderLockRequired_`/`assertRecorderThreadConfined` shape from
  `b96fdbda` is the reference implementation). The assertion helper must be
  shared, not re-implemented per site.

- **R-BACK-43.6** Reclassifying state OUT of `queue-shared` (lock removal,
  lock narrowing, atomic conversion) requires, before the production change
  ships: a bounded model (or exhaustive checker) covering the new
  interleavings **including a counterexample configuration that demonstrates
  the guarded failure when the new premise is removed**, shared pure
  predicates binding the model to the code, and a native spec exercising the
  predicate boundary cases. `ProducerMarkReclaim.tla` + its two Buggy configs
  (`cfcfdac1`, `ced79f73`) are the reference. The C++ memory-order obligation
  (release/acquire pairing, torn reads) additionally remains open until a
  deterministic interleaving harness covers it (R-VERIF-7.3 direction) —
  a model alone does not discharge it.

- **R-BACK-43.7** A tuning-constant promotion that changes the cardinality a
  data structure operates over (chunk record caps, retention epochs, batch
  sizes) must record, in the promoting change, a review of the structures
  whose complexity depends on that constant. (Two measured recurrences:
  the 256-record cadence promotion exposing O(n²) handle dedup at 277
  handles/call, and warm epochs enlarging the retainer's linear scan —
  `state-churn-encode-append-decomposition.{26,28}.md`.)
