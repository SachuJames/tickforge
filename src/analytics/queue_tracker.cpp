// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// QueueTracker implementation. Position is always recomputed from the
// live book; lifecycle is maintained from the observed event/fill stream.

#include "tickforge/analytics/queue_tracker.hpp"

namespace tickforge {

void QueueTracker::designate(OrderId id) {
  designated_.insert(id);
}

void QueueTracker::release(OrderId id) {
  designated_.erase(id);
  states_.erase(id);
}

bool QueueTracker::isTracked(OrderId id) const {
  return designated_.find(id) != designated_.end();
}

QueueTracker::TrackedState& QueueTracker::stateFor(OrderId id) {
  return states_[id];
}

void QueueTracker::resetForNew(OrderId id, Quantity quantity) {
  TrackedState fresh;
  fresh.originalQuantity = quantity;
  fresh.newSeen = true;
  states_[id] = fresh;
}

void QueueTracker::onEvent(const Event& event) {
  if (!isTracked(event.orderId)) {
    return;
  }
  switch (event.type) {
  case EventType::NewOrder:
    // A new order (or an id reused after cancellation) starts fresh.
    resetForNew(event.orderId, event.quantity);
    stateFor(event.orderId).currentPrice = event.price;
    break;
  case EventType::CancelOrder:
    stateFor(event.orderId).cancelSeen = true;
    break;
  case EventType::ModifyOrder: {
    TrackedState& state = stateFor(event.orderId);
    const bool priceSet = event.price.ticks() != 0;
    // A price is "changed" only if it differs from the recorded price.
    // (A ModifyOrder may redundantly set the same price.)
    const bool priceChanged = priceSet && (event.price != state.currentPrice);
    const Quantity newQuantity =
        event.quantity.lots() != 0 ? event.quantity : state.originalQuantity;
    const bool quantityIncreased = newQuantity.lots() > state.originalQuantity.lots();
    if (priceChanged || quantityIncreased) {
      // Cancel/replace (SPEC.md 5.5): the replacement is a new order for
      // fill-accounting purposes.
      resetForNew(event.orderId, newQuantity);
      stateFor(event.orderId).currentPrice = priceSet ? event.price : state.currentPrice;
    } else {
      // In-place update (quantity decrease at same price): keep history.
      state.originalQuantity = newQuantity;
      if (priceSet) {
        state.currentPrice = event.price;
      }
    }
    break;
  }
  }
}

void QueueTracker::onFills(std::span<const Fill> fills) {
  for (const Fill& fill : fills) {
    if (isTracked(fill.aggressorId)) {
      stateFor(fill.aggressorId).filledLots += fill.quantity.lots();
    }
    if (isTracked(fill.restingId) && fill.restingId != fill.aggressorId) {
      stateFor(fill.restingId).filledLots += fill.quantity.lots();
    }
  }
}

std::optional<QueueTracker::QueuePosition>
QueueTracker::queuePosition(const OrderBook& book, OrderId id) const {
  if (!isTracked(id)) {
    return std::nullopt;
  }
  const auto order = book.find(id);
  if (!order.has_value()) {
    return std::nullopt;
  }
  const std::vector<OrderId> levelIds = book.ordersAtLevel(order->side, order->price);
  std::size_t rank = 0;
  std::int64_t ahead = 0;
  for (std::size_t i = 0; i < levelIds.size(); ++i) {
    if (levelIds[i] == id) {
      rank = i + 1; // 1-based (SPEC.md 8.1)
      break;
    }
    const auto aheadOrder = book.find(levelIds[i]);
    // Internal consistency: every id in the level snapshot is live.
    if (aheadOrder.has_value()) {
      ahead += aheadOrder->quantity.lots();
    }
  }
  if (rank == 0) {
    return std::nullopt;
  }
  return QueuePosition{order->side, order->price, rank, Quantity{ahead}, order->quantity};
}

QueueTracker::Lifecycle QueueTracker::lifecycle(const OrderBook& book, OrderId id) const {
  if (!isTracked(id)) {
    return Lifecycle::Unknown;
  }
  if (book.contains(id)) {
    return Lifecycle::Resting;
  }
  const auto it = states_.find(id);
  if (it == states_.end() || !it->second.newSeen) {
    return Lifecycle::Unknown;
  }
  const TrackedState& state = it->second;
  if (state.cancelSeen) {
    return Lifecycle::Cancelled;
  }
  if (state.originalQuantity.lots() > 0 && state.filledLots >= state.originalQuantity.lots()) {
    return Lifecycle::Filled;
  }
  return Lifecycle::Unknown;
}

} // namespace tickforge
