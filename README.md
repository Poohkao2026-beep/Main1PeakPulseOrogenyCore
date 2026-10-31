# Main1PeakPulseOrogenyCore

**A C++20 engine for moving telemetry fast and simulating mountains slowly.**

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![CMake](https://img.shields.io/badge/CMake-3.25%2B-green.svg)](https://cmake.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-brightgreen.svg)](LICENSE)

Telemetry "pulses" arrive from many threads, flow through a bounded MPMC ring
buffer, and are designed to drive a 3D model of geological stress propagation
(*orogeny*: the building of mountains). Today the repository ships the queue,
its tests, and a benchmark harness built to be hard to fool. The memory pool,
simulation grid, and worker pipeline come next.

## Status

| Module | State | Notes |
|---|---|---|
| Bounded MPMC ring buffer | Working | Unit tests and a 10M-item stress test pass on the `dev` preset |
| Performance harness (`peak_pulse_perf`) | Working | Schema-v1 JSON/CSV output, checksum-validated runs |
| ASan / UBSan / TSan | Not yet run | TSan requires Linux, WSL2, or CI |
| Chunked memory pool | Planned | Phase 2 |
| 3D SoA stress/strain grid | Planned | Phase 3 |
| Worker pool and pipeline | Planned | Phase 4 |

## Architecture

```text
  producers (N threads)                              consumers (M threads)
 ┌────────────┐                                     ┌────────────┐
 │ PulseEvent │──┐                               ┌──│  handler   │
 └────────────┘  │   ┌───────────────────────┐   │  └────────────┘
 ┌────────────┐  ├──▶│    MpmcRingBuffer     │───┤  ┌────────────┐
 │ PulseEvent │──┤   │  bounded, per-slot    │   ├──│  handler   │
 └────────────┘  │   │  sequence numbers     │   │  └────────────┘
 ┌────────────┐  │   └───────────────────────┘   │          │
 │ PulseEvent │──┘                               │          ▼
 └────────────┘                                  │   Orogeny grid (Phase 3)
                                                 │   3D SoA stress/strain
```

The queue sits at the center because everything else (pool, workers, grid)
either feeds it or drains it, so its correctness and latency bound the whole
system.

### Queue design

- **Bounded, array-based MPMC queue** in the style of Dmitry Vyukov's design,
  using per-slot sequence numbers to coordinate producers and consumers.
- **Power-of-two capacity of at least 2.** A one-slot ring cannot distinguish
  "full" from "free for the next lap", so capacities below 2 are rejected.
  The test suite surfaced this during development.
- **Design goals:** no heap allocation on push or pop, minimal cache-line
  contention between producers and consumers, and explicit element lifetimes.

## Build and test

Requires CMake 3.25+ and GCC 12+, Clang 15+, or MSVC 19.34+. GoogleTest 1.15.2
and Google Benchmark 1.8.3 are fetched automatically by CMake.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

| Preset | Purpose |
|---|---|
| `dev` | Fast iteration, tests enabled |
| `asan` | AddressSanitizer and UndefinedBehaviorSanitizer |
| `tsan` | ThreadSanitizer (separate from ASan; not available on MSVC) |
| `release` | Optimized build, LTO where supported |

Sanitizer support depends on your compiler and runtime. Run the `tsan` preset
on Linux, WSL2, or CI.

## Performance harness

`peak_pulse_perf` compares the ring buffer with a `std::mutex` + `std::deque`
baseline and validates delivered items. Every run checks exactly-once delivery
by checksum, and a mismatch marks the run INVALID instead of reporting a
number.

```sh
cmake --preset release
cmake --build --preset release
./build/release/peak_pulse_perf \
  --scenario all --producers 4 --consumers 4 --capacity 1024 \
  --items 100000 --duration-sec 5 --warmup-sec 2 --repetitions 30 \
  --seed 42 --json docs/perf-results.json --csv docs/perf-results.csv
```

On Windows, run `build\release\peak_pulse_perf.exe` instead. For charts:

```sh
python -m pip install -r tools/requirements.txt
python tools/plot_perf.py docs/perf-results.json --output-dir docs/img
```

Benchmark numbers from Debug or sanitizer builds are not valid, so use the
`release` preset.

The checked-in measurements are a short, unpinned Windows sample that
exercises the output and chart pipeline. They are not a performance claim.
See [benchmarking methodology](docs/benchmarking.md) for collection guidance
and limitations.

## Known limitations

- **Not strictly lock-free.** A producer descheduled between claiming and
  publishing a slot can delay consumers.
- **Bounded by design.** `try_push` fails when the queue is full; callers
  choose to drop, retry, or back off.
- **No blocking wait yet.** Spin and yield helpers are left to callers.
- **Sanitizer coverage is incomplete.** ASan, UBSan, and TSan runs are pending.
- **Single-machine measurements.** Results so far come from one Windows machine.

## Roadmap

1. Ring buffer, tests, and performance harness (done)
2. Chunked memory pool with an ABA-safe free list
3. SoA 3D grid with a tiled, double-buffered stencil
4. Worker pool with concepts, metrics, and graceful drain
5. CI matrix (GCC, Clang, MSVC; sanitizers) and a benchmark regression gate

## License

Released under the [MIT License](LICENSE).
