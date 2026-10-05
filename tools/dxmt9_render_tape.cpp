#include "device_c_render_tape.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_identity_v3.hpp"
#include "device_c_render_tape_execution_topology_v1.hpp"
#include "device_c_render_tape_projection.hpp"
#include "device_c_render_tape_policy.hpp"
#include "device_c_render_tape_cpu_window.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using namespace dxmt9::d3d9;

std::vector<std::byte> readFile(const char* path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) throw std::runtime_error(std::string("cannot open tape: ") + path);
  const auto end = stream.tellg();
  if (end < 0) throw std::runtime_error(std::string("cannot size tape: ") + path);
  std::vector<std::byte> bytes(static_cast<std::size_t>(end));
  stream.seekg(0);
  if (!bytes.empty() &&
      !stream.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()))) {
    throw std::runtime_error(std::string("cannot read tape: ") + path);
  }
  return bytes;
}

std::uint32_t identityVersion(std::span<const std::byte> bytes) noexcept {
  constexpr auto offset = offsetof(RenderTapeIdentityHeader, version);
  std::uint32_t version = 0u;
  if (bytes.size() >= offset + sizeof(version))
    std::memcpy(&version, bytes.data() + offset, sizeof(version));
  return version;
}

bool requireV2Identity(std::span<const std::byte> bytes) {
  if (identityVersion(bytes) == kRenderTapeIdentityVersion) return true;
  if (identityVersion(bytes) == kRenderTapeIdentityV4Version) {
    std::cerr << "render tape identity v4 is inspection-only; V4 execution is not enabled\n";
    return false;
  }
  std::cerr << "render tape execution and policy transforms require identity v2; "
               "identity v3 is inspection-only\n";
  return false;
}

void writeFile(const char* path, std::span<const std::byte> bytes) {
  const std::filesystem::path output(path);
  const auto parent = output.has_parent_path() ? output.parent_path()
                                                : std::filesystem::path(".");
  const auto staging = parent /
      ("." + output.filename().string() + ".staging-" +
       std::to_string(static_cast<unsigned long long>(::getpid())));
  const int descriptor = ::open(staging.c_str(), O_WRONLY | O_CREAT | O_EXCL,
                                S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    throw std::runtime_error(std::string("cannot create tape staging: ") + path);
  }
  bool descriptorOpen = true;
  try {
    std::size_t offset = 0u;
    while (offset < bytes.size()) {
      const auto count = ::write(
          descriptor, bytes.data() + offset,
          std::min<std::size_t>(bytes.size() - offset,
                                static_cast<std::size_t>(
                                    std::numeric_limits<ssize_t>::max())));
      if (count <= 0) {
        throw std::runtime_error(std::string("cannot write tape: ") + path);
      }
      offset += static_cast<std::size_t>(count);
    }
    if (::fsync(descriptor) != 0) {
      throw std::runtime_error(std::string("cannot flush tape: ") + path);
    }
    const int closeStatus = ::close(descriptor);
    descriptorOpen = false;
    if (closeStatus != 0) {
      throw std::runtime_error(std::string("cannot close tape: ") + path);
    }
    if (::link(staging.c_str(), output.c_str()) != 0) {
      throw std::runtime_error(std::string("tape output already exists: ") + path);
    }
    (void)::unlink(staging.c_str());
    const int parentDescriptor = ::open(parent.c_str(), O_RDONLY);
    if (parentDescriptor >= 0) {
      (void)::fsync(parentDescriptor);
      (void)::close(parentDescriptor);
    }
  } catch (...) {
    if (descriptorOpen) (void)::close(descriptor);
    (void)::unlink(staging.c_str());
    throw;
  }
}

class InspectSink final : public RenderTapeReplaySink {
public:
  struct QueryDefinitionInspection {
    std::uint32_t event = 0u;
    D9CWireObjectIdentity identity{};
    RenderTapeQueryDescriptorDecoded descriptor{};
    RenderTapeQueryCycleState cycle{};
    bool versioned = false;
  };
  struct QueryIssueInspection {
    std::uint32_t event = 0u;
    std::uint32_t record = 0u;
    D9CWireObjectIdentity identity{};
    std::uint32_t flags = 0u;
    std::uint64_t issueCycle = 0u;
  };
  struct QueryPollInspection {
    std::uint32_t event = 0u;
    RenderTapeOrderedControlHeader header{};
    RenderTapeQueryGetDataControlV2 control{};
    bool versioned = false;
  };

  bool collectQueryInspection(const ImportedRenderTapeView& tape) {
    queryDefinitions.reserve(tape.events.size());
    queryPolls.reserve(tape.events.size());
    for (std::size_t index = 0u; index < tape.events.size(); ++index) {
      const auto event = tape.event(index);
      const auto ordinal = static_cast<std::uint32_t>(index + 1u);
      const auto type = static_cast<RenderTapeEventType>(event.header.type);
      if (type == RenderTapeEventType::ObjectDefine) {
        RenderTapeObjectDefineHeader fixed{};
        if (event.payload.size() < sizeof(fixed)) return false;
        std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
        if (fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_QUERY) continue;
        QueryDefinitionInspection row{.event = ordinal, .identity = fixed.identity};
        row.versioned = renderTapeLoadQueryDescriptor(
            event.payload.subspan(sizeof(fixed)), row.descriptor) &&
            renderTapeQueryDescriptorIdentityMatches(row.descriptor, fixed.identity);
        if (row.versioned) row.cycle = renderTapeInitialQueryCycle(row.descriptor);
        queryTranscriptComplete &= row.versioned;
        queryDefinitions.push_back(row);
      } else if (type == RenderTapeEventType::CommandChunk) {
        RenderTapeCommandChunkHeader fixed{};
        if (event.payload.size() < sizeof(fixed)) return false;
        std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
        ImportedChunkView chunk;
        if (!validateCommandChunk(event.payload.subspan(sizeof(fixed)),
            {.version = fixed.wireVersion, .recordCount = fixed.recordCount,
             .handleCount = fixed.handleCount}, &chunk).valid()) return false;
        for (std::size_t recordIndex = 0u; recordIndex < chunk.records.size(); ++recordIndex) {
          const auto record = chunk.record(recordIndex);
          if (record.header.type != D9C_COMMAND_RECORD_QUERY_ISSUE) continue;
          D9CCommandChunkWireQueryIssue issue{};
          if (record.payload.size() != sizeof(issue)) return false;
          std::memcpy(&issue, record.payload.data(), sizeof(issue));
          if (issue.queryHandleIndex >= chunk.handles.size()) return false;
          const auto& handle = chunk.handles[issue.queryHandleIndex];
          const D9CWireObjectIdentity identity{.kind = handle.kind,
              .generation = handle.generation, .objectId = handle.objectId};
          auto definition = std::find_if(queryDefinitions.begin(), queryDefinitions.end(),
              [&](const auto& row) { return row.identity.kind == identity.kind &&
                  row.identity.generation == identity.generation &&
                  row.identity.objectId == identity.objectId; });
          if (definition == queryDefinitions.end()) return false;
          if (definition->versioned &&
              !renderTapeAdvanceQueryCycle(definition->cycle, issue.flags, true)) return false;
          queryIssues.push_back({.event = ordinal,
              .record = static_cast<std::uint32_t>(recordIndex), .identity = identity,
              .flags = issue.flags, .issueCycle = definition->cycle.issueCycle});
        }
      } else if (type == RenderTapeEventType::OrderedControl) {
        RenderTapeOrderedControlHeader fixed{};
        if (event.payload.size() < sizeof(fixed)) return false;
        std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
        if (fixed.kind != static_cast<std::uint32_t>(RenderTapeControlKind::QueryGetData)) continue;
        QueryPollInspection row{.event = ordinal, .header = fixed};
        row.versioned = event.payload.size() == sizeof(fixed) + sizeof(row.control);
        if (row.versioned) {
          std::memcpy(&row.control, event.payload.data() + sizeof(fixed), sizeof(row.control));
          row.versioned = row.control.schemaVersion == kRenderTapeQueryTranscriptVersion2;
          const auto definition = std::find_if(queryDefinitions.begin(), queryDefinitions.end(),
              [&](const auto& value) {
                return value.identity.kind == fixed.identity.kind &&
                    value.identity.generation == fixed.identity.generation &&
                    value.identity.objectId == fixed.identity.objectId;
              });
          row.versioned &= definition != queryDefinitions.end() && definition->versioned &&
              renderTapeQueryPollCycleAdmitted(definition->cycle, row.control.issueCycle,
                                              fixed.resultCode);
        }
        queryTranscriptComplete &= row.versioned;
        queryPolls.push_back(row);
      }
    }
    return true;
  }

  struct BufferDefinitionInspection {
    std::uint32_t event = 0u;
    D9CWireObjectIdentity identity{};
    D9CBufferDesc descriptor{};
  };

  struct BufferMutationInspection {
    std::uint32_t event = 0u;
    D9CWireObjectIdentity identity{};
    std::uint32_t mutationKind = 0u;
    std::uint32_t disposition = 0u;
    std::uint64_t byteOffset = 0u;
    std::uint64_t byteSize = 0u;
  };

  struct BufferDestroyInspection {
    std::uint32_t event = 0u;
    D9CWireObjectIdentity identity{};
  };

