---
type: "Spec Requirements"
title: "Harness Gate Requirements — Comparison and Proof"
description: "Requirements for the gate domain: a comparison must prove a mechanism, not just that numbers moved."
tags: [specs, experiments, harness, gate, requirements]
---

# Harness Gate Requirements — Comparison and Proof

This is a domain document under `specs/experiments/harness/`. It
instantiates the `R-HARN-*` requirement groups in
`specs/experiments/harness/requirements.md` for the `gate` domain
named in `specs/experiments/harness/spec.md` §1. The gate domain
participates in the `compare-gate` pipeline stage (parent spec.md §0)
only: it reads the CSV/Markdown output of `reduce`-domain and
`join`-domain scripts across the `log-reduce → compare-gate` and
`external-join → compare-gate` boundaries (parent spec.md §2) and
returns a pass/fail verdict a human or another harness stage treats as
proof that a candidate change did what it claims. Requirement IDs in
this file use the prefix `R-HARN-GATE-`.

This domain exists because a comparison that only checks "did the
numbers move" is not the same contract as "did the numbers move for
the claimed reason," and because a comparison over degenerate input
can satisfy the first contract while proving nothing at all. Parent
`requirements.md` §3 (`R-HARN-3.4`) states the second failure mode
directly, citing the incident that motivated this whole
specification: four `run_3dmark05_mini_replay.py` lanes each wrote a
uniformly black image, so all four SHA-256 digests were identical, and
a digest-only equality gate would have read that as agreement — a
clean-looking pass over four artifacts that carried no content at all.
§3 below makes that rule concrete against this domain's own scripts,
not only the historical mini-replay incident; §3.4/§3.5 record that at
least two of this domain's six scripts can be made to reproduce the
same shape of false pass today, verified directly rather than assumed.

---

## 1. Scope

**R-HARN-GATE-1.1** This document covers exactly the six scripts the
parent domain map's `gate` row names via its `scripts/tools/compare_*`
glob plus `scripts/tools/analyze_xcode_replay_variance.py`:
`compare_3dmark05_p4_pair.py`, `compare_3dmark05_perf_counters.py`,
`compare_attachment_dumps.py`, `compare_experiment_images.py`,
`compare_xcode_dxmt_bottlenecks.py`, and
`analyze_xcode_replay_variance.py`. `spec.md` §1 records each script's
line count and role, verified against the working tree rather than
assumed from the glob alone. Instantiates R-HARN-1.1.

**R-HARN-GATE-1.2** This domain's own `spec.md` names only
`compare-gate` (parent spec.md §0) as a stage it participates in.
`finalize_3dmark05_perf_probe.sh` (`join` domain) and
`run_3dmark05_perf_probe.sh` (`probe` domain) each invoke this
domain's scripts inline — per parent spec.md §1's "the domain axis is
harness families, not stages" — and additionally enforce the
baseline-presence rule in §5 below at their own call sites; that
enforcement is those domains' own contract, not this one's, even
though §5 depends on it to make the overall pipeline behavior
checkable. Instantiates R-HARN-1.1.

**R-HARN-GATE-1.3** The `compare-gate → record` boundary (parent
spec.md §2) — where this domain's verdict becomes an evidence citation
the `audit` domain checks — is that domain's own contract. This
document does not restate it; `spec.md` cites it by reference where
relevant. Instantiates parent spec.md §1's domain-axis rule, mirrored
from R-HARN-JOIN-1.3/R-HARN-REDUCE-1.2.

---

## 2. A Gate Names the Mechanism It Proves

**R-HARN-GATE-2.1** A comparison flag in this domain that exists to
validate a specific candidate change is named, and documented, after
the mechanism it proves — a code path, a counter family, a specific
cache or storage behavior — not only after the direction a raw number
moved. A flag named only `--require-*-decrease` without a
disambiguating qualifier is acceptable when exactly one mechanism can
produce that decrease; when more than one mechanism could produce the
same numeric direction, this domain must expose them as separate,
separately named flags so a passing gate cannot be read as proof of a
mechanism it did not actually check. `spec.md` §3 records the verified
case where this domain keeps three separately named index-cache flags
for what a caller could otherwise conflate into one "cache locality
improved" claim.

