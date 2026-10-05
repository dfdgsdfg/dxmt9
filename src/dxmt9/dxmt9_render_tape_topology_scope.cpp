#include "dxmt9_render_tape_topology_scope.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9_draw_encoder_internal.hpp"

#include <algorithm>
#include <utility>

namespace dxmt9::render {
namespace {
constexpr auto maxRows = d3d9::kRenderTapeIdentityV4MaxRows;

bool rangeAdmitted(TapeTopologySourceRange range,
    const TapeTopologySourceIdentity& source) noexcept {
  return range.source == source.source && range.sequence == source.sequence &&
      range.commandCount && range.commandBegin <= source.commandCount &&
      range.commandCount <= source.commandCount - range.commandBegin;
}
} // namespace

TapeTopologyInvocationContext::TapeTopologyInvocationContext(
    TapeRuntimeCaptureLease lease, TapeTopologyCaptureIdentity capture) noexcept
    : lease_(std::move(lease)), capture_(capture) {}

std::shared_ptr<TapeTopologyInvocationContext> TapeTopologyInvocationContext::create(
    TapeRuntimeCaptureLease lease, TapeTopologyCaptureIdentity capture,
    std::span<const TapeTopologySourceRange> ranges) noexcept {
  if (!lease.active()) return {};
  std::shared_ptr<TapeTopologyInvocationContext> context;
  try {
    context.reset(new TapeTopologyInvocationContext(lease, capture));
    bool resolved = false;
    const bool admitted = lease.visit([&](TapeRuntimeCaptureAttachment& root) noexcept {
      try {
        if (!capture.valid() || ranges.empty() || ranges.size() > maxRows) {
          root.topology().fail(); return;
        }
        context->ranges_.assign(ranges.begin(), ranges.end());
        for (const auto range : ranges) {
          TapeTopologySourceIdentity source;
          if (!root.topology().sourceIdentity(range.source, range.sequence, source) ||
              !rangeAdmitted(range, source) ||
              range.commandCount > maxRows - context->commands_.size()) {
            root.topology().fail(); return;
          }
          for (std::uint32_t offset = 0u; offset < range.commandCount; ++offset) {
            TapeTopologyCommandRef ref;
            if (!root.topology().commandRef(range.source, range.sequence,
                    range.commandBegin + offset, ref) ||
                (!context->commands_.empty() &&
                 context->commands_.back().commandIndex >= ref.commandIndex)) {
              root.topology().fail(); return;
            }
            context->commands_.push_back(ref);
          }
        }
        resolved = true;
      } catch (...) { root.topology().fail(); }
    });
    if (admitted && resolved) return context;
  } catch (...) {
    lease.visit([](TapeRuntimeCaptureAttachment& root) noexcept { root.topology().fail(); });
  }
  return {};
}

TapeTopologyInvocationContext::~TapeTopologyInvocationContext() {
  // Scope destruction does not guess which staged invocation was accepted.
  // An explicit pending handle may outlive a TLS scope through this context.
  lease_.visit([&](TapeRuntimeCaptureAttachment& root) noexcept {
    for (const auto& invocation : invocations_) {
      if (invocation.accepted && invocation.encoded) continue;
      if (invocation.accepted || invocation.encoded || invocation.encodedFragments ||
          registrationEffect(invocation)) root.topology().fail();
      else root.topology().reject(invocation.handle.attempt);
    }
    if (invocations_.empty() && !registered_.empty()) root.topology().fail();
  });
}

bool TapeTopologyInvocationContext::active() const noexcept {
  std::lock_guard lock(mutex_);
  return !failed_ && lease_.active();
}

std::uint32_t TapeTopologyInvocationContext::commandCount() const noexcept {
  return static_cast<std::uint32_t>(commands_.size());
}

bool TapeTopologyInvocationContext::commandIndex(core::CpuReadyTape::SourceRef source,
    std::uint64_t sequence, std::uint32_t physicalCommandIndex,
    std::uint32_t& out) const noexcept {
  out = UINT32_MAX;
  std::lock_guard lock(mutex_);
  if (failed_) return false;
  bool found = false;
  const bool admitted = lease_.visit([&](TapeRuntimeCaptureAttachment& root) noexcept {
    TapeTopologyCommandRef ref;
    if (!root.topology().commandRef(source, sequence, physicalCommandIndex, ref)) return;
    const auto selected = std::find(commands_.begin(), commands_.end(), ref);
    if (selected == commands_.end()) return;
    out = static_cast<std::uint32_t>(selected - commands_.begin());
    found = true;
  });
  return admitted && found;
}

TapeTopologyInvocationContext::Invocation* TapeTopologyInvocationContext::find(
    TapeTopologyInvocationHandle handle) noexcept {
  if (handle.capture != capture_ || !handle) return nullptr;
  const auto selected = std::find_if(invocations_.begin(), invocations_.end(),
      [&](const auto& invocation) { return invocation.handle == handle; });
  return selected == invocations_.end() ? nullptr : &*selected;
}

bool TapeTopologyInvocationContext::invalidate() noexcept {
  failed_ = true;
  lease_.visit([](TapeRuntimeCaptureAttachment& root) noexcept { root.topology().fail(); });
  return false;
}

void TapeTopologyInvocationContext::fail() noexcept {
  std::lock_guard lock(mutex_);
  invalidate();
}

bool TapeTopologyInvocationContext::stageWithoutDag(
    d3d9::RenderTapeExecutionTopologyV1Route route,
    d3d9::RenderTapeExecutionTopologyV1FallbackReason reason,
    std::span<const std::uint32_t> order, TapeTopologyInvocationHandle& out) noexcept {
  out = {};
  std::lock_guard lock(mutex_);
  if (failed_ || !lease_.active()) return false;
  try {
    if (order.empty() || order.size() > commands_.size() || invocations_.size() >= maxRows)
      return invalidate();
    Invocation invocation;
    invocation.executionOrder.assign(order.begin(), order.end());
    std::vector<std::uint32_t> physical(order.begin(), order.end());
    std::sort(physical.begin(), physical.end());
    for (std::size_t index = 0u; index < physical.size(); ++index) {
      if (physical[index] >= commands_.size() || (index && physical[index - 1u] == physical[index]))
        return invalidate();
      invocation.commands.push_back(commands_[physical[index]]);
    }
    std::vector<std::uint32_t> permutation;
    for (const auto index : order)
      permutation.push_back(static_cast<std::uint32_t>(
          std::lower_bound(physical.begin(), physical.end(), index) - physical.begin()));
    // Allocate context storage before staging so no successful ledger attempt
    // can be lost if the context's vector growth throws afterwards.
    invocations_.push_back(std::move(invocation));
    auto& stored = invocations_.back();
    std::uint64_t attempt = 0u;
    const bool admitted = lease_.visit([&](TapeRuntimeCaptureAttachment& root) noexcept {
      attempt = root.topology().stageWithoutDag(route, reason, stored.commands, permutation);
    });
    if (!admitted || !attempt) {
      invocations_.pop_back();
      if (admitted) failed_ = true;
      return false;
    }
    out = stored.handle = {capture_, attempt};
    return true;
  } catch (...) { return invalidate(); }
}

bool TapeTopologyInvocationContext::stageSelectedDag(
    d3d9::RenderTapeExecutionTopologyV1FallbackReason reason,
    const framegraph::FrameGraph& graph, std::span<const std::uint32_t> actualOrder,
    std::span<const std::uint32_t> alreadyEncoded,
    TapeTopologyInvocationHandle& out) noexcept {
  out = {};
  std::lock_guard lock(mutex_);
  if (failed_ || !lease_.active()) return false;
  try {
    if (invocations_.size() >= maxRows) return invalidate();
    Invocation invocation;
    invocation.executionOrder.assign(actualOrder.begin(), actualOrder.end());
    std::vector<bool> prior(commands_.size(), false);
    for (const auto index : alreadyEncoded) {
      if (index >= commands_.size() || prior[index]) return invalidate();
      prior[index] = true;
    }
    for (const auto index : actualOrder)
      if (index >= commands_.size()) return invalidate();
    for (std::size_t index = 0u; index < commands_.size(); ++index)
      if (!prior[index]) invocation.commands.push_back(commands_[index]);
    invocations_.push_back(std::move(invocation));
    auto& stored = invocations_.back();
    std::uint64_t attempt = 0u;
    const bool admitted = lease_.visit([&](TapeRuntimeCaptureAttachment& root) noexcept {
      attempt = root.topology().stageSelectedDag(reason, graph, commands_, actualOrder, alreadyEncoded);
    });
    if (!admitted || !attempt) {
      invocations_.pop_back();
      if (admitted) failed_ = true;
      return false;
    }
    out = stored.handle = {capture_, attempt};
    return true;
  } catch (...) { return invalidate(); }
}

bool TapeTopologyInvocationContext::hasRegistration(
    const TapeTopologyCommandRef& command) const noexcept {
  return std::any_of(registered_.begin(), registered_.end(), [&](const auto range) {
    return range.source == command.owner.source && range.sequence == command.owner.sequence &&
        command.extent.physicalCommandIndex >= range.commandBegin &&
        command.extent.physicalCommandIndex - range.commandBegin < range.commandCount;
  });
}

bool TapeTopologyInvocationContext::registrationEffect(const Invocation& invocation) const noexcept {
  return std::any_of(invocation.commands.begin(), invocation.commands.end(),
      [&](const auto& command) { return hasRegistration(command); });
}

bool TapeTopologyInvocationContext::registerSources(
    std::span<const TapeTopologySourceRange> receipts) noexcept {
  std::lock_guard lock(mutex_);
  if (failed_ || !lease_.active()) return false;
  bool recorded = false;
  const bool admitted = lease_.visit([&](TapeRuntimeCaptureAttachment& root) noexcept {
    try {
      if (receipts.empty() || receipts.size() > maxRows - registered_.size()) {
        failed_ = true; root.topology().fail(); return;
      }
      for (const auto range : receipts) {
        TapeTopologySourceIdentity source;
        if (!root.topology().sourceIdentity(range.source, range.sequence, source) ||
            !rangeAdmitted(range, source) ||
            std::none_of(commands_.begin(), commands_.end(),
                [&](const auto& command) { return command.owner == source; })) {
          failed_ = true; root.topology().fail(); return;
        }
      }
      registered_.insert(registered_.end(), receipts.begin(), receipts.end());
      recorded = true;
    } catch (...) { failed_ = true; root.topology().fail(); }
  });
  return admitted && recorded;
}

bool TapeTopologyInvocationContext::acceptRegisteredBatch(
    std::span<const TapeTopologyInvocationHandle> handles) noexcept {
  std::lock_guard lock(mutex_);
  if (failed_ || !lease_.active()) return false;
  try {
    if (handles.empty() || handles.size() > invocations_.size()) return invalidate();
    std::vector<TapeTopologySourceIdentity> sources;
    std::vector<TapeTopologyAcceptedInvocation> accepted;
    for (const auto handle : handles) {
      const auto* invocation = find(handle);
      if (!invocation || invocation->accepted) return invalidate();
      for (const auto& command : invocation->commands) {
        if (!hasRegistration(command)) return invalidate();
        if (std::find(sources.begin(), sources.end(), command.owner) == sources.end())
          sources.push_back(command.owner);
      }
      accepted.push_back({handle.attempt, 0u});
    }
    std::sort(sources.begin(), sources.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.sourceIndex < rhs.sourceIndex;
    });
    bool result = false;
    const bool admitted = lease_.visit([&](TapeRuntimeCaptureAttachment& root) noexcept {
      for (auto& receipt : accepted) {
        receipt.edge = root.edges().next();
        if (!receipt.edge) { root.topology().fail(); return; }
      }
      result = root.topology().acceptBatch(capture_, accepted, sources);
    });
    if (!admitted || !result) {
      if (admitted) failed_ = true;
      return false;
    }
    for (const auto handle : handles) find(handle)->accepted = true;
    return true;
  } catch (...) { return invalidate(); }
}

