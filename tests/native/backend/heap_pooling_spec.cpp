// R-BACK-5.9 / R-BACK-5.10 / R-BACK-14.* — MTLHeap small-resource pooling.
//
// This is a pure-CPU classification + lifetime spec for HeapManager. It
// does NOT require a Metal device — allocBuffer/allocTexture and the
// makeBuffer/makeTexture path live behind an unrealized WMT::Device,
// so the spec exercises:
//
//   * HeapManager::classifyTexture / classifyBuffer (R-BACK-14.1 / 14.2)
//     — footprint threshold, usage flag rejection, family selection.
//   * Default-state queries (instanceCount, totalInstanceCount,
//     retireFreedHeaps) so the runtime can call them safely before
//     init() lands or after a teardown.
//   * retainHeapMember / releaseHeapMember on a no-instance manager
//     (silent no-op; matches the Pool path where a stale handle must
//     not crash).
//
// Live-allocation behavior (heap.makeBuffer / heap.makeTexture, the
// fragmentation walk, geometric grow) is covered by the runtime
// integration tests once a Metal device or sim backend is wired up —
// this spec stays deterministic and runs everywhere the rest of the
// `dxmt9-*-spec` family runs.

#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "dxmt9/core.hpp"
#include "../../../src/dxmt9/dxmt9_heap_manager.hpp"

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

// A test-only shim: HeapManager::init takes a WMT::Device. We can
// fabricate one with a non-zero handle so init() flips the
// `initialized_` flag — none of the classify*() entry points touch
// the device past that flag. The manager is non-movable (it owns
// WMT::Reference<Heap> instances internally) so we configure it
// in-place.
void initManager(dxmt9::resources::HeapManager& m, bool hasUnifiedMemory) {
  // Synthetic device handle (1 == non-null in WMT::Object's bool conv).
  // classify* never dereferences the handle; it only consults the
  // initialized_ flag and the cached unified-memory bit.
  WMT::Device dev{1};
  m.init(dev, hasUnifiedMemory);
}

void testClassifyRejectsTooLargeFootprint() {
  using namespace dxmt9::resources;
  using namespace dxmt9::core;
  HeapManager m;
  initManager(m, /*hasUnifiedMemory=*/true);
  // 64 KB exact is eligible; 64 KB + 1 byte is not.
  const auto eligibleAtThreshold =
      m.classifyTexture(kHeapEligibilityFootprintBytes, Pool::Default, 0u);
  check(eligibleAtThreshold.eligible,
        "classifyTexture: footprint at threshold should be eligible");
  check(eligibleAtThreshold.family == HeapFamily::PrivateTexture,
        "classifyTexture: DEFAULT pool maps to PrivateTexture family");

  const auto rejectedOverThreshold =
      m.classifyTexture(kHeapEligibilityFootprintBytes + 1u, Pool::Default, 0u);
  check(!rejectedOverThreshold.eligible,
        "classifyTexture: footprint over threshold must be rejected");

  // R-BACK-14.* — heap-backed buffer suballocation is temporarily disabled
  // because the bridge does not expose the host-mapped pointer of a
  // heap-suballocated MTLBuffer (see dxmt9_heap_manager.cpp::classifyBuffer
  // for the full rationale). Until that is restored, every classifyBuffer
  // call must short-circuit to direct allocation.
  const auto bufBelow =
      m.classifyBuffer(2048u, Pool::Default, 0u);
  check(!bufBelow.eligible,
        "classifyBuffer: small DEFAULT buffer falls back to direct alloc "
        "while heap path is disabled");

  const auto bufRejected =
      m.classifyBuffer(kHeapEligibilityFootprintBytes + 1u, Pool::Default, 0u);
  check(!bufRejected.eligible,
        "classifyBuffer: footprint over threshold must be rejected");
}

void testClassifyRejectsRenderTargetAndDepthStencil() {
  using namespace dxmt9::resources;
  using namespace dxmt9::core;
  HeapManager m;
  initManager(m, /*hasUnifiedMemory=*/true);
  const auto rt = m.classifyTexture(1024u, Pool::Default, UsageRenderTarget);
  check(!rt.eligible,
        "classifyTexture: UsageRenderTarget must always allocate direct");

  const auto ds = m.classifyTexture(1024u, Pool::Default, UsageDepthStencil);
  check(!ds.eligible,
        "classifyTexture: UsageDepthStencil must always allocate direct");

  const auto dynBuf = m.classifyBuffer(1024u, Pool::Default, UsageDynamic);
  check(!dynBuf.eligible,
        "classifyBuffer: UsageDynamic (rename ring) must always allocate direct");
}

