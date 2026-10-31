# TickForge Technical Specification

**Status:** Day 01 draft, normative for all future implementation.
**Version:** 0.1.0
**Language:** C++20 is the canonical implementation language.

This document defines the domain concepts that TickForge must implement.
Nothing in this spec is optional for the components it covers: if a later
design decision conflicts with this document, the document must be amended
first, with the reason recorded in the commit message.

Conformance language follows RFC 2119: MUST, MUST NOT, SHOULD, MAY.

---

## 1. Goals and non-goals

### 1.1 Goals

1. Deterministic, order-by-order simulation of a single limit order book.
2. Queue-position-aware execution modeling (how much quantity sits ahead of
   a simulated order at its price level).
3. Faithful replay of normalized historical market events.
4. Reproducible performance benchmarking with published methodology.

### 1.2 Non-goals (Day 01 through the 1.5-month plan)

1. Multi-venue or cross-asset simulation. One instrument, one book.
2. Network transport, colocation modeling, or real exchange connectivity.
3. Trading strategies, portfolio logic, or risk systems.
4. Machine learning models inside the simulation core.
5. Bindings for other languages. C++ is canonical.

---

## 2. Event model

### 2.1 Definition

An **event** is the atomic unit of market activity processed by TickForge.
Every state change in the simulated book is caused by exactly one event.
Events are immutable once created: a correction is a new event, never a
mutation of an old one.

### 2.2 Event fields

| Field        | Type   | Required | Meaning |
|--------------|--------|----------|---------|
| `timestamp`  | int64  | yes      | Event time, nanoseconds since the Unix epoch (see section 3). |
| `seq`        | uint64 | yes      | Logical sequence number, unique per replay session (see section 3). |
| `type`       | enum   | yes      | One of the values in section 2.3. |
| `instrument` | string | yes      | Instrument identifier, e.g. `"AAPL"`. Exactly one per session. |
| `order_id`   | uint64 | yes      | Source-assigned order identity (see section 5). |
| `side`       | enum   | yes      | `Bid` or `Ask`. |
| `price`      | int64  | type-dependent | Limit price in integer tick units (see 2.4). |
| `quantity`   | int64  | type-dependent | Quantity in minimum lot units (see 2.4). |
| `flags`      | uint32 | no       | Bitmask for future order attributes (reserved, default 0). |

Fields exist for exactly one reason each:

* `timestamp` and `seq` together form the total deterministic order of the
  replay (section 3). Neither alone is sufficient.
* `type` selects the state transition the replay engine applies.
* `instrument` scopes the session; multi-instrument support is a non-goal,
  but the field makes that scoping explicit and future-proof.
* `order_id` is the identity key of the MBO model (section 5).
* `side`, `price`, `quantity` are the minimum data needed to place, match,
  and track an order. Nothing smaller can express a limit order book.
* `flags` is reserved so future attributes (e.g. time-in-force) do not
  require a schema break. It MUST default to 0 and MUST be ignored by the
  Day 01 through matching-engine milestones.

### 2.3 Event types

| Type          | Meaning |
|---------------|---------|
| `NewOrder`    | A new limit order enters the book (or crosses and matches). |
| `ModifyOrder` | Price and/or quantity of a resting order changes. |
| `CancelOrder` | A resting order is removed from the book. |

`Trade` events do NOT appear in the order flow. Public trade prints belong
to reference market data used for validation, not to the simulated order
stream. Session markers (`SessionStart`, `SessionEnd`) MAY be added later
for multi-session datasets; they are out of scope for Day 01.

### 2.4 Numeric representation

* Prices are `int64` counts of the instrument's minimum price tick. The tick
  size is declared per instrument in the session configuration.
* Quantities are `int64` counts of the minimum lot size, likewise declared
  per instrument.
* Rationale: integer arithmetic is exact and platform-independent.
  Floating point prices are FORBIDDEN in the simulation core because
  rounding and contraction behavior can differ across compilers and
  optimization levels, which would break the determinism guarantee.
