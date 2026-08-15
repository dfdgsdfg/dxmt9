#include "d3d9_pe_render_tape_publisher.hpp"
#include "device_c_render_tape_identity.hpp"

#include "util/config/config.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <span>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "util/log/log.hpp"

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

void publisherInfoLog(const char *fmt, ...) noexcept {
  va_list args;
  va_start(args, fmt);
  dxmt9::util::vlogf(dxmt9::util::LogLevel::Info, "dxmt9-device", fmt, args);
  va_end(args);
}

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
  // `_commit` requires a writable descriptor under the Wine/Windows CRT.
  // Keep the close/rename ordering unchanged; only the descriptor mode
  // changes so durable flush remains part of the pre-rename transaction.
  const int descriptor = _wopen(path.c_str(), _O_RDWR | _O_BINARY);
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

std::uint32_t sealedProfile(std::span<const std::byte> events) noexcept {
  if (events.size() < sizeof(dxmt9::d3d9::RenderTapeHeader)) {
    return 0u;
  }
  dxmt9::d3d9::RenderTapeHeader header{};
  std::memcpy(&header, events.data(), sizeof(header));
  if (header.magic != dxmt9::d3d9::kRenderTapeMagic ||
      header.version != dxmt9::d3d9::kRenderTapeVersion ||
      header.headerSize != sizeof(header) ||
      (header.profile != dxmt9::d3d9::kRenderTapeProfileFrame &&
       header.profile != dxmt9::d3d9::kRenderTapeProfileSequence)) {
    return 0u;
  }
  return header.profile;
}

std::string_view profileName(std::uint32_t profile) noexcept {
  return profile == dxmt9::d3d9::kRenderTapeProfileSequence
             ? "sequence-tape"
             : "frame-tape";
}

std::string_view profilePrefix(std::uint32_t profile) noexcept {
  return profile == dxmt9::d3d9::kRenderTapeProfileSequence ? "sequence"
                                                             : "frame";
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
                        std::span<const std::string> blobDigests,
                        std::string_view identityDigest,
                        std::string_view outputOracleDigest) {
  const auto profile = sealedProfile(bundle.events);
  std::ostringstream manifest;
  manifest << "{\n"
           << "  \"schema\":\"" << kBundleSchema << "\",\n"
           << "  \"profile\":\"" << profileName(profile) << "\",\n"
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
  manifest << ']';
  if (!bundle.identity.empty()) {
    manifest << ",\n    \"identity\":{\"path\":\"identity.bin\","
                "\"schema\":\"dxmt9.render_tape.identity.v1\",\"bytes\":"
             << bundle.identity.size() << ",\"sha256\":\""
             << identityDigest << "\"}";
  }
  if (!bundle.outputOracle.empty()) {
    manifest << ",\n    \"output_oracle\":{\"path\":\"output.rgba\",\"bytes\":"
             << bundle.outputOracle.size() << ",\"sha256\":\""
             << outputOracleDigest << "\"}";
  }
  manifest << "\n  },\n"
           << "  \"scope\":{\"production_capture\":true,"
              "\"production_provider_replay\":false,"
              "\"output_oracle\":"
           << (!bundle.outputOracle.empty() ? "true" : "false") << "}\n"
           << "}\n";
  return manifest.str();
}

std::atomic<std::uint64_t> nextTapeId{1u};

bool publishDefault(const RenderTapePublicationBundle& bundle) noexcept {
  const auto root = outputRoot();
  if (root.empty()) {
    publisherInfoLog("render_tape_capture publisher rejected reason=root_empty");
    return false;
  }
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto ticks = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  const auto profile = sealedProfile(bundle.events);
  if (profile == 0u) {
    publisherInfoLog(
        "render_tape_capture publisher rejected reason=invalid_header");
    return false;
  }
  const auto id = nextTapeId.fetch_add(1u, std::memory_order_relaxed);
  std::ostringstream name;
  name << profilePrefix(profile) << '-' << ticks << '-' << id;
  publisherInfoLog(
      "render_tape_capture publisher attempt root=%s tape=%s events=%zu blobs=%zu",
      root.c_str(), name.str().c_str(), bundle.events.size(),
      bundle.blobs.size());
  return dxmt9PePublishRenderTapeBundle(bundle, root, name.str());
}

} // namespace

