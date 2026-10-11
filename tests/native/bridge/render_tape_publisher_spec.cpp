#include "d3d9_pe_render_tape_publisher.hpp"
#include "dxmt9/render_tape_attachment_history.hpp"
#include "device_c_render_tape_action_profile_component.hpp"
#include "device_c_render_tape_identity_v3.hpp"
#include "render_tape_identity_v4_fixture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <new>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace publisher_pair_allocation_probe {
bool enabled = false;
std::size_t count = 0u;
bool failEnabled = false;
std::size_t failAfter = 0u;
} // namespace publisher_pair_allocation_probe

void* operator new(std::size_t size) {
  if (publisher_pair_allocation_probe::failEnabled) {
    if (!publisher_pair_allocation_probe::failAfter) {
      publisher_pair_allocation_probe::failEnabled = false;
      throw std::bad_alloc();
    }
    --publisher_pair_allocation_probe::failAfter;
  }
  if (publisher_pair_allocation_probe::enabled) ++publisher_pair_allocation_probe::count;
  if (auto* value = std::malloc(size)) return value;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete(void* value, std::size_t) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }

namespace {

using namespace dxmt9::d3d9;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

bool testRoot(std::filesystem::path& root) noexcept {
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  std::error_code error;
  const auto base = std::filesystem::weakly_canonical(
      std::filesystem::temp_directory_path(), error);
  if (error) {
    return false;
  }
  root = base / ("dxmt9-render-tape-publisher-" + std::to_string(ticks));
  return true;
}

std::string readText(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  check(stream.good(), "publisher test reads manifest");
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

std::vector<std::byte> readBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  check(stream.good(), "publisher test reads bytes");
  stream.seekg(0, std::ios::end);
  const auto size = stream.tellg();
  check(size >= 0, "publisher test sizes bytes");
  stream.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty()) {
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    check(stream.gcount() == static_cast<std::streamsize>(bytes.size()),
          "publisher test reads every declared byte");
  }
  check(stream.good() || stream.eof(), "publisher test reads bytes fully");
  return bytes;
}

RenderTapePublicationBundle bundle(
    std::uint32_t profile = kRenderTapeProfileFrame) {
  RenderTapePublicationBundle value{};
  RenderTapeHeader header{};
  header.profile = profile;
  value.events.resize(sizeof(header));
  std::memcpy(value.events.data(), &header, sizeof(header));
  const std::array<std::byte, 2u> bytes{std::byte{0xaa}, std::byte{0xbb}};
  value.blobs.push_back(RenderTapePublishedBlob{
      .digest = RenderTapeCaptureSession::sha256(bytes),
      .bytes = std::vector<std::byte>(bytes.begin(), bytes.end()),
  });
  return value;
}

std::string digestText(const RenderTapeDigest& digest) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (const auto value : digest) {
    stream << std::setw(2)
           << static_cast<unsigned>(std::to_integer<unsigned char>(value));
  }
  return stream.str();
}

void setOutputRoot(const char* value) {
#if defined(_WIN32)
  _putenv_s("DXMT9_RENDER_TAPE_OUTPUT_ROOT", value ? value : "");
#else
  if (value) {
    setenv("DXMT9_RENDER_TAPE_OUTPUT_ROOT", value, 1);
  } else {
    unsetenv("DXMT9_RENDER_TAPE_OUTPUT_ROOT");
  }
#endif
}

struct OutputRootScope {
  const bool hadValue = std::getenv("DXMT9_RENDER_TAPE_OUTPUT_ROOT") != nullptr;
  const std::string savedValue =
      hadValue ? std::getenv("DXMT9_RENDER_TAPE_OUTPUT_ROOT") : "";

  ~OutputRootScope() {
    setOutputRoot(hadValue ? savedValue.c_str() : nullptr);
  }
};

void testOutputRootTruthTable(const std::filesystem::path& root) {
  const OutputRootScope restore;
  setOutputRoot(nullptr);
  check(!dxmt9PeRenderTapeOutputConfigured(),
        "unset output root is not configured");
  setOutputRoot("relative-render-tape-root");
  check(!dxmt9PeRenderTapeOutputConfigured(),
        "relative output root is not configured");
  const auto absent = root / "truth" / "nested";
  setOutputRoot(absent.string().c_str());
  check(dxmt9PeRenderTapeOutputConfigured(),
        "absolute absent output root is configured");
  const auto link = root / "truth-link";
  const auto target = root / "truth-target";
  std::error_code error;
  std::filesystem::create_directory(target, error);
  check(!error, "truth table creates symlink target");
  std::filesystem::create_directory_symlink(target, link, error);
  check(!error, "truth table creates symlink component");
  setOutputRoot((link / "nested").string().c_str());
  check(!dxmt9PeRenderTapeOutputConfigured(),
        "symlink output root is not configured");
}

