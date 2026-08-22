#include "device_c_render_tape.hpp"
#include "device_c_render_tape_projection.hpp"
#include "device_c_render_tape_policy.hpp"

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
  bool bootstrap(const RenderTapeBootstrapHeader&, std::span<const std::byte>,
                 RenderTapeBootstrapReplayMode mode) override {
    if (mode != RenderTapeBootstrapReplayMode::JournalOnlyDeferredProvider) return false;
    ++bootstraps;
    ++events;
    return true;
  }
  bool objectDefine(const RenderTapeObjectDefineHeader& fixed,
                    std::span<const std::byte> descriptor) override {
    if (fixed.descriptorKind < descriptorKinds.size()) {
      ++descriptorKinds[fixed.descriptorKind];
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
  std::array<std::uint64_t, 6u> contentDispositions{};
  std::array<std::uint64_t, 4u> surfaceStorages{};
  std::array<std::uint64_t, 5u> mutationKinds{};
  std::array<std::uint64_t, 6u> mutationObjectKinds{};
  std::array<std::uint64_t, 6u> destroyObjectKinds{};
  std::array<std::uint64_t, 6u> controlKinds{};
  std::array<std::uint64_t, 30u> recordTypes{};
  std::array<std::uint64_t, D9C_COMMAND_CHUNK_SECTION_COUNT + 1u>
      sectionKinds{};
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
constexpr std::array<const char*, 6u> kContentDispositionNames{
    nullptr, "complete_seed", "unavailable", "produced_present_output",
    "produced_by_captured_pass", "complete_depth_float32_v1"};
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
constexpr std::array<const char*, 30u> kRecordTypeNames{
    nullptr,
    "draw_primitive", "draw_indexed_primitive", "draw_primitive_up",
    "draw_indexed_primitive_up",
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr,
    "set_vs_const_f", "set_vs_const_i", "set_vs_const_b",
    "set_ps_const_f", "set_ps_const_i", "set_ps_const_b",
    "clear", "present", "stretch_rect", "color_fill", "update_texture",
    "update_surface", "query_issue", "readback", "apply_state",
    "resz_depth_resolve"};
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
      std::cout << "{\"schema\":\"" << kRenderTapeIdentitySchema
                << "\",\"valid\":true,\"authority\":"
                << view.header.authority << ",\"frame_id\":"
                << view.header.frameId << ",\"present_ordinal\":"
                << view.header.presentOrdinal << ",\"sources\":"
                << view.sources.size() << ",\"ranges\":"
                << view.ranges.size() << ",\"completed_segment_count\":"
                << view.sources.size() << ",\"settlement_count\":"
                << view.settlements.size() << ",\"settlement_table_count\":"
                << view.header.settlementCount << ",\"segments\":[";
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
      std::cout << "]}\n";
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
      const auto projection = materializeRenderTapeProjectionBundle(
          bytes, catalogue, identity, projectionSelector,
          hasOutputDigest ? RenderTapeDigestValidity::Sha256
                          : RenderTapeDigestValidity::NotCaptured,
          outputDigest);
      if (!projection.valid()) {
        std::cerr << "render tape materialization failed status="
                  << renderTapeProjectionBundleStatusName(projection.status)
                  << " identity="
                  << renderTapeIdentityStatusName(
                         projection.identityValidation.status)
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
      if (std::any_of(chunk.records.begin(), chunk.records.end(),
                      [](const auto& record) {
                        return record.type == D9C_COMMAND_RECORD_PRESENT;
                      })) {
        presentCommandEvents.push_back(i);
      }
    }

    InspectSink sink;
    const auto replay = replayPrevalidatedRenderTape(tape, catalogue, sink);
    if (!replay.complete) {
      std::cerr << "render tape inspect replay failed event="
                << replay.failedEventIndex << '\n';
      return 1;
    }
    std::cout << "{\"schema\":\"dxmt9.render_tape.v2\","
                 "\"profile\":\""
              << renderTapeProfileName(tape.header.profile)
              << "\",\"valid\":true,"
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
    std::cout
              << ",\"command_event_indices\":[";
    for (std::size_t i = 0u; i < commandEventIndices.size(); ++i) {
      if (i != 0u) std::cout << ',';
      std::cout << commandEventIndices[i];
    }
    std::cout << "],\"present_command_events\":[";
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
