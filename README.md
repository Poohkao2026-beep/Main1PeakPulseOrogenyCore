# PeakPulse Orogeny Core

A C++20 library providing a bounded multi-producer/multi-consumer (MPMC) ring
buffer, together with a benchmark harness that compares it against a
mutex-based queue.

## Status

Early-stage. The repository currently contains:

- A bounded MPMC ring buffer for passing fixed-size messages between threads
- Unit and stress tests (GoogleTest)
- A benchmark harness (`peak_pulse_perf`) with checksum-validated runs and
  JSON/CSV output

It does **not** yet contain:

- Any geological or stress-propagation model
- A simulation loop, 3D grid, or mesh
- A telemetry ingestion pipeline, worker pool, or memory pool
- Real-time guarantees. No latency bounds have been measured or claimed.

## Project goal

The long-term aim is a simulation engine that models 3D geological stress
propagation and ingests telemetry through bounded concurrent queues. The queue
is built and validated first because the later components depend on it. The
simulation goals are a roadmap, not a description of current functionality.

## Verification status

| Check | Result |
|---|---|
| Unit and stress tests, `dev` preset | Passing (9 tests, including a 10-million-item MPMC stress test) |
| AddressSanitizer / UndefinedBehaviorSanitizer | Not yet run |
| ThreadSanitizer | Not yet run (requires Linux, WSL2, or CI) |
| Cross-platform builds | Windows only so far |
| Performance results | Preliminary, single machine, unpinned |

## Queue design

The ring buffer is a bounded, array-based MPMC queue in the style of Dmitry
Vyukov's design. Each slot carries a sequence number that coordinates producers
and consumers.

- Capacity must be a power of two and at least 2. With one slot, "full" and
  "free for the next lap" cannot be distinguished, so smaller capacities are
  rejected.
- `try_push` and `try_pop` do not block. A full or empty queue is reported to
  the caller, who decides whether to drop, retry, or back off.
**A C++20 engine for moving telemetry fast and simulating mountains slowly.**

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![CMake](https://img.shields.io/badge/CMake-3.25%2B-green.svg)](https://cmake.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-brightgreen.svg)](LICENSE)

- CMake 3.25+
- GCC 12+, Clang 15+, or MSVC 19.34+
- Ninja recommended; CMake falls back to another available generator
- GoogleTest 1.15.2 and Google Benchmark 1.8.3 (fetched automatically by CMake)
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

Other presets:

- `asan`: AddressSanitizer and UndefinedBehaviorSanitizer
- `tsan`: ThreadSanitizer (separate, because it cannot be combined with ASan)
- `release`: optimized build, with interprocedural optimization when supported

Sanitizer availability depends on the compiler and platform. ThreadSanitizer
is not available with MSVC.

## Performance harness

`peak_pulse_perf` compares the ring buffer against a `std::mutex` +
`std::deque` baseline. Each run verifies exactly-once delivery by checksum; a
failed check invalidates the run. Results are written as schema-version-1 JSON
and CSV.
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

On Windows, use `build\release\peak_pulse_perf.exe`.

Optional plotting:
On Windows, run `build\release\peak_pulse_perf.exe` instead. For charts:

```sh
python -m pip install -r tools/requirements.txt
python tools/plot_perf.py docs/perf-results.json --output-dir docs/img
```

Results from Debug or sanitizer builds are not valid for performance
comparison.

### About the checked-in results

The files in `docs/` come from a short, unpinned run on a single Windows
machine. They demonstrate that the output and plotting pipeline works. They
are not evidence of general performance. Results depend on CPU, core count,
thread pinning, OS scheduling, and contention. Run the harness on your own
hardware before drawing conclusions. See
[benchmarking methodology](docs/benchmarking.md) for guidance and limitations.

## Known limitations

- The queue is not strictly lock-free. A producer that is descheduled between
  claiming and publishing a slot can delay consumers.
- No blocking wait is provided. Spin and yield strategies are left to callers.
- Sanitizer and multi-platform coverage is incomplete (see the table above).

## Roadmap

Planned, with no fixed order or timeline:

- [ ] Memory pool for fixed-size blocks
- [ ] Telemetry message types and ingestion layer
- [ ] Spatial data structures for the 3D domain
- [ ] Stress propagation model and numerical validation
- [ ] Worker pool, simulation loop, and scheduling
- [ ] CI matrix with sanitizers and a benchmark regression gate

## License

Released under the MIT License. See [LICENSE](LICENSE).
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