  bool collectBufferInspection(const ImportedRenderTapeView& tape) {
    const auto eventCount = tape.events.size();
    bufferDefinitions.reserve(eventCount);
    bufferMutations.reserve(eventCount);
    bufferDestroys.reserve(eventCount);
    for (std::size_t index = 0u; index < eventCount; ++index) {
      const auto event = tape.event(index);
      const auto eventOrdinal = static_cast<std::uint32_t>(index + 1u);
      const auto type = static_cast<RenderTapeEventType>(event.header.type);
      if (type == RenderTapeEventType::ObjectDefine) {
        if (event.payload.size() < sizeof(RenderTapeObjectDefineHeader)) {
          return false;
        }
        RenderTapeObjectDefineHeader fixed{};
        std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
        if (fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER) continue;
        if (fixed.descriptorKind !=
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer) ||
            fixed.descriptorBytes != sizeof(D9CBufferDesc) ||
            event.payload.size() != sizeof(fixed) + sizeof(D9CBufferDesc) ||
            bufferDefinitions.size() == eventCount) {
          return false;
        }
        D9CBufferDesc descriptor{};
        std::memcpy(&descriptor,
                    event.payload.data() + sizeof(fixed),
                    sizeof(descriptor));
        bufferDefinitions.push_back(BufferDefinitionInspection{
            .event = eventOrdinal,
            .identity = fixed.identity,
            .descriptor = descriptor});
      } else if (type == RenderTapeEventType::ResourceMutation) {
        if (event.payload.size() != sizeof(RenderTapeResourceMutationHeader)) {
          return false;
        }
        RenderTapeResourceMutationHeader fixed{};
        std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
        if (fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER) continue;
        if (bufferMutations.size() == eventCount) return false;
        bufferMutations.push_back(BufferMutationInspection{
            .event = eventOrdinal,
            .identity = fixed.identity,
            .mutationKind = fixed.kind,
            .disposition = fixed.bufferDisposition,
            .byteOffset = fixed.byteOffset,
            .byteSize = fixed.byteSize});
      } else if (type == RenderTapeEventType::ObjectDestroy) {
        if (event.payload.size() != sizeof(RenderTapeObjectDestroyHeader)) {
          return false;
        }
        RenderTapeObjectDestroyHeader fixed{};
        std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
        if (fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER) continue;
        if (bufferDestroys.size() == eventCount) return false;
        bufferDestroys.push_back(BufferDestroyInspection{
            .event = eventOrdinal, .identity = fixed.identity});
      }
    }
    return true;
  }

  bool bootstrapStateOnly = true;
  bool bootstrap(const RenderTapeBootstrapHeader& fixed, std::span<const std::byte> bytes,
                 RenderTapeBootstrapReplayMode mode) override {
    if (mode != RenderTapeBootstrapReplayMode::JournalOnlyDeferredProvider) return false;
    std::size_t offset = 0u;
    for (std::uint32_t index = 0u; index < fixed.overlayCount; ++index) {
      D9CCommandChunkWireHeader header{};
      if (offset > bytes.size() || bytes.size() - offset < sizeof(header)) return false;
      std::memcpy(&header, bytes.data() + offset, sizeof(header));
      if (header.payloadArenaOffset > bytes.size() - offset ||
          header.payloadArenaSize > bytes.size() - offset - header.payloadArenaOffset) return false;
      const auto size = std::size_t(header.payloadArenaOffset) + header.payloadArenaSize;
      ImportedChunkView chunk;
      if (!validateCommandChunk(bytes.subspan(offset, size),
              {.version = D9C_COMMAND_CHUNK_WIRE_VERSION, .recordCount = header.recordCount,
               .handleCount = header.handleCount}, &chunk).valid()) return false;
      for (const auto& record : chunk.records)
        bootstrapStateOnly &= record.type == D9C_COMMAND_RECORD_APPLY_STATE ||
            record.type == D9C_COMMAND_RECORD_SET_VS_CONST_F ||
            record.type == D9C_COMMAND_RECORD_SET_PS_CONST_F;
      offset += size;
    }
    if (fixed.gammaRampBytes > bytes.size() || offset != bytes.size() - fixed.gammaRampBytes)
      return false;
    ++bootstraps;
    ++events;
    return true;
  }
  bool objectDefine(const RenderTapeObjectDefineHeader& fixed,
                    std::span<const std::byte> descriptor) override {
    if (fixed.descriptorKind < descriptorKinds.size()) {
      ++descriptorKinds[fixed.descriptorKind];
    }
    if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER &&
        fixed.descriptorKind == static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer) &&
        descriptor.size() == sizeof(D9CBufferDesc)) {
      D9CBufferDesc buffer{};
      std::memcpy(&buffer, descriptor.data(), sizeof(buffer));
      dynamicBuffers += (buffer.usage & 0x200u) != 0u;
    }
    if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
      RenderTapeTextureDescriptorV2 texture{};
      if (!renderTapeLoadTextureDescriptorV2(descriptor, texture)) {
        return false;
      }
      ++textureDescriptorsV2;
      if (texture.dimension < textureDimensions.size()) {
        ++textureDimensions[texture.dimension];
      }
      if (texture.initialContentDisposition < contentDispositions.size()) {
        ++contentDispositions[texture.initialContentDisposition];
      }
    } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
      RenderTapeSurfaceDescriptorV2 surface{};
      if (!renderTapeLoadSurfaceDescriptorV2(descriptor, surface)) return false;
      ++surfaceDescriptorsV2;
      if (surface.storage < surfaceStorages.size()) {
        ++surfaceStorages[surface.storage];
      }
      if (surface.initialContentDisposition < contentDispositions.size()) {
        ++contentDispositions[surface.initialContentDisposition];
      }
    }
    ++defines;
    ++events;
    return true;
  }
  bool resourceMutation(const RenderTapeResourceMutationHeader& mutation) override {
    if (mutation.kind < mutationKinds.size()) {
      ++mutationKinds[mutation.kind];
    }
    if (mutation.identity.kind < mutationObjectKinds.size()) {
      ++mutationObjectKinds[mutation.identity.kind];
    }
    ++mutations;
    mutationBytes += mutation.byteSize;
    ++events;
    return true;
  }
  bool commandChunk(const CommandChunkEnvelope& envelope,
                    std::span<const std::byte> bytes) override {
    ImportedChunkView chunk;
    if (!validateCommandChunk(bytes, envelope, &chunk).valid()) return false;
    ++chunks;
    records += chunk.records.size();
    handles += chunk.handles.size();
    for (std::size_t recordIndex = 0u; recordIndex < chunk.records.size();
         ++recordIndex) {
      const auto record = chunk.record(recordIndex);
      if (record.header.type < recordTypes.size()) {
        ++recordTypes[record.header.type];
      }
      for (const auto& section : record.sections) {
        if (section.kind < sectionKinds.size()) {
          ++sectionKinds[section.kind];
        }
      }
    }
    ++events;
    return true;
  }
  bool objectDestroy(const RenderTapeObjectDestroyHeader& fixed) override {
    if (fixed.identity.kind < destroyObjectKinds.size()) {
      ++destroyObjectKinds[fixed.identity.kind];
    }
    ++destroys;
    ++events;
    return true;
  }
  bool orderedControl(const RenderTapeOrderedControlHeader& fixed,
                      std::span<const std::byte>) override {
    if (fixed.kind < controlKinds.size()) {
      ++controlKinds[fixed.kind];
    }
    ++controls;
    ++events;
    return true;
  }
  bool presentComplete(const RenderTapePresentCompleteHeader& complete,
                       std::span<const std::byte>) override {
    ++completions;
    lastPresentOrdinal = complete.presentOrdinal;
    lastDigestValidity = complete.digestValidity;
    lastExpectedDigest = complete.expectedDigest;
    ++events;
    return true;
  }

  std::uint64_t events = 0u;
  std::uint64_t dynamicBuffers = 0u;
  std::uint64_t bootstraps = 0u;
  std::uint64_t defines = 0u;
  std::uint64_t textureDescriptorsV2 = 0u;
  std::uint64_t surfaceDescriptorsV2 = 0u;
  std::uint64_t mutations = 0u;
  std::uint64_t mutationBytes = 0u;
  std::uint64_t chunks = 0u;
  std::uint64_t records = 0u;
  std::uint64_t handles = 0u;
  std::uint64_t destroys = 0u;
  std::uint64_t controls = 0u;
  std::uint64_t completions = 0u;
  std::uint64_t lastPresentOrdinal = 0u;
  std::uint32_t lastDigestValidity = 0u;
  RenderTapeDigest lastExpectedDigest{};
  std::array<std::uint64_t, 7u> descriptorKinds{};
  std::array<std::uint64_t, 4u> textureDimensions{};
  std::array<std::uint64_t, 7u> contentDispositions{};
  std::array<std::uint64_t, 4u> surfaceStorages{};
  std::array<std::uint64_t, 5u> mutationKinds{};
  std::array<std::uint64_t, 6u> mutationObjectKinds{};
  std::array<std::uint64_t, 6u> destroyObjectKinds{};
  std::array<std::uint64_t, 6u> controlKinds{};
  std::array<std::uint64_t, D9C_COMMAND_RECORD_GENERATE_MIPMAPS + 1u> recordTypes{};
  std::array<std::uint64_t, D9C_COMMAND_CHUNK_SECTION_COUNT + 1u>
      sectionKinds{};
  std::vector<BufferDefinitionInspection> bufferDefinitions{};
  std::vector<BufferMutationInspection> bufferMutations{};
  std::vector<BufferDestroyInspection> bufferDestroys{};
  bool queryTranscriptComplete = true;
  std::vector<QueryDefinitionInspection> queryDefinitions{};
  std::vector<QueryIssueInspection> queryIssues{};
  std::vector<QueryPollInspection> queryPolls{};
};

template <std::size_t N>
void writeTypedCounts(std::ostream& output,
                      const std::array<std::uint64_t, N>& counts,
                      const std::array<const char*, N>& names) {
  output << '{';
  bool first = true;
  for (std::size_t i = 0u; i < counts.size(); ++i) {
    if (counts[i] == 0u || names[i] == nullptr) continue;
    if (!first) output << ',';
    first = false;
    output << '"' << names[i] << "\":" << counts[i];
  }
  output << '}';
}

constexpr std::array<const char*, 7u> kDescriptorKindNames{
    nullptr, "texture", "surface", "buffer", "shader",
    "vertex_declaration", "query"};
constexpr std::array<const char*, 4u> kTextureDimensionNames{
    nullptr, "texture_2d", "cube", "volume"};
constexpr std::array<const char*, 7u> kContentDispositionNames{
    nullptr, "complete_seed", "unavailable", "produced_present_output",
    "produced_by_captured_pass", "complete_depth_float32_v1",
    "complete_depth_stencil_float32_uint8_v1"};
constexpr std::array<const char*, 4u> kSurfaceStorageNames{
    nullptr, "standalone", "texture_subresource", "swapchain_backbuffer"};
constexpr std::array<const char*, 5u> kMutationKindNames{
    nullptr, "cpu_unlock", "upload", "palette", "mipmap_class"};
constexpr std::array<const char*, 6u> kObjectKindNames{
    "texture", "surface", "buffer", "shader", "vertex_declaration",
    "query"};
constexpr std::array<const char*, 6u> kControlKindNames{
    nullptr, "query_get_data", "cpu_read", "flush_wait", "reset",
    "device_lost"};
constexpr std::array<const char*, D9C_COMMAND_RECORD_GENERATE_MIPMAPS + 1u> kRecordTypeNames{
    nullptr,
    "draw_primitive", "draw_indexed_primitive", "draw_primitive_up",
    "draw_indexed_primitive_up",
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr,
    "set_vs_const_f", "set_vs_const_i", "set_vs_const_b",
    "set_ps_const_f", "set_ps_const_i", "set_ps_const_b",
    "clear", "present", "stretch_rect", "color_fill", "update_texture",
    "update_surface", "query_issue", "readback", "apply_state",
    "resz_depth_resolve", "generate_mipmaps"};
