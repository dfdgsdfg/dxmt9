#include "d3d9_pe_diagnostics_state.hpp"

#include <array>
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
    check(after == before + 1u,
          "an enabled diagnostic owner uses one lazy allocation");
    check(diagnostics->config.recorderStats == config.recorderStats &&
              diagnostics->config.recorderChunkLog ==
                  config.recorderChunkLog &&
              diagnostics->config.statsDecimationN ==
                  config.statsDecimationN &&
              diagnostics->config.vsConstSetterRange ==
                  config.vsConstSetterRange &&
              diagnostics->config.moduleMap == config.moduleMap &&
              diagnostics->config.threadSampler == config.threadSampler &&
              diagnostics->config.threadSamplerHz == config.threadSamplerHz,
          "the owner preserves the resolved immutable diagnostic config");
    const bool scopeExpected =
        config.recorderStats || config.statsDecimationN != 0u;
    check(diagnostics->gates.callScope == scopeExpected &&
              diagnostics->gates.hotSetterTimer == scopeExpected,
          "call and setter scopes are enabled only by their owning gates");
    check(diagnostics->gates.moduleMap == config.moduleMap &&
              diagnostics->gates.threadSampler == config.threadSampler &&
              diagnostics->gates.debugLog == config.debugLog,
          "observer-only gates remain independently cached");
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

void testSourceContracts(const std::filesystem::path &root) {
  const auto diagnostics = readTextFile(
      root / "src/d3d9/d3d9_pe_diagnostics_state.hpp");
  const auto observer = readTextFile(
      root / "src/d3d9/d3d9_pe_diagnostic_observer.hpp");
  const auto child =
      readTextFile(root / "src/d3d9/d3d9_pe_device_child.hpp");
  const auto device =
      readTextFile(root / "src/d3d9/d3d9_pe_device_impl.hpp");
  const auto recorder =
      readTextFile(root / "src/d3d9/d3d9_pe_device_recorder.cpp");
  const auto deviceOwner =
      readTextFile(root / "src/d3d9/d3d9_pe_device.cpp");
  const auto misc = readTextFile(
      root / "src/d3d9/d3d9_pe_device_child_misc.cpp");
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
  checkContains(diagnostics, "struct PeDiagnosticsFeatureGates",
                "diagnostic ownership has feature-specific cached gates");
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
  checkBefore(
      device,
      "if (!diagnostics || !diagnostics->gates.hotSetterTimer) {\n            return setRenderStateCore",
      "PeHotStateSetterTimer hotSetter(\n            *this, *diagnostics",
      "SetRenderState branches before the enabled timer lifetime begins");
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
  checkContains(child, "virtual HRESULT FlushPeRecorderForChild() = 0;",
                "recorder protocol flush virtual remains first");
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
    testSourceContracts(argv[1]);
  } catch (const TestFailure &error) {
    std::cerr << "pe_diagnostics_spec failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "pe_diagnostics_spec passed\n";
  return EXIT_SUCCESS;
}
