#include "render_tape_cpu_v4_consumer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using namespace dxmt9::test;
using Clock = std::chrono::steady_clock;
std::uint64_t checks = 0u;

void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

CpuV4ConsumerBinding captureBinding() { return {71u, 72u, 73u, 0u}; }
CpuV4ConsumerIdentity identity(std::uint64_t sequence) {
  return {71u, 72u, 73u, sequence, 0u, sequence, 0u, 1u, sequence};
}

void lifecycleExhaustive() {
  using Action = CpuV4ConsumerAction;
  using Phase = CpuV4ConsumerPhase;
  using Source = CpuV4ConsumerSourcePhase;
  CpuV4ConsumerState initial;
  initial.binding = captureBinding();
  initial.limits = {2u, 2u, 8u};
  auto foreign = identity(1u);
  ++foreign.queue;
  auto stale = identity(1u);
  ++stale.sourceGeneration;
  auto missing = identity(1u);
  missing.storageGeneration = 0u;
  const std::array identities{CpuV4ConsumerIdentity{}, identity(1u), identity(2u),
                              foreign, stale, missing};
  const std::array actions{Action::Start, Action::Observe, Action::Submit, Action::Acknowledge,
      Action::Retire, Action::CloseProducer, Action::Stop, Action::EmptyWake, Action::Fail};
  std::vector<CpuV4ConsumerState> states{initial};
  for (std::size_t next = 0u; next < states.size(); ++next) {
    const auto before = states[next];
    for (const auto action : actions) {
      for (const auto source : identities) {
        for (const auto commands : {0u, 1u, 3u}) {
          auto after = before;
          const bool accepted = cpuV4ConsumerAdvance(after, action, source, commands);
          if (!accepted) {
            check(after == before, "all reducer rejection is transactional");
            continue;
          }
          check(after.retired <= after.acknowledged && after.acknowledged <= after.submitted &&
              after.submitted <= after.observed && after.observed - after.retired <= 1u,
              "every reachable interleaving preserves exactly-once FIFO lifecycle");
          check(after.commands <= after.limits.maxCommands && after.observed <= after.limits.maxSources &&
              after.lifecycleActions <= after.limits.maxLifecycleActions &&
              after.retiredSequence >= before.retiredSequence,
              "reachable caps and retirement never regress or overflow");
          check(after.sourcePhase == Source::Empty ? !after.active.valid()
              : after.binding.matches(after.active), "only a bound actual source may remain active");
          check(after.phase != Phase::Stopped || (after.producerClosed &&
              after.sourcePhase == Source::Empty && after.observed == after.retired),
              "normal stop cannot abandon an active source or open producer");
          if (std::find(states.begin(), states.end(), after) == states.end()) states.push_back(after);
        }
      }
    }
  }
  check(states.size() > 30u && states.size() < 10000u,
      "bounded exhaustive checker visits nontrivial closed lifecycle domain");
  for (const auto& state : states) {
    if (state.phase != Phase::Stopped && state.phase != Phase::Failed) continue;
    for (const auto action : actions) {
      auto after = state;
      check(!cpuV4ConsumerAdvance(after, action, identity(2u), 1u) && after == state,
          "every terminal state is irreversible under every action");
    }
  }
  check(CpuV4ConsumerLimits{65536u, 65536u, 262144u}.valid() &&
      !CpuV4ConsumerLimits{65537u, 65536u, 262144u}.valid() &&
      !CpuV4ConsumerLimits{65536u, 65537u, 262144u}.valid() &&
      !CpuV4ConsumerLimits{65536u, 65536u, 262145u}.valid(),
      "hard source, command and dedicated lifecycle-action limits accept equality but reject one over");
  auto running = initial;
  check(cpuV4ConsumerAdvance(running, Action::Start), "directed counter-check fixture starts");
  std::vector<CpuV4ConsumerState> corrupt;
  auto changed = running;
  changed.commands = changed.limits.maxCommands + 1u;
  corrupt.push_back(changed);
  changed = running;
  changed.lifecycleActions = changed.limits.maxLifecycleActions + 1u;
  corrupt.push_back(changed);
  changed = running;
  changed.observed = changed.limits.maxSources + 1u;
  corrupt.push_back(changed);
  changed = running;
  changed.submitted = 1u;
  corrupt.push_back(changed);
  changed = running;
  changed.sourcePhase = static_cast<Source>(255u);
  corrupt.push_back(changed);
  changed = running;
  changed.phase = static_cast<Phase>(255u);
  corrupt.push_back(changed);
  for (const auto& before : corrupt) {
    check(!cpuV4ConsumerStateValid(before), "corrupt dimensions are rejected before subtraction");
    for (const auto action : actions) {
      auto after = before;
      check(!cpuV4ConsumerAdvance(after, action, identity(1u), 1u) && after == before,
          "even fail/control events reject corrupt reducer input transactionally");
    }
  }
  auto equality = running;
  check(cpuV4ConsumerAdvance(equality, Action::Observe, identity(1u), 2u) &&
      cpuV4ConsumerAdvance(equality, Action::Submit, identity(1u)) &&
      cpuV4ConsumerAdvance(equality, Action::Acknowledge, identity(1u)) &&
      cpuV4ConsumerAdvance(equality, Action::Retire, identity(1u)),
      "command cap equality admits a genuine full lifecycle");
  const auto fullCommands = equality;
  check(!cpuV4ConsumerAdvance(equality, Action::Observe, identity(2u), 1u) && equality == fullCommands,
      "one command past equality rejects without underflow or mutation");
  equality = running;
  equality.limits.maxLifecycleActions = 3u;
  check(cpuV4ConsumerAdvance(equality, Action::Observe, identity(1u), 1u) &&
      cpuV4ConsumerAdvance(equality, Action::Submit, identity(1u)) &&
      cpuV4ConsumerAdvance(equality, Action::Acknowledge, identity(1u)),
      "dedicated lifecycle-action equality admits exactly its bounded prefix");
  const auto fullActions = equality;
  check(!cpuV4ConsumerAdvance(equality, Action::Retire, identity(1u)) && equality == fullActions,
      "one lifecycle action past equality cannot advance retirement");
}

