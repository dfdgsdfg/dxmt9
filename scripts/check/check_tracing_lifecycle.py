#!/usr/bin/env python3
"""Check the tracing-session TLC fixtures stay attached to production policy.

The semantic binding is the native truth-table test, which invokes the exact
constexpr predicates used by the session controller. This script checks that
the bounded model, expected counterexamples, predicates, and native test are
all present and continue to name the same lifecycle decisions.
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
TLA_DIR = ROOT / "specs/verification/tla"
MODEL = TLA_DIR / "TracingSessionLifecycle.tla"
PREDICATES = ROOT / "src/dxmt9/tracing/session_state.hpp"
NATIVE_TEST = ROOT / "tests/native/backend/tracing_lifecycle_spec.cpp"
CONCURRENCY_TEST = ROOT / "tests/native/backend/tracing_concurrency_spec.cpp"

COUNTEREXAMPLES = {
    "late-admission": "AdmittedOnlyDuringCollection",
    "premature-seal": "SealWaitsForQuiescence",
    "append-after-seal": "NoAppendToSealedStorage",
    "reuse-before-quiescence": "NoReuseWithLiveWriter",
    "stale-generation": "NoStaleGenerationAdmission",
}


def fail(message: str) -> None:
    raise SystemExit(message)


def read(path: Path) -> str:
    try:
        return path.read_text()
    except OSError as exc:
        fail(f"cannot read {path}: {exc}")


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def normalized(value: str) -> str:
    return re.sub(r"\s+", "", value)


def main() -> int:
    model = read(MODEL)
    predicates = read(PREDICATES)
    native_test = read(NATIVE_TEST)
    concurrency_test = read(CONCURRENCY_TEST)

    # Keep phase spelling synchronized with the production enum. Failed is
    # intentionally not in this successful-capture abstraction.
    phase_match = re.search(r"Phases\s*==\s*\{([^}]*)\}", model, re.S)
    enum_match = re.search(r"enum class TraceState\s*:\s*uint8_t\s*\{([^}]*)\}",
                           predicates, re.S)
    require(phase_match is not None and enum_match is not None,
            "model phases or production TraceState enum are missing")
    model_phases = set(re.findall(r'"([A-Za-z]+)"', phase_match.group(1)))
    production_phases = set(re.findall(r"\b(Idle|Preparing|Armed|Collecting|Closing|Draining|Finalizing|Sealed|Failed)\b",
                                       enum_match.group(1)))
    require(production_phases - {"Failed"} == model_phases,
            f"model/production phase drift: model={sorted(model_phases)!r}, "
            f"production={sorted(production_phases)!r}")

    # These are the production C++ decisions checked against model rows by the
    # native executable, not reimplemented here in Python.
    for name in ("shouldAdmit", "mayAppend", "maySeal"):
        require(re.search(rf"constexpr\s+bool\s+{name}\s*\(", predicates) is not None,
                f"production predicate {name} is missing")
        require(name in native_test,
                f"native truth-table test no longer calls {name}")
    require("mayAcquireWriter" in concurrency_test and
            "captureAdmissionGeneration" in concurrency_test and
            "closeAdmission" in concurrency_test and "openAdmission" in concurrency_test,
            "deterministic stale-snapshot test no longer exercises the production gate")
    for token in ("TraceState::Collecting", "TraceState::Closing",
                  "TraceState::Draining", "current_generation",
                  "requested_generation", "storage_sealed", "active_writers"):
        require(token in predicates, f"production lifecycle predicate lost {token}")
    policy_fragments = (
        "returnstate==TraceState::Collecting&&current_generation!=0&&current_generation==requested_generation;",
        "returnwriter_phase&&!storage_sealed&&current_generation!=0&&current_generation==requested_generation;",
        "returnstate==TraceState::Draining&&active_writers==0;",
    )
    compact_predicates = normalized(predicates)
    for fragment, label in zip(policy_fragments,
                               ("admission generation gate", "append generation/seal gate",
                                "quiescent seal gate")):
        require(fragment in compact_predicates,
                f"production {label} no longer matches the checked lifecycle contract")

    for invariant in ("AdmittedOnlyDuringCollection", "SealWaitsForQuiescence",
                      "NoAppendToSealedStorage", "NoReuseWithLiveWriter",
                      "NoStaleGenerationAppend", "NoStaleGenerationAdmission",
                      "WriterOwnsCurrentStorage"):
        require(re.search(rf"^{invariant}\s*==", model, re.M) is not None,
                f"required lifecycle invariant {invariant} is missing")
    require("writerGeneration = storageGeneration" in model and
            "storageSealed" in model and "requestGeneration" in model and
            "NoStaleGenerationAdmission" in model,
            "generation-owned gate/snapshot/storage relation is missing")

    base_cfg = read(TLA_DIR / "TracingSessionLifecycle.cfg")
    for invariant in ("TypeOK", "AdmittedOnlyDuringCollection",
                      "SealWaitsForQuiescence", "NoAppendToSealedStorage",
                      "NoReuseWithLiveWriter", "NoStaleGenerationAppend",
                      "NoStaleGenerationAdmission", "WriterOwnsCurrentStorage",
                      "SuccessfulReceiptIsSealed"):
        require(f"INVARIANT {invariant}" in base_cfg,
                f"base TLC configuration omits {invariant}")

    expected_files = set()
    for suffix, invariant in COUNTEREXAMPLES.items():
        path = TLA_DIR / f"TracingSessionLifecycle.{suffix}.counterexample.cfg"
        expected_files.add(path.name)
        cfg = read(path)
        require(f"INVARIANT {invariant}" in cfg,
                f"{path.name} no longer targets {invariant}")
        require("SPECIFICATION Spec" in cfg and "CHECK_DEADLOCK FALSE" in cfg,
                f"{path.name} must use the explicit safety contract")
    actual_files = {p.name for p in TLA_DIR.glob("TracingSessionLifecycle.*.counterexample.cfg")}
    require(actual_files == expected_files,
            f"lifecycle counterexample inventory drifted: {sorted(actual_files)!r}")

    print("tracing lifecycle model/code binding structure: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