**R-HARN-GATE-2.2** A composite proof flag — one that bundles several
individual checks behind a single name, such as this domain's
"stable frame proof" or "cache-opt apply proof" shape — must still let
a caller determine, from its own name or its failure diagnostic,
*which* of the bundled checks failed. A composite flag that only
reports "the composite failed" without naming the failing constituent
degrades to the same "numbers moved or did not" ambiguity this section
exists to forbid. `spec.md` §3 records, per composite flag in this
domain, whether the underlying implementation names the failing
constituent.

**R-HARN-GATE-2.3** This domain's own scripts must not be the only
place a flag's mechanism claim is documented — per parent R-HARN-5.1,
the engine-shape or counter-family a proof flag depends on is stated
in this domain's own `spec.md` in addition to the script's `--help`
text, so a reader auditing this domain's contract does not have to
reverse-engineer the mechanism from source. Instantiates R-HARN-5.1.

---

## 3. A Gate Over Degenerate Inputs Must Fail, Not Pass

**R-HARN-GATE-3.1** A comparison in this domain that finds two
artifacts in agreement — identical digests, identical byte content,
identical pixels, a zero delta — must not report that agreement as a
pass without first establishing that at least one of the two artifacts
carries real content. Matching values from two empty, uniform, or
otherwise degenerate artifacts is not evidence of anything a candidate
change did; per parent R-HARN-3.4, treating it as a pass is the exact
shape of the failure that let four identical black
`run_3dmark05_mini_replay.py` images produce four identical digests a
naive equality gate would have accepted.