// A weak authority and separately pinned fake queue model the real adapter's
// lease contract. This backend projects immutable owned command metadata; it
// deliberately has no Metal, query result, or full state/resource digest.
struct FakeAuthority { std::atomic<bool> active{true}; };
struct FakeSource {
  CpuV4ConsumerObservation value{};
  std::shared_ptr<const std::vector<CpuV4ConsumerCommandObservation>> payload{};
};
struct FakeQuarantine {
  CpuV4ConsumerIdentity identity{};
  bool submitted = false, acknowledged = false;
};
struct FakeQueue {
  std::mutex mutex{};
  std::condition_variable cv{};
  std::deque<FakeSource> ready{};
  std::optional<FakeSource> represented{};
  CpuV4ConsumerIdentity submitted{}, acknowledged{};
  std::vector<CpuV4ConsumerIdentity> submittedRows{}, acknowledgedRows{}, retiredRows{};
  std::optional<FakeQuarantine> quarantine{};
  std::size_t cleanupCalls = 0u;
  std::size_t resident = 0u, residentPeak = 0u, naturalPublications = 0u;
  std::size_t forcedPublications = 0u, rawAcknowledgements = 0u, writerCommands = 0u;
  bool producerClosed = false, stopRequested = false, poisoned = false, waiting = false;
  bool completionInsideRaw = false;
  CpuV4ConsumerFailure fault = CpuV4ConsumerFailure::None;
  CpuV4ConsumerFailure poisonReason = CpuV4ConsumerFailure::None;
  bool missingIdentity = false, wrongQueue = false, faultAfterEffect = false;
  std::size_t borrowed = 0u;
};