void testTransactionalSuccessAndCollision(const std::filesystem::path& root) {
  auto value = bundle();
  value.outputOracle = {
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0xff}};
  value.sourceOracle = {
      std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};
  check(dxmt9PePublishRenderTapeBundle(value, root.string(), "frame-test"),
        "publisher commits a complete bundle");
  const auto frame = root / "frame-test";
  check(std::filesystem::is_regular_file(frame / "events.bin"),
        "publisher commits events.bin");
  check(std::filesystem::is_regular_file(frame / "manifest.json"),
        "publisher commits manifest");
  check(std::filesystem::is_regular_file(frame / "output.rgba"),
        "publisher commits the authoritative output sidecar");
  check(std::filesystem::is_regular_file(frame / "source.rgba"),
        "publisher commits the source-before-Present sidecar");
  check(std::filesystem::is_directory(frame / "blobs"),
        "publisher commits blob directory");
  std::size_t blobFiles = 0u;
  for (const auto& entry : std::filesystem::directory_iterator(frame / "blobs")) {
    check(entry.is_regular_file() && entry.path().extension() == ".bin" &&
              entry.path().stem().string().size() == 64u,
          "publisher uses digest-named blob files");
    ++blobFiles;
  }
  check(blobFiles == 1u, "publisher commits one blob");
  const auto manifest = readText(frame / "manifest.json");
  const auto eventDigest = digestText(RenderTapeCaptureSession::sha256(
      std::span<const std::byte>(value.events)));
  const auto blobDigest = digestText(value.blobs[0].digest);
  const auto outputDigest = digestText(
      RenderTapeCaptureSession::sha256(value.outputOracle));
  const auto sourceDigest = digestText(
      RenderTapeCaptureSession::sha256(value.sourceOracle));
  check(readBytes(frame / "events.bin") == value.events &&
            readBytes(frame / "blobs" / (blobDigest + ".bin")) ==
                value.blobs[0].bytes &&
            readBytes(frame / "output.rgba") == value.outputOracle &&
            readBytes(frame / "source.rgba") == value.sourceOracle,
        "publisher preserves event, blob, output, and source bytes");
  check(manifest.find("dxmt9.render_tape.bundle.v2") != std::string::npos &&
            manifest.find("\"production_capture\":true") !=
                std::string::npos && manifest.find(eventDigest) !=
                std::string::npos &&
            manifest.find("\"production_provider_replay\":false") !=
                std::string::npos &&
            manifest.find("\"capture_action_profile\":\"legacy-unknown\"") != std::string::npos &&
            manifest.find("\"action_profile_stable_exact\":false") != std::string::npos &&
            !std::filesystem::exists(frame / "action_profile.bin") &&
            manifest.find("\"output_oracle\":true") !=
                std::string::npos &&
            manifest.find("output.rgba") != std::string::npos &&
            manifest.find(outputDigest) != std::string::npos &&
            manifest.find("\"source_oracle\":true") !=
                std::string::npos &&
            manifest.find("source.rgba") != std::string::npos &&
            manifest.find(sourceDigest) != std::string::npos &&
            manifest.find("blobs/" + blobDigest + ".bin") !=
                std::string::npos,
        "publisher manifest carries digests, paths, and scope");
  check(!std::filesystem::exists(root / ".frame-test.staging"),
        "publisher removes staging after rename");
  check(!dxmt9PePublishRenderTapeBundle(value, root.string(), "frame-test"),
        "publisher rejects a completed-name collision");

  const auto nestedRoot = root / "absent" / "nested";
  check(dxmt9PePublishRenderTapeBundle(value, nestedRoot.string(),
                                       "nested-frame"),
        "publisher creates an absent nested absolute root");
  check(std::filesystem::is_regular_file(nestedRoot / "nested-frame" /
                                         "events.bin") &&
            !std::filesystem::exists(nestedRoot / ".nested-frame.staging"),
        "nested publication is complete with no staging residue");

  const auto sequenceRoot = root / "sequence";
  check(dxmt9PePublishRenderTapeBundle(
            bundle(kRenderTapeProfileSequence), sequenceRoot.string(),
            "sequence-test"),
        "publisher accepts the explicit sequence profile");
  check(readText(sequenceRoot / "sequence-test" / "manifest.json")
                .find("\"profile\":\"sequence-tape\"") !=
            std::string::npos,
        "publisher manifest records the selected sequence profile");

  const OutputRootScope restore;
  const auto defaultRoot = root / "default-sequence";
  setOutputRoot(defaultRoot.string().c_str());
  const auto defaultPublisher = dxmt9PeDefaultRenderTapeArtifactPublisher();
  check(defaultPublisher &&
            defaultPublisher(bundle(kRenderTapeProfileSequence)),
        "default publisher accepts a sealed sequence bundle");
  std::size_t defaultBundles = 0u;
  for (const auto& entry : std::filesystem::directory_iterator(defaultRoot)) {
    check(entry.is_directory() &&
              entry.path().filename().string().starts_with("sequence-"),
          "default publisher uses a sequence-qualified directory name");
    ++defaultBundles;
  }
  check(defaultBundles == 1u,
        "default publisher creates exactly one sequence bundle");
}

