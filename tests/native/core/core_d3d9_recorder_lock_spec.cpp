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

#include <cstdint>

using namespace dxmt9::core::spec;

namespace {

// D3DCREATE_MULTITHREADED's numeric value per the D3D9 headers (d3d9.h);
// mirrored here because this TU does not include <d3d9.h>.
constexpr std::uint32_t kD3DCreateMultithreaded = 0x00000004u;
constexpr std::uint32_t kD3DCreateSoftwareVertexProcessing = 0x00000020u;
constexpr std::uint32_t kD3DCreateHardwareVertexProcessing = 0x00000040u;

// Mirrors dxmt9PeRecorderLockRequired() in src/d3d9/d3d9_pe_device.cpp.
bool peRecorderLockRequired(std::uint32_t behaviorFlags,
                            bool forceLockEnv) {
  return (behaviorFlags & kD3DCreateMultithreaded) != 0 || forceLockEnv;
}

void testFlagSetLocksRegardlessOfEnv() {
  check(peRecorderLockRequired(kD3DCreateMultithreaded, false),
        "D3DCREATE_MULTITHREADED alone requires the lock");
  check(peRecorderLockRequired(kD3DCreateMultithreaded, true),
        "D3DCREATE_MULTITHREADED plus the force env still requires the lock");
  check(peRecorderLockRequired(
            kD3DCreateMultithreaded | kD3DCreateHardwareVertexProcessing,
            false),
        "D3DCREATE_MULTITHREADED combined with unrelated flags still locks");
}

void testFlagClearEnvSetLocks() {
  // The rollback/insurance lane: DXMT9_PE_FORCE_RECORDER_LOCK forces the
  // lock on for apps that violate the D3DCREATE_MULTITHREADED contract
  // (e.g. release resources from a loader thread) without passing the flag.
  check(peRecorderLockRequired(0u, true),
        "no behavior flags but the force env is set -> locked");
  check(peRecorderLockRequired(kD3DCreateHardwareVertexProcessing, true),
        "unrelated behavior flags plus the force env -> locked");
}

void testFlagClearEnvClearUnlocked() {
  // The common case for 3DMark05 (behavior=0x40,
  // D3DCREATE_HARDWARE_VERTEXPROCESSING only): no lock is paid.
  check(!peRecorderLockRequired(kD3DCreateHardwareVertexProcessing, false),
        "hardware vertex processing only, no force env -> unlocked");
  check(!peRecorderLockRequired(0u, false),
        "no behavior flags, no force env -> unlocked");
  check(!peRecorderLockRequired(kD3DCreateSoftwareVertexProcessing, false),
        "software vertex processing only, no force env -> unlocked");
}

}  // namespace

int main() {
  try {
    testFlagSetLocksRegardlessOfEnv();
    testFlagClearEnvSetLocks();
    testFlagClearEnvClearUnlocked();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
