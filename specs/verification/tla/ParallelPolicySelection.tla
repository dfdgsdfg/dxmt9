---- MODULE ParallelPolicySelection ----
(***************************************************************************
 * R-BACK-2.68--2.75 / R-VERIF-2.16--2.22 bounded policy selection model.
 *
 * The C++ selector fails closed for any invalid plan/economics/overflow
 * candidate, so this model makes one InjectInvalid choice invalidate the
 * complete candidate batch.  Benefit zero is deliberately per-candidate and
 * may be skipped while another safe candidate wins.  candidateKey is the
 * documented scalar abstraction of the C++ lexicographic source-qualified
 * range-key vector; native tests own the complete vector comparison.
 ****************************************************************************)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS CandidateCount, ScoreMax, ChildMax, RangeKeyMax

Candidates == 1 .. CandidateCount
Scores == 0 .. ScoreMax
Children == 1 .. ChildMax
RangeKeys == 1 .. RangeKeyMax

VARIABLES benefit, childCount, candidateOrdinal, candidateKey,
          invalidCandidate, invalidKind, phase, selected, selectedProof,
          effect, parentEnded, completion

Phases == {"Ready", "Selected", "Serial", "Effect", "Joined", "Complete"}
InvalidKinds == 0 .. 5

OrdinalsDistinct ==
  \A a, b \in Candidates : a # b => candidateOrdinal[a] # candidateOrdinal[b]

AllCandidatesValid == invalidCandidate = 0 /\ invalidKind = 0 /\ OrdinalsDistinct

Eligible(c) == AllCandidatesValid /\ benefit[c] > 0

Better(a, b) ==
  benefit[a] > benefit[b] \/
  (benefit[a] = benefit[b] /\ childCount[a] < childCount[b]) \/
  (benefit[a] = benefit[b] /\ childCount[a] = childCount[b] /\
   candidateKey[a] < candidateKey[b]) \/
  (benefit[a] = benefit[b] /\ childCount[a] = childCount[b] /\
   candidateKey[a] = candidateKey[b] /\ candidateOrdinal[a] < candidateOrdinal[b])

Winners == IF AllCandidatesValid
  THEN {c \in Candidates : Eligible(c) /\
    \A other \in Candidates : Eligible(other) => Better(c, other) \/ c = other}
  ELSE {}

vars == <<benefit, childCount, candidateOrdinal, candidateKey,
          invalidCandidate, invalidKind, phase, selected, selectedProof,
          effect, parentEnded, completion>>

Init ==
  /\ benefit = [c \in Candidates |-> IF c = CandidateCount THEN 0 ELSE ScoreMax]
  /\ childCount = [c \in Candidates |-> 2]
  /\ candidateOrdinal = [c \in Candidates |-> c - 1]
  /\ candidateKey = [c \in Candidates |-> c]
  /\ invalidCandidate = 0
  /\ invalidKind = 0
  /\ phase = "Ready"
  /\ selected = 0
  /\ selectedProof = FALSE
  /\ effect = FALSE
  /\ parentEnded = FALSE
  /\ completion = FALSE

(* InvalidKind values abstract malformed/unsafe plan, overflow, invalid cost,
   invalid ordinal, duplicate ordinal, and an unspecified invalid proof. *)
InjectInvalid ==
  /\ phase = "Ready"
  /\ invalidCandidate = 0
  /\ \E c \in Candidates, k \in InvalidKinds \ {0} :
       /\ invalidCandidate' = c
       /\ invalidKind' = k
  /\ UNCHANGED <<benefit, childCount, candidateOrdinal, candidateKey,
                 phase, selected, selectedProof, effect, parentEnded,
                 completion>>

SelectCandidate ==
  /\ phase = "Ready"
  /\ invalidCandidate = 0
  /\ IF Winners = {}
     THEN /\ phase' = "Serial"
          /\ selected' = 0
          /\ selectedProof' = FALSE
     ELSE /\ phase' = "Selected"
          /\ selected' = CHOOSE c \in Winners : TRUE
          /\ selectedProof' = TRUE
  /\ UNCHANGED <<benefit, childCount, candidateOrdinal, candidateKey,
                 invalidCandidate, invalidKind, effect, parentEnded,
                 completion>>

