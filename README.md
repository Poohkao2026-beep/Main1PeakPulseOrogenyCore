# PeakPulse Orogeny Core

A C++20 bounded multi-producer/multi-consumer (MPMC) ring buffer, with unit tests, stress tests, and a benchmark against a mutex-backed queue.

This is the queue component of a proposed geological simulation project. The repository does not yet implement a 3D model, a telemetry pipeline, or real-time guarantees.

## Try the queue

The queue is header-only. This example fills a two-slot queue, observes backpressure, then reuses a freed slot.

```cpp
#include <cassert>
#include <cstdint>
#include "peak_pulse/ring_buffer.hpp"

int main() {
    peak_pulse::MpmcRingBuffer<std::uint64_t, 2> queue;
    assert(!queue.try_pop());

    const bool first = queue.try_push(10);
    const bool second = queue.try_push(20);
    const bool overflow = queue.try_push(30);
    assert(first && second && !overflow);

    const auto item = queue.try_pop();
    assert(item && *item == 10);

    const bool reused = queue.try_push(30);
    assert(reused);

    const auto next = queue.try_pop();
    const auto last = queue.try_pop();
    assert(next && *next == 20 && last && *last == 30);
    assert(!queue.try_pop());
}
```

Save it as `example.cpp` in the repository root. With GCC or Clang:

```sh
c++ -std=c++20 -pthread -Iinclude example.cpp -o example
./example
```

## Build and test the project

Requires CMake 3.25+ and GCC 12+, Clang 15+, or MSVC 19.34+. CMake fetches GoogleTest and Google Benchmark.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

Other presets: `asan` for AddressSanitizer and UndefinedBehaviorSanitizer, `tsan` for ThreadSanitizer on supported platforms, and `release` for benchmarks.

## Design and limits

- The array-based queue uses per-slot sequence numbers to coordinate producers and consumers. Capacity must be a power of two and at least two.
- Operations do not wait for capacity or data. On a full queue, `try_push` returns false; on an empty queue, `try_pop` returns an empty optional. Callers choose whether to retry, back off, or drop.
- The queue is not strictly lock-free. A producer paused after it claims a slot but before it publishes the value can delay consumers.
- The benchmark checks counts and checksums, but those checks do not prove exactly-once delivery. Checked-in numbers are preliminary samples from one unpinned Windows run, not a general performance claim. See the [benchmarking guide](docs/benchmarking.md) for the reproducible commands and interpretation.

Small, reproducible examples with a compiler version and build preset are especially helpful when reporting a problem.

## License

[MIT](LICENSE)