void testRejectsInvalidInputsAndSymlinkComponents(
    const std::filesystem::path& root) {
  const auto value = bundle();
  auto invalidDigest = value;
  invalidDigest.blobs[0].digest[0] ^= std::byte{1};
  check(!dxmt9PePublishRenderTapeBundle(invalidDigest, root.string(),
                                        "bad-digest"),
        "publisher rejects digest mismatch before staging");
  check(!std::filesystem::exists(root / "bad-digest"),
        "digest rejection leaves no final bundle");
  auto invalidIdentity = value;
  invalidIdentity.identity = {std::byte{0x01}, std::byte{0x02}};
  check(!dxmt9PePublishRenderTapeBundle(invalidIdentity, root.string(),
                                        "bad-identity"),
        "publisher rejects malformed identity before publication");
  check(!std::filesystem::exists(root / "bad-identity") &&
            !std::filesystem::exists(root / ".bad-identity.staging"),
        "identity rejection cleans staging and leaves no final bundle");
  auto invalidHeader = value;
  invalidHeader.events.resize(3u);
  check(!dxmt9PePublishRenderTapeBundle(invalidHeader, root.string(),
                                        "bad-header"),
        "publisher rejects metadata without a sealed tape header");
  const std::string_view embeddedNul("bad\0name", 8u);
  check(!dxmt9PePublishRenderTapeBundle(value, root.string(), embeddedNul),
        "publisher rejects embedded-NUL frame names");
  check(!dxmt9PePublishRenderTapeBundle(value,
                                        (root / ".." / "unsafe").string(),
                                        "traversal"),
        "publisher rejects traversal in output roots");

  const auto target = root / "symlink-target";
  const auto link = root / "symlink-component";
  std::error_code error;
  std::filesystem::create_directory(target, error);
  check(!error, "publisher test creates symlink target");
  std::filesystem::create_directory_symlink(target, link, error);
  check(!error, "publisher test creates symlink component");
  check(!dxmt9PePublishRenderTapeBundle(value,
                                        (link / "nested").string(),
                                        "symlink"),
        "publisher rejects symlink output-root components");
  check(!std::filesystem::exists(target / "nested"),
        "symlink rejection does not write through component");

  const auto stagingCollision = root / ".staging-collision.staging";
  {
    std::ofstream stream(stagingCollision, std::ios::binary);
    stream << "preexisting";
  }
  check(!dxmt9PePublishRenderTapeBundle(value, root.string(),
                                        "staging-collision"),
        "publisher rejects a preexisting staging collision");
  check(!std::filesystem::exists(root / "staging-collision") &&
            readText(stagingCollision) == "preexisting",
        "staging collision leaves no final or new residue");
}

bool actionProfileMatchesBundle(const RenderTapeActionProfileV1ComponentHeader& profile,
                               const RenderTapePublicationBundle& value) {
  return renderTapeActionProfileV1ComponentMatchesBinding(profile, 7u, 8u, 9u,
      value.events.size(), value.identity.size(), value.executionTopology.size(),
      RenderTapeCaptureSession::sha256(value.events),
      RenderTapeCaptureSession::sha256(value.identity),
      RenderTapeCaptureSession::sha256(value.executionTopology));
}

