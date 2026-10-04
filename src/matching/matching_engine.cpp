// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Matching engine implementation. See
// include/tickforge/matching/matching_engine.hpp for the semantics.

#include "tickforge/matching/matching_engine.hpp"

#include <algorithm>
#include <cassert>

namespace tickforge {

MatchingEngine::MatchingEngine(OrderBook& book) : book_(book) {}

bool MatchingEngine::onEvent(const Event& event) {
  fills_.clear();
  switch (event.type) {
  case EventType::NewOrder:
    return processNewOrder(event);
  case EventType::ModifyOrder:
    return processModify(event);
  case EventType::CancelOrder:
    return book_.onEvent(event);
  }
  return false;
}

const std::vector<Fill>& MatchingEngine::fills() const noexcept {
  return fills_;
}

OrderBook& MatchingEngine::book() noexcept {
  return book_;
}

bool MatchingEngine::processNewOrder(const Event& event) {
  if (book_.contains(event.orderId)) {
    return false;
  }
  matchAggressor(
      event.orderId, event.side, event.price, event.quantity, event.timestamp, event.sequence);
  return true;
}

bool MatchingEngine::processModify(const Event& event) {
  const auto current = book_.find(event.orderId);
  if (!current.has_value()) {
    return false;
  }

  // Zero means "unchanged" (Day 02 encoding, enforced by validateEvent).
  const Price newPrice = event.price.ticks() != 0 ? event.price : current->price;
  const Quantity newQuantity = event.quantity.lots() != 0 ? event.quantity : current->quantity;
  const bool priceChanged = newPrice != current->price;
  const bool quantityIncreased = newQuantity.lots() > current->quantity.lots();

  if (priceChanged || quantityIncreased) {
    // Cancel/replace (SPEC.md 5.5): the replacement loses time priority
    // and may now be aggressive. Remove it, then run it as a potential
    // aggressor: matchAggressor matches what crosses and rests any
    // residual with the modify event's seq as the new arrivalSeq. A
    // quantity increase at the same price can never newly cross, so it
    // simply rejoins at the back of its level.
    const Side side = current->side;
    const bool removed = book_.removeOrder(event.orderId);
    assert(removed);
    (void)removed;
    matchAggressor(event.orderId, side, newPrice, newQuantity, event.timestamp, event.sequence);
    return true;
  }
  // Quantity decrease at the same price: the book preserves priority.
  return book_.onEvent(event);
}

void MatchingEngine::matchAggressor(OrderId aggressorId,
                                    Side side,
                                    Price limitPrice,
                                    Quantity quantity,
                                    Timestamp timestamp,
                                    Sequence arrivalSeq) {
  Quantity remaining = quantity;
  const Side opposite = side == Side::Bid ? Side::Ask : Side::Bid;

  // Price levels arrive best-first, so the first non-crossing level ends
  // the sweep: no worse price can be eligible.
  for (const Price levelPrice : book_.priceLevels(opposite)) {
    if (remaining.lots() <= 0) {
      break;
    }
    if (!crosses(side, limitPrice, levelPrice)) {
      break;
    }
    for (const OrderId restingId : book_.ordersAtLevel(opposite, levelPrice)) {
      if (remaining.lots() <= 0) {
        break;
      }
      const auto resting = book_.find(restingId);
      // Internal invariant: an id from the level snapshot is live until
      // this loop removes it, and the loop visits ids in order.
      assert(resting.has_value());
      const Quantity fillQty{std::min(remaining.lots(), resting->quantity.lots())};
      assert(fillQty.lots() > 0);
      fills_.push_back(
          Fill{aggressorId, restingId, side, levelPrice, fillQty, timestamp, arrivalSeq});
      const bool reduced = book_.reduceQuantity(restingId, fillQty);
      assert(reduced);
      (void)reduced;
      remaining = Quantity{remaining.lots() - fillQty.lots()};
    }
  }

  // Unfilled residual rests (SPEC.md 7: an incompletely matched incoming
  // order's remainder rests). No IOC/FOK exists in the model.
  if (remaining.lots() > 0) {
    const bool rested = book_.addRestingOrder(aggressorId, side, limitPrice, remaining, arrivalSeq);
    assert(rested);
    (void)rested;
  }
}

bool MatchingEngine::crosses(Side aggressorSide, Price limitPrice, Price restingPrice) noexcept {
  if (aggressorSide == Side::Bid) {
    return limitPrice.ticks() >= restingPrice.ticks();
  }
  return limitPrice.ticks() <= restingPrice.ticks();
}

} // namespace tickforge
