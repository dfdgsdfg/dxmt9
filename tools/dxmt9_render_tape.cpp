#include "device_c_render_tape.hpp"
#include "device_c_render_tape_projection.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
  std::ofstream stream(path, std::ios::binary);
  if (!stream ||
      (!bytes.empty() &&
       !stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size())))) {
    throw std::runtime_error(std::string("cannot write tape: ") + path);
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
  bool objectDefine(const RenderTapeObjectDefineHeader&,
                    std::span<const std::byte>) override {
    ++defines;
    ++events;
    return true;
  }
  bool resourceMutation(const RenderTapeResourceMutationHeader& mutation) override {
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
    ++events;
    return true;
  }
  bool objectDestroy(const RenderTapeObjectDestroyHeader&) override {
    ++destroys;
    ++events;
    return true;
  }
  bool orderedControl(const RenderTapeOrderedControlHeader&,
                      std::span<const std::byte>) override {
    ++controls;
    ++events;
    return true;
  }
  bool presentComplete(const RenderTapePresentCompleteHeader& complete,
                       std::span<const std::byte>) override {
    ++completions;
    lastPresentOrdinal = complete.presentOrdinal;
    ++events;
    return true;
  }

  std::uint64_t events = 0u;
  std::uint64_t bootstraps = 0u;
  std::uint64_t defines = 0u;
  std::uint64_t mutations = 0u;
  std::uint64_t mutationBytes = 0u;
  std::uint64_t chunks = 0u;
  std::uint64_t records = 0u;
  std::uint64_t handles = 0u;
  std::uint64_t destroys = 0u;
  std::uint64_t controls = 0u;
  std::uint64_t completions = 0u;
  std::uint64_t lastPresentOrdinal = 0u;
};

void usage() {
  std::cerr << "usage: dxmt9-render-tape <validate|inspect> <tape.bin> "
               "[--verified-blob <sha256>:<bytes>]...\n"
               "       dxmt9-render-tape reduce <input> <output> "
               "--select-command-event <index>... "
               "[--verified-blob <sha256>:<bytes>]...\n"
               "       dxmt9-render-tape project <tape.bin> "
               "--command-event-ordinal <ordinal> --first-record <index> "
               "--record-count <count> "
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

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    usage();
    return 2;
  }
  const std::string command = argv[1];
  if (command != "validate" && command != "inspect" && command != "reduce" &&
      command != "project") {
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
    const char* inputPath = argv[2];
    const char* outputPath = nullptr;
    int optionStart = 3;
    if (command == "reduce") {
      if (argc < 5) {
        usage();
        return 2;
      }
      outputPath = argv[3];
      optionStart = 4;
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
                  << " record=" << projection.failedRecordIndex << '\n';
        return 1;
      }
      writeProjection(projection);
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
              << ",\"mutations\":" << sink.mutations
              << ",\"mutation_bytes\":" << sink.mutationBytes
              << ",\"chunks\":" << sink.chunks
              << ",\"records\":" << sink.records
              << ",\"handles\":" << sink.handles
              << ",\"destroys\":" << sink.destroys
              << ",\"controls\":" << sink.controls
              << ",\"present_completions\":" << sink.completions
              << ",\"last_present_ordinal\":" << sink.lastPresentOrdinal
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
    std::cout << "}\n";
  } catch (const std::exception& error) {
    std::cerr << "dxmt9-render-tape: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
