// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// ExecutionStatistics implementation. All accumulation is exact; the
// weighted average is stored as a rational, never rounded to floating
// point.

#include "tickforge/analytics/execution_statistics.hpp"

namespace tickforge {

void ExecutionStatistics::addFill(const Fill& fill) {
  ++fillCount_;
  totalLots_ += fill.quantity.lots();
  if (fill.side == Side::Bid) {
    buyLots_ += fill.quantity.lots();
  } else {
    sellLots_ += fill.quantity.lots();
  }
  if (!minPrice_.has_value() || fill.price < *minPrice_) {
    minPrice_ = fill.price;
  }
  if (!maxPrice_.has_value() || fill.price > *maxPrice_) {
    maxPrice_ = fill.price;
  }
  addPriceQuantity(fill.price, fill.quantity);
}

// Adds the 128-bit product price*quantity into (priceQuantitySumHi_,
// priceQuantitySumLo_). Uses 32-bit limbs so no intermediate exceeds 64
// bits; fully portable ISO C++.
void ExecutionStatistics::addPriceQuantity(Price price, Quantity quantity) {
  const auto priceTicks = static_cast<std::uint64_t>(price.ticks());
  const auto lots = static_cast<std::uint64_t>(quantity.lots());

  const std::uint64_t priceLo = priceTicks & 0xFFFFFFFFULL;
  const std::uint64_t priceHi = priceTicks >> 32;
  const std::uint64_t qtyLo = lots & 0xFFFFFFFFULL;
  const std::uint64_t qtyHi = lots >> 32;

  // 64-bit partial products (32-bit * 32-bit cannot overflow).
  const std::uint64_t lowLow = priceLo * qtyLo;   // bits 0..63
  const std::uint64_t lowHigh = priceLo * qtyHi;  // bits 32..95
  const std::uint64_t highLow = priceHi * qtyLo;  // bits 32..95
  const std::uint64_t highHigh = priceHi * qtyHi; // bits 64..127

  // Assemble the 128-bit product.
  std::uint64_t prod_lo = lowLow;
  std::uint64_t prod_hi = highHigh;

  // Add (lowHigh << 32).
  std::uint64_t shifted = (lowHigh & 0xFFFFFFFFULL) << 32;
  std::uint64_t new_lo = prod_lo + shifted;
  prod_hi += (lowHigh >> 32) + ((new_lo < prod_lo) ? 1ULL : 0ULL);
  prod_lo = new_lo;

  // Add (highLow << 32).
  shifted = (highLow & 0xFFFFFFFFULL) << 32;
  new_lo = prod_lo + shifted;
  prod_hi += (highLow >> 32) + ((new_lo < prod_lo) ? 1ULL : 0ULL);
  prod_lo = new_lo;

  // Accumulate into the running sum.
  new_lo = priceQuantitySumLo_ + prod_lo;
  priceQuantitySumHi_ += prod_hi + ((new_lo < priceQuantitySumLo_) ? 1ULL : 0ULL);
  priceQuantitySumLo_ = new_lo;
}

void ExecutionStatistics::addFills(std::span<const Fill> fills) {
  for (const Fill& fill : fills) {
    addFill(fill);
  }
}

std::size_t ExecutionStatistics::fillCount() const noexcept {
  return fillCount_;
}

Quantity ExecutionStatistics::totalQuantity() const noexcept {
  return Quantity{totalLots_};
}

Quantity ExecutionStatistics::buyQuantity() const noexcept {
  return Quantity{buyLots_};
}

Quantity ExecutionStatistics::sellQuantity() const noexcept {
  return Quantity{sellLots_};
}

std::optional<Price> ExecutionStatistics::minPrice() const noexcept {
  return minPrice_;
}

std::optional<Price> ExecutionStatistics::maxPrice() const noexcept {
  return maxPrice_;
}

std::optional<ExecutionStatistics::WeightedAveragePrice>
ExecutionStatistics::averagePrice() const noexcept {
  if (fillCount_ == 0) {
    return std::nullopt;
  }
  return WeightedAveragePrice{priceQuantitySumHi_, priceQuantitySumLo_, totalLots_};
}

} // namespace tickforge
