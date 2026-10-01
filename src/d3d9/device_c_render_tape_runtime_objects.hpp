#pragma once

#include "device_c_chunk_validate.hpp"
#include "device_c_render_tape_provider.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

struct D9CDevice;

namespace dxmt9::core {
struct QueryPollSnapshot;
}

namespace dxmt9::d3d9::render_tape_runtime {

// A definition view owns its fixed wire header but borrows the descriptor
// bytes from the immutable tape input.  The borrow is valid only while that
// tape storage remains alive; replay plans do not extend the tape lifetime.
struct Definition {
  RenderTapeObjectDefineHeader fixed{};
  std::span<const std::byte> descriptor{};
};

// One provider-owned acquisition. `value` is a raw D9C wrapper whose single
// ownership is transferred to the table by createRenderTapeObject's caller.
// The table must keep these records alive until all command consumers finish.
struct OwnedObject {
  D9CWireObjectIdentity identity{};
  void* value = nullptr;
};

// A command chunk view borrows immutable, already-admitted tape bytes and its
// envelope for the duration of resolveCommandHandles.
struct CommandChunkView {
  CommandChunkEnvelope envelope{};
  std::span<const std::byte> bytes{};
};

// A pure, resource-free admission plan for the general provider grammar.
// `tape`, definition descriptors, and command chunk bytes borrow the caller's
// immutable tape storage.  The plan must not outlive that storage. Gamma and
// ordered-control payloads retain their existing value-owned layout. On a
// failed build, the destination plan is left byte-for-byte/value-for-value
// unchanged.
struct GeneralPreflightPlan {
  struct ReplayEvent {
    RenderTapeEventType type = RenderTapeEventType::ObjectDefine;
    std::uint32_t eventIndex = 0u;
    std::uint32_t index = 0u;
    std::uint32_t count = 0u;
    std::uint32_t aux = 0u;
  };

  ImportedRenderTapeView tape{};
  RenderTapeBlobCatalogue catalogue{};
  std::vector<Definition> definitions{};
  std::vector<ReplayEvent> events{};
  std::vector<CommandChunkView> bootstrapChunks{};
  std::vector<std::vector<std::byte>> bootstrapGamma{};
  std::vector<CommandChunkView> commandChunks{};
  std::vector<RenderTapeResourceMutationHeader> mutations{};
  std::vector<RenderTapeObjectDestroyHeader> destroys{};
  std::vector<RenderTapeOrderedControlHeader> controls{};
  std::vector<std::vector<std::byte>> controlBytes{};
  std::vector<D9CWireObjectIdentity> bootstrapIdentities{};
  D9CWireObjectIdentity outputIdentity{};
  D9CSurfaceDesc outputDesc{};
  RenderTapeDigestValidity expectedDigestValidity =
      RenderTapeDigestValidity::NotCaptured;
  RenderTapeDigest expectedDigest{};
  std::uint64_t presentOrdinal = 0u;
  std::uint64_t completionOrdinal = 0u;
};

// Builds the same pure general-provider preflight used by historical replay.
// No provider/device/Metal effect occurs. Input tape and blob spans are
// borrowed for the duration of this call; successful plan views borrow the
// tape bytes until the plan is discarded. Blob bytes are authenticated during
// this call and represented only by the value-owned catalogue in the plan;
// callers must keep blob storage alive for any later replay that consumes it.
// Admission is transactional: failure leaves `plan` unchanged.
FrameTapeReplayResult buildGeneralReplayPlan(
    std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs,
    GeneralPreflightPlan* plan);

// Reference form for callers that need the admitted plan.
inline FrameTapeReplayResult buildGeneralReplayPlan(
    std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs,
    GeneralPreflightPlan& plan) {
  return buildGeneralReplayPlan(tape, blobs, &plan);
}

// Exact identity includes kind, slot generation, and object id. The table is
// a borrowed, stable view whose live records have unique identities. A null
// result means the identity is absent or its acquisition has been released.
void* findExactObject(std::span<const OwnedObject> objects,
                      const D9CWireObjectIdentity& identity) noexcept;

// Releases the one owned wrapper, then nulls value. Safe to call repeatedly;
// identity is retained only as bookkeeping and is not an ownership token.
void releaseOwnedObject(OwnedObject& object) noexcept;

// Resolves every wire handle in a chunk that has already passed immutable
// command validation. The object table must remain stable for this call and
// have unique identities. `resolved` is caller-owned scratch with capacity
// for at least the chunk handle count; no pointers escape that scratch. On
// failure, no scratch entry is published and `resolvedCount` remains zero.
bool resolveCommandHandles(const CommandChunkView& chunk,
                           std::span<const OwnedObject> objects,
                           std::span<void*> resolved,
                           std::size_t& resolvedCount) noexcept;

// Creates exactly one wrapper for a prevalidated ObjectDefine record. The
// descriptor and blob catalogue must remain valid for this call only, and the
// object table must be a stable, unique-identity borrowed view;
// device must be a valid provider device for non-alias objects.
// returned pointer owns one wrapper reference. Texture-subresource surfaces
// borrow their parent texture's storage but return an independently releasable
// surface wrapper. `objects` is a non-owning view used only for alias lookup.
void* createRenderTapeObject(
    D9CDevice* device, const RenderTapeObjectDefineHeader& fixed,
    std::span<const std::byte> descriptor,
    std::span<const RenderTapeProviderBlob> blobs,
    std::span<const OwnedObject> objects);

// Cold supplemental oracle. It polls a retained immutable local issue receipt,
// never the public Query that may already have been reissued or released. The
// bounded work and elapsed cost are kept outside the recorded transcript.
RenderTapeQueryReplayFailure settleQueryOracle(
    D9CDevice* device, const core::QueryPollSnapshot& snapshot,
    RenderTapeQueryReplayRow& row,
    RenderTapeQueryReplayEvidence& evidence) noexcept;

} // namespace dxmt9::d3d9::render_tape_runtime
