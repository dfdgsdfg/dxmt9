---- MODULE WireHandleGeneration ----
(*
 * dxmt9 Chunk Wire Handle Generation — TLA+ Specification
 *
 * Verifies the cross-side generation-stamp invariant on the PE → unix
 * command-chunk bridge:
 *
 *   PE-side recorder stamps the encoded generation of a core::Handle into
 *   `D9CCommandChunkWireHandleEntry.generation`. The unix-side chunk
 *   importer, on commit, resolves the wrapper pointer back to its current
 *   `core::Handle.value`, extracts the encoded generation, and rejects the
 *   chunk via `commitChunkFail("bad-handle-generation", ...)` when the
 *   stamped generation differs from the resolved one. The legacy NONE
 *   sentinel (0) is preserved for opaque-pointer recorder paths that
 *   cannot decode generation locally; importers must always accept it.
 *
 * Implementation strategy in dxmt9:
 *   - `D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_BITS` = 24, NONE sentinel
 *     = 0. `HandleArena::insert` only emits generations in [1, 2^24 - 1];
 *     `releaseSlot` bumps the generation and skips 0 on overflow so the
 *     sentinel is never produced by a live insert.
 *   - PE: `appendRecordWireHandleStamped` writes the wire entry; the
 *     `FromCoreHandle` overload decodes the generation from
 *     `core::Handle.value`. The plain `appendRecordWireHandleFrom` /
 *     `appendRecordWireHandle` callers stamp NONE because they only hold
 *     an opaque wrapper pointer.
 *   - Unix: `device_c_chunk_replay.cpp` walks `importedChunk.handles`,
 *     skips NONE entries, resolves each wrapper to its current
 *     `core::Handle`, and calls
 *     `d9c_command_chunk_wire_handle_generation_matches`. A mismatch is a
 *     hard reject before any record dispatch.
 *
 * Requirement traceability:
 *   include/dxmt9/device_c.h §"Cross-side generation check"
 *   src/d3d9/d3d9_pe_recorder.hpp::appendRecordWireHandleStamped
 *   src/d3d9/device_c_chunk_replay.cpp:705-772
 *   src/dxmt9/dxmt9_resource_pool.hpp::HandleArena (kGenerationBits = 24)
 *
 * Properties verified:
 *   Safety   — TypeOK, NoZombieAccept, LegacyNoneAlwaysAccepts,
 *              StampedMatchesArenaOnAdmit, NoForwardInconsistency
 *   Liveness — EventuallyDecided, EventuallyRejectStale
 *
 * Trade-off notes (not invariants, but model-level reminders):
 *   - LegacyNoneAlwaysAccepts is *intentional* opt-in laxity for opaque
 *     pointer producers. A wrong handle stamped NONE will be accepted by
 *     the generation gate; downstream wrapper-pointer validation must
 *     catch it. Promoting all callers to `FromCoreHandle` is the long-
 *     term plan; until then the trade-off is encoded here so reviewers
 *     are reminded the legacy path is a soft exception, not a bug.
 *   - The 24-bit domain means two distinct PE-side generations alias
 *     with probability ~1/2^24 in the worst case (a release+reinsert
 *     bursty enough to wrap). This is modeled by `GENERATION_DOMAIN` —
 *     when the model-checker exercises a constant that is wider than the
 *     handle slot count, no alias is possible and `NoZombieAccept` holds
 *     unconditionally. The aliasing reserved case is the documented
 *     residual risk and is *not* claimed as an invariant.
 *)

EXTENDS Naturals, FiniteSets, Sequences

CONSTANTS
  Handles,            \* set of handle slot identifiers (PE/unix shared id space)
  WireSlots,          \* set of wire-entry slot identifiers (the in-flight buffer)
  GENERATION_DOMAIN,  \* upper bound on per-slot generation values (models 2^24)
  MAX_BUMPS           \* model-checking bound on per-slot generation bumps

ASSUME Handles  # {}
ASSUME WireSlots # {}
ASSUME GENERATION_DOMAIN \in Nat /\ GENERATION_DOMAIN >= 2
ASSUME MAX_BUMPS \in Nat /\ MAX_BUMPS >= 1

(* ================================================================
   Domain encoding
   ================================================================ *)

\* `GENERATION_NONE` mirrors D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE.
\* Live arena generations live in 1..GENERATION_DOMAIN-1; the sentinel 0 is
\* never produced by `HandleArena::insert` and is reserved for the legacy
\* opaque-pointer recorder.
GENERATION_NONE == 0

\* WireOutcome is the per-slot decision the unix importer reaches for an
\* entry that has been written by PE.
WireOutcomes == {"Empty", "Pending", "Admitted", "Rejected"}

(* ================================================================
   State
   ================================================================ *)

