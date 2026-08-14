#pragma once

#include "device_c_render_tape_descriptors.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

// Public D3DFORMAT values from d3d9types.h. The host-native capture/layout
// tests cannot include the Windows header, so the wire-neutral helper pins the
// values here and the PE translation unit static_asserts them against the
// toolchain's actual D3DFMT_* enumerants.
namespace render_tape_d3d_format {
inline constexpr std::uint32_t R8G8B8 = 20u;
inline constexpr std::uint32_t A8R8G8B8 = 21u;
inline constexpr std::uint32_t X8R8G8B8 = 22u;
inline constexpr std::uint32_t R5G6B5 = 23u;
inline constexpr std::uint32_t X1R5G5B5 = 24u;
inline constexpr std::uint32_t A1R5G5B5 = 25u;
inline constexpr std::uint32_t A4R4G4B4 = 26u;
inline constexpr std::uint32_t X4R4G4B4 = 30u;
inline constexpr std::uint32_t A8 = 28u;
inline constexpr std::uint32_t A8B8G8R8 = 32u;
inline constexpr std::uint32_t X8B8G8R8 = 33u;
inline constexpr std::uint32_t A8P8 = 40u;
inline constexpr std::uint32_t P8 = 41u;
inline constexpr std::uint32_t L8 = 50u;
inline constexpr std::uint32_t A8L8 = 51u;
inline constexpr std::uint32_t V8U8 = 60u;
inline constexpr std::uint32_t L16 = 81u;
inline constexpr std::uint32_t D24S8 = 75u;
inline constexpr std::uint32_t DXT1 = 0x31545844u;
inline constexpr std::uint32_t DXT2 = 0x32545844u;
inline constexpr std::uint32_t DXT3 = 0x33545844u;
inline constexpr std::uint32_t DXT4 = 0x34545844u;
inline constexpr std::uint32_t DXT5 = 0x35545844u;
} // namespace render_tape_d3d_format

enum class RenderTapeCaptureRejectionReason : std::uint32_t {
  InvalidLockIdentity = 1u,
  UnsupportedLockFormat = 2u,
  InvalidBlockAlignment = 3u,
  InvalidLockPitch = 4u,
  LockLayoutOverflow = 5u,
  LockCopyFailed = 6u,
  IncompleteSubresourceSeed = 7u,
  DescriptorMismatch = 8u,
  FullSnapshotLockFailed = 9u,
  FullSnapshotCopyFailed = 10u,
  FullSnapshotUnlockFailed = 11u,
  FullSnapshotIdentityMismatch = 12u,
  FullSnapshotExtentMismatch = 13u,
  UnmaterializedPreArmObject = 14u,
  ExpectedContentContract = 15u,
};

const char* renderTapeCaptureRejectionReasonName(
    RenderTapeCaptureRejectionReason reason) noexcept;

struct RenderTapeLockRect {
  std::int32_t left = 0;
  std::int32_t top = 0;
  std::int32_t right = 0;
  std::int32_t bottom = 0;
};

struct RenderTapeCaptureLayoutDiagnostic {
  std::uint32_t format = 0u;
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  std::int32_t pitch = 0;
  std::uint64_t bytes = 0u;
};

enum class RenderTapeExpectedContentStatus : std::uint32_t {
  NotRequired = 0u,
  Accepted,
  InvalidDescriptor,
  UnsupportedFormat,
  UnsupportedDimension,
  InvalidExtent,
  Overflow,
};

struct RenderTapeExpectedContentContract {
  RenderTapeExpectedContentStatus status =
      RenderTapeExpectedContentStatus::NotRequired;
  std::uint64_t bytes = 0u;
  std::uint32_t count = 0u;
};

const char* renderTapeExpectedContentStatusName(
    RenderTapeExpectedContentStatus status) noexcept;

// Derives the exact initial-content extent for every content-bearing resource
// descriptor. Texture and standalone-surface inputs must use their canonical
// V2 descriptors; volume seed closure is explicitly unsupported. Identities
// whose contents are supplied by an immutable blob, an alias parent, or
// PresentOutput return NotRequired with a zero/zero contract.
RenderTapeExpectedContentContract renderTapeDeriveExpectedContentContract(
    std::uint32_t identityKind,
    std::span<const std::byte> descriptor,
    std::span<std::uint64_t> subresourceBytes = {}) noexcept;