struct FakeBorrowGuard {
  std::shared_ptr<FakeQueue> queue{};
  bool active = false;
  ~FakeBorrowGuard() {
    if (!active) return;
    {
      std::lock_guard lock(queue->mutex);
      --queue->borrowed;
    }
    queue->cv.notify_all();
  }
};

struct FakeBackend {
  std::shared_ptr<FakeQueue> queue{};
  std::weak_ptr<FakeAuthority> lease{};

  bool live() const noexcept {
    const auto owner = lease.lock();
    return owner && owner->active.load(std::memory_order_acquire);
  }
  CpuV4ConsumerBinding binding() const { return live() ? captureBinding() : CpuV4ConsumerBinding{}; }
  bool producerClosedAndWriterEmpty() const {
    std::lock_guard lock(queue->mutex);
    return live() && queue->producerClosed && !queue->writerCommands;
  }
  CpuV4ConsumerWait waitReady(Clock::time_point deadline) {
    std::unique_lock lock(queue->mutex);
    queue->waiting = true;
    queue->cv.notify_all();
    const bool woke = queue->cv.wait_until(lock, deadline, [&] {
      return !live() || queue->poisoned || queue->stopRequested || !queue->ready.empty();
    });
    queue->waiting = false;
    if (!woke) return CpuV4ConsumerWait::Deadline;
    if (!live()) return CpuV4ConsumerWait::LeaseRevoked;
    if (queue->poisoned) return CpuV4ConsumerWait::Failed;
    if (!queue->ready.empty()) return CpuV4ConsumerWait::Ready;
    return queue->stopRequested && queue->producerClosed && !queue->writerCommands
        ? CpuV4ConsumerWait::Stopped : CpuV4ConsumerWait::Failed;
  }
  CpuV4ConsumerFailure observeReady(std::size_t remainingCommands, CpuV4ConsumerObservation& out) {
    FakeSource owned;
    FakeBorrowGuard borrow{queue};
    {
      std::lock_guard lock(queue->mutex);
      if (!live()) return CpuV4ConsumerFailure::LeaseRevoked;
      if (queue->poisoned || queue->ready.empty() || queue->represented)
        return CpuV4ConsumerFailure::Observation;
      if (queue->fault == CpuV4ConsumerFailure::Observation && !queue->faultAfterEffect)
        return CpuV4ConsumerFailure::Observation;
      if (queue->ready.front().payload->size() > remainingCommands)
        return CpuV4ConsumerFailure::Capacity;
      queue->represented = std::move(queue->ready.front());
      queue->ready.pop_front();
      owned = *queue->represented;
      ++queue->borrowed;
      borrow.active = true;
      if (queue->fault == CpuV4ConsumerFailure::Observation)
        return CpuV4ConsumerFailure::Observation;
      if (queue->fault == CpuV4ConsumerFailure::Allocation)
        throw std::bad_alloc{};
      if (queue->fault == CpuV4ConsumerFailure::Capacity)
        return CpuV4ConsumerFailure::Capacity;
    }
    // The retained immutable payload is projected outside the queue lock.
    out.identity = owned.value.identity;
    out.rawOrdinal = owned.value.rawOrdinal;
    out.sourceOrdinal = owned.value.sourceOrdinal;
    out.commands = *owned.payload;
    {
      std::lock_guard lock(queue->mutex);
      if (queue->missingIdentity) out.identity.storageGeneration = 0u;
      if (queue->wrongQueue) ++out.identity.queue;
    }
    return live() ? CpuV4ConsumerFailure::None : CpuV4ConsumerFailure::LeaseRevoked;
  }
  bool submit(CpuV4ConsumerIdentity source) {
    std::lock_guard lock(queue->mutex);
    if (!live() || !queue->represented || queue->submitted.valid() ||
        queue->represented->value.identity != source ||
        (queue->fault == CpuV4ConsumerFailure::Submit && !queue->faultAfterEffect))
      return false;
    queue->submitted = source;
    queue->submittedRows.push_back(source);
    return queue->fault != CpuV4ConsumerFailure::Submit;
  }
  bool acknowledge(CpuV4ConsumerIdentity source) {
    std::lock_guard lock(queue->mutex);
    if (!live() || queue->submitted != source || queue->acknowledged.valid() ||
        (queue->fault == CpuV4ConsumerFailure::Acknowledge && !queue->faultAfterEffect)) return false;
    queue->acknowledged = source;
    queue->acknowledgedRows.push_back(source);
    return queue->fault != CpuV4ConsumerFailure::Acknowledge;
  }
  bool retire(CpuV4ConsumerIdentity source) {
    {
      std::lock_guard lock(queue->mutex);
      if (!live() || queue->acknowledged != source || !queue->represented ||
          queue->represented->value.identity != source || !queue->resident ||
          (queue->fault == CpuV4ConsumerFailure::Retire && !queue->faultAfterEffect)) return false;
      queue->retiredRows.push_back(source);
      queue->completionInsideRaw |= !queue->producerClosed;
      queue->represented.reset();
      queue->submitted = queue->acknowledged = {};
      --queue->resident;
    }
    queue->cv.notify_all();
    return queue->fault != CpuV4ConsumerFailure::Retire;
  }
  void requestStop() noexcept {
    {
      std::lock_guard lock(queue->mutex);
      queue->stopRequested = true;
    }
    queue->cv.notify_all();
  }
  void poison(CpuV4ConsumerFailure reason) noexcept {
    {
      std::lock_guard lock(queue->mutex);
      if (!queue->poisoned) queue->poisonReason = reason;
      queue->poisoned = true;
    }
    queue->cv.notify_all();
  }
  bool cleanup(Clock::time_point) noexcept {
    bool complete = false;
    {
      std::lock_guard lock(queue->mutex);
      ++queue->cleanupCalls;
      if (queue->fault == CpuV4ConsumerFailure::Cleanup) return false;
      if (queue->borrowed) return false;
      if (queue->represented) {
        if (!queue->resident) return false;
        const auto actual = queue->represented->value.identity;
        if ((queue->submitted.valid() && queue->submitted != actual) ||
            (queue->acknowledged.valid() && queue->acknowledged != actual)) return false;
        queue->quarantine = FakeQuarantine{actual,
            queue->submitted == actual, queue->acknowledged == actual};
        // Preserve the exact queue-owned payload and actual pending receipts.
        // No runtime abort API exists: this is NOT retirement/reclamation and
        // cannot satisfy cleanupComplete. Fixture teardown remains responsible.
        return false;
      }
      complete = !queue->represented && !queue->submitted.valid() && !queue->acknowledged.valid();
    }
    queue->cv.notify_all();
    return complete;
  }
};