SelectInvalidCandidate ==
  /\ phase = "Ready"
  /\ invalidCandidate # 0
  /\ phase' = "Serial"
  /\ selected' = 0
  /\ selectedProof' = FALSE
  /\ UNCHANGED <<benefit, childCount, candidateOrdinal, candidateKey,
                 invalidCandidate, invalidKind, effect, parentEnded,
                 completion>>

BeginSelectedEffect ==
  /\ phase = "Selected"
  /\ selected \in Candidates
  /\ Eligible(selected)
  /\ selectedProof
  /\ ~effect
  /\ phase' = "Effect"
  /\ effect' = TRUE
  /\ UNCHANGED <<benefit, childCount, candidateOrdinal, candidateKey,
                 invalidCandidate, invalidKind, selected, selectedProof,
                 parentEnded, completion>>

FinishSerial ==
  /\ phase = "Serial"
  /\ ~effect
  /\ phase' = "Joined"
  /\ UNCHANGED <<benefit, childCount, candidateOrdinal, candidateKey,
                 invalidCandidate, invalidKind, selected, selectedProof,
                 effect, parentEnded, completion>>

JoinSelected ==
  /\ phase = "Effect"
  /\ effect
  /\ phase' = "Joined"
  /\ UNCHANGED <<benefit, childCount, candidateOrdinal, candidateKey,
                 invalidCandidate, invalidKind, selected, selectedProof,
                 effect, parentEnded, completion>>

EndParent ==
  /\ phase = "Joined"
  /\ ~parentEnded
  /\ parentEnded' = TRUE
  /\ UNCHANGED <<benefit, childCount, candidateOrdinal, candidateKey,
                 invalidCandidate, invalidKind, phase, selected, selectedProof,
                 effect, completion>>

Complete ==
  /\ phase = "Joined"
  /\ parentEnded
  /\ ~completion
  /\ phase' = "Complete"
  /\ completion' = TRUE
  /\ UNCHANGED <<benefit, childCount, candidateOrdinal, candidateKey,
                 invalidCandidate, invalidKind, selected, selectedProof,
                 effect, parentEnded>>

Next == InjectInvalid \/ SelectCandidate \/ SelectInvalidCandidate \/
        BeginSelectedEffect \/ FinishSerial \/ JoinSelected \/ EndParent \/ Complete

Fairness ==
  /\ WF_vars(InjectInvalid)
  /\ WF_vars(SelectCandidate)
  /\ WF_vars(SelectInvalidCandidate)
  /\ WF_vars(BeginSelectedEffect)
  /\ WF_vars(FinishSerial)
  /\ WF_vars(JoinSelected)
  /\ WF_vars(EndParent)
  /\ WF_vars(Complete)

Spec == Init /\ [][Next]_vars /\ Fairness

TypeOK ==
  /\ benefit \in [Candidates -> Scores]
  /\ childCount \in [Candidates -> Children]
  /\ candidateOrdinal \in [Candidates -> 0 .. CandidateCount - 1]
  /\ candidateKey \in [Candidates -> RangeKeys]
  /\ invalidCandidate \in 0 .. CandidateCount
  /\ invalidKind \in InvalidKinds
  /\ phase \in Phases
  /\ selected \in 0 .. CandidateCount
  /\ selectedProof \in BOOLEAN
  /\ effect \in BOOLEAN
  /\ parentEnded \in BOOLEAN
  /\ completion \in BOOLEAN

SelectionIsSafe == phase = "Selected" => selected \in Candidates /\ Eligible(selected)
SelectionIsArgmax == phase = "Selected" => selected \in Winners
NoEffectBeforeSelection == effect => phase \in {"Effect", "Joined", "Complete"} /\ selectedProof
SerialFallbackHasNoParallelEffect == phase = "Serial" => ~effect /\ selected = 0
SelectedProofOnlyEffect == effect => selectedProof /\ selected \in Candidates /\ Eligible(selected)
JoinParentCompletion == completion => parentEnded /\ phase = "Complete"
EventuallySettles == <> (phase = "Complete")
SelectedEventuallySettles ==
  (phase = "Selected") ~> (phase = "Joined" \/ phase = "Complete")
SerialEventuallySettles ==
  (phase = "Serial") ~> (phase = "Joined" \/ phase = "Complete")

=============================================================================