RenderTapeExpectedContentStatus renderTapeValidateExpectedContentExtents(
    std::uint32_t identityKind, std::span<const std::byte> descriptor,
    std::span<const std::uint64_t> actualSubresourceBytes) noexcept;

// Cold, value-only description of one live object at the capture boundary.
// `complete` means that every content subresource required by the descriptor
// has an exact seed.  A descriptor dependency is deliberately explicit: the
// closure must not infer ownership from a kind/object-id pair or from a stale
// generation.
struct RenderTapeBootstrapClosureObject {
  D9CWireObjectIdentity identity{};
  bool complete = false;
  bool hasDescriptorDependency = false;
  D9CWireObjectIdentity descriptorDependency{};
};

enum class RenderTapeBootstrapClosureStatus : std::uint32_t {
  Accepted = 0u,
  ReferencedObjectMissing,
  ReferencedObjectIncomplete,
  DescriptorDependencyMissing,
  DescriptorDependencyIncomplete,
  DuplicateObjectIdentity,
  InvalidDescriptorDependency,
};

RenderTapeBootstrapClosureStatus renderTapeBuildBootstrapClosure(
    std::span<const D9CWireObjectIdentity> bootstrapHandles,
    const D9CWireObjectIdentity& presentOutput,
    std::span<const RenderTapeBootstrapClosureObject> objects,
    std::vector<D9CWireObjectIdentity>& closure) noexcept;

bool renderTapeBootstrapClosureContains(
    std::span<const D9CWireObjectIdentity> closure,
    const D9CWireObjectIdentity& identity) noexcept;

bool renderTapeBootstrapRequiresAllLiveObjects(
    std::uint32_t profile) noexcept;

// This status is shared by the value-only snapshot decision and the PE
// texture seam.  A required snapshot is the only path allowed to turn an
// unseeded partial lock into a complete seed; every failure remains typed and
// fail-closed.
enum class RenderTapeFullSnapshotStatus : std::uint32_t {
  Accepted = 0u,
  NotRequired,
  Required,
  InvalidIdentity,
  InvalidExtent,
  InvalidBytes,
};

// A surface wrapper may only use the exact-owner full snapshot fallback when
// it is a 2D texture-level alias and its mutation identity is the owning
// texture generation.  Keep this route decision value-only so the PE seam and
// native tests cannot silently widen it to standalone/cube/volume surfaces.
enum class RenderTapeSurfaceSnapshotRoute : std::uint32_t {
  NotRequired = 0u,
  TextureDerived,
  StandaloneSurface,
  InvalidIdentity,
};

RenderTapeSurfaceSnapshotRoute renderTapeClassifySurfaceSnapshotRoute(
    bool captureTrackingEnabled, bool ownerIsTexture2D,
    bool mutationIdentityIsTexture, bool partialMutation,
    bool mutationBytesPresent) noexcept;

struct RenderTapeBlockLockLayout {
  std::uint32_t blockBytes = 0u;
  std::uint32_t fullRowBytes = 0u;
  std::uint32_t fullRows = 0u;
  std::uint32_t blockLeft = 0u;
  std::uint32_t blockTop = 0u;
  std::uint32_t rowBytes = 0u;
  std::uint32_t rows = 0u;
  std::uint32_t pitch = 0u;
  std::uint64_t tightBytes = 0u;
  std::uint64_t sourceBytes = 0u;
  bool fullSubresource = false;
};

// Normal LockRect calls return a pointer at the requested rectangle origin
// while Pitch remains the full subresource row stride. The PE user-memory
// lane is the documented exception and returns the subresource base; its exact
// checked offset is represented separately so both paths use this one layout.
struct RenderTapeLinearLockLayout {
  std::uint32_t bytesPerPixel = 0u;
  std::uint32_t fullRowBytes = 0u;
  std::uint32_t fullRows = 0u;
  std::uint32_t destinationByteOffset = 0u;
  std::uint32_t top = 0u;
  std::uint32_t rowBytes = 0u;
  std::uint32_t rows = 0u;
  std::uint32_t pitch = 0u;
  std::uint64_t tightBytes = 0u;
  std::uint64_t sourceBytes = 0u;
  std::uint64_t subresourceSourceOffset = 0u;
  std::uint64_t subresourceSourceBytes = 0u;
  bool fullSubresource = false;
};