bool TapeTopologyInvocationContext::recordEncoded(Invocation& invocation,
    std::span<const std::uint32_t> actualOrder) noexcept {
  if (invocation.encoded || actualOrder.size() != invocation.executionOrder.size() ||
      !std::equal(actualOrder.begin(), actualOrder.end(), invocation.executionOrder.begin()))
    return invalidate();
  try {
    std::vector<TapeTopologyCommandRef> execution;
    for (const auto index : actualOrder) {
      if (index >= commands_.size()) return invalidate();
      execution.push_back(commands_[index]);
    }
    bool result = false;
    const bool admitted = lease_.visit([&](TapeRuntimeCaptureAttachment& root) noexcept {
      result = root.topology().observeEncoded({capture_, invocation.handle.attempt}, execution);
    });
    if (admitted && result) invocation.encoded = true;
    else if (admitted) failed_ = true;
    return admitted && result;
  } catch (...) { return invalidate(); }
}

bool TapeTopologyInvocationContext::observeEncoded(TapeTopologyInvocationHandle handle,
    std::span<const std::uint32_t> actualOrder) noexcept {
  std::lock_guard lock(mutex_);
  if (failed_ || !lease_.active()) return false;
  auto* invocation = find(handle);
  if (!invocation || invocation->expectedFragments) return invalidate();
  return recordEncoded(*invocation, actualOrder);
}

