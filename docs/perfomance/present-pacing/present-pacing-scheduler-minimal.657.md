---
domain: present-pacing
workload: native Rosetta CPU fixture with external ARM64 marker
title: "H657 — minimal scheduler instruments preserve external-clock CPU enclosure"
type: evidence
status: native-qualified-live-coverage-open
updated: 2026-10-17
source: traces/stkcop-scheduler-minimal-h657/{contract.md,run.py,analyze.py,stream_states.py,verify.py,analysis.json,verification.json}; traces/stkcop-scheduler-minimal-h657/run0/{receipt.json,target.jsonl,marker.jsonl,toc.xml,thread-state.xml,thread-info.xml,os-signpost.xml,record.log}
related: present-pacing-scheduler-full.656.md; present-pacing-external-clock.654.md
---

# H657 — remove unused instruments before another game capture

One5s native fixture uses only the installed `Thread Activity` and `os_signpost`
instruments, which xctrace hosts in a Blank template. No System Trace template,
rolling window, game launch, runtime rebuild or game-DLL patch is used. The
frozen H653 Rosetta target and H654 ARM64 marker are reused unchanged.

Record/save and all three required exports succeed. The saved trace is245MiB;
the thread-state XML is149MiB. This is not a controlled storage/overhead comparison
with H654: host workload and collection contents differ.

| Check | Result |
|---|---:|
| External clock offset aperture | 2334ns |
| Target self CPU | 300207000ns |
| Helper-only mapped possible CPU | 300211917ns |
| Running-only CPU | 299351840ns |
| Unknown inside mapped busy window | 0ns |
| Target thread census | 4 |
| Streaming decoder native equivalence cases | 4 |
| Rejected malformed-stream cases | 6 |
| Verified artifact hashes | 569 |

The external/target offset intervals intersect. Running alone still undercounts;
Running+Interrupted+Unknown contains self CPU. Verification peak RSS is347979776
bytes and the repeated streaming decode takes7.56s. No system-wide XML DOM is
constructed for thread-state analysis.

This qualifies the minimal collection route on the native fixture only. It does
not certify long-duration or STALKER collection, losslessness, observer-OFF
transfer, default largest-owner ranking or CPU savings. A later game population
must still retain both external marker triplets, the entire selected corridor,
all target identities and source evidence for any attributed dxmt9 work.
