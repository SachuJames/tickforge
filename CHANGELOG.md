# Changelog

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