struct Fixture {
  std::shared_ptr<FakeQueue> queue = std::make_shared<FakeQueue>();
  std::shared_ptr<FakeAuthority> authority = std::make_shared<FakeAuthority>();
  FakeBackend backend() const { return {queue, authority}; }
  bool publishRaw(std::size_t count, Clock::time_point deadline, std::size_t commandsPerSource = 1u) {
    for (std::size_t index = 0u; index < count; ++index) {
      std::unique_lock lock(queue->mutex);
      if (!queue->cv.wait_until(lock, deadline, [&] {
            return queue->poisoned || !authority->active.load(std::memory_order_acquire) ||
                queue->resident < 31u;
          }) || queue->poisoned || !authority->active.load(std::memory_order_acquire)) return false;
      queue->writerCommands = 1u;
      const auto sequence = queue->naturalPublications + 1u;
      FakeSource source;
      source.value.identity = identity(sequence);
      source.value.rawOrdinal = 11u;
      source.value.sourceOrdinal = sequence;
      source.payload = std::make_shared<const std::vector<CpuV4ConsumerCommandObservation>>(
          std::vector<CpuV4ConsumerCommandObservation>(commandsPerSource,
              {static_cast<std::uint32_t>(sequence % 3u), static_cast<std::uint32_t>(sequence + 2u)}));
      queue->ready.push_back(std::move(source));
      ++queue->naturalPublications;
      ++queue->resident;
      queue->residentPeak = std::max(queue->residentPeak, queue->resident);
      queue->writerCommands = 0u;
      lock.unlock();
      queue->cv.notify_all();
    }
    {
      std::lock_guard lock(queue->mutex);
      ++queue->rawAcknowledgements;
    }
    return true;
  }
  void closeProducer() {
    std::lock_guard lock(queue->mutex);
    queue->producerClosed = true;
  }
  void revoke() {
    authority->active.store(false, std::memory_order_release);
    queue->cv.notify_all();
  }
  bool waitUntilConsumerWaiting(Clock::time_point deadline) {
    std::unique_lock lock(queue->mutex);
    return queue->cv.wait_until(lock, deadline, [&] { return queue->waiting; });
  }
};