bool TapeTopologyInvocationContext::reject(TapeTopologyInvocationHandle handle) noexcept {
  std::lock_guard lock(mutex_);
  if (failed_ || !lease_.active()) return false;
  const auto* invocation = find(handle);
  if (!invocation || invocation->accepted || invocation->encoded ||
      invocation->encodedFragments || registrationEffect(*invocation)) return invalidate();
  bool result = false;
  const bool admitted = lease_.visit([&](TapeRuntimeCaptureAttachment& root) noexcept {
    result = root.topology().reject(handle.attempt);
  });
  if (!admitted || !result) return false;
  invocations_.erase(std::find_if(invocations_.begin(), invocations_.end(),
      [&](const auto& value) { return value.handle == handle; }));
  return true;
}

bool TapeTopologyInvocationContext::beginAggregate(TapeTopologyInvocationHandle handle,
    std::uint32_t expectedFragments) noexcept {
  std::lock_guard lock(mutex_);
  if (failed_ || !lease_.active()) return false;
  auto* invocation = find(handle);
  if (!invocation || invocation->encoded || invocation->expectedFragments ||
      !expectedFragments || expectedFragments > maxRows) return invalidate();
  try {
    invocation->fragmentOrder.reserve(invocation->executionOrder.size());
    return lease_.visit([&](TapeRuntimeCaptureAttachment&) noexcept {
      invocation->expectedFragments = expectedFragments;
    });
  } catch (...) { return invalidate(); }
}

