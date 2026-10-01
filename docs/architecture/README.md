# Architecture design notes

This directory holds design notes and diagrams supporting ARCHITECTURE.md.

## System layers (Day 01)

```text
+-------------------------------------------------------------+
| Data / Feed Layer          raw datasets, venue formats      |
+-------------------------------------------------------------+
| Event Normalization        canonical events, (ts, seq) order |
+-------------------------------------------------------------+
| Replay Engine              deterministic driver, sim clock  |
+-------------------------------------------------------------+
| Order Book (MBO)           resting orders by side and level |
+-------------------------------------------------------------+
| Matching Engine            price-time priority fills        |
+-------------------------------------------------------------+
| Microstructure Model       queue dynamics, latency, fees    |
+-------------------------------------------------------------+
| Execution Simulator        simulated-order lifecycle traces |
+-------------------------------------------------------------+
| Statistics / Validation    stats, invariants, determinism   |
+-------------------------------------------------------------+

  ................ deterministic boundary ................
  Everything below Event Normalization output is a pure
  function of (events, config, version).
```

Notes land here as the layers are designed, one focused document per
significant decision, cross-referenced from ARCHITECTURE.md.
