// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Independent reference model for the MBO book and price-time-priority
// matching (test oracle only, never production).
//
// Written directly from SPEC.md sections 5, 6, and 7 without using any
// production code. Deliberately simple: a flat vector of resting
// orders, with matching done by repeated scans for the best eligible
// price then earliest arrivalSeq. This structure is maximally
// independent from the production book (ordered maps + FIFO queues).
// Used for differential testing against OrderBook + MatchingEngine.
//
// Where the specification leaves room for interpretation, this model
// follows the production contract (e.g. "0 means unchanged" in
// ModifyOrder, from the Day 02 event encoding). Any divergence found
// between this model and production is investigated as a potential
// defect; the model itself is validated against hand-calculated cases
// in test_reference_model.cpp.

#pragma once

#include "tickforge/event/event.hpp"
#include "tickforge/matching/fill.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace tickforge::test {

struct ReferenceOrder {
  OrderId id;
  Side side;
  Price price;
  Quantity quantity; // remaining lots
  Sequence arrivalSeq;
};

class ReferenceBook {
public:
  ReferenceBook() = default;

  struct ApplyResult {
    bool applied = false;
    std::vector<Fill> fills;
  };

  // Applies one validated event. Returns whether it was applied and
  // any fills produced.
  ApplyResult apply(const Event& event);

  // Queries for invariant checks (mirror the production read API).
  [[nodiscard]] std::size_t orderCount() const;
  [[nodiscard]] std::optional<ReferenceOrder> find(OrderId id) const;
  [[nodiscard]] std::optional<Price> bestBid() const;
  [[nodiscard]] std::optional<Price> bestAsk() const;
  [[nodiscard]] std::vector<Price> priceLevels(Side side) const;                  // best-first
  [[nodiscard]] std::vector<OrderId> ordersAtLevel(Side side, Price price) const; // FIFO
  // All resting orders, in no particular order.
  [[nodiscard]] std::vector<ReferenceOrder> liveOrders() const;

private:
  std::vector<ReferenceOrder> resting_;

  [[nodiscard]] static bool crosses(Side aggressorSide, Price limit, Price resting) noexcept;
  // Finds the index of the best eligible resting order for an
  // aggressor, or nullopt when none crosses. Best = best price, then
  // earliest arrivalSeq (SPEC.md 6).
  [[nodiscard]] std::optional<std::size_t> bestEligible(Side aggressorSide,
                                                        Price limit) const noexcept;
  std::vector<Fill> matchAggressor(
      OrderId id, Side side, Price limit, Quantity quantity, Timestamp timestamp, Sequence seq);
  // Applies a ModifyOrder to the resting order at index. Handles the
  // cancel/replace vs in-place distinction (SPEC.md 5.5).
  ApplyResult applyModify(std::size_t index, const Event& event);
};

} // namespace tickforge::test
