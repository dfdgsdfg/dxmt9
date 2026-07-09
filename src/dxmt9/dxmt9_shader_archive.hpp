#pragma once

// Shader-cache / binary-archive service. Holds the WMT::BinaryArchive
// reference + on-disk cache path, and persists the archive on
// destruction. Previously the persist step lived inside
// CommandQueue::~CommandQueue — moved here so the execution service
// (CommandQueue) is free of persistence responsibility.
//
// R-BACK-3.9 / R-BACK-3.10 / R-BACK-3.11 extended this class with:
//   * beginAsyncFullLoad() — the non-blocking Full-mode prewarm entry
//     point. The device-init-synchronous ctor below (Archive(device,
//     path)) is unchanged and still used verbatim for Lazy/Disabled
//     mode, per the "device init behavior with DXMT9_PREWARM=disabled|
//     lazy unchanged" constraint.
//   * notePresent() — the R-BACK-3.10 mid-session milestone save, called
//     once per Present from CommandQueue::submitPresent(). Cheap when no
//     save is due.
//   * poisonSave() — the R-BACK-3.11 diagnostic-variant pollution guard.
//
// Thread-safety: `mutex_` guards `ref_` (the WMT::Reference written once
// by the async-load completion callback), the milestone tracker, and
// `savePoisoned_`. It does NOT guard the ~50 pre-existing call sites
// across the codebase that read `reference()`/`&archive->reference()`
// directly (dxmt9_pipeline_cache.cpp, dxmt9_draw_encoder.hpp,
// dxmt9_blit_encoders.*, dxmt9_presenter.hpp, dxmt9_device.cpp) — see the
// thread-safety note in dxmt9_archive_prewarm.hpp for why that specific,
// bounded, one-time-transition race is accepted rather than retrofitted.

#include "dxmt9_archive_prewarm.hpp"
#include "../winemetal/Metal.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace dxmt9::shaders {

class Archive {
 public:
  Archive() = default;
  // Opens the archive at `path` for the given device. On success,
  // reference() is non-null. On failure (null device, missing/malformed
  // archive), remains empty — valid() returns false and persist is a
  // no-op. Synchronous — unchanged behavior, used for Mode::Lazy /
  // Mode::Disabled (R-BACK-3.9 does not change those modes).
  Archive(WMT::Device device, std::string path);
  ~Archive();

  Archive(const Archive&) = delete;
  Archive& operator=(const Archive&) = delete;
  // Non-movable: mutex_/loadThread_ make a correct move nontrivial, and
  // nothing in the codebase moves an Archive (it is always a direct
  // CommandQueue member, constructed in place via the mem-init list;
  // grep confirmed no move-assignment call sites exist).
  Archive(Archive&&) = delete;
  Archive& operator=(Archive&&) = delete;

  bool valid() const noexcept { return static_cast<bool>(ref_); }
  WMT::Reference<WMT::BinaryArchive>& reference() noexcept { return ref_; }
  const std::string& path() const noexcept { return path_; }

  // R-BACK-3.9 — begin the async Full-mode load. Sets path() to `path`
  // (the default ctor above leaves it empty). No-op (besides setting
  // path()) when `path` is empty. Must be called at most once, and only
  // on a default-constructed Archive. The background thread this starts
  // is joined in ~Archive(), which may block briefly at shutdown if the
  // load is still in flight — an accepted trade-off since R-BACK-3.9
  // only requires device-init, not shutdown, to stay off the I/O path.
  void beginAsyncFullLoad(WMT::Device device, std::string path);

  // R-BACK-3.10 — call once per Present (CommandQueue::submitPresent()).
  // Advances the milestone tracker and performs the bounded mid-session
  // save when archive_prewarm::shouldMilestoneSave() says so. Cheap when
  // no save is due: a handful of integer compares under mutex_. Also
  // folds in R-BACK-3.11 (savePoisoned_) and skips entirely while the
  // async load is still in flight (nothing valid to save yet).
  void notePresent();

  // R-BACK-3.11 — mark this session's archive saves as skipped for the
  // remainder of the process (the shader debug-env key is non-default).
  // Load still proceeds normally; only save (mid-session and shutdown)
  // is affected. Idempotent. Safe to call before or after
  // beginAsyncFullLoad()/the synchronous ctor.
  void poisonSave();
  bool savePoisoned() const noexcept { return savePoisoned_; }

 private:
  void triggerSave(const char* reason);

  WMT::Reference<WMT::BinaryArchive> ref_{};
  std::string path_{};

  std::thread loadThread_{};
  std::thread saveThread_{};

  std::mutex mutex_{};
  archive_prewarm::MilestoneState milestone_{};
  bool savePoisoned_ = false;
};

}  // namespace dxmt9::shaders
