#include "d3d9_pe_public_allocation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace {

bool gFailAlloc = false;

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw Failure(std::string(message));
}

template <typename T>
struct FailingAllocator {
  using value_type = T;
  using is_always_equal = std::true_type;

  T* allocate(std::size_t count) {
    if (gFailAlloc) throw std::bad_alloc();
    return std::allocator<T>{}.allocate(count);
  }

  void deallocate(T* value, std::size_t count) noexcept {
    std::allocator<T>{}.deallocate(value, count);
  }

  template <typename U>
  bool operator==(const FailingAllocator<U>&) const noexcept {
    return true;
  }
};

using Bytes = std::vector<std::uint8_t, FailingAllocator<std::uint8_t>>;
using Palette = std::array<std::uint8_t, 4>;
using PaletteMap = std::unordered_map<
    unsigned, Palette, std::hash<unsigned>, std::equal_to<unsigned>,
    FailingAllocator<std::pair<const unsigned, Palette>>>;
using Result = dxmt9::d3d9::pe::PublicAllocationResult;

std::filesystem::path sourceFilePath(std::string_view filename) {
  const std::filesystem::path compiledPath(__FILE__);
  if (compiledPath.is_absolute()) return compiledPath;
  for (auto directory = std::filesystem::current_path();;
       directory = directory.parent_path()) {
    const auto compiledCandidate = (directory / compiledPath).lexically_normal();
    if (std::filesystem::exists(compiledCandidate)) return compiledCandidate;
    const auto repositoryCandidate =
        directory / "tests/native/bridge" / filename;
    if (std::filesystem::exists(repositoryCandidate)) return repositoryCandidate;
    if (directory == directory.parent_path()) break;
  }
  return (std::filesystem::current_path() / compiledPath).lexically_normal();
}

