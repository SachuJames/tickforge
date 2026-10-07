// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// MarketStateView implementation. Every aggregate is computed from the
// OrderBook's public read APIs (priceLevels, ordersAtLevel, find); the
// view never touches book internals and never mutates state.

#include "tickforge/market_data/market_state.hpp"

namespace tickforge {

MarketStateView::MarketStateView(const OrderBook& book) noexcept : book_(book) {}

std::optional<Price> MarketStateView::bestBid() const {
  return book_.bestBid();
}

std::optional<Price> MarketStateView::bestAsk() const {
  return book_.bestAsk();
}

std::size_t MarketStateView::orderCount() const noexcept {
  return book_.orderCount();
}

namespace {

// Aggregate one price level from the order ids resting there.
// Returns nullopt when the level holds no orders.
[[nodiscard]] std::optional<LevelAggregate>
aggregateLevel(const OrderBook& book, Side side, Price price) {
  const std::vector<OrderId> ids = book.ordersAtLevel(side, price);
  if (ids.empty()) {
    return std::nullopt;
  }
  std::int64_t total_lots = 0;
  for (const OrderId id : ids) {
    const std::optional<RestingOrder> order = book.find(id);
    // The id came from the book's own level listing, so it must exist.
    // If it does not, the book is internally inconsistent; skip rather
    // than fabricate.
    if (order.has_value()) {
      total_lots += order->quantity.lots();
    }
  }
  return LevelAggregate{price, Quantity{total_lots}, ids.size()};
}

} // namespace

std::vector<LevelAggregate> MarketStateView::levels(Side side) const {
  std::vector<LevelAggregate> result;
  const std::vector<Price> prices = book_.priceLevels(side);
  result.reserve(prices.size());
  for (const Price price : prices) {
    const std::optional<LevelAggregate> aggregate = aggregateLevel(book_, side, price);
    // priceLevels() only lists levels that hold orders.
    if (aggregate.has_value()) {
      result.push_back(*aggregate);
    }
  }
  return result;
}

std::optional<LevelAggregate> MarketStateView::level(Side side, Price price) const {
  return aggregateLevel(book_, side, price);
}

} // namespace tickforge
