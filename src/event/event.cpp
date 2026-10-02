// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Implementation of the domain primitives' non-trivial functions.
// Kept out of the headers so the public interface stays declarative.

#include "tickforge/event/event_type.hpp"
#include "tickforge/event/side.hpp"

#include <string_view>

namespace tickforge {

std::string_view toString(Side side) noexcept {
  switch (side) {
    case Side::Bid:
      return "Bid";
    case Side::Ask:
      return "Ask";
  }
  return "Unknown";
}

Side opposite(Side side) noexcept {
  return side == Side::Bid ? Side::Ask : Side::Bid;
}

std::string_view toString(EventType type) noexcept {
  switch (type) {
    case EventType::NewOrder:
      return "NewOrder";
    case EventType::ModifyOrder:
      return "ModifyOrder";
    case EventType::CancelOrder:
      return "CancelOrder";
  }
  return "Unknown";
}

}  // namespace tickforge
