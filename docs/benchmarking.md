# Performance harness methodology

`peak_pulse_perf` runs the bounded ring buffer and a `std::mutex` +
`std::deque` baseline with the same producer/consumer topology and item
validation. Throughput repetitions are calibrated to the requested duration;
a separate warmup runs before each measured repetition.
Latency samples use an open-loop producer schedule and record the time at
push intent, so producer delays do not reset the schedule. The four offered
rates are 25%, 50%, 75%, and 90% of the measured median throughput.

The fixed-seed bootstrap resamples repetition medians to estimate a 95%
confidence interval. Reported latency percentiles are upper bounds from a
logarithmic histogram with 128 subdivisions per power-of-two range; `max_ns`
is the observed maximum. The harness validates the delivered IDs and checksum
for warmup, calibration, measured, and latency runs.

## Recommended collection procedure

Use the release build and retain the generated JSON, CSV, and environment
metadata. For a more stable comparison, collect at least 30 repetitions and
use a longer duration than the smoke-test sample:

```powershell
cmake --preset release
cmake --build --preset release -j
.\build\release\peak_pulse_perf.exe --scenario all --producers 4 --consumers 4 --capacity 1024 --items 100000 --duration-sec 5 --warmup-sec 2 --repetitions 30 --pin --seed 42 --json docs\perf-results.json --csv docs\perf-results.csv
python tools\plot_perf.py docs\perf-results.json --output-dir docs\img
```

For defensible comparisons, run on an otherwise idle machine, pin workers to
available cores, keep the same CPU governor/power profile, and keep both runs
on the same NUMA node. Record those settings alongside the JSON; the harness
captures CPU model and logical-core count but does not select NUMA nodes,
change frequency governors, or collect hardware counters. On Linux, external
`perf stat` collection can be used, but its results are not currently part of
the JSON schema.

The `std::deque` baseline may allocate while pushing; that allocator cost is
part of the baseline being measured. The ring-buffer hot path itself does not
allocate. The checked-in `perf-results.json` is a short, unpinned Windows
sample (three repetitions with a 0.1-second requested duration) intended to
exercise the output and chart pipeline, not to support a general performance
claim. The throughput chart compares workload shapes with different thread
counts; it is not a controlled single-topology scaling study.