* Aggregate quantities (cumulative traded lots in execution
  statistics, including the VWAP denominator; total quantity at one
  price level; quantity ahead of an order) are sums of individual
  quantities. They are exact while the true sum fits in `int64`. A
  session whose aggregate sums exceed `INT64_MAX` lots is outside
  the supported domain, in the same way as an event whose fields are
  out of range: the individual values remain valid, but aggregate
  results past that bound are not meaningful and MUST NOT be relied
  on. Implementations MUST NOT invoke undefined behavior for any
  input, in or out of that domain; the VWAP numerator is exempt from
  the bound because it is accumulated exactly in 128 bits.

---

## 3. Timestamp semantics

### 3.1 Representation

* Timestamps are **signed 64-bit integers** counting **nanoseconds since the
  Unix epoch** (1970-01-01T00:00:00Z).
* Signedness is deliberate: it keeps pre-epoch or relative-time test
  fixtures representable and avoids unsigned underflow in timestamp
  arithmetic (e.g. `t - latency`).
* Range is approximately +/- 292 years, far beyond any market dataset.
* There is no separate "logical clock": the event stream's timestamps ARE
  the simulation clock. The engine MUST NOT read the wall clock on the
  deterministic path (see ARCHITECTURE.md, deterministic boundaries).

### 3.2 Ordering

Events are processed in strict order of the tuple `(timestamp, seq)`:

1. Smaller `timestamp` first.
2. On equal `timestamp`, smaller `seq` first.

`seq` is assigned by the parser in input-file order, starting from 0, and
MUST be dense (no gaps) within a session. This makes same-timestamp
ordering deterministic and reproducible: it is exactly the order the
source dataset listed the events.

### 3.3 Deterministic ordering rules

* The replay engine MUST sort (or verify sortedness of) the normalized
  event stream by `(timestamp, seq)` before simulation begins.
* A parser MUST reject a stream with duplicate `(timestamp, seq)` pairs.
* Two events with equal `timestamp` and the input order A-then-B MUST
  always produce the same result as any other run with the same input.

---

## 4. Determinism guarantee

### 4.1 Formal statement

> Given identical input events, identical configuration, and identical
> TickForge version, the simulator MUST produce identical outputs.

### 4.2 Meaning of "identical output"

Two runs are identical iff all of the following are byte-identical:

1. The fill records: for each fill, order id, side, price, quantity,
   timestamp, and sequence number.
2. The order-book state after each event (or, equivalently, at every
   checkpoint the statistics layer records).
3. The emitted event log, in order.
4. All computed statistics.

"Identical configuration" includes the session config file, the instrument
tick/lot definitions, and every parameter of the microstructure model.
"Identical TickForge version" is the `tickforge::kVersion` string; output
artifacts MUST embed it so a mismatch is detectable.

### 4.3 Randomness

* Uncontrolled randomness is FORBIDDEN. `std::rand`, `std::random_device`,
  time-seeded generators, and thread-scheduling-dependent outcomes MUST NOT
  affect simulation results.
* If a future model needs randomness (e.g. a stochastic latency model), it
  MUST use an explicitly seeded deterministic PRNG (e.g. splitmix64), and
  the seed MUST be part of the configuration, hence part of the
  determinism guarantee's inputs.
* Iteration order over hash-based containers MUST NOT influence output.
  Where iteration order is observable, use ordered containers or sort
  before iterating.

---

## 5. Market-by-Order (MBO) model

The book is modeled order by order: every resting order is individually
identified and tracked. Aggregated (level-2/MBP) views are derived, never
primary.

### 5.1 Order identity

* An order is identified by `(instrument, order_id)`.
* `order_id` values MUST be unique within a session. The parser MUST reject
  a `NewOrder` whose id is already live in the book.

### 5.2 Order record

A resting order carries: `order_id`, `side`, `price` (ticks), `quantity`
remaining (lots), `arrival_seq` (the `seq` of its `NewOrder` event, which
determines time priority), and lifecycle state (section 7).

### 5.3 Cancellation

`CancelOrder` removes the order from the book. Cancelling a non-resting
order is an error: in strict mode the replay MUST abort with a diagnostic;
a lenient mode MAY log and continue (see section 11).

### 5.4 Execution

When an incoming order crosses the book, quantity is matched against
resting orders following price-time priority (section 6). Each match
produces a fill record against both the incoming and the resting order.
Partial fills reduce the resting order's remaining quantity and move it to
the `PartiallyFilled` state.

