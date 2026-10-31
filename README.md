# Main1PeakPulseOrogenyCore

**A C++20 engine for moving telemetry fast and simulating mountains slowly.**

PeakPulseOrogenyCore is a real-time simulation core. Telemetry "pulses" arrive
from many threads, flow through bounded lock-free queues, and are meant to
drive a 3D model of geological stress propagation (*orogeny*: the building of
mountains).

This is an early but honest foundation. Today it ships a bounded MPMC ring
buffer and a benchmark harness built to be hard to fool. The simulation grid,
memory pool, and worker pipeline are planned next.

---

## Status

| Module | State | Notes |
|---|---|---|
| Bounded MPMC ring buffer | Working | Unit tests + 10M-item stress test pass on `dev` |
| Performance harness (`peak_pulse_perf`) | Working | JSON/CSV output, checksum-validated runs |
| Sanitizer coverage (ASan/UBSan/TSan) | In progress | TSan needs Linux, WSL2, or CI |
| Chunked memory pool | Planned | Phase 2 |
| 3D SoA stress/strain grid | Planned | Phase 3 |
| Worker pool + telemetry pipeline | Planned | Phase 4 |

---

## Why this exists

Most queue benchmarks answer "how fast can it go when nothing is wrong?"
I wanted the more useful question: **how fast is it when I can prove it is
correct, and what does it cost when the system is busy?**

So the project follows three rules:

1. **Correctness first.** A fast queue that drops or duplicates one item in a
   billion is a broken queue. Every benchmark run verifies exactly-once
   delivery with a checksum, and a failed check marks the run INVALID instead
   of reporting a number.
2. **No unmeasured claims.** If a result isn't in `docs/` and reproducible
   from a command in this README, it doesn't belong here.
3. **Negative results are results.** Where something doesn't help, the code
   stays behind a flag and the write-up says so.

---

## Architecture

```text
  producers (N threads)                          consumers (M threads)
 ┌────────────┐                                 ┌────────────┐
 │ PulseEvent │──┐                           ┌──│  handler   │
 └────────────┘  │   ┌───────────────────┐   │  └────────────┘
 ┌────────────┐  ├──▶│  MpmcRingBuffer   │───┤  ┌────────────┐
 │ PulseEvent │──┤   │  bounded, no heap │   ├──│  handler   │
 └────────────┘  │   │  per-slot seq no. │   │  └────────────┘
 ┌────────────┐  │   └───────────────────┘   │          │
 │ PulseEvent │──┘                           │          ▼
 └────────────┘                              │   Orogeny grid (planned)
                                             │   SoA stress/strain, 3D
```

The queue sits at the center for a reason. Everything else (pool, workers,
grid) either feeds it or drains it, so its correctness and latency bound the
whole system.

### The queue in 60 seconds

`MpmcRingBuffer<T, Capacity>` follows the bounded-array design popularized by
Dmitry Vyukov:

- **Per-slot sequence numbers.** Each slot carries its own counter that says
  whose turn it is: a producer's or a consumer's, and on which lap around the
  ring. Producers and consumers claim positions with a compare-and-swap and
  then publish with a release store. No global lock, no allocation on push or
  pop.
- **Power-of-two capacity.** Indexing is a mask, not a modulo.
- **Cache-line separation.** Head, tail, and slot storage are kept apart to
  avoid false sharing between producer and consumer cores.
- **Explicit lifetimes.** Slots are raw aligned storage with explicit
  construction and destruction, and the destructor drains whatever is left.

#### Why capacity must be at least 2

A one-slot ring can't tell "full" from "free for the next lap": both states
look the same in the sequence number. The queue therefore rejects
capacities below 2 at compile time. This bug was found by the test suite
during development, and the rule now has an explicit `static_assert`.

---

## Requirements

- CMake 3.25 or newer
- GCC 12+, Clang 15+, or MSVC 19.34+
- Ninja is recommended; CMake falls back to another available generator
- GoogleTest 1.15.2 and Google Benchmark 1.8.3, fetched automatically by CMake

## Build and test

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

