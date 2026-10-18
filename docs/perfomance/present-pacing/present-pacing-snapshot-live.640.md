---
domain: present-pacing
workload: STALKER Day high late camera corridor
title: "H640 — live shared-snapshot value qualification, not live savings"
type: evidence
status: live-value-qualified-default-cost-and-removal-open
updated: 2026-10-16
source: traces/stkcop-snapshot-live-h640/{contract.md,qualified/build.json,native/results.json,oracle/runs.json,verification.json}; traces/stkcop-snapshot-live-h640/Q0/{receipt.json,audit.bin,catalogue-result.json,post-tail.png}; traces/stkcop-snapshot-live-h640/live-retry/{contract.md,correction.json,inputs.json}; traces/stkcop-snapshot-live-h640/live-retry/Q1/{receipt.json,analysis.json,audit.bin,self.bin,post-tail.png,runtime-log.txt,catalogue-result.json}
related: present-pacing-snapshot-consumer.639.md; present-pacing-minimal-owner.636.md
---

# H640 — actual values agree with the sharing key in a live run

The private H639 consumer now passes a live actual-value audit: Q1 completes
the seven late-scene camera knots and exits normally, with **5,609,053 checks
and zero hot/layout/uniform/null-owner mismatches**. The audit TID equals the
qualified Replay worker TID. These counts cover the entire game execution,
not only the selected late corridor. This is observed writer/key agreement,
not an exhaustive proof of every production generation writer.

No live CPU saving or unique default-thread maximum is established. Audit
comparisons deliberately add work to Replay; Q1 also enables Info logging to
verify the startup ABI handshake. Neither its FPS nor its worker ranking is
a default-path performance result.

## Coherent private image and prerequisites

H640 retains H639's fully rebuilt class layout and archives, recompiling four
translation units for the value audit and sparse worker endpoints. The audit
compares the predecessor's owned base with the current cached hot state,
shader layout and uniforms whenever the generation/stride key permits reuse.
A mismatch fails stopped before sharing. Thread-local rows use fixed mmap
storage and are decoded only after process exit. No per-check text logging,
clock or heap allocation is used after thread-row initialization.

ARM64 and x86_64 tests cover disabled, positive, per-field/null negative,
eight-thread and capacity-exhaustion cases: ten processes pass, and 38 corrupt
streams reject. These are observer tests, not a replacement for H638/H639
ownership/refinement/fault tests.

Four fresh-process original-capture controls use the same three H640 images:
reference, candidate, audited candidate, reference. Every run preserves exact
original pixel bytes, query transcript, seven mapped sources and 670/670
resource lifecycle counts. The audited candidate makes 2,102 clean checks.
The sparse observer is also exercised; inline source-probe replay is not
misrepresented as the asynchronous live Replay worker.

## Live qualification and preserved failure

Both live attempts use original game/renderer bytes, Sikarugir, Day/high,
1280x720, max-threads2, snd_targets16, product counters off, draw limit0 and
production-counter occlusion0. The current PE artifacts and private Unix image
are hash-frozen; no guest binary patch or PE-root observation is enabled.
Startup focus and post-tail screenshot use computer-use outside the selected
corridor. No builds, parsers or UI actions occur inside that corridor.

| Attempt | Actual-value checks | Game outcome | Qualification |
|---|---:|---|---|
| Q0 | 5,851,247; zero mismatches | Seven knots, natural exit0 | Failed: supervisor required an Info ABI-success line while perf selected warn. |
| Q1 | 5,609,053; zero mismatches | Seven knots, natural exit0 | Passed with explicit Info logging and the original ABI-marker assertion. |

Q0 remains a failed qualification; its receipt is not rewritten. Q1's corrected
contract was frozen before launch, with unchanged renderer binaries. The
catalogue's sole `missing_capture` is predeclared: its delayed screenshot is
replaced by the supervisor's post-tail image. Both images were inspected and
show no obvious black/repeated-geometry defect; this is not live pixel-exact
equivalence or a guarantee against transient defects.

All14 shared staging files are restored after each run. An independent audit
reconciles4,553 hashes, raw value counts, catalogue/staging identity, process
exit and restoration. No Wine or source-probe process remains.

## Diagnostic CPU endpoints — not a treatment estimate

Q1 has88 completed frames between knots1 and6. Sparse native self counters
give Replay42.428–42.671 and Encode34.226–34.565 CPU ms/frame. These are
endpoint-uncertainty enclosures, not confidence intervals. Replay includes
the enabled value comparisons; concurrent CPU cannot be summed as frame
latency. The selected FPS12.848–12.852 cannot be compared with H636 or used to
claim either improvement or regression.

Next use this same binary with H640_AUDIT/H639_WITNESS unset and warn logging,
freeze matched reference/candidate order and sham controls, and compare sparse
Replay/Encode endpoints over the same camera corridor. Only that evidence can
transfer H639's captured-input reduction to live execution. Full generation-
writer coverage, Arena/general integration, default ownership and promotion
remain open. No product source change, commit or default promotion is made.