VARIABLES
  arenaGen,    \* FUNCTION Handles → 1..GENERATION_DOMAIN-1
               \*   current live generation of slot h on the unix side
  arenaLive,   \* FUNCTION Handles → BOOLEAN
               \*   TRUE while the wrapper pointer is valid; FALSE after
               \*   release-without-reinsert (models the bare zombie case)
  bumpsLeft,   \* FUNCTION Handles → Nat — remaining DestroyAndReinsert
               \*   operations available for model-checking bounding
  wireHandle,  \* FUNCTION WireSlots → Handles ∪ {0}
               \*   the slot the wire entry references (0 = empty)
  wireStamp,   \* FUNCTION WireSlots → Nat
               \*   the stamped generation (0 = NONE legacy sentinel)
  wireKind,    \* FUNCTION WireSlots → {"None", "Stamped", "Legacy"}
               \*   producer encoding kind chosen at WriteWireEntry time
  outcome,     \* FUNCTION WireSlots → WireOutcomes
  observedGen, \* FUNCTION WireSlots → Nat — arena generation observed
               \*   by the importer at admit/reject time; 0 before the
               \*   importer has decided. Mirrors the value read out of
               \*   `resolved.value`'s generation field in
               \*   device_c_chunk_replay.cpp:768.
  observedLive \* FUNCTION WireSlots → BOOLEAN — whether the importer
               \*   saw the slot as live at decision time. Mirrors
               \*   `resolved.value == 0` ⇒ FALSE in the importer.

vars == <<arenaGen, arenaLive, bumpsLeft, wireHandle, wireStamp, wireKind,
          outcome, observedGen, observedLive>>

(* ================================================================
   Initialization
   ================================================================ *)

Init ==
  /\ arenaGen     = [h \in Handles  |-> 1]   \* HandleArena::insert starts at 1
  /\ arenaLive    = [h \in Handles  |-> TRUE]
  /\ bumpsLeft    = [h \in Handles  |-> MAX_BUMPS]
  /\ wireHandle   = [w \in WireSlots |-> 0]
  /\ wireStamp    = [w \in WireSlots |-> GENERATION_NONE]
  /\ wireKind     = [w \in WireSlots |-> "None"]
  /\ outcome      = [w \in WireSlots |-> "Empty"]
  /\ observedGen  = [w \in WireSlots |-> 0]
  /\ observedLive = [w \in WireSlots |-> FALSE]

(* ================================================================
   Actions — PE side
   ================================================================ *)

(*
 * WriteStampedWireEntry(w, h)
 * Models `appendRecordWireHandleFromCoreHandle` /
 * `appendRecordWireHandleStamped` with a non-NONE generation: PE has a
 * `core::Handle` in hand and stamps the encoded generation onto the wire
 * entry. The slot must currently be live (the PE-side wrapper has not yet
 * been released) — that is the producer-side precondition; once written,
 * the entry is `Pending` until the unix importer decides.
 *)
WriteStampedWireEntry(w, h) ==
  /\ outcome[w] = "Empty"
  /\ arenaLive[h]
  /\ wireHandle' = [wireHandle EXCEPT ![w] = h]
  /\ wireStamp'  = [wireStamp  EXCEPT ![w] = arenaGen[h]]
  /\ wireKind'   = [wireKind   EXCEPT ![w] = "Stamped"]
  /\ outcome'    = [outcome    EXCEPT ![w] = "Pending"]
  /\ UNCHANGED <<arenaGen, arenaLive, bumpsLeft, observedGen, observedLive>>

(*
 * WriteLegacyWireEntry(w, h)
 * Models `appendRecordWireHandleFrom` / `appendRecordWireHandle`: PE only
 * has an opaque wrapper pointer and stamps `GENERATION_NONE`. The
 * importer will skip the cross-side equality check for this entry.
 *
 * The wrapper pointer itself may already be dead — this is the documented
 * trade-off of the legacy path: any stamped-NONE entry is admitted on the
 * generation gate, and downstream pointer validation (collectImported…)
 * is the remaining defense.
 *)
WriteLegacyWireEntry(w, h) ==
  /\ outcome[w] = "Empty"
  /\ wireHandle' = [wireHandle EXCEPT ![w] = h]
  /\ wireStamp'  = [wireStamp  EXCEPT ![w] = GENERATION_NONE]
  /\ wireKind'   = [wireKind   EXCEPT ![w] = "Legacy"]
  /\ outcome'    = [outcome    EXCEPT ![w] = "Pending"]
  /\ UNCHANGED <<arenaGen, arenaLive, bumpsLeft, observedGen, observedLive>>

(* ================================================================
   Actions — HandleArena side
   ================================================================ *)