bool TapeTopologyInvocationContext::observeFragmentEncoded(TapeTopologyInvocationHandle handle,
    std::span<const std::uint32_t> actualOrder) noexcept {
  std::lock_guard lock(mutex_);
  if (failed_ || !lease_.active()) return false;
  auto* invocation = find(handle);
  if (!invocation || !invocation->expectedFragments || invocation->encoded ||
      invocation->encodedFragments >= invocation->expectedFragments ||
      actualOrder.size() > invocation->executionOrder.size() - invocation->fragmentOrder.size() ||
      !std::equal(actualOrder.begin(), actualOrder.end(),
          invocation->executionOrder.begin() + invocation->fragmentOrder.size())) return invalidate();
  // Capacity was reserved before any fragment Encode, so recording an effect
  // does not allocate. Empty successful fragments still count as facts.
  return lease_.visit([&](TapeRuntimeCaptureAttachment&) noexcept {
    invocation->fragmentOrder.insert(invocation->fragmentOrder.end(), actualOrder.begin(), actualOrder.end());
    ++invocation->encodedFragments;
  });
}

bool TapeTopologyInvocationContext::foldFragment(TapeTopologyInvocationHandle handle) noexcept {
  std::lock_guard lock(mutex_);
  if (failed_ || !lease_.active()) return false;
  auto* invocation = find(handle);
  if (!invocation || !invocation->expectedFragments || invocation->encoded ||
      invocation->foldedFragments >= invocation->encodedFragments) return invalidate();
  return lease_.visit([&](TapeRuntimeCaptureAttachment&) noexcept {
    ++invocation->foldedFragments;
  });
}

bool TapeTopologyInvocationContext::finishAggregate(TapeTopologyInvocationHandle handle) noexcept {
  std::lock_guard lock(mutex_);
  if (failed_ || !lease_.active()) return false;
  auto* invocation = find(handle);
  if (!invocation || !invocation->expectedFragments ||
      invocation->encodedFragments != invocation->expectedFragments ||
      invocation->foldedFragments != invocation->expectedFragments) return invalidate();
  return recordEncoded(*invocation, invocation->fragmentOrder);
}

bool TapeTopologyInvocationContext::fragmentActive(TapeTopologyInvocationHandle handle) const noexcept {
  std::lock_guard lock(mutex_);
  if (failed_ || !lease_.active()) return false;
  return std::any_of(invocations_.begin(), invocations_.end(), [&](const auto& invocation) {
    return invocation.handle == handle && invocation.expectedFragments && !invocation.encoded &&
        invocation.encodedFragments < invocation.expectedFragments;
  });
}

class TapeTopologyInvocationStore {
public:
  struct Entry {
    std::shared_ptr<TapeTopologyInvocationContext> context{};
    bool tentative = false;
    bool selected = true;
  };
  struct Arm {
    TapeRuntimeCaptureLease lease{};
    TapeTopologyCaptureIdentity capture{};
    std::vector<TapeTopologySourceRange> registrations{};
    std::vector<Entry> entries{};
  };

  Arm* arm(TapeRuntimeCaptureLease lease) noexcept {
    std::erase_if(arms_, [](const auto& value) { return !value.lease.active(); });
    TapeTopologyCaptureIdentity capture;
    if (!lease.visit([&](TapeRuntimeCaptureAttachment& root) noexcept {
          capture = root.topology().identity();
        }) || !capture.valid()) return nullptr;
    for (auto& value : arms_)
      if (value.capture == capture) return &value;
    try {
      if (arms_.size() >= 64u) { fail(lease); return nullptr; }
      arms_.push_back({std::move(lease), capture, {}, {}});
      return &arms_.back();
    } catch (...) { fail(lease); return nullptr; }
  }

