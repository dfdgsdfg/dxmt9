---
domain: present-pacing
workload: 3DMark05 GT1
title: "Present-Pacing — display sync, frame latency, and the wallclock cap"
type: domain-index
status: current
updated: 2026-09-08
source: docs/perfomance/present-pacing/present-pacing-pe-constant-bitcompare.254.md; docs/perfomance/present-pacing/present-pacing-pe-canonical-authority.253.md; docs/perfomance/present-pacing/present-pacing-pe-append-effects.251.md; docs/perfomance/present-pacing/present-pacing-pe-sparse-work.250.md; docs/perfomance/present-pacing/present-pacing-pe-flush-layout.249.md; docs/perfomance/present-pacing/present-pacing-pe-identity-distribution.248.md
related: docs/perfomance/present-pacing/overview.md; docs/perfomance/present-pacing/log.md
---

# Present-Pacing — display sync, frame latency, and the wallclock cap

Latest implementation: [H260](present-pacing-pe-validated-constant-range.260.md)
reuses public constant-range validation in the Live kernel. Native negative
controls, final linked PE32/PE64 inspection and actual Wine checks pass.
Sampler-OFF 05/06 GT2 gives 29.810/18.205 FPS and partial process CPU of
69.857/95.113 ms/frame; separate full profiles retain 1,420/866 PE samples.
No renderer or sampler failures are recorded. Measured gain and repeatable
README recovery remain unproven.

Preceding measured implementation: [H259](present-pacing-pe-settlement-refinement.259.md)
consolidates compact settlement checks and skips absent constant work with
producer-issued immutable presence. Native/refinement and linked-layout
checks and actual x86/x64 getter/StateBlock tests pass. Sampler-OFF 05/06
GT2 spots give 29.682/18.220 FPS, with zero recorded renderer failures;
06 uses the requested same-binary repeat, superseding the interfered run.
Separate full profiles preserve all 1,430/854 PE samples. Measured gain,
CPU reduction and repeatable README recovery remain unproven.

Latest mechanism evidence: [H258](present-pacing-inline-constant-current.258.md)
removes almost all standalone constant records through the current owner,
but 05/06 OFF/ON spots show only +1.25%/+0.91% FPS and no demonstrated CPU
reduction. The mode stays default-off. [H257](present-pacing-pe-constant-copy.257.md)
owns the preceding implementation's measurement and profile conclusions.

Earlier implementation: [H254](present-pacing-pe-constant-bitcompare.254.md)
replaces short-circuit 16-byte constant equality with a bitwise reduction;
both linked PE images vectorize it, with native/ABI checks passing. One
post-commit perf run per product gives 05 GT2 29.500 and 06 GT2 17.958 FPS
(prior 29.009/17.926), with zero chunk/GPU/pipeline failures. This is not
attributed gain, README recovery or CPU-consumption reduction. The leaf owns
actual PE sampling, instruction-layout joins and partial process CPU evidence.

Previous implementation: [H253](present-pacing-pe-canonical-authority.253.md)
gates OFF constant-range queries, groups source authority, reuses retained
Live bindings and replaces paired owner pin indexes with one checked relation.
Native/linked verification and code/stack tradeoffs are recorded there.
Post-commit GT2 is 29.009 sampled FPS versus H251 29.034 (-0.084%), with zero
chunk/GPU/pipeline failures and close CB/pass ratios. This spans H252 and H253;
one run with background CPU contention is not an attributed gain or qualified
neutrality. Whole-scene correctness and current CPU attribution remain open.

Previous runtime checkpoint: [H251](present-pacing-pe-append-effects.251.md) adds
active-section dispatch, concrete constant settlement with narrower rollback,
constinit ledger overrides and fresh counts-only recorder bookkeeping.
Post-commit GT2 is 29.034 sampled FPS versus H250 28.846 (+0.650%), with zero
chunk/GPU/pipeline failures and close CB/pass ratios. Native/linked checks
pass; one run with differing background load does not establish an FPS gain
or qualified neutrality. Whole-scene evidence and CPU attribution remain open.

