// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Matching engine tests: crossing detection, price-time priority, full
// and partial fills, multi-order and multi-level sweeps, price limits,
// residual handling, and modify interaction.

#include "tickforge/book/order_book.hpp"
#include "tickforge/event/event.hpp"
#include "tickforge/matching/fill.hpp"
#include "tickforge/matching/matching_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
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

Event cancelEvent(Sequence seq, OrderId id) {
  Event event;
  event.timestamp = Timestamp{1000};
  event.sequence = seq;
  event.type = EventType::CancelOrder;
  event.instrument = "AAPL";
  event.orderId = id;
  event.side = Side::Bid;
  return event;
}

Event modifyEvent(Sequence seq, OrderId id, Price price, Quantity quantity) {
  Event event;
  event.timestamp = Timestamp{1000};
  event.sequence = seq;
  event.type = EventType::ModifyOrder;
  event.instrument = "AAPL";
  event.orderId = id;
  event.side = Side::Bid;
  event.price = price;
  event.quantity = quantity;
  return event;
}

RestingOrder requireOrder(const OrderBook& book, OrderId id) {
  const auto found = book.find(id);
  EXPECT_TRUE(found.has_value()) << "expected order " << id.value() << " in the book";
  return found.value_or(RestingOrder{});
}

TEST(MatchingTest, EmptyBookNoExecution) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));

  EXPECT_TRUE(engine.fills().empty());
  EXPECT_TRUE(book.contains(OrderId{1}));
  EXPECT_EQ(book.bestBid(), std::optional<Price>(Price{100}));
}

TEST(MatchingTest, NonCrossingBidRests) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{101}, Quantity{10})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));

  EXPECT_TRUE(engine.fills().empty());
  EXPECT_TRUE(book.contains(OrderId{2}));
  EXPECT_EQ(book.bestBid(), std::optional<Price>(Price{100}));
  EXPECT_EQ(book.bestAsk(), std::optional<Price>(Price{101}));
  const auto level = book.ordersAtLevel(Side::Bid, Price{100});
  ASSERT_EQ(level.size(), 1U);
  EXPECT_EQ(level[0], OrderId{2});
}

TEST(MatchingTest, NonCrossingAskRests) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Ask, Price{101}, Quantity{10})));

  EXPECT_TRUE(engine.fills().empty());
  EXPECT_TRUE(book.contains(OrderId{2}));
}

TEST(MatchingTest, ExactPriceCrosses) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));

  ASSERT_EQ(engine.fills().size(), 1U);
  const Fill& fill = engine.fills()[0];
  EXPECT_EQ(fill.aggressorId, OrderId{2});
  EXPECT_EQ(fill.restingId, OrderId{1});
  EXPECT_EQ(fill.side, Side::Bid);
  EXPECT_EQ(fill.price, Price{100});
  EXPECT_EQ(fill.quantity, Quantity{10});
  EXPECT_EQ(fill.timestamp, Timestamp{1000});
  EXPECT_EQ(fill.sequence, Sequence{1});
  EXPECT_FALSE(book.contains(OrderId{1}));
  EXPECT_FALSE(book.contains(OrderId{2}));
}

TEST(MatchingTest, FullFillRemovesRestingOrder) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{50})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{101}, Quantity{50})));

  ASSERT_EQ(engine.fills().size(), 1U);
  EXPECT_EQ(engine.fills()[0].quantity, Quantity{50});
  // Execution at the resting order's price, not the aggressor's limit.
  EXPECT_EQ(engine.fills()[0].price, Price{100});
  EXPECT_FALSE(book.contains(OrderId{1}));
  EXPECT_FALSE(book.contains(OrderId{2}));
  EXPECT_EQ(book.orderCount(), 0U);
  EXPECT_EQ(book.priceLevelCount(Side::Ask), 0U);
}

TEST(MatchingTest, PartialFillOfRestingOrder) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{50})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{101}, Quantity{20})));

  ASSERT_EQ(engine.fills().size(), 1U);
  EXPECT_EQ(engine.fills()[0].quantity, Quantity{20});
  // Resting order survives with reduced quantity, same priority.
  const RestingOrder order = requireOrder(book, OrderId{1});
  EXPECT_EQ(order.quantity, Quantity{30});
  EXPECT_EQ(order.arrivalSeq, Sequence{0});
  EXPECT_FALSE(book.contains(OrderId{2}));
}