constexpr std::array<const char*, D9C_COMMAND_CHUNK_SECTION_COUNT + 1u>
    kSectionKindNames{
        nullptr,
        "render_state", "texture", "stream", "shader", "vertex_input",
        "index_buffer", "render_target", "depth_stencil", "viewport",
        "scissor", "material", "clip_plane", "texture_stage_state",
        "sampler_state", "transform", "light", "light_enable",
        "vs_const_f", "vs_const_i", "vs_const_b", "ps_const_f",
        "ps_const_i", "ps_const_b", "up_index_data", "up_vertex_data"};

void usage() {
  std::cerr << "usage: dxmt9-render-tape <validate|inspect> <tape.bin> "
               "[--verified-blob <sha256>:<bytes>]...\n"
               "       dxmt9-render-tape identity <tape.bin> <identity.bin> "
               "[--execution-topology <execution_topology.bin>] "
               "[--verified-blob <sha256>:<bytes>]...\n"
               "       dxmt9-render-tape policy-explore <tape.bin> "
               "<identity.bin> [--verified-blob <sha256>:<bytes>]...\n"
               "       dxmt9-render-tape reduce <input> <output> "
               "--select-command-event <index>... "
               "[--verified-blob <sha256>:<bytes>]...\n"
               "       dxmt9-render-tape project <tape.bin> "
               "--command-event-ordinal <ordinal> --first-record <index> "
               "--record-count <count> "
               "[--verified-blob <sha256>:<bytes>]...\n"
               "       dxmt9-render-tape materialize <tape.bin> "
               "<identity.bin> <output.bin> --command-event-ordinal <ordinal> "
               "--first-record <index> --record-count <count> "
               "[--output-identity <identity-v2.bin>] "
               "[--output-sha256 <digest>] "
               "[--verified-blob <sha256>:<bytes>]...\n";
}

bool decodeDigest(std::string_view text, RenderTapeDigest& digest) {
  if (text.size() != digest.size() * 2u) return false;
  auto nibble = [](char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0u; i < digest.size(); ++i) {
    const auto high = nibble(text[i * 2u]);
    const auto low = nibble(text[i * 2u + 1u]);
    if (high < 0 || low < 0) return false;
    digest[i] = static_cast<std::byte>((high << 4) | low);
  }
  return true;
}

std::string encodeDigest(const RenderTapeDigest& digest) {
  constexpr char digits[] = "0123456789abcdef";
  std::string text;
  text.reserve(digest.size() * 2u);
  for (const auto byte : digest) {
    const auto value = static_cast<unsigned>(byte);
    text.push_back(digits[(value >> 4u) & 0xfu]);
    text.push_back(digits[value & 0xfu]);
  }
  return text;
}

std::string encodeQueryBytes(std::span<const std::byte> bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (const auto value : bytes) {
    const auto byte = std::to_integer<unsigned>(value);
    result.push_back(digits[byte >> 4u]);
    result.push_back(digits[byte & 0xfu]);
  }
  return result;
}

void appendVerifiedBlob(RenderTapeBlobCatalogue& catalogue,
                        std::string_view value) {
  const auto separator = value.find(':');
  RenderTapeBlob blob{};
  if (separator == std::string_view::npos ||
      !decodeDigest(value.substr(0u, separator), blob.digest)) {
    throw std::invalid_argument("invalid verified blob reference");
  }
  const auto sizeText = std::string(value.substr(separator + 1u));
  std::size_t parsed = 0u;
  blob.size = std::stoull(sizeText, &parsed);
  if (parsed != sizeText.size()) {
    throw std::invalid_argument("invalid verified blob byte count");
  }
  blob.verified = 1u;
  catalogue.blobs.push_back(blob);
}

std::uint64_t parseUnsigned(std::string_view value, const char* description) {
  if (value.empty() ||
      std::any_of(value.begin(), value.end(), [](char character) {
        return character < '0' || character > '9';
      })) {
    throw std::invalid_argument(std::string("invalid ") + description);
  }
  const auto text = std::string(value);
  std::size_t parsed = 0u;
  const auto number = std::stoull(text, &parsed);
  if (parsed != text.size()) {
    throw std::invalid_argument(std::string("invalid ") + description);
  }
  return number;
}

std::uint32_t parseU32(std::string_view value, const char* description) {
  const auto number = parseUnsigned(value, description);
  if (number > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument(std::string("invalid ") + description);
  }
  return static_cast<std::uint32_t>(number);
}

void writeIdentity(std::ostream& output,
                   const D9CWireObjectIdentity& identity) {
  output << "{\"kind\":" << identity.kind
         << ",\"object_id\":" << identity.objectId
         << ",\"generation\":" << identity.generation << '}';
}

void writeInspectionIdentity(std::ostream& output,
                             const D9CWireObjectIdentity& identity) {
  output << "{\"kind\":" << identity.kind
         << ",\"generation\":" << identity.generation
         << ",\"object_id\":\"" << identity.objectId << "\"}";
}

void writeLocator(std::ostream& output,
                  const RenderTapeProjectionLocator& locator) {
  output << "{\"event_ordinal\":" << locator.eventOrdinal
         << ",\"source_event_index\":" << locator.sourceEventIndex
         << ",\"record_index\":" << locator.recordIndex
         << ",\"record_type\":" << locator.recordType << '}';
}

void writeProjection(const RenderTapeProjectionResult& projection) {
  std::cout << "{\"schema\":\"" << kRenderTapeProjectionSchema
            << "\",\"status\":\"projection-ready\",\"ready\":true,"
               "\"scope\":{\"profile\":\"frame-tape\","
               "\"claim\":\"structural-projection-readiness-only\","
               "\"wire_bytes_rewritten\":false,"
               "\"legacy_mini_replay_manifest\":false,"
               "\"provider_replay\":false,\"gt2_replay\":false},"
               "\"source\":{\"schema\":\"dxmt9.render_tape.v2\","
               "\"sha256\":\""
            << encodeDigest(projection.sourceDigest) << "\",\"bytes\":"
            << projection.sourceBytes << ",\"events\":"
            << projection.sourceEventCount << ",\"records\":"
            << projection.sourceRecordCount << "},\"selector\":{"
               "\"command_event_ordinal\":"
            << projection.selector.commandEventOrdinal
            << ",\"first_record_index\":"
            << projection.selector.firstRecordIndex
            << ",\"record_count\":" << projection.selector.recordCount
            << "},\"bootstrap\":{\"derived\":true,"
               "\"selected_record_full_snapshot\":"
            << (projection.stateFold.selectedRecordWasFullSnapshot
                    ? "true" : "false")
            << ",\"coverage_mask\":" << projection.stateFold.coverageMask
            << "},\"boundaries\":{\"clear\":";
  writeLocator(std::cout, projection.clearLocator);
  std::cout << ",\"present\":";
  writeLocator(std::cout, projection.presentLocator);
  std::cout << "},\"conservation\":{\"selected_draws\":"
            << projection.selectedDrawCount << ",\"excluded_records\":"
            << projection.excludedRecordCount << ",\"objects\":"
            << projection.objects.size() << ",\"blob_references\":"
            << projection.blobReferences.size()
            << ",\"excluded_coordinator_events\":"
            << projection.excludedEvents.size() << "},\"draws\":[";
  for (std::size_t i = 0u; i < projection.selectedLocators.size(); ++i) {
    if (i != 0u) std::cout << ',';
    writeLocator(std::cout, projection.selectedLocators[i]);
  }
  std::cout << "],\"objects\":[";
  for (std::size_t i = 0u; i < projection.objects.size(); ++i) {
    if (i != 0u) std::cout << ',';
    const auto& object = projection.objects[i];
    std::cout << "{\"identity\":";
    writeIdentity(std::cout, object.identity);
    std::cout << ",\"descriptor_kind\":" << object.descriptorKind
              << ",\"descriptor_bytes\":" << object.descriptorBytes
              << ",\"definition\":{\"event_ordinal\":"
              << object.definitionEventOrdinal
              << ",\"source_event_index\":" << object.definitionEventIndex
              << "},\"immutable_payload_bytes\":"
              << object.immutablePayloadBytes
              << ",\"expected_initial_content_bytes\":"
              << object.expectedContentBytes
              << ",\"expected_initial_content_count\":"
              << object.expectedContentCount
              << ",\"closed_initial_content_bytes\":"
              << object.initialContentBytes
              << ",\"closed_initial_content_count\":"
              << object.initialContentCount << '}';
  }
  std::cout << "],\"blob_references\":[";
  for (std::size_t i = 0u; i < projection.blobReferences.size(); ++i) {
    if (i != 0u) std::cout << ',';
    const auto& reference = projection.blobReferences[i];
    std::cout << "{\"kind\":\""
              << renderTapeProjectionBlobKindName(reference.kind)
              << "\",\"identity\":";
    writeIdentity(std::cout, reference.identity);
    std::cout << ",\"digest\":\"" << encodeDigest(reference.digest)
              << "\",\"bytes\":" << reference.size
              << ",\"source_event_ordinal\":"
              << reference.sourceEventOrdinal
              << ",\"source_event_index\":" << reference.sourceEventIndex
              << ",\"mutation_kind\":" << reference.mutationKind
              << ",\"subresource\":" << reference.subresource
              << ",\"byte_offset\":" << reference.byteOffset
              << ",\"initial_content\":"
              << (reference.initialContent != 0u ? "true" : "false") << '}';
  }
  std::cout << "],\"excluded_coordinator_events\":[";
  for (std::size_t i = 0u; i < projection.excludedEvents.size(); ++i) {
    if (i != 0u) std::cout << ',';
    const auto& excluded = projection.excludedEvents[i];
    std::cout << "{\"kind\":\""
              << renderTapeProjectionExcludedKindName(excluded.kind)
              << "\",\"event_ordinal\":" << excluded.eventOrdinal
              << ",\"source_event_index\":" << excluded.sourceEventIndex
              << ",\"event_type\":" << excluded.eventType << '}';
  }
  std::cout << "]}\n";
}

