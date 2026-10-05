// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// End-to-end analytics integration: event stream -> replay -> matching
// engine -> book, with the QueueTracker and ExecutionStatistics fed as
// analytical observers. Proves the full pipeline is deterministic and
// that analytics never perturb matching.

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "tickforge/analytics/execution_statistics.hpp"
#include "tickforge/analytics/queue_tracker.hpp"
#include "tickforge/book/order_book.hpp"
#include "tickforge/event/event.hpp"
#include "tickforge/matching/fill.hpp"
#include "tickforge/matching/matching_engine.hpp"
#include "tickforge/replay/replay.hpp"

namespace {

using tickforge::Event;
using tickforge::EventType;
using tickforge::ExecutionStatistics;
using tickforge::Fill;
using tickforge::MatchingEngine;
using tickforge::OrderBook;
using tickforge::OrderId;
using tickforge::Price;
using tickforge::Quantity;
using tickforge::QueueTracker;
using tickforge::Sequence;
using tickforge::Side;
using tickforge::Timestamp;

Event newOrderEvent(Sequence seq, OrderId id, Side side, Price price, Quantity quantity) {
  Event event;
  event.timestamp = Timestamp{1000};
  event.sequence = seq;
  event.type = EventType::NewOrder;
  event.instrument = "AAPL";
  event.orderId = id;
  event.side = side;
  event.price = price;
  event.quantity = quantity;
  return event;
}

// Full analytical pipeline for one stream.
struct AnalyticalRun {
  OrderBook book;
  MatchingEngine engine{book};
  QueueTracker tracker;
  ExecutionStatistics stats;

  bool feed(const Event& event) {
    const bool ok = engine.onEvent(event);
    tracker.onEvent(event);
    tracker.onFills(engine.fills());
    stats.addFills(engine.fills());
    return ok;
  }
};

TEST(AnalyticsIntegrationTest, FullPipeline) {
  AnalyticalRun run;
  run.tracker.designate(OrderId{2});

  EXPECT_TRUE(
      run.feed(newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      run.feed(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(run.feed(newOrderEvent(Sequence{2}, OrderId{3}, Side::Bid, Price{99}, Quantity{10})));
  EXPECT_TRUE(run.feed(newOrderEvent(Sequence{3}, OrderId{4}, Side::Ask, Price{99}, Quantity{5})));

  // Statistics: two fills (10 @ 100, 5 @ 99).
  EXPECT_EQ(run.stats.fillCount(), 2U);
  EXPECT_EQ(run.stats.totalQuantity(), Quantity{15});
  EXPECT_EQ(run.stats.buyQuantity(), Quantity{10});
  EXPECT_EQ(run.stats.sellQuantity(), Quantity{5});
  EXPECT_EQ(run.stats.minPrice(), std::optional<Price>(Price{99}));
  EXPECT_EQ(run.stats.maxPrice(), std::optional<Price>(Price{100}));
  const auto avg = run.stats.averagePrice().value_or(ExecutionStatistics::WeightedAveragePrice{});
  EXPECT_EQ(avg.priceQuantitySumHi, 0ULL);
  EXPECT_EQ(avg.priceQuantitySumLo, static_cast<std::uint64_t>(100 * 10 + 99 * 5));
  EXPECT_EQ(avg.quantitySum, 15);

  // Queue tracking: order 2 was fully filled as aggressor.
  EXPECT_EQ(run.tracker.lifecycle(run.book, OrderId{2}), QueueTracker::Lifecycle::Filled);
  // Order 3 rests at 99 with nothing ahead.
  run.tracker.designate(OrderId{3});
  const auto pos =
      run.tracker.queuePosition(run.book, OrderId{3}).value_or(QueueTracker::QueuePosition{});
  EXPECT_EQ(pos.rank, 1U);
  EXPECT_EQ(pos.quantityAhead, Quantity{0});
}

// Helper: verify two fill vectors match in quantity and price.
void expectFillsEqual(const std::vector<Fill>& actual, const std::vector<Fill>& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(actual[i].quantity, expected[i].quantity);
    EXPECT_EQ(actual[i].price, expected[i].price);
  }
}

TEST(AnalyticsIntegrationTest, AnalyticsDoNotAffectMatching) {
  // Run the same stream with and without analytics attached; the book
  // state and fills must be identical.
  const std::vector<Event> events = {
      newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{10}),
      newOrderEvent(Sequence{1}, OrderId{2}, Side::Ask, Price{101}, Quantity{20}),
      newOrderEvent(Sequence{2}, OrderId{3}, Side::Bid, Price{101}, Quantity{25}),
  };

  OrderBook plain_book;
  MatchingEngine plain_engine(plain_book);
  for (const Event& event : events) {
    plain_engine.onEvent(event);
  }
  const std::vector<Fill> plainFills = plain_engine.fills();

  AnalyticalRun run;
  for (const Event& event : events) {
    run.feed(event);
  }

  expectFillsEqual(run.engine.fills(), plainFills);
  EXPECT_EQ(run.book.orderCount(), plain_book.orderCount());
  EXPECT_EQ(run.book.bestBid(), plain_book.bestBid());
  EXPECT_EQ(run.book.bestAsk(), plain_book.bestAsk());
}

// Helper: run the full analytical pipeline over events.
AnalyticalRun runAnalytical(const std::vector<Event>& events) {
  AnalyticalRun detailed;
  detailed.tracker.designate(OrderId{3});
  detailed.tracker.designate(OrderId{4});
  for (const Event& event : events) {
    EXPECT_TRUE(detailed.feed(event));
  }
  return detailed;
}

// Helper: verify two analytical runs produced identical statistics.
void expectStatsEqual(const AnalyticalRun& first, const AnalyticalRun& second) {
  EXPECT_EQ(first.stats.fillCount(), second.stats.fillCount());
  EXPECT_EQ(first.stats.totalQuantity(), second.stats.totalQuantity());
  const auto avgFirst =
      first.stats.averagePrice().value_or(ExecutionStatistics::WeightedAveragePrice{});
  const auto avgSecond =
      second.stats.averagePrice().value_or(ExecutionStatistics::WeightedAveragePrice{});
  EXPECT_EQ(avgFirst.priceQuantitySumHi, avgSecond.priceQuantitySumHi);
  EXPECT_EQ(avgFirst.priceQuantitySumLo, avgSecond.priceQuantitySumLo);
}

// Helper: verify two analytical runs produced identical queue positions.
void expectPositionsEqual(const AnalyticalRun& first, const AnalyticalRun& second, OrderId id) {
  const auto posFirst =
      first.tracker.queuePosition(first.book, id).value_or(QueueTracker::QueuePosition{});
  const auto posSecond =
      second.tracker.queuePosition(second.book, id).value_or(QueueTracker::QueuePosition{});
  EXPECT_EQ(posFirst.rank, posSecond.rank);
  EXPECT_EQ(posFirst.quantityAhead, posSecond.quantityAhead);
}

TEST(AnalyticsIntegrationTest, ReplayTwiceIdenticalAnalytics) {
  const std::vector<Event> events = {
      newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{10}),
      newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10}),
      newOrderEvent(Sequence{2}, OrderId{3}, Side::Ask, Price{102}, Quantity{30}),
      newOrderEvent(Sequence{3}, OrderId{4}, Side::Bid, Price{101}, Quantity{20}),
  };

  const AnalyticalRun first = runAnalytical(events);
  const AnalyticalRun second = runAnalytical(events);

  expectStatsEqual(first, second);
  expectPositionsEqual(first, second, OrderId{3});
}

} // namespace