void testActionProfileComponent(const RenderTapePublicationBundle& value,
                                const std::vector<std::byte>& bytes) {
  RenderTapeActionProfileV1ComponentHeader header;
  check(bytes.size() == kRenderTapeActionProfileV1Bytes &&
            importRenderTapeActionProfileV1Component(bytes, header) &&
            actionProfileMatchesBundle(header, value) && header.witness == value.actionWitness,
        "fixed-size action component authenticates its queue-observed witness against the exact published triple");
  const std::array<std::byte, 8u> magic{std::byte{'D'}, std::byte{'X'}, std::byte{'M'},
      std::byte{'T'}, std::byte{'R'}, std::byte{'A'}, std::byte{'P'}, std::byte{'1'}};
  check(std::equal(magic.begin(), magic.end(), bytes.begin()),
        "action component magic is the little-endian DXMTRAP1 wire contract");
  std::array<std::byte, kRenderTapeActionProfileV1Bytes> encoded;
  check(encodeRenderTapeActionProfileV1Component(header, encoded) &&
            std::equal(encoded.begin(), encoded.end(), bytes.begin()),
        "component import and fixed-length encode preserve every published byte");
  for (std::uint32_t mutation = 0u; mutation < 16u; ++mutation) {
    auto invalid = header;
    if (mutation == 0u) ++invalid.magic;
    if (mutation == 1u) ++invalid.version;
    if (mutation == 2u) ++invalid.headerSize;
    if (mutation == 3u) ++invalid.witness.version;
    if (mutation == 4u) invalid.witness = {};
    if (mutation == 5u) invalid.witness.reserved = 1u;
    if (mutation == 6u) ++invalid.witness.captureToken;
    if (mutation == 7u) ++invalid.witness.captureEpoch;
    if (mutation == 8u) ++invalid.witness.queue;
    if (mutation == 9u) ++invalid.eventsBytes;
    if (mutation == 10u) ++invalid.identityBytes;
    if (mutation == 11u) ++invalid.executionTopologyBytes;
    if (mutation == 12u) invalid.eventsDigest[0] ^= std::byte{1};
    if (mutation == 13u) invalid.identityDigest[0] ^= std::byte{1};
    if (mutation == 14u) invalid.executionTopologyDigest[0] ^= std::byte{1};
    if (mutation == 15u) invalid.witness.freezeRevision = 1u;
    std::array<std::byte, kRenderTapeActionProfileV1Bytes> candidate;
    std::memcpy(candidate.data(), &invalid, sizeof(invalid));
    RenderTapeActionProfileV1ComponentHeader imported;
    check(!importRenderTapeActionProfileV1Component(candidate, imported) ||
              !actionProfileMatchesBundle(imported, value),
        "component admission rejects malformed shape, unknown evidence, wrong keys, revision inconsistency and every triple binding");
  }
  const auto shortBytes = std::span<const std::byte>(bytes).first(bytes.size() - 1u);
  auto longBytes = bytes;
  longBytes.push_back(std::byte{});
  check(!importRenderTapeActionProfileV1Component(shortBytes, header) &&
            !renderTapeActionProfileV1ComponentValid(header) &&
            !importRenderTapeActionProfileV1Component(longBytes, header) &&
            !renderTapeActionProfileV1ComponentValid(header),
        "component importer rejects truncation and trailing bytes without exposing old accepted evidence");
}

