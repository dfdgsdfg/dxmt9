---
type: "Spec Requirements"
title: "Harness Requirements — Evidence Production Contracts"
description: "Harness requirements and cross-domain contracts."
tags: [specs, experiments, harness, requirements]
---

# Harness Requirements — Evidence Production Contracts

dxmt9's performance and correctness evidence is produced by a chain
of harness scripts: a run is captured, dumped, reduced, replayed,
joined against external tool exports, compared, and audited. Each
stage is a separate script or script family, and each one hands its
output to the next as an artifact on disk. This spec states the
contracts those artifacts must satisfy so a defect in one harness is
detectable at the boundary instead of silently propagating into a
performance claim or a correctness verdict.

Five contract groups below are each derived from a defect that
actually blocked an experiment: `scripts/tools/run_3dmark05_mini_replay.py`
failed five separate ways during a vertex-remap experiment on
2026-07-25/27, and none of the five failure modes was ruled out by an
existing written contract. §2-§6 state the contract each defect
should have violated loudly instead of silently.

---

## 1. Scope

**R-HARN-1.1** This spec governs the harness scripts under
`scripts/tools/`, `scripts/run_apps/`, `scripts/check/`, and
`scripts/run_suites/` — 86 files at time of writing — grouped into
seven domains: `runner`, `probe`, `replay`, `reduce`, `join`, `gate`,
and `audit`. Each domain's own scripts, stages, artifacts, and owned
environment variables are specified in
`specs/experiments/harness/<domain>/requirements.md` and `spec.md`.

**R-HARN-1.2** This spec governs the contracts between harnesses —
what a harness's output artifact promises the downstream harness that
consumes it — not the correctness of what any harness measures.
Whether a reported FPS number, GPU-time counter, or rendered pixel is
an accurate measurement of dxmt9 behavior is out of scope for this
spec; whether the artifact carrying that number or pixel is
interpretable by its declared consumer, and fails loudly when it is
not, is in scope.

**R-HARN-1.3** `agents/rules/environment_variables_*.rules.md` remains
the catalogue of every `DXMT*`/`DXMT9*` knob a harness may set or
read. This spec and its domain subdirectories are the contract layer
over that catalogue — which domain owns each contract-relevant
variable, and what a downstream artifact's interpretation depends on
when it is set — matching those files' own stated boundary: "These
files are **descriptive**, not a behavioral spec — for that, see
`specs/`."

---

## 2. No Silent Degradation

**R-HARN-2.1** A harness that produces an artifact exits zero only if
a valid artifact was written, and exits non-zero otherwise. A process
that exits zero after writing a partial, wrong-shape, or substitute
artifact violates this requirement regardless of whether it also
printed a warning to stderr.

**R-HARN-2.2** An input value that falls outside a harness's
recognized classification set must not silently select a fallback
behavior. A branch that maps "unrecognized input" onto "nearest known
behavior" without failing, and without a diagnostic naming the
unrecognized value, is a silent degradation. Rationale (defect 3):
`scripts/tools/run_3dmark05_mini_replay.py`'s `color_pixel_format()`
recognizes `core::Format` values 1-4 and returns
`MTLPixelFormatRGBA8Unorm` for every other value, including the R32F
format (`core::Format` 16) that row `60/0` actually uses. The replay
rendered into a wrong-format attachment with no diagnostic that the
format was unrecognized.

**R-HARN-2.3** A fallback that is genuinely wanted — a deliberate
compatibility behavior rather than an oversight — is reachable only
behind a named opt-in flag or environment variable. A harness must
not select that fallback by default for an input it does not
recognize.

**R-HARN-2.4** When a harness resolves an ambiguous, defaulted, or
fallback choice (a format, a variant, a code path), the resolved
choice is recorded in the artifact envelope so a downstream consumer
or human reviewer can detect that a default was taken rather than an
exact classification resolved.

---

## 3. Output Validity Self-Assertion

**R-HARN-3.1** A harness that produces a measurement artifact — an
image, a trace, a counter file — asserts that the artifact is
non-degenerate (not uniformly one value, not zero-length, not empty of
samples) before reporting success. Rationale (defect 4): all four
`run_3dmark05_mini_replay.py` lanes reported `mini replay draws=229
repeat=1` and exited 0 while each lane wrote a 1024x768 PPM containing
exactly one distinct pixel value. Nothing in the harness asserted the
image carried content before declaring success.

**R-HARN-3.2** The result of the validity assertion required by
R-HARN-3.1 is recorded in the artifact envelope's `validity` field. A
downstream consumer reads this field rather than re-deriving validity
itself from the payload.

**R-HARN-3.3** A harness that cannot assert validity — because the
check is unimplemented, inconclusive, or does not run — exits
non-zero rather than reporting success. Reporting success without an
executed validity assertion is equivalent to skipping the assertion.

