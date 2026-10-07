// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Derived market-state view (SPEC.md 9.5, SPEC.md section 5).
//
// Market State in the TickForge pipeline is "the MBO book plus derived
// views (best bid/ask, level aggregates) after each event." Aggregated
// (level-2/MBP) views are derived, never primary: they are computed from
// the order-by-order book and must never be mistaken for order identity.
//
// Semantic boundary:
//   * The order-event stream (NewOrder/ModifyOrder/CancelOrder) drives the
//     matching engine and mutates the OrderBook. Order ids, arrivalSeq,
//     queue positions, and fills live there.
//   * MarketStateView is a read-only lens over the OrderBook. It reports
//     best bid/ask and per-level aggregates (total quantity, order
//     count) in deterministic price order. It creates no orders, assigns
//     no sequences, produces no fills, and mutates nothing.
//
// The direction is strictly book -> aggregates. The view never
// reconstructs order identity from aggregate data.

#pragma once

#include "tickforge/book/order_book.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace tickforge {

// Aggregate state of one price level, derived from resting orders.
// totalQuantity is the sum of remaining lots at the level; orderCount is
// the number of resting orders. Neither implies anything about
// individual order identity beyond what the book already reports.
struct LevelAggregate {
  Price price;
  Quantity totalQuantity;
  std::size_t orderCount{0};

  friend bool operator==(const LevelAggregate& a, const LevelAggregate& b) noexcept = default;
};

// Read-only derived market-state view over an OrderBook.
//
// The view holds a reference and reflects the book's state at query
// time; it performs no caching and no mutation. All queries are
// deterministic: levels are returned best-first (bids high to low,
// asks low to high), matching the book's price priority order.
class MarketStateView {
public:
  explicit MarketStateView(const OrderBook& book) noexcept;

  [[nodiscard]] std::optional<Price> bestBid() const;
  [[nodiscard]] std::optional<Price> bestAsk() const;
  [[nodiscard]] std::size_t orderCount() const noexcept;

  // All price levels on the side, best first. Empty when the side is empty.
  [[nodiscard]] std::vector<LevelAggregate> levels(Side side) const;
  // Aggregate for one price level. Empty when the level does not exist.
  [[nodiscard]] std::optional<LevelAggregate> level(Side side, Price price) const;

private:
  const OrderBook& book_;
};

} // namespace tickforge
