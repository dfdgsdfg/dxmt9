#include "render_tape_cpu_resource_observation.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace dxmt9::test;
using Status = CpuTapeResourceObservationStatus;
constexpr std::uint64_t offset = 1469598103934665603ull;
constexpr D9CWireObjectIdentity buffer{D9C_CHUNK_HANDLE_KIND_BUFFER, 7u, 901u};
constexpr D9CWireObjectIdentity texture{D9C_CHUNK_HANDLE_KIND_TEXTURE, 8u, 902u};
constexpr D9CWireObjectIdentity surface{D9C_CHUNK_HANDLE_KIND_SURFACE, 9u, 903u};
std::size_t checks = 0u;

void check(bool value, std::string_view message) {
  ++checks;
  if (!value) throw std::runtime_error(std::string(message));
}

bool sameIdentity(D9CWireObjectIdentity a, D9CWireObjectIdentity b) {
  return a.kind == b.kind && a.generation == b.generation && a.objectId == b.objectId;
}

template <class T>
void appendScalar(std::vector<std::byte>& bytes, T value) {
  const auto prior = bytes.size();
  bytes.resize(prior + sizeof(value));
  std::memcpy(bytes.data() + prior, &value, sizeof(value));
}

// Independent legacy reference: materialize its exact scalar/SHA field stream
// before hashing. Neither normalization nor FNV primitives call the helper.
std::vector<std::byte> legacyStream(const CpuTapeResourceObservationInput& input) {
  std::vector<std::byte> bytes;
  appendScalar(bytes, input.identity.kind);
  appendScalar(bytes, input.identity.generation);
  appendScalar(bytes, input.identity.objectId);
  if (input.depthStencil) {
    const auto depth = *input.depthStencil;
    appendScalar(bytes, depth.disposition);
    appendScalar(bytes, depth.format);
    appendScalar(bytes, depth.width);
    appendScalar(bytes, depth.height);
    appendScalar(bytes, depth.depthPitch);
    appendScalar(bytes, depth.stencilPitch);
    appendScalar(bytes, depth.stencilOffset);
    appendScalar(bytes, depth.byteCount);
  }
  std::uint32_t count = 0u;
  for (const auto& receipt : input.receipts) {
    if (!sameIdentity(receipt.identity, input.contentIdentity) ||
        (input.aliasSubresource && receipt.subresource != *input.aliasSubresource)) continue;
    ++count;
    appendScalar(bytes, receipt.subresource);
    bytes.insert(bytes.end(), receipt.digest.begin(), receipt.digest.end());
    appendScalar(bytes, receipt.revision);
  }
  appendScalar(bytes, count);
  return bytes;
}

std::uint64_t legacyHash(const CpuTapeResourceObservationInput& input,
                         std::uint64_t seed = offset) {
  for (const auto byte : legacyStream(input)) {
    seed ^= std::to_integer<unsigned char>(byte);
    seed *= 1099511628211ull;
  }
  return seed;
}

CpuTapeResourceReceipt receipt(D9CWireObjectIdentity identity,
    std::uint32_t subresource, std::uint8_t salt, std::uint64_t revision) {
  CpuTapeResourceReceipt out{.identity = identity, .subresource = subresource,
      .revision = revision};
  for (std::size_t index = 0u; index < out.digest.size(); ++index)
    out.digest[index] = static_cast<std::byte>((salt + 7u * index) & 0xffu);
  return out;
}

std::uint64_t hash(const CpuTapeResourceObservation& observation,
                   std::uint64_t seed = offset) {
  check(appendCpuTapeResourceObservation(observation, seed), "owned observation emits");
  return seed;
}

