---- MODULE QueueT2dReserveCopyCommit ----
(***************************************************************************)
(* Queue T2d reserve/copy/commit preflight model.                         *)
(*                                                                         *)
(* This is deliberately a decision-gate model, not a production switch.  *)
(* The current implementation keeps submitDrawRunBatchImpl's append under *)
(* CommandQueue::mutex_.  The model records the failure modes that a      *)
(* future reservation design must exclude before that lock is narrowed:   *)
(*                                                                         *)
(*  - a publisher observes a half-constructed slot;                      *)
(*  - a stale reservation writes into a recycled slot generation;         *)
(*  - rollback loses the valid prefix already present in the slot.        *)
(*                                                                         *)
(* `Reserve` charges capacity, checkpoints the writing slot's valid       *)
(* prefix, and freezes both identity values.  `Copy` constructs private    *)
(* reservation bytes, outside the queue mutex, without changing the slot. *)
(* `Commit` is the only reservation operation that may publish the         *)
(* checkpointed prefix plus a complete private reservation.  A valid      *)
(* production design must either block publication while a reserve is live *)
(* or use an equivalent ownership token, and must validate generation and   *)
(* frozen ticket before committing.                                       *)
(*                                                                         *)
(* The companion configurations intentionally remove one premise each.    *)
(* They are executable counterexamples, not accepted implementations.      *)
(***************************************************************************)

EXTENDS Naturals, Sequences, TLC

CONSTANTS
  RequiredParts,
  PublishDiscipline,
  ReservationDiscipline,
  CommitDiscipline,
  RollbackDiscipline

PrefixBytes == <<"valid-prefix-byte">>
ReservationByte == "private-reservation-byte"
PayloadBytes == {"valid-prefix-byte", ReservationByte}

VARIABLES
  slotState,
  slotGeneration,
  slotTicket,
  slotOwner,
  nextTicket,
  reservation,
  reservationPrefix,
  reservationGeneration,
  reservationTicket,
  privateReservationBytes,
  slotBytes,
  capacity,
  reservedCapacity,
  publishedBytes,
  commitPublished,
  rollbackOccurred,
  wakeSignaled,
  halfAppendedVisible,
  staleReservationWrite

vars == <<slotState, slotGeneration, slotTicket, slotOwner, nextTicket,
  reservation, reservationPrefix, reservationGeneration,
  reservationTicket, privateReservationBytes, slotBytes, capacity,
  reservedCapacity, publishedBytes, commitPublished, rollbackOccurred,
  wakeSignaled, halfAppendedVisible, staleReservationWrite>>

Init ==
  /\ slotState = "Writing"
  /\ slotGeneration = 1
  /\ slotTicket = 1
  /\ slotOwner = "producer"
  /\ nextTicket = 2
  /\ reservation = FALSE
  /\ reservationPrefix = <<>>
  /\ reservationGeneration = 0
  /\ reservationTicket = 0
  /\ privateReservationBytes = <<>>
  /\ slotBytes = PrefixBytes
  /\ capacity = 1
  /\ reservedCapacity = 0
  /\ publishedBytes = <<>>
  /\ commitPublished = FALSE
  /\ rollbackOccurred = FALSE
  /\ wakeSignaled = FALSE
  /\ halfAppendedVisible = FALSE
  /\ staleReservationWrite = FALSE

(* A writing slot deliberately starts with a valid prefix before Reserve;
   it is not reservation-owned construction and must survive a failure. *)

(* Reserve is the bounded admission step.  It is the only step that charges
   capacity, checkpoints the current prefix, and freezes identity for Commit.
   The slot itself is unchanged. *)
Reserve ==
  /\ slotState = "Writing"
  /\ ~reservation
  /\ ~rollbackOccurred
  /\ reservedCapacity = 0
  /\ reservation' = TRUE
  /\ reservationPrefix' = slotBytes
  /\ reservationGeneration' = slotGeneration
  /\ reservationTicket' = slotTicket
  /\ privateReservationBytes' = <<>>
  /\ reservedCapacity' = 1
  /\ UNCHANGED <<slotState, slotGeneration, slotTicket, slotOwner,
      nextTicket, slotBytes, capacity, publishedBytes, commitPublished,
      rollbackOccurred, wakeSignaled, halfAppendedVisible,
      staleReservationWrite>>

(* Copy models construction into private bounded reservation storage.  It has
   no queue publication effect and may interleave with a force publisher. *)
Copy ==
  /\ reservation
  /\ Len(privateReservationBytes) < RequiredParts
  /\ privateReservationBytes' =
       Append(privateReservationBytes, ReservationByte)
  /\ UNCHANGED <<slotState, slotGeneration, slotTicket, slotOwner,
      nextTicket, reservation, reservationPrefix, reservationGeneration,
      reservationTicket, slotBytes, capacity, reservedCapacity,
      publishedBytes, commitPublished, rollbackOccurred, wakeSignaled,
      halfAppendedVisible, staleReservationWrite>>

(* A correct queue blocks a force publish while a reservation owns the slot.
   The permissive branch exists only in the half-appended counterexample. *)
Publish ==
  /\ slotState = "Writing"
  /\ IF PublishDiscipline = "BlockWhileReserved"
        THEN ~reservation
        ELSE TRUE
  /\ ~reservation => publishedBytes' = slotBytes
  /\ reservation => publishedBytes' =
       IF Len(privateReservationBytes) < RequiredParts
          THEN reservationPrefix \o privateReservationBytes
          ELSE slotBytes
  /\ slotState' = "Published"
  /\ halfAppendedVisible' =
       (halfAppendedVisible \/
        (reservation /\ Len(privateReservationBytes) < RequiredParts))
  /\ UNCHANGED <<slotGeneration, slotTicket, slotOwner, nextTicket,
      reservation, reservationPrefix, reservationGeneration,
      reservationTicket, privateReservationBytes, slotBytes, capacity,
      reservedCapacity, commitPublished, rollbackOccurred, wakeSignaled,
      staleReservationWrite>>

(* A force-publish/recycle actor advances generation only in the broken
   reservation discipline.  This is the stale-token interleaving: Reserve;
   Recycle; Commit.  The production token must make this action impossible. *)
RecycleWhileReserved ==
  /\ reservation
  /\ ReservationDiscipline = "AllowRecycle"
  /\ slotOwner = "producer"
  /\ slotGeneration' = slotGeneration + 1
  /\ slotTicket' = nextTicket
  /\ nextTicket' = nextTicket + 1
  /\ slotOwner' = "next-owner"
  /\ UNCHANGED <<slotState, reservation, reservationPrefix,
      reservationGeneration, reservationTicket, privateReservationBytes,
      slotBytes, capacity, reservedCapacity, publishedBytes,
      commitPublished, rollbackOccurred, wakeSignaled,
      halfAppendedVisible, staleReservationWrite>>

(* Commit validates reservation identity before copying the complete private
   reservation into final queue-owned storage.  An invalid token is excluded
   by the safe discipline; the intentionally unsafe branch records a stale
   write instead. *)
Commit ==
  /\ reservation
  /\ Len(privateReservationBytes) = RequiredParts
  /\ IF CommitDiscipline = "ValidateGeneration"
        THEN /\ reservationGeneration = slotGeneration
             /\ reservationTicket = slotTicket
        ELSE TRUE
  /\ staleReservationWrite' =
       (staleReservationWrite \/
        (reservationGeneration # slotGeneration \/
         reservationTicket # slotTicket))
  /\ slotBytes' = reservationPrefix \o privateReservationBytes
  /\ publishedBytes' = reservationPrefix \o privateReservationBytes
  /\ commitPublished' = TRUE
  /\ slotState' = "Published"
  /\ reservation' = FALSE
  /\ reservedCapacity' = 0
  /\ UNCHANGED <<slotGeneration, slotTicket, slotOwner, nextTicket,
      reservationPrefix, reservationGeneration, reservationTicket,
      privateReservationBytes, capacity, rollbackOccurred, wakeSignaled,
      halfAppendedVisible>>

(* A failed pre-effect construction or a rejected stale reservation restores
   the capacity charge, wakes a waiter, and restores the exact checkpointed
   prefix.  The unsafe branch is the lost-prefix negative control. *)
Rollback ==
  /\ reservation
  /\ ~rollbackOccurred
  /\ reservation' = FALSE
  /\ reservedCapacity' = 0
  /\ slotBytes' =
       IF RollbackDiscipline = "RestorePrefix"
          THEN reservationPrefix
          ELSE <<>>
  /\ privateReservationBytes' = <<>>
  /\ rollbackOccurred' = TRUE
  /\ wakeSignaled' = TRUE
  /\ UNCHANGED <<slotState, slotGeneration, slotTicket, slotOwner,
      nextTicket, reservationPrefix, reservationGeneration,
      reservationTicket, capacity, publishedBytes, commitPublished,
      halfAppendedVisible, staleReservationWrite>>

ObserveHalfAppend ==
  /\ slotState = "Published"
  /\ reservation
  /\ Len(privateReservationBytes) < RequiredParts
  /\ halfAppendedVisible' = TRUE
  /\ UNCHANGED <<slotState, slotGeneration, slotTicket, slotOwner,
      nextTicket, reservation, reservationPrefix, reservationGeneration,
      reservationTicket, privateReservationBytes, slotBytes, capacity,
      reservedCapacity, publishedBytes, commitPublished, rollbackOccurred,
      wakeSignaled, staleReservationWrite>>

Next == Reserve \/ Copy \/ Publish \/ RecycleWhileReserved \/ Commit \/ Rollback \/ ObserveHalfAppend

TypeOK ==
  /\ slotState \in {"Writing", "Published"}
  /\ slotGeneration \in Nat
  /\ slotTicket \in Nat
  /\ nextTicket \in Nat
  /\ reservationPrefix \in Seq(PayloadBytes)
  /\ reservationGeneration \in Nat
  /\ reservationTicket \in Nat
  /\ privateReservationBytes \in Seq(PayloadBytes)
  /\ slotBytes \in Seq(PayloadBytes)
  /\ Len(reservationPrefix) = 0 \/ Len(reservationPrefix) = Len(PrefixBytes)
  /\ Len(privateReservationBytes) \in 0..RequiredParts
  /\ Len(slotBytes) <= Len(PrefixBytes) + RequiredParts
  /\ publishedBytes \in Seq(PayloadBytes)
  /\ commitPublished \in BOOLEAN
  /\ rollbackOccurred \in BOOLEAN
  /\ wakeSignaled \in BOOLEAN

CapacityConserved == reservedCapacity = 1 <=> reservation
NoHalfAppendedSlot == ~halfAppendedVisible
NoStaleReservationWrite == ~staleReservationWrite
CompletePublication ==
  ~commitPublished \/
  Len(privateReservationBytes) = RequiredParts \/
  publishedBytes = reservationPrefix \o privateReservationBytes
ReservationIsolation ==
  ~reservation \/ slotBytes = reservationPrefix
RollbackRestoresCharge == reservedCapacity = 1 <=> reservation
RollbackWakesWaiters == ~rollbackOccurred \/ wakeSignaled
RollbackRestoresPrefix == ~rollbackOccurred \/ slotBytes = reservationPrefix

Spec == Init /\ [][Next]_vars

====