  static void fail(const TapeRuntimeCaptureLease& lease) noexcept {
    lease.visit([](TapeRuntimeCaptureAttachment& root) noexcept { root.topology().fail(); });
  }

  void prune(Arm& owner) noexcept {
    std::erase_if(owner.entries, [](const auto& entry) {
      std::lock_guard lock(entry.context->mutex_);
      return !entry.context->invocations_.empty() &&
          std::all_of(entry.context->invocations_.begin(), entry.context->invocations_.end(),
              [](const auto& invocation) { return invocation.accepted && invocation.encoded; });
    });
  }

  static bool sameRanges(const Entry& entry,
      std::span<const TapeTopologySourceRange> ranges) noexcept {
    return ranges.size() == entry.context->ranges_.size() &&
        std::equal(ranges.begin(), ranges.end(), entry.context->ranges_.begin());
  }

  Entry* candidate(Arm& owner, std::span<const TapeTopologySourceRange> ranges) noexcept {
    const auto selected = std::find_if(owner.entries.begin(), owner.entries.end(),
        [&](const auto& entry) { return entry.tentative && sameRanges(entry, ranges); });
    return selected == owner.entries.end() ? nullptr : &*selected;
  }

  bool apply(Arm& owner, Entry& entry,
      std::span<const TapeTopologySourceRange> receipts) noexcept {
    try {
      std::vector<TapeTopologySourceRange> matching;
      for (const auto receipt : receipts)
        if (std::any_of(entry.context->commands_.begin(), entry.context->commands_.end(),
                [&](const auto& ref) {
                  return ref.owner.source == receipt.source && ref.owner.sequence == receipt.sequence;
                })) matching.push_back(receipt);
      return matching.empty() || entry.context->registerSources(matching);
    } catch (...) { fail(owner.lease); return false; }
  }

  std::shared_ptr<TapeTopologyInvocationContext> retain(TapeRuntimeCaptureLease lease,
      std::span<const TapeTopologySourceRange> ranges, bool tentative) noexcept {
    auto* owner = arm(lease);
    if (!owner) return {};
    prune(*owner);
    try {
      if (owner->entries.size() >= maxRows || (tentative && candidate(*owner, ranges))) {
        fail(owner->lease); return {};
      }
      auto context = TapeTopologyInvocationContext::create(lease, owner->capture, ranges);
      if (!context) return {};
      owner->entries.push_back({context, tentative, !tentative});
      if (!tentative && !apply(*owner, owner->entries.back(), owner->registrations)) return {};
      return context;
    } catch (...) { fail(owner->lease); return {}; }
  }

  void accept(Arm& owner) noexcept {
    struct Pending {
      TapeTopologyInvocationContext* context = nullptr;
      TapeTopologyInvocationHandle handle{};
      bool selected = false;
    };
    try {
      std::vector<Pending> pending;
      for (const auto& entry : owner.entries) {
        std::lock_guard lock(entry.context->mutex_);
        for (const auto& invocation : entry.context->invocations_)
          if (!invocation.accepted) pending.push_back({entry.context.get(), invocation.handle, entry.selected});
      }
      std::sort(pending.begin(), pending.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.handle.attempt < rhs.handle.attempt;
      });
      std::vector<TapeTopologyAcceptedInvocation> accepted;
      std::vector<TapeTopologySourceIdentity> sources;
      for (const auto& receipt : pending) {
        if (!receipt.selected) break;
        std::lock_guard lock(receipt.context->mutex_);
        const auto* invocation = receipt.context->find(receipt.handle);
        if (!invocation || !std::all_of(invocation->commands.begin(), invocation->commands.end(),
                [&](const auto& ref) { return receipt.context->hasRegistration(ref); })) break;
        accepted.push_back({receipt.handle.attempt, 0u});
        for (const auto& ref : invocation->commands)
          if (std::find(sources.begin(), sources.end(), ref.owner) == sources.end()) sources.push_back(ref.owner);
      }
      if (accepted.empty()) { prune(owner); return; }
      std::sort(sources.begin(), sources.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.sourceIndex < rhs.sourceIndex;
      });
      bool result = false;
      owner.lease.visit([&](TapeRuntimeCaptureAttachment& root) noexcept {
        for (auto& receipt : accepted) {
          receipt.edge = root.edges().next();
          if (!receipt.edge) { root.topology().fail(); return; }
        }
        result = root.topology().acceptBatch(owner.capture, accepted, sources);
      });
      if (!result) return;
      for (std::size_t index = 0u; index < accepted.size(); ++index) {
        std::lock_guard lock(pending[index].context->mutex_);
        pending[index].context->find(pending[index].handle)->accepted = true;
      }
      prune(owner);
    } catch (...) { fail(owner.lease); }
  }

  void after(TapeTopologyInvocationContext& context) noexcept {
    for (auto& owner : arms_)
      if (owner.capture == context.capture_ && owner.lease.active()) { accept(owner); return; }
  }

  void registration(TapeRuntimeCaptureLease lease,
      std::span<const TapeTopologySourceRange> receipts) noexcept {
    auto* owner = arm(lease);
    if (!owner) return;
    try {
      bool valid = true;
      if (receipts.empty() || receipts.size() > maxRows) { fail(lease); return; }
      if (!lease.visit([&](TapeRuntimeCaptureAttachment& root) noexcept {
            for (const auto range : receipts) {
              TapeTopologySourceIdentity source;
              if (!root.topology().sourceIdentity(range.source, range.sequence, source) ||
                  !rangeAdmitted(range, source)) { valid = false; root.topology().fail(); return; }
            }
          }) || !valid) return;
      for (const auto receipt : receipts) {
        if (std::find(owner->registrations.begin(), owner->registrations.end(), receipt) != owner->registrations.end())
          continue;
        if (owner->registrations.size() >= maxRows) { fail(lease); return; }
        owner->registrations.push_back(receipt);
      }
      for (auto& entry : owner->entries)
        if (entry.selected && !apply(*owner, entry, receipts)) return;
      accept(*owner);
    } catch (...) { fail(lease); }
  }