(*
 * DestroyHandle(h)
 * The wrapper is released. `HandleArena::releaseSlot` clears the record
 * and bumps the slot generation modulo `GENERATION_DOMAIN`, skipping 0.
 * After this fires, any wire entry that was stamped with the old
 * generation references a zombie slot from the unix importer's
 * perspective.
 *)
DestroyHandle(h) ==
  /\ arenaLive[h]
  /\ bumpsLeft[h] > 0
  /\ arenaLive' = [arenaLive EXCEPT ![h] = FALSE]
  /\ bumpsLeft' = [bumpsLeft EXCEPT ![h] = bumpsLeft[h] - 1]
  /\ UNCHANGED <<arenaGen, wireHandle, wireStamp, wireKind, outcome,
                 observedGen, observedLive>>

(*
 * ReinsertHandle(h)
 * A subsequent `HandleArena::insert` reuses the free slot. The generation
 * advances; we model the 2^24 domain by wrap-around with NONE skip,
 * matching `nextGeneration` in dxmt9_resource_pool.hpp.
 *)
NextGen(g) ==
  IF ((g + 1) % GENERATION_DOMAIN) = 0
  THEN 1
  ELSE (g + 1) % GENERATION_DOMAIN

ReinsertHandle(h) ==
  /\ ~arenaLive[h]
  /\ arenaGen'  = [arenaGen  EXCEPT ![h] = NextGen(arenaGen[h])]
  /\ arenaLive' = [arenaLive EXCEPT ![h] = TRUE]
  /\ UNCHANGED <<bumpsLeft, wireHandle, wireStamp, wireKind, outcome,
                 observedGen, observedLive>>

(* ================================================================
   Actions — unix-side commit
   ================================================================ *)

(*
 * CommitChunkEntry(w)
 * Mirrors the per-entry check in `device_c_chunk_replay.cpp:705-772`:
 *
 *   - Legacy NONE: skip generation check ⇒ admit.
 *   - Non-NONE stamp: resolve wrapper to a core::Handle.
 *     * If the wrapper is no longer live (dead pointer / null obj), the
 *       resolved core handle is 0 and the importer rejects with
 *       `bad-handle-generation`.
 *     * Otherwise compare stamped vs encoded generation; mismatch ⇒
 *       reject; match ⇒ admit.
 *)
CommitChunkEntry(w) ==
  /\ outcome[w] = "Pending"
  /\ LET h     == wireHandle[w]
         stamp == wireStamp[w]
         kind  == wireKind[w]
     IN
       \/ /\ kind = "Legacy"
          /\ outcome'      = [outcome      EXCEPT ![w] = "Admitted"]
          /\ observedGen'  = [observedGen  EXCEPT ![w] = arenaGen[h]]
          /\ observedLive' = [observedLive EXCEPT ![w] = arenaLive[h]]
       \/ /\ kind = "Stamped"
          /\ \/ /\ ~arenaLive[h]
                /\ outcome'      = [outcome      EXCEPT ![w] = "Rejected"]
                /\ observedGen'  = [observedGen  EXCEPT ![w] = arenaGen[h]]
                /\ observedLive' = [observedLive EXCEPT ![w] = FALSE]
             \/ /\ arenaLive[h]
                /\ \/ /\ arenaGen[h] = stamp
                      /\ outcome'      = [outcome      EXCEPT ![w] = "Admitted"]
                      /\ observedGen'  = [observedGen  EXCEPT ![w] = arenaGen[h]]
                      /\ observedLive' = [observedLive EXCEPT ![w] = TRUE]
                   \/ /\ arenaGen[h] # stamp
                      /\ outcome'      = [outcome      EXCEPT ![w] = "Rejected"]
                      /\ observedGen'  = [observedGen  EXCEPT ![w] = arenaGen[h]]
                      /\ observedLive' = [observedLive EXCEPT ![w] = TRUE]
  /\ UNCHANGED <<arenaGen, arenaLive, bumpsLeft, wireHandle, wireStamp, wireKind>>

(* ================================================================
   Specification
   ================================================================ *)

Next ==
  \/ \E w \in WireSlots, h \in Handles : WriteStampedWireEntry(w, h)
  \/ \E w \in WireSlots, h \in Handles : WriteLegacyWireEntry(w, h)
  \/ \E h \in Handles : DestroyHandle(h)
  \/ \E h \in Handles : ReinsertHandle(h)
  \/ \E w \in WireSlots : CommitChunkEntry(w)

Spec ==
  Init
  /\ [][Next]_vars
  /\ \A w \in WireSlots : WF_vars(CommitChunkEntry(w))

(* ================================================================
   Type invariant
   ================================================================ *)