**R-HARN-3.4** A gate that compares two or more artifacts for
agreement must not treat matching digests or matching byte content
alone as evidence that the compared artifacts are valid; it must also
consult each artifact's `validity` field (R-HARN-3.2) before treating
agreement as a pass. Rationale: a degenerate artifact is worse than a
missing one precisely because it can silently pass this kind of gate
otherwise — the four identical black PPMs from defect 4 produced four
identical SHA-256 digests, which a digest-only equality gate would
have read as a pass without ever checking whether any of the four
images was valid.

---

## 4. Boundary Semantics Are Declared, Not Inferred

**R-HARN-4.1** Every artifact that crosses a domain boundary carries,
in band — inside the artifact itself or its accompanying sidecar —
the information a consumer needs to interpret its bytes. A consumer
must not need out-of-band knowledge or an assumed convention to read
the artifact correctly.

**R-HARN-4.2** Every offset, stride, slice origin, and index base
carried by a cross-boundary artifact names the coordinate system it is
expressed in — for example, "byte offset into the sliced payload"
versus "byte offset into the source D3D9 stream buffer." A numeric
field without a named coordinate system is not a complete
interpretation rule.

**R-HARN-4.3** A consumer of a cross-boundary artifact computes any
derived offset from the artifact's declared fields, never from an
assumed convention about how the producer populated those fields.
Rationale (defect 2): the geometry dump writes a slice starting at
`stream0_start_byte`, and `stream0_start_byte == stream0_offset`, so
payload byte 0 is fetch slot 0. `run_3dmark05_mini_replay.py` treated
`stream0_offset` as an offset within the already-sliced payload
instead of computing it from the declared slice origin, producing
negative slot capacities. The defect was latent for months because
every previously exercised row happened to have `stream0_offset == 0`,
the one case where both interpretations agree.

**R-HARN-4.4** Neither side of a cross-boundary contract may be
declared correct by inspecting only its own internal consistency. The
producer and consumer in R-HARN-4.3 were each internally consistent —
the producer's slice arithmetic was correct and the consumer's offset
arithmetic was correct in isolation — and no artifact field connected
the two conventions. A boundary contract is satisfied only when the
producer's declared fields and the consumer's derivation are checked
together against the same coordinate system, which is a class of
defect that single-sided code review of either side cannot catch.

---

## 5. Engine-Shape Dependencies Are Pinned and Detectable

**R-HARN-5.1** A harness that pattern-matches engine-generated output
— MSL source signatures, log line formats, CSV column names — declares
the expected shape it depends on in its owning domain's `spec.md`.

**R-HARN-5.2** When a harness's pattern match against engine output
fails, its failure message names the specific expectation that was
not met, rather than raising a generic or unrelated error.

**R-HARN-5.3** A change to a pattern-matched engine shape that a
harness cannot handle is a hard failure for that harness, not a
silent fallback to a different code path. Rationale (defect 1):
`transform_msl` in `scripts/tools/run_3dmark05_mini_replay.py` raised
`SystemExit("mini replay cbuf rewrite could not find buffer(30)
argbuf parameter")` after `DXMT9_ARGBUF_DIRECT_CBUF` was promoted
default-on in commit `9eb02437` ("perf(encoder): enable direct cbuf by
default"); dumped MSL began binding constants at `buffer(0)`/`buffer(3)`
with no argbuf parameter at all. Failing loudly here was the correct
behavior under this requirement — the defect was that the harness's
`buffer(30)` expectation was declared nowhere outside its own source,
so no one could see the dependency without reading the failure.

---

## 6. Diagnostic Paths Carry the Primary Contract

**R-HARN-6.1** A diagnostic or secondary mode of a harness — a flag
that alters its normal output for debugging or bisection — is bound
by the same no-silent-degradation (§2) and output-validity (§3)
contracts as the harness's primary path. A diagnostic mode is not
exempt from producing a valid artifact or exiting non-zero.

**R-HARN-6.2** Every flag that alters a harness's output appears in
the mode table of its owning domain's `spec.md`, whether the flag is
intended for routine use or for diagnostic bisection.

**R-HARN-6.3** A diagnostic mode that does not compile or execute is a
contract violation, not an acceptable degraded state, because a
diagnostic path exists specifically to be exercised once the primary
path has already failed. Rationale (defect 5):
`run_3dmark05_mini_replay.py`'s `--force-fragment-color` flag fails to
compile — it returns a bare `float4` from a function whose declared
return type is `FfpFsOut` — so the one diagnostic flag meant to
bisect a rendering failure between geometry and fragment stages is
itself unusable exactly when it would be needed.
