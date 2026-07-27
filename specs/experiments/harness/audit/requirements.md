---
type: "Spec Requirements"
title: "Harness Audit Requirements — Record Verification"
description: "Requirements for the audit domain: an audit's own checked/unchecked boundary, CI membership, and citation-referent verification."
tags: [specs, experiments, harness, audit, requirements]
---

# Harness Audit Requirements — Record Verification

This is a domain document under `specs/experiments/harness/`. It
instantiates the `R-HARN-*` requirement groups in
`specs/experiments/harness/requirements.md` for the `audit` domain
named in `specs/experiments/harness/spec.md` §1. The audit domain owns
exactly one stage, `record` (parent spec.md §0), and is the sole
consumer of the terminal `compare-gate → record` boundary (parent
spec.md §2) — the point where a `gate`-domain verdict becomes an
evidence citation a human writes into a `docs/perfomance/` leaf
document's `source:` field. Requirement IDs in this file use the
prefix `R-HARN-AUDIT-`.

**A verified absence, stated up front.** Parent spec.md §2 names eight
boundary sections. Extracting every `R-HARN-\d+\.\d+` token from each
`### ` boundary subsection in `specs/experiments/harness/spec.md`
(bounded to each section's own text, stopping at the next `##`/`###`
heading) on 2026-07-27 returns a non-empty match set for six of the
eight and an empty match set for exactly two: `run-capture →
dump-extract` (spec.md:112-123 — this section cites
`agents/rules/metal_debugging.rules.md` §1 but no `R-HARN-*` ID at
all) and `compare-gate → record` (spec.md:241-262 — the boundary this
domain owns). This document does not claim `compare-gate → record` is
the *only* zero-citation boundary — it is one of two, verified by
re-running the same extraction rather than assumed from memory — but
it is still true, and the only claim R-HARN-AUDIT-1.2 actually needs,
that no `R-HARN-*` clause governs this domain's own boundary
specifically. §1 below states plainly, for each requirement group that
follows, whether a parent clause actually governs it or whether the
fit is by analogy only. Where a requirement below cites a parent ID,
it is because that ID's general principle extends to this domain's
own artifact (a citation, not a measurement); where no citation
appears, this document says so rather than inventing one.

---

## 1. Scope

**R-HARN-AUDIT-1.1** This domain comprises exactly the scripts under
`scripts/check/`, per the parent domain map's `audit` row (parent
spec.md §1). `spec.md` §1 records the verified inventory: 10 files
(`assert_perf_counters.py`, `audit_perf_counter_callsites.py`,
`audit_perf_counter_table.py`, `audit_perf_docs_sources.py`,
`audit_winemetal_install_names.py`,
`check_d3d9_conformance_manifest.sh`,
`check_d3d9_conformance_status.py`, `check_drift.sh`,
`check_manifest.sh`, `verify_tla.sh`), 1,605 lines total (`wc -l`).
`scripts/check/README.md` is documentation, not a script, and is
excluded from this count. Instantiates R-HARN-1.1.

**R-HARN-AUDIT-1.2** This domain's own `spec.md` names only `record`
(parent spec.md §0) as the stage it participates in. No parent
`R-HARN-*` clause states what the `record` stage or the
`compare-gate → record` boundary specifically requires of its
consumer — the closest genuine parent material is parent spec.md §2's
own prose description of that boundary (not a numbered requirement)
and parent spec.md §3's "second use of `inputs` digests" passage. §4
below states, per parent spec.md's own words, where that prose
description already exceeds what the domain's current script does,
rather than treating the prose as settled fact.

**R-HARN-AUDIT-1.3** A script under `scripts/check/` that this
document's own inventory (`spec.md` §1) does not name is out of scope
for every requirement below; a future script added to that directory
must either be added to `spec.md` §1 with its Meson registration state
(§3) or this document must be treated as stale for that script. This
mirrors the same "no unstated deviation" discipline
`specs/experiments/harness/gate/requirements.md` R-HARN-GATE-1.1 and
`specs/experiments/harness/reduce/requirements.md`'s equivalent already
apply to their own inventories.

---

## 2. An Audit Declares What It Checks and What It Does Not

**R-HARN-AUDIT-2.1** Every script in this domain's own `spec.md` entry
states, as a complete enumeration and not a paraphrase, every
condition the script actually evaluates before it can exit non-zero.
A description such as "audits docs/perfomance provenance" is not
sufficient; the entry must name the exact predicates checked (§4 gives
the worked case). No parent `R-HARN-*` clause states this requirement
literally — parent §2 (`R-HARN-2.1`–`2.2`, no-silent-degradation) and
§3 (`R-HARN-3.1`, `R-HARN-3.3`, output-validity self-assertion) govern
a harness that *produces* an artifact and must not silently degrade or
skip its own validity assertion; this domain's scripts are verifiers,
not producers, and no parent clause names a verifier's obligation to
disclose its own check boundary. This requirement extends that same
"no silent degradation" spirit, by analogy rather than literal
citation, to the one domain the parent's own scope note (R-HARN-1.1)
places under audit but never separately addresses.

