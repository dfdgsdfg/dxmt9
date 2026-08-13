#include "d3d9_pe_render_tape_publisher.hpp"

#include "util/config/config.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <span>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

using dxmt9::d3d9::RenderTapeCaptureSession;
using dxmt9::d3d9::RenderTapeDigest;
using dxmt9::d3d9::RenderTapePublicationBundle;

constexpr std::string_view kOutputRootVariable =
    "DXMT9_RENDER_TAPE_OUTPUT_ROOT";
constexpr std::string_view kBundleSchema = "dxmt9.render_tape.bundle.v2";

std::string outputRoot() {
  return dxmt9::util::getenvString(kOutputRootVariable.data());
}

bool safeRootText(std::string_view text, std::filesystem::path& root) {
  if (text.empty() || text.find('\0') != std::string_view::npos) {
    return false;
  }
  root = std::filesystem::path(std::string(text));
  if (!root.is_absolute()) {
    return false;
  }
  for (const auto& component : root) {
    if (component == "." || component == "..") {
      return false;
    }
  }
  return true;
}

bool safeFrameName(std::string_view name) {
  if (name.empty() || name == "." || name == ".." ||
      name.find('/') != std::string_view::npos ||
      name.find('\\') != std::string_view::npos) {
    return false;
  }
  return name.find('\0') == std::string_view::npos;
}

bool existingPathIsSafeDirectory(const std::filesystem::path& path) {
  std::filesystem::path current;
  bool missingAncestor = false;
  for (const auto& component : path) {
    // Windows exposes the volume (for example, "C:") as a root-name
    // component rather than as a directory path on its own.
    if (component == path.root_name()) {
      current = component;
      continue;
    }
    current /= component;
    if (missingAncestor) {
      continue;
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(current, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && status.type() == std::filesystem::file_type::not_found)) {
      error.clear();
      missingAncestor = true;
      continue;
    }
    if (error || status.type() == std::filesystem::file_type::symlink ||
        !std::filesystem::is_directory(status)) {
      return false;
    }
  }
  return true;
}

bool flushAndCloseFile(const std::filesystem::path& path,
                       std::ofstream& stream) noexcept {
  stream.flush();
  if (!stream.good()) {
    return false;
  }
  stream.close();
  if (stream.fail()) {
    return false;
  }

#if defined(_WIN32)
  const int descriptor = _wopen(path.c_str(), _O_RDONLY | _O_BINARY);
  if (descriptor < 0) {
    return false;
  }
  const int committed = _commit(descriptor);
  const int closed = _close(descriptor);
  return committed == 0 && closed == 0;
#else
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    return false;
  }
  const int synced = ::fsync(descriptor);
  const int closed = ::close(descriptor);
  return synced == 0 && closed == 0;
#endif
}

bool syncDirectory(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
  (void)path;
  return true;
#else
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (descriptor < 0) {
    return false;
  }
  const int synced = ::fsync(descriptor);
  const int closed = ::close(descriptor);
  return synced == 0 && closed == 0;
#endif
}

std::string hexDigest(const RenderTapeDigest& digest) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (const auto value : digest) {
    stream << std::setw(2)
           << static_cast<unsigned>(std::to_integer<unsigned char>(value));
  }
  return stream.str();
}

bool writeBytes(const std::filesystem::path& path,
                std::span<const std::byte> bytes) noexcept {
  std::ofstream stream(path, std::ios::binary | std::ios::out | std::ios::trunc);
  if (!stream) {
    return false;
  }
  if (!bytes.empty()) {
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  return flushAndCloseFile(path, stream);
}

bool writeText(const std::filesystem::path& path,
               std::string_view text) noexcept {
  std::ofstream stream(path, std::ios::binary | std::ios::out | std::ios::trunc);
  if (!stream) {
    return false;
  }
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  return flushAndCloseFile(path, stream);
}

std::string manifestFor(const RenderTapePublicationBundle& bundle,
                        std::string_view eventDigest,
                        std::span<const std::string> blobDigests) {
  std::ostringstream manifest;
  manifest << "{\n"
           << "  \"schema\":\"" << kBundleSchema << "\",\n"
           << "  \"profile\":\"frame-tape\",\n"
           << "  \"producer\":{\"path\":\"dxmt9-pe-render-tape\","
              "\"git_revision\":\"runtime\"},\n"
           << "  \"stage\":\"production-capture\",\n"
           << "  \"domain\":\"replay\",\n"
           << "  \"validity\":{\"structural\":true,"
              "\"reference_replay\":false},\n"
           << "  \"components\":{\n"
           << "    \"events\":{\"path\":\"events.bin\",\"bytes\":"
           << bundle.events.size() << ",\"sha256\":\"" << eventDigest
           << "\"},\n"
           << "    \"blobs\":[";
  for (std::size_t i = 0u; i < bundle.blobs.size(); ++i) {
    if (i != 0u) {
      manifest << ',';
    }
    manifest << "{\"path\":\"blobs/" << blobDigests[i]
             << ".bin\",\"bytes\":" << bundle.blobs[i].bytes.size()
             << ",\"sha256\":\"" << blobDigests[i] << "\"}";
  }
  manifest << "]\n  },\n"
           << "  \"scope\":{\"production_capture\":true,"
              "\"production_provider_replay\":false,"
              "\"output_oracle\":false}\n"
           << "}\n";
  return manifest.str();
}

std::atomic<std::uint64_t> nextFrameId{1u};

bool publishDefault(const RenderTapePublicationBundle& bundle) noexcept {
  const auto root = outputRoot();
  if (root.empty()) {
    return false;
  }
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto ticks = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  const auto id = nextFrameId.fetch_add(1u, std::memory_order_relaxed);
  std::ostringstream name;
  name << "frame-" << ticks << '-' << id;
  return dxmt9PePublishRenderTapeBundle(bundle, root, name.str());
}

} // namespace

