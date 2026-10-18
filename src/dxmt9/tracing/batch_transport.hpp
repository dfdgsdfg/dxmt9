#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace dxmt9::tracing {

// Cold worker persistence policy. A failed/partial write never increments the
// complete-record count. The owning worker retains the span until this returns.
template<class Record, class Write>
bool persistBatch(std::span<const Record> records, uint64_t retained,
                  uint64_t maximum, Write write) noexcept {
  if (retained > maximum || records.size() > maximum - retained) return false;
  const char* bytes = reinterpret_cast<const char*>(records.data());
  size_t remaining = records.size_bytes();
  while (remaining) {
    const auto written = write(bytes, remaining);
    if (written <= 0 || static_cast<uint64_t>(written) > remaining) return false;
    bytes += written;
    remaining -= static_cast<size_t>(written);
  }
  return true;
}

// Producers own their partial block exclusively.
// The consumer sees only a
// release-published initialized prefix, and releases it only after persistence.
template<class Record, uint32_t MaxProducers = 256>
class BatchTransport {
 public:
  enum class State : uint8_t { Free, ProducerOwned, Published, ConsumerOwned };
  struct Block {
    std::atomic<State> state{State::Free};
    Record* records = nullptr;
    uint32_t capacity = 0, count = 0;
    uint64_t generation = 0, sequence = 0;
  };
  struct Producer {
    uint32_t current = UINT32_MAX;
    uint64_t serviced_epoch = 0;
  };

  bool configure(uint32_t events, uint32_t threshold, uint64_t generation) {
    if (!threshold || events < 4 * uint64_t(threshold)) return false;
    storage_ = std::make_unique<Record[]>(events);
    generation_ = generation;
    producers_ = std::min<uint32_t>(MaxProducers, events / (4 * threshold));
    slots_ = producers_ * 2 + 2;
    uint32_t offset = 0;
    for (uint32_t i = 0; i < slots_; ++i) {
      Block& block = blocks_[i];
      block.state.store(State::Free, std::memory_order_relaxed);
      block.count = 0;
      block.generation = generation;
      block.capacity = i < producers_ * 2 ? threshold :
          (events - producers_ * 2 * threshold) / 2;
      block.records = storage_.get() + offset;
      offset += block.capacity;
    }
    for (auto& producer : producer_) producer = {};
    epoch_.store(0);
    sequence_.store(0);
    return true;
  }
  uint32_t producer_count() const noexcept { return producers_; }
  void request_partials() noexcept { epoch_.fetch_add(1, std::memory_order_relaxed); }

  // prepare is called only after whole-batch capacity has been secured.
  template<class Prepare, class Include>
  bool append(uint32_t producer, std::span<const Record> records, bool report,
              uint64_t generation, Prepare prepare, Include include) noexcept {
    uint32_t accepted = 0;
    for (const auto& record : records) if (include(record)) ++accepted;
    if (!accepted) return true;
    if (generation != generation_ || producer >= producers_ || records.empty()) return false;
    if (report) {
      for (uint32_t i = producers_ * 2; i < slots_; ++i) {
        Block& block = blocks_[i];
        if (accepted > block.capacity) continue;
        State expected = State::Free;
        if (!block.state.compare_exchange_strong(expected, State::ProducerOwned,
              std::memory_order_acquire)) continue;
        block.count = 0;
        for (const auto& record : records)
          if (include(record)) block.records[block.count++] = prepare(record);
        publish(block);
        return true;
      }
      return false;
    }
    Producer& owner = producer_[producer];
    const uint64_t epoch = epoch_.load(std::memory_order_relaxed);
    if (owner.current != UINT32_MAX && owner.serviced_epoch != epoch) {
      publish(blocks_[owner.current]);
      owner.current = UINT32_MAX;
    }
    owner.serviced_epoch = epoch;
    if (accepted > blocks_[producer * 2].capacity) return false;
    if (owner.current != UINT32_MAX &&
        accepted > blocks_[owner.current].capacity - blocks_[owner.current].count) {
      publish(blocks_[owner.current]);
      owner.current = UINT32_MAX;
    }
    if (owner.current == UINT32_MAX) {
      for (uint32_t i = producer * 2; i < producer * 2 + 2; ++i) {
        State expected = State::Free;
        if (blocks_[i].state.compare_exchange_strong(expected, State::ProducerOwned,
              std::memory_order_acquire)) {
          blocks_[i].count = 0;
          owner.current = i;
          break;
        }
      }
    }
    if (owner.current == UINT32_MAX) return false;
    Block& block = blocks_[owner.current];
    for (const auto& record : records)
          if (include(record)) block.records[block.count++] = prepare(record);
    if (block.count == block.capacity) {
      publish(block);
      owner.current = UINT32_MAX;
    }
    return true;
  }

  // Only the controller, after admission closes and all writers finish.
  void close_partials() noexcept {
    for (uint32_t i = 0; i < producers_; ++i) {
      auto& owner = producer_[i];
      if (owner.current != UINT32_MAX) {
        publish(blocks_[owner.current]);
        owner.current = UINT32_MAX;
      }
    }
  }
  template<class Consume>
  bool drain(Consume consume) noexcept {
    bool consumed = false;
    for (uint32_t i = 0; i < slots_; ++i) {
      Block& block = blocks_[i];
      State expected = State::Published;
      if (!block.state.compare_exchange_strong(expected, State::ConsumerOwned,
            std::memory_order_acquire)) continue;
      consume(std::span<const Record>(block.records, block.count),
              block.generation, block.sequence);
      block.state.store(State::Free, std::memory_order_release);
      consumed = true;
    }
    return consumed;
  }
 private:
  void publish(Block& block) noexcept {
    block.sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    block.state.store(State::Published, std::memory_order_release);
  }
  std::unique_ptr<Record[]> storage_;
  Block blocks_[MaxProducers * 2 + 2];
  Producer producer_[MaxProducers];
  uint32_t producers_ = 0, slots_ = 0;
  uint64_t generation_ = 0;
  std::atomic<uint64_t> epoch_{0}, sequence_{0};
};

}  // namespace dxmt9::tracing