### 5.5 Modification

`ModifyOrder` changes price and/or quantity of a resting order:

* A price change, or a quantity *increase*, is treated as cancel/replace:
  the order loses its time priority and receives a new `arrival_seq`
  equal to the modify event's `seq`.
* A quantity *decrease* at the same price keeps the order's time priority.
* Rationale: this matches the dominant real-venue rule and keeps the
  priority model explainable. Any venue-specific deviation MUST be a
  documented configuration option, not silent behavior.

---

## 6. Price-time priority

The matching engine MUST implement strict price-time priority:

1. **Price priority:** a bid at a higher price has priority over a bid at a
   lower price; an ask at a lower price has priority over an ask at a
   higher price.
2. **Time priority:** at the same price on the same side, the order with
   the smaller `arrival_seq` (earlier eligible order) has priority.

No other priority factor (size, participant type, hidden status) exists in
the core model. Extensions such as pro-rata matching MUST be separate,
explicitly named matching rules selected by configuration, never silent
modifications of the default.

---

## 7. Order lifecycle

```text
Created -> Accepted -> Resting -> PartiallyFilled -> Filled
                      |              |
                      +--> Cancelled <+
```

Valid transitions:

| From             | To               | Trigger |
|------------------|------------------|---------|
| `Created`        | `Accepted`       | `NewOrder` passes validation |
| `Created`        | (rejected)       | `NewOrder` fails validation; never enters the book |
| `Accepted`       | `Resting`        | Order does not cross; rests in the book |
| `Accepted`       | `Filled`         | Incoming order fully matches on entry |
| `Accepted`       | `PartiallyFilled`| Incoming order partially matches, remainder rests |
| `Resting`        | `PartiallyFilled`| Incoming order matches part of it |
| `Resting`        | `Filled`         | Incoming order matches all of it |
| `Resting`        | `Cancelled`      | `CancelOrder` |
| `PartiallyFilled`| `Filled`         | Remaining quantity matched |
| `PartiallyFilled`| `Cancelled`      | `CancelOrder` |

Notes:

* `ModifyOrder` does not add states; per section 5.5 it is modeled as a
  priority-affecting update on a `Resting` or `PartiallyFilled` order.
* `Filled` and `Cancelled` are terminal. Any event referencing a terminal
  order is an error handled per section 11.
* Rejected orders never reach `Accepted`; the rejection is recorded in the
  event log with a reason code.

---

## 8. Queue-position concept

### 8.1 Definition

Within one price level on one side, resting orders form a FIFO queue
ordered by `arrival_seq`. The **queue position** of an order is its
1-based index in that queue. The quantity **ahead** of an order is the sum
of remaining quantities of all orders earlier in the same level's queue.

```text
Price level 100 (bids)
  [1] Order A  (300 lots)
  [2] Order B  (100 lots)
  [3] YOUR ORDER (200 lots)   <- 400 lots ahead
  [4] Order D  (150 lots)
```

### 8.2 What will be tracked

For each resting order the engine will eventually maintain:

* its queue index within its level,
* the total quantity ahead of it at its level,
* updates to both on every insert, cancel, modify, and partial fill
  affecting its level.

### 8.3 Intended implementation shape (not implemented Day 01)

Per price level, an intrusive FIFO doubly-linked list of order nodes plus
a cached aggregate (order count, total quantity). Queue metrics update in
O(1) amortized time per event by adjusting cached aggregates on the
affected level only. The full design arrives with the matching-engine
milestone; Day 01 only fixes the semantics above so later code has a
stable target.

---

## 9. Replay model

```text
Input Dataset -> Parser -> Normalized Events -> Deterministic Event Replay
  -> Market State -> Execution Results
```

Stage responsibilities:

1. **Input Dataset:** raw historical data (venue-specific formats). The
   simulator never reads these directly.
2. **Parser:** converts one dataset format into normalized events. Owns
   `seq` assignment, field validation, and rejection of malformed records
   with reasons. Parsers are the ONLY place allowed to know about
   venue-specific formats.
3. **Normalized Events:** the canonical `Event` stream defined in section 2,
   sorted by `(timestamp, seq)`. This is the reproducibility boundary: two
   parsers producing the same normalized stream MUST produce the same
   simulation.
