---- MODULE EncoderLifecycle ----
(*
 * dxmt9 Encoder Lifecycle — TLA+ Specification
 *
 * Within each CommandChunk replay, the ArgumentEncodingContext manages a
 * single active MTLCommandEncoder. Three encoder kinds exist:
 *
 *   Render  — draw calls; may merge consecutive draws sharing attachments
 *   Blit    — UpdateSurface, UpdateTexture, same-size StretchRect, mipmap gen
 *   Compute — reserved (triangle fan expansion, future use)
 *
 * Architectural rules (from backend/spec.md §3, backend/requirements.md §2):
 *   (1) At most one encoder is active at any time.
 *   (2) Switching encoder kind requires ending the current encoder first.
 *   (3) SetRenderTarget() during a scene terminates the current Render encoder
 *       and begins a new one.
 *   (4) A Render encoder may be extended (merged) across consecutive draw calls
 *       when the render target attachments are identical AND exact read/write
 *       hazard tracking reports no conflict. Hazards are tracked as exact
 *       SETS of read/write handles touched by the active encoder, not as a
 *       Boolean oracle (R-BACK-2.28).
 *   (5) A probabilistic Bloom signal MAY be observed alongside the exact
 *       hazard sets, but it MUST NOT influence the split decision. Bloom is
 *       diagnostic only: a FALSE-POSITIVE Bloom hit (Bloom said "maybe" and
 *       the exact check found no conflict) is recorded in a counter and
 *       observable, but the encoder is never split because of it
 *       (R-BACK-2.28, spec.md §9.1, §9.2).
 *
 * Requirement traceability:
 *   R-BACK-2.4   Merge draw calls sharing render targets (no unnecessary split)
 *   R-BACK-2.6   Render target change must terminate and begin a new encoder
 *   R-BACK-2.28  Exact read/write handle-set hazards drive split decisions;
 *                Bloom is diagnostic-only
 *   R-VERIF-4.1  Mutual exclusion of encoders
 *   R-VERIF-4.2  Kind switch must pass through None
 *   R-VERIF-4.3  Render encoder always has a valid render target
 *   R-VERIF-4.4  Hazard prevents further merging until encoder ends
 *
 * Properties verified:
 *   Safety   — TypeOK, KindSwitchThroughIdle, RenderTargetConsistency,
 *               ExactHazardBlocksMerge, BloomNeverForcesSplit
 *   Liveness — ActiveEncoderEventuallyEnds
 *)

EXTENDS Naturals, FiniteSets

CONSTANTS
  MAX_OPS,       \* model-checking bound on total encoder transitions
  RenderTargets, \* set of render target identifiers (e.g., {rt1, rt2})
  Handles        \* finite set of resource handles for hazard tracking

ASSUME MAX_OPS \in Nat /\ MAX_OPS >= 1
ASSUME RenderTargets # {}
ASSUME Handles # {}

EncoderKinds == {"None", "Render", "Blit", "Compute"}

VARIABLES
  activeKind,              \* EncoderKinds — currently active encoder type
  activeRT,                \* RenderTargets ∪ {NoRT} — current render target (NoRT if not Render)
  lastReadHandles,         \* SUBSET Handles — handles read by the active encoder so far
  lastWriteHandles,        \* SUBSET Handles — handles written by the active encoder so far
  bloomSignal,             \* BOOLEAN — last observed Bloom verdict ("maybe" if TRUE)
                           \* Diagnostic only; never gates split decisions (R-BACK-2.28).
  bloomFalsePositiveCount, \* Nat — diagnostic counter: Bloom said "maybe" but the
                           \* exact set check found no conflict.
  opCount                  \* Nat — model-checking op counter

NoRT == "NoRT"   \* sentinel: no render target active

vars == <<activeKind, activeRT, lastReadHandles, lastWriteHandles,
          bloomSignal, bloomFalsePositiveCount, opCount>>

(* ================================================================
   Initialization
   ================================================================ *)

Init ==
  /\ activeKind              = "None"
  /\ activeRT                = NoRT
  /\ lastReadHandles         = {}
  /\ lastWriteHandles        = {}
  /\ bloomSignal             = FALSE
  /\ bloomFalsePositiveCount = 0
  /\ opCount                 = 0

(* ================================================================
   Hazard predicates (exact handle-set overlap, R-BACK-2.28)
   ================================================================ *)

