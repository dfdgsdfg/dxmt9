---
domain: present-pacing
workload: STALKER CoP capture29
title: "Final-image grouping qualification and balanced CPU comparison"
type: experiment
status: bounded-cold-policy-matrix-complete
updated: 2026-10-04
source: experiments/output/stkcop-v4-final-grouping-26/verification.md; experiments/output/stkcop-v4-final-grouping-26/completion-audit.md; experiments/output/stkcop-v4-final-grouping-26/analysis.jsonl; experiments/output/stkcop-v4-final-grouping-26/{paired-l3-r1,paired-l64-r1,l0-aa-r1,l0-l3-r1,l3-l0-r1,l0-l64-r1,l64-l0-r1}/result.json; experiments/output/stkcop-v4-immutable-preparation-24/paired-l0-r1/result.json
related: docs/perfomance/present-pacing/present-pacing-v4-final-capacity.442.md; docs/perfomance/present-pacing/present-pacing-v4-pass-localization.434.md
---

# Final-image grouping matrix

L0/L3/L64 now have paired original CPU semantics and actual-GPU pixel/EVENT
qualification on the same final image and unchanged capture29. The historical
H434 receipts were not reused as current-image authority. New L3/L64 paired
qualifications close the missing CPU gate beside H441's GPU diagnostics.

Five balanced Compatibility comparisons pass 180 fresh execution-only CPU
children: two warm-ups plus 16 measured samples per arm. All headroom values
are zero. No analysis, builds or agent work overlapped these measurements;
external system load and caches remain uncontrolled. Main revalidated all
native receipts and input/image/qualification bindings with the artifact's
read-only `analyze.py`, checked omission/lifecycle/conservation evidence, and
independently recomputed measured medians.

| Ordered arms | Body wall medians ms | Process CPU medians ms |
|---|---:|---:|
| L0 / L0 | 69.489 / 69.452 | 69.926 / 69.836 |
| L0 / L3 | 69.401 / 73.984 | 69.823 / 76.700 |
| L3 / L0 | 74.066 / 69.486 | 76.799 / 69.913 |
| L0 / L64 | 69.539 / 67.225 | 69.922 / 67.981 |
| L64 / L0 | 67.339 / 69.515 | 68.056 / 69.941 |

L3 raises body process CPU by 9.849%/9.850% in forward/reverse order; L64
reduces it by 2.775%/2.694%. Corresponding wall changes are +6.604%/+6.591%
and -3.327%/-3.131%. A/A differences are observations, not confidence bounds.

All policies preserve 2,799 draws, 87 raw acknowledgments and 512 body
commands. Actual body source counts are 3/172/10 for L0/L3/L64. The independent
GPU lane observes lifecycle-wide encode chunk counts 5/174/12, preserves
original source/output bytes and EVENT readiness, and reports zero GPU errors.
More frequent publication has a clear CPU cost here; the moderate L64 CPU
benefit still trades against additional GPU-side partitioning. Neither encode
chunk counts nor this CPU-only timing establish GPU duration or game FPS.

## Completion and boundaries

Together with H442, the final image has 360 accepted observer-light CPU
executions (40 warm-ups, 320 measured samples) across capacity and grouping
A/A/forward/reverse matrices. The bounded query-aware original-input task is
complete for this capture and the declared cold conditional domain; the
completion audit maps its three objectives to implementation and evidence.

The CPU consumer uses immediate-FIFO controlled acknowledgment and assumes
legal NOOVERWRITE with closed external writers. Actual GPU query/pixel
qualification remains a separate authority. Same-device warm reset, an
independent full scalar/shader interpreter, numeric occlusion on this EVENT-only
input, unconditional future-backing preservation, exclusive encoder attribution
and FPS/default promotion are not claimed. No production default changes.
