// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Canonical normalized event (SPEC.md section 2): the reproducibility
// boundary of the TickForge pipeline. A parser's job ends when it hands
// a validated Event stream to the replay engine; everything downstream is
// a pure function of (events, config, version).
//
// Layout notes:
//   * Flat value type: fixed size, no polymorphism, no virtual dispatch.
//   * The only heap allocation is the instrument string, and typical
//     symbols fit in std::string's small-string buffer (no heap). A
//     future replay engine may intern the instrument per session.
//   * price/quantity are type-dependent (SPEC.md 2.2):
//       NewOrder:    the order's limit price and quantity in ticks/lots.
//       ModifyOrder: the new absolute price/quantity; zero means
//                    "unchanged" (zero can never be a valid new price or
//                    quantity, so the encoding is unambiguous).
//       CancelOrder: unused and must be zero (enforced by validateEvent).

#pragma once

#include <cstdint>
#include <string>

#include "tickforge/event/event_type.hpp"
#include "tickforge/event/side.hpp"
#include "tickforge/event/types.hpp"

namespace tickforge {

// Instrument identifier, e.g. "AAPL" (SPEC.md 2.2). Exactly one per
// session; carried per event because the spec requires the field.
using Instrument = std::string;

// EventKey: the canonical total order of the replay (SPEC.md 3.2).
// Smaller timestamp first; equal timestamps break ties by smaller
// sequence number. This is the single reusable ordering mechanism;
// comparison logic must not be duplicated elsewhere.
struct EventKey {
  Timestamp timestamp{};
  Sequence sequence{};

  constexpr bool operator==(const EventKey&) const noexcept = default;
  constexpr auto operator<=>(const EventKey&) const noexcept = default;
};

// Canonical normalized event.
struct Event {
  Timestamp timestamp{};
  Sequence sequence{};
  EventType type{EventType::NewOrder};
  Instrument instrument{};
  OrderId orderId{};
  Side side{Side::Bid};
  Price price{};
  Quantity quantity{};
  std::uint32_t flags{0};

  friend bool operator==(const Event& a, const Event& b) noexcept = default;
};

[[nodiscard]] constexpr EventKey eventKey(const Event& event) noexcept {
  return EventKey{event.timestamp, event.sequence};
}

// Canonical replay ordering: compares (timestamp, sequence) ONLY.
// This is deliberately not a full-field ordering: sorting a normalized
// stream by this operator reproduces the deterministic replay order.
// Full-field equality remains operator==.
[[nodiscard]] inline bool operator<(const Event& a, const Event& b) noexcept {
  return eventKey(a) < eventKey(b);
}
[[nodiscard]] inline bool operator>(const Event& a, const Event& b) noexcept {
  return b < a;
}
[[nodiscard]] inline bool operator<=(const Event& a, const Event& b) noexcept {
  return !(b < a);
}
[[nodiscard]] inline bool operator>=(const Event& a, const Event& b) noexcept {
  return !(a < b);
}

// Validation outcome for a normalized event (SPEC.md section 11).
// Construction never validates: validateEvent() is the explicit,
// separate step a parser runs before events reach the replay engine.
enum class EventValidationError : std::uint8_t {
  Ok = 0,
  EmptyInstrument,
  InvalidSide,
  InvalidPrice,
  InvalidQuantity,
  EmptyModify,
  UnexpectedCancelPayload,
};

[[nodiscard]] std::string_view toString(EventValidationError error) noexcept;
[[nodiscard]] EventValidationError validateEvent(const Event& event) noexcept;

}  // namespace tickforge
