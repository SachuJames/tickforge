// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Property-style tests for the matching engine: execution conservation,
// price priority, FIFO priority, no-overfill, and determinism. These test
// general properties rather than single scenarios.

#include "tickforge/book/order_book.hpp"
#include "tickforge/event/event.hpp"
#include "tickforge/matching/fill.hpp"
#include "tickforge/matching/matching_engine.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace {

using tickforge::Event;
using tickforge::EventType;
using tickforge::Fill;
using tickforge::MatchingEngine;
using tickforge::OrderBook;
using tickforge::OrderId;
using tickforge::Price;
using tickforge::Quantity;
using tickforge::RestingOrder;
using tickforge::Sequence;
using tickforge::Side;
using tickforge::Timestamp;

Event newOrderEvent(
    Timestamp ts, Sequence seq, OrderId id, Side side, Price price, Quantity quantity) {
  Event event;
  event.timestamp = ts;
  event.sequence = seq;
  event.type = EventType::NewOrder;
  event.instrument = "AAPL";
  event.orderId = id;
  event.side = side;
  event.price = price;
  event.quantity = quantity;
  return event;
}

// Sum of fill quantities in a vector.
std::int64_t totalFilled(const std::vector<Fill>& fills) {
  std::int64_t total = 0;
  for (const Fill& fill : fills) {
    total += fill.quantity.lots();
  }
  return total;
}

TEST(MatchingPropertyTest, ExecutionConservationAggressor) {
  // Incoming consumed = sum of fills + residual resting quantity.
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(engine.onEvent(newOrderEvent(
      Timestamp{1000}, Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{10})));
  EXPECT_TRUE(engine.onEvent(newOrderEvent(
      Timestamp{1000}, Sequence{1}, OrderId{2}, Side::Ask, Price{101}, Quantity{20})));
  const std::int64_t incomingQty = 45;
  EXPECT_TRUE(engine.onEvent(newOrderEvent(
      Timestamp{1000}, Sequence{2}, OrderId{3}, Side::Bid, Price{102}, Quantity{incomingQty})));

  const std::int64_t filled = totalFilled(engine.fills());
  std::int64_t residual = 0;
  const auto found = book.find(OrderId{3});
  if (found.has_value()) {
    residual = found->quantity.lots();
  }
  EXPECT_EQ(filled + residual, incomingQty);
}

// Sums fills per resting order. The branching is inherent to the check.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void expectFillsPerRestingOrder(const std::vector<Fill>& fills,
                                OrderId firstId,
                                OrderId secondId,
                                std::int64_t expectedFirst,
                                std::int64_t expectedSecond) {
  std::int64_t filled_against_first = 0;
  std::int64_t filled_against_second = 0;
  for (const Fill& fill : fills) {
    if (fill.restingId == firstId) {
      filled_against_first += fill.quantity.lots();
    } else if (fill.restingId == secondId) {
      filled_against_second += fill.quantity.lots();
    }
  }
  EXPECT_EQ(filled_against_first, expectedFirst);
  EXPECT_EQ(filled_against_second, expectedSecond);
}

TEST(MatchingPropertyTest, ExecutionConservationResting) {
  // For each resting order: original = filled + remaining.
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(engine.onEvent(newOrderEvent(
      Timestamp{1000}, Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{10})));
  EXPECT_TRUE(engine.onEvent(newOrderEvent(
      Timestamp{1000}, Sequence{1}, OrderId{2}, Side::Ask, Price{100}, Quantity{20})));
  EXPECT_TRUE(engine.onEvent(newOrderEvent(
      Timestamp{1000}, Sequence{2}, OrderId{3}, Side::Bid, Price{100}, Quantity{25})));

  // Order 1: 10 filled, 0 remaining. Order 2: 15 filled, 5 remaining.
  expectFillsPerRestingOrder(engine.fills(), OrderId{1}, OrderId{2}, 10, 15);
  const tickforge::RestingOrder order2 = book.find(OrderId{2}).value_or(tickforge::RestingOrder{});
  std::int64_t filled2 = 0;
  for (const Fill& fill : engine.fills()) {
    if (fill.restingId == OrderId{2}) {
      filled2 += fill.quantity.lots();
    }
  }
  EXPECT_EQ(filled2 + order2.quantity.lots(), 20);
  EXPECT_FALSE(book.contains(OrderId{1}));
}