using Consumer = CpuV4ControlledConsumer<FakeBackend>;

void naturalPublicationStreaming() {
  Fixture fixture;
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  Consumer consumer(fixture.backend(), {64u, 64u, 256u}, deadline);
  check(consumer.start(), "capability-bound dedicated consumer starts");
  check(fixture.publishRaw(64u, deadline),
      "one 64-record raw publishes beyond the 31-slot ring while consumer progresses");
  fixture.closeProducer();
  check(consumer.closeProducer(), "explicit producer close verifies no unpublished writer");
  check(consumer.waitThrough(identity(64u), deadline), "actual exact local source receipt reaches retirement");
  check(consumer.stopAndJoin(), "producer-closed consumer drains and joins");
  const auto snapshot = consumer.snapshot();
  check(snapshot.state.observed == 64u && snapshot.state.submitted == 64u &&
      snapshot.state.acknowledged == 64u && snapshot.state.retired == 64u &&
      snapshot.state.lifecycleActions == 256u && snapshot.receipts == 64u && snapshot.workerExited &&
      snapshot.cleanupComplete,
      "every genuine fake source receives exactly one controlled FIFO lifecycle");
  check(!snapshot.payloadDigestPresent && !snapshot.fullSemanticOracleComplete && !snapshot.actualMetal &&
      !snapshot.gpuResultOracle && !snapshot.policyQualified && !snapshot.timingEligible,
      "metadata observation grants no full semantic, GPU, pixel or performance qualification");
  auto receipts = consumer.takeReceipts();
  check(receipts.size() == 64u && consumer.takeReceipts().empty(), "bounded immutable receipts deliver once");
  for (std::size_t index = 0u; index < receipts.size(); ++index) {
    const auto& row = receipts[index];
    check(row.observation && row.observation->identity == identity(index + 1u) &&
        row.observation->rawOrdinal == 11u && row.observation->sourceOrdinal == index + 1u &&
        row.observation->commands.size() == 1u &&
        row.observation->commands[0].drawCount == index + 3u &&
        row.submitted && row.acknowledged && row.retired && row.cleanupComplete,
        "immutable command metadata and exact source identity survive payload retirement");
  }
  auto foreign = identity(64u);
  ++foreign.queue;
  auto recycled = identity(64u);
  ++recycled.sourceGeneration;
  auto storage = identity(64u);
  ++storage.storageGeneration;
  check(!consumer.waitThrough(foreign, deadline) && !consumer.waitThrough(recycled, deadline) &&
      !consumer.waitThrough(storage, deadline), "foreign queue and recycled source/storage cannot borrow completion");
  std::lock_guard lock(fixture.queue->mutex);
  check(!fixture.queue->resident && fixture.queue->ready.empty() &&
      fixture.queue->residentPeak <= 31u && fixture.queue->completionInsideRaw &&
      fixture.queue->forcedPublications == 0u && fixture.queue->naturalPublications == 64u &&
      fixture.queue->rawAcknowledgements == 1u && !fixture.queue->poisoned,
      "streaming makes capacity progress without forced publication or per-raw acknowledgement changes");
  check(fixture.queue->submittedRows == fixture.queue->acknowledgedRows &&
      fixture.queue->acknowledgedRows == fixture.queue->retiredRows,
      "exact actual FIFO identities bind all controlled stages");
}

