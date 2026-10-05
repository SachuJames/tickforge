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

---

## 7. Event model placement (Day 02)

The canonical normalized event model lives in:

* `include/tickforge/event/` — public domain types: `Timestamp`,
  `Sequence`, `OrderId`, `Price`, `Quantity`, `Side`, `EventType`,
  `EventKey`, `Event`, and `validateEvent()`.
* `src/event/` — non-trivial function definitions (enum stringification,
  event validation), compiled into the `tickforge_event` static library.

Design notes:

* The normalized `Event` is the reproducibility boundary from section 5:
  parsers produce it, the replay engine will consume it.
* Total ordering lives with the model: `EventKey` plus `operator<` on
  `Event` implement the `(timestamp, seq)` order from SPEC.md 3.2. The
  future replay engine sorts by this operator; ordering logic is not
  duplicated in the engine.
* Construction never validates. `validateEvent()` is the explicit gate
  between parsing and replay, returning a reason-coded error per
  SPEC.md section 11.
* No order-book, matching, or replay logic lives here. Day 02 ends at the
  event model.

---

## 8. Replay driver and order book (Day 03)

Day 03 turns normalized events into deterministic market state:

```text
Normalized Events
       |
       v
Replay driver  (verify order, scope, validate, dispatch)
       |
       v
EventProcessor (narrow interface: one validated event in, applied or rejected)
       |
       v
OrderBook      (MBO resting-order state; no matching)
       |
       v
Market State   (best bid/ask, price levels, per-order state)
```

### 8.1 Replay driver

Lives in `include/tickforge/replay/` (`event_processor.hpp`,
`replay.hpp`) and `src/replay/replay.cpp`, compiled into the
`tickforge_replay` static library.

Responsibilities, in order, per event:

1. Verify stream order: the input must already be sorted strictly by
   `Event::operator<` (`(timestamp, seq)`). The driver verifies rather
   than sorts (SPEC.md 3.3 permits either). Rationale: sorting belongs
   to the parser / Event Normalization layer, which owns the stream;
   the replay driver verifies as defense in depth without copying the
   event stream. Duplicate `(timestamp, seq)` keys are rejected as
   unsorted input, per SPEC.md 3.3. The input contract is
   `std::span<const Event>`: non-owning, no copy.
2. Verify session scoping: exactly one instrument per session
   (SPEC.md 2.2). A second instrument aborts the replay.
3. Validate via `validateEvent()` (SPEC.md section 11).
4. Dispatch to the `EventProcessor`.

The first failure aborts the replay with a diagnostic naming the
offending event's sequence number and the reason (`ReplayError`:
`UnsortedInput`, `InvalidEvent`, `UnknownOrder`, `DuplicateOrder`,
`InstrumentMismatch`). This is the strict-mode behavior of SPEC.md
section 11. Abort means abort, not rollback: already-applied events
stay applied.

The driver knows nothing about market logic. It never interprets
prices, sides, or book structure.

### 8.2 EventProcessor interface

`EventProcessor` is the narrow seam between replay and state. One
method: `onEvent(const Event&)` applies a single validated event and
returns true when applied, false when rejected for a state reason
(cancel/modify of a non-resting order, new order with a live id). The
replay driver maps the rejection to a `ReplayError` using the event
type, so the interface stays minimal. Future milestones can add
processors (matching engine, statistics sinks) without touching the
replay driver.

### 8.3 Order book

Lives in `include/tickforge/book/order_book.hpp` and
`src/book/order_book.cpp`, compiled into the `tickforge_book` static
library. Implements `EventProcessor`. Performs no matching.

Representation (all internals private):

* `orders_`: `OrderId` -> resting order, for O(1) lookup by id. Each
  entry also stores its position within its price-level queue, so
  cancellation is O(1) rather than a queue scan.
* `bids_` / `asks_`: price -> FIFO queue of order ids at that price.
  Bid levels are ordered highest-first, ask levels lowest-first, so
  `bestBid()` / `bestAsk()` are O(1). Empty levels are removed when
  their last order leaves.
* Each resting order records `OrderId`, `Side`, `Price`, remaining
  `Quantity`, and `arrivalSeq` (the `NewOrder` event's sequence: the
  time-priority key). Queue order within a level is `arrivalSeq`
  order. Order ids never determine priority: ids identify, sequences
  order.

Behavior:

* `NewOrder`: inserts into the id lookup, the side's price level (at
  the back of the queue), and updates best bid/ask. A duplicate live
  id is rejected. The book enforces live-uniqueness only;
  session-wide id uniqueness is the parser's job (SPEC.md 5.1), since
  the book cannot distinguish a reused id from a fresh one after
  cancellation.