TEST(MatchingPropertyTest, PricePriorityBidConsumesBestAskFirst) {
  // No fill at a worse ask price while a better one has liquidity.
  OrderBook book;
  MatchingEngine engine(book);
  // Add the worse price first to prove priority is by price, not insertion.
  EXPECT_TRUE(engine.onEvent(newOrderEvent(
      Timestamp{1000}, Sequence{0}, OrderId{1}, Side::Ask, Price{102}, Quantity{10})));
  EXPECT_TRUE(engine.onEvent(newOrderEvent(
      Timestamp{1000}, Sequence{1}, OrderId{2}, Side::Ask, Price{100}, Quantity{10})));
  EXPECT_TRUE(engine.onEvent(
      newOrderEvent(Timestamp{1000}, Sequence{2}, OrderId{3}, Side::Bid, Price{102}, Quantity{5})));

  ASSERT_EQ(engine.fills().size(), 1U);
  EXPECT_EQ(engine.fills()[0].price, Price{100});
  EXPECT_EQ(engine.fills()[0].restingId, OrderId{2});
}

TEST(MatchingPropertyTest, PricePriorityAskConsumesBestBidFirst) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(engine.onEvent(
      newOrderEvent(Timestamp{1000}, Sequence{0}, OrderId{1}, Side::Bid, Price{98}, Quantity{10})));
  EXPECT_TRUE(engine.onEvent(newOrderEvent(
      Timestamp{1000}, Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(engine.onEvent(
      newOrderEvent(Timestamp{1000}, Sequence{2}, OrderId{3}, Side::Ask, Price{98}, Quantity{5})));

  ASSERT_EQ(engine.fills().size(), 1U);
  EXPECT_EQ(engine.fills()[0].price, Price{100});
  EXPECT_EQ(engine.fills()[0].restingId, OrderId{2});
}

TEST(MatchingPropertyTest, FifoPriorityWithinPrice) {
  // At the same price, the smaller arrivalSeq matches first, even when a
  // later order has a smaller id.
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(engine.onEvent(newOrderEvent(
      Timestamp{1000}, Sequence{0}, OrderId{50}, Side::Ask, Price{100}, Quantity{10})));
  EXPECT_TRUE(engine.onEvent(newOrderEvent(
      Timestamp{1000}, Sequence{1}, OrderId{10}, Side::Ask, Price{100}, Quantity{10})));
  EXPECT_TRUE(engine.onEvent(newOrderEvent(
      Timestamp{1000}, Sequence{2}, OrderId{99}, Side::Bid, Price{100}, Quantity{10})));

  ASSERT_EQ(engine.fills().size(), 1U);
  EXPECT_EQ(engine.fills()[0].restingId, OrderId{50});
}

// Verifies fill quantities are positive and bounded by maxQty. The loop
// plus bounds checks exceed the complexity threshold.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void expectFillsBounded(const std::vector<Fill>& fills, std::int64_t maxQty) {
  for (const Fill& fill : fills) {
    EXPECT_GT(fill.quantity.lots(), 0);
  }
  EXPECT_LE(totalFilled(fills), maxQty);
}

TEST(MatchingPropertyTest, NoOverfillEitherSide) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(engine.onEvent(
      newOrderEvent(Timestamp{1000}, Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{7})));
  EXPECT_TRUE(engine.onEvent(newOrderEvent(
      Timestamp{1000}, Sequence{1}, OrderId{2}, Side::Ask, Price{101}, Quantity{13})));
  EXPECT_TRUE(engine.onEvent(newOrderEvent(
      Timestamp{1000}, Sequence{2}, OrderId{3}, Side::Bid, Price{105}, Quantity{11})));

  // Aggressor had 11 and resting had 20; fills cannot exceed either.
  expectFillsBounded(engine.fills(), 11);
  EXPECT_LE(totalFilled(engine.fills()), 20);
}

