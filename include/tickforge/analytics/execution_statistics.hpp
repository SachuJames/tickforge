// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// ExecutionStatistics: deterministic aggregate statistics over Fill
// records (SPEC.md section 9, stage 6: "Execution Results").
//
// Design:
//   * Pure observer: consume fills via addFill()/addFills(). Never
//     affects matching, never reads the book.
//   * All arithmetic is exact integer arithmetic. Prices are in ticks,
//     quantities in lots. The quantity-weighted average price is kept as
//     an exact rational (sum of price*quantity over sum of quantity);
//     no floating point is used anywhere.
//   * A single int64 price times an int64 quantity can overflow int64, so
//     the price*quantity sum is accumulated in 128 bits using a portable
//     two-word (hi/lo) unsigned accumulator. This is a deliberate,
//     documented precision choice, not a premature optimization. __int128
//     is avoided because it is not ISO C++ and trips -Wpedantic.

#pragma once

#include "tickforge/event/event.hpp"
#include "tickforge/matching/fill.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace tickforge {

class ExecutionStatistics {
public:
  ExecutionStatistics() = default;

  void addFill(const Fill& fill);
  void addFills(std::span<const Fill> fills);

  [[nodiscard]] std::size_t fillCount() const noexcept;
  [[nodiscard]] Quantity totalQuantity() const noexcept; // lots, all fills
  [[nodiscard]] Quantity buyQuantity() const noexcept;  // lots, aggressor Bid
  [[nodiscard]] Quantity sellQuantity() const noexcept; // lots, aggressor Ask
  [[nodiscard]] std::optional<Price> minPrice() const noexcept; // ticks
  [[nodiscard]] std::optional<Price> maxPrice() const noexcept; // ticks

  // Exact quantity-weighted average execution price:
  //   sum(price_ticks * quantity_lots) / sum(quantity_lots)
  // The numerator is a 128-bit unsigned integer (hi/lo words) because a
  // single price*quantity product can exceed 64 bits. Empty when no fills
  // have been observed.
  struct WeightedAveragePrice {
    std::uint64_t priceQuantitySumHi{0}; // high 64 bits of the numerator
    std::uint64_t priceQuantitySumLo{0}; // low 64 bits of the numerator
    std::int64_t quantitySum{0};         // denominator in lots, positive
  };
  [[nodiscard]] std::optional<WeightedAveragePrice> averagePrice() const noexcept;

private:
  std::size_t fillCount_{0};
  std::int64_t totalLots_{0};
  std::int64_t buyLots_{0};
  std::int64_t sellLots_{0};
  std::optional<Price> minPrice_;
  std::optional<Price> maxPrice_;
  std::uint64_t priceQuantitySumHi_{0};
  std::uint64_t priceQuantitySumLo_{0};

  // Add price*quantity to the 128-bit accumulator. Both inputs are
  // non-negative (fill prices and quantities are positive).
  void addPriceQuantity(Price price, Quantity quantity);
};

} // namespace tickforge
