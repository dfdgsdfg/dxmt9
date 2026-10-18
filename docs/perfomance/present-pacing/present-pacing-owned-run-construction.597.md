---
domain: present-pacing
workload: STALKER H502/r5 original capture and constructor refinement
title: "Owned final-location construction preserves effects but retains initialization work"
type: evidence
status: private-candidate-qualified-live-removal-unmeasured
updated: 2026-10-16
source: traces/stkcop-owned-run-h597/{contract.md,owned_run.hpp,prepare.py,derivation.json,run_type.hpp,native.cpp,native.json,native_growth.cpp,growth-tests.json,build.json,preflight.json,audit_codegen.py,codegen.json,verify.py,verification.json}; traces/stkcop-owned-run-h597/{original-observer1-gate0-perf0,original-observer1-gate1-perf0,original-observer1-gate0-perf1,original-observer1-gate1-perf1}.log
related: present-pacing-replay-cpu-owner.596.md; present-pacing-cache-clock.595.md
---

# Intermediate copies removed; CPU saving not yet established

[H596](present-pacing-replay-cpu-owner.596.md) identified owned-run construction
as a recurrent live CPU owner and confirmed three copies of the 8,112-byte
hot state in the canonical non-growing append path. H597 implements a private
same-image reference/candidate comparison without changing production source,
class definitions, capacity policy, queue ordering or resource lifetime.

## Candidate

Reference retains local `CompatibilityBatchRun` construction followed by
`push_back(std::move(run))`. Candidate uses aggregate `emplace_back` with a
temporary conversion input. That input borrows state/layout/debug only during
construction and returns an **owned** `CanonicalDrawState`; the resulting run
does not retain those references. Uniform and binding fields copy directly
into the final run. Reference-counted streams and shader bytecode stay owned.

The implementation deliberately avoids modifying one private copy of shared
class headers while linking unchanged archives: identical layout alone would
not resolve the resulting C++ ODR violation. Only the private core draw object
changes, and both the dynamic provider and statically linked capture core are
updated. `H597_GATE` selects the private candidate, not a product runtime knob.
A first-entry witness proves candidate execution in exact capture controls;
it is absent in reference controls.

## Refinement and effect evidence

- ARM64 and Rosetta, each with normal and disabled optional copy elision,
  pass 10,368 action transitions and 3,456 injected allocation failures per
  executable. The tests use the actual helper and a hash-bound extraction of
  the production run declaration, not a substitute layout.
- Bounded append/mutate/clear traces test A/B/A source changes, retained values,
  real shared ownership, shader bytes, flat offsets, reserved binding address
  stability and failure before construction. Dropping source references leaves
  run-owned resources alive; clearing both output lists releases them.
- An additional fixture on both architectures injects 32 failures while an
  existing run is retained, checking unchanged size, values, addresses and
  reference counts. Successful subsequent relocation preserves both runs.
- Four original capture controls (candidate OFF/ON, product counters OFF/ON)
  retain exact source/output pixels, seven source identities, query results,
  670 wrapper lifetimes and restoration of normal state. Candidate entry is
  witnessed only when enabled. Four ordinary queue controls preserve completion
  and readback; those skip draws and are not substitutes for the pixel oracle.
- Independent hash/log/code-generation verification passes. No Wine wild run,
  runtime staging, production promotion, commit or CPU saving follows yet.

The refinement changes only synchronous construction within the existing
transaction. It introduces no new interleaving/wake/publication action. Native
exhaustive value/lifetime/failure checks provide this layer; they do not prove
GPU execution, which is covered separately by the original capture controls.

## What generated code actually does

The candidate's non-growing vector branch has one hot-state `memcpy` and one
uniform `memcpy` directly into final storage. However, the default-initialize
then-assign conversion also retains **seven `bzero` call sites plus scalar
initialization stores**. The compiler did not eliminate all overwritten
initialization. Copy counts alone therefore do not price the net improvement.

The fast branch invokes shader-layout assignment as well; the complete cost
includes that ownership work and helper calls, not just the listed memory
intrinsics. No byte-count ratio is converted into a CPU or FPS saving. Capture
control wall times include cold setup/observer/scheduling effects and are not
accepted timing trials.

Next compare the qualified constructor's net replay CPU against reference on
matched input, including remaining initialization, then transfer any removal
to default-live late STALKER. A direct by-value conversion is another possible
code-generation variant if it trades an unavoidable extra copy for avoiding
default initialization; do not select it by static counts alone. More invasive
reference-input constructors would require consistent class definitions across
all linked translation units and new qualification.

The original goal remains open: establish the largest default-live dxmt9
single-thread CPU owner and a correctness-qualified removable amount. H597
adds an effect-qualified candidate, not that final measurement.
