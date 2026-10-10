// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Event generator implementation. See event_generator.hpp for the
// design contract.

#include "tests/support/event_generator.hpp"

#include <algorithm>
#include <sstream>

namespace tickforge::test {

EventGenerator::EventGenerator(const GeneratorConfig& config)
    : config_(config), rng_(config.seed) {}

Side EventGenerator::randomSide() {
  return rng_.nextBool() ? Side::Bid : Side::Ask;
}

Price EventGenerator::randomPrice() {
  return Price{static_cast<std::int64_t>(rng_.nextRange(
      static_cast<std::uint64_t>(config_.minPrice), static_cast<std::uint64_t>(config_.maxPrice)))};
}

Quantity EventGenerator::randomQuantity() {
  return Quantity{
      static_cast<std::int64_t>(rng_.nextRange(static_cast<std::uint64_t>(config_.minQuantity),
                                               static_cast<std::uint64_t>(config_.maxQuantity)))};
}

OrderId EventGenerator::randomLiveId() {
  return randomLiveOrder().id;
}

ReferenceOrder EventGenerator::randomLiveOrder() {
  // Deterministic order: sort by id so the same seed always picks the
  // same order regardless of the book's internal layout.
  std::vector<ReferenceOrder> live = book_.liveOrders();
  std::sort(live.begin(), live.end(), [](const ReferenceOrder& a, const ReferenceOrder& b) {
    return a.id < b.id;
  });
  return live[rng_.nextBounded(live.size())];
}

Event EventGenerator::makeNewOrder() {
  Event event;
  event.type = EventType::NewOrder;
  event.orderId = OrderId{nextOrderId_++};
  event.side = randomSide();
  event.price = randomPrice();
  event.quantity = randomQuantity();
  event.timestamp = Timestamp{timestamp_};
  event.instrument = config_.instrument;
  timestamp_ += static_cast<std::int64_t>(rng_.nextRange(1, 1000));
  return event;
}

Event EventGenerator::makeCancelOrder() {
  Event event;
  event.type = EventType::CancelOrder;
  event.orderId = randomLiveId();
  event.timestamp = Timestamp{timestamp_};
  event.instrument = config_.instrument;
  timestamp_ += static_cast<std::int64_t>(rng_.nextRange(1, 1000));
  return event;
}

Event EventGenerator::makeModifyOrder() {
  const ReferenceOrder currentOrder = randomLiveOrder();
  Event event;
  event.type = EventType::ModifyOrder;
  event.orderId = currentOrder.id;
  event.timestamp = Timestamp{timestamp_};
  event.instrument = config_.instrument;

  // Must change at least one of price or quantity (validation rule).
  const bool changePrice = rng_.nextBool();
  const bool changeQuantity = !changePrice || rng_.nextBool();
  if (changePrice) {
    Price new_price = randomPrice();
    if (new_price == currentOrder.price) {
      new_price = Price{new_price.ticks() + (rng_.nextBool() ? 1 : -1)};
      if (new_price.ticks() < 1) {
        new_price = Price{1};
      }
    }
    event.price = new_price;
  } else {
    event.price = Price{0}; // unchanged
  }
  if (changeQuantity) {
    Quantity new_quantity = randomQuantity();
    if (new_quantity == currentOrder.quantity) {
      new_quantity = Quantity{new_quantity.lots() + 1};
    }
    event.quantity = new_quantity;
  } else {
    event.quantity = Quantity{0}; // unchanged
  }
  timestamp_ += static_cast<std::int64_t>(rng_.nextRange(1, 1000));
  return event;
}

std::vector<Event> EventGenerator::generate() {
  std::vector<Event> events;
  events.reserve(config_.eventCount);
  const int totalWeight = config_.newWeight + config_.cancelWeight + config_.modifyWeight;
  for (std::size_t i = 0; i < config_.eventCount; ++i) {
    Event event;
    const bool hasLive = book_.orderCount() > 0;
    if (!hasLive) {
      event = makeNewOrder();
    } else {
      const int roll = static_cast<int>(rng_.nextBounded(static_cast<std::uint64_t>(totalWeight)));
      if (roll < config_.newWeight) {
        event = makeNewOrder();
      } else if (roll < config_.newWeight + config_.cancelWeight) {
        event = makeCancelOrder();
      } else {
        event = makeModifyOrder();
      }
    }
    event.sequence = Sequence{i};
    // Keep the internal book in sync so later cancels/modifies stay valid.
    book_.apply(event);
    events.push_back(event);
  }
  return events;
}

std::string EventGenerator::describe(const Event& event) {
  std::ostringstream out;
  out << "seq=" << event.sequence.value() << " ";
  switch (event.type) {
  case EventType::NewOrder:
    out << "New id=" << event.orderId.value()
        << " side=" << (event.side == Side::Bid ? "Bid" : "Ask") << " price=" << event.price.ticks()
        << " qty=" << event.quantity.lots();
    break;
  case EventType::CancelOrder:
    out << "Cancel id=" << event.orderId.value();
    break;
  case EventType::ModifyOrder:
    out << "Modify id=" << event.orderId.value() << " price="
        << (event.price.ticks() == 0 ? "(unchanged)" : std::to_string(event.price.ticks()))
        << " qty="
        << (event.quantity.lots() == 0 ? "(unchanged)" : std::to_string(event.quantity.lots()));
    break;
  }
  out << " ts=" << event.timestamp.count();
  return out.str();
}

} // namespace tickforge::test