void testPairedIdentityV4Publication(const std::filesystem::path& root) {
  const auto fixture = test::makeIdentityV4Fixture();
  RenderTapePublicationBundle value;
  value.events = fixture.tape;
  RenderTapeIdentityV4Status identityStatus{};
  RenderTapeExecutionTopologyV1Status topologyStatus{};
  value.identity = buildRenderTapeIdentityV4(value.events, {}, fixture.identity, &identityStatus);
  value.executionTopology = buildRenderTapeExecutionTopologyV1(value.events, {}, value.identity,
      fixture.topology, &topologyStatus);
  value.actionWitness = {
      .evidence = static_cast<std::uint32_t>(RenderTapeActionWitnessEvidenceV1::QueueObserved),
      .constructionProfile = 1u, .armProfile = 1u, .freezeProfile = 1u,
      .captureToken = 7u, .captureEpoch = 8u, .queue = 9u};
  check(identityStatus == RenderTapeIdentityV4Status::Valid &&
            topologyStatus == RenderTapeExecutionTopologyV1Status::Valid,
        "publication fixture is an authenticated exact neutral-provenance and accepted-topology pair");
  check(dxmt9PePublishRenderTapeBundle(value, root.string(), "identity-v4-pair"),
        "publisher atomically publishes both required v4 components");
  const auto finalPath = root / "identity-v4-pair";
  const auto manifest = readText(finalPath / "manifest.json");
  const auto actionProfileBytes = readBytes(finalPath / "action_profile.bin");
  testActionProfileComponent(value, actionProfileBytes);
  const auto identityDigest = digestText(RenderTapeCaptureSession::sha256(value.identity));
  const auto topologyDigest = digestText(RenderTapeCaptureSession::sha256(value.executionTopology));
  const auto eventDigest = digestText(RenderTapeCaptureSession::sha256(value.events));
  const auto identityDeclaration = "\"identity\":{\"path\":\"identity.bin\",\"schema\":\"" +
      std::string(kRenderTapeIdentityV4Schema) + "\",\"bytes\":" + std::to_string(value.identity.size()) +
      ",\"sha256\":\"" + identityDigest + "\"}";
  const auto topologyDeclaration = "\"execution_topology\":{\"path\":\"execution_topology.bin\",\"schema\":\"" +
      std::string(kRenderTapeExecutionTopologyV1Schema) + "\",\"bytes\":" +
      std::to_string(value.executionTopology.size()) + ",\"sha256\":\"" + topologyDigest + "\"}";
  const auto actionProfileDeclaration = "\"action_profile\":{\"path\":\"action_profile.bin\",\"schema\":\"" +
      std::string(kRenderTapeActionProfileV1Schema) + "\",\"bytes\":200,\"sha256\":\"" +
      digestText(RenderTapeCaptureSession::sha256(actionProfileBytes)) + "\"}";
  check(manifest.find(identityDeclaration) != std::string::npos &&
            manifest.find(topologyDeclaration) != std::string::npos &&
            manifest.find(actionProfileDeclaration) != std::string::npos &&
            manifest.find("\"capture_action_profile\":\"queue-observed\"") != std::string::npos &&
            manifest.find("\"action_profile_stable_exact\":true") != std::string::npos &&
            manifest.find(eventDigest) != std::string::npos &&
            manifest.find("\"production_capture\":true") != std::string::npos &&
            manifest.find("\"production_provider_replay\":false") != std::string::npos &&
            readBytes(finalPath / "identity.bin") == value.identity &&
            readBytes(finalPath / "execution_topology.bin") == value.executionTopology &&
            readBytes(finalPath / "action_profile.bin") == actionProfileBytes &&
            readBytes(finalPath / "events.bin") == value.events &&
            !std::filesystem::exists(root / ".identity-v4-pair.staging"),
        "manifest binds both exact schemas/byte counts/digests while capture attachment does not qualify provider execution");
  check(validateRenderTapeExecutionTopologyV1(readBytes(finalPath / "events.bin"), {},
            readBytes(finalPath / "identity.bin"), readBytes(finalPath / "execution_topology.bin")) ==
              RenderTapeExecutionTopologyV1Status::Valid,
        "published companion retains its one-way full identity/Tape binding");
  check(!dxmt9PePublishRenderTapeBundle(value, root.string(), "identity-v4-pair") &&
            readText(finalPath / "manifest.json") == manifest &&
            readBytes(finalPath / "identity.bin") == value.identity &&
            readBytes(finalPath / "execution_topology.bin") == value.executionTopology &&
            readBytes(finalPath / "action_profile.bin") == actionProfileBytes &&
            !std::filesystem::exists(root / ".identity-v4-pair.staging"),
        "post-write final-name collision rolls back staged pair/witness without altering any committed component");

  auto unknown = value;
  unknown.actionWitness = {};
  check(dxmt9PePublishRenderTapeBundle(unknown, root.string(), "identity-v4-unknown"),
        "legacy v4 pairs retain canonical unknown action evidence without retroactive policy attestation");
  const auto unknownPath = root / "identity-v4-unknown";
  const auto unknownManifest = readText(unknownPath / "manifest.json");
  check(!std::filesystem::exists(unknownPath / "action_profile.bin") &&
            unknownManifest.find("\"action_profile\":{") == std::string::npos &&
            unknownManifest.find("\"capture_action_profile\":\"legacy-unknown\"") != std::string::npos &&
            unknownManifest.find("\"action_profile_stable_exact\":false") != std::string::npos &&
            unknownManifest.find("\"production_provider_replay\":false") != std::string::npos &&
            readBytes(unknownPath / "events.bin") == value.events &&
            readBytes(unknownPath / "identity.bin") == value.identity &&
            readBytes(unknownPath / "execution_topology.bin") == value.executionTopology,
        "unknown policy omits authoritative component and cannot promote capture/provider qualification or alter the legacy triple");

  for (std::uint32_t mode = 0u; mode < 4u; ++mode) {
    auto changed = value;
    auto& witness = changed.actionWitness;
    if (mode == 0u) witness.constructionProfile = witness.armProfile = witness.freezeProfile = 0u;
    if (mode == 0u) {
      const auto& base = fixture.identity.header;
      RenderTapeAttachmentHistoryHeader history{.token = 7u, .epoch = 8u, .queue = 9u,
          .completedSequence = base.baselineCompletedSequence,
          .completedInitializer = base.baselineCompletedInitializer};
      check(encodeRenderTapeAttachmentHistory(history, {}, changed.attachmentHistory),
          "Normal publisher fixture carries explicit empty observed history");
    }
    if (mode == 1u) {
      witness.constructionProfile = 0u;
      witness.armRevision = witness.freezeRevision = 1u;
    }
    if (mode == 2u) witness.armRevision = witness.freezeRevision = 2u;
    if (mode == 3u) {
      witness.freezeProfile = 0u;
      witness.freezeRevision = 1u;
    }
    const auto name = "identity-v4-policy-observed-" + std::to_string(mode);
    check(renderTapeActionWitnessV1Valid(witness) && !renderTapeActionWitnessV1StableExact(witness) &&
              dxmt9PePublishRenderTapeBundle(changed, root.string(), name),
        "Normal, pre-arm policy change, ABA and in-interval change remain valid queue observations but not stable exact evidence");
    const auto changedManifest = readText(root / name / "manifest.json");
    if (mode == 0u) {
      check(readBytes(root / name / "attachment_history.bin") == changed.attachmentHistory &&
          changedManifest.find("\"attachment_history\"") != std::string::npos &&
          changedManifest.find("\"events_sha256\"") != std::string::npos &&
          changedManifest.find("\"action_profile_sha256\"") != std::string::npos,
          "published history owns bytes and binds all companion digests");
      changed.attachmentHistory[16] ^= std::byte{1};
      check(!dxmt9PePublishRenderTapeBundle(changed, root.string(), "history-mismatch") &&
          !std::filesystem::exists(root / "history-mismatch"),
          "mismatched original history cannot publish");
    }
    RenderTapeActionProfileV1ComponentHeader profile;
    check(importRenderTapeActionProfileV1Component(readBytes(root / name / "action_profile.bin"), profile) &&
              profile.witness == witness && actionProfileMatchesBundle(profile, changed) &&
              changedManifest.find("\"capture_action_profile\":\"queue-observed\"") != std::string::npos &&
              changedManifest.find("\"action_profile_stable_exact\":false") != std::string::npos &&
              changedManifest.find("\"production_provider_replay\":false") != std::string::npos,
        "published observed policy preserves revisions and derives non-promotion from evidence rather than a manifest flag");
  }

  for (std::uint32_t mutation = 0u; mutation < 12u; ++mutation) {
    auto invalid = value;
    RenderTapeExecutionTopologyV1Header topology{};
    std::memcpy(&topology, invalid.executionTopology.data(), sizeof(topology));
    if (mutation == 0u) invalid.identity[40] ^= std::byte{1};
    if (mutation == 1u) topology.identityDigest[0] ^= std::byte{1};
    if (mutation == 2u) topology.eventsDigest[0] ^= std::byte{1};
    if (mutation == 3u) ++topology.identityBytes;
    if (mutation == 4u) ++topology.eventsBytes;
    if (mutation == 5u) ++topology.captureToken;
    if (mutation == 6u) ++topology.captureEpoch;
    if (mutation == 7u) ++topology.queue;
    if (mutation == 8u) topology.version = 2u;
    if (mutation == 9u) topology.placements.offset = topology.plans.offset;
    std::memcpy(invalid.executionTopology.data(), &topology, sizeof(topology));
    if (mutation == 10u) invalid.executionTopology.clear();
    if (mutation == 11u) invalid.identity.clear();
    const auto name = "identity-v4-pair-reject-" + std::to_string(mutation);
    check(!dxmt9PePublishRenderTapeBundle(invalid, root.string(), name) &&
              !std::filesystem::exists(root / name) &&
              !std::filesystem::exists(root / ("." + name + ".staging")),
          "tamper, cross-pair binding, malformed topology, missing companion and orphan topology publish neither half");
  }
  for (std::uint32_t mutation = 0u; mutation < 15u; ++mutation) {
    auto invalid = value;
    auto& witness = invalid.actionWitness;
    if (mutation == 0u) ++witness.version;
    if (mutation == 1u) witness.evidence = 2u;
    if (mutation == 2u) witness.constructionProfile = 2u;
    if (mutation == 3u) witness.armProfile = 2u;
    if (mutation == 4u) witness.freezeProfile = 2u;
    if (mutation == 5u) witness.reserved = 1u;
    if (mutation == 6u) ++witness.captureToken;
    if (mutation == 7u) ++witness.captureEpoch;
    if (mutation == 8u) ++witness.queue;
    if (mutation == 9u) witness.armRevision = 1u;
    if (mutation == 10u) witness.freezeProfile = 0u;
    if (mutation == 11u) witness.armRevision = witness.freezeRevision = 1u;
    if (mutation == 12u) witness.freezeRevision = 1u;
    if (mutation == 13u) witness.evidence = 0u;
    if (mutation == 14u) {
      witness = {};
      witness.captureToken = 7u;
    }
    const auto name = "identity-v4-witness-reject-" + std::to_string(mutation);
    check(!dxmt9PePublishRenderTapeBundle(invalid, root.string(), name) &&
              !std::filesystem::exists(root / name) &&
              !std::filesystem::exists(root / ("." + name + ".staging")),
        "malformed, stale-key, noncanonical unknown and revision-inconsistent witness cannot publish any component");
  }
  auto orphan = bundle();
  orphan.actionWitness = value.actionWitness;
  check(!dxmt9PePublishRenderTapeBundle(orphan, root.string(), "orphan-action-witness") &&
            !std::filesystem::exists(root / "orphan-action-witness") &&
            !std::filesystem::exists(root / ".orphan-action-witness.staging"),
        "observed witness without the actual V4 identity/topology triple has no publication authority");
  std::vector<RenderTapeIdentitySource> sources;
  std::vector<RenderTapeIdentityRange> ranges;
  for (std::uint32_t index = 0u; index < 3u; ++index) {
    sources.push_back({.eventOrdinal = index + 3u, .sourceOrdinal = index + 1u,
        .seqId = index + 1u, .captureToken = 7u, .recordCount = 1u,
        .firstRange = index, .rangeCount = 1u});
    ranges.push_back({.eventOrdinal = index + 3u, .sourceOrdinal = index + 1u,
        .seqId = index + 1u, .logicalPassId = index + 1u, .recordCount = 1u,
        .dagPassIndex = index, .passKind = index == 2u ? 3u : 1u});
  }
  auto older = value;
  older.identity = buildRenderTapeIdentity(older.events, {}, 1u, 5u, 7u,
      RenderTapeIdentityAuthority::Capture, sources, ranges);
  check(!older.identity.empty() && validateRenderTapeIdentity(older.events, {}, older.identity).valid(),
        "orphan topology older-version negative uses a genuinely admitted v2 identity, not malformed bytes");
  check(!dxmt9PePublishRenderTapeBundle(older, root.string(), "identity-v2-orphan-topology") &&
            !std::filesystem::exists(root / "identity-v2-orphan-topology") &&
            !std::filesystem::exists(root / ".identity-v2-orphan-topology.staging"),
        "older identity versions cannot acquire a v4 execution companion");
  older.executionTopology.clear();
  check(!dxmt9PePublishRenderTapeBundle(older, root.string(), "identity-v2-orphan-witness") &&
            !std::filesystem::exists(root / "identity-v2-orphan-witness") &&
            !std::filesystem::exists(root / ".identity-v2-orphan-witness.staging"),
        "valid older identities cannot carry queue action authority from a V4 binding");
  older.actionWitness = {};
  check(dxmt9PePublishRenderTapeBundle(older, root.string(), "identity-v2-unknown") &&
            !std::filesystem::exists(root / "identity-v2-unknown" / "action_profile.bin") &&
            readText(root / "identity-v2-unknown" / "manifest.json")
                .find("\"capture_action_profile\":\"legacy-unknown\"") != std::string::npos,
        "authentic older fixtures continue to publish only legacy-unknown policy evidence");

  const auto outputRoot = root.string();
  const auto probeName = std::string("identity-v4-allocation-probe");
  const auto probeFinal = root / probeName;
  const auto probeStaging = root / ("." + probeName + ".staging");
  publisher_pair_allocation_probe::count = 0u;
  publisher_pair_allocation_probe::enabled = true;
  const bool admitted = dxmt9PePublishRenderTapeBundle(value, outputRoot, probeName);
  publisher_pair_allocation_probe::enabled = false;
  const auto allocations = publisher_pair_allocation_probe::count;
  check(admitted && allocations > 0u, "pair publication allocation probe includes validation, staging and manifest construction");
  check(readText(probeFinal / "manifest.json") == manifest &&
            readBytes(probeFinal / "events.bin") == value.events &&
            readBytes(probeFinal / "identity.bin") == value.identity &&
            readBytes(probeFinal / "execution_topology.bin") == value.executionTopology &&
            readBytes(probeFinal / "action_profile.bin") == actionProfileBytes &&
            !std::filesystem::exists(probeStaging),
        "successful allocation baseline publishes the full exact pair, witness component and manifest");
  std::error_code error;
  std::filesystem::remove_all(probeFinal, error);
  check(!error, "allocation test removes its own fully committed probe before fault injection");
  std::size_t rejected = 0u;
  for (std::size_t allocation = 0u; allocation < allocations; ++allocation) {
    publisher_pair_allocation_probe::failAfter = allocation;
    publisher_pair_allocation_probe::failEnabled = true;
    const bool published = dxmt9PePublishRenderTapeBundle(value, outputRoot, probeName);
    publisher_pair_allocation_probe::failEnabled = false;
    if (published) {
      // An optional library allocation may recover locally. Recovery still
      // requires the entire validated pair and exact manifest, never one half.
      check(readText(probeFinal / "manifest.json") == manifest &&
                readBytes(probeFinal / "events.bin") == value.events &&
                readBytes(probeFinal / "identity.bin") == value.identity &&
                readBytes(probeFinal / "execution_topology.bin") == value.executionTopology &&
                readBytes(probeFinal / "action_profile.bin") == actionProfileBytes &&
                !std::filesystem::exists(probeStaging),
            "any recovered allocation publishes the full exact pair and witness, not partial evidence");
      std::filesystem::remove_all(probeFinal, error);
      check(!error, "allocation test cleans only its own complete recovered probe");
    } else {
      ++rejected;
      check(!std::filesystem::exists(probeFinal) && !std::filesystem::exists(probeStaging),
            "validation/path/write/manifest allocation failure leaves no final or partial pair staging");
    }
  }
  check(rejected != 0u, "allocation probe observes nonvacuous fail-closed paired-publication rejection");
}

} // namespace

