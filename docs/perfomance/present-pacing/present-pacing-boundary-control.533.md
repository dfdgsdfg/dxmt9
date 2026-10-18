---
domain: present-pacing
workload: native multi-worker counter fixture and PE32/PE64 D3D9 control fixture
title: "Suspend-counter gate fails on a sleeper; boundary-only self reports provide a control"
type: evidence
status: quiescent-route-rejected-boundary-control-qualified
updated: 2026-10-14
source: traces/stkcop-native-endpoint-h532/{contract.md,observer.c,target.c,run_native.py,native-receipt.json,analyze_native.py,native-analysis.json,inspect_failure.py,failure-analysis.json}; traces/stkcop-boundary-control-h533/{contract.md,marker_tail.hpp,layout.hpp,fixture.hpp,build.py,build.json,run.py,run.json,selected-slots.json,epoch.hpp,control.cpp,spec.cpp,verify.py,verification.json}; traces/stkcop-boundary-control-h533/{i686,x86_64}-{baseline,diagnostic}/stdout.txt
related: present-pacing-native-endpoint.531.md; present-pacing-root-wall-live.525.md; present-pacing-self-counter-boundary.513.md
---

# Reject the suspension gate without removing the sleeping worker

H532 adds prewarmed kernel entry points, four selected native workers and
partial-failure release injection to H530/H531's endpoint prototype. Both
architectures complete 128 four-worker rounds and recover from failure after
the first, second, third and fourth successful stop. Every stopped worker has
a successful resume receipt and all four respond to the next self query.

The **counter gate fails**: the periodically sleeping worker advances by up to
3 us on ARM64 and 5 us on Rosetta between held reads, exceeding the frozen 2 us
allowance in seven cases. Every nonzero held change belongs to worker 3 (the
sleeper); the busy mutex-holder and both allocator-churn workers remain stable.
All 523,264 chronological CPU/wall comparisons per architecture pass, but that
does not waive the failed held-counter requirement. The raw data has no
user/system split, so kernel wake/accounting is a hypothesis, not attribution.

Replay and Encode can sleep. Excluding the sleeper, enlarging the allowance
after collection or repeating until a clean result would not qualify them.
The suspension route is not promoted to live CPU attribution. The planned
H532 Wine build/run is not executed; its copied runner is unfinished scratch.

## A control without draw/setter clocks

H533 returns to H513-qualified self queries. An isolated PE entry relink patches
only the three Present forms and typed COM-output methods needed to discover
returned objects: 69 slots across the existing COM/validator tables. All other
entries retain their exact original function pointers and original table
address point. Installation checks every untouched slot against its saved
pointer. In particular Draw, Set and GetRenderState do not traverse an observer
thunk. The fixture exercises 20,000 getters with zero additional reports or
clock reads.

Successful Present reports query the actual calling thread with H518's native
PID/birth/TID and wall brackets. There are no RDTSC/root/depth/elapsed counters.
A Present invoked inside an application callback is reported as a whole-thread
checkpoint, not reclassified as non-renderer or outer-root CPU. The new map
magic prevents root-wall parsers from accepting these zero-root fields.

| Actual-object result | PE32 | PE64 |
|---|---:|---:|
| Baseline byte-identical to canonical | Yes | Yes |
| Baseline and control fixture exits | 0 / 0 | 0 / 0 |
| Attachment epoch at exercised device | 4 | 4 |
| Native identity binds | 1 | 1 |
| Self reports: three Present forms plus callback | 4 | 4 |
| Root/elapsed clock reads | 0 | 0 |
| Getter-induced additional reports | 0 | 0 |

All COM output discovery, vtable address preservation and fixture behavior
remain checked. Forty-eight parser mutations reject; the adapted epoch receipt
passes 31 native cases on each architecture. Five pre-existing shared renderer
files are hash-restored, three temporary services recovered, no fixture remains.

This control is **not an observer-free canonical binary**. It still wraps cold
object-returning methods and successful Present, and the intended comparison
uses the same sparse diagnostic worker reporter in both arms. Its value is
removing the all-root observer from the control, not pretending the residual
instrumentation is free. Matched CPU/work/cadence comparison and eventual
canonical transfer remain separate gates. No product optimization or savings
claim follows from these fixtures.
