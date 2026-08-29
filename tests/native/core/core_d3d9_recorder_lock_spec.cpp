// Pure value-level spec for the PE-side recorder-lock resolution predicate
// that lives in src/d3d9/d3d9_pe_device.cpp:
//   * dxmt9PeRecorderLockRequired(behaviorFlags, forceLockEnv)
//
// Why a value-level spec?  d3d9_pe_device.cpp is built only on the PE
// (Windows) side (it includes <windows.h> / <d3d9.h>) so native
// dxmt9-core-* tests cannot instantiate D3D9DeviceImpl directly. The
// established pattern (core_d3d9_device_validation_spec.cpp,
// core_d3d9_multiply_transform_spec.cpp) is to mirror the small,
// self-contained PE-side pure logic here and pin its observable contract.
//
// Native D3D9 semantics: IDirect3DDevice9's internal wined3d lock is taken
// only when the app passed D3DCREATE_MULTITHREADED in CreateDevice's
// BehaviorFlags; without it the app promises single-threaded access and
// dxmt9 must not pay a recursive-mutex lock/unlock on every PE recorder
// append (see agents/rules/environment_variables_bridge.rules.md,
// DXMT9_PE_FORCE_RECORDER_LOCK — the rollback/insurance lane for apps that
// violate that contract despite not passing the flag).
//
// This pins the resolution predicate as a pure function of
// (behaviorFlags, forceLockEnv):
//   flag set              -> locked, regardless of env
//   flag clear + env set  -> locked (rollback lane)
//   flag clear + env clear-> unlocked (the common, default case)

#include "core_spec_fixtures.hpp"
#include "dxmt9/pe_recorder_lock.hpp"

#include <cstdint>
#include <type_traits>

using namespace dxmt9::core::spec;