void emptyStopAndRevocation() {
  for (const bool revoke : {false, true}) {
    Fixture fixture;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    Consumer consumer(fixture.backend(), {}, deadline);
    check(consumer.start() && fixture.waitUntilConsumerWaiting(deadline),
        "empty consumer parks on a condition variable without polling");
    if (revoke) {
      fixture.revoke();
      consumer.join();
      const auto snapshot = consumer.snapshot();
      check(snapshot.failure == CpuV4ConsumerFailure::LeaseRevoked && snapshot.workerExited &&
          snapshot.state.phase == CpuV4ConsumerPhase::Failed && snapshot.state.observed == 0u,
          "revoked weak authority wakes an empty wait and joins without a source effect");
    } else {
      fixture.closeProducer();
      check(consumer.closeProducer() && consumer.stopAndJoin(),
          "producer-close plus local-stop wakes an empty consumer and joins");
      check(consumer.snapshot().state.observed == 0u && !fixture.queue->poisoned,
          "empty normal stop neither fabricates a source nor poisons a healthy backend");
    }
  }
}

void failureBoundaries() {
  for (const auto fault : {CpuV4ConsumerFailure::Observation, CpuV4ConsumerFailure::Submit,
                          CpuV4ConsumerFailure::Acknowledge, CpuV4ConsumerFailure::Retire}) {
    for (const bool afterEffect : {false, true}) {
      Fixture fixture;
      fixture.queue->fault = fault;
      fixture.queue->faultAfterEffect = afterEffect;
      const auto deadline = Clock::now() + std::chrono::seconds(5);
      Consumer consumer(fixture.backend(), {}, deadline);
      check(consumer.start() && fixture.publishRaw(1u, deadline), "failure fixture publishes one actual source");
      consumer.join();
      const auto snapshot = consumer.snapshot();
      check(snapshot.failure == fault && snapshot.state.phase == CpuV4ConsumerPhase::Failed &&
          snapshot.workerExited && snapshot.receipts == 0u && fixture.queue->poisoned,
          "first pre/post-effect failure poisons admission and wakes every blocked actor");
      check(snapshot.state.submitted == (fault == CpuV4ConsumerFailure::Acknowledge ||
              fault == CpuV4ConsumerFailure::Retire ? 1u : 0u) &&
          snapshot.state.acknowledged == (fault == CpuV4ConsumerFailure::Retire ? 1u : 0u) &&
          snapshot.state.retired == 0u, "failed effects cannot promote later controller lifecycle evidence");
      check(!consumer.waitThrough(identity(1u), deadline),
          "even actual after-effect retirement cannot promote a failed local receipt");
      {
        std::lock_guard lock(fixture.queue->mutex);
        const bool actualSubmitted = fault != CpuV4ConsumerFailure::Observation &&
            (fault != CpuV4ConsumerFailure::Submit || afterEffect);
        const bool actualAcknowledged = fault == CpuV4ConsumerFailure::Retire ||
            (fault == CpuV4ConsumerFailure::Acknowledge && afterEffect);
        const bool actualRetired = fault == CpuV4ConsumerFailure::Retire && afterEffect;
        const bool held = !(fault == CpuV4ConsumerFailure::Observation && !afterEffect) && !actualRetired;
        check(fixture.queue->submittedRows.size() == (actualSubmitted ? 1u : 0u) &&
            fixture.queue->acknowledgedRows.size() == (actualAcknowledged ? 1u : 0u) &&
            fixture.queue->retiredRows.size() == (actualRetired ? 1u : 0u),
            "cleanup reads actual backend side effects rather than reducer phase");
        check(!fixture.queue->borrowed && snapshot.cleanupComplete == !held &&
            snapshot.cleanupFailed == held && fixture.queue->represented.has_value() == held &&
            fixture.queue->resident == (actualRetired ? 0u : 1u),
            "released borrow and exact fail-stop ownership do not fabricate storage reclamation");
        if (held) {
          check(fixture.queue->quarantine && fixture.queue->quarantine->identity == identity(1u) &&
              fixture.queue->quarantine->submitted == actualSubmitted &&
              fixture.queue->quarantine->acknowledged == actualAcknowledged,
              "incomplete actual source keeps exact quarantine facts but cleanup remains incomplete");
        } else {
          check(!fixture.queue->quarantine && fixture.queue->ready.size() == (actualRetired ? 0u : 1u),
              "unclaimed Ready backlog and genuinely retired source remain distinct");
        }
      }
      check(!fixture.publishRaw(64u, deadline), "failed consumer unblocks and rejects further producer work");
    }
  }
  for (const auto fault : {CpuV4ConsumerFailure::Allocation, CpuV4ConsumerFailure::Capacity}) {
    Fixture fixture;
    fixture.queue->fault = fault;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    Consumer consumer(fixture.backend(), {}, deadline);
    check(consumer.start() && fixture.publishRaw(1u, deadline), "post-dequeue failure fixture publishes");
    consumer.join();
    const auto snapshot = consumer.snapshot();
    check(snapshot.failure == fault && snapshot.state.observed == 0u && snapshot.state.submitted == 0u &&
        snapshot.workerExited && snapshot.cleanupFailed && !snapshot.cleanupComplete,
        "allocation or metadata-capacity failure after dequeue preserves exact incomplete cleanup");
    std::lock_guard lock(fixture.queue->mutex);
    check(fixture.queue->quarantine && fixture.queue->quarantine->identity == identity(1u) &&
        !fixture.queue->borrowed && fixture.queue->resident == 1u && fixture.queue->retiredRows.empty(),
        "post-dequeue metadata fault releases only the borrow, not the actual failed source");
  }
  for (const bool foreign : {false, true}) {
    Fixture fixture;
    fixture.queue->missingIdentity = !foreign;
    fixture.queue->wrongQueue = foreign;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    Consumer consumer(fixture.backend(), {}, deadline);
    check(consumer.start() && fixture.publishRaw(1u, deadline), "identity failure fixture publishes source");
    consumer.join();
    const auto snapshot = consumer.snapshot();
    check(snapshot.failure == (foreign ? CpuV4ConsumerFailure::InvalidBinding
                                      : CpuV4ConsumerFailure::MissingIdentity) &&
        snapshot.state.submitted == 0u && snapshot.state.acknowledged == 0u &&
        fixture.queue->poisoned && snapshot.cleanupFailed && !snapshot.cleanupComplete,
        "missing or foreign identity fails before submit and preserves actual ownership");
  }
}

