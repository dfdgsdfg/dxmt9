---
domain: present-pacing
workload: Unchanged archived-default STALKER Day, early CPU ancestry feasibility
title: "Native CPU sampling does not recover enough Primary ancestry"
type: evidence
status: finite-feasibility-rejected-no-tail-attribution
updated: 2026-10-08
source: traces/stkcop-default-unwind-h467/{contract.md,run.py,analyze.py}; traces/stkcop-default-unwind-h467/default-r1/{receipt.json,early-trace.log,early.trace,toc.xml,time-sample.xml,time-profile.xml,time-profile-serial.xml,ancestry.json,primary-samples.json}
related: present-pacing-primary-transfer-checkpoint.465.md; present-pacing-positiont-pixel-center.466.md; present-pacing-default-thread-identity.462.md
---

# The alternate ownership measurement route fails its ancestry gate

H467 uses H465's unchanged archived default renderer binaries/settings, with
no entry markers or diagnostic DLL. The run completes normally and restores
shared Wine staging. Native process identity is `[44649,1791385097,867601]`.
H466's corrected provider is not staged into this attribution experiment.

At scene 40.027 s the harness launches a six-second Time Profiler attachment.
The recording succeeds and produces a 19 MiB trace. Its TOC names the expected
process and `time-sample` schema with 1000-microsecond sample rate and
`all-thread-states=NO`. This is an early feasibility test, not the requested
102–110 s tail ownership measurement.

The modeled `time-profile` export terminates with status 133, leaving a
truncated XML file. A single serial export retry also terminates with 133.
Both artifacts are retained. Raw `time-sample` export succeeds:

| Raw observation | Count |
|---|---:|
| All Running / Timer Fired samples | 22,100 |
| Primary Running / Timer Fired samples | 5,589 |
| Primary samples carrying only one frame | 4,373 |
| Primary samples with multiple frames | 1,216 |

The unique profiler thread name is `X-RAY Primary thread`, native TID
`0x39caf26`, in PID 44649. Raw backtraces are addresses, frequently just a
translated PC, not recovered PE/Wine caller chains. Even the multi-frame rows
are not independently classified; their existence is not sufficient ancestry.
The 78.2% single-frame share is a raw sample-count observation, **not** an
estimated renderer CPU fraction. Stackshot/Blocked rows are kept separate.

The predeclared gate requires useful Primary ancestry before enabling the
same-run tail capture. No `allow-tail` authorization file is created, and no
tail trace is taken. Early export analysis was not completed before that
deadline; its eventual result also fails the ancestry requirement. Do not
replace this valid feasibility failure with another game run or a custom
Rosetta unwinder in this experiment.

## Decision

This route does not supply a default Primary renderer bound. Treat unreadable
or leaf-only ancestry as renderer-possible; a Wine/game leaf cannot prove that
the sample lies outside a synchronous D3D9 descendant. Do not extrapolate an
early CPU distribution into the tail, multiply wall-stack occupancy by CPU,
or attach an independent-binomial confidence interval to periodic samples.

H465 and H467 together close two attempted attribution methods, not the
objective. Default Encode remains about 30 ms per matched late frame, Replay
is close in H462, and mixed Primary is still unseparated. The largest default
dxmt9-attributable single-thread owner therefore remains unresolved.

The separate actionable path is to qualify and price H455's real-visibility
intervention using the same H466-corrected provider in both arms. That can
establish a removable per-thread cost without pretending to repair this
ownership inference. No runtime default, README FPS, or goal-completion claim
changes here.