**R-HARN-AUDIT-2.2** For a script whose name or docstring implies it
verifies a citation's *referent* — that a path it reads actually names
something real — `spec.md` states explicitly, as a table entry rather
than free prose, whether the script confirms the referent exists,
confirms only that a textual field is present in the expected shape,
or confirms neither. `spec.md` §2 gives this table for
`audit_perf_docs_sources.py` specifically, verified against
`audit_paths()` and reproduced live (§4 below), because that script's
name ("audit ... docs sources") most plausibly reads to a caller as
"verifies the cited sources," which is not what it does.

**R-HARN-AUDIT-2.3** A script's default invocation scope — which
files, paths, or records it evaluates when given no explicit selector
— is stated in `spec.md` whenever that default is narrower than "every
file the script's name suggests it covers." `audit_perf_docs_sources.py`'s
default of git-new-only `docs/perfomance` leaves (§4) is the case that
motivates this requirement: a reader who assumes the registered Meson
test audits the whole `docs/perfomance` corpus on every CI run is
wrong, and nothing in the script's own `--help`-equivalent surface
(it has no `--help`; `argparse`'s auto-generated one only documents
`--path`) corrects that assumption without reading source.

---

## 3. Meson Registration States CI Membership and Its Actual Scope

**R-HARN-AUDIT-3.1** `spec.md` records, per script in this domain's
inventory, whether it is registered as a Meson test, under which exact
test name, and in which `meson.build` file — `tests/meson.build`
directly, or a `subdir()`-included file such as
`tests/native/backend/meson.build`. A script not registered anywhere
is stated as such; a reader must not have to run `grep` themselves to
learn whether a script in this domain runs in CI at all. No parent
`R-HARN-*` clause addresses CI/Meson registration at all — this is a
second stated absence per this document's own header note, not a
forced citation.

**R-HARN-AUDIT-3.2** A script's Meson registration is recorded together
with the exact arguments the `test()` invocation passes it, because a
script that exposes an optional stricter mode (a `--fail-*` flag, an
explicit path list) is only as strict in CI as the arguments its
registered invocation actually supplies. `spec.md` §3 gives the
verified case: `check_d3d9_conformance_status.py`'s registered test
(`dxmt9-d3d9-conformance-status-report`) passes no arguments, so it
exercises only "the manifest parses and renders," not its own
`--fail-if-full-support-missing` gate, even though that flag exists in
the same script. Instantiates, by analogy, R-HARN-6.2's "every flag
that alters a harness's output appears in the mode table" — extended
here to also require the mode table to say which of those flags CI
registration actually exercises.

**R-HARN-AUDIT-3.3** A script that is registered as a Meson test by
being wired as the test *executable* itself (via `find_program()`),
rather than as a script path passed as an argument to a `python3`/
`bash` test executable, is still "part of CI" in the sense R-HARN-AUDIT-3.1
requires — `spec.md` §3 must not omit it merely because its wiring
shape differs from the majority pattern in this domain.
`assert_perf_counters.py` is the verified instance: it is declared via
`find_program()` in `tests/meson.build` and used as the test binary
for `dxmt9-allocation-counter-spec` in `tests/native/backend/meson.build`,
not invoked as `test(name, python3, args: [script])` the way every
other script in this domain is.

---

## 4. Citation Audits Verify Referent Existence

