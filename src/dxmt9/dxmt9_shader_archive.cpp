#include "dxmt9_shader_archive.hpp"

#include "dxmt9_perf_counters.hpp"
#include "dxmt9_shader_sources.hpp"
#include "util/log/log.hpp"

#include <utility>

namespace dxmt9::shaders {

Archive::Archive(WMT::Device device, std::string path) : path_(std::move(path)) {
  if (path_.empty()) {
    return;
  }
  ref_ = initShaderArchive(device, path_);
}

Archive::~Archive() {
  // Joining first establishes a happens-before edge for the unguarded
  // ref_/path_ reads below: once loadThread_ (whose body calls the
  // beginAsyncFullLoad() completion callback that writes ref_ under
  // mutex_) and saveThread_ (which reads ref_/path_ to persist) have
  // both returned, no other thread can still be touching this Archive.
  if (loadThread_.joinable()) {
    loadThread_.join();
  }
  if (saveThread_.joinable()) {
    saveThread_.join();
  }
  if (!ref_) {
    return;
  }
  if (savePoisoned_) {
    // R-BACK-3.11 — this session ran with a non-default shader debug-env
    // key; do not let a probe/diagnostic build's variants pollute the
    // shared production archive. Load already happened normally (only
    // save is gated).
    perf::countPrewarmSaveSkippedDebugEnv();
    dxmt9::util::logf(dxmt9::util::LogLevel::Info, "dxmt9-archive",
                      "shutdown save skipped: path=\"%s\" reason=debug_env_non_default",
                      path_.c_str());
    return;
  }
  persistShaderArchive(ref_, path_);
}

void Archive::beginAsyncFullLoad(WMT::Device device, std::string path) {
  path_ = std::move(path);
  if (path_.empty()) {
    return;
  }
  loadThread_ = archive_prewarm::beginAsyncFullLoad(
      device, path_,
      [this](WMT::Reference<WMT::BinaryArchive> loaded) {
        std::lock_guard<std::mutex> lock(mutex_);
        ref_ = std::move(loaded);
      });
}

void Archive::notePresent() {
  // R-BACK-3.9 interplay: while the async load is still in flight there
  // is nothing valid to evaluate yet (ref_ may still be empty, or about
  // to be overwritten by the attach callback) — skip this present's
  // milestone bookkeeping rather than take mutex_ and race the attach.
  // The next Present after attach picks the tracker back up; the
  // presentCount undercount this causes is bounded by how long the load
  // itself takes, which R-BACK-3.9's size guard already bounds.
  if (archive_prewarm::archiveLoadInFlight()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ref_) {
    // Disabled mode, or a Full/Lazy load that produced no usable archive
    // (no cache root, DXMT_DEBUG_DISABLE_SHADER_ARCHIVE, etc). Nothing to
    // persist regardless of present cadence.
    return;
  }
  milestone_.presentCount++;
  milestone_.savePoisoned = savePoisoned_;
  const auto totalEntries = archive_prewarm::archiveEntriesCompiledTotal();
  if (totalEntries != milestone_.entriesAddedThisSession) {
    milestone_.entriesAddedThisSession = totalEntries;
    milestone_.lastNewEntryPresent = milestone_.presentCount;
  }
  if (!archive_prewarm::shouldMilestoneSave(milestone_)) {
    return;
  }
  milestone_.entriesAddedAtLastSave = milestone_.entriesAddedThisSession;
  milestone_.savesPerformed++;
  perf::countPrewarmMilestoneSave();
  triggerSave("milestone");
}

void Archive::poisonSave() {
  std::lock_guard<std::mutex> lock(mutex_);
  savePoisoned_ = true;
}

void Archive::triggerSave(const char* reason) {
  // At most two milestone saves per process (shouldMilestoneSave's
  // savesPerformed >= 2 gate), so this reassignment happens at most
  // once; join defensively rather than assume the prior save finished —
  // reassigning over a still-joinable std::thread calls std::terminate.
  if (saveThread_.joinable()) {
    saveThread_.join();
  }
  saveThread_ = std::thread([this, reasonTag = std::string(reason)]() {
    // ref_/path_ are read here without mutex_: triggerSave() is only
    // reached from notePresent() after it already confirmed
    // !archiveLoadInFlight() and took mutex_ to observe ref_ as valid,
    // and ref_ is never mutated again after the async-load attach for
    // the remaining lifetime of this Archive — so no concurrent writer
    // can race this read.
    persistShaderArchive(ref_, path_);
    dxmt9::util::logf(dxmt9::util::LogLevel::Info, "dxmt9-archive",
                      "mid-session save: path=\"%s\" reason=%s",
                      path_.c_str(), reasonTag.c_str());
  });
}

}  // namespace dxmt9::shaders
