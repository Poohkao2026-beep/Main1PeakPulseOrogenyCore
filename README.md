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

## Requirements

- CMake 3.25+
- GCC 12+, Clang 15+, or MSVC 19.34+
- Ninja recommended; CMake falls back to another available generator
- GoogleTest 1.15.2 and Google Benchmark 1.8.3 (fetched automatically by CMake)

## Build and test

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