int main(int argc, char** argv) {
  std::filesystem::path root;
  std::error_code error;
  try {
    check(testRoot(root), "publisher test resolves isolated root");
    std::filesystem::create_directory(root, error);
    check(!error, "publisher test creates isolated root");
    testOutputRootTruthTable(root);
    testTransactionalSuccessAndCollision(root);
    testRejectsInvalidInputsAndSymlinkComponents(root);
    testPairedIdentityV4Publication(root);
    if (argc == 2) {
      const std::filesystem::path fixtures(argv[1]);
      for (const auto version : {2u, 3u}) {
        RenderTapePublicationBundle input;
        input.events = readBytes(fixtures / "events.bin");
        input.identity = readBytes(fixtures /
            (version == 3u ? "identity.bin" : "identity-v2.bin"));
        const auto name = "identity-v" + std::to_string(version);
        check(dxmt9PePublishRenderTapeBundle(input, root.string(), name),
              "publisher accepts authenticated version-specific identity");
        const auto manifest = readText(root / name / "manifest.json");
        check(manifest.find("dxmt9.render_tape.identity.v" +
                  std::to_string(version)) != std::string::npos,
              "manifest reports actual sidecar version");
        check(readBytes(root / name / "identity.bin") == input.identity,
              "publication retains exact identity bytes");
        input.executionTopology = {std::byte{1}};
        check(!dxmt9PePublishRenderTapeBundle(input, root.string(), name + "-orphan") &&
                  !std::filesystem::exists(root / (name + "-orphan")) &&
                  !std::filesystem::exists(root / ("." + name + "-orphan.staging")),
              "both admitted v2 and v3 fixture identities reject an orphan topology companion");
        input.executionTopology.clear();
        input.identity[40] ^= std::byte{1};
        check(!dxmt9PePublishRenderTapeBundle(input, root.string(), name + "-bad"),
              "publisher rejects identity bound to different tape bytes");
        check(!std::filesystem::exists(root / (name + "-bad")) &&
                  !std::filesystem::exists(root / ("." + name + "-bad.staging")),
              "invalid identity leaves neither final nor staged artifact");
      }
    } else {
      check(argc == 1, "publisher spec accepts at most one fixture directory");
    }
    std::filesystem::remove_all(root, error);
    check(!error, "publisher test cleans isolated root");
    return 0;
  } catch (const TestFailure& failure) {
    std::cerr << failure.what() << '\n';
    if (!root.empty()) {
      std::filesystem::remove_all(root, error);
    }
    return 1;
  } catch (...) {
    if (!root.empty()) {
      std::filesystem::remove_all(root, error);
    }
    return 1;
  }
}