void stopDeadlineAndCapacity() {
  {
    Fixture fixture;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    Consumer consumer(fixture.backend(), {}, deadline);
    check(consumer.start() && !consumer.requestStop(), "normal stop requires explicit producer close");
    consumer.join();
    check(consumer.snapshot().failure == CpuV4ConsumerFailure::ProducerOpen && fixture.queue->poisoned,
        "open-producer stop fails and wakes every blocked actor");
  }
  {
    Fixture fixture;
    fixture.closeProducer();
    fixture.queue->writerCommands = 1u;
    Consumer consumer(fixture.backend(), {}, Clock::now() + std::chrono::seconds(5));
    check(consumer.start() && !consumer.closeProducer(), "producer-close cannot silently drop writing commands");
    consumer.join();
    check(consumer.snapshot().failure == CpuV4ConsumerFailure::WriterNotEmpty,
        "unpublished writer causes explicit stop failure");
  }
  {
    Fixture fixture;
    Consumer consumer(fixture.backend(), {}, Clock::now() - std::chrono::milliseconds(1));
    check(consumer.start(), "deadline fixture starts");
    consumer.join();
    check(consumer.snapshot().failure == CpuV4ConsumerFailure::Deadline && fixture.queue->poisoned &&
        consumer.snapshot().workerExited, "expired wait deadline fails, wakes and joins without sleep/polling");
  }
  {
    Fixture fixture;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    check(fixture.publishRaw(1u, deadline), "already-Ready deadline fixture publishes before start");
    Consumer consumer(fixture.backend(), {}, Clock::now() - std::chrono::milliseconds(1));
    check(consumer.start(), "expired already-Ready fixture starts");
    consumer.join();
    const auto snapshot = consumer.snapshot();
    std::lock_guard lock(fixture.queue->mutex);
    check(snapshot.failure == CpuV4ConsumerFailure::Deadline && snapshot.state.observed == 0u &&
        snapshot.cleanupComplete && !snapshot.cleanupFailed && fixture.queue->ready.size() == 1u &&
        fixture.queue->resident == 1u && fixture.queue->submittedRows.empty(),
        "a continuously Ready predicate cannot bypass the global deadline or fabricate dequeue");
  }
  {
    Fixture fixture;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    Consumer consumer(fixture.backend(), {}, deadline);
    check(consumer.start() && fixture.waitUntilConsumerWaiting(deadline), "cleanup-failure empty consumer parks");
    {
      std::lock_guard lock(fixture.queue->mutex);
      fixture.queue->fault = CpuV4ConsumerFailure::Cleanup;
    }
    fixture.closeProducer();
    check(consumer.closeProducer() && !consumer.stopAndJoin(),
        "failed exit cleanup cannot report a successful normal stop");
    const auto snapshot = consumer.snapshot();
    check(snapshot.state.phase == CpuV4ConsumerPhase::Failed &&
        snapshot.failure == CpuV4ConsumerFailure::Cleanup && snapshot.cleanupFailed &&
        !snapshot.cleanupComplete && snapshot.workerExited,
        "normal empty stop commits Stopped only after bounded cleanup succeeds");
  }
  {
    Fixture fixture;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    check(fixture.publishRaw(1u, deadline, 2u), "oversized metadata source publishes naturally");
    Consumer consumer(fixture.backend(), {1u, 1u, 4u}, deadline);
    check(consumer.start(), "bounded metadata projection fixture starts");
    consumer.join();
    const auto snapshot = consumer.snapshot();
    std::lock_guard lock(fixture.queue->mutex);
    check(snapshot.failure == CpuV4ConsumerFailure::Capacity && snapshot.state.observed == 0u &&
        snapshot.cleanupComplete && fixture.queue->ready.size() == 1u && !fixture.queue->represented &&
        fixture.queue->submittedRows.empty(),
        "source metadata exceeding remaining command cap reports Capacity before claim/allocation");
  }
  for (const auto limits : {CpuV4ConsumerLimits{1u, 2u, 8u}, CpuV4ConsumerLimits{2u, 1u, 8u},
                            CpuV4ConsumerLimits{2u, 2u, 3u}}) {
    Fixture fixture;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    check(fixture.publishRaw(2u, deadline), "capacity fixture naturally publishes before consumer starts");
    Consumer consumer(fixture.backend(), limits, deadline);
    check(consumer.start(), "bounded capacity fixture starts");
    consumer.join();
    const auto snapshot = consumer.snapshot();
    check(snapshot.state.phase == CpuV4ConsumerPhase::Failed &&
        snapshot.failure == CpuV4ConsumerFailure::Capacity && snapshot.workerExited && fixture.queue->poisoned,
        "source, command or dedicated lifecycle-action cap fails before an unbounded extra effect");
    check(snapshot.state.observed <= limits.maxSources && snapshot.state.commands <= limits.maxCommands &&
        snapshot.state.lifecycleActions <= limits.maxLifecycleActions,
        "every consumed counter remains within admitted cap");
  }
}

} // namespace

int main() {
  try {
    lifecycleExhaustive();
    naturalPublicationStreaming();
    emptyStopAndRevocation();
    failureBoundaries();
    stopDeadlineAndCapacity();
    std::cout << "controlled CPU V4 consumer: " << checks << " checks\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "controlled CPU V4 consumer: " << error.what() << '\n';
    return 1;
  }
}