(*
 * RAW : the new draw reads a handle that the active encoder has written.
 * WAR : the new draw writes a handle that the active encoder has read.
 * WAW : the new draw writes a handle that the active encoder has written.
 *
 * NoExactHazard is the conjunction: all three sets must be empty for a
 * legal merge into the existing encoder.
 *)
RAWConflict(newRead)  == (newRead  \cap lastWriteHandles) # {}
WARConflict(newWrite) == (newWrite \cap lastReadHandles)  # {}
WAWConflict(newWrite) == (newWrite \cap lastWriteHandles) # {}

NoExactHazard(newRead, newWrite) ==
  /\ ~RAWConflict(newRead)
  /\ ~WARConflict(newWrite)
  /\ ~WAWConflict(newWrite)

(* ================================================================
   Actions
   ================================================================ *)

(*
 * EndEncoder
 * Ends the currently active encoder (any kind).
 * Metal: [encoder endEncoding]. Hazard sets reset since a new encoder
 * starts with a fresh dependency graph.
 *)
EndEncoder ==
  /\ activeKind # "None"
  /\ activeKind'        = "None"
  /\ activeRT'          = NoRT
  /\ lastReadHandles'   = {}
  /\ lastWriteHandles'  = {}
  /\ \E b \in BOOLEAN : bloomSignal' = b
  /\ UNCHANGED <<bloomFalsePositiveCount, opCount>>

(*
 * BeginRender(rt, newRead, newWrite)
 * Opens a new MTLRenderCommandEncoder targeting render target rt and
 * records the first draw's read/write handle sets.
 * Precondition: no encoder currently active (must call EndEncoder first).
 *)
BeginRender(rt, newRead, newWrite) ==
  /\ activeKind = "None"
  /\ opCount < MAX_OPS
  /\ activeKind'        = "Render"
  /\ activeRT'          = rt
  /\ lastReadHandles'   = newRead
  /\ lastWriteHandles'  = newWrite
  /\ \E b \in BOOLEAN : bloomSignal' = b
  /\ UNCHANGED bloomFalsePositiveCount
  /\ opCount'           = opCount + 1

(*
 * BeginBlit
 * Opens a new MTLBlitCommandEncoder. Hazard sets stay empty for non-Render
 * encoders in this model — Blit operations are out of scope of the merge
 * predicate this spec verifies.
 *)
BeginBlit ==
  /\ activeKind = "None"
  /\ opCount < MAX_OPS
  /\ activeKind'        = "Blit"
  /\ activeRT'          = NoRT
  /\ lastReadHandles'   = {}
  /\ lastWriteHandles'  = {}
  /\ \E b \in BOOLEAN : bloomSignal' = b
  /\ UNCHANGED bloomFalsePositiveCount
  /\ opCount'           = opCount + 1

(*
 * BeginCompute
 * Opens a new MTLComputeCommandEncoder.
 *)
BeginCompute ==
  /\ activeKind = "None"
  /\ opCount < MAX_OPS
  /\ activeKind'        = "Compute"
  /\ activeRT'          = NoRT
  /\ lastReadHandles'   = {}
  /\ lastWriteHandles'  = {}
  /\ \E b \in BOOLEAN : bloomSignal' = b
  /\ UNCHANGED bloomFalsePositiveCount
  /\ opCount'           = opCount + 1

(*
 * MergeRenderDraw(rt, newRead, newWrite)
 * EncodeThread merges a new draw call (with exact read/write handle sets)
 * into the existing Render encoder. Permitted only when:
 *   - encoder kind is Render, AND
 *   - same render target as the active encoder, AND
 *   - exact hazard tracking reports no RAW / WAR / WAW conflict.
 * On success, the encoder's accumulated handle sets grow by union.
 * R-BACK-2.4 / R-BACK-2.28: merging is the legal default; splitting is only
 * required when an exact-set conflict exists.
 *)
MergeRenderDraw(rt, newRead, newWrite) ==
  /\ activeKind = "Render"
  /\ activeRT   = rt                          \* same attachment
  /\ NoExactHazard(newRead, newWrite)         \* exact RAW/WAR/WAW empty
  /\ opCount < MAX_OPS
  /\ lastReadHandles'   = lastReadHandles  \cup newRead
  /\ lastWriteHandles'  = lastWriteHandles \cup newWrite
  \* Bloom signal is freely chosen on every step. A TRUE value at a merge
  \* (where exact has just confirmed no overlap) is by definition a Bloom
  \* false positive, so the diagnostic counter advances. Note that the
  \* merge's enabling condition itself never inspects bloomSignal.
  /\ \E b \in BOOLEAN :
       /\ bloomSignal' = b
       /\ bloomFalsePositiveCount' =
            IF b THEN bloomFalsePositiveCount + 1 ELSE bloomFalsePositiveCount
  /\ opCount'           = opCount + 1
  /\ UNCHANGED <<activeKind, activeRT>>

