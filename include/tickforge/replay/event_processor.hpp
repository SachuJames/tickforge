// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// EventProcessor: the narrow interface between the deterministic replay
// driver and the state it updates. The replay driver owns event ordering,
// validation, and stream-level checks; the processor owns the state
// transition for one event. Keeping this an interface (rather than a
// concrete book) lets future milestones add processors (matching engine,
// statistics sinks) without touching the replay driver.

#pragma once

#include "tickforge/event/event.hpp"

namespace tickforge {

class EventProcessor {
public:
  virtual ~EventProcessor() = default;

  // Applies one event to the processor's state.
  //
  // Precondition: the event has passed validateEvent(). The replay driver
  // guarantees this; calling onEvent with an invalid event is a contract
  // violation.
  //
  // Returns true when the event was applied, false when it was rejected
  // for a state reason the replay driver reports deterministically:
  // cancelling or modifying a non-resting order, or adding an order whose
  // id is already live. The replay driver maps the rejection to a
  // ReplayError using the event type, so the interface stays minimal.
  virtual bool onEvent(const Event& event) = 0;
};

} // namespace tickforge
