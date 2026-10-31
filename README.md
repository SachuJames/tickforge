<div align="center">

# TickForge

**Deterministic market-event replay and matching engine in C++20.**

<img src="docs/images/hero.webp" alt="TickForge" width="100%"/>

[![CI](https://github.com/SachuJames/tickforge/actions/workflows/ci.yml/badge.svg)](https://github.com/SachuJames/tickforge/actions/workflows/ci.yml)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C++-20-blue.svg)
![CMake](https://img.shields.io/badge/CMake-3.22+-064F8C.svg)

[SPEC.md](SPEC.md) · [ARCHITECTURE.md](ARCHITECTURE.md) · [BENCHMARKING.md](BENCHMARKING.md) · [CHANGELOG.md](CHANGELOG.md)

</div>

TickForge replays a stream of order events (new, modify, cancel) through a
market-by-order book with price-time priority matching. The same input always
produces the same fills, the same final book, and the same statistics, on any
machine: prices and quantities are integers (ticks and lots), event ordering
is explicit, and nothing in the core reads a clock or an unseeded random
source.

It is a research and verification tool, not a trading system. There is no
exchange connectivity, no live feed, no networking, and no order routing.

## Contents

- [What it does](#what-it-does)
- [Build](#build)
- [Quick start](#quick-start)
- [CLI reference](#cli-reference)
- [Input format](#input-format)
- [Determinism](#determinism)
- [Tests](#tests)
- [Layout](#layout)
- [Limitations](#limitations)
- [Version](#version)
- [License](#license)

## What it does

- Parses one documented CSV event format into normalized events, with
  one-based line diagnostics for malformed records.
- Maintains a market-by-order book: every order is tracked individually,
  with price-time priority, partial fills, and cancel/replace modify
  semantics.
- Replays events in strict mode (first invalid event fails the replay) or
  lenient mode (invalid events are skipped and counted). CSV parse errors
  are never skipped.
- Reports execution statistics (fill count, total/buy/sell quantity,
  min/max price, and an exact integer VWAP kept as a 128-bit numerator
  over the traded quantity), queue position for a designated order, and
  derived market-state views (best bid/ask, per-level aggregates).
- Runs sessions under an explicit configuration (instrument, tick size,
  lot size, matching rule, replay mode, optional PRNG seed, optional
  latency bounds) that is serialized canonically and identified by a
  deterministic hash, printed with every successful replay.
- Ships a CLI that replays a CSV file and prints a fixed summary.

Correctness is checked against an independent, scan-based reference
model: a generated 6,000-event session is run through both, and the
fills are compared field by field after every event, along with the
resting order count, best bid/ask, and the execution statistics. The
suite also has deterministic property tests and boundary tests at the
int64 limits.

## Build

Requirements: CMake 3.22 or newer, a C++20 compiler (GCC or Clang),
and Git. Building the tests fetches GoogleTest with CMake FetchContent on
first configure, so the first configure needs network access. The library
and the CLI have no third-party dependencies.

```bash
git clone https://github.com/SachuJames/tickforge.git
cd tickforge
cmake --preset default
cmake --build --preset default -j4
```

This produces `build/tickforge` (the CLI) and `build/tickforge_tests`.

Other presets: `release` (optimized build in `build-release/`) and `asan`
(Debug with AddressSanitizer and UndefinedBehaviorSanitizer in
`build-asan/`).

To install just the CLI executable from the release build (headers and
libraries are not installed; there is no stable library installation
contract yet):

```bash
cmake --install build-release --prefix /usr/local
```

## Quick start

The repository includes a five-event example session in
`examples/sample_session.csv`: an ask rests, a smaller bid rests, a second
bid crosses the ask for a partial fill, the ask is reduced, and the first
bid is cancelled.

```bash
./build/tickforge --input examples/sample_session.csv --instrument AAPL
```

Output:

```text
TickForge v0.2.0
Input: examples/sample_session.csv
Instrument: AAPL
Replay mode: strict
Events processed: 5
Events skipped: 0
Fills: 1
Resting orders: 1
Best bid: none
Best ask: 105
Config hash: 5dddb109ead805bc
Replay status: success
```

One order is left resting (the ask at 105, reduced to 4 lots), so there is
no best bid. The config hash identifies the exact session configuration
that produced this run; the same command on any machine prints the same
summary.

## CLI reference

```text
tickforge --input PATH --instrument SYMBOL [--mode strict|lenient]
                   [--tick-size VALUE] [--lot-size VALUE] [--seed VALUE]
                   [--latency-min-ns VALUE] [--latency-max-ns VALUE]
tickforge --help
tickforge --version
```

| Option | Required | Value |
|---|---|---|
| `--input PATH` | yes | CSV event file to replay. |
| `--instrument SYMBOL` | yes | Session instrument, e.g. `AAPL`. Non-empty, no `=`, newline, or carriage return. |
| `--mode` | no | `strict` (default) or `lenient`. |
| `--tick-size VALUE` | no | Tick size as a decimal string, e.g. `0.01`. Validated by the session configuration rules. |
| `--lot-size VALUE` | no | Lot size as a decimal string, e.g. `1`. Validated the same way. |
| `--seed VALUE` | no | PRNG seed, an unsigned 64-bit decimal integer (`0` to `18446744073709551615`). Stored in the session configuration and covered by its hash. Seeds the latency model when latency bounds are given. |
| `--latency-min-ns VALUE` | no | Minimum simulated latency in nanoseconds. Needs `--latency-max-ns` and `--seed`. |
| `--latency-max-ns VALUE` | no | Maximum simulated latency in nanoseconds. Needs `--latency-min-ns` and `--seed`. |
| `--help`, `--version` | no | Print usage or the version and exit 0. Invalid arguments are still errors when combined with these. |

Every option may appear at most once. A successful replay prints the
12-line summary to stdout and nothing to stderr. Failures print one
diagnostic to stderr and no summary.

With both latency bounds and a seed, the replay also draws one
simulated latency sample per fill from the seeded model and prints
them after the summary:

```console
$ ./build/tickforge --input examples/sample_session.csv --instrument AAPL --seed 42 --latency-min-ns 1000 --latency-max-ns 1999
...
Latency model: uniform-latency v1
Latency seed: 42
Latency min (ns): 1000
Latency max (ns): 1999
Latency samples: 1
Latency sample 0 (ns): 1413
```

The same seed, bounds, and input always produce the same samples.
The values are simulated for study, not measurements of any real
system, and enabling the model does not change the matching results
above it.

Exit codes:

| Code | Meaning |
|---|---|
| 0 | Success, `--help`, or `--version`. |
| 2 | Invalid arguments: unknown or duplicate option, missing or empty value, missing required option, malformed seed. |
| 3 | The input file could not be opened, or reading it failed. |
| 4 | CSV parsing failed; the diagnostic names the category and the one-based line. |
| 5 | Invalid session configuration, or the replay itself failed (in strict mode the diagnostic names the error and the event position). |
| 6 | An unexpected internal exception. |

## Input format

The first line must be exactly this header. Whitespace at the ends of
the line, and a trailing carriage return, are trimmed before the
comparison; the fields of each record are trimmed the same way.

```text
timestamp,event_type,order_id,side,price,quantity
```

Each record has six fields:

- `timestamp`: event time in nanoseconds, a non-negative integer.
- `event_type`: `N` (new order), `M` (modify), `C` (cancel).
- `order_id`: positive integer identifying the order.
- `side`: `B` (bid) or `A` (ask).
- `price`: price in ticks. Required for new orders; empty on a modify
  means unchanged, empty on a cancel means unused.
- `quantity`: quantity in lots. Required for new orders; on a modify, `0`
  or empty means unchanged. Reducing the quantity keeps queue priority;
  changing the price or increasing the quantity is a cancel/replace and
  the order rejoins at the back of its new level.

Timestamps must never decrease across the file; events that share a
timestamp are ordered by their position in the file. A record that
breaks the format stops parsing with an error; there is no
partial-success parse.

The full contract, including validation rules, matching semantics, the
configuration format and its canonical hash, and the aggregate-arithmetic
domain, is in [SPEC.md](SPEC.md). Design notes are in
[ARCHITECTURE.md](ARCHITECTURE.md).

## Determinism

- Prices and quantities are `int64` ticks and lots end to end; no floating
  point is used in the core.
- Events are ordered by `(timestamp, sequence)`; sequences are assigned
  densely in input order.
- Replaying the same file under the same configuration produces identical
  output, including the configuration hash.
- Aggregate quantities (total traded lots, per-level totals, quantity
  ahead of an order) are exact while their sum fits in `int64`; see
  SPEC.md section 2.4 for the supported domain.

## Tests

```bash
ctest --preset default --output-on-failure
# a small number of stress tests are gated behind an environment flag:
TICKFORGE_EXTENDED=1 ./build/tickforge_tests
```

The suite covers the event model, book, matching, analytics, CSV parser,
session configuration and hashing, replay, the CLI (run as a real
subprocess), and the reference-model differentials described above.
Formatting is checked with clang-format and static analysis with
clang-tidy. CI runs both, plus GCC and Clang Debug and Release builds
and the sanitizer build, on every push and pull request.

## Layout

```text
include/tickforge/   public headers (core, event, book, matching,
                     replay, analytics, market_data, model)
src/                 implementation, plus main.cpp (the CLI)
tests/               GoogleTest suites and the property-test support code
examples/            sample_session.csv, the quick-start session
SPEC.md              normative behavior contract
ARCHITECTURE.md      design notes and decision records
BENCHMARKING.md      benchmarking policy: what a performance claim
                     has to include before it counts as a result
```

## Limitations

- One instrument per session; no multi-instrument book.
- Price-time priority is the only matching rule.
- No persistence, no networking, no live data, no concurrency.
- Sessions are bounded by the aggregate domain in SPEC.md section 2.4.

## Version

Current version: 0.2.0 (see the `project()` declaration in
`CMakeLists.txt`; `tickforge --version` prints the same value). Changes
are recorded in [CHANGELOG.md](CHANGELOG.md).

## License

Apache License 2.0. See [LICENSE](LICENSE).
