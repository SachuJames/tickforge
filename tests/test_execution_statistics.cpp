// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// ExecutionStatistics tests: exact integer arithmetic, quantity-weighted
// averages, min/max, buy/sell splits, conservation, and determinism.

#include "tickforge/analytics/execution_statistics.hpp"
#include "tickforge/event/event.hpp"
#include "tickforge/matching/fill.hpp"

#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

namespace {

using tickforge::ExecutionStatistics;
using tickforge::Fill;
using tickforge::OrderId;
using tickforge::Price;
using tickforge::Quantity;
using tickforge::Sequence;
using tickforge::Side;
using tickforge::Timestamp;

Fill makeFill(OrderId aggressor, OrderId resting, Side side, Price price, Quantity quantity) {
  Fill fill;
  fill.aggressorId = aggressor;
  fill.restingId = resting;
  fill.side = side;
  fill.price = price;
  fill.quantity = quantity;
  fill.timestamp = Timestamp{1000};
  fill.sequence = Sequence{7};
  return fill;
}

TEST(ExecutionStatisticsTest, EmptyStatistics) {
  const ExecutionStatistics stats;
  EXPECT_EQ(stats.fillCount(), 0U);
  EXPECT_EQ(stats.totalQuantity(), Quantity{0});
  EXPECT_EQ(stats.buyQuantity(), Quantity{0});
  EXPECT_EQ(stats.sellQuantity(), Quantity{0});
  EXPECT_FALSE(stats.minPrice().has_value());
  EXPECT_FALSE(stats.maxPrice().has_value());
  EXPECT_FALSE(stats.averagePrice().has_value());
}

TEST(ExecutionStatisticsTest, SingleFill) {
  ExecutionStatistics stats;
  stats.addFill(makeFill(OrderId{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10}));

  EXPECT_EQ(stats.fillCount(), 1U);
  EXPECT_EQ(stats.totalQuantity(), Quantity{10});
  EXPECT_EQ(stats.buyQuantity(), Quantity{10});
  EXPECT_EQ(stats.sellQuantity(), Quantity{0});
  EXPECT_EQ(stats.minPrice(), std::optional<Price>(Price{100}));
  EXPECT_EQ(stats.maxPrice(), std::optional<Price>(Price{100}));
  const auto avg = stats.averagePrice().value_or(ExecutionStatistics::WeightedAveragePrice{});
  EXPECT_EQ(avg.priceQuantitySumHi, 0ULL);
  EXPECT_EQ(avg.priceQuantitySumLo, 1000ULL);
  EXPECT_EQ(avg.quantitySum, 10);
}

TEST(ExecutionStatisticsTest, QuantityWeightedAverage) {
  // (100*5 + 101*10) / 15 = 1510/15. Exact rational, no floating point.
  ExecutionStatistics stats;
  stats.addFill(makeFill(OrderId{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{5}));
  stats.addFill(makeFill(OrderId{1}, OrderId{3}, Side::Bid, Price{101}, Quantity{10}));

  EXPECT_EQ(stats.fillCount(), 2U);
  EXPECT_EQ(stats.totalQuantity(), Quantity{15});
  const auto avg = stats.averagePrice().value_or(ExecutionStatistics::WeightedAveragePrice{});
  EXPECT_EQ(avg.priceQuantitySumHi, 0ULL);
  EXPECT_EQ(avg.priceQuantitySumLo, 1510ULL);
  EXPECT_EQ(avg.quantitySum, 15);
}

TEST(ExecutionStatisticsTest, MinMaxPrices) {
  ExecutionStatistics stats;
  stats.addFill(makeFill(OrderId{1}, OrderId{2}, Side::Bid, Price{105}, Quantity{10}));
  stats.addFill(makeFill(OrderId{1}, OrderId{3}, Side::Bid, Price{100}, Quantity{10}));
  stats.addFill(makeFill(OrderId{1}, OrderId{4}, Side::Bid, Price{103}, Quantity{10}));

  EXPECT_EQ(stats.minPrice(), std::optional<Price>(Price{100}));
  EXPECT_EQ(stats.maxPrice(), std::optional<Price>(Price{105}));
}

TEST(ExecutionStatisticsTest, BuySellSplit) {
  ExecutionStatistics stats;
  stats.addFill(makeFill(OrderId{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10}));
  stats.addFill(makeFill(OrderId{3}, OrderId{4}, Side::Ask, Price{101}, Quantity{20}));
  stats.addFill(makeFill(OrderId{5}, OrderId{6}, Side::Bid, Price{102}, Quantity{30}));

  EXPECT_EQ(stats.buyQuantity(), Quantity{40});
  EXPECT_EQ(stats.sellQuantity(), Quantity{20});
  EXPECT_EQ(stats.totalQuantity(), Quantity{60});
}

TEST(ExecutionStatisticsTest, AddFillsBatch) {
  ExecutionStatistics stats;
  const std::vector<Fill> fills = {
      makeFill(OrderId{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10}),
      makeFill(OrderId{1}, OrderId{3}, Side::Bid, Price{101}, Quantity{20}),
  };
  stats.addFills(fills);

  EXPECT_EQ(stats.fillCount(), 2U);
  EXPECT_EQ(stats.totalQuantity(), Quantity{30});
}

TEST(ExecutionStatisticsTest, ConservationTotalEqualsSum) {
  ExecutionStatistics stats;
  const std::vector<Fill> fills = {
      makeFill(OrderId{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{7}),
      makeFill(OrderId{1}, OrderId{3}, Side::Bid, Price{101}, Quantity{13}),
      makeFill(OrderId{4}, OrderId{5}, Side::Ask, Price{99}, Quantity{5}),
  };
  stats.addFills(fills);

  std::int64_t sum = 0;
  for (const Fill& fill : fills) {
    sum += fill.quantity.lots();
  }
  EXPECT_EQ(stats.totalQuantity().lots(), sum);
}

TEST(ExecutionStatisticsTest, LargeValuesNoOverflow) {
  ExecutionStatistics stats;
  // price * quantity exceeds int64; the 128-bit accumulator must hold it.
  // 4e9 * 4e9 = 1.6e19 = 0xDE0B6B3A7640000, fits in 64 bits (hi == 0).
  const std::int64_t bigPrice = 4000000000LL; // 4e9 ticks
  const std::int64_t bigQty = 4000000000LL;   // 4e9 lots
  stats.addFill(makeFill(OrderId{1}, OrderId{2}, Side::Bid, Price{bigPrice}, Quantity{bigQty}));

  EXPECT_EQ(stats.totalQuantity(), Quantity{bigQty});
  const auto avg = stats.averagePrice().value_or(ExecutionStatistics::WeightedAveragePrice{});
  EXPECT_EQ(avg.priceQuantitySumHi, 0ULL);
  EXPECT_EQ(avg.priceQuantitySumLo, 16000000000000000000ULL);
  EXPECT_EQ(avg.quantitySum, bigQty);
}

TEST(ExecutionStatisticsTest, ProductExceeding64Bits) {
  ExecutionStatistics stats;
  // 2^40 ticks * 2^40 lots = 2^80, needs the high word.
  // 2^80 = 0x1_00000000000000000000: hi = 0x10000, lo = 0.
  const auto price = static_cast<std::int64_t>(1ULL << 40);
  const auto qty = static_cast<std::int64_t>(1ULL << 40);
  stats.addFill(makeFill(OrderId{1}, OrderId{2}, Side::Bid, Price{price}, Quantity{qty}));

  const auto avg = stats.averagePrice().value_or(ExecutionStatistics::WeightedAveragePrice{});
  EXPECT_EQ(avg.priceQuantitySumHi, 0x10000ULL);
  EXPECT_EQ(avg.priceQuantitySumLo, 0ULL);
  EXPECT_EQ(avg.quantitySum, qty);
}

// Helper: verify basic counts match.
void expectBasicStatsEqual(const ExecutionStatistics& first, const ExecutionStatistics& second) {
  EXPECT_EQ(first.fillCount(), second.fillCount());
  EXPECT_EQ(first.totalQuantity(), second.totalQuantity());
  EXPECT_EQ(first.buyQuantity(), second.buyQuantity());
  EXPECT_EQ(first.minPrice(), second.minPrice());
  EXPECT_EQ(first.maxPrice(), second.maxPrice());
}

// Helper: verify weighted averages match.
void expectAverageEqual(const ExecutionStatistics& first, const ExecutionStatistics& second) {
  const auto avgFirst = first.averagePrice().value_or(ExecutionStatistics::WeightedAveragePrice{});
  const auto avgSecond =
      second.averagePrice().value_or(ExecutionStatistics::WeightedAveragePrice{});
  EXPECT_EQ(avgFirst.priceQuantitySumHi, avgSecond.priceQuantitySumHi);
  EXPECT_EQ(avgFirst.priceQuantitySumLo, avgSecond.priceQuantitySumLo);
  EXPECT_EQ(avgFirst.quantitySum, avgSecond.quantitySum);
}

// Helper: verify two statistics objects are identical.
void expectStatsEqual(const ExecutionStatistics& first, const ExecutionStatistics& second) {
  expectBasicStatsEqual(first, second);
  expectAverageEqual(first, second);
}

TEST(ExecutionStatisticsTest, DeterministicForRepeatedSequences) {
  const auto run = []() {
    ExecutionStatistics stats;
    stats.addFill(makeFill(OrderId{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10}));
    stats.addFill(makeFill(OrderId{1}, OrderId{3}, Side::Bid, Price{101}, Quantity{20}));
    return stats;
  };
  const ExecutionStatistics first = run();
  const ExecutionStatistics second = run();

  expectStatsEqual(first, second);
}

} // namespace