### Presets

| Preset | Purpose |
|---|---|
| `dev` | Fast iteration, tests enabled |
| `asan` | AddressSanitizer + UndefinedBehaviorSanitizer |
| `tsan` | ThreadSanitizer (separate; cannot be combined with ASan) |
| `release` | Optimized build, LTO where the toolchain supports it |

Sanitizer support depends on your compiler and runtime. **ThreadSanitizer is
not available on MSVC**, so run the `tsan` preset on Linux, WSL2, or CI. For a
lock-free queue, that run is the most important one.

---

## What the tests check

- Single-thread FIFO order, plus full and empty behavior
- Wraparound across many multiples of the capacity
- A move-only type with a live-instance counter, which proves there are no
  leaks and no double destruction
- A multi-producer, multi-consumer stress test of 10 million items that
  verifies every item is delivered **exactly once**

Benchmark numbers from Debug or sanitizer builds are invalid, so the harness
warns when it detects either.

---

## Performance harness

`peak_pulse_perf` runs the ring buffer against a `std::mutex` + `std::deque`
baseline under identical conditions, with identical validation.

```sh
cmake --preset release
cmake --build --preset release
./build/release/peak_pulse_perf \
  --scenario all --producers 4 --consumers 4 --capacity 1024 \
  --items 100000 --duration-sec 5 --warmup-sec 2 --repetitions 30 \
  --seed 42 --json docs/perf-results.json --csv docs/perf-results.csv
```

On Windows, run `build\release\peak_pulse_perf.exe` instead.

### What it does to keep itself honest

- **Start barrier.** All threads begin together, so early starters don't
  inflate results.
- **Warmup, then repetitions.** Reported as median, mean, stddev, min, max,
  and p95, with a bootstrap 95% confidence interval on the median.
- **Checksum on every run.** A mismatch invalidates the run.
- **Open-loop latency.** Items are offered at a fixed rate, so queueing delay
  isn't hidden by a producer that politely slows down (coordinated omission).
- **No I/O or allocation in the timed region.**
- **Seeded randomness.** The seed is printed and stored in the output.
- **Environment capture.** Compiler, build type, CPU, core count, OS, and git
  commit travel with the results.

### Charts

```sh
python -m pip install -r tools/requirements.txt
python tools/plot_perf.py docs/perf-results.json --output-dir docs/img
```

### A word about the checked-in numbers

The measurements in `docs/` are a short, **unpinned Windows sample**. They
exist to prove the output and chart pipeline works, not to rank the queue
against anything. Reliable numbers need pinned cores, a fixed frequency
governor, and 30+ repetitions on a quiet machine. See
[benchmarking methodology](docs/benchmarking.md) for how to collect them and
what can go wrong.

---

## Known limitations

- **Not strictly lock-free.** This queue design can stall consumers if a
  producer is descheduled between claiming a slot and publishing it. It is
  fast and wait-free in the common case, but the formal progress guarantee is
  weaker than "lock-free".
- **Bounded by design.** When the queue is full, `try_push` fails. Callers
  decide whether to drop, retry, or apply backpressure.
- **No blocking wait yet.** Spin/yield helpers are for callers to compose.
- **Single-platform measurements so far.** No cross-machine results yet.

---

## Roadmap

1. ~~Bounded MPMC ring buffer, tests, performance harness~~
2. Chunked memory pool with an ABA-safe free list
3. 3D grid in SoA layout with a tiled, double-buffered stress/strain stencil
4. Worker pool using C++20 concepts, with metrics and graceful drain
5. CI matrix (GCC, Clang, MSVC; ASan, UBSan, TSan) and a benchmark
   regression gate

---

## Repository layout

```text
include/peak_pulse/   public headers
src/                  implementations
tests/                GoogleTest unit and stress tests
benchmarks/           Google Benchmark targets and peak_pulse_perf
tools/                plotting scripts
docs/                 methodology, results, charts
cmake/                build helper modules
```

## License

Standard MIT license

## Author

Made by Poohkao bug reports, and sharp criticism are welcome.
