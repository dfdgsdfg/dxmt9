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
      PeDiagnosticsConfig{.copyMaterializationLedger = true},
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
              diagnostics->config.copyMaterializationLedger ==
                  config.copyMaterializationLedger &&
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
                  config.scalarSemanticObserver &&
              diagnostics->gates.copyMaterializationLedger ==
                  config.copyMaterializationLedger,
          "observer-only gates remain independently cached");
    check(static_cast<bool>(diagnostics->scalarSemanticTokens) ==
              config.scalarSemanticObserver,
          "the exact semantic ledger exists only behind its own gate");
  }
}

void testCopyMaterializationReportSnapshotValue() {
  const dxmt9::core::CopyMaterializationSnapshot empty{};
  check(!dxmt9::core::copyMaterializationSnapshotHasActivity(empty),
        "empty copy-materialization snapshot is omitted");
  for (const auto field : {0u, 1u}) {
    auto snapshot = empty;
    if (field == 0u) {
      snapshot.semanticCalls = 1u;
    } else {
      snapshot.retainedBytes = 1u;
    }
    check(dxmt9::core::copyMaterializationSnapshotHasActivity(snapshot),
          "copy-materialization activity includes semantic and retained rows");
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
      "capture_pre=0x80070057,bridge_pre=0x80004003,"
      "apply_entered=0x8876086c,bridge_entered");
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
  check(config.hresult[static_cast<std::size_t>(
            PeStateBlockFaultPoint::BridgePre)] ==
            static_cast<std::int32_t>(0x80004003u),
        "fault selector preserves generic pre-entry bridge HRESULT");
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
  const auto contexts = readTextFile(
      root / "src/d3d9/d3d9_pe_child_context.hpp");
  const auto scopes = readTextFile(
      root / "src/d3d9/d3d9_pe_child_scopes.hpp");
  const auto stateBlockShadow = readTextFile(
      root / "src/d3d9/d3d9_pe_stateblock_shadow.hpp");
  const auto device = readTextFile(
      root / "src/d3d9/d3d9_pe_device_impl.hpp");
  const auto deviceOwner = readTextFile(
      root / "src/d3d9/d3d9_pe_device.cpp");
  const auto presentBatch = readTextFile(
      root / "src/d3d9/d3d9_pe_batch.hpp");
  for (const std::string_view fragment : {
           "d3d9_pe_device_state_core.inc.hpp",
           "d3d9_pe_device_state_texture_fvf.inc.hpp",
           "d3d9_pe_device_state_shader_stream.inc.hpp"}) {
    check(device.find(fragment) == std::string::npos &&
              deviceOwner.find(fragment) == std::string::npos &&
              !std::filesystem::exists(root / "src/d3d9" / fragment),
          "retired state fragment is absent");
  }
  check(std::count(device.begin(), device.end(), '\n') <= 3677,
        "device declaration shell stays within the measured 3677-line residual");
  checkContains(presentBatch, "struct PePresentBatch",
                "Present has a typed immutable value owner");
  checkContains(device, "const dxmt9::d3d9::pe::PePresentBatch presentBatch",
                "the real Present boundary owns its batch value");
  checkContains(device,
                "builder, presentBatch.command, presentBatch.source",
                "legacy Present append consumes the owned batch value");
  checkNotContains(device, "PePresentBatchTransaction",
                   "exact Present transaction remains unselected in production");
  for (const std::string_view retainedInline : {
           "HRESULT STDMETHODCALLTYPE Present(",
           "HRESULT STDMETHODCALLTYPE SetStreamSource(",
           "HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive("}) {
    check(device.find(retainedInline) != std::string::npos,
          "zero-delta inline entry remains legally reachable in the class header");
  }
  for (const std::string_view owner : {
           "D3D9DeviceImpl::PresentEx(",
           "D3D9DeviceImpl::SetRenderState(",
           "D3D9DeviceImpl::DrawIndexedPrimitiveUP("}) {
    check(deviceOwner.find(owner) != std::string::npos,
          "hot state/draw method has a real translation-unit owner");
  }
  check(deviceOwner.find("d3d9_pe_device_hot.cpp") == std::string::npos,
        "hot implementations are not hidden behind an included source fragment");
  const auto deviceSource = device + deviceOwner;
  const auto recorder =
      readTextFile(root / "src/d3d9/d3d9_pe_device_recorder.cpp");
  const auto recorderHeader =
      readTextFile(root / "src/d3d9/d3d9_pe_recorder.hpp");
  const auto deviceCold =
      readTextFile(root / "src/d3d9/d3d9_pe_device_com_cold.cpp");
  const auto deviceDiag =
      readTextFile(root / "src/d3d9/d3d9_pe_device_diag.cpp");
  const auto peBuilder =
      readTextFile(root / "src/d3d9/d3d9_pe_chunk_builder.cpp");
  const auto peConstShadow =
      readTextFile(root / "src/d3d9/d3d9_pe_const_shadow.hpp");
  const auto providerReplay =
      readTextFile(root / "src/d3d9/device_c_chunk_replay.cpp");
  const auto coreResources =
      readTextFile(root / "src/d3d9/core_resources.cpp");
  const auto providerOffload =
      readTextFile(root / "src/d3d9/device_c_replay_offload.cpp");
  const auto providerPayload =
      readTextFile(root / "src/dxmt9/dxmt9_source_payload.cpp");
  const auto transientArena =
      readTextFile(root / "src/dxmt9/dxmt9_transient_resource_arena.cpp");
  const auto resourcePool =
      readTextFile(root / "src/dxmt9/dxmt9_resource_pool.cpp");
  const auto argbufHybrid =
      readTextFile(root / "src/dxmt9/dxmt9_argbuf_hybrid.cpp");
  const auto queue =
      readTextFile(root / "src/dxmt9/dxmt9_command_queue.cpp");
  const auto sourcePayload =
      readTextFile(root / "src/dxmt9/dxmt9_source_payload.cpp");
  const auto copyLedger =
      readTextFile(root / "include/dxmt9/copy_materialization_ledger.hpp");
  const auto cpuPipelineOwnership = readTextFile(
      root / "specs/verification/tla/CpuPipelineOwnership.tla");
  for (const std::string_view coldOwner : {
           "D3D9DeviceImpl::PrepareStateBlockApplyForChild",
           "D3D9DeviceImpl::CommitStateBlockApplyForChild",
           "D3D9DeviceImpl::resolveImplicitDeclForFvf",
           "D3D9DeviceImpl::validateConstRange",
           "D3D9DeviceImpl::readConstShadow",
           "D3D9DeviceImpl::SetVertexShaderConstantFSlow",
           "D3D9DeviceImpl::SetPixelShaderConstantFSlow"}) {
    check(deviceCold.find(coldOwner) != std::string::npos,
          "cold state body has a real COM-cold owner");
  }
  const auto recorderState =
      readTextFile(root / "src/d3d9/d3d9_pe_recorder_state.hpp");
  const auto stateBlockTransaction = readTextFile(
      root / "src/d3d9/d3d9_pe_stateblock_transaction.hpp");
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
  checkContains(deviceSource, "std::unique_ptr<PeDiagnosticsState> diagnostics_{};",
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
  checkContains(deviceSource,
                "dxmt9::d3d9::pe::PeRecorderState recorderState_{};",
                "device stores the hot recorder owner directly");
  for (const std::string_view alias : {
           "bool& recorderLockRequired_",
           "std::recursive_mutex& recorderMutex_",
           "PeHotStateShadow& peState_", "PeConstShadowBlock& peConsts_",
           "PeBindingView& bindingScratch_", "PeSparseScratch& sparseScratch_",
           "CommandChunkBuilder& commandChunk_",
           "bool& commandChunkNegotiated_"}) {
    checkNotContains(deviceSource, alias,
                     "device must not retain audited recorder-field aliases");
  }
  checkContains(stateBlockShadow, "class D3D9StateBlockShadow",
                "StateBlock wrapper snapshot is a closed class");
  checkContains(stateBlockShadow, "class Writer",
                "wrapper snapshot exposes a writer capability");
  checkContains(stateBlockShadow, "class Snapshot",
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
  checkNotContains(stateBlockShadow, "operator()(void*",
                   "StateBlock ownership policies reject untyped pointers");
  checkNotContains(stateBlockShadow, "reinterpret_cast<IUnknown",
                   "StateBlock ownership preserves the exact interface address");
  checkContains(stateBlockShadow, "if (auto* object = value.raw()) object->AddRef();",
                "typed retain policy calls the original interface pointer");
  checkContains(stateBlockShadow, "if (auto* object = value.raw()) object->Release();",
                "typed release policy calls the original interface pointer");
  check(!std::filesystem::exists(
            root / "src/d3d9/d3d9_pe_trusted_handles.hpp"),
        "convention-only trusted wire extractors are removed");
  checkNotContains(deviceSource, "std::array<void*, kPeTextureSlots> stagedApply",
                   "device cannot bypass typed StateBlock staging");
  checkContains(diagnostics, "struct PeDiagnosticsFeatureGates",
                "diagnostic ownership has feature-specific cached gates");
  checkContains(diagnostics,
                "std::unique_ptr<dxmt9::d3d9::pe::PeScalarSemanticTokenLedger>",
                "the exact scalar witness is a nullable cold owner");
  checkNotContains(recorderState, "PeScalarSemanticTokenLedger",
                   "the always-on recorder does not own the scalar ledger");
  checkContains(deviceSource, "DXMT9_PE_SCALAR_SEMANTIC_OBSERVER",
                "the cold scalar witness has an explicit default-off gate");
  checkContains(deviceSource, "if (!diagnostics->gates.callScope)",
                "unrelated diagnostic gates skip call scope construction");
  checkContains(deviceSource, "if (!diagnostics->gates.hotSetterTimer)",
                "unrelated diagnostic gates skip setter timer construction");
  checkContains(deviceSource, "#define dxmt9DeviceDebugLog(...)",
                "disabled device debug logging is guarded at each call site");
  checkContains(recorder, "peDiagnosticsRead(\n                    chunkDiagnostics",
                "chunk commit clocks are behind the nullable owner gate");
  checkContains(deviceSource, "const auto t0 = phase.begin();",
                "append phase clocks use the nullable clock gate");
  checkNotContains(
      deviceSource,
      "dxmt9PeArmDecimatedScope(peEntryScope, diagnostics_ ? "
      "&diagnostics_->peEntryStateDecimatedStats_",
      "hot state setters use one combined nullable diagnostic scope");
  checkNotContains(
      deviceSource,
      "dxmt9PeArmDecimatedScope(peEntryScope, diagnostics_ ? "
      "&diagnostics_->peEntryDrawDecimatedStats_",
      "draw entry and call tracking share one nullable diagnostic scope");
  const auto setRenderBegin = deviceSource.find(
      "HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetRenderState(D3DRENDERSTATETYPE state");
  const auto setRenderEnd = setRenderBegin == std::string::npos
      ? std::string::npos
      : deviceSource.find("bool D3D9DeviceImpl::isValidTextureStageStateType", setRenderBegin);
  check(setRenderBegin != std::string::npos && setRenderEnd != std::string::npos,
        "SetRenderState source contract is present");
  const std::string_view setRenderBody(
      deviceSource.data() + setRenderBegin, setRenderEnd - setRenderBegin);
  checkBefore(
      setRenderBody,
      "if (!recorderState_.stateBlockTransaction.writeAllowed()) {\n        return D3DERR_DEVICELOST;\n    }\n    PeDiagnosticsState* const diagnostics",
      "return setRenderStateCore(state, value, peNullHotSetter_);",
      "SetRenderState fail-stops before its diagnostic/core bypass");
  checkNotContains(deviceSource,
                   "PeStateBlockTransactionState& stateBlockTransaction_",
                   "device has no dependent StateBlock transaction pointer");
  const auto appendRecordBegin = deviceSource.find(
      "HRESULT appendRecord(uint32_t type, size_t sizeHint, EmitFn emit)");
  const auto appendRecordEnd = appendRecordBegin == std::string::npos
      ? std::string::npos
      : deviceSource.find("HRESULT appendDrawPrimitiveRecord", appendRecordBegin);
  check(appendRecordBegin != std::string::npos &&
            appendRecordEnd != std::string::npos,
        "common appendRecord source contract is present");
  const std::string_view appendRecordBody(
      deviceSource.data() + appendRecordBegin, appendRecordEnd - appendRecordBegin);
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
      deviceSource,
      "HRESULT AppendQueryIssueForChild(\n        std::uint32_t flags",
      "Query::Issue remains routed through the common poisoned append gate");
  checkBefore(
      setRenderBody,
      "if (!diagnostics || !diagnostics->gates.hotSetterTimer) {\n        return setRenderStateCore",
      "PeHotStateSetterTimer hotSetter(\n        *this, *diagnostics",
      "SetRenderState branches before the enabled timer lifetime begins");
  const auto setFvfBegin = deviceSource.find(
      "HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetFVF(DWORD fvf) noexcept");
  const auto setFvfEnd = setFvfBegin == std::string::npos
      ? std::string::npos
      : deviceSource.find("HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetVertexDeclaration", setFvfBegin);
  check(setFvfBegin != std::string::npos && setFvfEnd != std::string::npos,
        "SetFVF noexcept source contract is present");
  const std::string_view setFvfBody(
      deviceSource.data() + setFvfBegin, setFvfEnd - setFvfBegin);
  checkBefore(
      setFvfBody,
      "resolveImplicitDeclForFvf(fvf, &implicitDecl);",
      "fvf_ = fvf;",
      "SetFVF resolves its implicit declaration before shadow mutation");
  checkBefore(
      deviceSource,
      "if (!diagnostics || !diagnostics->gates.callScope) {\n        return drawPrimitiveCore",
      "PeCallScope peCall(\n        *diagnostics, \"DrawPrimitive\"",
      "DrawPrimitive branches before the enabled call scope lifetime begins");
  checkBefore(
      surface,
      "if (!diagnostics_)\n      return getDescCore",
      "D3D9PeChildCallScope peCall(*diagnostics_, \"Surface::GetDesc\"",
      "Surface::GetDesc branches before the enabled child scope lifetime");
  checkContains(deviceSource, "__attribute__((always_inline)) noexcept",
                "generic hot entry bodies are forced through the branch");
  checkContains(deviceDiag,
                "CopyMaterializationOwner::Pe",
                "PE copy report reads the PE-owned binary-local ledger");
  checkNotContains(deviceDiag,
                   "CopyMaterializationOwner::Unix",
                   "PE copy report never reads the Unix-owned ledger");
  checkContains(deviceDiag, "binary=pe owner=pe",
                "PE copy report rows identify their binary and owner");
  checkContains(deviceDiag,
                "identity=%s classification=%s reason=%s",
                "PE copy rows emit identity, classification, and reason");
  checkContains(queue, "identity=%s classification=%s reason=%s",
                "Unix copy rows emit identity, classification, and reason");
  checkContains(queue, "copyMaterializationReportPresents_ != 0u",
                "Unix destruction emits only a non-empty partial report");
  checkContains(queue, "noteCopyMaterializationPublishedPresent();",
                "Unix reports count only after successful publication");
  checkBefore(queue, "queueLifecycle_.presentAndCommit(",
              "noteCopyMaterializationPublishedPresent();",
              "ordinary Unix Present report count follows publication");
  checkContains(queue,
                "if (hasPublishedPresent) {\n"
                "    noteCopyMaterializationPublishedPresent();",
                "active arena Present reports after queue publication");
  check(countOccurrences(
            queue,
            "if (hasPublishedPresent) {\n"
            "    noteCopyMaterializationPublishedPresent();") == 2u,
        "single- and multi-source arena publication each report Present");
  checkNotContains(
      queue,
      "if (appendResult == ActiveArenaAppendResult::Appended) {\n"
      "      noteCopyMaterializationPublishedPresent();",
      "active arena append does not count an uncommitted Present");
  checkContains(copyLedger, "classificationName",
                "copy descriptor retains a stable classification name");
  checkContains(copyLedger, "ownershipAbiReason",
                "copy descriptor retains a named ownership/ABI reason");
  checkContains(copyLedger, "productionCopyMaterializationLedgers",
                "disabled production lookup uses cached owner pointers");
  checkContains(copyLedger, "gCopyMaterializationEffectiveOwnerLedgers",
                "effective owner slots cache production pointers once");
  checkNotContains(copyLedger, "if (auto* ledger =",
                   "active production lookup has no test-then-production probe");
  checkNotContains(copyLedger, "gCopyMaterializationTestOwnerLedgers.fill",
                   "test sinks remain owner-qualified rather than all-owner");
  checkContains(sourcePayload, "tryAppendDrawRunBatch",
                "batch assembler remains covered by the source audit");
  checkNotContains(sourcePayload,
                   "CopyMaterializationClass::ReplaySubmissionCarrierCopy",
                   "batch assembler does not claim a false whole-carrier copy");
  checkContains(sourcePayload,
                "CopyMaterializationClass::QueueFinalSlotAppend",
                "batch assembler retains final-slot accounting");
  checkContains(cpuPipelineOwnership,
                "does not claim concrete bindings for DCE/lookahead",
                "CPU ownership model narrows borrowCount to implemented visits");
  checkNotContains(cpuPipelineOwnership,
                   "borrowCount=1 obligation also\n * covers the replay, serial, DCE/lookahead",
                   "CPU ownership model does not overclaim unimplemented bindings");
  checkContains(deviceCold, "logPeCopyMaterializationLedger();",
                "device destruction emits the final PE copy report");
  checkContains(deviceCold,
                "peCopyMaterializationReportPresents_ % 60u != 0u",
                "PE destruction does not duplicate full-interval reports");
  checkContains(coreResources, "dxmt9/progress_predicates.hpp",
                "PE Query uses the shared pure progress predicates");
  checkNotContains(coreResources, "dxmt9_pipeline_lifecycle.hpp",
                   "PE Query never includes backend-private lifecycle state");
  checkContains(coreResources, "queryGetDataPollSatisfied",
                "Query::getData is bound to the shared poll predicate");

  // These source files are compiled into the Unix provider/runtime targets,
  // despite their historical src/d3d9 or shared-runtime paths. An explicit
  // owner at every call site prevents a native mixed-image test from hiding a
  // production PE/Unix attribution inversion behind a default argument.
  for (const auto *provider : {&providerReplay, &providerOffload,
                               &providerPayload, &transientArena,
                               &resourcePool, &argbufHybrid, &queue}) {
    checkContains(*provider, "CopyMaterializationOwner::Unix",
                  "Unix provider copy sites name the Unix owner explicitly");
    checkNotContains(*provider, "CopyMaterializationOwner::Pe",
                     "Unix provider copy sites never select the PE owner");
    checkNotContains(*provider, "activeCopyMaterializationLedger()",
                     "copy sites never rely on an implicit owner default");
  }
  checkContains(providerReplay, "CopyMaterializationClass::BridgeRawOwnership",
                "Unix replay records bridge raw ownership");
  checkContains(providerOffload, "CopyMaterializationClass::BridgeRawOwnership",
                "Unix offload import records bridge raw ownership");
  checkContains(providerOffload, "CopyMaterializationClass::MutationStaging",
                "Unix offload stages mutation bytes in the Unix ledger");
  checkContains(transientArena, "CopyMaterializationClass::GpuUploadCopy",
                "Unix transient arena records physical GPU upload copies");
  checkContains(argbufHybrid,
                "CopyMaterializationClass::GpuSharedMaterialization",
                "Unix argbuf direct construction records shared GPU materialization");
  checkContains(queue, "binary=unix owner=unix",
                "Unix queue report identifies the Unix binary/owner row");
  checkContains(peBuilder, "CopyMaterializationOwner::Pe",
                "PE builder copy sites explicitly select the PE owner");
  checkNotContains(peBuilder, "CopyMaterializationOwner::Unix",
                   "PE builder copy sites never select the Unix owner");
  checkContains(peBuilder, "CopyMaterializationClass::PeWireFinal",
                "PE builder final-wire materialization remains PE-owned");
  checkContains(peConstShadow, "CopyMaterializationOwner::Pe",
                "PE constant shadow copy sites explicitly select the PE owner");
  checkNotContains(peConstShadow, "CopyMaterializationOwner::Unix",
                   "PE constant shadow copy sites never select the Unix owner");
  checkContains(peConstShadow, "CopyMaterializationClass::PeStateShadow",
                "PE constant shadow materialization remains PE-owned");
  checkNotContains(deviceSource, "CopyMaterializationOwner::Unix",
                   "PE device source never selects the Unix owner");

  for (const std::string_view removed : {
           "NotifyPeFirstCallAfterPresentForChild",
           "PushPeCallScopeForChild", "NotifyPeCallScopeReturnForChild",
           "PopPeCallScopeForChild",
       }) {
    checkNotContains(child, removed,
                     "diagnostic virtual is absent from recorder protocol");
  }
  checkNotContains(contexts, "D3D9PeChildContextBase",
                   "broad child context base is absent");
  for (const std::string_view family : {
           "struct D3D9PeStateBlockContext",
           "struct D3D9PeBufferContext",
           "struct D3D9PeSurfaceTextureContext",
           "struct D3D9PeQueryContext",
           "struct D3D9PePresentationContext",
           "struct D3D9PeShaderDeclarationContext",
       }) {
    checkContains(contexts, family, "all typed child contexts are declared");
  }
  checkNotContains(contexts, "virtual ",
                   "typed child contexts are non-polymorphic");
  checkNotContains(contexts, "NotifyRenderTapeObjectDefineForChild",
                   "object definition remains device-private");
  checkNotContains(contexts, "InvalidateStateBlockShadowForChild",
                   "state shadow invalidation remains device-private");
  checkContains(contexts, "D3D9DeviceImpl *device = nullptr;",
                "contexts carry one nullable device pointer");
  checkContains(child, "#include \"d3d9_pe_child_context.hpp\"",
                "child header includes typed context declarations");
  checkContains(
      scopes,
      "requires std::is_nothrow_invocable_v<Body&&, D3D9PeChildCallScope&>",
      "child call-scope bodies are constrained at the noexcept boundary");
  checkNotContains(observer, "virtual ",
                   "the child diagnostic observer is concrete and nonvirtual");
  checkContains(deviceOwner,
                "HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::QueryInterface(",
                "QueryInterface key function remains in the hot owning TU");
  checkContains(deviceOwner,
                "HRESULT D3D9DeviceImpl::FlushPeRecorderForChild() noexcept",
                "private child flush helper remains out of line");

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
  checkBefore(captureBody, "PeStateBlockFaultPoint::BridgePre",
              "dxmt9c_stateblock_capture(sb_)",
              "generic bridge pre-effect fault precedes Capture entry");
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
  checkBefore(applyBody, "PeStateBlockFaultPoint::BridgePre",
              "context_->PrepareStateBlockApplyForChild(saved_)",
              "generic bridge pre-effect fault precedes Apply ref staging");
  checkBefore(applyBody, "PeStateBlockFaultPoint::BridgePre",
              "dxmt9c_stateblock_apply(sb_)",
              "generic bridge pre-effect fault precedes Apply entry");
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
  checkBefore(endBody, "PeStateBlockFaultPoint::BridgePre",
              "dxmt9c_device_end_state_block(dev_, &sb)",
              "generic bridge pre-effect fault precedes End entry");
  check(countOccurrences(deviceCold,
                         "dxmt9c_device_end_state_block(dev_, &sb)") == 1u,
        "End has one backend call site");
  checkContains(misc, "ULONG refs_ = 1;",
                "ordinary child COM wrappers retain non-atomic ownership");
  checkContains(deviceSource, "ULONG        refs_    = 1;",
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
    testCopyMaterializationReportSnapshotValue();
    testStateBlockFaultSelector();
    testSourceContracts(argv[1]);
  } catch (const TestFailure &error) {
    std::cerr << "pe_diagnostics_spec failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "pe_diagnostics_spec passed\n";
  return EXIT_SUCCESS;
}
