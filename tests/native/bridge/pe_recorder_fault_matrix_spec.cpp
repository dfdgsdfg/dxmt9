// Native algebra coverage for the common PE recorder fault envelope.
// These cases do not claim PE COM or x64/x86 Wine behaviour; those claims
// require the canonical cross-built conformance runner and fresh processes.

#include "d3d9_pe_recorder_fault.hpp"
#include "d3d9_pe_retainer.hpp"
#include "d3d9_pe_recorder_transaction.hpp"

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

struct RefCounter {
  std::uint32_t refs = 1u;
};
struct D9CSurface : RefCounter {};
struct D9CTexture : RefCounter {};
struct D9CBuffer : RefCounter {};
struct D9CShader : RefCounter {};
struct D9CVertexDecl : RefCounter {};
struct D9CQuery : RefCounter {};

template <typename T>
void testAddRef(T* value) { ++value->refs; }

template <typename T>
std::uint32_t testRelease(T* value) { return --value->refs; }

extern "C" void dxmt9c_surface_addref(D9CSurface* value) { testAddRef(value); }
extern "C" std::uint32_t dxmt9c_surface_release(D9CSurface* value) {
  return testRelease(value);
}
extern "C" void dxmt9c_texture_addref(D9CTexture* value) { testAddRef(value); }
extern "C" std::uint32_t dxmt9c_texture_release(D9CTexture* value) {
  return testRelease(value);
}
extern "C" void dxmt9c_buffer_addref(D9CBuffer* value) { testAddRef(value); }
extern "C" std::uint32_t dxmt9c_buffer_release(D9CBuffer* value) {
  return testRelease(value);
}
extern "C" void dxmt9c_shader_addref(D9CShader* value) { testAddRef(value); }
extern "C" std::uint32_t dxmt9c_shader_release(D9CShader* value) {
  return testRelease(value);
}
extern "C" void dxmt9c_vdecl_addref(D9CVertexDecl* value) { testAddRef(value); }
extern "C" std::uint32_t dxmt9c_vdecl_release(D9CVertexDecl* value) {
  return testRelease(value);
}
extern "C" void dxmt9c_query_addref(D9CQuery* value) { testAddRef(value); }
extern "C" std::uint32_t dxmt9c_query_release(D9CQuery* value) {
  return testRelease(value);
}

namespace {

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw Failure(std::string(message));
}

void testConsumptionReceiptMatching() {
  check(peRecorderFaultReceiptLineMatches(
            "DXMT9_PE_RECORDER_FAULT_CONSUMED=bridge_pre", "bridge_pre"),
        "exact consumption receipt is accepted");
  check(!peRecorderFaultReceiptLineMatches(
             "DXMT9_PE_RECORDER_FAULT_CONSUMED=bridge_entered", "bridge_pre"),
        "wrong selector consumption receipt is rejected");
  check(!peRecorderFaultReceiptLineMatches(nullptr, "bridge_pre"),
        "missing consumption receipt is rejected");
}

void testTypedSelector() {
  const auto config = peRecorderFaultConfigFromString(
      "capacity_pre_reserve,retain_acquire=0x80070057,"
      "bridge_pre=0x80004003,bridge_entered,capture_disposition,"
      "capture_throw,reset,teardown");
  check(config.mask == ((1u << static_cast<unsigned>(
                             PeRecorderFaultPoint::Count)) - 1u),
        "fault selector covers every typed point exactly once");
  check(config.hresult[static_cast<std::size_t>(
            PeRecorderFaultPoint::CapacityPreReserve)] ==
            static_cast<std::int32_t>(0x8007000eu),
        "capacity reserve defaults to E_OUTOFMEMORY");
  check(config.hresult[static_cast<std::size_t>(
            PeRecorderFaultPoint::RetainAcquire)] ==
            static_cast<std::int32_t>(0x80070057u),
        "retain/acquire preserves an explicit HRESULT");
  const auto preFirst = peRecorderFaultConfigFromString("retain_acquire=0");
  check(preFirst.retainSuccessesBeforeFailure == 0u,
        "retain/acquire zero budget fails before the first unique retain");
  const auto partial = peRecorderFaultConfigFromString("retain_acquire=1");
  check(partial.retainSuccessesBeforeFailure == 1u,
        "retain/acquire one budget permits one unique retain first");
  const auto bounded = peRecorderFaultConfigFromString("retain_acquire=37");
  check(bounded.retainSuccessesBeforeFailure == 37u,
        "retain/acquire accepts a bounded N-success budget");
  check(config.hresult[static_cast<std::size_t>(
            PeRecorderFaultPoint::BridgePre)] ==
            static_cast<std::int32_t>(0x80004003u),
        "bridge pre parses an explicit HRESULT");
  check(peRecorderFaultConfigFromString(nullptr).mask == 0u,
        "fault selector is default-off");
  check(peRecorderFaultConfigFromString("unknown,bridge_pre=bad").mask ==
            peRecorderFaultBit(PeRecorderFaultPoint::BridgePre),
        "unknown and malformed tokens fail closed");
}