* `CancelOrder`: removes from the queue (preserving the order of the
  rest), removes the id from the lookup, drops the level if empty,
  and updates best bid/ask. Cancelling a non-resting order is
  rejected.
* `ModifyOrder`: follows SPEC.md 5.5 exactly. Zero price/quantity
  means "unchanged". A price change or quantity increase is
  cancel/replace: the order leaves its queue and rejoins at the back
  of its (possibly new) level with `arrivalSeq` set to the modify
  event's sequence. A quantity decrease at the same price keeps its
  queue position and `arrivalSeq`.

Queries are read-only: `contains`, `find` (returns a `RestingOrder`
view), `bestBid` / `bestAsk` (as `std::optional<Price>`, empty when
the side is empty), `orderCount`, `priceLevelCount`, and
`ordersAtLevel` (FIFO-ordered ids at a price). No standard containers
leak through the public API.

### 8.4 Deliberate Day 03 gap: no matching

Day 03 performs no trade matching. A new order that would cross the
opposite side still rests in the book, which may leave `bestBid() >=
bestAsk()` until the matching engine arrives. This is documented in
the `OrderBook` header and covered by a test so the interim behavior
is explicit, not accidental. Crossing/matching behavior belongs to
the matching-engine milestone (SPEC.md 5.4, ARCHITECTURE.md 1.5).

---

## 9. Matching engine and execution (Day 04)

Day 04 replaces the Day 03 gap with deterministic price-time priority matching:

```text
Normalized Events
       |
       v
Replay driver  (verify order, scope, validate, dispatch)
       |
       v
MatchingEngine (crossing detection, price-time priority, fills)
       |  |
       |  +----> Execution Results (Fill records, in order)
       v
OrderBook      (MBO resting-order state)
       |
       v
Updated Market State
```

### 9.1 Responsibility split

* **Replay engine:** unchanged from Day 03. Event ordering, validation,
  dispatch. It dispatches to any `EventProcessor`; it does not know
  whether the processor matches.
* **MatchingEngine** (`include/tickforge/matching/`, `src/matching/`,
  `tickforge_matching` library): implements `EventProcessor`. Decides
  crossing, selects resting orders by price-time priority, generates
  fills, rests residuals. It drives the `OrderBook` exclusively through
  the book's public mutation API (`addRestingOrder`, `removeOrder`,
  `reduceQuantity`, plus the read queries); it never touches book
  containers.
* **OrderBook:** still a state container. Day 04 adds the controlled
  mutation API above and a `priceLevels()` query; `onEvent` keeps Day 03
  semantics (a `NewOrder` rests without matching) so the book remains
  directly testable.

### 9.2 Execution semantics

Derived from SPEC.md sections 1.1, 2.3, 5.4, 5.5, 6, and 7:

* Only `NewOrder` events are aggressors; every `NewOrder` is a limit
  order. A bid crosses when its price >= best ask; an ask crosses when
  its price <= best bid. Equality crosses.
* Execution price is the resting order's price (definitional to
  price-time priority in a limit order book).
* Fills are generated best-eligible-price-first, then FIFO
  (`arrivalSeq`) within a price. An aggressor never consumes beyond its
  limit price; the first non-crossing level ends the sweep.
* A partially filled resting order keeps its queue position and
  `arrivalSeq` with reduced quantity.
* Unfilled residual quantity rests in the book at the limit price
  (SPEC.md 7). No IOC/FOK exists; `flags` remain ignored.
* A `ModifyOrder` that changes price is cancel/replace: the replacement
  is removed and run as a potential aggressor, matching if it crosses
  and otherwise resting with the modify event's seq as the new
  `arrivalSeq`. Quantity-only changes use the book's Day 03 logic.
* Self-trade by order id cannot happen: a `NewOrder` with a live id is
  rejected as a duplicate (SPEC.md 5.1). No participant fields exist.

### 9.3 Fill records

`Fill` (`include/tickforge/matching/fill.hpp`) is an execution result,
not a normalized event (SPEC.md 2.3: trade events do not appear in the
order flow). Fields: aggressor id, resting id, aggressor side,
execution price (ticks), quantity (lots, always positive), timestamp
and sequence from the aggressor event. Multiple fills from one event
share timestamp/sequence; their deterministic order is the vector
order. `MatchingEngine::fills()` returns the most recent event's fills;
the vector is reused across calls.

### 9.4 Failure model

