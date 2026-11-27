# Use an installed PeakPulse queue from CMake

This standalone example links the installed C++20 queue without using the
PeakPulse source tree as a subdirectory. Its test verifies FIFO order, a
rejected push to a full queue, and reuse of a freed slot.

From the PeakPulse repository root, install the headers and target export:

```sh
cmake -S . -B build/package -DPEAK_PULSE_BUILD_TESTS=OFF -DPEAK_PULSE_BUILD_BENCHMARKS=OFF
cmake --install build/package --prefix "${PWD}/build/prefix"
```

Configure, build, and run the separate consumer:

```sh
cmake -S examples/installed-consumer -B build/consumer -DCMAKE_PREFIX_PATH="${PWD}/build/prefix"
cmake --build build/consumer --config Release
ctest --test-dir build/consumer -C Release --output-on-failure
```

The current install exports `peak_pulse::peak_pulse` but does not provide a
package config file, so the example imports the generated targets file
directly. For a custom installation layout, pass
`-DPEAK_PULSE_TARGETS_FILE=/absolute/path/to/PeakPulseOrogenyCoreTargets.cmake`
instead of relying on the prefix search.
