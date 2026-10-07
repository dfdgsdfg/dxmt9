---
domain: present-pacing
workload: ARM host live reads of frozen PE32/PE64 caller CPU accumulators
title: "Coherent inactive snapshots delimit whole-root CPU intervals"
type: evidence
status: live-reader-fixture-qualified-game-join-open
updated: 2026-10-08
source: traces/stkcop-root-reader-h476/{contract.md,reader.c,run.py,analyze.py,receipt.json,reader-disassembly.txt,i686-samples.json,x86_64-samples.json}; traces/stkcop-root-reader-h476-r2/{reader.c,run.py,executed-runner.py,analyze.py,executed-analyzer.py,receipt.json,summary.json,reader-disassembly.txt,i686-samples.json,x86_64-samples.json,i686.jsonl,x86_64.jsonl}; traces/stkcop-root-reader-h476-r2/recovered
related: present-pacing-root-observer.474.md; present-pacing-root-objects.475.md
---

# Live reading is a separate gate from same-thread checkpoints

An external ARM reader observes H474's frozen PE32/PE64 scope fixtures through
their shared file mappings. It never executes on the observed guest thread.
It accepts a slot only with matching nonzero even sequence values around all
128 bytes, an unchanged header and exactly one matching current-incarnation
guest TID. Missing, malformed, overflowed or duplicate mappings are unusable,
not zero CPU. A coherent slot fault has a separate rejected status.

Coherent active snapshots can describe state, but only coherent **inactive**
snapshots may delimit a whole-root CPU difference. This avoids attributing the
entire eventual completion of a root that began before a selected endpoint.
Endpoints retain observer monotonic begin/end envelopes and must share exact
process creation, guest/native thread, attachment and slot identities.

## Finite fixture results

Seven constructed host-memory cases cover inactive/active snapshots and
rejection of odd sequence, slot fault, map overflow, duplicate slot and wrong
incarnation. The concurrent fixture additionally checks every accepted sample
against the writer's publication algebra, monotone per-slot counters and its
final recorded identities. Reload allocates new slots; counters are not joined
across epochs. Intentional final capacity exhaustion remains rejected.

| Corrected reader | Unusable observations | Coherent active | Coherent inactive | Inactive intervals / positive deltas |
|---|---:|---:|---:|---:|
| PE32 writer | 747 | 1,792 | 43 | 41 / 36 |
| PE64 writer | 66 | 1,688 | 46 | 44 / 42 |

These populations are not occupancy or workload-cost estimates: the writer
deliberately sleeps inside roots, reloads and finally injects overflow. The
reader polls at approximately 200 us and itself perturbs this diagnostic
fixture. No game or performance comparison is run.

## Emitted barrier correction

The first reader's live algebra checks pass, but disassembly shows that Apple
inline assembly treats the semicolon in `"dsb ish; isb"` as a comment: only
DSB is emitted. Its samples, source, receipt and assembly are retained. The
corrected reader uses a newline separator and is rebuilt/retested in the
separate `h476-r2` directory. Both DSB and ISB are present in the final image.
Clang 21 emits LDAPR for acquire loads, rather than LDAR; this is retained and
recognized explicitly, not mistaken for a relaxed load.

The explicit pre/post-copy DSB with compiler memory clobbers is the essential
shared-data ordering barrier. Missing ISB was an emitted-instruction mismatch,
not an observed torn-snapshot counterexample. Do not turn the empirical pass
into proof of all ARM/Rosetta interleavings.

Both corrected helpers exit normally. All five newly staged clock-service
copies/stubs are recovered, and the five existing renderer images remain
byte-identical before/after. No source/default/ABI change or file deletion.

## Game integration constraint

A subsequent finite game comparison must join accepted inactive endpoints to
completed-frame/camera evidence and bracket the other native workers' CPU
inside/outside those endpoint envelopes. Inspect every relevant slot fault;
map-level coverage alone does not detect a partial faulted total. Reject
unqualified or overlapping owner intervals rather than forcing a ranking.

H476 establishes the bounded reader fixture, not the native worker join,
default-path observer transfer or correctness-qualified removable CPU. Those
clauses of the original STALKER objective remain open.
