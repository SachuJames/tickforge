// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Unit tests for the MBO order book: insertion, FIFO price levels,
// cancellation, modification (SPEC.md 5.5), best bid/ask, and the core
// structural invariants.

#include "tickforge/book/order_book.hpp"
#include "tickforge/event/event.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <vector>

namespace {

using tickforge::Event;
using tickforge::EventType;
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

// Returns the resting order; fails the test if it is absent.
RestingOrder requireOrder(const OrderBook& book, OrderId id) {
  const auto found = book.find(id);
  EXPECT_TRUE(found.has_value()) << "expected order " << id.value() << " in the book";
  return found.value_or(RestingOrder{});
}

// Structural invariant: every order in the id lookup appears in exactly
// one price-level queue, and every queued id is in the lookup.
void expectLookupConsistency(const OrderBook& book, const std::vector<OrderId>& liveIds) {
  EXPECT_EQ(book.orderCount(), liveIds.size());
  std::size_t queued = 0;
  for (const Side side : {Side::Bid, Side::Ask}) {
    for (const OrderId id : liveIds) {
      const RestingOrder order = requireOrder(book, id);
      if (order.side == side) {
        const auto level = book.ordersAtLevel(side, order.price);
        EXPECT_NE(std::find(level.begin(), level.end(), id), level.end());
        ++queued;
      }
    }
  }
  EXPECT_EQ(queued, liveIds.size());
}

TEST(OrderBookTest, NewOrderInsertion) {
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{7}, Side::Bid, Price{100}, Quantity{10})));

  EXPECT_TRUE(book.contains(OrderId{7}));
  EXPECT_EQ(book.orderCount(), 1U);
  const RestingOrder order = requireOrder(book, OrderId{7});
  EXPECT_EQ(order.side, Side::Bid);
  EXPECT_EQ(order.price, Price{100});
  EXPECT_EQ(order.quantity, Quantity{10});
  EXPECT_EQ(order.arrivalSeq, Sequence{1});

  EXPECT_EQ(book.bestBid(), std::optional<Price>(Price{100}));
  EXPECT_EQ(book.bestAsk(), std::optional<Price>());
}

TEST(OrderBookTest, MultiplePricesBestBid) {
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{2}, OrderId{2}, Side::Bid, Price{101}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{3}, OrderId{3}, Side::Bid, Price{99}, Quantity{10})));

  EXPECT_EQ(book.bestBid(), std::optional<Price>(Price{101}));
  EXPECT_EQ(book.priceLevelCount(Side::Bid), 3U);
  expectLookupConsistency(book, {OrderId{1}, OrderId{2}, OrderId{3}});
}

TEST(OrderBookTest, AskOrderingBestAsk) {
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Ask, Price{102}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{2}, OrderId{2}, Side::Ask, Price{101}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{3}, OrderId{3}, Side::Ask, Price{103}, Quantity{10})));

  EXPECT_EQ(book.bestAsk(), std::optional<Price>(Price{101}));
  EXPECT_EQ(book.priceLevelCount(Side::Ask), 3U);
}

TEST(OrderBookTest, FifoOrderWithinLevel) {
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{2}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{3}, OrderId{3}, Side::Bid, Price{100}, Quantity{10})));

  const auto level = book.ordersAtLevel(Side::Bid, Price{100});
  ASSERT_EQ(level.size(), 3U);
  EXPECT_EQ(level[0], OrderId{1});
  EXPECT_EQ(level[1], OrderId{2});
  EXPECT_EQ(level[2], OrderId{3});
}

TEST(OrderBookTest, FifoUsesSequenceNotOrderId) {
  OrderBook book;
  // Ids arrive out of numeric order; the queue must follow arrivalSeq.
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{30}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{2}, OrderId{10}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{3}, OrderId{20}, Side::Bid, Price{100}, Quantity{10})));

  const auto level = book.ordersAtLevel(Side::Bid, Price{100});
  ASSERT_EQ(level.size(), 3U);
  EXPECT_EQ(level[0], OrderId{30});
  EXPECT_EQ(level[1], OrderId{10});
  EXPECT_EQ(level[2], OrderId{20});
}

TEST(OrderBookTest, CancelFromFront) {
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{2}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{3}, OrderId{3}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(book.onEvent(cancelEvent(Sequence{4}, OrderId{1})));

  const auto level = book.ordersAtLevel(Side::Bid, Price{100});
  ASSERT_EQ(level.size(), 2U);
  EXPECT_EQ(level[0], OrderId{2});
  EXPECT_EQ(level[1], OrderId{3});
  EXPECT_FALSE(book.contains(OrderId{1}));
  EXPECT_FALSE(book.find(OrderId{1}).has_value());
  expectLookupConsistency(book, {OrderId{2}, OrderId{3}});
}

