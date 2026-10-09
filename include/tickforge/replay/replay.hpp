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
// Error modes (SPEC.md section 11):
//   * Strict (default): the first invalid event aborts the replay with a
//     diagnostic naming the event's sequence number and reason.
//   * Lenient: invalid events are skipped, counted in skippedCount, and
//     the replay continues. The mode used is recorded in the result.
//     Stream-level violations (unsorted input, instrument mismatch) still
//     abort in lenient mode: they are not "invalid events" but broken
//     stream invariants (SPEC.md 3.3, 2.2).
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
#include "tickforge/replay/replay_mode.hpp"
#include "tickforge/replay/session_config.hpp"
#include "tickforge/version.hpp"

#include <cstdint>
#include <optional>
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
  InvalidConfig,      // the session configuration failed validation
};

[[nodiscard]] std::string_view toString(ReplayError error) noexcept;

// Replay error mode (SPEC.md section 11). Strict is the default;
// lenient must be explicitly selected by the caller. Declared in
// replay_mode.hpp so session_config.hpp can name it without including
// this header.

struct ReplayResult {
  ReplayError error = ReplayError::Ok;
  Sequence failedAt; // sequence number of the offending event, if any
  EventValidationError validationReason = EventValidationError::Ok;
  // Lenient-mode run summary (SPEC.md section 11): how many invalid
  // events were skipped. Always 0 in strict mode.
  std::uint64_t skippedCount = 0;
  // Records which mode produced this result (SPEC.md 11: lenient use
  // must be recorded in the output).
  ReplayMode mode = ReplayMode::Strict;
  // Result metadata (SPEC.md 9.6): the TickForge version and the hash
  // of the effective session configuration. configHash is set only when
  // the replay ran with an explicit SessionConfig; otherwise the run
  // had no configuration to identify.
  std::string_view version = kVersion;
  std::optional<std::uint64_t> configHash;

  [[nodiscard]] bool ok() const noexcept {
    return error == ReplayError::Ok;
  }
};

// Replays events in order through the processor.
//
// Requires: events sorted strictly by Event::operator< ((timestamp, seq)),
// all carrying the same instrument. Both are verified; the first
// violation aborts the replay with the offending event's sequence number,
// in both modes.
//
// In strict mode (default), the first invalid event aborts: a rejected
// validation yields InvalidEvent, a rejected dispatch yields UnknownOrder
// or DuplicateOrder derived from the event type.
//
// In lenient mode, invalid events are skipped and counted instead of
// aborting; the replay completes with error == Ok and skippedCount set.
[[nodiscard]] ReplayResult replayEvents(std::span<const Event> events,
                                        EventProcessor& processor,
                                        ReplayMode mode = ReplayMode::Strict);

// Configured replay (SPEC.md sections 9.6 and 10).
//
// Validates the configuration before touching the processor: an invalid
// configuration yields ReplayError::InvalidConfig and the processor
// observes nothing. The configuration's instrument must match the
// event stream's instrument (InstrumentMismatch otherwise). The replay
// runs with config.mode, and the result embeds the TickForge version
// and the configuration hash (SPEC.md 9.6).
[[nodiscard]] ReplayResult
replayEvents(std::span<const Event> events, EventProcessor& processor, const SessionConfig& config);

} // namespace tickforge