bool dxmt9PePublishRenderTapeBundle(const RenderTapePublicationBundle& bundle,
                                    std::string_view outputRootText,
                                    std::string_view frameName) noexcept {
  try {
    const auto reject = [&](const char *reason) noexcept {
      publisherInfoLog(
          "render_tape_capture publisher rejected reason=%s root=%.*s frame=%.*s",
          reason, static_cast<int>(outputRootText.size()), outputRootText.data(),
          static_cast<int>(frameName.size()), frameName.data());
      return false;
    };
    std::filesystem::path root;
    if (!safeRootText(outputRootText, root) || !safeFrameName(frameName) ||
        sealedProfile(bundle.events) == 0u) {
      return reject("input");
    }

    std::error_code error;
    if (!existingPathIsSafeDirectory(root)) {
      return reject("root_not_safe_directory");
    }
    std::filesystem::create_directories(root, error);
    if (error || !existingPathIsSafeDirectory(root)) {
      return reject("root_create_or_safety");
    }

    std::unordered_set<std::string> seen;
    std::vector<std::string> blobDigests;
    blobDigests.reserve(bundle.blobs.size());
    for (const auto& blob : bundle.blobs) {
      const auto actualDigest =
          hexDigest(RenderTapeCaptureSession::sha256(blob.bytes));
      const auto declaredDigest = hexDigest(blob.digest);
      if (declaredDigest != actualDigest || !seen.insert(actualDigest).second) {
        return reject("blob_digest");
      }
      blobDigests.push_back(actualDigest);
    }
    const auto eventDigest =
        hexDigest(RenderTapeCaptureSession::sha256(bundle.events));
    const auto identityDigest = bundle.identity.empty()
        ? std::string{}
        : hexDigest(RenderTapeCaptureSession::sha256(bundle.identity));
    const auto outputOracleDigest = bundle.outputOracle.empty()
        ? std::string{}
        : hexDigest(RenderTapeCaptureSession::sha256(bundle.outputOracle));

    const auto finalPath = root / std::filesystem::path(std::string(frameName));
    if (!existingPathIsSafeDirectory(finalPath)) {
      return reject("final_not_safe_directory");
    }
    const auto stagingPath = root / ("." + std::string(frameName) + ".staging");
    if (!existingPathIsSafeDirectory(stagingPath)) {
      return reject("staging_not_safe_directory");
    }
    if (!std::filesystem::create_directory(stagingPath, error) || error) {
      return reject("staging_create");
    }

    const auto fail = [&](const char *reason) noexcept {
      publisherInfoLog(
          "render_tape_capture publisher rejected reason=%s root=%.*s frame=%.*s",
          reason, static_cast<int>(outputRootText.size()), outputRootText.data(),
          static_cast<int>(frameName.size()), frameName.data());
      std::error_code cleanupError;
      std::filesystem::remove_all(stagingPath, cleanupError);
      return false;
    };
    const auto blobsPath = stagingPath / "blobs";
    if (!std::filesystem::create_directory(blobsPath, error) || error) {
      return fail("blobs_create");
    }
    if (!writeBytes(stagingPath / "events.bin", bundle.events)) {
      return fail("events_write_or_sync");
    }
    for (std::size_t i = 0u; i < bundle.blobs.size(); ++i) {
      const auto blobPath = blobsPath / (blobDigests[i] + ".bin");
      if (!writeBytes(blobPath, bundle.blobs[i].bytes)) {
        return fail("blob_write_or_sync");
      }
    }
    if (!bundle.outputOracle.empty() &&
        !writeBytes(stagingPath / "output.rgba", bundle.outputOracle)) {
      return fail("output_oracle_write_or_sync");
    }
    if (!bundle.identity.empty()) {
      dxmt9::d3d9::RenderTapeBlobCatalogue catalogue;
      for (std::size_t i = 0u; i < bundle.blobs.size(); ++i) {
        catalogue.blobs.push_back(dxmt9::d3d9::RenderTapeBlob{
            .digest = bundle.blobs[i].digest,
            .size = bundle.blobs[i].bytes.size(),
            .verified = 1u,
        });
      }
      if (!dxmt9::d3d9::validateRenderTapeIdentity(
               bundle.events, catalogue, bundle.identity).valid() ||
          !writeBytes(stagingPath / "identity.bin", bundle.identity)) {
        return fail("identity_validate_write_or_sync");
      }
    }
    const auto manifest = manifestFor(
        bundle, eventDigest, blobDigests, identityDigest, outputOracleDigest);
    if (!writeText(stagingPath / "manifest.json", manifest)) {
      return fail("manifest_write_or_sync");
    }
    if (!syncDirectory(blobsPath)) {
      return fail("blobs_directory_sync");
    }
    if (!syncDirectory(stagingPath)) {
      return fail("staging_directory_sync");
    }
    if (std::filesystem::exists(finalPath, error) || error) {
      return fail("final_exists");
    }
    std::filesystem::rename(stagingPath, finalPath, error);
    if (error) {
      return fail("final_rename");
    }
    // The final directory is complete and claimable. Parent-directory fsync is
    // best effort on filesystems/platforms that do not expose it; do not
    // report failure after the atomic rename has made the artifact visible.
    (void)syncDirectory(root);
    return true;
  } catch (...) {
    publisherInfoLog("render_tape_capture publisher rejected reason=exception");
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