private:
  std::vector<Arm> arms_{};
};

namespace {
TapeTopologyInvocationStore& invocationStore() noexcept {
  static thread_local TapeTopologyInvocationStore store;
  return store;
}
} // namespace

std::shared_ptr<TapeTopologyInvocationContext> retainTapeTopologyInvocation(
    TapeRuntimeCaptureLease lease, std::span<const TapeTopologySourceRange> ranges,
    bool tentative) noexcept {
  return invocationStore().retain(std::move(lease), ranges, tentative);
}

void registerTapeTopologySources(TapeRuntimeCaptureLease lease,
    std::span<const TapeTopologySourceRange> receipts) noexcept {
  invocationStore().registration(std::move(lease), receipts);
}

void rejectTapeTopologyCandidate(TapeRuntimeCaptureLease lease,
    std::span<const TapeTopologySourceRange> ranges) noexcept {
  auto& store = invocationStore();
  auto* owner = store.arm(lease);
  if (!owner) return;
  auto* entry = store.candidate(*owner, ranges);
  if (!entry) return;
  const auto context = entry->context;
  // The exact tentative context owns explicit handles, not "latest attempt".
  while (!context->invocations_.empty())
    if (!context->reject(context->invocations_.front().handle)) return;
  std::erase_if(owner->entries, [&](const auto& value) { return value.context == context; });
}

bool activateTapeTopologyCandidate(TapeRuntimeCaptureLease lease,
    std::span<const TapeTopologySourceRange> ranges, std::uint32_t fragments) noexcept {
  auto& store = invocationStore();
  auto* owner = store.arm(lease);
  if (!owner) return false;
  auto* entry = store.candidate(*owner, ranges);
  if (!entry || entry->selected || entry->context->invocations_.size() != 1u) {
    store.fail(lease); return false;
  }
  if (!entry->context->beginAggregate(entry->context->invocations_[0].handle, fragments)) return false;
  entry->selected = true;
  if (!store.apply(*owner, *entry, owner->registrations)) return false;
  store.accept(*owner);
  return true;
}

std::shared_ptr<TapeTopologyInvocationContext> tapeTopologyCandidateContext(
    TapeRuntimeCaptureLease lease, std::span<const TapeTopologySourceRange> ranges,
    TapeTopologyInvocationHandle& handle) noexcept {
  handle = {};
  auto& store = invocationStore();
  auto* owner = store.arm(lease);
  if (!owner) return {};
  auto* entry = store.candidate(*owner, ranges);
  if (!entry || !entry->selected || entry->context->invocations_.size() != 1u) {
    store.fail(lease); return {};
  }
  handle = entry->context->invocations_[0].handle;
  return entry->context;
}

void foldTapeTopologyCandidate(TapeRuntimeCaptureLease lease,
    std::span<const TapeTopologySourceRange> ranges, bool finish) noexcept {
  TapeTopologyInvocationHandle handle;
  auto context = tapeTopologyCandidateContext(lease, ranges, handle);
  if (!context || !context->foldFragment(handle)) return;
  if (finish && context->finishAggregate(handle)) invocationStore().after(*context);
}