TEST(OrderBookTest, CancelFromMiddle) {
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{2}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{3}, OrderId{3}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(book.onEvent(cancelEvent(Sequence{4}, OrderId{2})));

  const auto level = book.ordersAtLevel(Side::Bid, Price{100});
  ASSERT_EQ(level.size(), 2U);
  EXPECT_EQ(level[0], OrderId{1});
  EXPECT_EQ(level[1], OrderId{3});
  EXPECT_FALSE(book.contains(OrderId{2}));
}

TEST(OrderBookTest, CancelFromBack) {
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{2}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{3}, OrderId{3}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(book.onEvent(cancelEvent(Sequence{4}, OrderId{3})));

  const auto level = book.ordersAtLevel(Side::Bid, Price{100});
  ASSERT_EQ(level.size(), 2U);
  EXPECT_EQ(level[0], OrderId{1});
  EXPECT_EQ(level[1], OrderId{2});
}

TEST(OrderBookTest, CancelUnknownOrderIsRejected) {
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_FALSE(book.onEvent(cancelEvent(Sequence{2}, OrderId{99})));
  EXPECT_EQ(book.orderCount(), 1U);
  EXPECT_TRUE(book.contains(OrderId{1}));
}

TEST(OrderBookTest, DuplicateNewOrderIsRejected) {
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_FALSE(
      book.onEvent(newOrderEvent(Sequence{2}, OrderId{1}, Side::Ask, Price{200}, Quantity{5})));

  // The original order is untouched.
  const RestingOrder order = requireOrder(book, OrderId{1});
  EXPECT_EQ(order.side, Side::Bid);
  EXPECT_EQ(order.price, Price{100});
  EXPECT_EQ(book.orderCount(), 1U);
}

TEST(OrderBookTest, EmptyPriceLevelIsRemoved) {
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{2}, OrderId{2}, Side::Bid, Price{101}, Quantity{10})));
  EXPECT_TRUE(book.onEvent(cancelEvent(Sequence{3}, OrderId{1})));

  EXPECT_EQ(book.priceLevelCount(Side::Bid), 1U);
  EXPECT_TRUE(book.ordersAtLevel(Side::Bid, Price{100}).empty());
  EXPECT_EQ(book.bestBid(), std::optional<Price>(Price{101}));
}

TEST(OrderBookTest, BestBidUpdatesAfterRemoval) {
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{2}, OrderId{2}, Side::Bid, Price{101}, Quantity{10})));
  EXPECT_TRUE(book.onEvent(cancelEvent(Sequence{3}, OrderId{2})));

  EXPECT_EQ(book.bestBid(), std::optional<Price>(Price{100}));
}

TEST(OrderBookTest, BestAskUpdatesAfterRemoval) {
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Ask, Price{101}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{2}, OrderId{2}, Side::Ask, Price{102}, Quantity{10})));
  EXPECT_TRUE(book.onEvent(cancelEvent(Sequence{3}, OrderId{1})));

  EXPECT_EQ(book.bestAsk(), std::optional<Price>(Price{102}));
}

TEST(OrderBookTest, EmptyBookHasNoBestPrices) {
  const OrderBook book;
  EXPECT_EQ(book.bestBid(), std::optional<Price>());
  EXPECT_EQ(book.bestAsk(), std::optional<Price>());
  EXPECT_EQ(book.orderCount(), 0U);
  EXPECT_FALSE(book.contains(OrderId{1}));
  EXPECT_FALSE(book.find(OrderId{1}).has_value());
}

TEST(OrderBookTest, ModifyQuantityDecreaseKeepsPriority) {
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Bid, Price{100}, Quantity{100})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{2}, OrderId{2}, Side::Bid, Price{100}, Quantity{100})));
  // Quantity decrease at the same price (SPEC.md 5.5): keep priority.
  EXPECT_TRUE(book.onEvent(modifyEvent(Sequence{3}, OrderId{1}, Price{0}, Quantity{50})));

  const auto level = book.ordersAtLevel(Side::Bid, Price{100});
  ASSERT_EQ(level.size(), 2U);
  EXPECT_EQ(level[0], OrderId{1});
  EXPECT_EQ(level[1], OrderId{2});
  const RestingOrder order = requireOrder(book, OrderId{1});
  EXPECT_EQ(order.quantity, Quantity{50});
  EXPECT_EQ(order.arrivalSeq, Sequence{1});
}