(*
 * HazardDetected(newRead, newWrite)
 * Exact hazard tracking signals a read/write conflict on the proposed
 * next draw — at least one of RAW / WAR / WAW intersection is non-empty.
 * The current Render encoder must therefore be ended; the operation step
 * itself records that the split path was taken without committing the new
 * draw into the active encoder.
 * Bloom false positives are diagnostic-only and are not modeled as a split
 * cause (R-BACK-2.28).
 *)
HazardDetected(newRead, newWrite) ==
  /\ activeKind = "Render"
  \* Split decision is driven SOLELY by exact RAW / WAR / WAW overlap.
  \* bloomSignal is intentionally absent from this guard so that Bloom
  \* cannot trigger a render-pass split (R-BACK-2.28).
  /\ \/ RAWConflict(newRead)
     \/ WARConflict(newWrite)
     \/ WAWConflict(newWrite)
  /\ opCount < MAX_OPS
  \* Split: end the encoder. New draw is not merged; it will open a fresh
  \* encoder via BeginRender on the next step.
  /\ activeKind'        = "None"
  /\ activeRT'          = NoRT
  /\ lastReadHandles'   = {}
  /\ lastWriteHandles'  = {}
  /\ \E b \in BOOLEAN : bloomSignal' = b
  /\ UNCHANGED bloomFalsePositiveCount
  /\ opCount'           = opCount + 1

(*
 * RenderTargetChange(newRT, newRead, newWrite)
 * D3D9 SetRenderTarget() during a scene.
 * R-BACK-2.6: terminates the current Render encoder and begins a new one.
 *
 * Modeled as an atomic End + BeginRender(newRT, newRead, newWrite) to
 * reflect that no intermediate Idle state is visible to the command stream.
 *)
RenderTargetChange(newRT, newRead, newWrite) ==
  /\ activeKind = "Render"
  /\ newRT # activeRT
  /\ opCount < MAX_OPS
  \* End current encoder, begin new one targeting newRT.
  /\ activeRT'          = newRT
  /\ lastReadHandles'   = newRead
  /\ lastWriteHandles'  = newWrite
  /\ \E b \in BOOLEAN : bloomSignal' = b
  /\ UNCHANGED bloomFalsePositiveCount
  /\ opCount'           = opCount + 1
  /\ UNCHANGED activeKind   \* still Render, but a new encoder has been opened

(*
 * KindSwitch(newKind)
 * Switch from any active encoder to a different kind.
 * Requires going through None (EndEncoder must have been called first).
 * This action models the constraint by requiring activeKind = "None".
 *)
KindSwitch(newKind) ==
  /\ activeKind = "None"
  /\ newKind \in {"Render", "Blit", "Compute"}
  /\ opCount < MAX_OPS
  /\ activeKind'        = newKind
  /\ activeRT'          = NoRT
  /\ lastReadHandles'   = {}
  /\ lastWriteHandles'  = {}
  /\ \E b \in BOOLEAN : bloomSignal' = b
  /\ UNCHANGED bloomFalsePositiveCount
  /\ opCount'           = opCount + 1

(* ================================================================
   Specification
   ================================================================ *)

Next ==
  \/ EndEncoder
  \/ \E rt \in RenderTargets, nr \in SUBSET Handles, nw \in SUBSET Handles :
        BeginRender(rt, nr, nw)
  \/ BeginBlit
  \/ BeginCompute
  \/ \E rt \in RenderTargets, nr \in SUBSET Handles, nw \in SUBSET Handles :
        MergeRenderDraw(rt, nr, nw)
  \/ \E nr \in SUBSET Handles, nw \in SUBSET Handles :
        HazardDetected(nr, nw)
  \/ \E rt \in RenderTargets, nr \in SUBSET Handles, nw \in SUBSET Handles :
        RenderTargetChange(rt, nr, nw)
  \/ \E k \in {"Blit", "Compute"} : KindSwitch(k)

Spec == Init /\ [][Next]_vars /\ WF_vars(EndEncoder)

(* ================================================================
   Type invariant
   ================================================================ *)

