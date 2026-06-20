---- MODULE EncodeSessionCompletion ----
(*
 * dxmt9 EncodeSession completion refinement.
 *
 * Models R-BACK-2.49: one Metal session tail can back multiple consecutive
 * source seqIds. The Metal completion expands into ordered per-source
 * completion events; no source seqId can complete before the Metal tail that
 * contains its commands completes.
 *)

EXTENDS Naturals, Sequences, TLC

CONSTANTS MaxSeqId, MaxSessionLen

SeqIds == 1 .. MaxSeqId
SeqId0 == 0 .. MaxSeqId
Session == [sources : Seq(SeqIds), presentSeq : SeqId0]

VARIABLES
  nextSeq,
  lastCommittedSeqId,
  completedSeqId,
  presentCompletedSeqId,
  presentSeqs,
  activeSources,
  gpuSessions,
  completionQueue,
  metalCompletedSeqs

vars ==
  << nextSeq,
     lastCommittedSeqId,
     completedSeqId,
     presentCompletedSeqId,
     presentSeqs,
     activeSources,
     gpuSessions,
     completionQueue,
     metalCompletedSeqs >>

SeqSet(seq) == {seq[i] : i \in DOMAIN seq}

IsConsecutive(seq) ==
  \A i \in DOMAIN seq :
    i < Len(seq) => seq[i + 1] = seq[i] + 1

LastSeq(seq) == seq[Len(seq)]

SessionOK(session) ==
  /\ session \in Session
  /\ Len(session.sources) > 0
  /\ Len(session.sources) <= MaxSessionLen
  /\ IsConsecutive(session.sources)
  /\ session.presentSeq = 0
     \/ session.presentSeq = LastSeq(session.sources)

Init ==
  /\ nextSeq = 1
  /\ lastCommittedSeqId = 0
  /\ completedSeqId = 0
  /\ presentCompletedSeqId = 0
  /\ presentSeqs = {}
  /\ activeSources = <<>>
  /\ gpuSessions = <<>>
  /\ completionQueue = <<>>
  /\ metalCompletedSeqs = {}

StartSessionHead ==
  /\ activeSources = <<>>
  /\ nextSeq <= MaxSeqId
  /\ activeSources' = <<nextSeq>>
  /\ lastCommittedSeqId' = nextSeq
  /\ nextSeq' = nextSeq + 1
  /\ UNCHANGED << completedSeqId,
                  presentCompletedSeqId,
                  presentSeqs,
                  gpuSessions,
                  completionQueue,
                  metalCompletedSeqs >>

AppendSessionHead ==
  /\ activeSources # <<>>
  /\ Len(activeSources) < MaxSessionLen
  /\ nextSeq <= MaxSeqId
  /\ activeSources' = Append(activeSources, nextSeq)
  /\ lastCommittedSeqId' = nextSeq
  /\ nextSeq' = nextSeq + 1
  /\ UNCHANGED << completedSeqId,
                  presentCompletedSeqId,
                  presentSeqs,
                  gpuSessions,
                  completionQueue,
                  metalCompletedSeqs >>

SubmitSessionTail ==
  /\ activeSources # <<>>
  /\ Len(activeSources) < MaxSessionLen
  /\ nextSeq <= MaxSeqId
  /\ LET sources == Append(activeSources, nextSeq) IN
       /\ gpuSessions' =
            Append(gpuSessions,
                   [sources |-> sources, presentSeq |-> nextSeq])
       /\ activeSources' = <<>>
  /\ presentSeqs' = presentSeqs \cup {nextSeq}
  /\ lastCommittedSeqId' = nextSeq
  /\ nextSeq' = nextSeq + 1
  /\ UNCHANGED << completedSeqId,
                  presentCompletedSeqId,
                  completionQueue,
                  metalCompletedSeqs >>

FailOpenPrefixSubmit ==
  /\ activeSources # <<>>
  /\ gpuSessions' =
       Append(gpuSessions,
              [sources |-> activeSources, presentSeq |-> 0])
  /\ activeSources' = <<>>
  /\ UNCHANGED << nextSeq,
                  lastCommittedSeqId,
                  completedSeqId,
                  presentCompletedSeqId,
                  presentSeqs,
                  completionQueue,
                  metalCompletedSeqs >>

SubmitSingleNonPresent ==
  /\ activeSources = <<>>
  /\ nextSeq <= MaxSeqId
  /\ gpuSessions' =
       Append(gpuSessions,
              [sources |-> <<nextSeq>>, presentSeq |-> 0])
  /\ lastCommittedSeqId' = nextSeq
  /\ nextSeq' = nextSeq + 1
  /\ UNCHANGED << completedSeqId,
                  presentCompletedSeqId,
                  presentSeqs,
                  activeSources,
                  completionQueue,
                  metalCompletedSeqs >>

