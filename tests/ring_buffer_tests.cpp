#include "peak_pulse/ring_buffer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace peak_pulse {
namespace {

static_assert(detail::valid_ring_buffer_capacity(2));
static_assert(detail::valid_ring_buffer_capacity(1024));
static_assert(!detail::valid_ring_buffer_capacity(1));
static_assert(!detail::valid_ring_buffer_capacity(0));
static_assert(!detail::valid_ring_buffer_capacity(3));

struct MoveOnly final {
  static inline std::atomic<int> live_instances{0};
  static inline std::atomic<int> constructions{0};
  static inline std::atomic<int> destructions{0};

  explicit MoveOnly(int input) noexcept : value(input) {
    live_instances.fetch_add(1, std::memory_order_relaxed);
    constructions.fetch_add(1, std::memory_order_relaxed);
  }

  MoveOnly(const MoveOnly&) = delete;
  MoveOnly& operator=(const MoveOnly&) = delete;

  MoveOnly(MoveOnly&& other) noexcept : value(other.value) {
    other.value = -1;
    live_instances.fetch_add(1, std::memory_order_relaxed);
    constructions.fetch_add(1, std::memory_order_relaxed);
  }

  MoveOnly& operator=(MoveOnly&& other) noexcept {
    value = other.value;
    other.value = -1;
    return *this;
  }

  ~MoveOnly() noexcept {
    live_instances.fetch_sub(1, std::memory_order_relaxed);
    destructions.fetch_add(1, std::memory_order_relaxed);
  }

  int value;
};

static_assert(NothrowMoveConstructible<MoveOnly>);
static_assert(!std::is_copy_constructible_v<MoveOnly>);
static_assert(MpmcRingBuffer<int, 8>::capacity() == 8);

TEST(RingBufferTests, StartsEmptyAndReportsCapacity) {
  MpmcRingBuffer<int, 8> queue;

  EXPECT_TRUE(queue.empty_approx());
  EXPECT_EQ(queue.size_approx(), 0U);
  EXPECT_EQ(queue.capacity(), 8U);
  EXPECT_FALSE(queue.try_pop().has_value());
}

TEST(RingBufferTests, PublishesSlotDataWithReleaseOrdering) {
  EXPECT_EQ(detail::sequence_publication_order, std::memory_order_release);
}

TEST(RingBufferTests, PreservesFifoOrder) {
  MpmcRingBuffer<int, 8> queue;

  ASSERT_TRUE(queue.try_push(10));
  ASSERT_TRUE(queue.try_push(20));
  ASSERT_TRUE(queue.try_push(30));
  EXPECT_EQ(queue.size_approx(), 3U);
  EXPECT_EQ(queue.try_pop(), 10);
  EXPECT_EQ(queue.try_pop(), 20);
  EXPECT_EQ(queue.try_pop(), 30);
  EXPECT_FALSE(queue.try_pop().has_value());
  EXPECT_TRUE(queue.empty_approx());
}

TEST(RingBufferTests, ReportsFullAndRecoversAfterDrain) {
  MpmcRingBuffer<int, 4> queue;

  for (int value = 0; value < 4; ++value) {
    ASSERT_TRUE(queue.try_push(value));
  }
  EXPECT_FALSE(queue.try_push(4));
  EXPECT_EQ(queue.size_approx(), 4U);

  for (int value = 0; value < 4; ++value) {
    ASSERT_EQ(queue.try_pop(), value);
  }
  EXPECT_TRUE(queue.empty_approx());
  EXPECT_TRUE(queue.try_push(4));
  EXPECT_EQ(queue.try_pop(), 4);
}

TEST(RingBufferTests, WrapsAroundRepeatedlyWithoutLossOrDuplication) {
  constexpr std::size_t iterations = 100'000;
  MpmcRingBuffer<std::size_t, 16> queue;

  for (std::size_t value = 0; value < iterations; ++value) {
    ASSERT_TRUE(queue.try_push(value));
    ASSERT_EQ(queue.try_pop(), value);
  }
  EXPECT_TRUE(queue.empty_approx());
}

TEST(RingBufferTests, SupportsNothrowCopyableValues) {
  MpmcRingBuffer<int, 2> queue;
  const int value = 42;

  EXPECT_TRUE(queue.try_push(value));
  EXPECT_EQ(queue.try_pop(), value);
}

TEST(RingBufferTests, FailedPushDoesNotMoveFromInput) {
  MpmcRingBuffer<MoveOnly, 2> queue;
  MoveOnly first(1);
  MoveOnly second(2);
  MoveOnly rejected(3);

  ASSERT_TRUE(queue.try_push(std::move(first)));
  ASSERT_TRUE(queue.try_push(std::move(second)));
  ASSERT_EQ(first.value, -1);
  EXPECT_FALSE(queue.try_push(std::move(rejected)));
  EXPECT_EQ(rejected.value, 3);
  EXPECT_EQ(queue.try_pop()->value, 1);
  EXPECT_EQ(queue.try_pop()->value, 2);
}

TEST(RingBufferTests, DestroysEachMoveOnlyValueExactlyOnce) {
  MoveOnly::live_instances.store(0, std::memory_order_relaxed);
  MoveOnly::constructions.store(0, std::memory_order_relaxed);
  MoveOnly::destructions.store(0, std::memory_order_relaxed);

  {
    MpmcRingBuffer<MoveOnly, 4> queue;
    MoveOnly first(11);
    MoveOnly second(22);
    ASSERT_TRUE(queue.try_push(std::move(first)));
    ASSERT_TRUE(queue.try_push(std::move(second)));
    EXPECT_EQ(queue.try_pop()->value, 11);
    EXPECT_EQ(MoveOnly::live_instances.load(std::memory_order_relaxed), 3);
  }

  EXPECT_EQ(MoveOnly::live_instances.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(MoveOnly::constructions.load(std::memory_order_relaxed),
            MoveOnly::destructions.load(std::memory_order_relaxed));
}

TEST(RingBufferTests, DestructorDrainsRemainingValues) {
  MoveOnly::live_instances.store(0, std::memory_order_relaxed);
  MoveOnly::constructions.store(0, std::memory_order_relaxed);
  MoveOnly::destructions.store(0, std::memory_order_relaxed);

  {
    MpmcRingBuffer<MoveOnly, 4> queue;
    MoveOnly first(11);
    MoveOnly second(22);
    ASSERT_TRUE(queue.try_push(std::move(first)));
    ASSERT_TRUE(queue.try_push(std::move(second)));
    EXPECT_EQ(MoveOnly::live_instances.load(std::memory_order_relaxed), 4);
  }

  EXPECT_EQ(MoveOnly::live_instances.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(MoveOnly::constructions.load(std::memory_order_relaxed),
            MoveOnly::destructions.load(std::memory_order_relaxed));
}

}
}