4. **Deterministic Event Replay:** feeds events to the book in
   `(timestamp, seq)` order with no wall-clock involvement. Owns the
   determinism guarantee (section 4).
5. **Market State:** the MBO book plus derived views (best bid/ask,
   level aggregates) after each event.
6. **Execution Results:** fill records, queue-position traces for
   designated simulated orders, and statistics. All outputs embed the
   TickForge version and the configuration hash.

---

## 10. Configuration and versioning

* Every simulation run takes a single session configuration: instrument
  tick/lot sizes, matching rule selection, microstructure model
  parameters, PRNG seeds, strict/lenient error mode.
* The configuration MUST be serializable to a file, and output artifacts
  MUST embed its hash alongside the TickForge version.
* The project version (`tickforge::kVersion`, single source of truth in
  `CMakeLists.txt`) follows semantic versioning. Spec changes that alter
  simulation results REQUIRE a minor or major version bump.

---

## 11. Error handling

* **Strict mode (default):** the first invalid event (unknown order id on
  cancel/modify, duplicate order id on new, negative quantity, etc.)
  aborts the replay with a diagnostic naming the event `seq` and reason.
* For order-carrying events (`NewOrder`, `ModifyOrder`), quantity MUST be
  strictly positive: zero and negative quantities are rejected. A
  `ModifyOrder` MUST change at least one of price or quantity; an event
  changing neither is invalid.
* **Lenient mode:** invalid events are skipped, counted, and reported in
  the run summary. Lenient mode exists for messy real datasets and MUST be
  explicitly selected; its use is recorded in the output.
* Parsers MUST NOT silently drop records. Every dropped record gets a
  reason code.

---

## 12. Glossary

* **MBO (Market by Order):** book representation tracking individual orders.
* **Price-time priority:** matching rule defined in section 6.
* **Queue position:** an order's FIFO index within its price level.
* **Quantity ahead:** resting quantity earlier in the same level's queue.
* **Replay:** deterministic processing of a normalized event stream.
* **Fill:** a matched quantity between an incoming and a resting order.
* **Tick:** minimum price increment of the instrument.
* **Determinism guarantee:** the formal statement in section 4.1.

---

## 13. Command-line interface

The `tickforge` executable replays one CSV event file through the
library pipeline (sections 9 and 11). This section defines its
contract; behavior not specified here is not part of the CLI.

### 13.1 Invocation and options

```text
tickforge --input PATH --instrument SYMBOL [--mode strict|lenient]
                   [--tick-size VALUE] [--lot-size VALUE] [--seed VALUE]
                   [--latency-min-ns VALUE] [--latency-max-ns VALUE]
tickforge --help
tickforge --version
```

* `--input PATH` (required for replay): the CSV file to replay.
  Relative paths resolve against the process working directory.
* `--instrument SYMBOL` (required for replay): the session
  instrument. The CSV format carries no instrument column, so the
  instrument is a session property supplied here; it is used both as
  the parser's instrument and as the session configuration's
  instrument, and the two always agree.
* `--mode strict|lenient` (optional, default `strict`): the replay
  mode of section 11. Lenient mode skips invalid events and counts
  them; it never relaxes CSV parsing, which still stops at the first
  malformed record.
* `--tick-size VALUE`, `--lot-size VALUE` (optional): the session
  configuration's tick and lot sizes (section 10), given as decimal
  strings and validated by the configuration validator; an invalid
  value is a configuration error (exit 5), reported by the validator,
  not by a second CLI-side rule.
* `--seed VALUE` (optional): the session configuration's PRNG seed.
  An unsigned 64-bit decimal integer: no sign, no whitespace, no
  trailing characters; `0` and `18446744073709551615` are valid. A
  malformed or out-of-range seed is an argument error (exit 2).
* `--latency-min-ns VALUE`, `--latency-max-ns VALUE` (optional):
  the latency model's bounds in nanoseconds (section 15). Each is
  a signed 64-bit decimal integer as text: no whitespace, no
  trailing characters; a malformed or out-of-range value is an
  argument error (exit 2). Whether the parsed values form a usable
  configuration (both present, nonnegative, ordered, seed present)
  is decided by the configuration validator, not the argument
  parser: an unusable combination is a configuration error
  (exit 5), reported before the input file is opened. Supplying
  the bounds enables the latency model; supplying neither leaves
  it disabled. `--seed` alone does not enable it, and a seed's
  existing meaning is unchanged when the model is disabled.
