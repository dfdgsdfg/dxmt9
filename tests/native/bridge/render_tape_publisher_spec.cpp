#include "d3d9_pe_render_tape_publisher.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
  }
  check(stream.good() || stream.eof(), "publisher test reads bytes fully");
  return bytes;
}

RenderTapePublicationBundle bundle() {
  RenderTapePublicationBundle value{};
  value.events = {std::byte{1}, std::byte{2}, std::byte{3}};
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
  const auto value = bundle();
  check(dxmt9PePublishRenderTapeBundle(value, root.string(), "frame-test"),
        "publisher commits a complete bundle");
  const auto frame = root / "frame-test";
  check(std::filesystem::is_regular_file(frame / "events.bin"),
        "publisher commits events.bin");
  check(std::filesystem::is_regular_file(frame / "manifest.json"),
        "publisher commits manifest");
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
  check(readBytes(frame / "events.bin") == value.events &&
            readBytes(frame / "blobs" / (blobDigest + ".bin")) ==
                value.blobs[0].bytes,
        "publisher preserves event and blob bytes");
  check(manifest.find("dxmt9.render_tape.bundle.v2") != std::string::npos &&
            manifest.find("\"production_capture\":true") !=
                std::string::npos && manifest.find(eventDigest) !=
                std::string::npos &&
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

} // namespace

int main() {
  std::filesystem::path root;
  std::error_code error;
  try {
    check(testRoot(root), "publisher test resolves isolated root");
    std::filesystem::create_directory(root, error);
    check(!error, "publisher test creates isolated root");
    testOutputRootTruthTable(root);
    testTransactionalSuccessAndCollision(root);
    testRejectsInvalidInputsAndSymlinkComponents(root);
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
