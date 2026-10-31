#include "peak_pulse/ring_buffer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace peak_pulse {
namespace {

struct StressItem final {
  std::uint64_t id;
  std::uint32_t producer;
  std::uint32_t sequence;
};

static_assert(NothrowMoveConstructible<StressItem>);

TEST(RingBufferStressTests, DeliversTenMillionItemsExactlyOnceWithFourByFourMpmc) {
  constexpr std::uint32_t producer_count = 4;
  constexpr std::uint32_t consumer_count = 4;
  constexpr std::uint32_t items_per_producer = 2'500'000;
  constexpr std::size_t total_items =
      static_cast<std::size_t>(producer_count) * items_per_producer;

  MpmcRingBuffer<StressItem, 1024> queue;
  auto seen = std::make_unique<std::atomic<std::uint8_t>[]>(total_items);
  auto next_sequence =
      std::make_unique<std::atomic<std::uint32_t>[]>(producer_count);
  for (std::size_t index = 0; index < total_items; ++index) {
    seen[index].store(0, std::memory_order_relaxed);
  }
  for (std::uint32_t producer = 0; producer < producer_count; ++producer) {
    next_sequence[producer].store(0, std::memory_order_relaxed);
  }

  std::atomic<std::uint32_t> producers_done{0};
  std::atomic<std::size_t> delivered{0};
  std::atomic<std::uint64_t> checksum{0};
  std::atomic<std::size_t> duplicate_ids{0};
  std::atomic<std::size_t> invalid_items{0};
  std::vector<std::thread> threads;
  threads.reserve(producer_count + consumer_count);

  for (std::uint32_t producer = 0; producer < producer_count; ++producer) {
    threads.emplace_back([&, producer] {
      SpinBackoff spin;
      YieldBackoff yield;
      for (std::uint32_t sequence = 0; sequence < items_per_producer;
           ++sequence) {
        const std::uint64_t id =
            static_cast<std::uint64_t>(producer) * items_per_producer + sequence;
        StressItem item{id, producer, sequence};
        while (!queue.try_push(std::move(item))) {
          spin.pause();
          yield.pause();
        }
      }
      producers_done.fetch_add(1, std::memory_order_release);
    });
  }

  for (std::uint32_t consumer = 0; consumer < consumer_count; ++consumer) {
    threads.emplace_back([&] {
      SpinBackoff spin;
      YieldBackoff yield;
      for (;;) {
        const auto item = queue.try_pop();
        if (item.has_value()) {
          if (item->producer >= producer_count ||
              item->sequence >= items_per_producer ||
              item->id != static_cast<std::uint64_t>(item->producer) *
                                  items_per_producer +
                              item->sequence) {
            invalid_items.fetch_add(1, std::memory_order_relaxed);
          } else {
            const std::size_t id = static_cast<std::size_t>(item->id);
            if (seen[id].fetch_add(1, std::memory_order_relaxed) != 0) {
              duplicate_ids.fetch_add(1, std::memory_order_relaxed);
            }

            for (;;) {
              std::uint32_t expected = next_sequence[item->producer].load(
                  std::memory_order_relaxed);
              if (expected == item->sequence) {
                if (next_sequence[item->producer].compare_exchange_weak(
                        expected, expected + 1, std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                  break;
                }
              } else if (expected > item->sequence) {
                invalid_items.fetch_add(1, std::memory_order_relaxed);
                break;
              } else {
                yield.pause();
              }
            }
            checksum.fetch_add(item->id, std::memory_order_relaxed);
          }
          delivered.fetch_add(1, std::memory_order_relaxed);
          continue;
        }

        if (producers_done.load(std::memory_order_acquire) == producer_count &&
            queue.empty_approx()) {
          break;
        }
        spin.pause();
        yield.pause();
      }
    });
  }

  for (std::thread& thread : threads) {
    thread.join();
  }

  const std::uint64_t expected_checksum =
      static_cast<std::uint64_t>(total_items) * (total_items - 1) / 2;
  EXPECT_EQ(delivered.load(std::memory_order_relaxed), total_items);
  EXPECT_EQ(duplicate_ids.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(invalid_items.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(checksum.load(std::memory_order_relaxed), expected_checksum);
  EXPECT_TRUE(queue.empty_approx());
  for (std::uint32_t producer = 0; producer < producer_count; ++producer) {
    EXPECT_EQ(next_sequence[producer].load(std::memory_order_relaxed),
              items_per_producer);
  }
  for (std::size_t id = 0; id < total_items; ++id) {
    EXPECT_EQ(seen[id].load(std::memory_order_relaxed), 1U);
  }
}

}
}
