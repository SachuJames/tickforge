// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: Apache-2.0.
//
// Matching-engine integer boundary verification (SPEC.md 2.4).
//
// Audit basis: prices and quantities are int64. The matching path
// performs comparisons on prices only (no price arithmetic), fill
// quantities are bounded by both orders' remaining quantities, and
// ExecutionStatistics accumulates its VWAP numerator in a portable
// 128-bit hi/lo pair. Aggregate int64 sums (total lots, queue-ahead)
// are bounded by the sum of live order quantities; SPEC.md does not
// define behavior when that sum itself exceeds int64, so tests stay
// inside the representable aggregate range and the gap is documented
// in ARCHITECTURE.md instead of being assigned invented semantics.

#include "tickforge/analytics/execution_statistics.hpp"
#include "tickforge/analytics/queue_tracker.hpp"
#include "tickforge/book/order_book.hpp"
#include "tickforge/market_data/market_state.hpp"
#include "tickforge/matching/matching_engine.hpp"
#include "tickforge/replay/replay.hpp"

#include <gtest/gtest.h>
#include <limits>
#include <vector>

namespace tickforge::test {
namespace {

constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

Event boundaryEvent(EventType type,
                    std::uint64_t id,
                    Side side,
                    std::int64_t price,
                    std::int64_t quantity,
                    std::uint64_t seq) {
  Event e;
  e.type = type;
  e.orderId = OrderId{id};
  e.side = side;
  e.price = Price{price};
  e.quantity = Quantity{quantity};
  e.timestamp = Timestamp{1000000 + static_cast<std::int64_t>(seq)};
  e.sequence = Sequence{seq};
  e.instrument = "AAPL";
  return e;
}

Event newOrder(
    std::uint64_t id, Side side, std::int64_t price, std::int64_t qty, std::uint64_t seq) {
  return boundaryEvent(EventType::NewOrder, id, side, price, qty, seq);
}

// Runs two orders through the engine, returns the fills of the second.
std::vector<Fill> crossTwo(MatchingEngine& engine, const Event& first, const Event& second) {
  EXPECT_TRUE(engine.onEvent(first));
  EXPECT_TRUE(engine.onEvent(second));
  return engine.fills();
}

TEST(PriceBoundary, FillAtInt64MaxPrice) {
  OrderBook book;
  MatchingEngine engine(book);
  const auto fills =
      crossTwo(engine, newOrder(1, Side::Ask, kMax, 10, 0), newOrder(2, Side::Bid, kMax, 10, 1));
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_EQ(fills[0].price.ticks(), kMax);
  EXPECT_EQ(fills[0].quantity.lots(), 10);
  EXPECT_EQ(book.orderCount(), 0U);
}

TEST(PriceBoundary, BestPriceSelectionAtBoundary) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(engine.onEvent(newOrder(1, Side::Ask, kMax, 5, 0)));
  EXPECT_TRUE(engine.onEvent(newOrder(2, Side::Ask, kMax - 1, 5, 1)));
  EXPECT_TRUE(engine.onEvent(newOrder(3, Side::Bid, kMax, 5, 2)));
  const auto& fills = engine.fills();
  ASSERT_EQ(fills.size(), 1U);
  // The cheaper ask (kMax - 1) wins over kMax: comparison, no arithmetic.
  EXPECT_EQ(fills[0].restingId.value(), 2U);
  EXPECT_EQ(fills[0].price.ticks(), kMax - 1);
  EXPECT_EQ(book.bestAsk(), Price{kMax});
}

TEST(PriceBoundary, FillAtMinimumPositivePrice) {
  OrderBook book;
  MatchingEngine engine(book);
  const auto fills =
      crossTwo(engine, newOrder(1, Side::Ask, 1, 7, 0), newOrder(2, Side::Bid, kMax, 7, 1));
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_EQ(fills[0].price.ticks(), 1); // resting price, not the taker's kMax
}

TEST(PriceBoundary, HugeQuantityPartialFills) {
  OrderBook book;
  MatchingEngine engine(book);
  const auto fills =
      crossTwo(engine, newOrder(1, Side::Ask, 100, kMax, 0), newOrder(2, Side::Bid, 100, 10, 1));
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_EQ(fills[0].quantity.lots(), 10);
  // Maker remains with kMax - 10 (subtraction bounded by the fill).
  const auto resting = book.find(OrderId{1});
  ASSERT_TRUE(resting.has_value());
  EXPECT_EQ(resting->quantity.lots(), kMax - 10);

  // A maximal taker against a small maker rests the difference.
  OrderBook book2;
  MatchingEngine engine2(book2);
  const auto fills2 =
      crossTwo(engine2, newOrder(1, Side::Ask, 100, 5, 0), newOrder(2, Side::Bid, 100, kMax, 1));
  ASSERT_EQ(fills2.size(), 1U);
  EXPECT_EQ(fills2[0].quantity.lots(), 5);
  const auto taker = book2.find(OrderId{2});
  ASSERT_TRUE(taker.has_value());
  EXPECT_EQ(taker->quantity.lots(), kMax - 5);
}

TEST(PriceBoundary, ModifyToBoundaryPrice) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(engine.onEvent(newOrder(1, Side::Bid, 100, 10, 0)));
  // Price change to kMax: cancel/replace path, order rests at kMax.
  Event mod = boundaryEvent(EventType::ModifyOrder, 1, Side::Bid, kMax, 0, 1);
  EXPECT_TRUE(engine.onEvent(mod));
  EXPECT_EQ(book.bestBid(), Price{kMax});
  // An ask at kMax then fills against it at kMax.
  EXPECT_TRUE(engine.onEvent(newOrder(2, Side::Ask, kMax, 4, 2)));
  const auto& fills = engine.fills();
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_EQ(fills[0].price.ticks(), kMax);
  EXPECT_EQ(fills[0].quantity.lots(), 4);
}

