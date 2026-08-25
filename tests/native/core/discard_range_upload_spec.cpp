// R-237.5 (docs/perfomance/present-pacing/
// present-pacing-bridge-crossing-decomposition.237.md, "Lane spec:
// DXMT9_DISCARD_RANGE_UPLOAD (V1, experimental candidate)"): pins
// Buffer::unlock route selection for a Default-pool + Dynamic + DISCARD
// writable unlock behind DXMT9_DISCARD_RANGE_UPLOAD, and pins that
// NOOVERWRITE / Managed / plain / readonly routing is unaffected.
//
// The env resolver in src/d3d9/core_buffer.cpp caches on first use, so the
// two states (env unset/"0" vs set) are exercised as two separate test()
// registrations against this one executable (see meson.build), matching the
// dxmt9-replay-byte-identity-{off,on}-spec precedent. This file reads the
// env directly (not through the production resolver) purely to select which
// assertions apply in-process; production route selection is exercised only
// through the real dxmt9c_buffer_lock/unlock entry points below.

#include "core_spec_fixtures.hpp"
#include "device_c_common.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;
using namespace dxmt9::core::spec;

namespace {

// D3DUSAGE_DYNAMIC / D3DLOCK_DISCARD / D3DLOCK_NOOVERWRITE / D3DLOCK_READONLY
// raw bit values, matching lockFlagsToCore()'s constants in
// src/d3d9/device_c_resources.cpp.
constexpr std::uint32_t kUsageDynamic = 0x00000200u;
constexpr std::uint32_t kLockDiscard = 0x00002000u;
constexpr std::uint32_t kLockNoOverwrite = 0x00001000u;
constexpr std::uint32_t kLockReadOnly = 0x00000010u;

bool discardRangeUploadEnvSet() {
  return getenvFlag("DXMT9_DISCARD_RANGE_UPLOAD");
}

// D3DPOOL_DEFAULT == 0, D3DPOOL_MANAGED == 1.
constexpr std::uint32_t kPoolDefault = 0u;
constexpr std::uint32_t kPoolManaged = 1u;

void testDiscardLockRoutingFollowsEnv() {
  using namespace dxmt9::com;

  auto backend = std::make_shared<RecordingBackend>();
  auto* d3d = Direct3DCreate9Ex(D3D_SDK_VERSION, backend);
  check(d3d != nullptr, "factory for DISCARD range routing");

  PresentParameters params{};
  params.backBufferWidth = 320;
  params.backBufferHeight = 240;
  params.windowed = true;
  auto* device = d3d->CreateDeviceEx(0, params, nullptr);
  check(device != nullptr, "device for DISCARD range routing");

  {
    device->AddRef();
    D9CDevice cDevice(device);
    auto* buffer = dxmt9c_device_create_vertex_buffer(
        &cDevice, 256, kUsageDynamic, 0, kPoolDefault);
    check(buffer != nullptr, "create Default+Dynamic buffer");

    void* data = nullptr;
    checkEq(dxmt9c_buffer_lock(buffer, 0, 256, &data, 0), D3D_OK,
            "initial full lock");
    std::memset(data, 0x5a, 256);
    checkEq(dxmt9c_buffer_unlock(buffer), D3D_OK, "initial full unlock");
    checkEq(backend->bufferUploads.size(), size_t{1},
            "initial lock always uses full upload");
    checkEq(backend->bufferRangeUploads.size(), size_t{0},
            "initial lock enqueues no range upload");

    // D3DLOCK_DISCARD on a Default+Dynamic buffer zero-fills the whole
    // backing and always locks from offset 0 (Buffer::lock in
    // src/d3d9/core_buffer.cpp treats DISCARD as "rename the whole
    // buffer"), regardless of the offset requested here. A 32-byte
    // partial-size DISCARD lock is still meaningful: it exercises the
    // range path with lockedOffset_==0, lockedSize_==32 < desc.size==256.
    data = nullptr;
    checkEq(dxmt9c_buffer_lock(buffer, 64, 32, &data, kLockDiscard), D3D_OK,
            "Default+Dynamic DISCARD partial lock");
    std::memset(data, 0xa5, 32);
    checkEq(dxmt9c_buffer_unlock(buffer), D3D_OK,
            "Default+Dynamic DISCARD partial unlock");

    if (discardRangeUploadEnvSet()) {
      checkEq(backend->bufferUploads.size(), size_t{1},
              "env on: DISCARD unlock does not add a full upload");
      checkEq(backend->bufferRangeUploads.size(), size_t{1},
              "env on: DISCARD unlock enqueues one range upload");
      const auto& range = backend->bufferRangeUploads.back();
      checkEq(range.offset, std::uint64_t{0},
              "env on: DISCARD lock always resets to offset 0");
      checkEq(range.bytes.size(), size_t{32},
              "env on: DISCARD range upload preserves the lock size");
      check(std::all_of(range.bytes.begin(), range.bytes.end(),
                        [](std::uint8_t value) { return value == 0xa5u; }),
            "env on: DISCARD range upload carries only the written bytes");
    } else {
      checkEq(backend->bufferUploads.size(), size_t{2},
              "env off: DISCARD unlock takes the full-upload path");
      checkEq(backend->bufferRangeUploads.size(), size_t{0},
              "env off: DISCARD unlock enqueues no range upload");
      const auto& upload = backend->bufferUploads.back().second;
      checkEq(upload.size(), size_t{256},
              "env off: DISCARD full upload carries the whole buffer extent");
      check(upload[0] == 0xa5u && upload[31] == 0xa5u,
            "env off: DISCARD full upload includes the written subrange");
      check(upload[32] == 0u,
            "env off: DISCARD zero-fills the rest of the renamed backing");
    }

    checkEq(dxmt9c_buffer_release(buffer), 0u,
            "release DISCARD routing buffer");
  }
  checkEq(device->Release(), 0u, "release DISCARD routing device");
  checkEq(d3d->Release(), 0u, "release DISCARD routing factory");
}

void testNoOverwriteRoutingUnaffectedByEnv() {
  using namespace dxmt9::com;

  auto backend = std::make_shared<RecordingBackend>();
  auto* d3d = Direct3DCreate9Ex(D3D_SDK_VERSION, backend);
  check(d3d != nullptr, "factory for NOOVERWRITE env-independence");

  PresentParameters params{};
  params.backBufferWidth = 320;
  params.backBufferHeight = 240;
  params.windowed = true;
  auto* device = d3d->CreateDeviceEx(0, params, nullptr);
  check(device != nullptr, "device for NOOVERWRITE env-independence");

  {
    device->AddRef();
    D9CDevice cDevice(device);
    auto* buffer = dxmt9c_device_create_vertex_buffer(
        &cDevice, 256, kUsageDynamic, 0, kPoolDefault);
    check(buffer != nullptr, "create Default+Dynamic buffer for NOOVERWRITE");

    void* data = nullptr;
    checkEq(dxmt9c_buffer_lock(buffer, 0, 256, &data, 0), D3D_OK,
            "NOOVERWRITE fixture initial full lock");
    std::memset(data, 0x11, 256);
    checkEq(dxmt9c_buffer_unlock(buffer), D3D_OK,
            "NOOVERWRITE fixture initial full unlock");

    data = nullptr;
    checkEq(dxmt9c_buffer_lock(buffer, 96, 96, &data, kLockNoOverwrite),
            D3D_OK, "NOOVERWRITE range lock");
    std::memset(data, 0x22, 96);
    checkEq(dxmt9c_buffer_unlock(buffer), D3D_OK, "NOOVERWRITE range unlock");

    checkEq(backend->bufferUploads.size(), size_t{1},
            "NOOVERWRITE never adds a full upload regardless of env");
    checkEq(backend->bufferRangeUploads.size(), size_t{1},
            "NOOVERWRITE always enqueues exactly one range upload");
    const auto& range = backend->bufferRangeUploads.back();
    checkEq(range.offset, std::uint64_t{96},
            "NOOVERWRITE preserves the lock offset regardless of env");
    checkEq(range.bytes.size(), size_t{96},
            "NOOVERWRITE preserves the lock size regardless of env");

    checkEq(dxmt9c_buffer_release(buffer), 0u,
            "release NOOVERWRITE env-independence buffer");
  }
  checkEq(device->Release(), 0u, "release NOOVERWRITE env-independence device");
  checkEq(d3d->Release(), 0u, "release NOOVERWRITE env-independence factory");
}

void testManagedDiscardRoutingUnaffectedByEnv() {
  using namespace dxmt9::com;

  auto backend = std::make_shared<RecordingBackend>();
  auto* d3d = Direct3DCreate9Ex(D3D_SDK_VERSION, backend);
  check(d3d != nullptr, "factory for Managed DISCARD env-independence");

  PresentParameters params{};
  params.backBufferWidth = 320;
  params.backBufferHeight = 240;
  params.windowed = true;
  auto* device = d3d->CreateDeviceEx(0, params, nullptr);
  check(device != nullptr, "device for Managed DISCARD env-independence");

  {
    device->AddRef();
    D9CDevice cDevice(device);
    // Managed buffers are never Dynamic in this fixture; DISCARD on Managed
    // must always take the full-shadow path regardless of the env.
    auto* buffer = dxmt9c_device_create_vertex_buffer(
        &cDevice, 64, 0, 0, kPoolManaged);
    check(buffer != nullptr, "create Managed buffer for DISCARD");

    void* data = nullptr;
    checkEq(dxmt9c_buffer_lock(buffer, 0, 64, &data, kLockDiscard), D3D_OK,
            "Managed DISCARD lock");
    std::memset(data, 0x33, 64);
    checkEq(dxmt9c_buffer_unlock(buffer), D3D_OK, "Managed DISCARD unlock");

    checkEq(backend->bufferUploads.size(), size_t{1},
            "Managed DISCARD always uses the full-upload path");
    checkEq(backend->bufferRangeUploads.size(), size_t{0},
            "Managed DISCARD never enqueues a range upload");

    checkEq(dxmt9c_buffer_release(buffer), 0u,
            "release Managed DISCARD env-independence buffer");
  }
  checkEq(device->Release(), 0u, "release Managed DISCARD env-independence device");
  checkEq(d3d->Release(), 0u, "release Managed DISCARD env-independence factory");
}

void testPlainDefaultDiscardRoutingUnaffectedByEnv() {
  using namespace dxmt9::com;

  auto backend = std::make_shared<RecordingBackend>();
  auto* d3d = Direct3DCreate9Ex(D3D_SDK_VERSION, backend);
  check(d3d != nullptr, "factory for plain Default DISCARD env-independence");

  PresentParameters params{};
  params.backBufferWidth = 320;
  params.backBufferHeight = 240;
  params.windowed = true;
  auto* device = d3d->CreateDeviceEx(0, params, nullptr);
  check(device != nullptr, "device for plain Default DISCARD env-independence");

  {
    device->AddRef();
    D9CDevice cDevice(device);
    // Default pool without D3DUSAGE_DYNAMIC: discardRange requires Dynamic,
    // so this must always take the full-shadow path regardless of the env.
    auto* buffer = dxmt9c_device_create_vertex_buffer(
        &cDevice, 64, 0, 0, kPoolDefault);
    check(buffer != nullptr, "create plain Default buffer for DISCARD");

    void* data = nullptr;
    checkEq(dxmt9c_buffer_lock(buffer, 0, 64, &data, kLockDiscard), D3D_OK,
            "plain Default DISCARD lock");
    std::memset(data, 0x44, 64);
    checkEq(dxmt9c_buffer_unlock(buffer), D3D_OK,
            "plain Default DISCARD unlock");

    checkEq(backend->bufferUploads.size(), size_t{1},
            "plain Default DISCARD always uses the full-upload path");
    checkEq(backend->bufferRangeUploads.size(), size_t{0},
            "plain Default DISCARD never enqueues a range upload");

    checkEq(dxmt9c_buffer_release(buffer), 0u,
            "release plain Default DISCARD env-independence buffer");
  }
  checkEq(device->Release(), 0u,
          "release plain Default DISCARD env-independence device");
  checkEq(d3d->Release(), 0u,
          "release plain Default DISCARD env-independence factory");
}

void testReadonlyDiscardUnlockUploadsNothingRegardlessOfEnv() {
  using namespace dxmt9::com;

  auto backend = std::make_shared<RecordingBackend>();
  auto* d3d = Direct3DCreate9Ex(D3D_SDK_VERSION, backend);
  check(d3d != nullptr, "factory for readonly DISCARD env-independence");

  PresentParameters params{};
  params.backBufferWidth = 320;
  params.backBufferHeight = 240;
  params.windowed = true;
  auto* device = d3d->CreateDeviceEx(0, params, nullptr);
  check(device != nullptr, "device for readonly DISCARD env-independence");

  {
    device->AddRef();
    D9CDevice cDevice(device);
    auto* buffer = dxmt9c_device_create_vertex_buffer(
        &cDevice, 256, kUsageDynamic, 0, kPoolDefault);
    check(buffer != nullptr, "create Default+Dynamic buffer for readonly");

    void* data = nullptr;
    checkEq(dxmt9c_buffer_lock(buffer, 0, 256, &data, 0), D3D_OK,
            "readonly fixture initial full lock");
    std::memset(data, 0x66, 256);
    checkEq(dxmt9c_buffer_unlock(buffer), D3D_OK,
            "readonly fixture initial full unlock");
    const auto uploadsBeforeReadonly = backend->bufferUploads.size();
    const auto rangeUploadsBeforeReadonly = backend->bufferRangeUploads.size();

    data = nullptr;
    checkEq(dxmt9c_buffer_lock(buffer, 0, 256, &data, kLockReadOnly), D3D_OK,
            "readonly lock");
    checkEq(dxmt9c_buffer_unlock(buffer), D3D_OK, "readonly unlock");

    checkEq(backend->bufferUploads.size(), uploadsBeforeReadonly,
            "readonly unlock uploads nothing regardless of env");
    checkEq(backend->bufferRangeUploads.size(), rangeUploadsBeforeReadonly,
            "readonly unlock enqueues no range upload regardless of env");

    checkEq(dxmt9c_buffer_release(buffer), 0u,
            "release readonly DISCARD env-independence buffer");
  }
  checkEq(device->Release(), 0u,
          "release readonly DISCARD env-independence device");
  checkEq(d3d->Release(), 0u,
          "release readonly DISCARD env-independence factory");
}

}  // namespace

int main() {
  try {
    testDiscardLockRoutingFollowsEnv();
    testNoOverwriteRoutingUnaffectedByEnv();
    testManagedDiscardRoutingUnaffectedByEnv();
    testPlainDefaultDiscardRoutingUnaffectedByEnv();
    testReadonlyDiscardUnlockUploadsNothingRegardlessOfEnv();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