void writePolicyExplore(const RenderTapePolicyExploreResult& result) {
  std::cout << "{\"schema\":\"dxmt9.render_tape.parallel_policy.v1\","
               "\"status\":\""
            << renderTapePolicyExploreStatusName(result.status)
            << "\",\"authenticated_input\":"
            << (result.authenticatedInput ? "true" : "false")
            << ",\"structural_only\":"
            << (result.structuralOnly ? "true" : "false")
            << ",\"proof_core_validated\":"
            << (result.proofCoreValidated ? "true" : "false")
            << ",\"candidates\":[";
  for (std::size_t index = 0u; index < result.candidates.size(); ++index) {
    if (index != 0u) std::cout << ',';
    const auto& candidate = result.candidates[index];
    std::cout << "{\"event_ordinal\":" << candidate.eventOrdinal
              << ",\"source_ordinal\":" << candidate.sourceOrdinal
              << ",\"seq_id\":" << candidate.seqId
              << ",\"logical_pass_id\":" << candidate.logicalPassId
              << ",\"dag_pass_index\":" << candidate.dagPassIndex
              << ",\"pass_kind\":" << candidate.passKind
              << ",\"first_record\":" << candidate.firstRecord
              << ",\"record_count\":" << candidate.recordCount
              << ",\"child_count\":" << candidate.children.size()
              << ",\"draw_total\":" << candidate.drawTotal
              << ",\"primitive_total\":" << candidate.primitiveTotal
              << ",\"min_child_draws\":" << candidate.minChildDraws
              << ",\"max_child_draws\":" << candidate.maxChildDraws
              << ",\"imbalance\":" << candidate.imbalance
              << ",\"vertex_shader_facts\":" << candidate.vertexShaderFacts
              << ",\"pixel_shader_facts\":" << candidate.pixelShaderFacts
              << ",\"vertex_input_facts\":" << candidate.vertexInputFacts
              << ",\"uniform_section_facts\":"
              << candidate.uniformSectionFacts
              << ",\"pipeline_input_section_facts\":"
              << candidate.pipelineInputSectionFacts
              << ",\"structural_only\":"
              << (candidate.structuralOnly ? "true" : "false")
              << ",\"proof_core_validated\":"
              << (candidate.proofCoreValidated ? "true" : "false")
              << ",\"children\":[";
    for (std::size_t child = 0u; child < candidate.children.size(); ++child) {
      if (child != 0u) std::cout << ',';
      const auto& range = candidate.children[child];
      std::cout << "{\"first_record\":" << range.firstRecord
                << ",\"record_count\":" << range.recordCount
                << ",\"draw_count\":" << range.drawCount
                << ",\"primitive_total\":" << range.primitiveTotal << '}';
    }
    std::cout << "]}";
  }
  std::cout << "],\"rejections\":[";
  for (std::size_t index = 0u; index < result.rejections.size(); ++index) {
    if (index != 0u) std::cout << ',';
    const auto& rejection = result.rejections[index];
    std::cout << "{\"event_ordinal\":" << rejection.eventOrdinal
              << ",\"source_ordinal\":" << rejection.sourceOrdinal
              << ",\"seq_id\":" << rejection.seqId
              << ",\"logical_pass_id\":" << rejection.logicalPassId
              << ",\"first_record\":" << rejection.firstRecord
              << ",\"record_count\":" << rejection.recordCount
              << ",\"reason\":\""
              << renderTapePolicyRejectionReasonName(rejection.reason)
              << "\"}";
  }
  std::cout << "]}\n";
}

// This writer is deliberately local to v3/v4 inspection. In particular, it must
// not change v2's existing numeric JSON contract. Full-width wire integers
// use decimal strings so ordinary JSON clients cannot round evidence IDs.
class IdentityV3JsonObject {
public:
  explicit IdentityV3JsonObject(std::ostream& output) : output_(output) {
    output_ << '{';
  }
  ~IdentityV3JsonObject() { output_ << '}'; }

  void field(const char* name, std::uint64_t value) {
    key(name);
    output_ << '"' << value << '"';
  }
  void field(const char* name, std::uint32_t value) {
    key(name);
    output_ << value;
  }
  void field(const char* name, std::int32_t value) {
    key(name);
    output_ << value;
  }
  void field(const char* name, std::string_view value) {
    key(name);
    // Values are fixed schema/phase strings or a lowercase SHA-256 digest.
    output_ << '"' << value << '"';
  }
  void field(const char* name, const RenderTapeIdentityV3Table& value) {
    key(name);
    IdentityV3JsonObject table(output_);
    table.field("offset", value.offset);
    table.field("count", value.count);
    table.field("entry_size", value.entrySize);
  }
  void field(const char* name, const D9CWireObjectIdentity& value) {
    key(name);
    writeInspectionIdentity(output_, value);
  }
  void field(const char* name, const RenderTapeIdentityV4ReplaySourceIdentity& value) {
    key(name);
    IdentityV3JsonObject object(output_);
    object.field("frame", value.frame);
    object.field("source", value.source);
    object.field("last_source", value.lastSource);
    object.field("sequence", value.sequence);
  }
  void field(const char* name, const RenderTapeIdentityV4ReplayReceipt& value) {
    key(name);
    IdentityV3JsonObject object(output_);
    object.field("identity", value.identity);
    object.field("queue_sequence", value.queueSequence);
    object.field("build_generation", value.buildGeneration);
    object.field("source_generation", value.sourceGeneration);
    object.field("storage_generation", value.storageGeneration);
    object.field("kind", value.kind);
    object.field("control_index", value.controlIndex);
    object.field("command_count", value.commandCount);
    object.field("reserved0", value.reserved0);
  }

private:
  void key(const char* name) {
    if (!first_) output_ << ',';
    first_ = false;
    output_ << '"' << name << "\":";
  }
  std::ostream& output_;
  bool first_ = true;
};

const char* identityV3PhaseName(RenderTapeIdentityV3SourcePhase phase) noexcept {
  switch (phase) {
  case RenderTapeIdentityV3SourcePhase::Published: return "published";
  case RenderTapeIdentityV3SourcePhase::Submitted: return "submitted";
  case RenderTapeIdentityV3SourcePhase::Completed: return "completed";
  case RenderTapeIdentityV3SourcePhase::Invalid: return "invalid";
  }
  return "invalid";
}

template <typename Row, typename Write>
void writeIdentityV3Rows(std::ostream& output, const char* name,
                         const std::vector<Row>& rows, Write write) {
  output << ",\"" << name << "\":[";
  for (std::size_t index = 0u; index < rows.size(); ++index) {
    if (index != 0u) output << ',';
    IdentityV3JsonObject object(output);
    write(object, rows[index]);
  }
  output << ']';
}

void writeIdentitySourceFields(IdentityV3JsonObject& object,
                               const RenderTapeIdentityV3Source& row) {
  object.field("source_ordinal", row.sourceOrdinal);
  object.field("seq_id", row.seqId);
  object.field("writer_capture_id", row.writerCaptureId);
  object.field("writer_generation", row.writerGeneration);
  object.field("storage_generation", row.storageGeneration);
  object.field("publication_edge", row.publicationEdge);
  object.field("submission_edge", row.submissionEdge);
  object.field("completion_edge", row.completionEdge);
  object.field("completed_sequence", row.completedSequence);
  object.field("cut_event_ordinal", row.cutEventOrdinal);
  object.field("command_count", row.commandCount);
  object.field("cut_kind", row.cutKind);
  object.field("cut_record", row.cutRecord);
  object.field("reserved0", row.reserved0);
  object.field("phase", identityV3PhaseName(renderTapeIdentityV3SourcePhase(row)));
}

void writeIdentityV4SharedRows(std::ostream& output, const RenderTapeIdentityV4View& view) {
  writeIdentityV3Rows(output, "initializers", view.initializers,
      [](IdentityV3JsonObject& object, const RenderTapeIdentityV3Initializer& row) {
        object.field("sequence", row.sequence);
        object.field("epoch", row.epoch);
        object.field("publication_edge", row.publicationEdge);
        object.field("submission_edge", row.submissionEdge);
        object.field("completion_edge", row.completionEdge);
        object.field("completed_sequence", row.completedSequence);
        object.field("cut_event_ordinal", row.cutEventOrdinal);
        object.field("cut_kind", row.cutKind);
        object.field("cut_record", row.cutRecord);
        object.field("reserved0", row.reserved0);
        object.field("reserved1", row.reserved1);
        object.field("phase", identityV3PhaseName(renderTapeIdentityV3InitializerPhase(row)));
      });
  writeIdentityV3Rows(output, "dispositions", view.dispositions,
      [](IdentityV3JsonObject& object, const RenderTapeIdentityV3Disposition& row) {
        object.field("event_ordinal", row.eventOrdinal);
        object.field("first_record", row.firstRecord);
        object.field("record_count", row.recordCount);
        object.field("kind", row.kind);
        object.field("command_index", row.commandIndex);
        object.field("first_subdraw", row.firstSubdraw);
        object.field("subdraw_count", row.subdrawCount);
        object.field("coordinator_index", row.coordinatorIndex);
        object.field("reserved0", row.reserved0);
      });
  writeIdentityV3Rows(output, "coordinators", view.coordinators,
      [&](IdentityV3JsonObject& object, const RenderTapeIdentityV4Coordinator& row) {
        object.field("admission_edge", row.admissionEdge);
        object.field("authority", row.authority);
        object.field("reserved0", row.reserved0);
        output << ",\"provenance\":";
        IdentityV3JsonObject provenance(output);
        const auto& p = row.provenance;
        provenance.field("event_ordinal", p.eventOrdinal);
        provenance.field("raw_ordinal", p.rawOrdinal);
        provenance.field("build_generation", p.buildGeneration);
        provenance.field("dispatch_edge", p.dispatchEdge);
        provenance.field("release_ordinal", p.releaseOrdinal);
        provenance.field("release_generation", p.releaseGeneration);
        provenance.field("query_identity", p.queryIdentity);
        provenance.field("issue_cycle", p.issueCycle);
        provenance.field("issue_ordinal", p.issueOrdinal);
        provenance.field("device_epoch", p.deviceEpoch);
        provenance.field("writer_capture_id", p.writerCaptureId);
        provenance.field("writer_generation", p.writerGeneration);
        provenance.field("storage_generation", p.storageGeneration);
        provenance.field("prefix_sequence", p.prefixSequence);
        provenance.field("prefix_initializer", p.prefixInitializer);
        provenance.field("prefix_initializer_epoch", p.prefixInitializerEpoch);
        provenance.field("record_index", p.recordIndex);
        provenance.field("record_type", p.recordType);
        provenance.field("flags", p.flags);
        provenance.field("query_type", p.queryType);
        provenance.field("dispatch_result", p.dispatchResult);
        provenance.field("release_action", p.releaseAction);
        provenance.field("release_writer", p.releaseWriter);
        provenance.field("prefix_kind", p.prefixKind);
        provenance.field("source_index", p.sourceIndex);
        provenance.field("initializer_index", p.initializerIndex);
        provenance.field("command_end", p.commandEnd);
        provenance.field("observation", p.observation);
        provenance.field("reserved0", p.reserved0);
        provenance.field("reserved1", p.reserved1);
      });
}