Day 03's "abort, don't rollback" is preserved. Expected domain failures
(unknown order, duplicate id) return false through the `EventProcessor`
interface and abort replay with a diagnostic. Internal invariant
violations (e.g. a level snapshot referencing a missing order) are
`assert`s: programming errors, not domain failures. A valid event never
leaves the book half-mutated: matching computes fills against snapshots
and applies removals/reductions in order, and the residual is rested
only after the sweep completes.

## 10. Queue tracking and execution statistics (Day 05)

Day 05 adds a deterministic analytical layer on top of matching. It
observes; it never mutates the book and never influences matching:

```text
Normalized Events
       |
       v
Replay driver  (verify order, scope, validate, dispatch)
       |
       v
MatchingEngine (crossing detection, price-time priority, fills)
       |  |
       |  +----> Execution Results (Fill records, in order)
       |                |
       |                v
       |         ExecutionStatistics (counts, quantities, min/max, avg)
       v
OrderBook      (MBO resting-order state)
       |
       v
QueueTracker   (designated-order positions, read-only book queries)
```

### 10.1 Responsibility split

* **QueueTracker** (`include/tickforge/analytics/queue_tracker.hpp`,
  `src/analytics/queue_tracker.cpp`): tracks queue positions for
  *designated* order ids. A designated order is one the caller
  explicitly registers via `designate()`; tracking is opt-in so the
  component stays cheap when only a few orders matter. The tracker is
  fed validated events (`onEvent`) and fills (`onFills`) in stream
  order, and answers position queries by recomputing from the live book
  through the public read API (`find`, `ordersAtLevel`, `contains`).
  It never writes to the book and never performs matching.
* **ExecutionStatistics**
  (`include/tickforge/analytics/execution_statistics.hpp`,
  `src/analytics/execution_statistics.cpp`): consumes `Fill` records
  via `addFill()`/`addFills()` and maintains aggregate execution
  metrics. Pure observer; knows nothing about the book or the engine.
* Both live in the `tickforge_analytics` library. The intended driver
  flow per event is: `engine.onEvent(e)` then
  `tracker.onEvent(e)`, `tracker.onFills(engine.fills())`,
  `stats.addFills(engine.fills())`.

### 10.2 Queue-position semantics

Positions follow SPEC.md 8.1 exactly:

* Within one price level on one side, resting orders form a FIFO queue
  ordered by `arrivalSeq`. Queue position is the 1-based index in that
  queue. Quantity ahead is the sum of remaining quantities of all
  orders earlier in the same level's queue. Order ids never determine
  priority.
* Positions are recomputed from the live book on every query, so queue
  advancement is derived only from actual book events: inserts push
  later orders back, cancels and executions pull later orders forward,
  partial fills reduce the ahead quantity by exactly the executed
  amount. Nothing is estimated or interpolated.
* Lifecycle for a designated id: `Resting` (in the book), `Filled`
  (left the book via execution, detected from accumulated fills),
  `Cancelled` (a `CancelOrder` was observed), `Unknown` (not tracked or
  never observed). A price-changing `ModifyOrder` (or a quantity
  increase) is cancel/replace per SPEC.md 5.5: the tracker's
  fill-accounting resets, mirroring the engine. A same-price quantity
  decrease keeps history and priority. An id reused after cancellation
  starts fresh on its `NewOrder`.

### 10.3 Execution statistics

All statistics are exact integer arithmetic; no floating point appears:

* `fillCount()`: number of fills observed.
* `totalQuantity()`: sum of fill quantities (lots).
* `buyQuantity()` / `sellQuantity()`: lots where the aggressor side is
  Bid / Ask.
* `minPrice()` / `maxPrice()`: extreme execution prices (ticks),
  empty when no fills observed.
* `averagePrice()`: the quantity-weighted average as an exact rational
  `sum(price_ticks * quantity_lots) / sum(quantity_lots)`, empty when
  no fills observed. The numerator is accumulated in 128 bits (portable
  hi/lo words, no `__int128`, no `-Wpedantic` issues) because a single
  int64 price times an int64 quantity can already overflow int64.

Deliberately out of scope: PnL, mark-to-market, strategy metrics,
latency models, and anything stochastic. The statistics describe what
executed, nothing more.

### 10.4 Deterministic guarantees

* Given the same event stream fed in the same order, queue positions,
  lifecycle states, and statistics are bit-identical across runs.
* Analytics cannot perturb matching: they hold no mutable references
  into the engine or book, and the integration tests assert that a run
  with analytics attached produces identical fills and book state to a
  run without.
