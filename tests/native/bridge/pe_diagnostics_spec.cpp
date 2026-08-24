#include "d3d9_pe_diagnostics_state.hpp"
#include "d3d9_pe_stateblock_fault.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::atomic<std::uint64_t> gAllocations{0};

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

std::string readTextFile(const std::filesystem::path &path) {
  std::ifstream file(path);
  check(static_cast<bool>(file), "source-contract input exists");
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

void checkContains(std::string_view source, std::string_view needle,
                   std::string_view message) {
  check(source.find(needle) != std::string_view::npos, message);
}

void checkNotContains(std::string_view source, std::string_view needle,
                      std::string_view message) {
  check(source.find(needle) == std::string_view::npos, message);
}

void checkBefore(std::string_view source, std::string_view first,
                 std::string_view second, std::string_view message) {
  const auto firstPos = source.find(first);
  const auto secondPos = firstPos == std::string_view::npos
      ? std::string_view::npos
      : source.find(second, firstPos);
  check(firstPos != std::string_view::npos &&
            secondPos != std::string_view::npos && firstPos < secondPos,
        message);
}

std::size_t countOccurrences(std::string_view source,
                             std::string_view needle) {
  std::size_t count = 0;
  for (std::size_t pos = 0; (pos = source.find(needle, pos)) !=
                             std::string_view::npos;
       pos += needle.size()) {
    ++count;
  }
  return count;
}

void testOptionalOwnerAllocationAndConfig() {
  const PeDiagnosticsConfig disabled{};
  const auto disabledBefore = gAllocations.load(std::memory_order_relaxed);
  auto absent = makePeDiagnosticsState(nullptr, disabled);
  const auto disabledAfter = gAllocations.load(std::memory_order_relaxed);
  check(!absent, "all diagnostic gates off keeps the cold owner absent");
  check(disabledAfter == disabledBefore,
        "all diagnostic gates off performs no owner allocation");

  const std::array configs = {
      PeDiagnosticsConfig{.recorderStats = true},
      PeDiagnosticsConfig{.recorderChunkLog = true},
      PeDiagnosticsConfig{.statsDecimationN = 17},
      PeDiagnosticsConfig{.vsConstSetterRange = true},
      PeDiagnosticsConfig{.moduleMap = true},
      PeDiagnosticsConfig{.threadSampler = true, .threadSamplerHz = 733},
      PeDiagnosticsConfig{.debugLog = true},
      PeDiagnosticsConfig{.scalarSemanticObserver = true},
      PeDiagnosticsConfig{.moduleMap = true, .threadSampler = true},
      PeDiagnosticsConfig{.moduleMap = true, .debugLog = true},
      PeDiagnosticsConfig{.threadSampler = true, .debugLog = true},
      PeDiagnosticsConfig{.moduleMap = true, .threadSampler = true,
                          .debugLog = true},
      PeDiagnosticsConfig{.vsConstSetterRange = true, .moduleMap = true,
                          .threadSampler = true, .debugLog = true},
  };
  for (const auto &config : configs) {
    const auto before = gAllocations.load(std::memory_order_relaxed);
    auto diagnostics = makePeDiagnosticsState(nullptr, config);
    const auto after = gAllocations.load(std::memory_order_relaxed);
    check(diagnostics != nullptr,
          "each individual diagnostic gate constructs the cold owner");
    const auto expectedAllocations =
        config.scalarSemanticObserver ? 2u : 1u;
    check(after == before + expectedAllocations,
          "the optional scalar observer adds only its cold allocation");
    check(diagnostics->config.recorderStats == config.recorderStats &&
              diagnostics->config.recorderChunkLog ==
                  config.recorderChunkLog &&
              diagnostics->config.statsDecimationN ==
                  config.statsDecimationN &&
              diagnostics->config.vsConstSetterRange ==
                  config.vsConstSetterRange &&
              diagnostics->config.moduleMap == config.moduleMap &&
              diagnostics->config.threadSampler == config.threadSampler &&
              diagnostics->config.scalarSemanticObserver ==
                  config.scalarSemanticObserver &&
              diagnostics->config.threadSamplerHz == config.threadSamplerHz,
          "the owner preserves the resolved immutable diagnostic config");
    const bool scopeExpected =
        config.recorderStats || config.statsDecimationN != 0u;
    check(diagnostics->gates.callScope == scopeExpected &&
              diagnostics->gates.hotSetterTimer == scopeExpected,
          "call and setter scopes are enabled only by their owning gates");
    check(diagnostics->gates.moduleMap == config.moduleMap &&
              diagnostics->gates.threadSampler == config.threadSampler &&
              diagnostics->gates.debugLog == config.debugLog &&
              diagnostics->gates.scalarSemanticObserver ==
                  config.scalarSemanticObserver,
          "observer-only gates remain independently cached");
    check(static_cast<bool>(diagnostics->scalarSemanticTokens) ==
              config.scalarSemanticObserver,
          "the exact semantic ledger exists only behind its own gate");
  }
}

void testNullableDiagnosticDispatchAndClock() {
  std::uint32_t calls = 0;
  peDiagnosticsCall(nullptr, [&](PeDiagnosticsState &) noexcept { ++calls; });
  check(calls == 0u, "null diagnostic dispatch invokes no callback");

  auto diagnostics = makePeDiagnosticsState(
      nullptr, PeDiagnosticsConfig{.recorderStats = true});
  peDiagnosticsCall(diagnostics.get(),
                    [&](PeDiagnosticsState &) noexcept { ++calls; });
  check(calls == 1u, "enabled diagnostic dispatch invokes one callback");

  std::uint32_t clockCalls = 0;
  const auto disabledClock = peDiagnosticsRead(
      nullptr, [&](PeDiagnosticsState &) noexcept {
        ++clockCalls;
        return std::int64_t{91};
      });
  check(disabledClock == 0 && clockCalls == 0u,
        "null diagnostic clock path performs no clock callback");
  const auto enabledClock = peDiagnosticsRead(
      diagnostics.get(), [&](PeDiagnosticsState &) noexcept {
        ++clockCalls;
        return std::int64_t{91};
      });
  check(enabledClock == 91 && clockCalls == 1u,
        "enabled diagnostic clock path performs exactly one clock callback");
}

void testStateBlockFaultSelector() {
  const auto config = peStateBlockFaultConfigFromString(
      "capture_pre=0x80070057,apply_entered=0x8876086c,bridge_entered");
  check((config.mask & peStateBlockFaultBit(
                         PeStateBlockFaultPoint::CapturePre)) != 0u,
        "fault selector parses pre-effect point");
  check(config.hresult[static_cast<std::size_t>(
            PeStateBlockFaultPoint::CapturePre)] ==
            static_cast<std::int32_t>(0x80070057u),
        "fault selector preserves exact HRESULT");
  check(config.hresult[static_cast<std::size_t>(
            PeStateBlockFaultPoint::ApplyEntered)] ==
            static_cast<std::int32_t>(0x8876086cu),
        "fault selector preserves entered HRESULT");
  check((config.mask & peStateBlockFaultBit(
                         PeStateBlockFaultPoint::BridgeEntered)) != 0u,
        "fault selector parses generic bridge point");
  const auto empty = peStateBlockFaultConfigFromString(nullptr);
  check(empty.mask == 0u, "fault selector is default-off");
}

void testSourceContracts(const std::filesystem::path &root) {
  const auto diagnostics = readTextFile(
      root / "src/d3d9/d3d9_pe_diagnostics_state.hpp");
  const auto observer = readTextFile(
      root / "src/d3d9/d3d9_pe_diagnostic_observer.hpp");
  const auto child =
      readTextFile(root / "src/d3d9/d3d9_pe_device_child.hpp");
  const auto validatedObject = readTextFile(
      root / "src/d3d9/d3d9_pe_validated_object.hpp");
  const auto recorderFacade = readTextFile(
      root / "src/d3d9/d3d9_pe_recorder_flush_facade.inc.hpp");
  auto device = readTextFile(root / "src/d3d9/d3d9_pe_device_impl.hpp");
  for (const std::string_view fragment : {
           "d3d9_pe_device_state_core.inc.hpp",
           "d3d9_pe_device_state_texture_fvf.inc.hpp",
           "d3d9_pe_device_state_shader_stream.inc.hpp"}) {
    const std::string marker = "#include \"" + std::string(fragment) + "\"";
    const auto markerPos = device.find(marker);
    check(markerPos != std::string::npos,
          "device header contains each ordered class fragment");
    device.replace(markerPos, marker.size(),
                   readTextFile(root / "src/d3d9" / fragment));
  }
  check(std::count(device.begin(), device.end(), '\n') >= 6700,
        "expanded device header retains the complete declaration surface");
  const auto recorder =
      readTextFile(root / "src/d3d9/d3d9_pe_device_recorder.cpp");
  const auto recorderHeader =
      readTextFile(root / "src/d3d9/d3d9_pe_recorder.hpp");
  const auto deviceCold =
      readTextFile(root / "src/d3d9/d3d9_pe_device_com_cold.cpp");
  const auto recorderState =
      readTextFile(root / "src/d3d9/d3d9_pe_recorder_state.hpp");
  const auto stateBlockTransaction = readTextFile(
      root / "src/d3d9/d3d9_pe_stateblock_transaction.hpp");
  const auto deviceOwner =
      readTextFile(root / "src/d3d9/d3d9_pe_device.cpp");
  const auto misc = readTextFile(
      root / "src/d3d9/d3d9_pe_device_child_misc.cpp");
  const auto fault = readTextFile(
      root / "src/d3d9/d3d9_pe_stateblock_fault.hpp");
  const auto surface = readTextFile(
      root / "src/d3d9/d3d9_pe_device_child_surface.cpp");
  const auto registry =
      readTextFile(root / "src/d3d9/device_c_chunk_registry.cpp");

  for (const std::string_view field : {
           "vsConstSetterRangePerf_", "peRecorderStats_",
           "peChunkAppendDecimatedStats_", "peAppendTypeCounts_",
           "peAppendTypeBytes_", "peAppendPhaseEncode_",
           "peAppendPhaseFlush_", "peConstFlushDecimatedStats_",
           "peEntryConstDecimatedStats_", "peEntryDrawDecimatedStats_",
           "peEntryStateDecimatedStats_", "peDrawPhaseSwvpDecimatedStats_",
           "peDrawPhaseRecordDecimatedStats_",
           "peDrawPacketDecimatedStats_", "peStatsDecimationPresents_",
           "peThreadSampler_", "peThreadSamplerPresents_",
           "peThreadSamplerPresentThreadChecked_",
           "peRecorderStatsLastLoggedCommitCount_",
           "peRecorderLastChunkReturnNs_",
           "peRecorderCurrentChunkFirstAppendNs_",
           "peRecorderLastAppendReturnNs_",
           "peRecorderLastAppendCallEntryNs_",
           "peRecorderLastAppendCallExitNs_",
           "peRecorderLastAppendRecordType_",
           "peRecorderBetweenCallsActive_",
           "peRecorderBetweenCallsStartNs_",
           "peRecorderBetweenCallFamilySamples_",
           "peRecorderBetweenCallNameSamples_",
           "peRecorderBetweenCallNameCpuNsTotal_",
           "peRecorderBetweenCallNameCpuNsMax_",
           "peRecorderBetweenLastCallFamily_",
           "peRecorderBetweenLastCallName_",
           "peRecorderBetweenLastCallExitNs_",
           "peRecorderBetweenCallTransitionSamples_",
           "peRecorderBetweenCallTransitionNsTotal_",
           "peRecorderBetweenCallTransitionNsMax_",
           "peRecorderBetweenCallNameTransitionSamples_",
           "peRecorderBetweenCallNameTransitionNsTotal_",
           "peRecorderBetweenCallNameTransitionNsMax_",
           "peRecorderBetweenCallNameTransitionSites_",
           "peRecorderFocusBetweenCallNameTransitionSites_",
           "peRecorderBetweenCallBodyCalls_",
           "peRecorderBetweenCallBodyCpuNsTotal_",
           "peRecorderBetweenCallBodyCpuNsMax_",
           "pePresentCadenceOrdinal_", "pePresentCadencePendingOrdinal_",
           "pePresentCallMilestonePendingOrdinal_", "pePresentCallCount_",
           "pePresentCallMilestoneMask_", "pePresentChunkPendingOrdinal_",
           "pePresentRecordPendingOrdinal_",
           "pePresentRecordMilestoneMask_", "pePresentCadenceReturnNs_",
       }) {
    checkContains(diagnostics, field,
                  "diagnostic-only field remains in the cold owner");
  }
  checkContains(device, "std::unique_ptr<PeDiagnosticsState> diagnostics_{};",
                "device stores one nullable diagnostic owner pointer");
  checkContains(recorderState,
                "PeStateBlockTransactionState stateBlockTransaction{};",
                "recorder state owns the closed StateBlock transaction");
  checkContains(recorderHeader, "stateBlockFaultPreCalls",
                "recorder exposes pre-fault observability");
  checkContains(recorderHeader, "stateBlockFaultEnteredCalls",
                "recorder exposes entered-fault observability");
  checkContains(recorderHeader, "stateBlockFaultLastHr",
                "recorder exposes fault HRESULT observability");
  checkContains(device,
                "dxmt9::d3d9::pe::PeRecorderState recorderState_{};",
                "device stores the hot recorder owner directly");
  for (const std::string_view alias : {
           "bool& recorderLockRequired_",
           "std::recursive_mutex& recorderMutex_",
           "PeHotStateShadow& peState_", "PeConstShadowBlock& peConsts_",
           "PeBindingView& bindingScratch_", "PeSparseScratch& sparseScratch_",
           "CommandChunkBuilder& commandChunk_",
           "bool& commandChunkNegotiated_"}) {
    checkNotContains(device, alias,
                     "device must not retain audited recorder-field aliases");
  }
  checkContains(child, "class D3D9StateBlockShadow",
                "StateBlock wrapper snapshot is a closed class");
  checkContains(child, "class Writer",
                "wrapper snapshot exposes a writer capability");
  checkContains(child, "class Snapshot",
                "wrapper snapshot exposes an immutable snapshot capability");
  checkContains(stateBlockTransaction,
                "StateBlockVertexShaderRef stagedVertexShader_{};",
                "Apply staging remains kind-qualified inside its owner");
  checkContains(validatedObject, "struct D3D9PeValidatedObject",
                "wrapper admission returns a typed local capability");
  checkContains(validatedObject, "constexpr Public* publicIdentity() const noexcept",
                "validated capability preserves the exact interface address");
  checkContains(validatedObject, "Wire wire_{};",
                "validated capability carries the kind-qualified wire ref");
  checkContains(validatedObject, "constexpr D3D9PeValidatedObject() noexcept = default;",
                "validated object construction is private");
  checkContains(child, "d3d9_pe_validated_object.hpp",
                "child surface imports the private validation object shape");
  checkNotContains(child, "D3D9PeValidatedObjectWriter::clear",
                   "child surface cannot mint validation evidence");
  std::size_t validatedWriterIncludes = 0;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(root / "src/d3d9")) {
    if (!entry.is_regular_file()) continue;
    const auto extension = entry.path().extension();
    if (extension != ".cpp" && extension != ".hpp") continue;
    validatedWriterIncludes += countOccurrences(
        readTextFile(entry.path()),
        "#include \"d3d9_pe_validated_object_writer.hpp\"");
  }
  check(validatedWriterIncludes == 4u,
        "only the four concrete validator TUs can mint validation evidence");
  checkNotContains(child, "operator()(void*",
                   "StateBlock ownership policies reject untyped pointers");
  checkNotContains(child, "reinterpret_cast<IUnknown",
                   "StateBlock ownership preserves the exact interface address");
  checkContains(child, "if (auto* object = value.raw()) object->AddRef();",
                "typed retain policy calls the original interface pointer");
  checkContains(child, "if (auto* object = value.raw()) object->Release();",
                "typed release policy calls the original interface pointer");
  check(!std::filesystem::exists(
            root / "src/d3d9/d3d9_pe_trusted_handles.hpp"),
        "convention-only trusted wire extractors are removed");
  checkNotContains(device, "std::array<void*, kPeTextureSlots> stagedApply",
                   "device cannot bypass typed StateBlock staging");
  checkContains(diagnostics, "struct PeDiagnosticsFeatureGates",
                "diagnostic ownership has feature-specific cached gates");
  checkContains(diagnostics,
                "std::unique_ptr<dxmt9::d3d9::pe::PeScalarSemanticTokenLedger>",
                "the exact scalar witness is a nullable cold owner");
  checkNotContains(recorderState, "PeScalarSemanticTokenLedger",
                   "the always-on recorder does not own the scalar ledger");
  checkContains(device, "DXMT9_PE_SCALAR_SEMANTIC_OBSERVER",
                "the cold scalar witness has an explicit default-off gate");
  checkContains(device, "if (!diagnostics->gates.callScope)",
                "unrelated diagnostic gates skip call scope construction");
  checkContains(device, "if (!diagnostics->gates.hotSetterTimer)",
                "unrelated diagnostic gates skip setter timer construction");
  checkContains(device, "#define dxmt9DeviceDebugLog(...)",
                "disabled device debug logging is guarded at each call site");
  checkContains(recorder, "peDiagnosticsRead(\n                    chunkDiagnostics",
                "chunk commit clocks are behind the nullable owner gate");
  checkContains(device, "const auto t0 = phase.begin();",
                "append phase clocks use the nullable clock gate");
  checkNotContains(
      device,
      "dxmt9PeArmDecimatedScope(peEntryScope, diagnostics_ ? "
      "&diagnostics_->peEntryStateDecimatedStats_",
      "hot state setters use one combined nullable diagnostic scope");
  checkNotContains(
      device,
      "dxmt9PeArmDecimatedScope(peEntryScope, diagnostics_ ? "
      "&diagnostics_->peEntryDrawDecimatedStats_",
      "draw entry and call tracking share one nullable diagnostic scope");
  const auto setRenderBegin = device.find(
      "HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state");
  const auto setRenderEnd = setRenderBegin == std::string::npos
      ? std::string::npos
      : device.find("HRESULT STDMETHODCALLTYPE GetRenderState", setRenderBegin);
  check(setRenderBegin != std::string::npos && setRenderEnd != std::string::npos,
        "SetRenderState source contract is present");
  const std::string_view setRenderBody(
      device.data() + setRenderBegin, setRenderEnd - setRenderBegin);
  checkBefore(
      setRenderBody,
      "if (!recorderState_.stateBlockTransaction.writeAllowed()) {\n            return D3DERR_DEVICELOST;\n        }\n        PeDiagnosticsState* const diagnostics",
      "return setRenderStateCore(state, value, peNullHotSetter_);",
      "SetRenderState fail-stops before its diagnostic/core bypass");
  checkNotContains(device,
                   "PeStateBlockTransactionState& stateBlockTransaction_",
                   "device has no dependent StateBlock transaction pointer");
  const auto appendRecordBegin = device.find(
      "HRESULT appendRecord(uint32_t type, size_t sizeHint, EmitFn emit)");
  const auto appendRecordEnd = appendRecordBegin == std::string::npos
      ? std::string::npos
      : device.find("HRESULT appendDrawPrimitiveRecord", appendRecordBegin);
  check(appendRecordBegin != std::string::npos &&
            appendRecordEnd != std::string::npos,
        "common appendRecord source contract is present");
  const std::string_view appendRecordBody(
      device.data() + appendRecordBegin, appendRecordEnd - appendRecordBegin);
  checkBefore(
      appendRecordBody,
      "if (recorderState_.stateBlockTransaction.isPoisoned())",
      "if (!recorderState_.commandChunkNegotiated || bytes == 0u",
      "poison stops every writer before negotiation and capacity work");
  const auto flushBegin = recorder.find(
      "HRESULT D3D9DeviceImpl::flushPendingCommandChunk(");
  const auto flushEnd = flushBegin == std::string::npos
      ? std::string::npos
      : recorder.find("HRESULT D3D9DeviceImpl::flushPeRecorder(", flushBegin);
  check(flushBegin != std::string::npos && flushEnd != std::string::npos,
        "production flush source contract is present");
  const std::string_view flushBody(
      recorder.data() + flushBegin, flushEnd - flushBegin);
  checkBefore(
      flushBody,
      "if (flushAction == PeRecorderFlushAction::RejectPoisoned)",
      "if (!recorderState_.commandChunkNegotiated)",
      "ordinary flush rejects poison before negotiation, seal, or bridge entry");
  checkContains(
      deviceCold,
      "D3D9DeviceImpl::~D3D9DeviceImpl() {\n    (void)flushPeRecorder(\n        PeRecorderFlushReason::Destructor,\n        PeRecorderFlushDisposition::DiscardForRecovery);",
      "destructor unconditionally selects recovery discard");
  constexpr std::string_view resetDispositionCall =
      "peRecorderResetDisposition(\n            recorderState_.stateBlockTransaction.isPoisoned())";
  const auto resetBegin = deviceCold.find(
      "HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::Reset(");
  const auto resetEnd = deviceCold.find(
      "HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::ResetEx(", resetBegin);
  check(resetBegin != std::string::npos && resetEnd != std::string::npos,
        "Reset source contract is present");
  checkContains(std::string_view(deviceCold).substr(resetBegin,
                                                     resetEnd - resetBegin),
                resetDispositionCall,
                "Reset selects disposition from poisoned recovery state");
  const auto resetExBegin = resetEnd;
  const auto resetExEnd = deviceCold.find(
      "HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetDisplayModeEx(",
      resetExBegin);
  check(resetExEnd != std::string::npos,
        "ResetEx source contract has a bounded body");
  checkContains(std::string_view(deviceCold).substr(
                    resetExBegin, resetExEnd - resetExBegin),
                resetDispositionCall,
                "ResetEx selects disposition from poisoned recovery state");
  checkContains(
      device,
      "HRESULT AppendQueryIssueForChild(\n        std::uint32_t flags",
      "Query::Issue remains routed through the common poisoned append gate");
  checkBefore(
      setRenderBody,
      "if (!diagnostics || !diagnostics->gates.hotSetterTimer) {\n            return setRenderStateCore",
      "PeHotStateSetterTimer hotSetter(\n            *this, *diagnostics",
      "SetRenderState branches before the enabled timer lifetime begins");
  const auto setFvfBegin = device.find(
      "HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) noexcept override");
  const auto setFvfEnd = setFvfBegin == std::string::npos
      ? std::string::npos
      : device.find("HRESULT STDMETHODCALLTYPE GetFVF", setFvfBegin);
  check(setFvfBegin != std::string::npos && setFvfEnd != std::string::npos,
        "SetFVF noexcept source contract is present");
  const std::string_view setFvfBody(
      device.data() + setFvfBegin, setFvfEnd - setFvfBegin);
  checkBefore(
      setFvfBody,
      "resolveImplicitDeclForFvf(fvf, &implicitDecl);",
      "fvf_ = fvf;",
      "SetFVF resolves its implicit declaration before shadow mutation");
  checkBefore(
      device,
      "if (!diagnostics || !diagnostics->gates.callScope) {\n            return drawPrimitiveCore",
      "PeCallScope peCall(\n            *diagnostics, \"DrawPrimitive\"",
      "DrawPrimitive branches before the enabled call scope lifetime begins");
  checkBefore(
      surface,
      "if (!diagnostics_)\n      return getDescCore",
      "D3D9PeChildCallScope peCall(*diagnostics_, \"Surface::GetDesc\"",
      "Surface::GetDesc branches before the enabled child scope lifetime");
  checkContains(device, "__attribute__((always_inline)) noexcept",
                "generic hot entry bodies are forced through the branch");

  for (const std::string_view removed : {
           "NotifyPeFirstCallAfterPresentForChild",
           "PushPeCallScopeForChild", "NotifyPeCallScopeReturnForChild",
           "PopPeCallScopeForChild",
       }) {
    checkNotContains(child, removed,
                     "diagnostic virtual is absent from recorder protocol");
  }
  checkContains(recorderFacade,
                "virtual HRESULT FlushPeRecorderForChild() noexcept = 0;",
                "recorder protocol flush virtual remains first");
  check(countOccurrences(recorderFacade, "virtual ") == 29u,
        "recorder protocol retains exactly 29 virtual declarations");
  check(countOccurrences(recorderFacade, "noexcept") == 29u,
        "every recorder protocol virtual is noexcept");
  checkContains(child,
                "#include \"d3d9_pe_recorder_flush_facade.inc.hpp\"",
                "child header includes the ordered callback facade fragment");
  checkNotContains(observer, "virtual ",
                   "the child diagnostic observer is concrete and nonvirtual");
  checkContains(deviceOwner,
                "HRESULT D3D9DeviceImpl::FlushPeRecorderForChild() noexcept",
                "device key function remains in the hot owning TU");

  checkContains(misc, "std::atomic<ULONG> refs_{1};",
                "StateBlock retains its atomic COM lifetime exception");
  checkContains(misc, "const ULONG refs = refs_.fetch_add(1) + 1;",
                "StateBlock AddRef stays atomic");
  checkContains(misc, "const ULONG refs = refs_.fetch_sub(1) - 1;",
                "StateBlock Release stays atomic");
  checkContains(fault, "DXMT9_PE_STATEBLOCK_FAULT",
                "StateBlock fault seam is explicitly named");
  checkBefore(misc, "const HRESULT hr = hr32(dxmt9c_stateblock_capture(sb_));",
              "dxmt9PeConsumeStateBlockEnteredFault(",
              "CaptureEntered is sampled only after the backend call");
  check(countOccurrences(misc,
                         "if (diagnostics_)\n        diagnostics_->notifyStateBlockFault(true") >= 2u,
        "Capture and Apply entered observability tolerate a null observer");
  const auto captureBegin = misc.find("HRESULT STDMETHODCALLTYPE Capture()");
  const auto captureEnd = misc.find("HRESULT STDMETHODCALLTYPE Apply()", captureBegin);
  check(captureBegin != std::string::npos && captureEnd != std::string::npos,
        "Capture source contract is present");
  const auto captureBody = std::string_view(misc).substr(
      captureBegin, captureEnd - captureBegin);
  checkBefore(captureBody, "PeStateBlockFaultPoint::CapturePre",
              "dxmt9c_stateblock_capture(sb_)",
              "CapturePre precedes the sole backend call");
  check(countOccurrences(misc, "dxmt9c_stateblock_capture(sb_)") == 1u,
        "Capture has one backend call site");
  checkBefore(misc, "const HRESULT hr = hr32(dxmt9c_stateblock_apply(sb_));",
              "dxmt9PeConsumeStateBlockEnteredFault(",
              "ApplyEntered is sampled only after the backend call");
  const auto applyBegin = misc.find("HRESULT STDMETHODCALLTYPE Apply()");
  const auto applyEnd = misc.find("/* ── SwapChain", applyBegin);
  check(applyBegin != std::string::npos && applyEnd != std::string::npos,
        "Apply source contract is present");
  const auto applyBody = std::string_view(misc).substr(
      applyBegin, applyEnd - applyBegin);
  checkBefore(applyBody, "PeStateBlockFaultPoint::ApplyPre",
              "dxmt9c_stateblock_apply(sb_)",
              "ApplyPre precedes the sole backend call");
  check(countOccurrences(misc, "dxmt9c_stateblock_apply(sb_)") == 1u,
        "Apply has one backend call site");
  checkBefore(deviceCold,
              "HRESULT hr = hr32(dxmt9c_device_end_state_block(dev_, &sb));",
              "dxmt9PeConsumeStateBlockEnteredFault(",
              "EndEntered is sampled only after the backend call");
  const auto endBegin = deviceCold.find(
      "HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::EndStateBlock(");
  const auto endEnd = deviceCold.find(
      "HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetTextureStageState(",
      endBegin);
  check(endBegin != std::string::npos && endEnd != std::string::npos,
        "End source contract is present");
  const auto endBody = std::string_view(deviceCold).substr(
      endBegin, endEnd - endBegin);
  checkBefore(endBody, "PeStateBlockFaultPoint::EndPre",
              "dxmt9c_device_end_state_block(dev_, &sb)",
              "EndPre precedes the sole backend call");
  check(countOccurrences(deviceCold,
                         "dxmt9c_device_end_state_block(dev_, &sb)") == 1u,
        "End has one backend call site");
  checkContains(misc, "ULONG refs_ = 1;",
                "ordinary child COM wrappers retain non-atomic ownership");
  checkContains(device, "ULONG        refs_    = 1;",
                "the device retains ordinary non-atomic COM ownership");
  checkContains(registry, "WireRegistryRetainCallbackScope callbackScope;",
                "registry retain callbacks keep a scoped re-entry guard");
  checkNotContains(registry,
                   "DXMT_ASSERT(false && \"WireObjectRegistry retain callback re-entry\")",
                   "registry re-entry fails closed in every build type");
}

}  // namespace

void *operator new(std::size_t size) {
  if (void *storage = std::malloc(size)) {
    gAllocations.fetch_add(1u, std::memory_order_relaxed);
    return storage;
  }
  throw std::bad_alloc();
}

void operator delete(void *storage) noexcept {
  std::free(storage);
}

void operator delete(void *storage, std::size_t) noexcept {
  std::free(storage);
}

int main(int argc, char **argv) {
  try {
    check(argc == 2, "project source root argument is present");
    testOptionalOwnerAllocationAndConfig();
    testNullableDiagnosticDispatchAndClock();
    testStateBlockFaultSelector();
    testSourceContracts(argv[1]);
  } catch (const TestFailure &error) {
    std::cerr << "pe_diagnostics_spec failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "pe_diagnostics_spec passed\n";
  return EXIT_SUCCESS;
}
