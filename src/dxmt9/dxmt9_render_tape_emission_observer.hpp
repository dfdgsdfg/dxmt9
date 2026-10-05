#pragma once

#include "dxmt9_backend_types.hpp"
#include "dxmt9/query_end_marker.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace dxmt9::render {

struct TapeEmissionRecordLocator {
  std::uint64_t eventOrdinal = 0u;
  std::uint32_t recordIndex = 0u;
  friend constexpr bool operator==(TapeEmissionRecordLocator,
      TapeEmissionRecordLocator) noexcept = default;
};

struct TapeEmissionObserverSink {
  void* context = nullptr;
  bool (*bind)(void*, query::WriterIdentity, std::uint32_t,
      core::MetalCommandKind, std::span<const TapeEmissionRecordLocator>,
      std::uint32_t) noexcept = nullptr;
  void (*fail)(void*) noexcept = nullptr;
};

// Cold, synchronous original-record provenance. Only copied value locators
// survive acceptance/staging. Renderer payloads, transactions and spans are
// never retained; every scope is installed inside an enabled-capture branch.
class TapeEmissionObserver {
public:
  explicit TapeEmissionObserver(TapeEmissionObserverSink sink,
      std::uint32_t maxLocators = 65536u) noexcept
      : sink_(sink), maxLocators_(maxLocators) {}
  void record(TapeEmissionRecordLocator locator) noexcept;
  void beginRaw(std::uint64_t eventOrdinal) noexcept { rawEventOrdinal_ = eventOrdinal; }
  void recordIndex(std::uint32_t index) noexcept { record({rawEventOrdinal_, index}); }
  void acceptCompatibilityDraw() noexcept;
  void stagePresent(query::WriterIdentity writer) noexcept;
  void discardTentative() noexcept;
  void fail() noexcept;
  bool failed() const noexcept { return failed_; }
  void append(query::WriterIdentity writer, std::size_t physical,
      core::MetalCommandKind kind, std::uint32_t firstSubdraw = 0u,
      std::uint32_t drawCount = 0u) noexcept;
  void appendLocal(std::size_t physical, core::MetalCommandKind kind,
      std::uint32_t firstSubdraw = 0u, std::uint32_t drawCount = 0u) noexcept;

  template <typename Fn>
  decltype(auto) withWriter(query::WriterIdentity writer,
      std::size_t commandOffset, Fn&& fn) {
    struct Scope {
      TapeEmissionObserver& owner;
      query::WriterIdentity previous;
      std::size_t offset;
      ~Scope() { owner.writer_ = previous; owner.commandOffset_ = offset; }
    } scope{*this, writer_, commandOffset_};
    writer_ = writer;
    commandOffset_ = commandOffset;
    return std::forward<Fn>(fn)();
  }

  template <typename Fn>
  decltype(auto) withStagedPresent(query::WriterIdentity writer, Fn&& fn) {
    const auto saved = current_;
    struct Scope {
      TapeEmissionObserver& owner;
      TapeEmissionRecordLocator previous;
      ~Scope() { owner.current_ = previous; }
    } scope{*this, saved};
    takePresent(writer);
    return withWriter(writer, 0u, std::forward<Fn>(fn));
  }

  template <typename Fn>
  bool flushCompatibilityBatch(Fn&& fn) {
    // A capacity cut runs under the incoming record. The accepted draws own
    // their old locators, so the current record cannot substitute for them.
    if (batchActive_) { fail(); return std::forward<Fn>(fn)(); }
    std::vector<TapeEmissionRecordLocator> records;
    records.swap(pendingDraws_);
    struct Scope {
      TapeEmissionObserver& owner;
      ~Scope() { owner.batch_ = {}; owner.batchCursor_ = 0u; owner.batchActive_ = false; }
    } scope{*this};
    batch_ = records;
    batchCursor_ = 0u;
    batchActive_ = true;
    const bool result = std::forward<Fn>(fn)();
    if (result && batchCursor_ != batch_.size()) fail();
    return result;
  }

private:
  struct Present {
    query::WriterIdentity writer{};
    TapeEmissionRecordLocator record{};
  };
  void takePresent(query::WriterIdentity writer) noexcept;
  TapeEmissionObserverSink sink_{};
  std::uint32_t maxLocators_ = 0u;
  std::uint64_t rawEventOrdinal_ = 0u;
  TapeEmissionRecordLocator current_{};
  query::WriterIdentity writer_{};
  std::size_t commandOffset_ = 0u;
  std::vector<TapeEmissionRecordLocator> pendingDraws_{};
  std::vector<Present> presents_{};
  std::span<const TapeEmissionRecordLocator> batch_{};
  std::size_t batchCursor_ = 0u;
  bool batchActive_ = false, failed_ = false;
};

inline thread_local TapeEmissionObserver* activeTapeEmissionObserver = nullptr;

class ScopedTapeEmissionObserver {
public:
  explicit ScopedTapeEmissionObserver(TapeEmissionObserver& observer) noexcept
      : previous_(activeTapeEmissionObserver) { activeTapeEmissionObserver = &observer; }
  ~ScopedTapeEmissionObserver() { activeTapeEmissionObserver = previous_; }
  ScopedTapeEmissionObserver(const ScopedTapeEmissionObserver&) = delete;
  ScopedTapeEmissionObserver& operator=(const ScopedTapeEmissionObserver&) = delete;
private:
  TapeEmissionObserver* previous_ = nullptr;
};

} // namespace dxmt9::render