// Verifies fills are positive and all surviving orders have positive
// quantity. The nested loops exceed the complexity threshold.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void expectPositiveQuantities(const OrderBook& book, const std::vector<Fill>& fills) {
  for (const Fill& fill : fills) {
    EXPECT_GT(fill.quantity.lots(), 0);
  }
  for (std::uint64_t i = 0; i < 5; ++i) {
    const auto found = book.find(OrderId{i + 1});
    if (found.has_value()) {
      EXPECT_GT(found->quantity.lots(), 0);
    }
  }
}

TEST(MatchingPropertyTest, NoNegativeQuantitiesAfterSweep) {
  OrderBook book;
  MatchingEngine engine(book);
  for (std::uint64_t i = 0; i < 5; ++i) {
    EXPECT_TRUE(engine.onEvent(newOrderEvent(Timestamp{1000},
                                             Sequence{i},
                                             OrderId{i + 1},
                                             Side::Ask,
                                             Price{100 + static_cast<std::int64_t>(i)},
                                             Quantity{10})));
  }
  EXPECT_TRUE(engine.onEvent(newOrderEvent(
      Timestamp{1000}, Sequence{5}, OrderId{99}, Side::Bid, Price{110}, Quantity{33})));

  expectPositiveQuantities(book, engine.fills());
}

// Runs a fixed scenario and returns the fills. Used twice to prove
// determinism.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
std::vector<Fill> runDeterminismScenario() {
  OrderBook book;
  MatchingEngine engine(book);
  engine.onEvent(
      newOrderEvent(Timestamp{1000}, Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{10}));
  engine.onEvent(
      newOrderEvent(Timestamp{1000}, Sequence{1}, OrderId{2}, Side::Ask, Price{100}, Quantity{10}));
  engine.onEvent(
      newOrderEvent(Timestamp{1000}, Sequence{2}, OrderId{3}, Side::Ask, Price{101}, Quantity{10}));
  engine.onEvent(
      newOrderEvent(Timestamp{1000}, Sequence{3}, OrderId{4}, Side::Bid, Price{101}, Quantity{25}));
  return engine.fills();
}

// Compares two fill vectors field by field. The loop exceeds the
// complexity threshold.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void expectFillsEqual(const std::vector<Fill>& first, const std::vector<Fill>& second) {
  ASSERT_EQ(first.size(), second.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(first[i].aggressorId, second[i].aggressorId);
    EXPECT_EQ(first[i].restingId, second[i].restingId);
    EXPECT_EQ(first[i].price, second[i].price);
    EXPECT_EQ(first[i].quantity, second[i].quantity);
    EXPECT_EQ(first[i].sequence, second[i].sequence);
  }
}

TEST(MatchingPropertyTest, DeterministicFillOrder) {
  // The same event stream processed twice yields identical fill sequences.
  const std::vector<Fill> first = runDeterminismScenario();
  const std::vector<Fill> second = runDeterminismScenario();
  expectFillsEqual(first, second);
}

TEST(MatchingPropertyTest, FillsCarryAggressorEventIdentity) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(engine.onEvent(newOrderEvent(
      Timestamp{5000}, Sequence{42}, OrderId{1}, Side::Ask, Price{100}, Quantity{10})));
  EXPECT_TRUE(engine.onEvent(newOrderEvent(
      Timestamp{6000}, Sequence{43}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));

  ASSERT_EQ(engine.fills().size(), 1U);
  EXPECT_EQ(engine.fills()[0].timestamp, Timestamp{6000});
  EXPECT_EQ(engine.fills()[0].sequence, Sequence{43});
}

} // namespace