* `--help`: prints usage to standard output and exits 0.
* `--version`: prints `TickForge v<version>` and exits 0.

Each option may appear at most once. Unknown options, positional
arguments, missing option values, empty values, and invalid mode
values are argument errors. Options are validated before `--help` or
`--version` are honored, so a malformed command line is an error even
if it also contains `--help`.

### 13.2 Exit codes

| Code | Meaning |
|---|---|
| 0 | Replay succeeded, or a help/version request succeeded |
| 2 | Invalid command-line arguments |
| 3 | Input file could not be opened, or reading it failed |
| 4 | CSV parsing failed (including event validation during parsing) |
| 5 | Session configuration invalid, or replay failed |
| 6 | Unexpected internal failure (an exception reached `main`) |

### 13.3 Input processing

The file is opened with `std::ifstream`. Failure to open is exit 3.
The stream is parsed by `CsvParser::parseStream()` with unchanged
parser semantics (section 9 input format: exact header, six fields,
first-error termination, one-based line numbers). After parsing, the
stream state is checked: if the stream's bad bit is set, the file did
not read completely and the run fails with exit 3, even if the parser
reported success for a prefix. A parse error is exit 4 and reports
the parser error category and line number. Replay starts only after
the input is known to be complete and well-formed.

Empty files and header-only files are distinct: an empty file (or a
file whose first line is not the header) is a parse failure (exit 4);
a header-only file is a valid replay of zero events (exit 0).

### 13.4 Output

On success the summary is printed to standard output, exactly:

```text
TickForge v<version>
Input: <path as given>
Instrument: <symbol>
Replay mode: <strict|lenient>
Events processed: <applied event count>
Events skipped: <lenient-mode skip count>
Fills: <fill count>
Resting orders: <orders left in the book>
Best bid: <price ticks, or none>
Best ask: <price ticks, or none>
Config hash: <16 lowercase hex characters>
Replay status: success
```

All diagnostics go to standard error; a failed run prints no success
summary. Output contains no timestamps, addresses, or environment
data: the same input file, instrument, and mode produce byte-identical
output (the `Input:` line echoes the path exactly as supplied).

When the latency model is enabled (section 15), a latency section
follows the summary above, in this order:

```text
Latency model: uniform-latency v<model version>
Latency seed: <seed>
Latency min (ns): <minimum bound>
Latency max (ns): <maximum bound>
Latency samples: <sample count>
Latency sample <index> (ns): <sample>
```

One `Latency sample` line is printed per fill, in fill order,
with zero-based indices matching the artifact's positional
association (section 15.2). A replay with no fills prints the
five header lines with a sample count of zero and no sample
lines. The full ordered list is printed deliberately: it is the
artifact itself, deterministic and directly verifiable, and no
summary statistic is substituted for it. When the model is
disabled, no latency section is printed and the output is exactly
the twelve lines above. A failed run prints no latency section:
there is no partial artifact.

---

## 14. Deterministic session generation

This section specifies the seeded event generator used by the test
suite. The generator is test infrastructure: it produces valid event
sessions for differential and property testing. It is not a
production simulation model, and `SessionConfig.prngSeed` does not
drive replay; replay remains a pure function of the parsed events
and the configuration. Section 4.3 governs any future production
model that needs randomness.

### 14.1 PRNG

The generator uses splitmix64. The state is a `uint64` initialized
to the seed verbatim; every `uint64` value is a valid seed,
including `0` and `2^64 - 1`. Each output is:

```text
state += 0x9E3779B97F4A7C15
z = state
z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
z = (z ^ (z >> 27)) * 0x94D049BB133111EB
output = z ^ (z >> 31)
```

All arithmetic is unsigned 64-bit, so the output stream is a pure
function of the seed and the number of draws consumed, identical on
every platform.

### 14.2 Bounded mapping

* `nextBounded(b)`, for `b > 0`, returns `next() % b`.
* `nextRange(lo, hi)`, for `lo <= hi`, returns
  `lo + nextBounded(hi - lo + 1)`.
