# TickForge Benchmarking Policy

**Status:** Day 01. No benchmark harness exists yet and no performance claims are made.

## The rule

Every performance claim about TickForge MUST ship with a reproducible benchmark. The sentence "TickForge is fast" is not a claim; the following is:

```text
Dataset:      <name, source, event count, date range>
Hardware:     <CPU model, cores, RAM>
Compiler:     <compiler, version, flags, build type>
Events:       <number of events replayed>
Runtime:      <wall-clock seconds>
Events/sec:   <throughput>
Median latency: <ns per event>
p99 latency:    <ns per event>
```

Any benchmark report missing one of these fields is incomplete and MUST NOT be published as a TickForge result.

## Methodology requirements (for the future harness)

1. **Deterministic input:** benchmarks replay a fixed, versioned dataset. The dataset identifier and its checksum are part of the report.
2. **Warmup and repetition:** the harness performs warmup runs, then reports statistics over repeated measured runs (mean, median, p99), not a single run.
3. **Isolation:** CPU pinning and frequency scaling notes where relevant; background load disclosed.
4. **Separation of concerns:** replay throughput (events/sec) and per-event latency distributions are reported separately. Optimizing one must not silently regress the other.
5. **No benchmark gaming:** the harness measures the real replay path (parser output through matching and statistics), not a stripped-down micro-loop, unless the micro-benchmark is explicitly labeled as such.

## Reproducibility benchmark (planned)

A public script will let anyone rerun the headline benchmark: it downloads the pinned dataset, builds the pinned TickForge version, runs the harness, and prints the report above. Discrepancies beyond stated tolerance are treated as bugs.

## Current state

v0.1.0 makes zero performance claims. This document exists so the claims made later are honest.
