---- MODULE DrawPsoIdentity ----
(***************************************************************************
 * R-BACK-3.12/3.13/3.15: bounded backend PSO identity canonicalization.
 *
 * This is a value-level refinement, intentionally separate from the queue
 * and Metal-driver models.  It enumerates the distinguishing inputs for the
 * portable/tile FFP alpha identity, general-draw versus stretch filtering,
 * and attached/inactive blend descriptors.  The production pure helpers are
 * exercised by the native truth table; this model keeps the algebra explicit
 * and catches accidental reintroduction of an omitted/retained field.
 ***************************************************************************)

EXTENDS Naturals, TLC

AlphaFunctions == 0..2
Filters == BOOLEAN
Formats == {0, 1}
BlendOps == 0..2

PortableFfpIdentity(alphaEnabled, alphaFunction) == 0
TileFfpIdentity(alphaEnabled, alphaFunction) ==
  <<alphaEnabled, alphaFunction>>
GeneralDrawIdentity(filter) == 0
StretchIdentity(filter) == filter

CanonicalBlend(blendingEnabled, operation, format) ==
  IF format = 0 THEN
    <<FALSE, 0, 0, 0>>
  ELSE IF ~blendingEnabled THEN
    <<FALSE, 0, 0, format>>
  ELSE
    <<TRUE, operation, operation, format>>

VARIABLES alphaEnabled, alphaFunction, filter, blendingEnabled, operation,
          format, portableIdentity, tileIdentity, generalIdentity,
          stretchIdentity, blendIdentity

vars == <<alphaEnabled, alphaFunction, filter, blendingEnabled, operation,
           format, portableIdentity, tileIdentity, generalIdentity,
           stretchIdentity, blendIdentity>>

Init ==
  /\ alphaEnabled = FALSE
  /\ alphaFunction = 0
  /\ filter = FALSE
  /\ blendingEnabled = FALSE
  /\ operation = 0
  /\ format = 0
  /\ portableIdentity = PortableFfpIdentity(alphaEnabled, alphaFunction)
  /\ tileIdentity = TileFfpIdentity(alphaEnabled, alphaFunction)
  /\ generalIdentity = GeneralDrawIdentity(filter)
  /\ stretchIdentity = StretchIdentity(filter)
  /\ blendIdentity = CanonicalBlend(blendingEnabled, operation, format)

Next ==
  \E nextAlphaEnabled \in BOOLEAN,
     nextAlphaFunction \in AlphaFunctions,
     nextFilter \in Filters,
     nextBlendingEnabled \in BOOLEAN,
     nextOperation \in BlendOps,
     nextFormat \in Formats:
    /\ alphaEnabled' = nextAlphaEnabled
    /\ alphaFunction' = nextAlphaFunction
    /\ filter' = nextFilter
    /\ blendingEnabled' = nextBlendingEnabled
    /\ operation' = nextOperation
    /\ format' = nextFormat
    /\ portableIdentity' =
         PortableFfpIdentity(nextAlphaEnabled, nextAlphaFunction)
    /\ tileIdentity' =
         TileFfpIdentity(nextAlphaEnabled, nextAlphaFunction)
    /\ generalIdentity' = GeneralDrawIdentity(nextFilter)
    /\ stretchIdentity' = StretchIdentity(nextFilter)
    /\ blendIdentity' =
         CanonicalBlend(nextBlendingEnabled, nextOperation, nextFormat)

IdentityInvariant ==
  /\ portableIdentity = 0
  /\ tileIdentity = TileFfpIdentity(alphaEnabled, alphaFunction)
  /\ generalIdentity = 0
  /\ stretchIdentity = filter
  /\ blendIdentity = CanonicalBlend(blendingEnabled, operation, format)

PortableAlphaCollapsed ==
  \A a1 \in BOOLEAN, a2 \in BOOLEAN,
     f1 \in AlphaFunctions, f2 \in AlphaFunctions:
    PortableFfpIdentity(a1, f1) = PortableFfpIdentity(a2, f2)

TileAlphaInjective ==
  \A a1 \in BOOLEAN, a2 \in BOOLEAN,
     f1 \in AlphaFunctions, f2 \in AlphaFunctions:
    TileFfpIdentity(a1, f1) = TileFfpIdentity(a2, f2)
      => /\ a1 = a2
         /\ f1 = f2

GeneralFilterCollapsed ==
  \A f1 \in Filters, f2 \in Filters:
    GeneralDrawIdentity(f1) = GeneralDrawIdentity(f2)

StretchFilterInjective ==
  \A f1 \in Filters, f2 \in Filters:
    StretchIdentity(f1) = StretchIdentity(f2) => f1 = f2

InvalidBlendCollapsed ==
  \A enabled \in BOOLEAN, op \in BlendOps:
    CanonicalBlend(enabled, op, 0) = <<FALSE, 0, 0, 0>>

AttachedDisabledBlendCollapsed ==
  \A op1 \in BlendOps, op2 \in BlendOps:
    CanonicalBlend(FALSE, op1, 1) = CanonicalBlend(FALSE, op2, 1)

AttachedEnabledBlendInjective ==
  \A op1 \in BlendOps, op2 \in BlendOps:
    CanonicalBlend(TRUE, op1, 1) = CanonicalBlend(TRUE, op2, 1)
      => op1 = op2

Spec == Init /\ [][Next]_vars

=============================================================================