void exactStreamAndOrdering() {
  std::vector<CpuTapeResourceReceipt> receipts{
      receipt(buffer, 3u, 11u, 5u), receipt(texture, 0u, 22u, 2u),
      receipt(buffer, 0u, 33u, 9u)};
  CpuTapeResourceObservationInput input{.identity = buffer, .contentIdentity = buffer,
      .receipts = receipts};
  CpuTapeResourceObservation observation;
  check(buildCpuTapeResourceObservation(input, observation) == Status::Accepted,
      "buffer normalization copies the matching receipts only");
  check(observation.receipts().size() == 2u && observation.receipts()[0].subresource == 3u &&
      observation.receipts()[1].subresource == 0u, "receipt order is not sorted or repartitioned");
  for (const auto seed : {offset, std::uint64_t{0}, std::uint64_t{0x8123456789abcdefull}})
    check(hash(observation, seed) == legacyHash(input, seed), "arbitrary incoming hash preserves exact legacy field bytes");
  auto repeated = hash(observation);
  check(appendCpuTapeResourceObservation(observation, repeated) &&
      repeated == legacyHash(input, legacyHash(input)), "repeated emission appends fields instead of appending a prehash");

  const auto original = hash(observation);
  std::swap(receipts[0], receipts[2]);
  CpuTapeResourceObservation reordered;
  check(buildCpuTapeResourceObservation(input, reordered) == Status::Accepted &&
      hash(reordered) == legacyHash(input) && hash(reordered) != original,
      "reordering two matched receipts is a semantic mismatch");
  ++receipts[0].revision;
  CpuTapeResourceObservation revised;
  check(buildCpuTapeResourceObservation(input, revised) == Status::Accepted &&
      hash(revised) == legacyHash(input) && hash(revised) != hash(reordered),
      "every receipt's uint64 revision remains in the stream");
  receipts[0].digest[31] ^= std::byte{1};
  CpuTapeResourceObservation changed;
  check(buildCpuTapeResourceObservation(input, changed) == Status::Accepted &&
      hash(changed) == legacyHash(input) && hash(changed) != hash(revised),
      "all 32 SHA bytes remain in the stream");

  const auto captured = hash(changed);
  auto copied = changed;
  CpuTapeResourceObservation assigned;
  assigned = changed;
  receipts.assign(1u, receipt(buffer, 99u, 44u, 88u));
  input.identity.objectId = 999u;
  input.contentIdentity.generation = 99u;
  check(hash(changed) == captured && hash(copied) == captured && hash(assigned) == captured &&
      sameIdentity(changed.identity(), buffer),
      "later source mutation cannot change normalized owner or its independent copies");
  auto moved = std::move(changed);
  auto untouched = offset;
  check(!changed.valid() && !appendCpuTapeResourceObservation(changed, untouched) &&
      untouched == offset && hash(moved) == captured, "move transfers authority and invalid emission leaves hash untouched");
}

void aliasDepthAndEmpty() {
  auto staleParent = texture;
  ++staleParent.generation;
  const std::array receipts{receipt(texture, 0u, 1u, 1u),
      receipt(staleParent, 7u, 2u, 2u), receipt(texture, 7u, 3u, 3u),
      receipt(surface, 7u, 4u, 4u)};
  CpuTapeResourceObservationInput alias{.identity = surface, .contentIdentity = texture,
      .aliasSubresource = 7u, .receipts = receipts};
  CpuTapeResourceObservation observation;
  check(buildCpuTapeResourceObservation(alias, observation) == Status::Accepted &&
      sameIdentity(observation.identity(), surface) &&
      sameIdentity(observation.contentIdentity(), texture) && observation.aliasSubresource() == 7u &&
      observation.receipts().size() == 1u && observation.receipts()[0].revision == 3u &&
      hash(observation) == legacyHash(alias), "alias hashes its own full identity and only the exact parent's selected subresource");
  auto parentInput = alias;
  parentInput.identity = parentInput.contentIdentity;
  parentInput.aliasSubresource.reset();
  check(hash(observation) != legacyHash(parentInput), "alias identity cannot be replaced by parent identity");

  CpuTapeResourceObservationInput depth{.identity = surface, .contentIdentity = surface,
      .depthStencil = CpuTapeDepthStencilObservation{
          .disposition = static_cast<std::uint32_t>(dxmt9::d3d9::RenderTapeInitialContentDisposition::
              CompleteDepthStencilFloat32UInt8V1), .format = 75u, .width = 4u, .height = 3u,
          .depthPitch = 16u, .stencilPitch = 4u, .stencilOffset = 48u, .byteCount = 60u},
      .receipts = receipts};
  check(buildCpuTapeResourceObservation(depth, observation) == Status::Accepted &&
      observation.depthStencil() && hash(observation) == legacyHash(depth),
      "D24S8 encoding/format/dimensions and four layout fields preserve legacy widths and order");
  const auto depthHash = hash(observation);
  depth.depthStencil->width = 99u;
  check(hash(observation) == depthHash && observation.depthStencil()->width == 4u,
      "owned D24S8 metadata survives source mutation");

  const CpuTapeResourceObservationInput empty{.identity = texture, .contentIdentity = texture};
  check(buildCpuTapeResourceObservation(empty, observation, {.maxInputReceipts = 0u}) == Status::Accepted &&
      observation.receipts().empty() && !observation.depthStencil() &&
      legacyStream(empty).size() == 20u && hash(observation) == legacyHash(empty),
      "produced/no-seed resource emits identity and uint32 zero count with no presence bits");
}