**R-HARN-GATE-3.2** Per parent R-HARN-3.2/3.4, the mechanism this
domain's scripts must use to satisfy R-HARN-GATE-3.1, once the
artifact envelope (parent spec.md §3) exists, is to consult each
compared artifact's `validity` field before trusting agreement — not
to re-derive non-degeneracy from the payload itself inside this
domain's own comparator. `spec.md` §4 records, as a verified fact and
not an assumption, that no script in this domain reads or writes a
`validity` field today, because the envelope has not been adopted by
any upstream producer this domain consumes (parent spec.md §5, "No
migration step has been performed").

**R-HARN-GATE-3.3** Until R-HARN-GATE-3.2's envelope consultation
exists, a comparator in this domain that lacks *any* non-degeneracy
check — opt-in or default-on — over the artifacts it compares is a
verified gap against R-HARN-GATE-3.1, not a compliant fallback.
`spec.md` §4 states, per script, whether such a check exists at all,
whether it is default-on or requires an explicit flag, and whether it
is included in every named comparison policy the script offers or must
be requested separately from the policy.

**R-HARN-GATE-3.4** This document does not treat the four-black-image
incident as a closed, historical case specific to
`run_3dmark05_mini_replay.py`. Two of this domain's own six scripts
were verified, by running them on freshly generated degenerate inputs,
to reproduce the identical shape of false pass on 2026-07-27:
`compare_experiment_images.py --policy exact` reports "Passed: all
requested image gates were satisfied" (exit `0`) for two
bit-identical, fully uniform (RGB `(0,0,0)`) 8x8 images, because its
`exact`/`lsb1` named policies set `max_changed_pct`/`min_ssim`
thresholds but do not set `min_before_active_pct`/
`min_after_active_pct` — the one flag pair this script has that can
detect an all-black frame is not part of either named policy and must
be requested separately. `compare_attachment_dumps.py --require-exact`
reports "Passed: all attachment dumps are byte-exact and
metadata-compatible" (exit `0`) for two byte-identical, all-zero
64-byte dumps with matching metadata sidecars, because this script has
no active-content or non-degeneracy check of any kind, opt-in or
otherwise — `spec.md` §4 records both reproductions with the exact
commands run and their real output.

**R-HARN-GATE-3.5** A future change to either script named in
R-HARN-GATE-3.4 that adds a degeneracy check must make it part of
every named policy preset the script offers (`exact`, `lsb1`, or any
future preset), not only available as a separately-requested flag —
per R-HARN-GATE-3.1, a caller who asks for the strictest available
named policy has a reasonable expectation that "strictest" already
includes "not degenerate," and a preset that omits it silently
degrades that expectation.

---

## 4. A Delta Inside the Measured Noise Floor Is Inconclusive, Not a Result

**R-HARN-GATE-4.1** A comparison in this domain that reports a
directional verdict (win/lose, pass/fail, regression/improvement) over
a metric known to carry run-to-run replay or sampling variance must be
able to report a third state — inconclusive — when the observed delta
falls inside that metric's own measured noise floor, distinct from
both "the candidate improved it" and "the candidate regressed it."
Collapsing a within-noise delta into either directional bucket
attributes a real-looking verdict to what may be measurement noise.

**R-HARN-GATE-4.2** `spec.md` §5 records, per script in this domain,
whether R-HARN-GATE-4.1's third state is actually implemented.
`compare_3dmark05_p4_pair.py` is the one script in this domain that
does: it defines an explicit `REPEAT` verdict (a distinct exit code)
for an FPS delta inside its own `--noise-pct` band, separate from
`WIN`/`LOSE`. `compare_xcode_dxmt_bottlenecks.py` and
`compare_3dmark05_perf_counters.py` accept regression *tolerance*
bands (`--max-*-regression-*`) but do not expose a distinct
inconclusive verdict; a delta that clears a tolerance band is reported
as passing exactly like a delta with a wide safety margin, and a
caller cannot tell the two apart from this domain's own output. This
is recorded as a verified gap in this document rather than implied
closed.

**R-HARN-GATE-4.3** `analyze_xcode_replay_variance.py` is this
domain's dedicated mechanism for measuring a metric's own noise floor
(the coefficient of variation across N >= 3 replays of the identical
`.gputrace`), but it is a separate script from the ones that produce a
directional verdict — no script in this domain invokes it, or reads
its output, before emitting a pass/fail/win/lose result. Per
`agents/rules/metal_debugging.rules.md`'s own guidance ("Use this tool
whenever an A/B comparison reports a sub-10% delta or when only some
encoders move and others appear noisy"), consulting the noise floor
before trusting a small delta is a documented manual procedure a human
follows, not a check any script in this domain enforces on the other's
behalf. A future change that wants R-HARN-GATE-4.1 enforced
automatically for the Xcode-joined comparators must either wire this
script's variance computation into them or add an explicit
`--require-variance-checked`-shaped flag; until then, the connection
between the two scripts is procedural, not mechanical, and this
document does not claim otherwise.

**R-HARN-GATE-4.4** A caller must not be able to infer, from
`analyze_xcode_replay_variance.py`'s own command-line surface, that it
enforces a fixed noise-floor threshold by default. `spec.md` §5
records the verified fact that its `--max-cv-pct` flag's own default
is "no gate" (report-only unless the caller supplies a value); any
specific percentage a runbook recommends is that runbook's own
convention passed explicitly on the command line, not a value this
script's argument parser falls back to on its own.

---

## 5. A Gate That Requires a Baseline Must Fail When the Baseline Is Absent

**R-HARN-GATE-5.1** A comparison in this domain that is meaningless
without a second artifact — every script in this domain compares
exactly two inputs — must not silently proceed, produce a
single-sided report, or exit zero when only one input is available;
per parent R-HARN-2.1, a script that produces a report from a missing
counterpart is producing a partial or substitute artifact while
reporting success. `spec.md` §6 records that this domain's two largest
comparators enforce this at the argument-parsing level: both inputs
are required positional arguments, so the process cannot start at all
without both, distinct from an optional flag a caller could omit.

**R-HARN-GATE-5.2** A caller-requested proof gate in this domain (a
`--require-*` or `--max-*-regression-*` flag) that depends on a
baseline artifact supplied by an upstream wrapper, not by this domain's
own script directly, must cause that wrapper to fail before this
domain's script is even invoked when the baseline was not supplied —
not cause this domain's script to be silently skipped while the
wrapper's overall run still exits zero. `spec.md` §6 records that this
rule is enforced today by the `probe`- and `join`-domain wrapper
scripts that invoke this domain (`run_3dmark05_perf_probe.sh` and
`finalize_3dmark05_perf_probe.sh` respectively), each with its own
flag spelling for the run-level baseline path — this document states
that difference plainly rather than assuming the two wrappers agree on
a single flag name.

**R-HARN-GATE-5.3** This domain's own scripts are not the layer that
implements R-HARN-GATE-5.2's wrapper-level check, and a future change
to this domain must not describe that check as belonging here; it
belongs to the `probe`/`join` domains' own requirements
(cross-referenced in `spec.md` §6). This domain's own contribution to
"fail rather than run standalone" is limited to R-HARN-GATE-5.1's
two-required-positional-argument shape, which is necessary but not
sufficient — it stops a caller from omitting a baseline path entirely,
but it cannot stop a caller from pointing that path at the wrong run,
which is R-HARN-JOIN-3.1/3.2's concern, not this domain's.

---

## 6. This Domain Sets No Contract-Relevant Environment Variable

**R-HARN-GATE-6.1** No script in this domain sets, exports, reads, or
forwards any `DXMT9_*`/`DXMT_*` environment variable — this domain's
scripts read only the file paths and threshold values given on their
own command line. `spec.md` §7 records the verified grep that finds no
`os.environ`/`getenv` reference across all six scripts. A change that
adds one is a deviation from this requirement and must either be
reverted or must first update this document to state, and justify, the
new ownership. Instantiates parent spec.md §4 Rule 1, mirrored from
R-HARN-REDUCE-6.1/R-HARN-JOIN-6.1.

**R-HARN-GATE-6.2** A `DXMT_3DMARK05_*` wrapper variable that a
`probe`- or `join`-domain script reads to select a *default value* for
one of this domain's own CLI flags (for example, a default regression
tolerance) is not, by that fact alone, a variable this domain sets or
reads; per parent spec.md §4's "contract-relevant, defined" test, the
variable changes which of this domain's own threshold values is
supplied on the command line, not what a dxmt9-produced counter, image,
or geometry payload means. This is the same distinction
R-HARN-JOIN-6.1 draws for the `join` domain's own wrapper-default
variables, applied here rather than restated in full.

---

## 7. Diagnostic Paths Carry the Primary Contract

**R-HARN-GATE-7.1** Every command-line flag accepted by any script in
this domain that alters its verdict, its exit code, or its report
content — a proof gate, a tolerance, a policy preset, a noise
threshold — appears in this domain's own `spec.md` mode table, whether
the flag is meant for routine promotion evidence or for a one-off
diagnostic probe. Instantiates parent R-HARN-6.2, mirrored from
R-HARN-REDUCE-7.1/R-HARN-JOIN-7.1.

**R-HARN-GATE-7.2** A flag whose entire purpose is to add or narrow a
proof gate is bound by parent R-HARN-3.3/6.1 the same as this domain's
primary comparison path: it must actually execute the check it
advertises and exit non-zero with a diagnostic naming the checked
quantity on failure, not merely accept the flag and otherwise proceed
as if it were absent. Instantiates parent R-HARN-6.1, mirrored from
R-HARN-REDUCE-7.2/R-HARN-JOIN-7.2.
