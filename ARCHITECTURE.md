# TickForge System Architecture

**Status:** Day 01, current as of v0.1.0.
**Companion document:** `SPEC.md` (domain semantics). This document covers
system structure: layers, data flow, performance boundaries, and extension
points.

Design intent: the architecture must let future work optimize the hot path
aggressively without rewriting the system. Layers communicate through
narrow, versioned interfaces, and determinism is enforced by construction,
not by convention.

---

## 1. Layered architecture

```text
Data / Feed Layer
        |
Event Normalization
        |
Replay Engine
        |
Order Book (MBO)
        |
Matching Engine
        |
Microstructure Model
        |
Execution Simulator
        |
Statistics / Validation
```

### 1.1 Data / Feed Layer

Owns all contact with raw historical datasets: file reading, format
detection, decompression, and venue-specific quirks. It produces raw
records and knows nothing about simulation. This is the only layer allowed
to depend on dataset formats.

### 1.2 Event Normalization

Converts raw records into the canonical `Event` stream defined in
SPEC.md section 2: assigns dense `seq` numbers, validates fields, rejects
malformed records with reason codes, and sorts by `(timestamp, seq)`.
Output of this layer is the reproducibility boundary: identical normalized
streams MUST yield identical simulations regardless of source format.

### 1.3 Replay Engine

The deterministic driver. Feeds normalized events to the order book in
`(timestamp, seq)` order, advances the simulation clock (which is simply
the current event's timestamp), and emits the ordered event log. It owns
the determinism guarantee: no wall-clock reads, no uncontrolled
randomness, no observable hash-iteration order on this path.

### 1.4 Order Book (MBO)

The central state: individually tracked resting orders organized by
side and price level, per SPEC.md section 5. Answers queries such as
"best bid/ask", "quantity at level", and "queue position of order X".
The book itself never matches; it only stores and retrieves state.

### 1.5 Matching Engine

Applies incoming orders to the book under a configured matching rule
(default: price-time priority, SPEC.md section 6). Produces fill records
and book updates. Matching rules are pluggable: the default rule is one
implementation of a narrow `MatchingRule` interface, so alternatives
(e.g. pro-rata) can be added without touching the book.

### 1.6 Microstructure Model

Models effects beyond plain matching: queue-position dynamics, latency
between decision and execution, and eventually fee/rebate accounting.
Consumes fill records and book states; produces adjusted execution
outcomes. All stochastic elements here MUST be explicitly seeded
deterministic PRNGs (SPEC.md section 4.3).

### 1.7 Execution Simulator

Orchestrates designated "simulated orders" (the orders whose execution
quality is being studied) through the replay: injects them at configured
times, tracks their queue positions and fills via the microstructure
model, and records their full lifecycle traces.

### 1.8 Statistics / Validation

Computes summary statistics (fill rates, slippage vs. reference, queue
wait distributions) and validates runs: determinism self-checks
(re-running a prefix must reproduce its log), invariant checks (e.g.
quantity conservation), and comparison against reference market data.
This layer reads state but MUST NOT mutate the book.

---

## 2. Hot path and cold path

### 2.1 Hot path

The per-event inner loop:

```text
Replay Engine -> Order Book lookup -> Matching Engine -> state update
```

This path runs once per event, so it dominates runtime on large datasets.
Design constraints for the hot path:

* No heap allocation per event in steady state (use pools/arenas).
* No virtual dispatch where a branch or template parameter will do; where
  polymorphism is needed, keep call targets predictable.
* Cache-friendly book layout: price levels in contiguous structures,
  orders in intrusive nodes.
* No I/O, no logging, no string formatting on the hot path. Observability
  hooks write to preallocated buffers consumed on the cold path.

Day 01 implements none of this yet; the constraints exist so that the
interfaces designed now do not preclude them later.

### 2.2 Cold path

Everything else: parsing and normalization, configuration loading,
statistics computation, report generation, benchmark harness control.
The cold path MAY allocate freely, use standard containers, and perform
I/O. It must still be deterministic where its outputs feed the
reproducibility guarantee (e.g. statistics must not depend on thread
scheduling).

---

## 3. Public API (planned)

The public surface lives in `include/tickforge/` under namespace
`tickforge`. Day 01 ships only `version.hpp`; the following is the
intended shape, not implemented code:

* `Event`, `OrderId`, `Price`, `Quantity`, `Side`, `TimestampNs`: strong
  typedefs over the integer representations in SPEC.md section 2.4.
* `SessionConfig`: parsed session configuration (tick/lot sizes, rule
  selection, seeds, error mode).
* `ReplayEngine`: constructed from a `SessionConfig` and a normalized
  event range; exposes `run()` and per-event callbacks for the statistics
  layer.
* `OrderBook`: MBO state with query methods; not directly mutated by
  users, only through the replay engine.
* `MatchingRule`: interface with a single default implementation
  (`PriceTimePriority`).

API stability rule: anything under `include/tickforge/` is public and
requires a minor version bump to change incompatibly before 1.0.

---

## 4. Future extension points

Each extension point is a narrow interface, implemented later:

| Extension point      | Interface idea | Example future use |
|----------------------|----------------|--------------------|
| Feed parsers         | `Parser`       | New venue data formats |
| Matching rules       | `MatchingRule` | Pro-rata matching |
| Microstructure models| `MicrostructureModel` | Latency distributions, fees |
| Metrics sinks        | `MetricsSink`  | Prometheus export, CSV traces |
| Validation checks    | `InvariantCheck` | Quantity conservation audits |

Extension points MUST NOT compromise the deterministic core: a new model
that cannot run deterministically does not ship.

---

## 5. Deterministic boundaries

Determinism is enforced by drawing a hard line through the system:

* **Inside the boundary** (Replay Engine, Order Book, Matching Engine,
  Microstructure Model, Execution Simulator): no wall-clock time, no
  unseeded randomness, no thread-scheduling-dependent behavior, no
  observable unordered-container iteration. Code review MUST treat a
  violation here as a defect.
* **Outside the boundary** (Feed Layer I/O, Statistics reporting,
  benchmark harness timing): wall-clock and OS interaction are allowed,
  but anything they produce that feeds back into a run (e.g. a measured
  parameter) MUST be recorded into the configuration so the run stays
  reproducible.

The normalized event stream (output of Event Normalization) is the
checkpoint: everything downstream of it is pure function of
(events, config, version).

---

## 6. Repository map (Day 01)

```text
tickforge/
  include/tickforge/      public headers (version.hpp.in template today)
  src/                    implementation units (main.cpp today)
  tests/                  unit tests (GoogleTest)
  benchmarks/             benchmark harness (arrives with the benchmark milestone)
  examples/               usage examples (arrive with the first real API)
  docs/architecture/      design notes and diagrams
  .github/workflows/      CI
```

The `src/` tree will grow one directory per layer (e.g. `src/replay/`,
`src/book/`, `src/matching/`) as milestones land. No layer may include
headers from a layer above it in the diagram except through the
extension-point interfaces.
