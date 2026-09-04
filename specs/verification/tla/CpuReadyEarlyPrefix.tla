---- MODULE CpuReadyEarlyPrefix ----
(***************************************************************************
 * Default-off bounded source-publication experiment. One non-Present source
 * may publish only while a distinct trailing publication credit and Writing
 * owner are reserved. Encode may park one unsubmitted session; only the
 * Present tail submits it. Ordered controls, capacity denial, and stop remain
 * pre-effect fallback/terminal edges and cannot create a second command
 * buffer or render pass.
 *************************************************************************)

EXTENDS Naturals, TLC

Phases == {"Candidate", "PresentOnly", "PrefixReady", "Parked",
           "OrdinaryPending", "OrdinaryWithPrefix", "TailReady",
           "Submitted", "Stopped"}

VARIABLES phase, tailCredit, tailOwner, prefixPublications,
          sourceEffects, presentEffects, commandBuffers, renderPasses,
          ordinaryPending, lease

vars == <<phase, tailCredit, tailOwner, prefixPublications,
          sourceEffects, presentEffects, commandBuffers, renderPasses,
          ordinaryPending, lease>>

Init ==
  /\ phase = "Candidate"
  /\ tailCredit \in BOOLEAN
  /\ tailOwner = FALSE
  /\ prefixPublications = 0
  /\ sourceEffects = 0
  /\ presentEffects = 0
  /\ commandBuffers = 0
  /\ renderPasses = 0
  /\ ordinaryPending = FALSE
  /\ lease = FALSE

EncodeOrdinary ==
  /\ phase = "Candidate"
  /\ phase' = "OrdinaryPending"
  /\ ordinaryPending' = TRUE
  /\ lease' = TRUE
  /\ UNCHANGED <<tailCredit, tailOwner, prefixPublications,
                  sourceEffects, presentEffects, commandBuffers,
                  renderPasses>>

PublishPrefix ==
  /\ phase = "Candidate"
  /\ tailCredit
  /\ phase' = "PrefixReady"
  /\ tailOwner' = TRUE
  /\ prefixPublications' = 1
  /\ lease' = TRUE
  /\ UNCHANGED <<tailCredit, sourceEffects, presentEffects,
                  commandBuffers, renderPasses, ordinaryPending>>

PublishPrefixBehindOrdinary ==
  /\ phase = "OrdinaryPending"
  /\ tailCredit
  /\ ordinaryPending
  /\ phase' = "OrdinaryWithPrefix"
  /\ tailOwner' = TRUE
  /\ prefixPublications' = 1
  /\ UNCHANGED <<tailCredit, sourceEffects, presentEffects,
                  commandBuffers, renderPasses, ordinaryPending, lease>>

FoldPrefixIntoOrdinary ==
  /\ phase = "OrdinaryWithPrefix"
  /\ ordinaryPending
  /\ phase' = "OrdinaryWithPrefix"
  /\ sourceEffects' = 1
  /\ UNCHANGED <<tailCredit, tailOwner, prefixPublications,
                  presentEffects, commandBuffers, renderPasses,
                  ordinaryPending, lease>>

FailClosed ==
  /\ phase = "Candidate"
  /\ ~tailCredit
  /\ phase' = "PresentOnly"
  /\ UNCHANGED <<tailCredit, tailOwner, prefixPublications,
                  sourceEffects, presentEffects, commandBuffers,
                  renderPasses, ordinaryPending, lease>>

EncodeAndPark ==
  /\ phase = "PrefixReady"
  /\ tailOwner
  /\ phase' = "Parked"
  /\ sourceEffects' = 1
  /\ lease' = TRUE
  /\ UNCHANGED <<tailCredit, tailOwner, prefixPublications,
                  presentEffects, commandBuffers, renderPasses,
                  ordinaryPending>>

PublishPresentTail ==
  /\ phase \in {"Parked", "OrdinaryWithPrefix"}
  /\ tailOwner
  /\ phase' = "TailReady"
  /\ presentEffects' = 1
  /\ UNCHANGED <<tailCredit, tailOwner, prefixPublications,
                  sourceEffects, commandBuffers, renderPasses,
                  ordinaryPending, lease>>

JoinAndSubmit ==
  /\ phase = "TailReady"
  /\ phase' = "Submitted"
  /\ commandBuffers' = 1
  /\ renderPasses' = 1
  /\ lease' = FALSE
  /\ UNCHANGED <<tailCredit, tailOwner, prefixPublications,
                  sourceEffects, presentEffects, ordinaryPending>>

StopBeforeJoin ==
  /\ phase \in {"PrefixReady", "Parked", "OrdinaryPending",
                "OrdinaryWithPrefix"}
  /\ phase' = "Stopped"
  /\ lease' = FALSE
  /\ ordinaryPending' = FALSE
  /\ UNCHANGED <<tailCredit, tailOwner, prefixPublications,
                  sourceEffects, presentEffects, commandBuffers,
                  renderPasses>>

Next ==
  \/ EncodeOrdinary
  \/ PublishPrefix
  \/ PublishPrefixBehindOrdinary
  \/ FoldPrefixIntoOrdinary
  \/ FailClosed
  \/ EncodeAndPark
  \/ PublishPresentTail
  \/ JoinAndSubmit
  \/ StopBeforeJoin

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ phase \in Phases
  /\ tailCredit \in BOOLEAN
  /\ tailOwner \in BOOLEAN
  /\ prefixPublications \in 0..1
  /\ sourceEffects \in 0..1
  /\ presentEffects \in 0..1
  /\ commandBuffers \in 0..1
  /\ renderPasses \in 0..1
  /\ ordinaryPending \in BOOLEAN
  /\ lease \in BOOLEAN

TailReservedBeforePrefix ==
  prefixPublications = 1 => tailCredit /\ tailOwner

NoSubmitBeforePresentJoin ==
  phase # "Submitted" => commandBuffers = 0 /\ renderPasses = 0

SubmissionShapeConserved ==
  /\ commandBuffers = renderPasses
  /\ commandBuffers <= presentEffects

ExactlyOnce ==
  /\ prefixPublications <= 1
  /\ sourceEffects <= prefixPublications
  /\ presentEffects <= prefixPublications

LeaseClearedAfterStop ==
  phase = "Stopped" => ~lease

Safety ==
  /\ TypeOK
  /\ TailReservedBeforePrefix
  /\ NoSubmitBeforePresentJoin
  /\ SubmissionShapeConserved
  /\ ExactlyOnce
  /\ LeaseClearedAfterStop

=============================================================================