void testProductionDrawCandidateBindings() {
  const std::filesystem::path sourcePath = sourceFilePath(
      "pe_public_allocation_spec.cpp");
  const std::filesystem::path implementationPath =
      sourcePath.parent_path() / "../../../src/d3d9/d3d9_pe_device_impl.hpp";
  std::ifstream input(implementationPath);
  check(input.good(), "production draw source is available for the binding audit");
  const std::string source{
      std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  const std::filesystem::path hotImplementationPath =
      sourcePath.parent_path() / "../../../src/d3d9/d3d9_pe_device.cpp";
  std::ifstream hotInput(hotImplementationPath);
  check(hotInput.good(),
        "production hot draw source is available for the binding audit");
  const std::string hotSource{
      std::istreambuf_iterator<char>(hotInput),
      std::istreambuf_iterator<char>()};
  constexpr std::array<std::string_view, 4> drawNames{
      "DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP",
      "DrawIndexedPrimitiveUP"};
  for (const auto drawName : drawNames) {
    const bool inlineOwner = drawName == "DrawIndexedPrimitive";
    const std::string signature =
        std::string("HRESULT STDMETHODCALLTYPE ") +
        (inlineOwner ? "" : "D3D9DeviceImpl::") +
        std::string(drawName) + "(";
    const std::string& publicOwner = inlineOwner ? source : hotSource;
    const std::size_t publicBegin = publicOwner.find(signature);
    check(publicBegin != std::string::npos,
          "production draw entry is present for the binding audit");
    const std::string& bodyOwner =
        drawName == "DrawPrimitive" ? source : publicOwner;
    const std::size_t begin = drawName == "DrawPrimitive"
        ? bodyOwner.find("HRESULT drawPrimitiveCore")
        : publicBegin;
    check(begin != std::string::npos,
          "production draw entry is present for the binding audit");
    if (drawName == "DrawPrimitive") {
      check(publicOwner.find("drawPrimitiveCore", publicBegin) !=
                std::string::npos,
            "DrawPrimitive routes through the shared draw core");
    }
    const std::size_t next = bodyOwner.find(
        "HRESULT STDMETHODCALLTYPE ", begin + signature.size());
    const std::string body = bodyOwner.substr(
        begin, next == std::string::npos ? std::string::npos : next - begin);
    const std::size_t phase = body.find("prepareSoftwareDrawCandidate([&]");
    check(phase != std::string::npos,
          "every public SWVP draw enters the shared candidate phase");
    const std::size_t transform = body.find("trySoftware");
    check(transform != std::string::npos && transform >= phase,
          "SWVP transform/instance preparation is inside the shared phase");
    const std::size_t filter = body.find("filterSoftware");
    check(filter != std::string::npos && filter >= phase,
          "SWVP clip/filter preparation is inside the shared phase");
  }
}

void testSwvpUnlockOwnershipBindings() {
  const std::filesystem::path sourcePath = sourceFilePath(
      "pe_public_allocation_spec.cpp");
  const std::filesystem::path implementationPath =
      sourcePath.parent_path() / "../../../src/d3d9/d3d9_pe_device_swvp.cpp";
  std::ifstream input(implementationPath);
  check(input.good(), "SWVP source is available for the unlock audit");
  const std::string source{
      std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  constexpr std::array<std::pair<std::string_view, std::string_view>, 3> sites{{
      {"hr = dstBuffer->Unlock();", "unlockGuard.dismiss();"},
      {"hr = indexBuf_->Unlock();", "unlockGuard.dismiss();"},
      {"hr = srcBuffer->Unlock();", "srcUnlockGuard.dismiss();"},
  }};
  for (const auto& [unlock, dismiss] : sites) {
    std::size_t siteCount = 0u;
    std::size_t position = 0u;
    while ((position = source.find(unlock, position)) != std::string::npos) {
      ++siteCount;
      const std::size_t next = position + unlock.size();
      const std::size_t first = source.find_first_not_of(" \t\r\n", next);
      check(first != std::string::npos && source.compare(first, dismiss.size(),
                                                          dismiss) == 0,
            "every explicit SWVP Unlock is immediately followed by dismiss");
      position = next;
    }
    check(siteCount == (unlock == "hr = srcBuffer->Unlock();" ? 4u : 1u),
          "SWVP unlock audit covers exactly the six explicit Unlock sites");
  }
  constexpr std::array<std::pair<std::string_view, std::size_t>, 3> guards{{
      {"SwvpUnlockGuard unlockGuard", 1u},
      {"SwvpIndexUnlockGuard unlockGuard", 1u},
      {"SwvpUnlockGuard srcUnlockGuard", 4u}}};
  for (const auto& [guard, expectedCount] : guards) {
    std::size_t guardCount = 0u;
    std::size_t search = 0u;
    while ((search = source.find(guard, search)) != std::string::npos) {
      const std::size_t lockPosition = source.rfind("Lock(", search);
      const std::size_t failureCheck = source.rfind(
          "if (FAILED(hr)) return", search);
      check(lockPosition != std::string::npos && lockPosition < search &&
                failureCheck != std::string::npos &&
                failureCheck > lockPosition && failureCheck < search,
            "SWVP unlock guards are constructed only after Lock succeeds");
      ++guardCount;
      search += guard.size();
    }
    check(guardCount == expectedCount,
          "SWVP unlock audit covers exactly six post-Lock guards");
  }
}

void testSwvpResizeFailureIsContained() {
  Bytes destination;
  const std::array<std::uint8_t, 4> source{{1u, 2u, 3u, 4u}};
  gFailAlloc = true;
  const Result result = dxmt9::d3d9::pe::prepareSwvpIndices(
      destination, source.data(), source.size(), [](auto&) { return true; });
  gFailAlloc = false;
  check(result == Result::OutOfMemory && destination.empty(),
        "SWVP index resize allocation failure is contained before publication");
}

void testSwvpFilterFailurePreservesDestination() {
  Bytes destination;
  destination.push_back(0x7au);
  const std::array<std::uint8_t, 4> source{{1u, 2u, 3u, 4u}};
  gFailAlloc = false;
  const Result result = dxmt9::d3d9::pe::prepareSwvpIndices(
      destination, source.data(), source.size(), [](auto&) {
        Bytes filterScratch;
        gFailAlloc = true;
        filterScratch.resize(64u);
        return true;
      });
  gFailAlloc = false;
  check(result == Result::OutOfMemory && destination.size() == 1u &&
            destination[0] == 0x7au,
        "SWVP filter allocation failure preserves the prior index state");
}

void testPaletteInsertionFailureIsTransactional() {
  PaletteMap palettes;
  const Palette prior{{1u, 2u, 3u, 4u}};
  const Palette replacement{{5u, 6u, 7u, 8u}};
  const Palette newValue{{9u, 10u, 11u, 12u}};
  gFailAlloc = false;
  palettes.emplace(3u, prior);

  gFailAlloc = true;
  const Result insertion = dxmt9::d3d9::pe::replacePalette(
      palettes, 9u, newValue);
  check(insertion == Result::OutOfMemory && palettes.find(9u) == palettes.end() &&
            palettes.find(3u)->second == prior,
        "palette map allocation failure preserves all prior entries");

  const Result update = dxmt9::d3d9::pe::replacePalette(
      palettes, 3u, replacement);
  gFailAlloc = false;
  check(update == Result::Completed && palettes.find(3u)->second == replacement,
        "existing palette replacement remains allocation-free");
}

void testDrawCandidateAllocationFailures() {
  constexpr std::array<std::string_view, 4> paths{
      "bound", "indexed-bound", "up", "indexed-up"};
  constexpr std::array<std::string_view, 3> phases{
      "transform", "instance-expansion", "clip-filter"};
  const std::array<std::uint8_t, 4> callerData{{0x11u, 0x22u, 0x33u, 0x44u}};

  for (const auto path : paths) {
    for (const auto phase : phases) {
      Bytes published;
      published.push_back(0x7au);
      const auto before = callerData;
      gFailAlloc = true;
      const Result result = dxmt9::d3d9::pe::runPublicAllocationPhase([&] {
        if (phase == "instance-expansion") {
          std::array<Bytes, 2> instances{};
          instances[0].resize(64u);
        } else if (phase == "clip-filter") {
          Bytes filtered;
          filtered.reserve(128u);
        } else {
          Bytes transformed;
          transformed.resize(128u);
        }
      });
      gFailAlloc = false;
      check(result == Result::OutOfMemory && published.size() == 1u &&
                published[0] == 0x7au && callerData == before,
            std::string("draw candidate failure is contained for ") +
                std::string(path) + " / " + std::string(phase));
    }
  }
}

}  // namespace

int main() {
  try {
    testProductionDrawCandidateBindings();
    testSwvpUnlockOwnershipBindings();
    testSwvpResizeFailureIsContained();
    testSwvpFilterFailurePreservesDestination();
    testPaletteInsertionFailureIsTransactional();
    testDrawCandidateAllocationFailures();
  } catch (const Failure& failure) {
    std::cerr << "pe_public_allocation_spec failed: " << failure.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "pe_public_allocation_spec passed\n";
  return EXIT_SUCCESS;
}
