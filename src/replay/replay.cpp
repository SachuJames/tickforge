// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Deterministic replay driver implementation. Single interleaved pass:
// for each event, in stream order, verify ordering, verify instrument
// scoping, validate, then dispatch. In strict mode (default) the first
// failure aborts the replay with a diagnostic (SPEC.md section 11);
// in lenient mode invalid events are skipped and counted. A mid-stream
// abort intentionally leaves already-applied events in place: abort means
// abort, not rollback.

#include "tickforge/replay/replay.hpp"

#include <string_view>

namespace tickforge {

std::string_view toString(ReplayError error) noexcept {
  switch (error) {
  case ReplayError::Ok:
    return "Ok";
  case ReplayError::UnsortedInput:
    return "UnsortedInput";
  case ReplayError::InvalidEvent:
    return "InvalidEvent";
  case ReplayError::UnknownOrder:
    return "UnknownOrder";
  case ReplayError::DuplicateOrder:
    return "DuplicateOrder";
  case ReplayError::InstrumentMismatch:
    return "InstrumentMismatch";
  case ReplayError::InvalidConfig:
    return "InvalidConfig";
  }
  return "Unknown";
}

std::string_view toString(ReplayMode mode) noexcept {
  switch (mode) {
  case ReplayMode::Strict:
    return "Strict";
  case ReplayMode::Lenient:
    return "Lenient";
  }
  return "Unknown";
}

ReplayResult
replayEvents(std::span<const Event> events, EventProcessor& processor, ReplayMode mode) {
  ReplayResult result;
  result.mode = mode;
  if (events.empty()) {
    return result;
  }

  const std::string& session_instrument = events.front().instrument;

  for (std::size_t i = 0; i < events.size(); ++i) {
    const Event& event = events[i];

    // 1. Ordering: strictly increasing (timestamp, seq). Rejects both
    //    out-of-order events and duplicate keys (SPEC.md 3.3). A broken
    //    stream invariant aborts in both modes.
    if (i > 0 && !(events[i - 1] < event)) {
      result.error = ReplayError::UnsortedInput;
      result.failedAt = event.sequence;
      return result;
    }

    // 2. Session scoping: exactly one instrument per session (SPEC.md 2.2).
    //    Also a stream invariant: aborts in both modes.
    if (event.instrument != session_instrument) {
      result.error = ReplayError::InstrumentMismatch;
      result.failedAt = event.sequence;
      return result;
    }

    // 3. Validation gate (SPEC.md section 11).
    const EventValidationError validation = validateEvent(event);
    if (validation != EventValidationError::Ok) {
      if (mode == ReplayMode::Lenient) {
        ++result.skippedCount;
        continue;
      }
      result.error = ReplayError::InvalidEvent;
      result.failedAt = event.sequence;
      result.validationReason = validation;
      return result;
    }

    // 4. Dispatch. A rejection names its reason via the event type:
    //    cancel/modify of a non-resting order, or a duplicate live id.
    if (!processor.onEvent(event)) {
      if (mode == ReplayMode::Lenient) {
        ++result.skippedCount;
        continue;
      }
      result.error = event.type == EventType::NewOrder ? ReplayError::DuplicateOrder
                                                       : ReplayError::UnknownOrder;
      result.failedAt = event.sequence;
      return result;
    }
  }

  return result;
}

ReplayResult replayEvents(std::span<const Event> events,
                          EventProcessor& processor,
                          const SessionConfig& config) {
  // 1. Configuration gate: invalid configuration fails before the
  //    processor observes anything (SPEC.md section 10).
  if (validateConfig(config) != ConfigError::Ok) {
    ReplayResult result;
    result.error = ReplayError::InvalidConfig;
    result.mode = config.mode;
    return result;
  }

  // 2. Session scoping: the configuration declares the session's
  //    instrument; the event stream must carry it (SPEC.md 2.2).
  if (!events.empty() && events.front().instrument != config.instrument) {
    ReplayResult result;
    result.error = ReplayError::InstrumentMismatch;
    result.failedAt = events.front().sequence;
    result.mode = config.mode;
    return result;
  }

  // 3. Replay with the configured mode, then attach result metadata
  //    (SPEC.md 9.6): the version and the configuration hash identify
  //    exactly what produced this result.
  ReplayResult result = replayEvents(events, processor, config.mode);
  result.configHash = hashConfig(config);
  return result;
}

} // namespace tickforge
