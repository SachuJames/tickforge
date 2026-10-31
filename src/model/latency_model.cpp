// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: Apache-2.0.
//
// Per-fill simulated latency model implementation (SPEC.md 15).

#include "tickforge/model/latency_model.hpp"

namespace tickforge {

LatencyModelError validateLatencyModelConfig(const LatencyModelConfig& config) noexcept {
  if (config.minLatencyNs < 0) {
    return LatencyModelError::MinNegative;
  }
  if (config.maxLatencyNs < config.minLatencyNs) {
    return LatencyModelError::MaxLessThanMin;
  }
  return LatencyModelError::Ok;
}

std::optional<LatencyModel> LatencyModel::fromConfig(const SessionConfig& config) {
  if (!config.latencyMinNs.has_value()) {
    return std::nullopt;
  }
  LatencyModelConfig modelConfig;
  modelConfig.seed = *config.prngSeed;
  modelConfig.minLatencyNs = *config.latencyMinNs;
  modelConfig.maxLatencyNs = *config.latencyMaxNs;
  return LatencyModel(modelConfig);
}

std::int64_t LatencyModel::observeFill(const Fill& /*fill*/) {
  // One draw per fill. The interval span is computed in unsigned
  // arithmetic: with 0 <= min <= max <= INT64_MAX the span is at
  // most 2^63, and the offset is at most max - min, so the sample
  // lands inside [min, max] with no signed overflow anywhere.
  const auto span = static_cast<std::uint64_t>(config_.maxLatencyNs) -
                    static_cast<std::uint64_t>(config_.minLatencyNs) + 1;
  const auto offset = rng_.nextBounded(span);
  const std::int64_t sample = config_.minLatencyNs + static_cast<std::int64_t>(offset);
  samples_.push_back(sample);
  return sample;
}

void LatencyModel::observeFills(std::span<const Fill> fills) {
  for (const Fill& fill : fills) {
    observeFill(fill);
  }
}

LatencyArtifact LatencyModel::artifact() const {
  LatencyArtifact out;
  out.seed = config_.seed;
  out.minLatencyNs = config_.minLatencyNs;
  out.maxLatencyNs = config_.maxLatencyNs;
  out.samplesNs = samples_;
  return out;
}

} // namespace tickforge
