---- MODULE DirectPrefixPrepare ----
(* R-BACK-2.107: transaction-local authority, not cross-transaction caching.
   Reserve validates the prefix. An owner-thread-pinned build excludes foreign
   writers even while the queue mutex is released. Commit rechecks live identity
   under the mutex, mints authority and immediately consumes it.
   Two generations distinguish same-address reconstruction. No progress/wake
   behavior changes. Excludes arbitrary aliases violating queue ownership,
   allocator/driver behavior, concrete payload bytes, pixels and CPU savings. *)
EXTENDS Naturals
CONSTANTS AllowForeign, IgnoreGeneration, AllowRepeat, ReopenTerminal
VARIABLES phase, generation, token, consumed, accepts, prefixValid,
          suffixValid, extents, sound, fresh, committed, rolledBack
vars == <<phase, generation, token, consumed, accepts, prefixValid,
          suffixValid, extents, sound, fresh, committed, rolledBack>>

Decision(buildable, present, matches, preserved) ==
  IF ~buildable THEN "Reject"
  ELSE IF ~present THEN "Full"
  ELSE IF matches /\ preserved THEN "Suffix" ELSE "Reject"

Init ==
  /\ phase = "Building" /\ generation = 1 /\ token = 0
  /\ consumed = FALSE /\ accepts = 0
  /\ prefixValid = TRUE /\ suffixValid = TRUE /\ extents = TRUE
  /\ sound = TRUE /\ fresh = TRUE
  /\ committed = FALSE /\ rolledBack = FALSE

Mint ==
  /\ phase = "Building" /\ token = 0
  /\ token' = generation
  /\ UNCHANGED <<phase, generation, consumed, accepts, prefixValid,
                  suffixValid, extents, sound, fresh, committed, rolledBack>>

Append ==
  /\ phase = "Building"
  /\ suffixValid' \in BOOLEAN
  /\ UNCHANGED <<phase, generation, token, consumed, accepts, prefixValid,
                  extents, sound, fresh, committed, rolledBack>>

Shrink ==
  /\ phase = "Building" /\ extents' = FALSE
  /\ UNCHANGED <<phase, generation, token, consumed, accepts, prefixValid,
                  suffixValid, sound, fresh, committed, rolledBack>>

ForeignWrite ==
  /\ AllowForeign /\ phase = "Building" /\ prefixValid' = FALSE
  /\ UNCHANGED <<phase, generation, token, consumed, accepts, suffixValid,
                  extents, sound, fresh, committed, rolledBack>>

Replace ==
  /\ phase = "Building" /\ generation = 1 /\ token = 1
  /\ generation' = 2
  /\ UNCHANGED <<phase, token, consumed, accepts, prefixValid,
                  suffixValid, extents, sound, fresh, committed, rolledBack>>

Prepare ==
  /\ token # 0 /\ accepts < 2
  /\ LET buildable == phase = "Building" \/
                       (AllowRepeat /\ phase = "Prepared")
         matches == (IgnoreGeneration \/ token = generation) /\
                    (~consumed \/ AllowRepeat)
         decision == Decision(buildable, TRUE, matches, extents)
         accepted == decision = "Suffix" /\ suffixValid
     IN /\ phase' = IF accepted THEN "Prepared"
                    ELSE IF phase \in {"Committed", "RolledBack"} /\
                            ~ReopenTerminal THEN phase ELSE "Failed"
        /\ accepts' = IF accepted THEN accepts + 1 ELSE accepts
        /\ sound' = (sound /\ (~accepted \/ (prefixValid /\ suffixValid)))
        /\ fresh' = (fresh /\ (~accepted \/ token = generation))
  /\ consumed' = TRUE
  /\ UNCHANGED <<generation, token, prefixValid, suffixValid, extents,
                  committed, rolledBack>>

Commit ==
  /\ phase = "Prepared" /\ phase' = "Committed" /\ committed' = TRUE
  /\ UNCHANGED <<generation, token, consumed, accepts, prefixValid,
                  suffixValid, extents, sound, fresh, rolledBack>>

Rollback ==
  /\ phase \in {"Building", "Prepared", "Failed"}
  /\ phase' = "RolledBack" /\ rolledBack' = TRUE
  /\ UNCHANGED <<generation, token, consumed, accepts, prefixValid,
                  suffixValid, extents, sound, fresh, committed>>

Next == Mint \/ Append \/ Shrink \/ ForeignWrite \/ Replace \/ Prepare \/
        Commit \/ Rollback
TypeOK ==
  /\ phase \in {"Building", "Prepared", "Committed", "Failed", "RolledBack"}
  /\ generation \in 1..2 /\ token \in 0..2 /\ accepts \in 0..2
  /\ consumed \in BOOLEAN /\ prefixValid \in BOOLEAN
  /\ suffixValid \in BOOLEAN /\ extents \in BOOLEAN
  /\ sound \in BOOLEAN /\ fresh \in BOOLEAN
  /\ committed \in BOOLEAN /\ rolledBack \in BOOLEAN
ReferenceSound == sound
IdentityFresh == fresh
ConsumeOnce == accepts <= 1
TerminalStable == ~(committed /\ rolledBack)
Spec == Init /\ [][Next]_vars
====