void transactionalInvalidInputs() {
  std::array receipts{receipt(buffer, 0u, 4u, 1u), receipt(buffer, 1u, 5u, 2u)};
  const CpuTapeResourceObservationInput valid{.identity = buffer, .contentIdentity = buffer,
      .receipts = receipts};
  CpuTapeResourceObservation observation;
  check(buildCpuTapeResourceObservation(valid, observation, {.maxInputReceipts = 2u}) == Status::Accepted,
      "exact admitted receipt bound accepts");
  const auto retained = hash(observation);
  check(buildCpuTapeResourceObservation(valid, observation, {.maxInputReceipts = 1u}) == Status::CapacityExceeded &&
      hash(observation) == retained, "one-over input receipt bound preserves prior destination");
  check(buildCpuTapeResourceObservation(valid, observation,
      {.maxInputReceipts = std::numeric_limits<std::uint32_t>::max()}) == Status::CapacityExceeded &&
      hash(observation) == retained, "unsupported index budget rejects before allocation");
  auto input = valid;
  input.identity.generation = 0u;
  check(buildCpuTapeResourceObservation(input, observation) == Status::InvalidIdentity &&
      hash(observation) == retained, "zero generation cannot partially replace normalized identity");
  input = valid;
  ++input.contentIdentity.objectId;
  check(buildCpuTapeResourceObservation(input, observation) == Status::InvalidIdentity &&
      hash(observation) == retained, "non-alias content owner must match full bound identity");
  input = valid;
  input.aliasSubresource = 0u;
  input.contentIdentity = texture;
  check(buildCpuTapeResourceObservation(input, observation) == Status::InvalidIdentity &&
      hash(observation) == retained, "only a surface can carry a texture alias selection");
  receipts[1].revision = 0u;
  check(buildCpuTapeResourceObservation(valid, observation) == Status::InvalidReceipt &&
      hash(observation) == retained, "invalid receipt revision rejects without partial metadata");
  receipts[1].revision = 2u;
  input = valid;
  input.identity = input.contentIdentity = surface;
  input.depthStencil = CpuTapeDepthStencilObservation{};
  check(buildCpuTapeResourceObservation(input, observation) == Status::InvalidMetadata &&
      hash(observation) == retained, "invalid D24S8 layout leaves previous owner and hash behavior intact");
  CpuTapeResourceObservation invalid;
  auto seed = std::uint64_t{77u};
  check(!appendCpuTapeResourceObservation(invalid, seed) && seed == 77u,
      "unadmitted observation never writes a partial hash");
}

} // namespace

int main() {
  try {
    exactStreamAndOrdering();
    aliasDepthAndEmpty();
    transactionalInvalidInputs();
    std::cout << "cpu resource observation checks: " << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "cpu resource observation failure: " << error.what() << '\n';
    return 1;
  }
}
