// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// MBO order book implementation. See include/tickforge/book/order_book.hpp
// for the representation and the Day 03 scope notes.

#include "tickforge/book/order_book.hpp"

#include <cassert>
#include <iterator>

namespace tickforge {

bool OrderBook::onEvent(const Event& event) {
  switch (event.type) {
  case EventType::NewOrder:
    return applyNew(event);
  case EventType::ModifyOrder:
    return applyModify(event);
  case EventType::CancelOrder:
    return applyCancel(event);
  }
  return false;
}

bool OrderBook::contains(OrderId id) const {
  return orders_.find(id) != orders_.end();
}

std::optional<RestingOrder> OrderBook::find(OrderId id) const {
  const auto it = orders_.find(id);
  if (it == orders_.end()) {
    return std::nullopt;
  }
  return it->second.order;
}

std::optional<Price> OrderBook::bestBid() const {
  if (bids_.empty()) {
    return std::nullopt;
  }
  return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const {
  if (asks_.empty()) {
    return std::nullopt;
  }
  return asks_.begin()->first;
}

std::size_t OrderBook::orderCount() const noexcept {
  return orders_.size();
}

std::size_t OrderBook::priceLevelCount(Side side) const noexcept {
  return side == Side::Bid ? bids_.size() : asks_.size();
}

std::vector<OrderId> OrderBook::ordersAtLevel(Side side, Price price) const {
  std::vector<OrderId> ids;
  const auto collect = [&ids, price](const auto& levels) {
    const auto it = levels.find(price);
    if (it != levels.end()) {
      ids.assign(it->second.queue.begin(), it->second.queue.end());
    }
  };
  if (side == Side::Bid) {
    collect(bids_);
  } else {
    collect(asks_);
  }
  return ids;
}

template <typename LevelMap>
void OrderBook::insertIntoLevel(LevelMap& levels, Entry& entry) {
  Queue& queue = levels[entry.order.price].queue;
  queue.push_back(entry.order.id);
  entry.queueIt = std::prev(queue.end());
}

template <typename LevelMap>
void OrderBook::removeFromLevel(LevelMap& levels, Entry& entry) {
  const auto levelIt = levels.find(entry.order.price);
  // Internal invariant: a live order always has a price level.
  assert(levelIt != levels.end());
  Queue& queue = levelIt->second.queue;
  queue.erase(entry.queueIt);
  if (queue.empty()) {
    levels.erase(levelIt);
  }
}

bool OrderBook::applyNew(const Event& event) {
  if (contains(event.orderId)) {
    return false;
  }
  // Day 03 performs no matching: even a crossing order rests in the book.
  // Matching behavior belongs to the matching-engine milestone.
  Entry entry{RestingOrder{event.orderId, event.side, event.price, event.quantity, event.sequence},
              Queue::iterator{}};
  if (event.side == Side::Bid) {
    insertIntoLevel(bids_, entry);
  } else {
    insertIntoLevel(asks_, entry);
  }
  orders_.emplace(event.orderId, entry);
  return true;
}

bool OrderBook::applyCancel(const Event& event) {
  const auto it = orders_.find(event.orderId);
  if (it == orders_.end()) {
    return false;
  }
  Entry& entry = it->second;
  if (entry.order.side == Side::Bid) {
    removeFromLevel(bids_, entry);
  } else {
    removeFromLevel(asks_, entry);
  }
  orders_.erase(it);
  return true;
}

bool OrderBook::applyModify(const Event& event) {
  const auto it = orders_.find(event.orderId);
  if (it == orders_.end()) {
    return false;
  }
  Entry& entry = it->second;

  // Zero means "unchanged" (Day 02 encoding, enforced by validateEvent).
  const Price newPrice = event.price.ticks() != 0 ? event.price : entry.order.price;
  const Quantity newQuantity = event.quantity.lots() != 0 ? event.quantity : entry.order.quantity;

  const bool priceChanged = newPrice != entry.order.price;
  const bool quantityIncreased = newQuantity.lots() > entry.order.quantity.lots();

  if (priceChanged || quantityIncreased) {
    // Cancel/replace (SPEC.md 5.5): the order loses its time priority and
    // rejoins at the back of its (possibly new) price-level queue.
    if (entry.order.side == Side::Bid) {
      removeFromLevel(bids_, entry);
    } else {
      removeFromLevel(asks_, entry);
    }
    entry.order.price = newPrice;
    entry.order.quantity = newQuantity;
    entry.order.arrivalSeq = event.sequence;
    if (entry.order.side == Side::Bid) {
      insertIntoLevel(bids_, entry);
    } else {
      insertIntoLevel(asks_, entry);
    }
  } else {
    // Quantity decrease at the same price: keep the queue position.
    entry.order.quantity = newQuantity;
  }
  return true;
}

} // namespace tickforge
