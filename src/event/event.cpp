// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Implementation of the normalized event model's non-trivial functions:
// enum stringification and event validation. Kept out of the headers so
// the public interface stays declarative; see include/tickforge/event/.

#include "tickforge/event/event.hpp"

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

std::string_view toString(EventValidationError error) noexcept {
  switch (error) {
  case EventValidationError::Ok:
    return "Ok";
  case EventValidationError::EmptyInstrument:
    return "EmptyInstrument";
  case EventValidationError::InvalidSide:
    return "InvalidSide";
  case EventValidationError::InvalidPrice:
    return "InvalidPrice";
  case EventValidationError::InvalidQuantity:
    return "InvalidQuantity";
  case EventValidationError::EmptyModify:
    return "EmptyModify";
  case EventValidationError::UnexpectedCancelPayload:
    return "UnexpectedCancelPayload";
  }
  return "Unknown";
}

EventValidationError validateEvent(const Event& event) noexcept {
  if (event.instrument.empty()) {
    return EventValidationError::EmptyInstrument;
  }
  if (event.side != Side::Bid && event.side != Side::Ask) {
    return EventValidationError::InvalidSide;
  }

  const bool priceSet = event.price.ticks() != 0;
  const bool quantitySet = event.quantity.lots() != 0;

  switch (event.type) {
  case EventType::NewOrder:
    // A new order must carry a strictly positive price and quantity.
    if (event.price.ticks() <= 0) {
      return EventValidationError::InvalidPrice;
    }
    if (event.quantity.lots() <= 0) {
      return EventValidationError::InvalidQuantity;
    }
    return EventValidationError::Ok;
  case EventType::ModifyOrder:
    // Zero means "field unchanged"; negative values are never valid.
    // At least one field must actually change.
    if (event.price.ticks() < 0) {
      return EventValidationError::InvalidPrice;
    }
    if (event.quantity.lots() < 0) {
      return EventValidationError::InvalidQuantity;
    }
    if (!priceSet && !quantitySet) {
      return EventValidationError::EmptyModify;
    }
    return EventValidationError::Ok;
  case EventType::CancelOrder:
    // A cancel carries no price or quantity; anything set is stale
    // data from a buggy producer.
    if (priceSet || quantitySet) {
      return EventValidationError::UnexpectedCancelPayload;
    }
    return EventValidationError::Ok;
  }
  return EventValidationError::Ok;
}

} // namespace tickforge
