// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Independent reference model implementation. See reference_model.hpp
// for the design contract.

#include "tests/support/reference_model.hpp"

#include <algorithm>

namespace tickforge::test {

bool ReferenceBook::crosses(Side aggressorSide, Price limit, Price resting) noexcept {
  if (aggressorSide == Side::Bid) {
    return limit.ticks() >= resting.ticks();
  }
  return limit.ticks() <= resting.ticks();
}

std::optional<std::size_t> ReferenceBook::bestEligible(Side aggressorSide,
                                                       Price limit) const noexcept {
  const Side opposite = (aggressorSide == Side::Bid) ? Side::Ask : Side::Bid;
  std::optional<std::size_t> best;
  for (std::size_t i = 0; i < resting_.size(); ++i) {
    const ReferenceOrder& order = resting_[i];
    if (order.side != opposite || !crosses(aggressorSide, limit, order.price)) {
      continue;
    }
    if (!best.has_value()) {
      best = i;
      continue;
    }
    const ReferenceOrder& current = resting_[*best];
    // Price priority, then time priority (SPEC.md 6).
    const bool betterPrice = (opposite == Side::Ask)
                                 ? (order.price.ticks() < current.price.ticks())
                                 : (order.price.ticks() > current.price.ticks());
    if (betterPrice || (order.price == current.price && order.arrivalSeq < current.arrivalSeq)) {
      best = i;
    }
  }
  return best;
}

std::vector<Fill> ReferenceBook::matchAggressor(
    OrderId id, Side side, Price limit, Quantity quantity, Timestamp timestamp, Sequence seq) {
  std::vector<Fill> fills;
  std::int64_t remaining = quantity.lots();
  while (remaining > 0) {
    const auto best = bestEligible(side, limit);
    if (!best.has_value()) {
      break;
    }
    ReferenceOrder& resting = resting_[*best];
    const std::int64_t fillLots = std::min(remaining, resting.quantity.lots());
    fills.push_back(Fill{id, resting.id, side, resting.price, Quantity{fillLots}, timestamp, seq});
    resting.quantity = Quantity{resting.quantity.lots() - fillLots};
    remaining -= fillLots;
    if (resting.quantity.lots() == 0) {
      resting_.erase(resting_.begin() + static_cast<std::ptrdiff_t>(*best));
    }
  }
  if (remaining > 0) {
    resting_.push_back(ReferenceOrder{id, side, limit, Quantity{remaining}, seq});
  }
  return fills;
}

ReferenceBook::ApplyResult ReferenceBook::apply(const Event& event) {
  ApplyResult result;
  switch (event.type) {
  case EventType::NewOrder: {
    for (const auto& order : resting_) {
      if (order.id == event.orderId) {
        return result; // duplicate live id: rejected
      }
    }
    result.fills = matchAggressor(
        event.orderId, event.side, event.price, event.quantity, event.timestamp, event.sequence);
    result.applied = true;
    return result;
  }
  case EventType::CancelOrder: {
    for (auto it = resting_.begin(); it != resting_.end(); ++it) {
      if (it->id == event.orderId) {
        resting_.erase(it);
        result.applied = true;
        return result;
      }
    }
    return result; // unknown order: rejected
  }
  case EventType::ModifyOrder: {
    for (std::size_t i = 0; i < resting_.size(); ++i) {
      if (resting_[i].id == event.orderId) {
        return applyModify(i, event);
      }
    }
    return result; // unknown order: rejected
  }
  }
  return result;
}

ReferenceBook::ApplyResult ReferenceBook::applyModify(std::size_t index, const Event& event) {
  ApplyResult result;
  ReferenceOrder& current = resting_[index];
  // "0 means unchanged" (event contract).
  const Price newPrice = event.price.ticks() != 0 ? event.price : current.price;
  const Quantity newQuantity = event.quantity.lots() != 0 ? event.quantity : current.quantity;
  const bool priceChanged = newPrice != current.price;
  const bool quantityIncreased = newQuantity.lots() > current.quantity.lots();
  if (priceChanged || quantityIncreased) {
    // Cancel/replace (SPEC.md 5.5): loses priority, may match.
    const Side side = current.side;
    resting_.erase(resting_.begin() + static_cast<std::ptrdiff_t>(index));
    result.fills =
        matchAggressor(event.orderId, side, newPrice, newQuantity, event.timestamp, event.sequence);
    result.applied = true;
    return result;
  }
  // Quantity decrease at same price: in place, priority kept.
  current.quantity = newQuantity;
  result.applied = true;
  return result;
}

std::size_t ReferenceBook::orderCount() const {
  return resting_.size();
}

std::optional<ReferenceOrder> ReferenceBook::find(OrderId id) const {
  for (const auto& order : resting_) {
    if (order.id == id) {
      return order;
    }
  }
  return std::nullopt;
}

std::optional<Price> ReferenceBook::bestBid() const {
  std::optional<Price> best;
  for (const auto& order : resting_) {
    if (order.side == Side::Bid && (!best.has_value() || order.price > *best)) {
      best = order.price;
    }
  }
  return best;
}

std::optional<Price> ReferenceBook::bestAsk() const {
  std::optional<Price> best;
  for (const auto& order : resting_) {
    if (order.side == Side::Ask && (!best.has_value() || order.price < *best)) {
      best = order.price;
    }
  }
  return best;
}

std::vector<ReferenceOrder> ReferenceBook::liveOrders() const {
  return resting_;
}

std::vector<Price> ReferenceBook::priceLevels(Side side) const {
  std::vector<Price> prices;
  prices.reserve(resting_.size());
  for (const auto& order : resting_) {
    if (order.side == side &&
        std::find(prices.begin(), prices.end(), order.price) == prices.end()) {
      prices.push_back(order.price);
    }
  }
  std::sort(prices.begin(), prices.end(), [side](Price a, Price b) {
    return (side == Side::Bid) ? (a.ticks() > b.ticks()) : (a.ticks() < b.ticks());
  });
  return prices;
}

std::vector<OrderId> ReferenceBook::ordersAtLevel(Side side, Price price) const {
  std::vector<ReferenceOrder> level;
  level.reserve(resting_.size());
  for (const auto& order : resting_) {
    if (order.side == side && order.price == price) {
      level.push_back(order);
    }
  }
  std::sort(level.begin(), level.end(), [](const ReferenceOrder& a, const ReferenceOrder& b) {
    return a.arrivalSeq < b.arrivalSeq;
  });
  std::vector<OrderId> ids;
  ids.reserve(level.size());
  for (const auto& order : level) {
    ids.push_back(order.id);
  }
  return ids;
}

} // namespace tickforge::test