bool dxmt9PePublishRenderTapeBundle(const RenderTapePublicationBundle& bundle,
                                    std::string_view outputRootText,
                                    std::string_view frameName) noexcept {
  try {
    std::filesystem::path root;
    if (!safeRootText(outputRootText, root) || !safeFrameName(frameName) ||
        bundle.events.empty()) {
      return false;
    }

    std::error_code error;
    if (!existingPathIsSafeDirectory(root)) {
      return false;
    }
    std::filesystem::create_directories(root, error);
    if (error || !existingPathIsSafeDirectory(root)) {
      return false;
    }

    std::unordered_set<std::string> seen;
    std::vector<std::string> blobDigests;
    blobDigests.reserve(bundle.blobs.size());
    for (const auto& blob : bundle.blobs) {
      const auto actualDigest =
          hexDigest(RenderTapeCaptureSession::sha256(blob.bytes));
      const auto declaredDigest = hexDigest(blob.digest);
      if (declaredDigest != actualDigest || !seen.insert(actualDigest).second) {
        return false;
      }
      blobDigests.push_back(actualDigest);
    }
    const auto eventDigest =
        hexDigest(RenderTapeCaptureSession::sha256(bundle.events));

    const auto finalPath = root / std::filesystem::path(std::string(frameName));
    if (!existingPathIsSafeDirectory(finalPath)) {
      return false;
    }
    const auto stagingPath = root / ("." + std::string(frameName) + ".staging");
    if (!existingPathIsSafeDirectory(stagingPath)) {
      return false;
    }
    if (!std::filesystem::create_directory(stagingPath, error) || error) {
      return false;
    }

    const auto fail = [&]() noexcept {
      std::error_code cleanupError;
      std::filesystem::remove_all(stagingPath, cleanupError);
      return false;
    };
    const auto blobsPath = stagingPath / "blobs";
    if (!std::filesystem::create_directory(blobsPath, error) || error ||
        !writeBytes(stagingPath / "events.bin", bundle.events)) {
      return fail();
    }
    for (std::size_t i = 0u; i < bundle.blobs.size(); ++i) {
      const auto blobPath = blobsPath / (blobDigests[i] + ".bin");
      if (!writeBytes(blobPath, bundle.blobs[i].bytes)) {
        return fail();
      }
    }
    const auto manifest = manifestFor(bundle, eventDigest, blobDigests);
    if (!writeText(stagingPath / "manifest.json", manifest) ||
        !syncDirectory(blobsPath) || !syncDirectory(stagingPath)) {
      return fail();
    }
    if (std::filesystem::exists(finalPath, error) || error) {
      return fail();
    }
    std::filesystem::rename(stagingPath, finalPath, error);
    if (error) {
      return fail();
    }
    // The final directory is complete and claimable. Parent-directory fsync is
    // best effort on filesystems/platforms that do not expose it; do not
    // report failure after the atomic rename has made the artifact visible.
    (void)syncDirectory(root);
    return true;
  } catch (...) {
    return false;
  }
}

bool dxmt9PeRenderTapeOutputConfigured() noexcept {
  std::filesystem::path root;
  const auto text = outputRoot();
  return safeRootText(text, root) && existingPathIsSafeDirectory(root);
}

D3D9PeRenderTapeArtifactPublisher
dxmt9PeDefaultRenderTapeArtifactPublisher() noexcept {
  return dxmt9PeRenderTapeOutputConfigured() ? &publishDefault : nullptr;
}