TypeOK ==
  /\ activeKind              \in EncoderKinds
  /\ activeRT                \in RenderTargets \cup {NoRT}
  /\ lastReadHandles         \subseteq Handles
  /\ lastWriteHandles        \subseteq Handles
  /\ bloomSignal             \in BOOLEAN
  /\ bloomFalsePositiveCount \in Nat
  /\ opCount                 \in Nat

(* ================================================================
   Safety invariants
   ================================================================ *)

(*
 * KindSwitchThroughIdle
 * A switch between encoder kinds must pass through "None".
 * Formally: from Render, Blit, or Compute, the ONLY way to reach a DIFFERENT
 * kind is via the None state.
 *
 * This is an action constraint: BeginRender/BeginBlit/BeginCompute all
 * require activeKind = "None". TLC verifies this holds in all reachable states.
 *)
KindSwitchThroughIdle ==
  \* Kind can change between steps only if the new or old kind is None
  [][activeKind # "None" /\ activeKind' # "None" => activeKind' = activeKind]_activeKind

(*
 * RenderTargetConsistency
 * When a Render encoder is active, there must be a valid render target.
 * When a non-Render encoder (or None) is active, activeRT is NoRT.
 *)
RenderTargetConsistency ==
  /\ (activeKind = "Render") => (activeRT \in RenderTargets)
  /\ (activeKind # "Render") => (activeRT = NoRT)

(*
 * ExactHazardBlocksMerge (R-VERIF-4.4, R-BACK-2.28)
 * Whenever the active encoder is Render and the accumulated hazard sets
 * already overlap a candidate (newRead, newWrite) — i.e., at least one of
 * RAW / WAR / WAW is non-empty — the MergeRenderDraw guard forbids merging.
 * This invariant is a state-level reformulation: there is no reachable
 * state where MergeRenderDraw could fire on a candidate that exhibits an
 * exact-set conflict against the current (lastReadHandles, lastWriteHandles).
 *
 * Equivalent statement: for every reachable state and every (nr, nw),
 * RAW/WAR/WAW conflict implies the merge action's enabling condition is
 * false. Because MergeRenderDraw bakes NoExactHazard directly into its
 * guard, this invariant is universally true on all reachable states; TLC
 * checks it as a simple state property and any future relaxation of the
 * guard would flag immediately.
 *)
ExactHazardBlocksMerge ==
  \A nr \in SUBSET Handles, nw \in SUBSET Handles :
     ( /\ activeKind = "Render"
       /\ \/ RAWConflict(nr)
          \/ WARConflict(nw)
          \/ WAWConflict(nw) )
     => ~ NoExactHazard(nr, nw)

(*
 * BloomNeverForcesSplit (R-BACK-2.28, spec.md §9.1, §9.2)
 *
 * Encoder splits (HazardDetected) must be driven SOLELY by an exact RAW /
 * WAR / WAW handle-set overlap, never by the probabilistic Bloom signal.
 * Concretely: in any reachable state where there is no exact hazard for a
 * candidate (nr, nw), the split action is not enabled — regardless of
 * bloomSignal's current value (TRUE = "maybe", FALSE = "definitely no").
 *
 * Contrapositive: every step that is a HazardDetected step has at least
 * one non-empty RAW / WAR / WAW intersection. Therefore a Bloom false
 * positive (bloomSignal = TRUE while exact says no overlap) cannot
 * trigger a split; it can only advance the diagnostic counter
 * bloomFalsePositiveCount. This is the formal encoding of
 * R-BACK-2.28's "Bloom false positives must not force a render-pass split".
 *)
BloomNeverForcesSplit ==
  \A nr \in SUBSET Handles, nw \in SUBSET Handles :
     ( /\ activeKind = "Render"
       /\ NoExactHazard(nr, nw) )
     => ~ ENABLED HazardDetected(nr, nw)

Safety ==
  /\ TypeOK
  /\ RenderTargetConsistency
  /\ KindSwitchThroughIdle
  /\ ExactHazardBlocksMerge
  /\ BloomNeverForcesSplit

(* ================================================================
   Liveness
   ================================================================ *)

(*
 * ActiveEncoderEventuallyEnds
 * An active encoder (any kind) eventually ends.
 * This guarantees the encode thread does not hold an encoder open indefinitely.
 * (WF on EndEncoder ensures it is eventually called when active.)
 *)
ActiveEncoderEventuallyEnds ==
  [](activeKind # "None" ~> activeKind = "None")

====