void testClassifyManagedDependsOnUnifiedMemory() {
  using namespace dxmt9::resources;
  using namespace dxmt9::core;

  HeapManager unified;
  initManager(unified, /*hasUnifiedMemory=*/true);
  const auto unifiedManaged =
      unified.classifyTexture(1024u, Pool::Managed, 0u);
  check(unifiedManaged.eligible &&
            unifiedManaged.family == HeapFamily::SharedTextureUm,
        "classifyTexture: MANAGED on unified memory targets SharedTextureUm");

  HeapManager discrete;
  initManager(discrete, /*hasUnifiedMemory=*/false);
  const auto discreteManaged =
      discrete.classifyTexture(1024u, Pool::Managed, 0u);
  check(!discreteManaged.eligible,
        "classifyTexture: MANAGED on discrete memory must allocate direct");
}

void testClassifyRejectsSystemMemAndScratchPools() {
  using namespace dxmt9::resources;
  using namespace dxmt9::core;
  HeapManager m;
  initManager(m, /*hasUnifiedMemory=*/true);
  check(!m.classifyTexture(512u, Pool::SystemMem, 0u).eligible,
        "classifyTexture: SystemMem pool never allocates a Metal texture");
  check(!m.classifyTexture(512u, Pool::Scratch, 0u).eligible,
        "classifyTexture: Scratch pool never allocates a Metal texture");
  check(!m.classifyBuffer(512u, Pool::SystemMem, 0u).eligible,
        "classifyBuffer: SystemMem pool stays on the shadow path");
  check(!m.classifyBuffer(512u, Pool::Scratch, 0u).eligible,
        "classifyBuffer: Scratch pool stays on the shadow path");
}

void testUninitializedManagerNeverEligible() {
  using namespace dxmt9::resources;
  using namespace dxmt9::core;
  HeapManager m;  // not init'd
  check(!m.classifyTexture(1024u, Pool::Default, 0u).eligible,
        "uninitialized HeapManager rejects all classify requests");
  check(!m.classifyBuffer(1024u, Pool::Default, 0u).eligible,
        "uninitialized HeapManager rejects all buffer requests too");
  check(m.totalInstanceCount() == 0u,
        "uninitialized HeapManager reports zero instances");
  // retireFreedHeaps is a no-op on an empty manager.
  check(m.retireFreedHeaps(/*completedSeqId=*/123u) == 0u,
        "retireFreedHeaps on empty HeapManager retires nothing");
  // retain/release with a stale (unknown) handle must not crash.
  m.retainHeapMember(/*heapHandle=*/0xdeadbeefu, /*seqId=*/1u);
  m.releaseHeapMember(/*heapHandle=*/0xdeadbeefu, /*seqId=*/2u);
}

void testInitializedManagerStartsEmpty() {
  using namespace dxmt9::resources;
  HeapManager m;
  initManager(m, /*hasUnifiedMemory=*/true);
  check(m.totalInstanceCount() == 0u,
        "freshly initialized HeapManager has no heaps until first alloc");
  check(m.instanceCount(HeapFamily::PrivateTexture) == 0u,
        "PrivateTexture family starts empty");
  check(m.instanceCount(HeapFamily::SharedTextureUm) == 0u,
        "SharedTextureUm family starts empty");
  check(m.instanceCount(HeapFamily::SharedBuffer) == 0u,
        "SharedBuffer family starts empty");
  m.purgeAll();  // teardown safe before any alloc
  check(m.totalInstanceCount() == 0u,
        "purgeAll on empty manager keeps the count at zero");
}

void testEligibilityThresholdConstants() {
  using namespace dxmt9::resources;
  // R-BACK-14.2 — the threshold default is the documented 64 KB.
  check(kHeapEligibilityFootprintBytes == 64u * 1024u,
        "kHeapEligibilityFootprintBytes default must match R-BACK-14.2 (64 KB)");
  // R-BACK-5.10 — geometric growth bounds.
  check(kHeapInitialBytes == 4ull * 1024ull * 1024ull,
        "kHeapInitialBytes default must be 4 MB");
  check(kHeapMaxBytesPerFamily == 256ull * 1024ull * 1024ull,
        "kHeapMaxBytesPerFamily default must be 256 MB");
  check(kHeapMaxBytesPerFamily > kHeapInitialBytes,
        "max-per-family must exceed initial size for geometric growth");
}

}  // namespace

int main() {
  try {
    testEligibilityThresholdConstants();
    testUninitializedManagerNeverEligible();
    testInitializedManagerStartsEmpty();
    testClassifyRejectsTooLargeFootprint();
    testClassifyRejectsRenderTargetAndDepthStencil();
    testClassifyManagedDependsOnUnifiedMemory();
    testClassifyRejectsSystemMemAndScratchPools();
  } catch (const TestFailure& e) {
    std::cerr << "heap_pooling_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "heap_pooling_spec unexpected exception: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "heap_pooling_spec passed\n";
  return EXIT_SUCCESS;
}
