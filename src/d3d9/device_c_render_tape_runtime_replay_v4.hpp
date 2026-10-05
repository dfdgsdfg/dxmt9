#pragma once

#include "device_c_render_tape_runtime_identity_v4.hpp"

#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace dxmt9::d3d9 {

// Retained lifetime is not admission. Every short diagnostic operation joins
// the endpoint's current arm through this scope-owned lease, never through a
// lease retained by the root itself or an activity spanning replay/Encode.
class RenderTapeRuntimeReplayAccessV4 {
public:
  RenderTapeRuntimeReplayAccessV4(RenderTapeRuntimeIdentityV4* owner,
      ReplayTransactionRawIdentity raw, render::TapeRuntimeCaptureLease lease) noexcept
      : owner_(owner), raw_(raw), lease_(std::move(lease)) {}

  bool matches(RenderTapeRuntimeIdentityV4* owner,
      ReplayTransactionRawIdentity raw) const noexcept {
    return owner_ == owner && raw_ == raw;
  }
  const render::TapeRuntimeCaptureLease& captureLease() const noexcept { return lease_; }

  template <typename Fn>
  bool visit(Fn&& fn) const noexcept {
    static_assert(std::is_nothrow_invocable_r_v<bool, Fn&, RenderTapeRuntimeIdentityV4&>);
    bool result = false;
    lease_.visit([&](render::TapeRuntimeCaptureAttachment& admitted) noexcept {
      if (&admitted == owner_) result = fn(*owner_);
    });
    return result;
  }

  bool beginRecord(std::uint32_t index, std::uint32_t type) const noexcept {
    return visit([&](RenderTapeRuntimeIdentityV4& owner) noexcept {
      return owner.beginRecord(index, type);
    });
  }
  bool beginDispatch() const noexcept {
    return visit([](RenderTapeRuntimeIdentityV4& owner) noexcept { return owner.beginDispatch(); });
  }
  bool endRecord(std::int32_t result) const noexcept {
    return visit([&](RenderTapeRuntimeIdentityV4& owner) noexcept { return owner.endRecord(result); });
  }
  bool acknowledgeRaw(bool succeeded) const noexcept {
    return visit([&](RenderTapeRuntimeIdentityV4& owner) noexcept {
      return owner.acknowledgeRaw(raw_, succeeded);
    });
  }
  void fail() const noexcept {
    visit([](RenderTapeRuntimeIdentityV4& owner) noexcept { owner.fail(); return true; });
  }
  ReplayTransactionObserverSink replaySink() noexcept {
    return {this, observeReplay, replayFailed};
  }
  render::TapeEmissionObserverSink emissionSink() noexcept {
    return {this, bindEmission, failEmission};
  }

private:
  static bool observeReplay(void* context, const ReplayTransactionObservation& observation) noexcept {
    return static_cast<RenderTapeRuntimeReplayAccessV4*>(context)->visit(
        [&](RenderTapeRuntimeIdentityV4& owner) noexcept {
          const auto sink = owner.replaySink();
          return sink.observe(sink.context, observation);
        });
  }
  static void replayFailed(void* context, ReplayTransactionObserverFailure) noexcept {
    static_cast<RenderTapeRuntimeReplayAccessV4*>(context)->fail();
  }
  static bool bindEmission(void* context, query::WriterIdentity writer,
      std::uint32_t physical, core::MetalCommandKind kind,
      std::span<const render::TapeEmissionRecordLocator> records,
      std::uint32_t firstSubdraw) noexcept {
    return static_cast<RenderTapeRuntimeReplayAccessV4*>(context)->visit(
        [&](RenderTapeRuntimeIdentityV4& owner) noexcept {
          return owner.bindEmission(writer, physical, kind, records, firstSubdraw);
        });
  }
  static void failEmission(void* context) noexcept {
    static_cast<RenderTapeRuntimeReplayAccessV4*>(context)->fail();
  }

  RenderTapeRuntimeIdentityV4* owner_ = nullptr;
  ReplayTransactionRawIdentity raw_{};
  render::TapeRuntimeCaptureLease lease_{};
};

// Borrowed only for synchronous record hooks inside the retained raw scope.
// No root, scope, transaction or receipt pointer enters a replay packet.
inline thread_local RenderTapeRuntimeReplayAccessV4* activeRenderTapeRuntimeReplayV4 = nullptr;

// Cold raw-boundary owner. Construct only after authentic ingress/ledger raw
// identity is available and a retained root was acquired. Same-raw nesting
// uses the transaction driver's alias rules; every real begin remains a
// distinct attempt. Scope exit diagnoses an open attempt, never invents a
// terminal outcome from a renderer return code.
class ScopedRenderTapeRuntimeReplayV4 {
public:
  ScopedRenderTapeRuntimeReplayV4(std::shared_ptr<RenderTapeRuntimeIdentityV4> owner,
      ReplayTransactionRawIdentity raw, bool requireLedgerAck,
      render::TapeRuntimeCaptureLease lease) noexcept
      : owner_(std::move(owner)), access_(owner_.get(), raw, std::move(lease)),
        previous_(activeRenderTapeRuntimeReplayV4), emission_(access_.emissionSink()),
        requireLedgerAck_(requireLedgerAck) {
    // Same-root/same-raw nesting aliases the outer observer's exact callback
    // context as well as its pending emission locators. A facade-this sink in
    // the inner scope would falsely poison the existing transaction driver.
    installed_ = previous_ && previous_->matches(owner_.get(), raw) ? previous_ : &access_;
    activeRenderTapeRuntimeReplayV4 = installed_;
    observer_.emplace(raw, installed_->replaySink());
    if (installed_ == &access_) {
      emission_.beginRaw(raw.eventOrdinal);
      emissionScope_.emplace(emission_);
    }
  }
  ~ScopedRenderTapeRuntimeReplayV4() {
    observer_.reset();
    emissionScope_.reset();
    // An exception/early return may leave an admitted ledger raw unsettled.
    // Reject capture only; do not guess a transaction outcome or append ACK.
    if (requireLedgerAck_ && !ledgerBoundaryObserved_) installed_->fail();
    activeRenderTapeRuntimeReplayV4 = previous_;
  }
  ScopedRenderTapeRuntimeReplayV4(const ScopedRenderTapeRuntimeReplayV4&) = delete;
  ScopedRenderTapeRuntimeReplayV4& operator=(const ScopedRenderTapeRuntimeReplayV4&) = delete;

  // Invoke only after the actual raw ledger ACK/failure boundary. A commit is
  // independently observed by the driver and is not sufficient for this ACK.
  bool acknowledge(bool ledgerSucceeded) noexcept {
    ledgerBoundaryObserved_ = true;
    return installed_->acknowledgeRaw(ledgerSucceeded);
  }

private:
  std::shared_ptr<RenderTapeRuntimeIdentityV4> owner_{};
  RenderTapeRuntimeReplayAccessV4 access_;
  RenderTapeRuntimeReplayAccessV4* previous_ = nullptr;
  RenderTapeRuntimeReplayAccessV4* installed_ = nullptr;
  std::optional<ScopedReplayTransactionObserver> observer_{};
  render::TapeEmissionObserver emission_;
  std::optional<render::ScopedTapeEmissionObserver> emissionScope_{};
  bool requireLedgerAck_ = false, ledgerBoundaryObserved_ = false;
};

} // namespace dxmt9::d3d9
