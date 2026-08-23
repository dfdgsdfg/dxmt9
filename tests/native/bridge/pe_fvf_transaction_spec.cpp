#include "d3d9_pe_fvf_transaction.hpp"

#include <cstdint>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw Failure(std::string(message));
}

struct Backend {
  std::uint32_t releases = 0u;
};

struct Wrapper {
  Backend* backend = nullptr;
  std::uint32_t releases = 0u;
};

struct Harness {
  Backend backend{};
  Wrapper wrapper{&backend};
  Wrapper* published = nullptr;

  auto run(bool backendNull, bool wrapperNull, bool publishAccepted,
           bool throwInBackend = false, bool throwInWrapper = false,
           bool throwInPublish = false) {
    return dxmt9::d3d9::pe::createImplicitFvfDeclTransaction<
        Backend*, Wrapper*>(
        [&]() -> Backend* {
          if (throwInBackend) throw std::bad_alloc{};
          return backendNull ? nullptr : &backend;
        },
        [](Backend* value) noexcept { ++value->releases; },
        [&](Backend*) -> Wrapper* {
          if (throwInWrapper) throw std::bad_alloc{};
          return wrapperNull ? nullptr : &wrapper;
        },
        [](Wrapper* value) noexcept {
          ++value->releases;
          ++value->backend->releases;
        },
        [&](Wrapper* value) {
          if (throwInPublish) throw std::bad_alloc{};
          if (!publishAccepted) return false;
          published = value;
          return true;
        });
  }
};

void testBackendAndAllocationFailuresSettleOwnership() {
  Harness backendNull;
  const auto backendFailure = backendNull.run(true, false, true);
  check(backendFailure.failure ==
            dxmt9::d3d9::pe::ImplicitFvfDeclFailure::Backend &&
            backendNull.backend.releases == 0u &&
            backendNull.wrapper.releases == 0u &&
            backendNull.published == nullptr,
        "backend null is reported without inventing ownership");

  Harness backendThrow;
  const auto allocationFailure = backendThrow.run(
      false, false, true, true);
  check(allocationFailure.failure ==
            dxmt9::d3d9::pe::ImplicitFvfDeclFailure::Allocation &&
            backendThrow.backend.releases == 0u &&
            backendThrow.published == nullptr,
        "backend allocation exception is contained by the noexcept boundary");

  Harness wrapperNull;
  const auto wrapperFailure = wrapperNull.run(false, true, true);
  check(wrapperFailure.failure ==
            dxmt9::d3d9::pe::ImplicitFvfDeclFailure::Allocation &&
            wrapperNull.backend.releases == 1u &&
            wrapperNull.wrapper.releases == 0u &&
            wrapperNull.published == nullptr,
        "wrapper null releases the untransferred backend handle once");

  Harness wrapperThrow;
  const auto wrapperException = wrapperThrow.run(
      false, false, true, false, true);
  check(wrapperException.failure ==
            dxmt9::d3d9::pe::ImplicitFvfDeclFailure::Allocation &&
            wrapperThrow.backend.releases == 1u &&
            wrapperThrow.wrapper.releases == 0u,
        "wrapper allocation exception releases the backend handle once");
}

void testPublicationFailureReleasesWrapperOwnership() {
  Harness rejected;
  const auto rejectedResult = rejected.run(false, false, false);
  check(rejectedResult.failure ==
            dxmt9::d3d9::pe::ImplicitFvfDeclFailure::Allocation &&
            rejected.wrapper.releases == 1u &&
            rejected.backend.releases == 1u &&
            rejected.published == nullptr,
        "cache rejection releases the wrapper and its backend exactly once");

  Harness throwing;
  const auto throwingResult = throwing.run(
      false, false, true, false, false, true);
  check(throwingResult.failure ==
            dxmt9::d3d9::pe::ImplicitFvfDeclFailure::Allocation &&
            throwing.wrapper.releases == 1u &&
            throwing.backend.releases == 1u &&
            throwing.published == nullptr,
        "cache allocation exception is noexcept-safe and settles ownership");
}

void testSuccessTransfersBeforeShadowCommit() {
  Harness harness;
  const auto result = harness.run(false, false, true);
  check(result && result.backend == &harness.backend &&
            result.wrapper == &harness.wrapper &&
            harness.published == &harness.wrapper &&
            harness.backend.releases == 0u &&
            harness.wrapper.releases == 0u,
        "successful resolution transfers backend and wrapper ownership to cache");

  std::uint32_t fvfShadow = 0x11u;
  Wrapper* declShadow = reinterpret_cast<Wrapper*>(0x22u);
  bool pendingFvf = false;
  bool pendingDecl = false;
  if (result) {
    fvfShadow = 0x33u;
    declShadow = result.wrapper;
    pendingFvf = true;
    pendingDecl = true;
  }
  check(fvfShadow == 0x33u && declShadow == &harness.wrapper &&
            pendingFvf && pendingDecl,
        "shadow publication follows successful implicit declaration ownership");

  Harness failed;
  const auto failedResult = failed.run(true, false, true);
  fvfShadow = 0x44u;
  declShadow = reinterpret_cast<Wrapper*>(0x55u);
  pendingFvf = false;
  pendingDecl = false;
  if (failedResult) {
    fvfShadow = 0x66u;
    declShadow = failedResult.wrapper;
    pendingFvf = true;
    pendingDecl = true;
  }
  check(fvfShadow == 0x44u &&
            declShadow == reinterpret_cast<Wrapper*>(0x55u) &&
            !pendingFvf && !pendingDecl,
        "failed implicit declaration resolution leaves every shadow untouched");

  // Simulate cache teardown for the successful transaction.
  ++harness.wrapper.releases;
  ++harness.backend.releases;
}

}  // namespace

int main() {
  try {
    testBackendAndAllocationFailuresSettleOwnership();
    testPublicationFailureReleasesWrapperOwnership();
    testSuccessTransfersBeforeShadowCommit();
    std::cout << "pe fvf transaction spec: PASS\n";
    return 0;
  } catch (const Failure& failure) {
    std::cerr << "pe fvf transaction spec: FAIL: " << failure.what() << '\n';
    return 1;
  }
}
