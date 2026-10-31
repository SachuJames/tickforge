// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: Apache-2.0.
//
// Per-fill simulated latency model (SPEC.md 15). The model observes
// completed fills and produces one simulated latency sample per
// fill, drawn from a discrete uniform distribution over a
// configured inclusive nanosecond interval.
//
// The model is an observer: it never modifies fills, events, book
// state, or statistics. Samples are simulation inputs for study,
// not measurements of any real system.
//
// The model owns its PRNG state (the production SplitMix64, seeded
// from its configuration). Exactly one draw is consumed per
// observed fill, in observation order, so the sample sequence is a
// pure function of the model configuration and the ordered fills.

#pragma once

#include "tickforge/core/splitmix64.hpp"
#include "tickforge/matching/fill.hpp"
#include "tickforge/replay/session_config.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace tickforge {

// Model identity (SPEC.md 15). Changing any output-affecting rule
// (algorithm, mapping, draw count, bounds interpretation, unit) is
// a model-contract change and requires a new model version.
inline constexpr std::string_view kLatencyModelName = "uniform-latency";
inline constexpr std::uint32_t kLatencyModelVersion = 1;

struct LatencyModelConfig {
  std::uint64_t seed = 0;
  std::int64_t minLatencyNs = 0;
  std::int64_t maxLatencyNs = 0;

  [[nodiscard]] bool operator==(const LatencyModelConfig&) const = default;
};

enum class LatencyModelError : std::uint8_t {
  Ok = 0,
  MinNegative,   // minLatencyNs < 0
  MaxLessThanMin // maxLatencyNs < minLatencyNs
};

[[nodiscard]] LatencyModelError
validateLatencyModelConfig(const LatencyModelConfig& config) noexcept;

// The latency artifact: everything needed to interpret the samples.
// samplesNs[i] is the sample for the i-th observed fill, in
// observation order (positional association, SPEC.md 15).
struct LatencyArtifact {
  std::string_view modelName = kLatencyModelName;
  std::uint32_t modelVersion = kLatencyModelVersion;
  std::uint64_t seed = 0;
  std::int64_t minLatencyNs = 0;
  std::int64_t maxLatencyNs = 0;
  std::vector<std::int64_t> samplesNs;

  [[nodiscard]] bool operator==(const LatencyArtifact&) const = default;
};

class LatencyModel {
public:
  // The configuration must be valid (validateLatencyModelConfig,
  // or a SessionConfig that passed validateConfig). The model does
  // not re-check it on the hot path.
  explicit LatencyModel(LatencyModelConfig config) noexcept : config_(config), rng_(config.seed) {}

  // Builds a model from a session configuration. Returns nullopt
  // when the latency model is disabled in the configuration (the
  // normal case). Assumes the configuration passed validateConfig.
  [[nodiscard]] static std::optional<LatencyModel> fromConfig(const SessionConfig& config);

  // Observes one fill: consumes exactly one PRNG draw and returns
  // the fill's latency sample, within the configured inclusive
  // bounds.
  std::int64_t observeFill(const Fill& fill);

  void observeFills(std::span<const Fill> fills);

  [[nodiscard]] const LatencyModelConfig& config() const noexcept {
    return config_;
  }
  [[nodiscard]] std::span<const std::int64_t> samples() const noexcept {
    return samples_;
  }
  [[nodiscard]] std::size_t sampleCount() const noexcept {
    return samples_.size();
  }

  // Snapshot of the samples with the metadata needed to interpret
  // them.
  [[nodiscard]] LatencyArtifact artifact() const;

private:
  LatencyModelConfig config_;
  SplitMix64 rng_;
  std::vector<std::int64_t> samples_;
};

} // namespace tickforge