TEST(MatchingTest, PartialFillOfAggressorRestsResidual) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{20})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{101}, Quantity{50})));

  ASSERT_EQ(engine.fills().size(), 1U);
  EXPECT_EQ(engine.fills()[0].quantity, Quantity{20});
  EXPECT_FALSE(book.contains(OrderId{1}));
  // Residual 30 rests as a bid at the limit price.
  const RestingOrder residual = requireOrder(book, OrderId{2});
  EXPECT_EQ(residual.side, Side::Bid);
  EXPECT_EQ(residual.price, Price{101});
  EXPECT_EQ(residual.quantity, Quantity{30});
  EXPECT_EQ(residual.arrivalSeq, Sequence{1});
  EXPECT_EQ(book.bestBid(), std::optional<Price>(Price{101}));
}

TEST(MatchingTest, MultipleOrdersSamePriceFifo) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Ask, Price{100}, Quantity{20})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{2}, OrderId{3}, Side::Ask, Price{100}, Quantity{30})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{3}, OrderId{4}, Side::Bid, Price{100}, Quantity{25})));

  ASSERT_EQ(engine.fills().size(), 2U);
  EXPECT_EQ(engine.fills()[0].restingId, OrderId{1});
  EXPECT_EQ(engine.fills()[0].quantity, Quantity{10});
  EXPECT_EQ(engine.fills()[1].restingId, OrderId{2});
  EXPECT_EQ(engine.fills()[1].quantity, Quantity{15});

  EXPECT_FALSE(book.contains(OrderId{1}));
  EXPECT_FALSE(book.contains(OrderId{4}));
  const RestingOrder second = requireOrder(book, OrderId{2});
  EXPECT_EQ(second.quantity, Quantity{5});
  EXPECT_EQ(second.arrivalSeq, Sequence{1});
  const RestingOrder third = requireOrder(book, OrderId{3});
  EXPECT_EQ(third.quantity, Quantity{30});
  // FIFO preserved: order 2 (partially filled) still ahead of order 3.
  const auto level = book.ordersAtLevel(Side::Ask, Price{100});
  ASSERT_EQ(level.size(), 2U);
  EXPECT_EQ(level[0], OrderId{2});
  EXPECT_EQ(level[1], OrderId{3});
}

TEST(MatchingTest, MultiLevelSweep) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Ask, Price{101}, Quantity{20})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{2}, OrderId{3}, Side::Ask, Price{102}, Quantity{30})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{3}, OrderId{4}, Side::Bid, Price{102}, Quantity{45})));

  ASSERT_EQ(engine.fills().size(), 3U);
  EXPECT_EQ(engine.fills()[0].price, Price{100});
  EXPECT_EQ(engine.fills()[0].quantity, Quantity{10});
  EXPECT_EQ(engine.fills()[1].price, Price{101});
  EXPECT_EQ(engine.fills()[1].quantity, Quantity{20});
  EXPECT_EQ(engine.fills()[2].price, Price{102});
  EXPECT_EQ(engine.fills()[2].quantity, Quantity{15});

  EXPECT_FALSE(book.contains(OrderId{1}));
  EXPECT_FALSE(book.contains(OrderId{2}));
  EXPECT_FALSE(book.contains(OrderId{4}));
  const RestingOrder remaining = requireOrder(book, OrderId{3});
  EXPECT_EQ(remaining.quantity, Quantity{15});
  EXPECT_EQ(book.priceLevelCount(Side::Ask), 1U);
}

TEST(MatchingTest, PriceLimitRespected) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Ask, Price{101}, Quantity{10})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{2}, OrderId{3}, Side::Ask, Price{102}, Quantity{10})));
  // Bid limited to 101 must not touch the 102 ask.
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{3}, OrderId{4}, Side::Bid, Price{101}, Quantity{25})));

  ASSERT_EQ(engine.fills().size(), 2U);
  EXPECT_EQ(engine.fills()[0].price, Price{100});
  EXPECT_EQ(engine.fills()[1].price, Price{101});
  EXPECT_TRUE(book.contains(OrderId{3}));
  // Residual 5 rests at the bid limit price.
  const RestingOrder residual = requireOrder(book, OrderId{4});
  EXPECT_EQ(residual.price, Price{101});
  EXPECT_EQ(residual.quantity, Quantity{5});
}