TEST(OrderBookTest, ModifyQuantityIncreaseLosesPriority) {
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Bid, Price{100}, Quantity{100})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{2}, OrderId{2}, Side::Bid, Price{100}, Quantity{100})));
  // Quantity increase (SPEC.md 5.5): cancel/replace, rejoins at the back.
  EXPECT_TRUE(book.onEvent(modifyEvent(Sequence{3}, OrderId{1}, Price{0}, Quantity{200})));

  const auto level = book.ordersAtLevel(Side::Bid, Price{100});
  ASSERT_EQ(level.size(), 2U);
  EXPECT_EQ(level[0], OrderId{2});
  EXPECT_EQ(level[1], OrderId{1});
  const RestingOrder order = requireOrder(book, OrderId{1});
  EXPECT_EQ(order.quantity, Quantity{200});
  EXPECT_EQ(order.arrivalSeq, Sequence{3});
}

TEST(OrderBookTest, ModifyPriceChangeMovesLevel) {
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{2}, OrderId{2}, Side::Bid, Price{101}, Quantity{10})));
  EXPECT_TRUE(book.onEvent(modifyEvent(Sequence{3}, OrderId{1}, Price{102}, Quantity{0})));

  EXPECT_TRUE(book.ordersAtLevel(Side::Bid, Price{100}).empty());
  const auto level = book.ordersAtLevel(Side::Bid, Price{102});
  ASSERT_EQ(level.size(), 1U);
  EXPECT_EQ(level[0], OrderId{1});
  EXPECT_EQ(book.bestBid(), std::optional<Price>(Price{102}));
  const RestingOrder order = requireOrder(book, OrderId{1});
  EXPECT_EQ(order.arrivalSeq, Sequence{3});
}

TEST(OrderBookTest, ModifyUnknownOrderIsRejected) {
  OrderBook book;
  EXPECT_FALSE(book.onEvent(modifyEvent(Sequence{1}, OrderId{99}, Price{100}, Quantity{10})));
  EXPECT_EQ(book.orderCount(), 0U);
}

TEST(OrderBookTest, CrossingOrderRestsWithoutMatching) {
  // Day 03 performs no matching: a crossing order rests in the book.
  // This documents the interim behavior until the matching engine lands.
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{2}, OrderId{2}, Side::Ask, Price{90}, Quantity{10})));

  EXPECT_TRUE(book.contains(OrderId{2}));
  EXPECT_EQ(book.bestBid(), std::optional<Price>(Price{100}));
  EXPECT_EQ(book.bestAsk(), std::optional<Price>(Price{90}));
}

TEST(OrderBookTest, OrderIdReuseAfterCancelIsAccepted) {
  // The book enforces live-uniqueness; session-wide id uniqueness is the
  // parser's job (SPEC.md 5.1). A cancelled id is no longer live.
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(book.onEvent(cancelEvent(Sequence{2}, OrderId{1})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{3}, OrderId{1}, Side::Ask, Price{200}, Quantity{5})));

  const RestingOrder order = requireOrder(book, OrderId{1});
  EXPECT_EQ(order.side, Side::Ask);
  EXPECT_EQ(order.price, Price{200});
  EXPECT_EQ(order.arrivalSeq, Sequence{3});
}

TEST(OrderBookTest, MixedOperationInvariants) {
  OrderBook book;
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{1}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{2}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{3}, OrderId{3}, Side::Ask, Price{101}, Quantity{10})));
  EXPECT_TRUE(
      book.onEvent(newOrderEvent(Sequence{4}, OrderId{4}, Side::Ask, Price{102}, Quantity{10})));
  EXPECT_TRUE(book.onEvent(cancelEvent(Sequence{5}, OrderId{2})));
  EXPECT_TRUE(book.onEvent(modifyEvent(Sequence{6}, OrderId{3}, Price{0}, Quantity{5})));
  EXPECT_TRUE(book.onEvent(modifyEvent(Sequence{7}, OrderId{4}, Price{103}, Quantity{0})));

  expectLookupConsistency(book, {OrderId{1}, OrderId{3}, OrderId{4}});
  EXPECT_EQ(book.priceLevelCount(Side::Bid), 1U);
  EXPECT_EQ(book.priceLevelCount(Side::Ask), 2U);
  EXPECT_EQ(book.bestBid(), std::optional<Price>(Price{100}));
  EXPECT_EQ(book.bestAsk(), std::optional<Price>(Price{101}));
  const auto bidLevel = book.ordersAtLevel(Side::Bid, Price{100});
  ASSERT_EQ(bidLevel.size(), 1U);
  EXPECT_EQ(bidLevel[0], OrderId{1});
}

} // namespace
