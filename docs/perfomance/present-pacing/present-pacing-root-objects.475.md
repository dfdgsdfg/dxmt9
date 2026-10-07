---
domain: present-pacing
workload: Actual PE32/PE64 D3D9 objects with inclusive caller CPU scopes
title: "CPU-clock interposition preserves actual object identity and private vtable patches"
type: evidence
status: actual-object-closure-qualified-live-window-attribution-open
updated: 2026-10-08
source: traces/stkcop-root-objects-h475/{contract.md,build.py,build.json,marker.hpp,interposer.hpp,generate.py,entry.generated.hpp,tables.generated.hpp,manifest.json,objects.cpp,run.py,run.json,analyze.py,summary.json}; traces/stkcop-root-objects-h475/{i686-baseline,i686-diagnostic,x86_64-baseline,x86_64-diagnostic}/{stdout.txt,stderr.txt}; traces/stkcop-root-objects-h475/{restore,recovered}
related: present-pacing-root-observer.474.md; present-pacing-primary-inplace-bound.460.md; present-pacing-visibility-budget.468.md
---

# Actual-object gate passes; this is not a game timing result

H475 connects H474's unchanged CPU accumulator to H460's original-vtable-
address-point interposer. PE32 and PE64 each complete one frozen-default
actual-object helper and one diagnostic parent/child COW helper. No STALKER
run, runtime default change or largest-owner/removable-budget claim follows.

## Preserved runtime properties

The diagnostic child exercises four sequential DLL incarnations. The final
incarnation installs all sixteen concrete COM/validator tables and observes
all twelve public exports (`tables=0xffff`, `exports=0xfff`). It preserves
IUnknown/base/Ex identity, typed returned interfaces and containers, cached
zero-reference surface resurrection and self-destroying final Release. This
is closure/ABI evidence, not execution coverage of every COM method.

The full-object incarnation records 173 scope entries but only 107 outer
roots and 214 observer clock reads in each architecture. Nested calls do not
create independent CPU roots. Every completed checkpoint is inactive and
fault-free; native/guest identity stays constant across the four distinct
attachment slots. Final mappings show no live image, coverage failure or
overflow. The parent compares its entire `.rdata` before/after the diagnostic
child and finds it unchanged: the child's table patch remains process-private.

All four runs log successful ABI negotiation with hash
`0xabe19c5ff74245b3`. No current-run error/failure/fault token appears in stderr.
No GPU scene/pixel equality is asserted by this object-lifetime test.

## Build and staging provenance

Only a diagnostic entry translation unit is newly compiled for each
architecture; existing canonical frontend archives/objects and the provider
are reused. No production source, generated bridge or canonical output is
edited. All twelve build commands succeed without warnings, and recorded
archive/object hashes remain unchanged after the tests.

Relinking the original entry object produces a different whole-file hash
from the H468 frozen default. Structural PE parsing and full byte comparison
show that only the COFF and debug-directory timestamp DWORDs differ; code,
imports, layout, PDB identity and every other byte agree. The runtime control
uses the original frozen H468 DLL, not that relink.

Shared Wine initially contains an older five-image set. Each test therefore
stages the complete H468 set, replacing only `d3d9.dll` for the diagnostic
lane. The actual five prelaunch files are backed up and restored after both
helpers and their children are terminal; all final hashes match the prelaunch
values. The three newly staged clock-service files and two app-local discovery
stubs are moved to `recovered/`. No user or renderer file is deleted.

## Remaining scope

The measured ancestry encloses original function entry/return code and
synchronous Wine/Unix work, but also includes diagnostic work and synchronous
callbacks. It is not a PE-exclusive instruction classification. These cold
helper CPU totals must not be extrapolated to per-frame STALKER costs.

H475 reads mappings at quiescent checkpoints. A concurrent game reader still
needs sequence-qualified snapshots of PE32 64-bit counters, slot-fault and
attachment checks, and explicit treatment of roots crossing the measured
window edges. After that, a finite default-policy comparison must establish
the relative Primary/Replay/Encode CPU intervals and observer/default transfer.
The original maximum-owner and correctness-qualified removable-amount clauses
remain open; H468/H469's negative and indeterminate results remain unchanged.