enum class RenderTapeLockBitsOrigin : std::uint32_t {
  Rectangle = 0u,
  Subresource = 1u,
};

RenderTapeFullSnapshotStatus renderTapeClassifySnapshot(
    bool captureTrackingEnabled, bool identityMatches, bool extentMatches,
    bool partialLock, std::size_t existingContentBytes,
    std::uint64_t expectedBytes) noexcept;

// Buffer seed closure has a one-dimensional extent rather than a surface
// row/column layout. Keep its decision predicate explicit and value-only so
// PE lock code and native tests share the same generation/extent gate.
RenderTapeFullSnapshotStatus renderTapeClassifyBufferSnapshot(
    bool captureTrackingEnabled, bool identityMatches, bool extentMatches,
    bool partialWritableLock, std::size_t existingContentBytes,
    std::uint64_t expectedBytes) noexcept;

RenderTapeFullSnapshotStatus renderTapeValidateFullSnapshot(
    bool fullSubresource, std::uint64_t expectedBytes,
    std::span<const std::byte> bytes) noexcept;

enum class RenderTapeBlockLayoutStatus : std::uint32_t {
  Accepted = 0u,
  UnsupportedFormat,
  InvalidExtent,
  InvalidAlignment,
  InvalidPitch,
  Overflow,
};

std::uint32_t renderTapeLinearBytesPerPixel(std::uint32_t format) noexcept;

RenderTapeBlockLayoutStatus renderTapeBlockLockLayout(
    const D9CSurfaceDesc& desc, std::int32_t pitch,
    const RenderTapeLockRect* rect, RenderTapeBlockLockLayout& out) noexcept;

bool copyRenderTapeBlockRows(const void* bits,
                             const RenderTapeBlockLockLayout& layout,
                             std::vector<std::byte>& out) noexcept;

enum class RenderTapeBlockMutationStatus : std::uint32_t {
  Accepted = 0u,
  IncompleteSeed,
  InvalidLayout,
  InvalidBytes,
  Overflow,
  AllocationFailed,
};

RenderTapeBlockMutationStatus applyRenderTapeBlockMutation(
    const RenderTapeBlockLockLayout& layout, std::span<const std::byte> bytes,
    std::vector<std::byte>& completeContent) noexcept;

RenderTapeBlockMutationStatus applyRenderTapeBufferMutation(
    std::uint64_t expectedBytes, std::uint64_t byteOffset,
    std::span<const std::byte> bytes,
    std::vector<std::byte>& completeContent) noexcept;

enum class RenderTapeLinearLayoutStatus : std::uint32_t {
  Accepted = 0u,
  UnsupportedFormat,
  InvalidExtent,
  InvalidPitch,
  Overflow,
};

RenderTapeLinearLayoutStatus renderTapeLinearLockLayout(
    const D9CSurfaceDesc& desc, std::int32_t pitch,
    const RenderTapeLockRect* rect, RenderTapeLinearLockLayout& out) noexcept;

bool copyRenderTapeLinearRows(const void* bits,
                              const RenderTapeLinearLockLayout& layout,
                              std::vector<std::byte>& out,
                              RenderTapeLockBitsOrigin origin =
                                  RenderTapeLockBitsOrigin::Rectangle) noexcept;

bool writeRenderTapeLinearRows(std::span<const std::byte> tightBytes,
                               void* bits,
                               const RenderTapeLinearLockLayout& layout) noexcept;

RenderTapeBlockMutationStatus applyRenderTapeLinearMutation(
    const RenderTapeLinearLockLayout& layout, std::span<const std::byte> bytes,
    std::vector<std::byte>& completeContent) noexcept;

} // namespace dxmt9::d3d9
