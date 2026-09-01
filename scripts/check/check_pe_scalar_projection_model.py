#!/usr/bin/env python3
"""Regex-audit the handwritten scalar projection model's production structure.

This is a structural freshness seam, not a TLA generator or semantic
refinement proof: it rejects drift in the exact
closed category/tuple/capacity contract, bounded table limits, production
settlement identifiers, and the finite model/counterexample inventory.
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / "src/d3d9/d3d9_pe_semantic_tokens.hpp"
RECORDER_STATE = ROOT / "src/d3d9/d3d9_pe_recorder_state.hpp"
DIAGNOSTICS_STATE = ROOT / "src/d3d9/d3d9_pe_diagnostics_state.hpp"
PRODUCER_HPP = ROOT / "src/d3d9/d3d9_pe_producer.hpp"
PRODUCER_CPP = ROOT / "src/d3d9/d3d9_pe_producer.cpp"
DEVICE_IMPL = ROOT / "src/d3d9/d3d9_pe_device_impl.hpp"
DEVICE = ROOT / "src/d3d9/d3d9_pe_device.cpp"
DEVICE_RECORDER = ROOT / "src/d3d9/d3d9_pe_device_recorder.cpp"
TLA = ROOT / "specs/verification/tla/PeRecorderScalarProjection.tla"
TLA_DIR = TLA.parent


EXPECTED_CATEGORIES = ("RenderState", "TextureStageState", "SamplerState")
EXPECTED_TUPLE_FIELDS = (
    "category", "key", "index", "value", "sourceOrdinal", "recordOrdinal"
)
EXPECTED_TLA_FIELDS = (
    "category", "key", "index", "value", "source", "record"
)
EXPECTED_MODES = (
    "Normal", "Missing", "Duplicate", "Value", "SourceOrdinal",
    "RecordOrdinal", "Category", "Key", "NoTokenMutation",
)
EXPECTED_COUNTEREXAMPLES = {
    "category": "Category",
    "duplicate": "Duplicate",
    "key": "Key",
    "missing": "Missing",
    "no-token": "NoTokenMutation",
    "record-ordinal": "RecordOrdinal",
    "source-ordinal": "SourceOrdinal",
    "value": "Value",
}


def fail(message: str) -> None:
    raise SystemExit(message)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def normalized(value: str) -> str:
    return re.sub(r"\s+", "", value)


def read(path: Path) -> str:
    try:
        return path.read_text()
    except OSError as exc:
        fail(f"cannot read {path}: {exc}")


def main() -> int:
    cpp = read(CPP)
    recorder_state = read(RECORDER_STATE)
    diagnostics_state = read(DIAGNOSTICS_STATE)
    producer_hpp = read(PRODUCER_HPP)
    producer_cpp = read(PRODUCER_CPP)
    device_impl = read(DEVICE_IMPL) + read(DEVICE)
    device_recorder = read(DEVICE_RECORDER)
    tla = read(TLA)

    enum = re.search(
        r"enum class ScalarSemanticCategory\s*:\s*std::uint8_t\s*\{(.*?)\};",
        cpp,
        re.S,
    )
    require(enum is not None, "scalar category enum is missing")
    categories = tuple(
        re.findall(r"\b(RenderState|TextureStageState|SamplerState)\b\s*,?", enum.group(1))
    )
    require(categories == EXPECTED_CATEGORIES,
            f"C++ scalar category order drifted: {categories!r}")
    for category in EXPECTED_CATEGORIES:
        require(
            re.search(rf"\b{category}\b", tla) is not None,
            f"TLA scalar category missing: {category}",
        )

    cpp_tuple = re.search(
        r"struct ScalarSemanticProjectionTuple\s*\{(.*?)\};", cpp, re.S
    )
    require(cpp_tuple is not None, "C++ projection tuple is missing")
    cpp_fields = tuple(
        re.findall(
            r"(?:ScalarSemanticCategory|std::uint32_t|std::uint64_t)\s+"
            r"(category|key|index|value|sourceOrdinal|recordOrdinal)\s*=",
            cpp_tuple.group(1),
        )
    )
    require(cpp_fields == EXPECTED_TUPLE_FIELDS,
            f"C++ projection tuple fields drifted: {cpp_fields!r}")

    tla_tuple = re.search(r"BoundRecord\s*==\s*\[(.*?)\]", tla, re.S)
    require(tla_tuple is not None, "TLA BoundRecord tuple is missing")
    tla_fields = tuple(re.findall(r"\b(category|key|index|value|source|record)\s*:",
                                  tla_tuple.group(1)))
    require(tla_fields == EXPECTED_TLA_FIELDS,
            f"TLA projection tuple fields drifted: {tla_fields!r}")

    capacity = re.search(
        r"constexpr\s+std::size_t\s+kPeScalarSemanticTokenCapacity\s*=\s*(.*?);",
        cpp,
        re.S,
    )
    require(capacity is not None, "scalar ledger capacity expression is missing")
    require(
        normalized(capacity.group(1)) == "256u+(8u*64u)+(20u*16u)",
        "scalar ledger capacity expression drifted from the bounded domains",
    )
    array_contract = (
        (r"std::array\s*<\s*std::uint64_t\s*,\s*4u\s*>\s+renderMask_", "render mask"),
        (r"std::array\s*<\s*std::uint64_t\s*,\s*8u\s*>\s+tssMask_", "TSS mask"),
        (r"std::array\s*<\s*std::uint16_t\s*,\s*20u\s*>\s+samplerMask_", "sampler mask"),
        (r"std::array\s*<\s*std::uint64_t\s*,\s*256u\s*>\s+renderOrdinals_", "render ordinals"),
        (r"std::array\s*<\s*std::uint64_t\s*,\s*8u\s*\*\s*64u\s*>\s+tssOrdinals_", "TSS ordinals"),
        (r"std::array\s*<\s*std::uint64_t\s*,\s*20u\s*\*\s*16u\s*>\s+samplerOrdinals_", "sampler ordinals"),
    )
    for pattern, label in array_contract:
        require(re.search(pattern, cpp) is not None,
                f"scalar ledger {label} extent drifted")

    valid = re.search(
        r"constexpr\s+bool\s+valid\(.*?\) const noexcept\s*\{(.*?)\n\s*\}\n\n\s*constexpr\s+std::uint64_t\s+get",
        cpp,
        re.S,
    )
    require(valid is not None, "scalar ledger validity function is missing")
    for condition, label in (
        ("if (key >= 256u || index != 0u) return false;", "render limits"),
        ("if (key >= 8u || index >= 64u) return false;", "TSS limits"),
        ("if (key >= 20u || index >= 16u) return false;", "sampler limits"),
    ):
        require(condition in valid.group(1), f"scalar ledger {label} drifted")

    require(re.search(r"bool\s+acceptPreparedSparseState\s*\(", producer_hpp)
            is not None, "production scalar settlement helper is missing")
    for identifier, source in (
        ("tokens->project(", producer_cpp),
        ("tokens->canConsumeProjected(", producer_cpp),
        ("tokens->consumeProjected(", producer_cpp),
        ("semanticOwner->nextRecordOrdinal()", device_recorder),
        ("input.recordOrdinal", device_recorder),
        ("semanticTokens->canRecord(", device_impl),
        ("semanticTokens->record(", device_impl),
        ("scalarSemanticObserver()", device_impl + device_recorder),
        ("acceptPreparedSparseState(", device_impl + device_recorder),
    ):
        require(identifier in source, f"production seam identifier missing: {identifier}")
    for bypass in (
        "acceptRenderStateBatch(",
        "acceptTextureStageStateBatch(",
        "acceptSamplerStateBatch(",
    ):
        require(bypass not in device_impl + device_recorder,
                f"production scalar bypass remains: {bypass}")
    require("PeScalarSemanticTokenLedger" not in recorder_state,
            "always-on recorder must not name the scalar ledger")
    require("std::unique_ptr<dxmt9::d3d9::pe::PeScalarSemanticTokenLedger>"
            in diagnostics_state,
            "cold diagnostics owner no longer owns the nullable scalar ledger")
    require("DXMT9_PE_SCALAR_SEMANTIC_OBSERVER" in device_impl,
            "default-off scalar observer gate is missing")
    require("scalarSemanticObserver = false" in diagnostics_state,
            "scalar observer config must default off")
    require("strictlyOrdered(state.renderStates" in producer_cpp and
            "pending != entry.value" in producer_cpp,
            "default production order/value preflight is missing")
    require("sizeof(PeRecorderState) == 104120u" in recorder_state and
            "sizeof(PeRecorderState) == 103376u" in recorder_state,
            "canonical x64/x86 recorder footprint pins are missing")

    require("Handwritten exact bounded refinement" in tla,
            "TLA artifact must remain explicitly handwritten")
    require("/\\ noTokenCount = 0" in tla,
            "AcceptNoToken must be one-shot bounded")
    require("Mode = \"NoTokenMutation\"" in tla,
            "no-token mutation mode is missing")
    require("noTokenPending' =" in tla,
            "no-token witness mutation seam is missing")
    require("TerminalStutter" not in tla and "terminalTick" not in tla,
            "artificial terminal toggle must not enter the scalar model")
    for action in ("Prepare", "Retry", "Discard", "Accept", "AcceptNoToken"):
        require(re.search(rf"^{action}\s*==", tla, re.M) is not None,
                f"TLA projection action missing: {action}")
    for mode in EXPECTED_MODES:
        require(f'Mode = "{mode}"' in tla,
                f"TLA projection mode missing: {mode}")

    base_cfg = read(TLA_DIR / "PeRecorderScalarProjection.cfg")
    require('CONSTANT Mode = "Normal"' in base_cfg,
            "good scalar projection cfg must use Normal mode")
    require("SPECIFICATION Spec" in base_cfg and "CHECK_DEADLOCK FALSE" in base_cfg,
            "good scalar projection cfg must use the explicit terminating-safety contract")
    require("INVARIANT TypeOK ExactProjection NoTokenIsExplicit" in base_cfg,
            "good scalar projection cfg must check finite/no-token invariants")
    expected_files = set()
    for suffix, mode in EXPECTED_COUNTEREXAMPLES.items():
        path = TLA_DIR / f"PeRecorderScalarProjection.{suffix}.counterexample.cfg"
        expected_files.add(path.name)
        cfg = read(path)
        require(f'CONSTANT Mode = "{mode}"' in cfg,
                f"{path.name} mode drifted")
        require("SPECIFICATION Spec" in cfg and "CHECK_DEADLOCK FALSE" in cfg,
                f"{path.name} must use Spec plus explicit deadlock suppression")
        expected_invariant = "NoTokenIsExplicit" if suffix == "no-token" else "ExactProjection"
        require(f"INVARIANT {expected_invariant}" in cfg,
                f"{path.name} invariant drifted")
    actual_files = {
        path.name for path in TLA_DIR.glob("PeRecorderScalarProjection.*.counterexample.cfg")
    }
    require(actual_files == expected_files,
            f"scalar projection counterexample inventory drifted: {sorted(actual_files)!r}")

    print("scalar projection regex structural freshness: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