* `nextBool()` returns the low bit of `next()`.

The modulo mapping has a small bias for bounds that do not divide
2^64. That bias is acceptable for test generation and is part of
this contract: changing the mapping changes every generated
session.

### 14.3 Generator

Generation is parameterized by `GeneratorConfig`: the seed, the
event count, inclusive price and quantity bounds, relative weights
for new/cancel/modify operations, and the instrument label. A
generator instance consumes its own PRNG stream; reproducibility is
stated per instance constructed from an equal configuration.

Generated sessions satisfy:

* Order ids are assigned densely from 1 in creation order.
* Sequences are dense from 0 in event order.
* Timestamps start at 1,000,000 and strictly increase; after each
  event the timestamp advances by a drawn value in [1, 1000].
* Operations are chosen by one bounded draw against the cumulative
  weights; when no order is live, the operation is a new order.
* Prices and quantities are drawn uniformly (per 14.2) from the
  configured inclusive bounds.
* Cancels and modifies target live orders tracked by the
  generator's internal reference book, so every generated event
  passes `validateEvent()` and is accepted by the matching engine
  in order: sessions are valid by construction. Generating invalid
  or adversarial sessions is a separate concern and is not part of
  this generator's contract.

### 14.4 Reproducibility and versioning

The same `GeneratorConfig` produces the identical event sequence,
and the identical CSV serialization of it, on every platform. The
algorithm, mapping, and draw order defined here are generation
version 1. Changing any of them changes generated artifacts and
MUST be recorded as a new generation version in this section and
in the changelog before the change lands.

---

## 15. Per-fill latency model

The latency model is the production stochastic model anticipated by
section 4.3 and the first consumer of `prngSeed`. It observes
completed fills and produces one simulated latency sample per fill.
The samples are a simulation artifact for study: they are not
measurements of any real exchange, gateway, or execution path,
and no value in this section is an empirical claim.

### 15.1 Enablement and configuration

The model is disabled by default. It is enabled by a session
configuration carrying both `latencyMinNs` and `latencyMaxNs`
(canonical keys `latency-min-ns` and `latency-max-ns`), and
enabling it requires `prngSeed`: the seed is the model's explicit
seed, per section 4.3. `validateConfig` rejects, before any event
is processed:

* exactly one bound present (`LatencyBoundsIncomplete`),
* a negative minimum or a maximum below the minimum
  (`InvalidLatencyBounds`),
* enabled bounds without a seed (`LatencySeedMissing`).

Disabled configurations omit both keys from the canonical
serialization, so every pre-existing configuration, including the
default hash, is unchanged. Enabled configurations hash with the
bounds and the seed as part of their identity.

### 15.2 Sampling contract

* Unit: integer nanoseconds, stored as `int64`.
* Distribution: discrete uniform over the inclusive interval
  [min, max], computed as `min + nextBounded(max - min + 1)` over
  the production SplitMix64 (section 14.1's algorithm, as its own
  production component and stream), with the modulo mapping of
  section 14.2 and its documented bias.
* The model instance owns its PRNG state, initialized from the
  configuration's `prngSeed`. Instances share no state.
* Exactly one draw is consumed per observed fill, in the order
  fills are produced by the matching engine. A run with no fills
  produces no samples and consumes no draws.
* Association is positional: sample `i` belongs to the `i`-th fill
  produced, in production order. The artifact (`LatencyArtifact`)
  carries the model name and version, the seed, and the bounds
  alongside the ordered samples.

### 15.3 Isolation and reproducibility

The model only observes. It MUST NOT modify fills, events, book
state, statistics, timestamps, or replay outcomes; enabling it
leaves every existing replay and matching result bit-identical,
which the test suite verifies by differential comparison. For the
same configuration and the same ordered fills, the artifact is
identical on every platform: all arithmetic is unsigned 64-bit or
bounded `int64` computation with no signed overflow.

Model version 1 is the contract above (name `uniform-latency`).
Changing the algorithm, the mapping, the draw count or order, the
bounds interpretation, or the unit is a model-contract change and
requires a new model version, recorded here and in the changelog.
This section does not change the generation contract of section
14; the two share the SplitMix64 algorithm but have separate
streams, consumers, and versioning.