SubmitSinglePresent ==
  /\ activeSources = <<>>
  /\ nextSeq <= MaxSeqId
  /\ gpuSessions' =
       Append(gpuSessions,
              [sources |-> <<nextSeq>>, presentSeq |-> nextSeq])
  /\ presentSeqs' = presentSeqs \cup {nextSeq}
  /\ lastCommittedSeqId' = nextSeq
  /\ nextSeq' = nextSeq + 1
  /\ UNCHANGED << completedSeqId,
                  presentCompletedSeqId,
                  activeSources,
                  completionQueue,
                  metalCompletedSeqs >>

MetalSessionComplete ==
  /\ Len(gpuSessions) > 0
  /\ completionQueue' =
       completionQueue \o Head(gpuSessions).sources
  /\ metalCompletedSeqs' =
       metalCompletedSeqs \cup SeqSet(Head(gpuSessions).sources)
  /\ gpuSessions' = Tail(gpuSessions)
  /\ UNCHANGED << nextSeq,
                  lastCommittedSeqId,
                  completedSeqId,
                  presentCompletedSeqId,
                  presentSeqs,
                  activeSources >>

DrainCompletion ==
  /\ Len(completionQueue) > 0
  /\ Head(completionQueue) = completedSeqId + 1
  /\ completedSeqId' = Head(completionQueue)
  /\ completionQueue' = Tail(completionQueue)
  /\ presentCompletedSeqId' =
       IF Head(completionQueue) \in presentSeqs
       THEN Head(completionQueue)
       ELSE presentCompletedSeqId
  /\ UNCHANGED << nextSeq,
                  lastCommittedSeqId,
                  presentSeqs,
                  activeSources,
                  gpuSessions,
                  metalCompletedSeqs >>

Next ==
  \/ StartSessionHead
  \/ AppendSessionHead
  \/ SubmitSessionTail
  \/ FailOpenPrefixSubmit
  \/ SubmitSingleNonPresent
  \/ SubmitSinglePresent
  \/ MetalSessionComplete
  \/ DrainCompletion

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ MaxSessionLen \in Nat \ {0}
  /\ nextSeq \in 1 .. (MaxSeqId + 1)
  /\ lastCommittedSeqId \in SeqId0
  /\ completedSeqId \in SeqId0
  /\ presentCompletedSeqId \in SeqId0
  /\ presentSeqs \subseteq SeqIds
  /\ activeSources \in Seq(SeqIds)
  /\ gpuSessions \in Seq(Session)
  /\ completionQueue \in Seq(SeqIds)
  /\ metalCompletedSeqs \subseteq SeqIds

CommittedWaterlineOK ==
  /\ lastCommittedSeqId = nextSeq - 1
  /\ completedSeqId <= lastCommittedSeqId
  /\ presentCompletedSeqId <= completedSeqId
  /\ \A s \in presentSeqs : s <= lastCommittedSeqId

ActiveSourcesOK ==
  \/ activeSources = <<>>
  \/ /\ Len(activeSources) <= MaxSessionLen
     /\ IsConsecutive(activeSources)
     /\ \A s \in SeqSet(activeSources) : s > completedSeqId

QueuedSessionsOK ==
  \A i \in DOMAIN gpuSessions :
    /\ SessionOK(gpuSessions[i])
    /\ \A s \in SeqSet(gpuSessions[i].sources) :
         /\ s > completedSeqId
         /\ s <= lastCommittedSeqId

CompletionQueueOK ==
  /\ IsConsecutive(completionQueue)
  /\ (Len(completionQueue) = 0
      \/ Head(completionQueue) = completedSeqId + 1)
  /\ \A s \in SeqSet(completionQueue) :
       /\ s > completedSeqId
       /\ s \in metalCompletedSeqs

NoInlineCompletionOfSessionSources ==
  /\ \A s \in SeqSet(activeSources) : s > completedSeqId
  /\ \A i \in DOMAIN gpuSessions :
       \A s \in SeqSet(gpuSessions[i].sources) :
         s > completedSeqId
  /\ \A s \in 1 .. completedSeqId : s \in metalCompletedSeqs

PresentCompletionAfterTail ==
  /\ presentCompletedSeqId = 0 \/ presentCompletedSeqId \in presentSeqs
  /\ presentCompletedSeqId = 0
     \/ presentCompletedSeqId \in metalCompletedSeqs
  /\ presentCompletedSeqId <= completedSeqId

OrderedCompletionExpansion ==
  /\ ActiveSourcesOK
  /\ QueuedSessionsOK
  /\ CompletionQueueOK

Inv ==
  /\ TypeOK
  /\ CommittedWaterlineOK
  /\ OrderedCompletionExpansion
  /\ NoInlineCompletionOfSessionSources
  /\ PresentCompletionAfterTail

====
