# Changelog

## 0.2.0

- Per-fill simulated latency model (SPEC.md section 15). A seeded
  SplitMix64 draws one latency sample per fill, uniform over a
  configured inclusive interval in nanoseconds. The model only
  observes fills: matching results are the same whether it is on
  or off. Samples come back as a separate artifact carrying the
  model name and version, the seed, and the bounds. The values are
  simulated, not measurements of a real system.
- Session configuration gained optional latency bounds
  (latency-min-ns, latency-max-ns). Enabling the model requires
  both bounds and a seed; configurations without them serialize
  and hash exactly as in 0.1.0.
- CLI: --latency-min-ns and --latency-max-ns options. An enabled
  run appends a latency section to the summary with every sample
  in fill order; the default output is unchanged.
- SPEC.md section 14 pins the deterministic session-generation
  contract (the SplitMix64 stream, the generator rules, generation
  version 1), backed by known-answer tests for the stream and for
  generated sessions across several seeds.

## 0.1.0

First release.

- Deterministic replay of CSV order-event streams (new, modify, cancel)
  through a market-by-order book with price-time priority matching,
  partial fills, and cancel/replace modify semantics.
- Strict and lenient replay modes with reason-coded errors. Lenient mode
  skips invalid events and counts them; CSV parse errors always stop the
  run.
- Execution statistics with exact integer VWAP (128-bit numerator),
  queue position tracking for a designated order, and market-state views
  (best bid/ask, per-level aggregates).
- Session configuration (instrument, tick size, lot size, matching rule,
  replay mode, optional PRNG seed) with canonical serialization and a
  deterministic configuration hash reported by every replay.
- CLI: `tickforge --input PATH --instrument SYMBOL` with `--mode`,
  `--tick-size`, `--lot-size`, and `--seed` options, a fixed 12-line
  summary on success, diagnostics on stderr, and documented exit codes.
- The behavior contract is specified in SPEC.md, with design notes and
  decision records in ARCHITECTURE.md.

Known limitations: one instrument per session; price-time priority is
the only matching rule; no persistence, networking, or live data.
Aggregate quantities are exact within the domain defined in SPEC.md
section 2.4.