void testCapacityAndRetainRemainUnattempted() {
  using namespace dxmt9::d3d9::pe;
  PeRecorderChunkTransaction tx;
  check(tx.beginChunk(), "capacity fixture starts");
  check(tx.recordCapacityPreEvidence(false) && tx.retryable(),
        "capacity-pre failure remains retryable");
  check(!tx.activeRecord() && tx.recordCount() == 0u,
        "capacity-pre failure opens no record");
  tx.discard();

  check(tx.beginChunk() && tx.noteRecord(7u, 16u, {}, 0u, 0u, 0u, 0u),
        "retain fixture opens one record");
  check(tx.recordEmitResult(false) && tx.recordCount() == 0u,
        "retain/acquire failure rolls back the active record");
  check(tx.phase() == RecorderChunkTransactionPhase::Collecting,
        "retain/acquire rollback leaves the chunk collecting");
}

void testBridgeCaptureResetEnvelope() {
  using namespace dxmt9::d3d9::pe;
  PeRecorderChunkTransaction tx;
  check(tx.beginChunk() && tx.noteRecord(7u, 16u, {}, 0u, 0u, 0u, 0u) &&
            tx.recordEmitResult(true) && tx.recordSealResult(true) &&
            tx.recordSealedEvidence(1u, 0u, 0u, 0u),
        "bridge fixture reaches sealed");
  check(tx.recordBridgePreEffectFailure() && tx.retryable(),
        "bridge-pre failure is retryable before entry");
  check(tx.reopenBridgePreEffectRetry() &&
            tx.recordSealResult(true) && tx.recordSealedEvidence(1u, 0u, 0u, 0u),
        "bridge-pre retry reuses sealed bytes");
  check(tx.recordBridgeResult(false) && tx.poisoned(),
        "entered bridge failure is effect-unknown and fail-stop");
  tx.discard();
  check(tx.phase() == RecorderChunkTransactionPhase::Idle,
        "Reset/teardown discard clears the terminal owner");
}

void testRetainBoundaryRollback(const char* selector,
                                bool firstSucceeds) {
  const auto pid = fork();
  check(pid >= 0, "retain fault fixture forks a fresh process");
  if (pid == 0) {
    setenv("DXMT9_PE_RECORDER_FAULT", selector, 1);
    D3D9PePendingCommandRetainer retainer;
    D9CBuffer first{};
    D9CBuffer second{};
    auto acquired = retainer.beginAcquire();
    const bool firstResult = retainer.retainBuffer(&first, acquired);
    const bool secondResult = retainer.retainBuffer(&second, acquired);
    const bool shape = firstResult == firstSucceeds &&
        secondResult == !firstSucceeds &&
        first.refs == (firstSucceeds ? 2u : 1u) &&
        second.refs == (firstSucceeds ? 1u : 2u) &&
        retainer.size() == (firstSucceeds ? 1u : 1u);
    retainer.rollback(acquired);
    const bool rolledBack = first.refs == 1u && second.refs == 1u &&
        retainer.size() == 0u;
    _exit(shape && rolledBack ? 0 : 1);
  }
  int status = 0;
  check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0,
        "typed retain fault rolls back pre-first and partial acquisitions");
}

void testCaptureDispositions() {
  using namespace dxmt9::d3d9::pe;
  constexpr std::array dispositions = {
      RecorderChunkCaptureDisposition::Materialized,
      RecorderChunkCaptureDisposition::Rejected,
      RecorderChunkCaptureDisposition::Skipped,
  };
  for (const auto disposition : dispositions) {
    PeRecorderChunkTransaction tx;
    check(tx.beginChunk() && tx.noteRecord(7u, 16u, {}, 0u, 0u, 0u, 0u) &&
              tx.recordEmitResult(true) && tx.recordSealResult(true) &&
              tx.recordSealedEvidence(1u, 0u, 0u, 0u) &&
              tx.recordCaptureReservation(1u, 2u,
                  disposition == RecorderChunkCaptureDisposition::Materialized) &&
              tx.recordBridgeResult(true),
          "capture fixture reaches bridge accepted");
    check(disposition != RecorderChunkCaptureDisposition::Materialized ||
              tx.captureReserved(),
          "materialized capture requires reservation");
    if (disposition == RecorderChunkCaptureDisposition::Skipped) {
      // This deliberately exercises the typed disposition algebra only; the
      // production capture owner owns the richer reserve predicate.
      check(!tx.captureReserved(), "skipped capture has no reservation");
    }
    check(tx.recordCaptureResult(disposition) &&
              tx.captureDisposition() == disposition,
          "capture disposition settles after bridge acceptance");
  }
}

}  // namespace

int main() {
  try {
    testConsumptionReceiptMatching();
    testTypedSelector();
    testCapacityAndRetainRemainUnattempted();
    testRetainBoundaryRollback("retain_acquire=0", false);
    testRetainBoundaryRollback("retain_acquire=1", true);
    testBridgeCaptureResetEnvelope();
    testCaptureDispositions();
  } catch (const Failure &failure) {
    return (void(std::fprintf(stderr, "FAIL: %s\n", failure.what())), 1);
  }
  return 0;
}