Prior runtime checkpoint: [H250](present-pacing-pe-sparse-work.250.md) limits
compact binding traversal, quota processing and constant bitmap clearing to
active input. Native/TLA, sanitizer and linked PE checks pass. Post-commit GT2
is 28.846 FPS versus H249 28.835 (+0.038%), with zero chunk/GPU/pipeline
failures and about 4 CB/Present. One run with observed background CPU load
does not establish a gain or qualified neutrality; whole-scene evidence
remains open. No new mutable cache or transport change is introduced.

Prior runtime checkpoint: [H249](present-pacing-pe-flush-layout.249.md) gates
disabled constant-flush diagnostics, skips unobserved empty flushes, and reuses
prepared compact counts. Native/codegen checks pass. A post-commit GT2 spot
check gives 28.835 FPS versus prior H248 29.048 (-0.73%); run health passes,
but the single-run comparison establishes no attributed FPS gain.

Latest diagnostic: [H248](present-pacing-pe-identity-distribution.248.md) measures
2.588 unique identities / 4.494 comparisons per sampled compact GT2 preparation.
Keep the small linear lookup; no hash or FPS promotion follows from this run.

Prior refinement evidence: `247` - compact receipts, fused binding emission, index reuse,
and live borrow authority pass bounded native verification. Ordinary PE append
frames no longer probe. GT2 OFF pairs are 27.884/27.427 harmonic-mean FPS
(-1.64%, substantial same-build variation); no FPS promotion is established.
The CPU-ready Tape locality gap remains an independent workstream.

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)
- [Root 3DMark05 GT2 map](../overview-3dmark05-gt2.md)

## Recent Leaf Documents

