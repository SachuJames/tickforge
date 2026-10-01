# TickForge

An open-source deterministic market microstructure simulator for high-fidelity order-level execution research.

**Status:** Early development
**Version:** 0.1.0

---

## Why TickForge

Backtesting on bars or aggregated quotes hides the mechanics that decide real execution quality: where your order sits in the queue, how much quantity is ahead of it, and exactly which events moved the book before you traded. TickForge replays historical market activity order by order, with nanosecond timestamps and a strict determinism guarantee, so execution research is reproducible down to the individual fill.

## Current capabilities (v0.1.0)

Day 01 establishes the engineering foundation only:

* C++20 project that configures, compiles, and runs (`tickforge` prints its version and exits cleanly).
* Single-source-of-truth versioning (`tickforge::kVersion`, generated from `CMakeLists.txt`).
* GoogleTest smoke tests wired through CTest.
* CMake presets for Debug, Release, and sanitizer (ASan + UBSan) builds.
* clang-format and clang-tidy configuration.
* GitHub Actions CI (build matrix, tests, format check, sanitizer build).

The matching engine, order book, replay engine, and benchmarks are **specified but not implemented**. See SPEC.md and ARCHITECTURE.md for the design they will follow.

## Planned capabilities

* Normalized event model with nanosecond timestamps and logical sequencing (SPEC.md section 2-3).
* Deterministic event replay with a formal reproducibility guarantee (SPEC.md section 4).
* Market-by-order book with price-time priority matching (SPEC.md section 5-6).
* Queue-position tracking: quantity ahead of a simulated order at its price level (SPEC.md section 8).
* Execution simulator with microstructure models (latency, fees) and statistics/validation.
* Published reproducibility and latency benchmark with full methodology (BENCHMARKING.md).

## Architecture

TickForge is a layered C++20 system:

```text
Data / Feed Layer -> Event Normalization -> Replay Engine -> Order Book (MBO)
  -> Matching Engine -> Microstructure Model -> Execution Simulator
  -> Statistics / Validation
```

The replay engine, book, and matching engine form the deterministic core: no wall-clock reads, no uncontrolled randomness. See ARCHITECTURE.md for hot/cold path design, public API plans, and extension points.

## Build

Requirements: CMake 3.22+, a C++20 compiler (GCC 11+ or Clang 14+), Git, and network access on first configure (GoogleTest is fetched via CMake FetchContent).

```bash
cmake --preset default
cmake --build --preset default
./build/tickforge
```

Release build:

```bash
cmake --preset release
cmake --build --preset release
```

Sanitizer build (AddressSanitizer + UndefinedBehaviorSanitizer):

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

## Test

```bash
ctest --preset default
# or, from the build directory:
ctest --output-on-failure
```

## Benchmarking

Performance benchmarking arrives with the benchmark milestone. Per project rule, every performance claim must ship with a benchmark and full methodology (dataset, hardware, compiler, events, runtime, events/sec, median/p99 latency). There are no performance claims in v0.1.0. See BENCHMARKING.md.

## Roadmap (1.5 months)

* **Week 1:** Foundation (this release) plus the normalized event model and parser interface.
* **Week 2:** Market-by-order book and price-time priority matching engine, with unit tests.
* **Week 3:** Deterministic replay engine and queue-position tracking.
* **Week 4:** Execution simulator, microstructure models, statistics and validation layer.
* **Week 5:** Benchmark harness, determinism self-checks, reproducibility tooling.
* **Week 6:** Documentation polish, examples, public reproducibility/latency benchmark.

## Contributing

See CONTRIBUTING.md. Bug reports and design discussion are welcome via GitHub issues.

## Security

See SECURITY.md for the vulnerability reporting policy.

## License

MIT. See LICENSE.