TypeOK ==
  /\ arenaGen     \in [Handles  -> 1..(GENERATION_DOMAIN - 1)]
  /\ arenaLive    \in [Handles  -> BOOLEAN]
  /\ bumpsLeft    \in [Handles  -> 0..MAX_BUMPS]
  /\ wireHandle   \in [WireSlots -> Handles \cup {0}]
  /\ wireStamp    \in [WireSlots -> 0..(GENERATION_DOMAIN - 1)]
  /\ wireKind     \in [WireSlots -> {"None", "Stamped", "Legacy"}]
  /\ outcome      \in [WireSlots -> WireOutcomes]
  /\ observedGen  \in [WireSlots -> 0..(GENERATION_DOMAIN - 1)]
  /\ observedLive \in [WireSlots -> BOOLEAN]

(* ================================================================
   Safety invariants
   ================================================================ *)

(*
 * NoZombieAccept
 * The headline invariant: if the importer admitted a wire entry that
 * carried a non-NONE stamped generation, then the arena's current
 * generation for that slot must equal the stamped generation. Stated
 * contrapositively: a zombie/use-after-free wire record (where the slot
 * was released and re-keyed between PE record and unix commit) cannot
 * reach `Admitted`.
 *)
NoZombieAccept ==
  \A w \in WireSlots :
    (outcome[w] = "Admitted" /\ wireKind[w] = "Stamped")
      => /\ wireHandle[w] \in Handles
         /\ observedLive[w]
         /\ observedGen[w] = wireStamp[w]

(*
 * LegacyNoneAlwaysAccepts
 * The opt-in trade-off: any wire entry whose stamp is the NONE sentinel
 * and whose outcome is decided MUST have been admitted, never rejected
 * on the generation gate. (Rejection of a Legacy entry would be a
 * regression in the importer's NONE-skip path; downstream wrapper
 * validation is a separate concern.)
 *)
LegacyNoneAlwaysAccepts ==
  \A w \in WireSlots :
    wireKind[w] = "Legacy" /\ outcome[w] \in {"Admitted", "Rejected"}
      => outcome[w] = "Admitted"

(*
 * StampedMatchesArenaOnAdmit
 * Same content as NoZombieAccept, restated as a pure equality on the
 * admit boundary. Kept separately because TLC reports failures by
 * invariant name and a localized equality check makes counter-examples
 * smaller.
 *)
StampedMatchesArenaOnAdmit ==
  \A w \in WireSlots :
    (outcome[w] = "Admitted" /\ wireStamp[w] # GENERATION_NONE)
      => observedGen[w] = wireStamp[w]

(*
 * NoForwardInconsistency
 * No PE-stamped wire entry carries a generation outside the live arena
 * domain. Concretely: a Stamped entry must hold a value in
 * 1..GENERATION_DOMAIN-1 — never NONE, never above the domain. This is
 * the model-level statement of the 24-bit field being honored.
 *
 * The aliasing residual (two distinct PE writes happen to land the same
 * generation after a release+reinsert wrap) lives outside this
 * invariant; the model exercises it only when GENERATION_DOMAIN is set
 * small enough to allow wrap-around, and we document it as the residual
 * ~1/2^24 false-accept risk rather than claiming it as a safety
 * guarantee.
 *)
NoForwardInconsistency ==
  \A w \in WireSlots :
    wireKind[w] = "Stamped"
      => wireStamp[w] \in 1..(GENERATION_DOMAIN - 1)

Safety ==
  TypeOK
  /\ NoZombieAccept
  /\ LegacyNoneAlwaysAccepts
  /\ StampedMatchesArenaOnAdmit
  /\ NoForwardInconsistency

(* ================================================================
   Liveness
   ================================================================ *)

(*
 * EventuallyDecided
 * Every wire entry that reaches the importer (`Pending`) is eventually
 * either admitted or rejected. The unix-side loop in
 * `device_c_chunk_replay.cpp:727` is strongly-fair on the model side via
 * `WF_vars(CommitChunkEntry(w))`.
 *)
EventuallyDecided ==
  \A w \in WireSlots :
    outcome[w] = "Pending" ~> outcome[w] \in {"Admitted", "Rejected"}

(*
 * EventuallyRejectStale
 * The stale-handle case: if PE stamped a generation but the slot is
 * destroyed (and not yet reinserted to the same generation) at the
 * moment the importer runs, the importer eventually decides reject.
 * Modeled by the disjunction "either reinsertion races back to the
 * stamped generation (admit) or the slot is still dead/different
 * (reject)." Either way the entry leaves Pending — the unsafe outcome
 * "stays Pending forever" is what this property rules out.
 *)
EventuallyRejectStale ==
  \A w \in WireSlots :
    (outcome[w] = "Pending" /\ wireKind[w] = "Stamped")
      ~> outcome[w] \in {"Admitted", "Rejected"}

====
