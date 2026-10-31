#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace peak_pulse {

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#endif
#if defined(__cpp_lib_hardware_interference_size) && \
    __cpp_lib_hardware_interference_size >= 201703L
inline constexpr std::size_t cache_line_size =
    std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t cache_line_size = 64;
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

static_assert(cache_line_size >= alignof(std::atomic<std::size_t>));

template <typename T>
concept NothrowMoveConstructible =
    std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>;

namespace detail {

[[nodiscard]] consteval bool valid_ring_buffer_capacity(std::size_t capacity) {
  return capacity >= 2 && (capacity & (capacity - 1)) == 0;
}

#if defined(PEAK_PULSE_MUTATE_WEAKEN_ORDER)
inline constexpr std::memory_order sequence_publication_order =
    std::memory_order_relaxed;
#else
inline constexpr std::memory_order sequence_publication_order =
    std::memory_order_release;
#endif

}

template <NothrowMoveConstructible T, std::size_t Capacity>
class MpmcRingBuffer final {
  // With capacity one, the occupied sequence (position + 1) aliases the
  // next producer lap's free sequence; full and reusable cannot be distinguished.
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                "Capacity must be at least 2 and a power of two");
  static_assert(Capacity <= (static_cast<std::size_t>(-1) / 2),
                "Capacity must be less than half the unsigned index range");

  struct alignas(cache_line_size) Slot final {
    std::atomic<std::size_t> sequence{0};
    alignas(T) std::byte storage[sizeof(T)];
  };

  static constexpr std::size_t index_mask = Capacity - 1;
  static constexpr std::size_t half_index_range =
      static_cast<std::size_t>(-1) / 2 + 1;

 public:
  MpmcRingBuffer() noexcept {
    for (std::size_t index = 0; index < Capacity; ++index) {
      slots_[index].sequence.store(index, std::memory_order_relaxed);
    }
  }

  ~MpmcRingBuffer() noexcept {
    // Destruction requires all producers and consumers to have stopped.
    const std::size_t begin = dequeue_position_.load(std::memory_order_relaxed);
    const std::size_t end = enqueue_position_.load(std::memory_order_relaxed);
    for (std::size_t position = begin; position != end; ++position) {
      Slot& slot = slots_[position & index_mask];
      if (slot.sequence.load(std::memory_order_relaxed) == position + 1) {
        std::destroy_at(object_at(slot));
      }
    }
  }

  MpmcRingBuffer(const MpmcRingBuffer&) = delete;
  MpmcRingBuffer& operator=(const MpmcRingBuffer&) = delete;
  MpmcRingBuffer(MpmcRingBuffer&&) = delete;
  MpmcRingBuffer& operator=(MpmcRingBuffer&&) = delete;

  [[nodiscard]] bool try_push(T&& value) noexcept {
    std::size_t position = enqueue_position_.load(std::memory_order_relaxed);
    Slot* slot = nullptr;

    for (;;) {
      slot = &slots_[position & index_mask];
      // Acquire pairs with the consumer's release after freeing this slot.
      const std::size_t sequence =
          slot->sequence.load(std::memory_order_acquire);
      const std::size_t difference = sequence - position;

      if (difference == 0) {
        // The relaxed CAS reserves a unique position; it does not publish T.
        if (enqueue_position_.compare_exchange_weak(
                position, position + 1, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
          break;
        }
      } else if (difference < half_index_range) {
        // A later sequence means another producer advanced the reservation.
        position = enqueue_position_.load(std::memory_order_relaxed);
      } else {
        return false;
      }
    }

    std::construct_at(raw_storage(*slot), std::move(value));
    // Release publishes the constructed object to a consumer's acquire load.
    slot->sequence.store(position + 1,
                         detail::sequence_publication_order);
    return true;
  }

  [[nodiscard]] bool try_push(const T& value) noexcept
    requires std::is_nothrow_copy_constructible_v<T>
  {
    std::size_t position = enqueue_position_.load(std::memory_order_relaxed);
    Slot* slot = nullptr;

    for (;;) {
      slot = &slots_[position & index_mask];
      // Acquire pairs with the consumer's release after freeing this slot.
      const std::size_t sequence =
          slot->sequence.load(std::memory_order_acquire);
      const std::size_t difference = sequence - position;

      if (difference == 0) {
        // The relaxed CAS reserves a unique position; it does not publish T.
        if (enqueue_position_.compare_exchange_weak(
                position, position + 1, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
          break;
        }
      } else if (difference < half_index_range) {
        position = enqueue_position_.load(std::memory_order_relaxed);
      } else {
        return false;
      }
    }

    std::construct_at(raw_storage(*slot), value);
    // Release publishes the constructed object to a consumer's acquire load.
    slot->sequence.store(position + 1,
                         detail::sequence_publication_order);
    return true;
  }

  [[nodiscard]] std::optional<T> try_pop() noexcept {
    std::size_t position = dequeue_position_.load(std::memory_order_relaxed);
    Slot* slot = nullptr;

    for (;;) {
      slot = &slots_[position & index_mask];
      const std::size_t expected_sequence = position + 1;
      // Acquire observes the producer's object construction and publication.
      const std::size_t sequence =
          slot->sequence.load(std::memory_order_acquire);
      const std::size_t difference = sequence - expected_sequence;

      if (difference == 0) {
        // The relaxed CAS reserves a unique position; it does not free the slot.
        if (dequeue_position_.compare_exchange_weak(
                position, position + 1, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
          break;
        }
      } else if (difference < half_index_range) {
        position = dequeue_position_.load(std::memory_order_relaxed);
      } else {
        return std::nullopt;
      }
    }

    T* object = object_at(*slot);
    std::optional<T> result(std::in_place, std::move(*object));
    std::destroy_at(object);
    // Release publishes object destruction and slot reuse to the next producer.
    slot->sequence.store(position + Capacity, std::memory_order_release);
    return result;
  }

  [[nodiscard]] std::size_t size_approx() const noexcept {
    const std::size_t enqueued =
        enqueue_position_.load(std::memory_order_relaxed);
    const std::size_t dequeued =
        dequeue_position_.load(std::memory_order_relaxed);
    const std::size_t observed = enqueued - dequeued;
    return observed < Capacity ? observed : Capacity;
  }

  [[nodiscard]] static constexpr std::size_t capacity() noexcept {
    return Capacity;
  }

  [[nodiscard]] bool empty_approx() const noexcept {
    return size_approx() == 0;
  }

  [[nodiscard]] bool atomic_operations_are_lock_free() const noexcept {
    if (!enqueue_position_.is_lock_free() || !dequeue_position_.is_lock_free()) {
      return false;
    }
    for (const Slot& slot : slots_) {
      if (!slot.sequence.is_lock_free()) {
        return false;
      }
    }
    return true;
  }

 private:
  [[nodiscard]] static T* object_at(Slot& slot) noexcept {
    return std::launder(reinterpret_cast<T*>(slot.storage));
  }

  [[nodiscard]] static T* raw_storage(Slot& slot) noexcept {
    return reinterpret_cast<T*>(slot.storage);
  }

  alignas(cache_line_size) std::atomic<std::size_t> enqueue_position_{0};
  alignas(cache_line_size) std::atomic<std::size_t> dequeue_position_{0};
  alignas(cache_line_size) std::array<Slot, Capacity> slots_{};
};

class SpinBackoff final {
 public:
  void pause() noexcept {
#if (defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))) || \
    defined(__i386__) || defined(__x86_64__)
    _mm_pause();
#else
    std::atomic_signal_fence(std::memory_order_relaxed);
#endif
  }
};

class YieldBackoff final {
 public:
  void pause() const noexcept {
    std::this_thread::yield();
  }
};

}