void observeTapeTopologyInlineTerminal(TapeRuntimeCaptureLease lease,
    TapeTopologySourceRange source) noexcept {
  // V4 still excludes a source with zero executed placements. Preserve that
  // schema gap: a genuine inline terminal is not a GPU Submit/Complete pair.
  auto& store = invocationStore();
  auto* owner = store.arm(lease);
  if (!owner) return;
  for (const auto& entry : owner->entries)
    for (const auto& invocation : entry.context->invocations_)
      if (std::any_of(invocation.commands.begin(), invocation.commands.end(), [&](const auto& command) {
            return command.owner.source == source.source && command.owner.sequence == source.sequence;
          })) { entry.context->fail(); return; }
}

std::vector<TapeTopologySourceRange> tapeTopologySourceRanges(
    std::span<const core::metalqueue::ResolvedPublishedSource> sources) noexcept {
  try {
    std::vector<TapeTopologySourceRange> result;
    if (sources.empty() || sources.size() > maxRows) return {};
    result.reserve(sources.size());
    for (const auto& source : sources) {
      if (!source.valid() || source.payload.commandCount() > UINT32_MAX) return {};
      result.push_back({source.source, source.seqId, 0u,
          static_cast<std::uint32_t>(source.payload.commandCount())});
    }
    return result;
  } catch (...) { return {}; }
}

void registerTapeTopologyCompletionSources(TapeRuntimeCaptureLease lease,
    std::span<const core::metalqueue::QueueCompletionSource> sources) noexcept {
  try {
    std::vector<TapeTopologySourceRange> receipts;
    for (const auto& source : sources) {
      // A post-Encode replacement receipt is not a new registration. Its
      // original locator was observed at the actual append/assignment owner.
      if (source.receiptBacked()) continue;
      if (!source.locatorBacked() || source.commandBegin > UINT32_MAX || source.commandCount > UINT32_MAX) {
        TapeTopologyInvocationStore::fail(lease); return;
      }
      receipts.push_back({source.source, source.seqId,
          static_cast<std::uint32_t>(source.commandBegin), static_cast<std::uint32_t>(source.commandCount)});
    }
    if (receipts.empty()) return;
    registerTapeTopologySources(std::move(lease), receipts);
  } catch (...) { TapeTopologyInvocationStore::fail(lease); }
}

void stageTapeTopologySourceWithoutDag(core::SourcePayloadView payload, std::uint64_t sequence,
    const encoders::EncodeChunkOptions& options,
    d3d9::RenderTapeExecutionTopologyV1FallbackReason reason) noexcept {
  auto* scope = activeTapeTopologyInvocation;
  if (!scope) return;
  const auto replay = encoders::encodeChunkReplayRange(
      options.sessionSource ? options.sessionSource->slotIndex : 0u, payload, sequence, options);
  if (!replay.valid || replay.commandEnd > UINT32_MAX) { scope->fail(); return; }
  const auto route = options.replayCommandPlanActive
      ? d3d9::RenderTapeExecutionTopologyV1Route::PlannerBypass
      : d3d9::RenderTapeExecutionTopologyV1Route::NaturalOrder;
  scope->stageSourceWithoutDag(route, reason, options.partitionSource, sequence,
      static_cast<std::uint32_t>(replay.commandBegin), static_cast<std::uint32_t>(replay.commandCount()),
      options.replayCommandPlanActive, options.replayCommandOrder);
}

void prepareTapeTopologySourceEncode(core::SourcePayloadView payload, std::uint64_t sequence,
    const encoders::EncodeChunkOptions& options) noexcept {
  auto* scope = activeTapeTopologyInvocation;
  if (!scope) return;
  const auto replay = encoders::encodeChunkReplayRange(
      options.sessionSource ? options.sessionSource->slotIndex : 0u, payload, sequence, options);
  if (!replay.valid || replay.commandEnd > UINT32_MAX) { scope->fail(); return; }
  scope->prepareSourceEncode(options.partitionSource, sequence,
      static_cast<std::uint32_t>(replay.commandBegin), static_cast<std::uint32_t>(replay.commandCount()),
      options.replayCommandPlanActive, options.replayCommandOrder);
}

ScopedTapeTopologyInvocation::ScopedTapeTopologyInvocation(
    std::shared_ptr<TapeTopologyInvocationContext> context,
    TapeTopologyInvocationHandle handle, bool aggregateFragment) noexcept
    : context_(std::move(context)), handle_(handle),
      previous_(activeTapeTopologyInvocation), aggregateFragment_(aggregateFragment) {
  activeTapeTopologyInvocation = this;
}