namespace {

// D3DCREATE_MULTITHREADED's numeric value per the D3D9 headers (d3d9.h);
// mirrored here because this TU does not include <d3d9.h>.
constexpr std::uint32_t kD3DCreateMultithreaded = 0x00000004u;
constexpr std::uint32_t kD3DCreateSoftwareVertexProcessing = 0x00000020u;
constexpr std::uint32_t kD3DCreateHardwareVertexProcessing = 0x00000040u;

void testFlagSetLocksRegardlessOfEnv() {
  check(dxmt9::d3d9::pe::recorderLockRequired(
            kD3DCreateMultithreaded, false),
        "D3DCREATE_MULTITHREADED alone requires the lock");
  check(dxmt9::d3d9::pe::recorderLockRequired(
            kD3DCreateMultithreaded, true),
        "D3DCREATE_MULTITHREADED plus the force env still requires the lock");
  check(dxmt9::d3d9::pe::recorderLockRequired(
            kD3DCreateMultithreaded | kD3DCreateHardwareVertexProcessing,
            false),
        "D3DCREATE_MULTITHREADED combined with unrelated flags still locks");
}

void testFlagClearEnvSetLocks() {
  // The rollback/insurance lane: DXMT9_PE_FORCE_RECORDER_LOCK forces the
  // lock on for apps that violate the D3DCREATE_MULTITHREADED contract
  // (e.g. release resources from a loader thread) without passing the flag.
  check(dxmt9::d3d9::pe::recorderLockRequired(0u, true),
        "no behavior flags but the force env is set -> locked");
  check(dxmt9::d3d9::pe::recorderLockRequired(
            kD3DCreateHardwareVertexProcessing, true),
        "unrelated behavior flags plus the force env -> locked");
}

void testFlagClearEnvClearUnlocked() {
  // The common case for 3DMark05 (behavior=0x40,
  // D3DCREATE_HARDWARE_VERTEXPROCESSING only): no lock is paid.
  check(!dxmt9::d3d9::pe::recorderLockRequired(
            kD3DCreateHardwareVertexProcessing, false),
        "hardware vertex processing only, no force env -> unlocked");
  check(!dxmt9::d3d9::pe::recorderLockRequired(0u, false),
        "no behavior flags, no force env -> unlocked");
  check(!dxmt9::d3d9::pe::recorderLockRequired(
            kD3DCreateSoftwareVertexProcessing, false),
        "software vertex processing only, no force env -> unlocked");
}

void testRecorderAccessPredicateExhaustive() {
  using dxmt9::d3d9::pe::RecorderAccessFacts;
  using dxmt9::d3d9::pe::RecorderAccessLane;
  using dxmt9::d3d9::pe::planRecorderAccess;
  for (unsigned required = 0u; required < 2u; ++required) {
    for (unsigned held = 0u; held < 2u; ++held) {
      for (unsigned owner = 0u; owner < 2u; ++owner) {
        const auto lane = planRecorderAccess(RecorderAccessFacts{
            .lockRequired = required != 0u,
            .lockHeld = held != 0u,
            .ownerThread = owner != 0u,
        });
        const auto expected = required != 0u
            ? (held != 0u ? RecorderAccessLane::Locked
                          : RecorderAccessLane::Denied)
            : (owner != 0u ? RecorderAccessLane::Owner
                           : RecorderAccessLane::Denied);
        check(lane == expected,
              "all owner/conditional-lock witness rows use one predicate");
      }
    }
  }
}

void testScopedBorrowEpochAndTypeClosure() {
  using Capability = dxmt9::d3d9::pe::RecorderLockCapability;
  using Borrow = dxmt9::d3d9::pe::RecorderBorrow<const std::uint32_t>;
  static_assert(!std::is_copy_constructible_v<Capability> &&
                !std::is_move_constructible_v<Capability> &&
                !std::is_copy_assignable_v<Capability> &&
                !std::is_move_assignable_v<Capability>);
  static_assert(!std::is_copy_constructible_v<Borrow> &&
                !std::is_move_constructible_v<Borrow> &&
                !std::is_copy_assignable_v<Borrow> &&
                !std::is_move_assignable_v<Borrow>);

  std::recursive_mutex mutex;
  dxmt9::core::ThreadOwnershipToken owner;
  std::uint64_t epoch = 7u;
  dxmt9::d3d9::pe::RecorderLockGuard guard(mutex, false);
  const auto access = guard.capability(owner, epoch);
  check(access.valid() &&
            access.lane() == dxmt9::d3d9::pe::RecorderAccessLane::Owner,
        "no-MT producer ownership issues the scoped production capability");
  {
    dxmt9::d3d9::pe::RecorderLockGuard lockedGuard(mutex, true);
    const auto lockedAccess = lockedGuard.capability(owner, epoch);
    check(lockedAccess.valid() &&
              lockedAccess.lane() ==
                  dxmt9::d3d9::pe::RecorderAccessLane::Locked,
          "the multithreaded lane issues the same capability under its lock");
  }
  const std::uint32_t value = 0x12345678u;
  Borrow borrow;
  check(access.bind(borrow, value), "valid access binds an immutable borrow");
  std::uint32_t observed = 0u;
  check(borrow.with([&](const std::uint32_t& current) noexcept {
          observed = current;
        }) && observed == value,
        "current-epoch borrow visits synchronously");
  ++epoch;
  check(!access.valid() && !borrow.valid() &&
            !borrow.with([&](const std::uint32_t&) noexcept {}),
        "epoch advance invalidates both mutation and source capabilities");
}

void testProductionGuardSerializesRecorderIntervals() {
  // This is the same guard instantiated by the PE append envelope and the
  // StateBlock/Render Tape cold entry points.  Keep the witness on the
  // production type so a test-only std::mutex cannot hide a guard drift.
  std::recursive_mutex mutex;
  const auto exercise = [&](const char* operation) {
    std::atomic<bool> entered{false};
    std::thread contender;
    {
      dxmt9::d3d9::pe::RecorderLockGuard owner(mutex, true);
      contender = std::thread([&] {
        dxmt9::d3d9::pe::RecorderLockGuard guard(mutex, true);
        entered.store(true, std::memory_order_release);
      });
      std::this_thread::yield();
      check(!entered.load(std::memory_order_acquire),
            std::string(operation) + " must serialize under the production guard");
      // Recursive re-entry is the existing PE contract: Begin/Set/End and
      // child callbacks may reach append/flush helpers while this interval is
      // held, without creating a lock cycle.
      {
        dxmt9::d3d9::pe::RecorderLockGuard nested(mutex, true);
      }
    }
    contender.join();
    check(entered.load(std::memory_order_acquire),
          std::string(operation) + " must admit the next producer after unlock");
  };
  exercise("Begin/Set/End");
  exercise("Capture/Apply");
  exercise("child callback/capture mutation");
}

void testDefaultPoolResetLegalityInterval() {
  // Reset and child AddRef/Release callbacks use the same production guard.
  // Under MULTITHREADED, Reset's legality read must not race a last-resource
  // release (or a concurrent create that adds the first default-pool owner).
  std::recursive_mutex mutex;
  std::uint32_t defaultPoolRefs = 1u;
  std::atomic<bool> releaseStarted{false};
  std::atomic<bool> releaseFinished{false};
  std::thread release;
  {
    dxmt9::d3d9::pe::RecorderLockGuard resetGuard(mutex, true);
    release = std::thread([&] {
      releaseStarted.store(true, std::memory_order_release);
      dxmt9::d3d9::pe::RecorderLockGuard childGuard(mutex, true);
      --defaultPoolRefs;
      releaseFinished.store(true, std::memory_order_release);
    });
    while (!releaseStarted.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    check(defaultPoolRefs == 1u &&
              !releaseFinished.load(std::memory_order_acquire),
          "Reset legality and default-pool child release share one interval");
  }
  release.join();
  check(defaultPoolRefs == 0u &&
            releaseFinished.load(std::memory_order_acquire),
        "default-pool child release settles after Reset leaves the interval");

  // The single-thread contract stays branch-only: a disabled guard must not
  // wait for an unrelated holder or introduce an atomic counter operation.
  std::atomic<bool> holderReady{false};
  std::atomic<bool> releaseHolder{false};
  std::atomic<bool> disabledEntered{false};
  std::thread holder([&] {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    holderReady.store(true, std::memory_order_release);
    while (!releaseHolder.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  });
  while (!holderReady.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  std::thread disabled([&] {
    dxmt9::d3d9::pe::RecorderLockGuard childGuard(mutex, false);
    disabledEntered.store(true, std::memory_order_release);
  });
  disabled.join();
  check(disabledEntered.load(std::memory_order_acquire),
        "disabled default-pool callbacks do not acquire the recorder mutex");
  releaseHolder.store(true, std::memory_order_release);
  holder.join();
}

}  // namespace

int main() {
  try {
    testFlagSetLocksRegardlessOfEnv();
    testFlagClearEnvSetLocks();
    testFlagClearEnvClearUnlocked();
    testRecorderAccessPredicateExhaustive();
    testScopedBorrowEpochAndTypeClosure();
    testProductionGuardSerializesRecorderIntervals();
    testDefaultPoolResetLegalityInterval();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
