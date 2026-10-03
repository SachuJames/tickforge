// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Deterministic replay driver (SPEC.md sections 3.3 and 9, stage 4).
//
// Responsibility: turn a normalized event stream into an ordered sequence
// of state updates. The driver owns stream-level concerns only:
//   1. verifying the (timestamp, seq) order (SPEC.md 3.3),
//   2. verifying single-instrument session scoping (SPEC.md 2.2),
//   3. validating each event via validateEvent() (SPEC.md section 11),
//   4. dispatching each event to an EventProcessor in order.
//
// The driver knows nothing about market logic: it never interprets
// prices, sides, or book structure. It does not copy the event stream.
//
// Ordering decision (SPEC.md 3.3 permits "sort or verify sortedness"):
// replay REQUIRES pre-sorted input and verifies it, returning
// ReplayError::UnsortedInput otherwise. Sorting belongs to the parser /
// Event Normalization layer (ARCHITECTURE.md 1.2); the replay driver
// verifies as defense in depth without copying the stream. Duplicate
// (timestamp, seq) keys are rejected as unsorted input, per SPEC.md 3.3.

#pragma once

#include "tickforge/event/event.hpp"
#include "tickforge/replay/event_processor.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace tickforge {

enum class ReplayError : std::uint8_t {
  Ok = 0,
  UnsortedInput,      // events not strictly ordered by (timestamp, seq)
  InvalidEvent,       // validateEvent() failed; see validationReason
  UnknownOrder,       // cancel/modify referenced a non-resting order
  DuplicateOrder,     // new order reused a live order id
  InstrumentMismatch, // more than one instrument in the stream
};

[[nodiscard]] std::string_view toString(ReplayError error) noexcept;

struct ReplayResult {
  ReplayError error = ReplayError::Ok;
  Sequence failedAt; // sequence number of the offending event, if any
  EventValidationError validationReason = EventValidationError::Ok;

  [[nodiscard]] bool ok() const noexcept {
    return error == ReplayError::Ok;
  }
};

// Replays events in order through the processor.
//
// Requires: events sorted strictly by Event::operator< ((timestamp, seq)),
// all carrying the same instrument. Both are verified; the first
// violation aborts the replay with the offending event's sequence number.
// Each event is validated with validateEvent() before dispatch; a rejected
// dispatch aborts with UnknownOrder or DuplicateOrder derived from the
// event type. This is the strict-mode behavior of SPEC.md section 11:
// the first invalid event aborts the replay with a diagnostic.
[[nodiscard]] ReplayResult replayEvents(std::span<const Event> events, EventProcessor& processor);

} // namespace tickforge