ScopedTapeTopologyInvocation::~ScopedTapeTopologyInvocation() {
  activeTapeTopologyInvocation = previous_;
}

bool ScopedTapeTopologyInvocation::stageWithoutDag(
    d3d9::RenderTapeExecutionTopologyV1Route route,
    d3d9::RenderTapeExecutionTopologyV1FallbackReason reason,
    std::span<const std::uint32_t> order) noexcept {
  if (!context_) return false;
  if (aggregateFragment_) return context_->fragmentActive(handle_);
  if (handle_) { context_->fail(); return false; }
  const bool result = context_->stageWithoutDag(route, reason, order, handle_);
  if (result) invocationStore().after(*context_);
  return result;
}

bool ScopedTapeTopologyInvocation::stageSelectedDag(
    d3d9::RenderTapeExecutionTopologyV1FallbackReason reason,
    const framegraph::FrameGraph& graph, std::span<const std::uint32_t> actualOrder,
    std::span<const std::uint32_t> alreadyEncoded) noexcept {
  if (!context_) return false;
  if (aggregateFragment_) return context_->fragmentActive(handle_);
  if (handle_) { context_->fail(); return false; }
  const bool result = context_->stageSelectedDag(reason, graph, actualOrder, alreadyEncoded, handle_);
  if (result) invocationStore().after(*context_);
  return result;
}

bool ScopedTapeTopologyInvocation::observeSuccessfulEncode(
    std::span<const std::uint32_t> actualOrder) noexcept {
  if (!context_) return false;
  const bool result = aggregateFragment_ ? context_->observeFragmentEncoded(handle_, actualOrder)
      : context_->observeEncoded(handle_, actualOrder);
  if (result) invocationStore().after(*context_);
  return result;
}

bool ScopedTapeTopologyInvocation::commandIndex(core::CpuReadyTape::SourceRef source,
    std::uint64_t sequence, std::uint32_t physicalCommandIndex,
    std::uint32_t& out) const noexcept {
  out = UINT32_MAX;
  return context_ && context_->commandIndex(source, sequence, physicalCommandIndex, out);
}

bool ScopedTapeTopologyInvocation::prepareSourceEncode(core::CpuReadyTape::SourceRef source,
    std::uint64_t sequence, std::uint32_t commandBegin, std::uint32_t commandCount,
    bool explicitOrder, std::span<const std::uint32_t> physicalOrder) noexcept {
  if (!context_ || commandBegin > UINT32_MAX - commandCount) {
    if (context_) context_->fail();
    return false;
  }
  try {
    const auto count = explicitOrder ? physicalOrder.size() : commandCount;
    if (count > maxRows) { context_->fail(); return false; }
    std::vector<std::uint32_t> observed;
    observed.reserve(count);
    for (std::size_t offset = 0u; offset < count; ++offset) {
      const auto physical = explicitOrder ? physicalOrder[offset]
          : commandBegin + static_cast<std::uint32_t>(offset);
      std::uint32_t index = UINT32_MAX;
      if (!commandIndex(source, sequence, physical, index)) { context_->fail(); return false; }
      observed.push_back(index);
    }
    if (prepared_ && observed != actualOrder_) { context_->fail(); return false; }
    if (!prepared_) actualOrder_ = std::move(observed);
    prepared_ = true;
    return true;
  } catch (...) { context_->fail(); return false; }
}

bool ScopedTapeTopologyInvocation::stageSourceWithoutDag(
    d3d9::RenderTapeExecutionTopologyV1Route route,
    d3d9::RenderTapeExecutionTopologyV1FallbackReason reason,
    core::CpuReadyTape::SourceRef source, std::uint64_t sequence,
    std::uint32_t commandBegin, std::uint32_t commandCount,
    bool explicitOrder, std::span<const std::uint32_t> physicalOrder) noexcept {
  if (aggregateFragment_) return context_ && context_->fragmentActive(handle_);
  if (!prepareSourceEncode(source, sequence, commandBegin, commandCount, explicitOrder, physicalOrder))
    return false;
  return stageWithoutDag(route, reason, actualOrder_);
}

bool ScopedTapeTopologyInvocation::observePreparedEncode(bool succeeded) noexcept {
  if (!context_ || !prepared_) { if (context_) context_->fail(); return false; }
  if (!succeeded) {
    // A wholly omitted invocation waits for its genuine inline terminal.
    // Nonempty unsuccessful execution has no authoritative Encode receipt.
    if (!actualOrder_.empty()) context_->fail();
    return false;
  }
  return observeSuccessfulEncode(actualOrder_);
}

} // namespace dxmt9::render
