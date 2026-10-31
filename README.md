# Main1PeakPulseOrogenyCore

A C++20 foundation for a real-time simulation engine that models 3D geological
stress propagation and ingests telemetry through bounded concurrent queues.
This initial implementation focuses on a bounded MPMC ring buffer and a
reproducible queue performance harness.

## Requirements

- CMake 3.25 or newer
- GCC 12+, Clang 15+, or MSVC 19.34+
- Ninja is recommended; CMake can use another available generator
- GoogleTest 1.15.2 and Google Benchmark 1.8.3 are fetched with CMake

## Build and test

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

The `asan` preset enables AddressSanitizer and UndefinedBehaviorSanitizer; the
`tsan` preset is separate. Sanitizer support depends on the selected compiler
and runtime. The `release` preset enables interprocedural optimization when
supported.

## Performance harness

`peak_pulse_perf` compares the ring buffer with a `std::mutex` +
`std::deque` baseline. It validates delivered items and writes schema-version-1
JSON and CSV results.

```sh
cmake --preset release
cmake --build --preset release
./build/release/peak_pulse_perf \
  --scenario all --producers 4 --consumers 4 --capacity 1024 \
  --items 100000 --duration-sec 5 --warmup-sec 2 --repetitions 30 \
  --seed 42 --json docs/perf-results.json --csv docs/perf-results.csv
```

On Windows, run `build\release\peak_pulse_perf.exe` instead. For charts, install
the optional plotting dependency and pass the JSON file to the plotting script:

```sh
python -m pip install -r tools/requirements.txt
python tools/plot_perf.py docs/perf-results.json --output-dir docs/img
```

The checked-in measurements are a short, unpinned Windows sample intended to
exercise the output and chart pipeline, not a general performance claim. See
[benchmarking methodology](docs/benchmarking.md) for collection guidance and
limitations.