void writeIdentityV3(const RenderTapeIdentityV3View& view) {
  auto& output = std::cout;
  output << "{\"schema\":\"" << kRenderTapeIdentityV3Schema
         << "\",\"valid\":true,\"inspection_only\":true,"
            "\"execution_supported\":false,\"production_qualified\":false,"
            "\"header\":";
  {
    IdentityV3JsonObject object(output);
    const auto& h = view.header;
    object.field("magic", h.magic);
    object.field("events_bytes", h.eventsBytes);
    object.field("frame_id", h.frameId);
    object.field("present_ordinal", h.presentOrdinal);
    object.field("capture_token", h.captureToken);
    object.field("events_digest", encodeDigest(h.eventsDigest));
    object.field("version", h.version);
    object.field("header_size", h.headerSize);
    object.field("capture_epoch", h.captureEpoch);
    object.field("queue", h.queue);
    object.field("baseline_sequence", h.baselineSequence);
    object.field("baseline_initializer", h.baselineInitializer);
    object.field("baseline_completed_sequence", h.baselineCompletedSequence);
    object.field("initializer_epoch", h.initializerEpoch);
    object.field("baseline_completed_initializer", h.baselineCompletedInitializer);
    object.field("baseline_completion_edge", h.baselineCompletionEdge);
    object.field("baseline_kind", h.baselineKind);
    object.field("authority", h.authority);
    object.field("sources", h.sources);
    object.field("initializers", h.initializers);
    object.field("commands", h.commands);
    object.field("dispositions", h.dispositions);
    object.field("coordinators", h.coordinators);
    object.field("settlements", h.settlements);
    object.field("reserved0", h.reserved0);
    object.field("reserved1", h.reserved1);
    object.field("reserved2", h.reserved2);
    object.field("reserved3", h.reserved3);
  }
  writeIdentityV3Rows(output, "sources", view.sources,
      [](IdentityV3JsonObject& object, const RenderTapeIdentityV3Source& row) {
        object.field("source_ordinal", row.sourceOrdinal);
        object.field("seq_id", row.seqId);
        object.field("writer_capture_id", row.writerCaptureId);
        object.field("writer_generation", row.writerGeneration);
        object.field("storage_generation", row.storageGeneration);
        object.field("publication_edge", row.publicationEdge);
        object.field("submission_edge", row.submissionEdge);
        object.field("completion_edge", row.completionEdge);
        object.field("completed_sequence", row.completedSequence);
        object.field("cut_event_ordinal", row.cutEventOrdinal);
        object.field("command_count", row.commandCount);
        object.field("cut_kind", row.cutKind);
        object.field("cut_record", row.cutRecord);
        object.field("reserved0", row.reserved0);
        object.field("phase", identityV3PhaseName(renderTapeIdentityV3SourcePhase(row)));
      });
  writeIdentityV3Rows(output, "initializers", view.initializers,
      [](IdentityV3JsonObject& object, const RenderTapeIdentityV3Initializer& row) {
        object.field("sequence", row.sequence);
        object.field("epoch", row.epoch);
        object.field("publication_edge", row.publicationEdge);
        object.field("submission_edge", row.submissionEdge);
        object.field("completion_edge", row.completionEdge);
        object.field("completed_sequence", row.completedSequence);
        object.field("cut_event_ordinal", row.cutEventOrdinal);
        object.field("cut_kind", row.cutKind);
        object.field("cut_record", row.cutRecord);
        object.field("reserved0", row.reserved0);
        object.field("reserved1", row.reserved1);
        object.field("phase", identityV3PhaseName(renderTapeIdentityV3InitializerPhase(row)));
      });
  writeIdentityV3Rows(output, "commands", view.commands,
      [](IdentityV3JsonObject& object, const RenderTapeIdentityV3Command& row) {
        object.field("logical_pass_id", row.logicalPassId);
        object.field("source_index", row.sourceIndex);
        object.field("physical_command_index", row.physicalCommandIndex);
        object.field("kind", row.kind);
        object.field("draw_count", row.drawCount);
        object.field("membership", row.membership);
        object.field("dag_pass_index", row.dagPassIndex);
        object.field("pass_kind", row.passKind);
        object.field("coordinator_index", row.coordinatorIndex);
        object.field("reserved0", row.reserved0);
        object.field("reserved1", row.reserved1);
      });
  writeIdentityV3Rows(output, "dispositions", view.dispositions,
      [](IdentityV3JsonObject& object, const RenderTapeIdentityV3Disposition& row) {
        object.field("event_ordinal", row.eventOrdinal);
        object.field("first_record", row.firstRecord);
        object.field("record_count", row.recordCount);
        object.field("kind", row.kind);
        object.field("command_index", row.commandIndex);
        object.field("first_subdraw", row.firstSubdraw);
        object.field("subdraw_count", row.subdrawCount);
        object.field("coordinator_index", row.coordinatorIndex);
        object.field("reserved0", row.reserved0);
      });
  writeIdentityV3Rows(output, "coordinators", view.coordinators,
      [](IdentityV3JsonObject& object, const RenderTapeIdentityV3Coordinator& row) {
        object.field("event_ordinal", row.eventOrdinal);
        object.field("raw_ordinal", row.rawOrdinal);
        object.field("build_generation", row.buildGeneration);
        object.field("dispatch_edge", row.dispatchEdge);
        object.field("release_ordinal", row.releaseOrdinal);
        object.field("release_generation", row.releaseGeneration);
        object.field("query_identity", row.queryIdentity);
        object.field("issue_cycle", row.issueCycle);
        object.field("issue_ordinal", row.issueOrdinal);
        object.field("device_epoch", row.deviceEpoch);
        object.field("writer_capture_id", row.writerCaptureId);
        object.field("writer_generation", row.writerGeneration);
        object.field("storage_generation", row.storageGeneration);
        object.field("prefix_sequence", row.prefixSequence);
        object.field("prefix_initializer", row.prefixInitializer);
        object.field("prefix_initializer_epoch", row.prefixInitializerEpoch);
        object.field("record_index", row.recordIndex);
        object.field("record_type", row.recordType);
        object.field("flags", row.flags);
        object.field("query_type", row.queryType);
        object.field("dispatch_result", row.dispatchResult);
        object.field("release_action", row.releaseAction);
        object.field("release_writer", row.releaseWriter);
        object.field("prefix_kind", row.prefixKind);
        object.field("source_index", row.sourceIndex);
        object.field("initializer_index", row.initializerIndex);
        object.field("command_end", row.commandEnd);
        object.field("observation", row.observation);
        object.field("reserved0", row.reserved0);
        object.field("reserved1", row.reserved1);
      });
  writeIdentityV3Rows(output, "replay_settlements", view.settlements,
      [](IdentityV3JsonObject& object, const RenderTapeIdentityV3ReplaySettlement& row) {
        object.field("event_ordinal", row.eventOrdinal);
        object.field("raw_ordinal", row.rawOrdinal);
        object.field("build_generation", row.buildGeneration);
        object.field("replay_edge", row.replayEdge);
        object.field("record_count", row.recordCount);
        object.field("reserved0", row.reserved0);
      });
  output << "}\n";
}

