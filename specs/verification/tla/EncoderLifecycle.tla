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
 * Architectural rules (from backend/design.md §3, backend/requirements.md §2):
 *   (1) At most one encoder is active at any time.
 *   (2) Switching encoder kind requires ending the current encoder first.
 *   (3) SetRenderTarget() during a scene terminates the current Render encoder
 *       and begins a new one.
 *   (4) A Render encoder may be extended (merged) across consecutive draw calls
 *       when the render target attachments are identical AND exact read/write
 *       hazard tracking reports no conflict (approximated here as a boolean flag).
 *
 * Requirement traceability:
 *   R-BACK-2.4  Merge draw calls sharing render targets (no unnecessary split)
 *   R-BACK-2.6  Render target change must terminate and begin a new encoder
 *
 * Properties verified:
 *   Safety   — TypeOK, AtMostOneEncoder, KindSwitchThroughIdle,
 *               RenderTargetConsistency
 *   Liveness — ActiveEncoderEventuallyEnds
 *)

EXTENDS Naturals

CONSTANTS
  MAX_OPS,       \* model-checking bound on total encoder transitions
  RenderTargets  \* set of render target identifiers (e.g., {rt1, rt2})

ASSUME MAX_OPS \in Nat /\ MAX_OPS >= 1
ASSUME RenderTargets # {}

EncoderKinds == {"None", "Render", "Blit", "Compute"}

VARIABLES
  activeKind,    \* EncoderKinds — currently active encoder type
  activeRT,      \* RenderTargets ∪ {NoRT} — current render target (NoRT if not Render)
  hazardFlag,    \* BOOLEAN -- simulates exact hazard tracking detecting a read/write conflict
  opCount        \* Nat — model-checking op counter

NoRT == "NoRT"   \* sentinel: no render target active

vars == <<activeKind, activeRT, hazardFlag, opCount>>

(* ================================================================
   Initialization
   ================================================================ *)

Init ==
  /\ activeKind = "None"
  /\ activeRT   = NoRT
  /\ hazardFlag = FALSE
  /\ opCount    = 0

(* ================================================================
   Actions
   ================================================================ *)

(*
 * EndEncoder
 * Ends the currently active encoder (any kind).
 * Metal: [encoder endEncoding]
 *)
EndEncoder ==
  /\ activeKind # "None"
  /\ activeKind'  = "None"
  /\ activeRT'    = NoRT
  /\ hazardFlag'  = FALSE
  /\ UNCHANGED opCount

(*
 * BeginRender(rt)
 * Opens a new MTLRenderCommandEncoder targeting render target rt.
 * Precondition: no encoder currently active (must call EndEncoder first).
 *)
BeginRender(rt) ==
  /\ activeKind = "None"
  /\ opCount < MAX_OPS
  /\ activeKind' = "Render"
  /\ activeRT'   = rt
  /\ opCount'    = opCount + 1
  /\ UNCHANGED hazardFlag

(*
 * BeginBlit
 * Opens a new MTLBlitCommandEncoder.
 *)
BeginBlit ==
  /\ activeKind = "None"
  /\ opCount < MAX_OPS
  /\ activeKind' = "Blit"
  /\ activeRT'   = NoRT
  /\ opCount'    = opCount + 1
  /\ UNCHANGED hazardFlag

(*
 * BeginCompute
 * Opens a new MTLComputeCommandEncoder.
 *)
BeginCompute ==
  /\ activeKind = "None"
  /\ opCount < MAX_OPS
  /\ activeKind' = "Compute"
  /\ activeRT'   = NoRT
  /\ opCount'    = opCount + 1
  /\ UNCHANGED hazardFlag

(*
 * MergeRenderDraw
 * EncodeThread merges a new draw call into the existing Render encoder.
 * Permitted only when:
 *   - encoder kind is Render, AND
 *   - same render target as the active encoder, AND
 *   - exact hazard tracking indicates no read/write hazard (hazardFlag = FALSE).
 * R-BACK-2.4: merging must be considered; splitting is correct but unnecessary.
 *)
MergeRenderDraw(rt) ==
  /\ activeKind = "Render"
  /\ activeRT   = rt          \* same attachment
  /\ ~hazardFlag              \* exact hazard tracking: no detected conflict
  /\ opCount < MAX_OPS
  /\ opCount' = opCount + 1
  /\ UNCHANGED <<activeKind, activeRT, hazardFlag>>

(*
 * HazardDetected
 * Exact hazard tracking signals a read/write conflict.
 * Forces a split: the current Render encoder must be ended before the next draw.
 * Bloom false positives are diagnostic-only and are not modeled as split causes.
 *)
HazardDetected ==
  /\ activeKind = "Render"
  /\ ~hazardFlag
  /\ hazardFlag' = TRUE
  /\ UNCHANGED <<activeKind, activeRT, opCount>>

(*
 * RenderTargetChange(newRT)
 * D3D9 SetRenderTarget() during a scene.
 * R-BACK-2.6: terminates the current Render encoder and begins a new one.
 *
 * Modeled as an atomic End + BeginRender(newRT) to reflect that no intermediate
 * Idle state is visible to the command stream — the transition is internal to
 * queue-local command record replay.
 *)
RenderTargetChange(newRT) ==
  /\ activeKind = "Render"
  /\ newRT # activeRT
  /\ opCount < MAX_OPS
  \* End current encoder, begin new one targeting newRT
  /\ activeRT'   = newRT
  /\ hazardFlag' = FALSE
  /\ opCount'    = opCount + 1
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
  /\ activeKind' = newKind
  /\ activeRT'   = NoRT
  /\ opCount'    = opCount + 1
  /\ UNCHANGED hazardFlag

(* ================================================================
   Specification
   ================================================================ *)

Next ==
  \/ EndEncoder
  \/ \E rt \in RenderTargets : BeginRender(rt)
  \/ BeginBlit
  \/ BeginCompute
  \/ \E rt \in RenderTargets : MergeRenderDraw(rt)
  \/ HazardDetected
  \/ \E rt \in RenderTargets : RenderTargetChange(rt)
  \/ \E k \in {"Blit", "Compute"} : KindSwitch(k)

Spec == Init /\ [][Next]_vars /\ WF_vars(EndEncoder)

(* ================================================================
   Type invariant
   ================================================================ *)

TypeOK ==
  /\ activeKind \in EncoderKinds
  /\ activeRT   \in RenderTargets \cup {NoRT}
  /\ hazardFlag \in BOOLEAN
  /\ opCount    \in Nat

(* ================================================================
   Safety invariants
   ================================================================ *)

(*
 * AtMostOneEncoder
 * There is exactly one active encoder state at any time.
 * This is trivially guaranteed by having a single activeKind variable,
 * but it formalizes the architectural guarantee that no two encoders
 * can be open simultaneously.
 *)
AtMostOneEncoder ==
  activeKind \in EncoderKinds   \* exactly one value

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
 * HazardForcesNewEncoder
 * When a hazard is detected (hazardFlag = TRUE), no further MergeRenderDraw
 * is possible until the encoder is ended and a new one is started
 * (hazardFlag is cleared on EndEncoder / RenderTargetChange).
 * Verified implicitly by MergeRenderDraw's ~hazardFlag precondition.
 *)

Safety == TypeOK /\ RenderTargetConsistency /\ KindSwitchThroughIdle

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