**R-HARN-AUDIT-4.1** A script whose job is to validate a citation — a
path, a line reference, a cross-file pointer — from one file to
another must confirm that the referent resolves (the path exists, the
line is in range, or an equivalent positive check) before it can
report success for that citation; checking only that a citation field
is present and correctly *shaped* is not validating the citation.
`R-HARN-4.1`–`4.2` (parent §4, "Boundary Semantics Are Declared, Not
Inferred") state the general principle this requirement specializes:
a consumer must be able to interpret an artifact from its own declared
fields rather than an assumed convention, and a citation with no
verified referent gives a downstream reader nothing to interpret
against — it is indistinguishable, from the reader's side, from a
citation that was never checked at all. Parent spec.md §3's own
"second use of `inputs` digests" passage makes the concrete case for
this exact domain: "A citation that names a path but carries no digest
cannot distinguish 'the file was never produced' from 'the file
existed and was later cleaned up' from 'a different file now sits at
that path'."

**R-HARN-AUDIT-4.2** `audit_perf_docs_sources.py`'s `audit_paths()`
does not satisfy R-HARN-AUDIT-4.1 today: verified directly against
source and by live reproduction (`spec.md` §2, §4), it checks exactly
two conditions — a frontmatter `source:` line is present, and that
line's text does not contain the retired string
`specs/perfomance.plan.md` — and neither condition inspects whether
any path named inside the `source:` value resolves on disk. This is
the concrete gap the task that produced this document was written to
record: a disk cleanup on 2026-07-27 found cited log/output paths
already missing from disk that this audit had never flagged, because
it was never checking for that condition in the first place, not
because it checked and passed a broken case. Parent
`specs/experiments/harness/requirements.md` and `spec.md` cite this
same incident (34 of 56 paths, parent spec.md §2/§3) as the motivating
background; this document's own contribution, verified independently
in `spec.md` §4, is a second, live-reproduced instance of the same gap
against the current working tree rather than a restatement of the
parent's number.

**R-HARN-AUDIT-4.3** A future change that adds referent-existence
checking to `audit_perf_docs_sources.py` (R-HARN-AUDIT-4.1's fix) must
not narrow the audit's default scope (R-HARN-AUDIT-2.3) as a side
effect — the fix is "check that a cited path exists," not "check fewer
paths so existence-checking stays cheap." A change that adds the
check only under a new opt-in flag, leaving the default git-new-only
invocation unchanged, satisfies this requirement; a change that
removes the git-new-only default without adding an explicit
`--path`-equivalent for full-corpus runs would create a different,
undocumented default and must update `spec.md` §2 in the same change.

**R-HARN-AUDIT-4.4** This domain is not the only place in
`scripts/check/` that already performs referent-existence checking —
`spec.md` §5 records two scripts in this domain's own inventory that
do, today, verify a cited path or line resolves before reporting
success (`check_manifest.sh`'s corpus-vs-manifest diff and
`check_d3d9_conformance_manifest.sh`'s evidence-source/line-range and
`source_file`/function-name checks). R-HARN-AUDIT-4.1's requirement is
therefore not a novel capability this domain lacks the means to build;
it is a gap in one specific script that two sibling scripts in the
same directory already close for their own citation shapes, and any
future fix to `audit_perf_docs_sources.py` should draw on that existing
in-repo pattern rather than inventing a new one.

---

## 5. Diagnostic and Mode Surfaces Carry the Primary Contract

**R-HARN-AUDIT-5.1** Every command-line flag any script in this domain
accepts that changes its exit code, its checked-file set, or its
report content appears in `spec.md`'s mode table (§6), whether the
flag is exercised by the script's own Meson registration (§3) or only
available for manual invocation. Instantiates parent R-HARN-6.2.

**R-HARN-AUDIT-5.2** A flag whose entire purpose is to make a check in
this domain stricter — `check_d3d9_conformance_status.py`'s
`--fail-if-full-support-missing`, or an explicit `--path` list for
`audit_perf_docs_sources.py` — is bound by parent R-HARN-6.1/6.3 the
same as this domain's default invocation: it must actually evaluate
the condition it advertises and exit non-zero with a diagnostic naming
the failing case, not merely accept the flag and otherwise behave as
if it were absent. `spec.md` §6 records that both named flags above do
in fact evaluate their advertised condition when supplied, which is a
different claim from R-HARN-AUDIT-3.2's point that CI's own
registration does not supply them.

---

## 6. Environment Variable Ownership

**R-HARN-AUDIT-6.1** No script in this domain sets, exports, or reads
a `DXMT9_*`/`DXMT_*` environment variable to change *its own*
checking behavior — `spec.md` §7 records the verified grep across all
10 scripts. This mirrors `specs/experiments/harness/gate/requirements.md`
R-HARN-GATE-6.1 and `specs/experiments/harness/reduce/requirements.md`'s
equivalent, applied to this domain's own inventory.

**R-HARN-AUDIT-6.2** `assert_perf_counters.py` is a narrow, stated
exception to R-HARN-AUDIT-6.1's spirit, not a violation of it: it sets
`DXMT_PERF_COUNTERS=1` (`spec.md` §7) into the environment of the one
subprocess it directly launches and immediately asserts against — a
Meson-built native test executable it invokes as `argv[1:]` — not into
a shared experiment run another domain coordinates. Parent spec.md
§4's Rule 1 ("exactly one domain may set each contract-relevant
variable for a given run") is not violated here because the two
setters never share a run:
`specs/experiments/harness/reduce/spec.md` §"Environment Variables"
already records `DXMT_PERF_COUNTERS` as `runner`-owned for a real
catalogue/Wine experiment run (set in
`experiments/launchers/common.sh`), and this domain's own use is
scoped entirely to a one-shot native unit-test invocation that never
goes through the `runner` domain's pipeline at all. A future change
must not read R-HARN-AUDIT-6.1 as license to remove this line from
`spec.md`; the exception is real and must stay documented, not
resolved by pretending it does not exist.