TEST(PriceBoundary, StatisticsAtBoundary) {
  // Single fill at kMax price: the 128-bit numerator holds kMax * 3
  // exactly, so the exact VWAP is kMax itself.
  ExecutionStatistics stats;
  Fill fill;
  fill.aggressorId = OrderId{2};
  fill.restingId = OrderId{1};
  fill.side = Side::Bid;
  fill.price = Price{kMax};
  fill.quantity = Quantity{3};
  fill.timestamp = Timestamp{1000000};
  fill.sequence = Sequence{1};
  stats.addFill(fill);
  EXPECT_EQ(stats.fillCount(), 1U);
  EXPECT_EQ(stats.totalQuantity().lots(), 3);
  const auto avg = stats.averagePrice();
  ASSERT_TRUE(avg.has_value());
  EXPECT_EQ(avg->quantitySum, 3);
  // Numerator = kMax * 3 = 2^64 + 2^63 - 3, i.e. hi = 1, lo = 2^63 - 3.
  EXPECT_EQ(avg->priceQuantitySumHi, 1U);
  EXPECT_EQ(avg->priceQuantitySumLo, static_cast<std::uint64_t>(kMax) - 2);
}

TEST(PriceBoundary, AggregateWithinRepresentableRange) {
  // Two fills of kMax/2 lots each: the aggregate (kMax - 1) still fits
  // int64, and statistics stay exact.
  ExecutionStatistics stats;
  for (std::uint64_t i = 0; i < 2; ++i) {
    Fill fill;
    fill.aggressorId = OrderId{10 + i};
    fill.restingId = OrderId{20 + i};
    fill.side = Side::Bid;
    fill.price = Price{100};
    fill.quantity = Quantity{kMax / 2};
    fill.timestamp = Timestamp{1000000};
    fill.sequence = Sequence{i};
    stats.addFill(fill);
  }
  EXPECT_EQ(stats.totalQuantity().lots(), kMax - 1);
  const auto avg = stats.averagePrice();
  ASSERT_TRUE(avg.has_value());
  EXPECT_EQ(avg->quantitySum, kMax - 1);
}

TEST(PriceBoundary, ValidationRejectsNonPositivePrices) {
  // Boundary values below the valid domain are rejected by validation
  // before matching: strict replay aborts with InvalidEvent.
  OrderBook book;
  MatchingEngine engine(book);
  const std::vector<Event> zeroPrice{newOrder(1, Side::Bid, 0, 10, 0)};
  const auto r1 = replayEvents(zeroPrice, engine, ReplayMode::Strict);
  EXPECT_EQ(r1.error, ReplayError::InvalidEvent);

  OrderBook book2;
  MatchingEngine engine2(book2);
  const std::vector<Event> negativePrice{newOrder(1, Side::Bid, -1, 10, 0)};
  const auto r2 = replayEvents(negativePrice, engine2, ReplayMode::Strict);
  EXPECT_EQ(r2.error, ReplayError::InvalidEvent);

  OrderBook book3;
  MatchingEngine engine3(book3);
  const std::vector<Event> minPrice{
      newOrder(1, Side::Bid, std::numeric_limits<std::int64_t>::min(), 10, 0)};
  const auto r3 = replayEvents(minPrice, engine3, ReplayMode::Strict);
  EXPECT_EQ(r3.error, ReplayError::InvalidEvent);
}