- [present-pacing-pe-settlement-refinement.259 - Qualified Compact Settlement Work Removal](present-pacing-pe-settlement-refinement.259.md)
- [present-pacing-inline-constant-current.258 - Current Semantic-Owner Inline-Constant Economics](present-pacing-inline-constant-current.258.md)
- [present-pacing-pe-constant-copy.257 - Bounded Changed-Constant Copy](present-pacing-pe-constant-copy.257.md)
- [present-pacing-pe-autogen-bound-slots.256 - Immutable AUTOGEN Eligibility](present-pacing-pe-autogen-bound-slots.256.md)
- [present-pacing-pe-full-selfpc.255 - Complete Bounded PE Self-PC Reporting](present-pacing-pe-full-selfpc.255.md)
- [present-pacing-pe-constant-bitcompare.254 - Actual PE Attribution and Branch-Reduced Constant Equality](present-pacing-pe-constant-bitcompare.254.md)
- [present-pacing-pe-canonical-authority.253 - Grouped Borrow and Single Pin Authority](present-pacing-pe-canonical-authority.253.md)
- [present-pacing-pe-append-boundary.252 - Family Preparation and Production Ledger Routing](present-pacing-pe-append-boundary.252.md)
- [present-pacing-pe-append-effects.251 - Active Dispatch and Closed Append Effects](present-pacing-pe-append-effects.251.md)
- [present-pacing-pe-sparse-work.250 - Active Sparse Work and Bounded Dirty Clearing](present-pacing-pe-sparse-work.250.md)
- [present-pacing-pe-flush-layout.249 - Constant Flush and Prepared Count Refinement](present-pacing-pe-flush-layout.249.md)
- [present-pacing-pe-identity-distribution.248 - Measured Small-Set Identity Decision](present-pacing-pe-identity-distribution.248.md)
- [present-pacing-pe-compact-refinement.247 - Compact Transaction and Live Borrow Refinement](present-pacing-pe-compact-refinement.247.md)
- [present-pacing-pe-plan-authority.246 - Private Plan Authority and Fused Retention](present-pacing-pe-plan-authority.246.md)
- [present-pacing-pe-compact-reuse.245 - Compact Destination and Identity Reuse](present-pacing-pe-compact-reuse.245.md)
- [present-pacing-pe-prepared-emission.244 - Prepared Emission and Compact Producer Integration](present-pacing-pe-prepared-emission.244.md)
- [present-pacing-pe-admission-reuse.243 - Qualified Pin/Wire Reuse and Active-Prefix Staging](present-pacing-pe-admission-reuse.243.md)
- [present-pacing-pe-direct-canonical.242 - Direct Canonical Value Payload](present-pacing-pe-direct-canonical.242.md)
- [present-pacing-pe-prepared-reset.241 - Prepared-Witness Reset](present-pacing-pe-prepared-reset.241.md)
- [present-pacing-cpu-ready-next-source-intent.239 - Replay-FIFO Intent Is Safe but Does Not Recover Tape Locality](present-pacing-cpu-ready-next-source-intent.239.md)
- [present-pacing-cpu-ready-active-head-locality.238 - Active-Session Lookahead Closes the Local Hold Gap but Not Tape Locality](present-pacing-cpu-ready-active-head-locality.238.md)
- [present-pacing-cpu-ready-tape-promotion-gate.237 - CPU-Ready Tape Restores Progress but Misses the Locality Gate](present-pacing-cpu-ready-tape-promotion-gate.237.md)
- [present-pacing-current-bottleneck-pe-symbol.236 - Current GT2 Ceiling Is The Producer Thread; PE d3d9.dll Is 10.6%](present-pacing-current-bottleneck-pe-symbol.236.md)
- [present-pacing-sfiv-current-tail-attribution.235 - Current SFIV Tail Is CPU/Producer Cadence, Not A GPU Hot Frame](present-pacing-sfiv-current-tail-attribution.235.md)
- [present-pacing-arena-admission-regression-boundary.233 - Segmented Arena Admission Turns Capacity Pressure Into Session Fragmentation](present-pacing-arena-admission-regression-boundary.233.md)
- [present-pacing-post-defselect-cpu-attribution.05 - The Producer Thread Cannot Be Split By Image](present-pacing-post-defselect-cpu-attribution.05.md)
- [present-pacing-drain-fence-attribution.207 - The Blocked Locks Are Mostly MANAGED](present-pacing-drain-fence-attribution.207.md)
- [present-pacing-drain-fence-attribution.206 - One Of 84 Bridge Entry Points Owns 99.8% Of The Drain Fence](present-pacing-drain-fence-attribution.206.md)
- [present-pacing-sfiv-shader-cost-attribution.205 - The SFIV 88ms Instances Are Real Shader Work, Not A Wait](present-pacing-sfiv-shader-cost-attribution.205.md)
- [present-pacing-sfiv-scene-pass-stall.204 - SFIV Scene-Pass Frame-Period Stall Owns The Frame Wall](present-pacing-sfiv-scene-pass-stall.204.md)
- [present-pacing-engine-default-trio.203 - The Promoted Trio Becomes The Engine Default](present-pacing-engine-default-trio.203.md)
- [present-pacing-archive-prewarm-hardening.202 - Archive Prewarm Hardening Closes The Startup-Flake Class](present-pacing-archive-prewarm-hardening.202.md)
- [present-pacing-inline-const-delta.201 - Inline Const Delta Proves Mechanism But Lands Inside The Noise Band](present-pacing-inline-const-delta.201.md)
- [present-pacing-decimated-pe-stats.200 - Decimated PE Stats Size The Recorder Core At ~8.5ms/present](present-pacing-decimated-pe-stats.200.md)
- [present-pacing-postcache-resample.199 - Post-Cache Producer Re-Sample - The Wall Is Now The Guest Blob](present-pacing-postcache-resample.199.md)
- [present-pacing-readonly-cache-stacking.198 - Readonly Cache Stacks With The Promoted Pair For +26% Cumulative](present-pacing-readonly-cache-stacking.198.md)
- [present-pacing-readonly-managed-buffer-cache.197 - Readonly Managed Buffer Cache Collapses The Lock Bridge Storm](present-pacing-readonly-managed-buffer-cache.197.md)
- [present-pacing-producer-sampling-attribution.196 - Non-Perturbing Producer Sampling Finds The Buffer-Lock Bridge Storm](present-pacing-producer-sampling-attribution.196.md)
- [present-pacing-consolidation-long-confirm.194 - Consolidation Long Confirm - Offload+IndexCache +10% Over The Full Demo](present-pacing-consolidation-long-confirm.194.md)
- [present-pacing-pe-const-overhead-cut.193 - PE Const-Chain Overhead Cuts Land Clean But Move No FPS](present-pacing-pe-const-overhead-cut.193.md)
- [present-pacing-pe-cost-verification.192 - PE Recording Cost Is Real (~10ms/present, Overhead-Corrected)](present-pacing-pe-cost-verification.192.md)
