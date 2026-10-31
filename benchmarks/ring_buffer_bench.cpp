#include "peak_pulse/ring_buffer.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

namespace {

peak_pulse::MpmcRingBuffer<std::uint64_t, 4096> ring_buffer;
std::mutex deque_mutex;
std::deque<std::uint64_t> mutex_deque;

template <typename Push, typename Pop>
void run_producer_consumer_benchmark(benchmark::State& state, Push&& push,
                                     Pop&& pop) {
  const int producer_count = state.threads() / 2;
  std::int64_t completed_operations = 0;
  peak_pulse::SpinBackoff spin;
  peak_pulse::YieldBackoff yield;
  std::uint64_t value = static_cast<std::uint64_t>(state.thread_index());

  for (auto _ : state) {
    if (state.thread_index() < producer_count) {
      while (!push(value)) {
        spin.pause();
        yield.pause();
      }
    } else {
      while (!pop(value)) {
        spin.pause();
        yield.pause();
      }
      benchmark::DoNotOptimize(value);
    }
    ++completed_operations;
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(completed_operations);
}

void BM_MpmcRingBuffer(benchmark::State& state) {
  run_producer_consumer_benchmark(
      state,
      [](std::uint64_t& value) {
        return ring_buffer.try_push(std::move(value));
      },
      [](std::uint64_t& value) {
        auto result = ring_buffer.try_pop();
        if (!result.has_value()) {
          return false;
        }
        value = *result;
        return true;
      });
}

void BM_MutexDeque(benchmark::State& state) {
  run_producer_consumer_benchmark(
      state,
      [](std::uint64_t& value) {
        const std::lock_guard<std::mutex> lock(deque_mutex);
        mutex_deque.push_back(value);
        return true;
      },
      [](std::uint64_t& value) {
        const std::lock_guard<std::mutex> lock(deque_mutex);
        if (mutex_deque.empty()) {
          return false;
        }
        value = mutex_deque.front();
        mutex_deque.pop_front();
        return true;
      });
}

BENCHMARK(BM_MpmcRingBuffer)
    ->Name("MpmcRingBuffer/1P1C")
    ->Threads(2)
    ->UseRealTime();
BENCHMARK(BM_MpmcRingBuffer)
    ->Name("MpmcRingBuffer/4P4C")
    ->Threads(8)
    ->UseRealTime();
BENCHMARK(BM_MpmcRingBuffer)
    ->Name("MpmcRingBuffer/8P8C")
    ->Threads(16)
    ->UseRealTime();
BENCHMARK(BM_MutexDeque)
    ->Name("MutexDeque/1P1C")
    ->Threads(2)
    ->UseRealTime();
BENCHMARK(BM_MutexDeque)
    ->Name("MutexDeque/4P4C")
    ->Threads(8)
    ->UseRealTime();
BENCHMARK(BM_MutexDeque)
    ->Name("MutexDeque/8P8C")
    ->Threads(16)
    ->UseRealTime();

}