TEST(MatchingTest, EntireSideConsumed) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Ask, Price{101}, Quantity{10})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{2}, OrderId{3}, Side::Bid, Price{101}, Quantity{20})));

  EXPECT_EQ(engine.fills().size(), 2U);
  EXPECT_EQ(book.orderCount(), 0U);
  EXPECT_EQ(book.bestAsk(), std::optional<Price>());
  EXPECT_EQ(book.bestBid(), std::optional<Price>());
}

TEST(MatchingTest, AskAggressorMatchesBids) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{99}, Quantity{10})));
  // Ask at 99 crosses both bids; best bid (100) matches first.
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{2}, OrderId{3}, Side::Ask, Price{99}, Quantity{15})));

  ASSERT_EQ(engine.fills().size(), 2U);
  EXPECT_EQ(engine.fills()[0].restingId, OrderId{1});
  EXPECT_EQ(engine.fills()[0].price, Price{100});
  EXPECT_EQ(engine.fills()[0].quantity, Quantity{10});
  EXPECT_EQ(engine.fills()[1].restingId, OrderId{2});
  EXPECT_EQ(engine.fills()[1].price, Price{99});
  EXPECT_EQ(engine.fills()[1].quantity, Quantity{5});
  EXPECT_EQ(engine.fills()[0].side, Side::Ask);
}

TEST(MatchingTest, DuplicateNewOrderRejected) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{10})));
  EXPECT_FALSE(
      engine.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(engine.fills().empty());
  // Original resting ask untouched.
  const RestingOrder order = requireOrder(book, OrderId{1});
  EXPECT_EQ(order.side, Side::Ask);
  EXPECT_EQ(order.quantity, Quantity{10});
}

TEST(MatchingTest, CancelAfterPartialFill) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{50})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{20})));
  EXPECT_TRUE(engine.onEvent(cancelEvent(Sequence{2}, OrderId{1})));

  EXPECT_FALSE(book.contains(OrderId{1}));
  EXPECT_EQ(book.orderCount(), 0U);
}

TEST(MatchingTest, ModifyPriceChangeCrossesAndMatches) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{105}, Quantity{10})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));
  // Modify the bid up to a crossing price: cancel/replace matches as aggressor.
  EXPECT_TRUE(engine.onEvent(modifyEvent(Sequence{2}, OrderId{2}, Price{105}, Quantity{0})));

  ASSERT_EQ(engine.fills().size(), 1U);
  const Fill& fill = engine.fills()[0];
  EXPECT_EQ(fill.aggressorId, OrderId{2});
  EXPECT_EQ(fill.restingId, OrderId{1});
  EXPECT_EQ(fill.price, Price{105});
  EXPECT_EQ(fill.quantity, Quantity{10});
  EXPECT_EQ(fill.sequence, Sequence{2});
  EXPECT_FALSE(book.contains(OrderId{1}));
  EXPECT_FALSE(book.contains(OrderId{2}));
}

TEST(MatchingTest, ModifyPriceChangeWithoutCrossingRests) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{105}, Quantity{10})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));
  // Modify to a still-passive price: no fill, order moves level.
  EXPECT_TRUE(engine.onEvent(modifyEvent(Sequence{2}, OrderId{2}, Price{101}, Quantity{0})));

  EXPECT_TRUE(engine.fills().empty());
  const RestingOrder order = requireOrder(book, OrderId{2});
  EXPECT_EQ(order.price, Price{101});
  EXPECT_EQ(order.arrivalSeq, Sequence{2});
  EXPECT_EQ(book.bestBid(), std::optional<Price>(Price{101}));
}

TEST(MatchingTest, ModifyQuantityDecreaseKeepsPriorityNoMatch) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{100})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{100})));
  EXPECT_TRUE(engine.onEvent(modifyEvent(Sequence{2}, OrderId{1}, Price{0}, Quantity{50})));

  EXPECT_TRUE(engine.fills().empty());
  const auto level = book.ordersAtLevel(Side::Bid, Price{100});
  ASSERT_EQ(level.size(), 2U);
  EXPECT_EQ(level[0], OrderId{1});
}

TEST(MatchingTest, LargeQuantitiesNoNarrowing) {
  OrderBook book;
  MatchingEngine engine(book);
  const std::int64_t big = 9000000000000000000LL / 4; // well within int64
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{big})));
  EXPECT_TRUE(
      engine.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{big})));

  ASSERT_EQ(engine.fills().size(), 1U);
  EXPECT_EQ(engine.fills()[0].quantity, Quantity{big});
  EXPECT_EQ(book.orderCount(), 0U);
}

} // namespace
