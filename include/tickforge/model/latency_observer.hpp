// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: Apache-2.0.
//
// Composition helper for the latency model (SPEC.md 15): wraps a
// MatchingEngine as an EventProcessor, forwarding each event to the
// engine and feeding the fills it produced to the model, in order.
// This is the same composition the CLI uses for its fill collector:
// the engine stays the only component that matches, and the model
// only observes.

#pragma once

#include "tickforge/matching/matching_engine.hpp"
#include "tickforge/model/latency_model.hpp"
#include "tickforge/replay/event_processor.hpp"

#include <utility>

namespace tickforge {

class LatencyObserver : public EventProcessor {
public:
  LatencyObserver(MatchingEngine& engine, LatencyModel model) noexcept
      : engine_(engine), model_(std::move(model)) {}

  bool onEvent(const Event& event) override {
    const bool applied = engine_.onEvent(event);
    if (applied) {
      model_.observeFills(engine_.fills());
    }
    return applied;
  }

  [[nodiscard]] const LatencyModel& model() const noexcept {
    return model_;
  }
  [[nodiscard]] LatencyArtifact artifact() const {
    return model_.artifact();
  }

private:
  MatchingEngine& engine_;
  LatencyModel model_;
};

} // namespace tickforge