void writeIdentityV4(const RenderTapeIdentityV4View& view,
                     const RenderTapeExecutionTopologyV1View& topology) {
  auto& output = std::cout;
  output << "{\"schema\":\"" << kRenderTapeIdentityV4Schema
         << "\",\"valid\":true,\"pair_valid\":true,\"inspection_only\":true,"
            "\"execution_supported\":false,\"production_qualified\":false,"
            "\"header\":";
  {
    IdentityV3JsonObject object(output);
    const auto& h = view.header;
    object.field("magic", h.magic);
    object.field("events_bytes", h.eventsBytes);
    object.field("frame_id", h.frameId);
    object.field("present_ordinal", h.presentOrdinal);
    object.field("capture_token", h.captureToken);
    object.field("events_digest", encodeDigest(h.eventsDigest));
    object.field("version", h.version);
    object.field("header_size", h.headerSize);
    object.field("capture_epoch", h.captureEpoch);
    object.field("queue", h.queue);
    object.field("baseline_sequence", h.baselineSequence);
    object.field("baseline_initializer", h.baselineInitializer);
    object.field("baseline_completed_sequence", h.baselineCompletedSequence);
    object.field("initializer_epoch", h.initializerEpoch);
    object.field("baseline_completed_initializer", h.baselineCompletedInitializer);
    object.field("baseline_completion_edge", h.baselineCompletionEdge);
    object.field("baseline_kind", h.baselineKind);
    object.field("authority", h.authority);
    object.field("sources", h.sources);
    object.field("initializers", h.initializers);
    object.field("commands", h.commands);
    object.field("dispositions", h.dispositions);
    object.field("coordinators", h.coordinators);
    object.field("settlements", h.settlements);
    object.field("attempts", h.attempts);
    object.field("required_execution_topology_version", h.requiredExecutionTopologyVersion);
    object.field("reserved0", h.reserved0);
    object.field("reserved1", h.reserved1);
    object.field("reserved2", h.reserved2);
    object.field("reserved3", h.reserved3);
    object.field("coordinator_domain", h.coordinatorDomain);
    object.field("reserved4", h.reserved4);
  }
  writeIdentityV3Rows(output, "sources", view.sources,
      [&](IdentityV3JsonObject& object, const RenderTapeIdentityV4Source& row) {
        object.field("raw_ordinal", row.rawOrdinal);
        object.field("publication_build_generation", row.publicationBuildGeneration);
        object.field("publication_control_index", row.publicationControlIndex);
        object.field("reserved0", row.reserved0);
        output << ",\"lifecycle\":";
        IdentityV3JsonObject lifecycle(output);
        writeIdentitySourceFields(lifecycle, row.lifecycle);
      });
  writeIdentityV4SharedRows(output, view);
  writeIdentityV3Rows(output, "commands", view.commands,
      [](IdentityV3JsonObject& object, const RenderTapeIdentityV4PhysicalCommand& row) {
        object.field("source_index", row.sourceIndex);
        object.field("physical_command_index", row.physicalCommandIndex);
        object.field("kind", row.kind);
        object.field("draw_count", row.drawCount);
        object.field("coordinator_index", row.coordinatorIndex);
        object.field("reserved0", row.reserved0);
      });
  writeIdentityV3Rows(output, "replay_settlements", view.settlements,
      [](IdentityV3JsonObject& object, const RenderTapeIdentityV4RawSettlement& row) {
        object.field("event_ordinal", row.eventOrdinal);
        object.field("raw_ordinal", row.rawOrdinal);
        object.field("replay_edge", row.replayEdge);
        object.field("record_count", row.recordCount);
        object.field("reserved0", row.reserved0);
      });
  writeIdentityV3Rows(output, "replay_attempts", view.attempts,
      [](IdentityV3JsonObject& object, const RenderTapeIdentityV4ReplayAttempt& row) {
        object.field("event_ordinal", row.eventOrdinal);
        object.field("raw_ordinal", row.rawOrdinal);
        object.field("attempt_generation", row.attemptGeneration);
        object.field("begin_edge", row.beginEdge);
        object.field("terminal_edge", row.terminalEdge);
        object.field("effect_edge", row.effectEdge);
        object.field("identity", row.identity);
        object.field("receipt", row.receipt);
        object.field("first_record", row.firstRecord);
        object.field("record_count", row.recordCount);
        object.field("outcome", row.outcome);
        object.field("staged_command_count", row.stagedCommandCount);
      });
  output << ",\"execution_topology\":{\"schema\":\""
         << kRenderTapeExecutionTopologyV1Schema << "\",\"valid\":true,\"header\":";
  {
    IdentityV3JsonObject object(output);
    const auto& h = topology.header;
    object.field("magic", h.magic);
    object.field("identity_bytes", h.identityBytes);
    object.field("events_bytes", h.eventsBytes);
    object.field("capture_token", h.captureToken);
    object.field("capture_epoch", h.captureEpoch);
    object.field("queue", h.queue);
    object.field("identity_digest", encodeDigest(h.identityDigest));
    object.field("events_digest", encodeDigest(h.eventsDigest));
    object.field("version", h.version);
    object.field("header_size", h.headerSize);
    object.field("plans", h.plans);
    object.field("placements", h.placements);
    object.field("reserved0", h.reserved0);
    object.field("reserved1", h.reserved1);
  }
  writeIdentityV3Rows(output, "plans", topology.plans,
      [](IdentityV3JsonObject& object, const RenderTapeExecutionTopologyV1Plan& row) {
        object.field("plan_id", row.planId);
        object.field("acceptance_edge", row.acceptanceEdge);
        object.field("route", row.route);
        object.field("fallback_reason", row.fallbackReason);
        object.field("first_placement", row.firstPlacement);
        object.field("placement_count", row.placementCount);
        object.field("executed_count", row.executedCount);
        object.field("reserved0", row.reserved0);
      });
  writeIdentityV3Rows(output, "placements", topology.placements,
      [](IdentityV3JsonObject& object, const RenderTapeExecutionTopologyV1Placement& row) {
        object.field("plan_index", row.planIndex);
        object.field("command_index", row.commandIndex);
        object.field("outcome", row.outcome);
        object.field("execution_ordinal", row.executionOrdinal);
        object.field("dag_pass_index", row.dagPassIndex);
        object.field("pass_kind", row.passKind);
        object.field("omission_reason", row.omissionReason);
        object.field("reserved0", row.reserved0);
      });
  output << "}}\n";
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    usage();
    return 2;
  }
  const std::string command = argv[1];
  if (command != "validate" && command != "inspect" && command != "reduce" &&
      command != "project" && command != "identity" &&
      command != "materialize" && command != "policy-explore") {
    usage();
    return 2;
  }

  try {
    RenderTapeBlobCatalogue catalogue;
    std::vector<std::uint32_t> selected;
    RenderTapeProjectionSelector projectionSelector{};
    bool hasProjectionOrdinal = false;
    bool hasProjectionFirst = false;
    bool hasProjectionCount = false;
    bool hasOutputDigest = false;
    RenderTapeDigest outputDigest{};
    const char* inputPath = argv[2];
    const char* identityPath = nullptr;
    const char* executionTopologyPath = nullptr;
    const char* outputPath = nullptr;
    const char* outputIdentityPath = nullptr;
    int optionStart = 3;
    if (command == "reduce") {
      if (argc < 5) {
        usage();
        return 2;
      }
      outputPath = argv[3];
      optionStart = 4;
    } else if (command == "identity") {
      if (argc < 4) {
        usage();
        return 2;
      }
      identityPath = argv[3];
      optionStart = 4;
    } else if (command == "policy-explore") {
      if (argc < 4) {
        usage();
        return 2;
      }
      identityPath = argv[3];
      optionStart = 4;
    } else if (command == "materialize") {
      if (argc < 6) {
        usage();
        return 2;
      }
      identityPath = argv[3];
      outputPath = argv[4];
      optionStart = 5;
    }
    for (int i = optionStart; i < argc;) {
      const std::string_view option(argv[i]);
      if (option == "--verified-blob" && i + 1 < argc) {
        appendVerifiedBlob(catalogue, argv[i + 1]);
        i += 2;
      } else if (command == "identity" && option == "--execution-topology" &&
                 !executionTopologyPath && i + 1 < argc) {
        executionTopologyPath = argv[i + 1];
        i += 2;
      } else if (command == "reduce" && option == "--select-command-event" &&
                 i + 1 < argc) {
        selected.push_back(parseU32(argv[i + 1], "command event index"));
        i += 2;
      } else if (command == "project" &&
                 option == "--command-event-ordinal" && i + 1 < argc) {
        projectionSelector.commandEventOrdinal =
            parseUnsigned(argv[i + 1], "command event ordinal");
        hasProjectionOrdinal = true;
        i += 2;
      } else if (command == "project" && option == "--first-record" &&
                 i + 1 < argc) {
        projectionSelector.firstRecordIndex =
            parseU32(argv[i + 1], "first record index");
        hasProjectionFirst = true;
        i += 2;
      } else if (command == "project" && option == "--record-count" &&
                 i + 1 < argc) {
        projectionSelector.recordCount =
            parseU32(argv[i + 1], "record count");
        hasProjectionCount = true;
        i += 2;
      } else if (command == "materialize" &&
                 option == "--command-event-ordinal" && i + 1 < argc) {
        projectionSelector.commandEventOrdinal =
            parseUnsigned(argv[i + 1], "command event ordinal");
        hasProjectionOrdinal = true;
        i += 2;
      } else if (command == "materialize" && option == "--first-record" &&
                 i + 1 < argc) {
        projectionSelector.firstRecordIndex =
            parseU32(argv[i + 1], "first record index");
        hasProjectionFirst = true;
        i += 2;
      } else if (command == "materialize" && option == "--record-count" &&
                 i + 1 < argc) {
        projectionSelector.recordCount =
            parseU32(argv[i + 1], "record count");
        hasProjectionCount = true;
        i += 2;
      } else if (command == "materialize" && option == "--output-sha256" &&
                 i + 1 < argc && decodeDigest(argv[i + 1], outputDigest)) {
        hasOutputDigest = true;
        i += 2;
      } else if (command == "materialize" &&
                 option == "--output-identity" && i + 1 < argc) {
        outputIdentityPath = argv[i + 1];
        i += 2;
      } else {
        usage();
        return 2;
      }
    }
    if (command == "reduce") {
      if (selected.empty()) {
        usage();
        return 2;
      }
      const auto bytes = readFile(inputPath);
      const auto reduced = reduceRenderTape(bytes, catalogue, selected);
      if (!reduced.valid()) {
        std::cerr << "render tape reduction failed status="
                  << renderTapeReductionStatusName(reduced.status)
                  << " validation="
                  << renderTapeValidationStatusName(reduced.validation.status)
                  << " event=" << reduced.validation.failedEventIndex << '\n';
        return 1;
      }
      writeFile(outputPath, reduced.bytes);
      std::cout << "{\"schema\":\"dxmt9.render_tape.v2\","
                   "\"profile\":\"frame-tape\",\"status\":\"valid\","
                   "\"events\":"
                << reduced.retainedSourceEventIndices.size()
                << ",\"bytes\":" << reduced.bytes.size()
                << ",\"retained_source_events\":[";
      for (std::size_t i = 0u; i < reduced.retainedSourceEventIndices.size();
           ++i) {
        if (i != 0u) std::cout << ',';
        std::cout << reduced.retainedSourceEventIndices[i];
      }
      std::cout << "],\"referenced_blobs\":[";
      for (std::size_t i = 0u; i < reduced.referencedBlobDigests.size(); ++i) {
        if (i != 0u) std::cout << ',';
        std::cout << '\"' << encodeDigest(reduced.referencedBlobDigests[i])
                  << '\"';
      }
      std::cout << "]}\n";
      return 0;
    }

    if (command == "identity") {
      const auto bytes = readFile(inputPath);
      const auto identity = readFile(identityPath);
      if (identityVersion(identity) == kRenderTapeIdentityV4Version) {
        if (!executionTopologyPath) {
          std::cerr << "render tape identity v4 requires execution topology companion\n";
          return 1;
        }
        RenderTapeIdentityV4View view;
        const auto identityStatus = validateRenderTapeIdentityV4(
            bytes, catalogue, identity, &view, true);
        if (identityStatus != RenderTapeIdentityV4Status::Valid) {
          std::cerr << "render tape identity v4 failed status="
                    << static_cast<std::uint32_t>(identityStatus) << '\n';
          return 1;
        }
        const auto topology = readFile(executionTopologyPath);
        RenderTapeExecutionTopologyV1View topologyView;
        const auto topologyStatus = validateRenderTapeExecutionTopologyV1(
            bytes, catalogue, identity, topology, &topologyView);
        if (topologyStatus != RenderTapeExecutionTopologyV1Status::Valid) {
          std::cerr << "render tape identity v4 topology validation failed status="
                    << static_cast<std::uint32_t>(topologyStatus) << '\n';
          return 1;
        }
        writeIdentityV4(view, topologyView);
        return 0;
      }
      if (executionTopologyPath) {
        std::cerr << "render tape orphan execution topology requires identity v4\n";
        return 1;
      }
      if (identityVersion(identity) == kRenderTapeIdentityV3Version) {
        RenderTapeIdentityV3View view;
        const auto status = validateRenderTapeIdentityV3(
            bytes, catalogue, identity, &view);
        if (status != RenderTapeIdentityV3Status::Valid) {
          std::cerr << "render tape identity v3 failed status="
                    << static_cast<std::uint32_t>(status) << '\n';
          return 1;
        }
        writeIdentityV3(view);
        return 0;
      }
      RenderTapeIdentityView view;
      const auto validation =
          validateRenderTapeIdentity(bytes, catalogue, identity, &view);
      if (!validation.valid()) {
        std::cerr << "render tape identity failed status="
                  << renderTapeIdentityStatusName(validation.status)
                  << " source=" << validation.failedSource
                  << " range=" << validation.failedRange << '\n';
        return 1;
      }
      const auto authority = static_cast<RenderTapeIdentityAuthority>(
          view.header.authority);
      const bool capture = authority == RenderTapeIdentityAuthority::Capture;
      const bool derived = authority ==
          RenderTapeIdentityAuthority::DerivedProjection;
      const bool providerReplay = authority ==
          RenderTapeIdentityAuthority::ProviderReplay;
      const bool queueSettled = capture && !view.settlements.empty();
      const char* completionEvidence = capture
          ? (queueSettled ? "queue-tail-fenced" : "none")
          : derived ? "not-queue-authenticated" : "process-local-unverified";
      const std::size_t evidenceSettlementCount =
          providerReplay ? 0u : view.settlements.size();
      const std::uint32_t evidenceTableCount =
          providerReplay ? 0u : view.header.settlementCount;
      std::cout << "{\"schema\":\"" << kRenderTapeIdentitySchema
                << "\",\"valid\":true,\"authority\":"
                << view.header.authority << ",\"frame_id\":"
                << view.header.frameId << ",\"present_ordinal\":"
                << view.header.presentOrdinal << ",\"sources\":"
                << view.sources.size() << ",\"ranges\":"
                << view.ranges.size() << ",\"completed_segment_count\":"
                << (queueSettled ? view.sources.size() : 0u)
                << ",\"completion_evidence\":\""
                << completionEvidence << "\",\"settlement_count\":"
                << evidenceSettlementCount << ",\"settlement_table_count\":"
                << evidenceTableCount << ",\"segments\":[";
      for (std::size_t index = 0u; index < view.sources.size(); ++index) {
        if (index != 0u) std::cout << ',';
        const auto& source = view.sources[index];
        std::cout << "{\"segment_index\":" << index
                  << ",\"event_ordinal\":" << source.eventOrdinal
                  << ",\"source_ordinal\":" << source.sourceOrdinal
                  << ",\"seq_id\":" << source.seqId
                  << ",\"first_record\":" << source.firstRecord
                  << ",\"record_count\":" << source.recordCount << "}";
      }
      std::cout << "],\"event_settlement_table\":[";
      if (capture) for (std::size_t index = 0u;
                         index < view.settlements.size(); ++index) {
        if (index != 0u) std::cout << ',';
        const auto& settlement = view.settlements[index];
        std::cout << "{\"event_ordinal\":" << settlement.eventOrdinal
                  << ",\"raw_ordinal\":" << settlement.rawOrdinal
                  << ",\"build_generation\":"
                  << settlement.buildGeneration
                  << ",\"first_source_ordinal\":"
                  << settlement.firstSourceOrdinal
                  << ",\"tail_seq_id\":" << settlement.tailSeqId
                  << ",\"source_count\":" << settlement.sourceCount << "}";
      }
      std::cout << "],\"final_event_settlement\":";
      if (!capture || view.settlements.empty()) {
        std::cout << "null";
      } else {
        const auto& settlement = view.settlements.back();
        std::cout << "{\"event_ordinal\":" << settlement.eventOrdinal
                  << ",\"tail_seq_id\":" << settlement.tailSeqId
                  << ",\"source_count\":" << settlement.sourceCount << "}";
      }
      std::cout << "}\n";
      return 0;
    }

    if (command == "materialize") {
      if (!hasProjectionOrdinal || !hasProjectionFirst ||
          !hasProjectionCount) {
        usage();
        return 2;
      }
      const auto bytes = readFile(inputPath);
      const auto identity = readFile(identityPath);
      if (!requireV2Identity(identity)) return 1;
      const auto projection = materializeRenderTapeProjectionBundle(
          bytes, catalogue, identity, projectionSelector,
          hasOutputDigest ? RenderTapeDigestValidity::Sha256
                          : RenderTapeDigestValidity::NotCaptured,
          outputDigest);
      if (!projection.valid()) {
        std::cerr << "render tape materialization failed status="
                  << renderTapeProjectionBundleStatusName(projection.status)
                  << " projection="
                  << renderTapeProjectionStatusName(
                         projection.projection.status)
                  << " identity="
                  << renderTapeIdentityStatusName(
                         projection.identityValidation.status)
                  << " validation="
                  << renderTapeValidationStatusName(
                         projection.projection.sourceValidation.status)
                  << " event="
                  << projection.projection.sourceValidation.failedEventIndex
                  << " event_type="
                  << projection.projection.sourceValidation.failedEventType
                  << " incomplete_reason="
                  << renderTapeIncompleteFrameReasonName(
                         projection.projection.sourceValidation
                             .incompleteFrameReason)
                  << " chunk_status="
                  << static_cast<std::uint32_t>(
                         projection.projection.sourceValidation.chunkStatus)
                  << '\n';
        return 1;
      }
      writeFile(outputPath, projection.bytes);
      if (outputIdentityPath) {
        writeFile(outputIdentityPath, projection.identity);
      }
      std::cout << "{\"schema\":\"dxmt9.render_tape.v2\","
                   "\"profile\":\"frame-tape\",\"status\":\"valid\","
                   "\"logical_pass_id\":"
                << projection.logicalPassId << ",\"bytes\":"
                << projection.bytes.size() << ",\"referenced_blobs\":[";
      for (std::size_t i = 0u; i < projection.referencedBlobDigests.size();
           ++i) {
        if (i != 0u) std::cout << ',';
        std::cout << '\"'
                  << encodeDigest(projection.referencedBlobDigests[i]) << '\"';
      }
      std::cout << "]}\n";
      return 0;
    }

    if (command == "project") {
      if (!hasProjectionOrdinal || !hasProjectionFirst ||
          !hasProjectionCount) {
        usage();
        return 2;
      }
      const auto bytes = readFile(inputPath);
      const auto projection =
          projectRenderTapeDrawSlice(bytes, catalogue, projectionSelector);
      if (!projection.valid()) {
        std::cerr << "render tape projection failed status="
                  << renderTapeProjectionStatusName(projection.status)
                  << " validation="
                  << renderTapeValidationStatusName(
                         projection.sourceValidation.status)
                  << " event=" << projection.failedEventIndex
                  << " record=" << projection.failedRecordIndex;
        if (projection.status == RenderTapeProjectionStatus::StateFoldFailed) {
          std::cerr << " state_fold="
                    << renderTapeStateFoldStatusName(
                           projection.stateFold.status)
                    << " state_section="
                    << projection.stateFold.failedSectionKind;
        }
        std::cerr << '\n';
        return 1;
      }
      writeProjection(projection);
      return 0;
    }

    if (command == "policy-explore") {
      const auto bytes = readFile(inputPath);
      const auto identity = readFile(identityPath);
      if (!requireV2Identity(identity)) return 1;
      const auto exploration = exploreRenderTapeParallelPolicy(
          bytes, catalogue, identity);
      if (!exploration.valid()) {
        std::cerr << "render tape policy exploration failed status="
                  << renderTapePolicyExploreStatusName(exploration.status)
                  << " identity="
                  << renderTapeIdentityStatusName(
                         exploration.identityValidation.status)
                  << "\n";
        return 1;
      }
      writePolicyExplore(exploration);
      return 0;
    }

    const auto bytes = readFile(inputPath);
    ImportedRenderTapeView tape;
    const auto validation = validateRenderTape(bytes, catalogue, &tape);
    if (!validation.valid()) {
      std::cerr << "render tape invalid status="
                << renderTapeValidationStatusName(validation.status)
                << " event=" << validation.failedEventIndex << " chunk_status="
                << static_cast<std::uint32_t>(validation.chunkStatus) << '\n';
      return 1;
    }
    if (command == "validate") {
      std::cout << "{\"schema\":\"dxmt9.render_tape.v2\","
                   "\"profile\":\""
                << renderTapeProfileName(tape.header.profile)
                << "\",\"valid\":true,"
                   "\"events\":"
                << tape.header.eventCount
                << ",\"presents\":" << tape.header.presentCount << "}\n";
      return 0;
    }

    std::vector<std::uint32_t> commandEventIndices;
    std::vector<RenderTapeCpuWindowSourcePlan> cpuWindowSources;
    std::vector<std::uint32_t> presentCommandEvents;
    for (std::uint32_t i = 0u; i < tape.events.size(); ++i) {
      if (static_cast<RenderTapeEventType>(tape.events[i].type) !=
          RenderTapeEventType::CommandChunk) {
        continue;
      }
      commandEventIndices.push_back(i);
      const auto event = tape.event(i);
      RenderTapeCommandChunkHeader fixed{};
      std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
      ImportedChunkView chunk;
      const auto chunkResult = validateCommandChunk(
          event.payload.subspan(sizeof(fixed), fixed.chunkBytes),
          CommandChunkEnvelope{.version = fixed.wireVersion,
                               .recordCount = fixed.recordCount,
                               .handleCount = fixed.handleCount},
          &chunk);
      if (!chunkResult.valid()) {
        throw std::runtime_error("validated command chunk could not be imported");
      }
      cpuWindowSources.push_back(planRenderTapeCpuWindowSource(
          chunk, static_cast<RawOrdinal>(i + 1u), kRenderTapeCpuWindowPageSize));
      if (std::any_of(chunk.records.begin(), chunk.records.end(),
                      [](const auto& record) {
                        return record.type == D9C_COMMAND_RECORD_PRESENT;
                      })) {
        presentCommandEvents.push_back(i);
      }
    }

    InspectSink sink;
    if (!sink.collectBufferInspection(tape)) {
      throw std::runtime_error(
          "validated render tape buffer inspection could not be imported");
    }
    if (!sink.collectQueryInspection(tape)) {
      throw std::runtime_error("validated render tape query inspection could not be imported");
    }
    const auto replay = replayPrevalidatedRenderTape(tape, catalogue, sink);
    if (!replay.complete) {
      std::cerr << "render tape inspect replay failed event="
                << replay.failedEventIndex << '\n';
      return 1;
    }
    std::cout << "{\"schema\":\"dxmt9.render_tape.v2\","
                 "\"profile\":\""
              << renderTapeProfileName(tape.header.profile)
              << "\",\"valid\":true,\"events_sha256\":\""
              << encodeDigest(RenderTapeCaptureSession::sha256(bytes)) << "\","
                 "\"events\":"
              << sink.events
              << ",\"bootstraps\":" << sink.bootstraps
              << ",\"defines\":" << sink.defines
              << ",\"texture_descriptors_v2\":"
              << sink.textureDescriptorsV2
              << ",\"surface_descriptors_v2\":"
              << sink.surfaceDescriptorsV2
              << ",\"mutations\":" << sink.mutations
              << ",\"mutation_bytes\":" << sink.mutationBytes
              << ",\"chunks\":" << sink.chunks
              << ",\"records\":" << sink.records
              << ",\"handles\":" << sink.handles
              << ",\"destroys\":" << sink.destroys
              << ",\"controls\":" << sink.controls
              << ",\"present_completions\":" << sink.completions
              << ",\"last_present_ordinal\":" << sink.lastPresentOrdinal
              << ",\"expected_output_sha256\":";
    if (sink.lastDigestValidity ==
        static_cast<std::uint32_t>(RenderTapeDigestValidity::Sha256)) {
      std::cout << '\"' << encodeDigest(sink.lastExpectedDigest) << '\"';
    } else {
      std::cout << "null";
    }
    std::cout << ",\"dynamic_buffers\":" << sink.dynamicBuffers
              << ",\"event_types\":[";
    for (std::size_t i = 0u; i < tape.events.size(); ++i) {
      if (i != 0u) std::cout << ',';
      std::cout << tape.events[i].type;
    }
    std::cout << "],\"buffer_definitions\":[";
    for (std::size_t i = 0u; i < sink.bufferDefinitions.size(); ++i) {
      if (i != 0u) std::cout << ',';
      const auto& definition = sink.bufferDefinitions[i];
      std::cout << "{\"event\":" << definition.event << ",\"identity\":";
      writeInspectionIdentity(std::cout, definition.identity);
      std::cout << ",\"usage\":" << definition.descriptor.usage
                << ",\"pool\":" << definition.descriptor.pool
                << ",\"size\":" << definition.descriptor.size << '}';
    }
    std::cout << "],\"buffer_mutations\":[";
    for (std::size_t i = 0u; i < sink.bufferMutations.size(); ++i) {
      if (i != 0u) std::cout << ',';
      const auto& mutation = sink.bufferMutations[i];
      std::cout << "{\"event\":" << mutation.event << ",\"identity\":";
      writeInspectionIdentity(std::cout, mutation.identity);
      std::cout << ",\"mutation_kind\":" << mutation.mutationKind
                << ",\"disposition\":" << mutation.disposition
                << ",\"byte_offset\":" << mutation.byteOffset
                << ",\"byte_size\":" << mutation.byteSize << '}';
    }
    std::cout << "],\"buffer_destroys\":[";
    for (std::size_t i = 0u; i < sink.bufferDestroys.size(); ++i) {
      if (i != 0u) std::cout << ',';
      const auto& destroy = sink.bufferDestroys[i];
      std::cout << "{\"event\":" << destroy.event << ",\"identity\":";
      writeInspectionIdentity(std::cout, destroy.identity);
      std::cout << '}';
    }
    std::cout << "],\"query_transcript\":{\"schema\":\"dxmt9.render_tape.query_transcript.v2\","
                 "\"complete\":" << (sink.queryTranscriptComplete ? "true" : "false")
              << ",\"query_definitions\":" << sink.queryDefinitions.size()
              << ",\"issue_records\":" << sink.queryIssues.size()
              << ",\"recorded_polls\":" << sink.queryPolls.size()
              << ",\"definitions\":[";
    for (std::size_t index = 0u; index < sink.queryDefinitions.size(); ++index) {
      if (index) std::cout << ',';
      const auto& row = sink.queryDefinitions[index];
      std::cout << "{\"event_ordinal\":" << row.event << ",\"identity\":";
      writeInspectionIdentity(std::cout, row.identity);
      std::cout << ",\"versioned\":" << (row.versioned ? "true" : "false")
                << ",\"schema_version\":" << row.descriptor.schemaVersion
                << ",\"type\":" << row.descriptor.type
                << ",\"data_bytes\":" << row.descriptor.dataBytes
                << ",\"initial_state\":" << row.descriptor.initialState
                << ",\"initial_issue_cycle\":" << row.descriptor.initialIssueCycle;
      if (row.descriptor.schemaVersion == kRenderTapeQueryDescriptorVersion3) {
        const auto& checkpoint = row.descriptor.checkpoint;
        std::cout << ",\"checkpoint\":{\"checkpoint_version\":" << checkpoint.checkpointVersion
                  << ",\"status\":" << checkpoint.status
                  << ",\"query_type\":" << checkpoint.queryType
                  << ",\"active\":" << checkpoint.active << ",\"identity\":";
        writeInspectionIdentity(std::cout, checkpoint.identity);
        std::cout << ",\"issue_cycle\":" << checkpoint.issueCycle
                  << ",\"core_query_identity\":" << checkpoint.coreQueryIdentity
                  << ",\"device_epoch\":" << checkpoint.deviceEpoch
                  << ",\"issue_ordinal\":" << checkpoint.issueOrdinal
                  << ",\"raw_ordinal\":" << checkpoint.rawOrdinal
                  << ",\"queue_identity\":" << checkpoint.queueIdentity
                  << ",\"end_sequence\":" << checkpoint.endSequence
                  << ",\"initializer_sequence\":" << checkpoint.initializerSequence
                  << ",\"completed_sequence\":" << checkpoint.completedSequence
                  << ",\"completed_initializer\":" << checkpoint.completedInitializer
                  << ",\"result_code\":" << checkpoint.resultCode
                  << ",\"reserved0\":" << checkpoint.reserved0 << '}';
      }
      std::cout << '}';
    }
    std::cout << "],\"issues\":[";
    for (std::size_t index = 0u; index < sink.queryIssues.size(); ++index) {
      if (index) std::cout << ',';
      const auto& row = sink.queryIssues[index];
      std::cout << "{\"event_ordinal\":" << row.event << ",\"record_index\":"
                << row.record << ",\"identity\":";
      writeInspectionIdentity(std::cout, row.identity);
      std::cout << ",\"flags\":" << row.flags << ",\"issue_cycle\":" << row.issueCycle << '}';
    }
    std::cout << "],\"polls\":[";
    for (std::size_t index = 0u; index < sink.queryPolls.size(); ++index) {
      if (index) std::cout << ',';
      const auto& row = sink.queryPolls[index];
      std::cout << "{\"event_index\":" << row.event - 1u
                << ",\"event_ordinal\":" << row.event << ",\"identity\":";
      writeInspectionIdentity(std::cout, row.header.identity);
      std::cout << ",\"versioned\":" << (row.versioned ? "true" : "false")
                << ",\"issue_cycle\":" << row.control.issueCycle
                << ",\"flags\":" << row.control.flags
                << ",\"has_output\":" << (row.control.hasOutput ? "true" : "false")
                << ",\"data_size\":" << row.control.dataSize
                << ",\"captured_result\":" << row.header.resultCode
                << ",\"captured_disposition\":" << row.header.disposition
                << ",\"captured_completion_ordinal\":" << row.header.completionOrdinal
                << ",\"defined_bytes\":" << row.control.definedBytes
                << ",\"captured_hex\":\"" << encodeQueryBytes(std::span(row.control.result).first(
                       std::min<std::size_t>(row.control.definedBytes, row.control.result.size())))
                << "\"}";
    }
    std::cout << "]},\"command_event_indices\":[";
    for (std::size_t i = 0u; i < commandEventIndices.size(); ++i) {
      if (i != 0u) std::cout << ',';
      std::cout << commandEventIndices[i];
    }
    std::cout << "],\"cpu_window_plan\":{\"schema\":\"dxmt9.render_tape.cpu_window_plan.v1\","
                 "\"page_size\":" << kRenderTapeCpuWindowPageSize
              << ",\"bootstrap_state_only\":" << (sink.bootstrapStateOnly ? "true" : "false")
              << ",\"sources\":[";
    for (std::size_t index = 0u; index < cpuWindowSources.size(); ++index) {
      if (index) std::cout << ',';
      const auto& shape = cpuWindowSources[index];
      const auto& emission = shape.emission;
      std::cout << "{\"event\":" << commandEventIndices[index] + 1u
                << ",\"record_count\":" << shape.recordCount
                << ",\"allowed_record_count\":" << shape.allowedRecordCount
                << ",\"partitioned\":" << (emission.partitioned() ? "true" : "false")
                << ",\"lease_eligible\":" << (emission.leaseEligible() ? "true" : "false")
                << ",\"coordinator_count\":" << emission.coordinatorCount
                << ",\"ordered_control_count\":" << emission.orderedControlCount
                << ",\"compatibility_record_count\":" << emission.compatibilityRecordCount
                << ",\"eligible\":" << (shape.eligible() ? "true" : "false")
                << ",\"lease_spans\":[";
      for (std::size_t i = 0u; i < emission.leaseSpans.size(); ++i) {
        if (i) std::cout << ',';
        const auto& span = emission.leaseSpans[i];
        std::cout << "{\"first_record_index\":" << span.firstRecordIndex
                  << ",\"record_count\":" << span.recordCount
                  << ",\"owns_lease\":" << (span.ownsLease ? "true" : "false")
                  << ",\"final_lease_span\":" << (span.finalLeaseSpan ? "true" : "false") << '}';
      }
      std::cout << "]}";
    }
    std::cout << "]},\"present_command_events\":[";
    for (std::size_t i = 0u; i < presentCommandEvents.size(); ++i) {
      if (i != 0u) std::cout << ',';
      std::cout << presentCommandEvents[i];
    }
    std::cout << "],\"present_command_event\":";
    if (presentCommandEvents.empty()) {
      std::cout << "null";
    } else {
      std::cout << presentCommandEvents.back();
    }
    std::cout << ",\"grammar\":{\"descriptor_kinds\":";
    writeTypedCounts(std::cout, sink.descriptorKinds, kDescriptorKindNames);
    std::cout << ",\"texture_dimensions\":";
    writeTypedCounts(std::cout, sink.textureDimensions,
                     kTextureDimensionNames);
    std::cout << ",\"content_dispositions\":";
    writeTypedCounts(std::cout, sink.contentDispositions,
                     kContentDispositionNames);
    std::cout << ",\"surface_storages\":";
    writeTypedCounts(std::cout, sink.surfaceStorages, kSurfaceStorageNames);
    std::cout << ",\"mutation_kinds\":";
    writeTypedCounts(std::cout, sink.mutationKinds, kMutationKindNames);
    std::cout << ",\"mutation_object_kinds\":";
    writeTypedCounts(std::cout, sink.mutationObjectKinds, kObjectKindNames);
    std::cout << ",\"destroy_object_kinds\":";
    writeTypedCounts(std::cout, sink.destroyObjectKinds, kObjectKindNames);
    std::cout << ",\"control_kinds\":";
    writeTypedCounts(std::cout, sink.controlKinds, kControlKindNames);
    std::cout << ",\"record_types\":";
    writeTypedCounts(std::cout, sink.recordTypes, kRecordTypeNames);
    std::cout << ",\"section_kinds\":";
    writeTypedCounts(std::cout, sink.sectionKinds, kSectionKindNames);
    std::cout << "}}\n";
  } catch (const std::exception& error) {
    std::cerr << "dxmt9-render-tape: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