TEST(PriceBoundary, StatisticsTotalReachesInt64MaxExactly) {
  // The largest representable aggregate: fills of kMax/2, kMax/2, and 1
  // lot sum to exactly INT64_MAX and must stay exact. (One more lot
  // would leave the representable range, which SPEC.md does not
  // define; see ARCHITECTURE.md 22.)
  ExecutionStatistics stats;
  const std::int64_t quantities[3] = {kMax / 2, kMax / 2, 1};
  for (std::uint64_t i = 0; i < 3; ++i) {
    Fill fill;
    fill.aggressorId = OrderId{10 + i};
    fill.restingId = OrderId{20 + i};
    fill.side = Side::Bid;
    fill.price = Price{100};
    fill.quantity = Quantity{quantities[i]};
    fill.timestamp = Timestamp{1000000};
    fill.sequence = Sequence{i};
    stats.addFill(fill);
  }
  EXPECT_EQ(stats.totalQuantity().lots(), kMax);
  const auto avg = stats.averagePrice();
  ASSERT_TRUE(avg.has_value());
  EXPECT_EQ(avg->quantitySum, kMax);
}

TEST(PriceBoundary, QueueAheadAtLargeFittingSum) {
  OrderBook book;
  MatchingEngine engine(book);
  QueueTracker tracker;
  tracker.designate(OrderId{3});
  auto feed = [&](const Event& e) {
    ASSERT_TRUE(engine.onEvent(e));
    tracker.onEvent(e);
    tracker.onFills(engine.fills());
  };
  feed(newOrder(1, Side::Bid, 100, kMax / 2, 0));
  feed(newOrder(2, Side::Bid, 100, kMax / 2, 1));
  feed(newOrder(3, Side::Bid, 100, 1, 2));
  const auto pos = tracker.queuePosition(book, OrderId{3});
  ASSERT_TRUE(pos.has_value());
  EXPECT_EQ(pos->rank, 3U);
  EXPECT_EQ(pos->quantityAhead.lots(), kMax - 1);
}

TEST(PriceBoundary, LevelAggregateAtLargeFittingSum) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(engine.onEvent(newOrder(1, Side::Bid, 100, kMax / 2, 0)));
  EXPECT_TRUE(engine.onEvent(newOrder(2, Side::Bid, 100, kMax / 2, 1)));
  const MarketStateView view(book);
  const auto levels = view.levels(Side::Bid);
  ASSERT_EQ(levels.size(), 1U);
  EXPECT_EQ(levels[0].totalQuantity.lots(), kMax - 1);
  EXPECT_EQ(levels[0].orderCount, 2U);
}

TEST(PriceBoundary, PastBoundAggregatesAreDefinedNotUndefined) {
  // SPEC.md 2.4: aggregates past INT64_MAX total lots are outside the
  // supported domain and their values are not meaningful, but no
  // input may invoke undefined behavior. These scenarios overflowed
  // signed int64 under the pre-Day-20 accumulation; under UBSan this
  // test fails if any site regresses to signed accumulation. Values
  // past the bound are deliberately not asserted.
  ExecutionStatistics stats;
  for (std::uint64_t i = 0; i < 2; ++i) {
    Fill fill;
    fill.aggressorId = OrderId{10 + i};
    fill.restingId = OrderId{20 + i};
    fill.side = Side::Bid;
    fill.price = Price{100};
    fill.quantity = Quantity{kMax};
    fill.timestamp = Timestamp{1000000};
    fill.sequence = Sequence{i};
    stats.addFill(fill);
  }
  EXPECT_EQ(stats.fillCount(), 2U);
  EXPECT_TRUE(stats.averagePrice().has_value());

  OrderBook book;
  MatchingEngine engine(book);
  QueueTracker tracker;
  tracker.designate(OrderId{3});
  auto feed = [&](const Event& e) {
    ASSERT_TRUE(engine.onEvent(e));
    tracker.onEvent(e);
    tracker.onFills(engine.fills());
  };
  feed(newOrder(1, Side::Bid, 100, kMax, 0));
  feed(newOrder(2, Side::Bid, 100, kMax, 1));
  feed(newOrder(3, Side::Bid, 100, 1, 2));
  const auto pos = tracker.queuePosition(book, OrderId{3});
  ASSERT_TRUE(pos.has_value());
  EXPECT_EQ(pos->rank, 3U);

  const MarketStateView view(book);
  const auto levels = view.levels(Side::Bid);
  ASSERT_EQ(levels.size(), 1U);
  EXPECT_EQ(levels[0].orderCount, 3U);
}

} // namespace
} // namespace tickforge::test
