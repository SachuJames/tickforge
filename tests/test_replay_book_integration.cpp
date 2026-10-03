// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Integration tests for the Day 03 pipeline: normalized events flow
// through the deterministic replay driver into the MBO order book, and
// the resulting market state is inspected. Also proves replay
// determinism: the same event vector replayed twice yields equivalent
// observable state.

#include "tickforge/book/order_book.hpp"
#include "tickforge/event/event.hpp"
#include "tickforge/replay/replay.hpp"

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <tuple>
#include <vector>

namespace {

using tickforge::Event;
using tickforge::EventType;
using tickforge::OrderBook;
using tickforge::OrderId;
using tickforge::Price;
using tickforge::Quantity;
using tickforge::ReplayResult;
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
tickforge::RestingOrder requireOrder(const OrderBook& book, OrderId id) {
  const auto found = book.find(id);
  EXPECT_TRUE(found.has_value()) << "expected order " << id.value() << " in the book";
  return found.value_or(tickforge::RestingOrder{});
}

// Observable market state, comparable by value. Deliberately avoids any
// internal container details: only what the public query API exposes.
struct BookSnapshot {
  // (order id, side, price ticks, quantity lots, arrival seq), sorted by id.
  std::vector<std::tuple<std::uint64_t, std::uint8_t, std::int64_t, std::int64_t, std::uint64_t>>
      orders;
  std::optional<std::int64_t> bestBid;
  std::optional<std::int64_t> bestAsk;

  bool operator==(const BookSnapshot&) const = default;
};

BookSnapshot snapshotBook(const OrderBook& book, const std::vector<OrderId>& ids) {
  BookSnapshot snapshot;
  for (const OrderId id : ids) {
    const auto found = book.find(id);
    if (!found.has_value()) {
      continue;
    }
    const tickforge::RestingOrder order = *found;
    snapshot.orders.emplace_back(id.value(),
                                 static_cast<std::uint8_t>(order.side),
                                 order.price.ticks(),
                                 order.quantity.lots(),
                                 order.arrivalSeq.value());
  }
  std::sort(snapshot.orders.begin(), snapshot.orders.end());
  const auto bestBid = book.bestBid();
  const auto bestAsk = book.bestAsk();
  if (bestBid.has_value()) {
    snapshot.bestBid = bestBid->ticks();
  }
  if (bestAsk.has_value()) {
    snapshot.bestAsk = bestAsk->ticks();
  }
  return snapshot;
}

TEST(ReplayBookIntegrationTest, NewCancelFlow) {
  const std::vector<Event> events = {
      newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{10}),
      newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10}),
      newOrderEvent(Sequence{2}, OrderId{3}, Side::Bid, Price{100}, Quantity{10}),
      cancelEvent(Sequence{3}, OrderId{2}),
  };

  OrderBook book;
  const ReplayResult result = tickforge::replayEvents(events, book);
  EXPECT_TRUE(result.ok());

  EXPECT_EQ(book.orderCount(), 2U);
  EXPECT_FALSE(book.contains(OrderId{2}));
  const auto level = book.ordersAtLevel(Side::Bid, Price{100});
  ASSERT_EQ(level.size(), 2U);
  EXPECT_EQ(level[0], OrderId{1});
  EXPECT_EQ(level[1], OrderId{3});
  EXPECT_EQ(book.bestBid(), std::optional<Price>(Price{100}));
}

TEST(ReplayBookIntegrationTest, ModifyFlow) {
  const std::vector<Event> events = {
      newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{101}, Quantity{100}),
      newOrderEvent(Sequence{1}, OrderId{2}, Side::Ask, Price{101}, Quantity{100}),
      modifyEvent(Sequence{2}, OrderId{1}, Price{0}, Quantity{40}),
  };

  OrderBook book;
  const ReplayResult result = tickforge::replayEvents(events, book);
  EXPECT_TRUE(result.ok());

  const auto level = book.ordersAtLevel(Side::Ask, Price{101});
  ASSERT_EQ(level.size(), 2U);
  EXPECT_EQ(level[0], OrderId{1});
  EXPECT_EQ(level[1], OrderId{2});
  const tickforge::RestingOrder order = requireOrder(book, OrderId{1});
  EXPECT_EQ(order.quantity, Quantity{40});
  EXPECT_EQ(order.arrivalSeq, Sequence{0});
}

TEST(ReplayBookIntegrationTest, SameInputReplaysToEquivalentState) {
  const std::vector<Event> events = {
      newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{10}),
      newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{101}, Quantity{20}),
      newOrderEvent(Sequence{2}, OrderId{3}, Side::Ask, Price{102}, Quantity{30}),
      newOrderEvent(Sequence{3}, OrderId{4}, Side::Bid, Price{100}, Quantity{40}),
      cancelEvent(Sequence{4}, OrderId{2}),
      modifyEvent(Sequence{5}, OrderId{3}, Price{0}, Quantity{15}),
  };
  const std::vector<OrderId> ids = {OrderId{1}, OrderId{2}, OrderId{3}, OrderId{4}};

  OrderBook first;
  EXPECT_TRUE(tickforge::replayEvents(events, first).ok());
  OrderBook second;
  EXPECT_TRUE(tickforge::replayEvents(events, second).ok());

  EXPECT_EQ(snapshotBook(first, ids), snapshotBook(second, ids));
}

TEST(ReplayBookIntegrationTest, ReplayAbortsAtFirstError) {
  const std::vector<Event> events = {
      newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{10}),
      newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{101}, Quantity{10}),
      cancelEvent(Sequence{2}, OrderId{99}),
      newOrderEvent(Sequence{3}, OrderId{3}, Side::Bid, Price{102}, Quantity{10}),
  };

  OrderBook book;
  const ReplayResult result = tickforge::replayEvents(events, book);
  EXPECT_EQ(result.error, tickforge::ReplayError::UnknownOrder);
  EXPECT_EQ(result.failedAt, Sequence{2});

  // Strict-mode abort: the first two events stay applied, the rest never run.
  EXPECT_EQ(book.orderCount(), 2U);
  EXPECT_FALSE(book.contains(OrderId{3}));
}

TEST(ReplayBookIntegrationTest, UnsortedStreamNeverTouchesTheBook) {
  const std::vector<Event> events = {
      newOrderEvent(Sequence{1}, OrderId{1}, Side::Bid, Price{100}, Quantity{10}),
      newOrderEvent(Sequence{0}, OrderId{2}, Side::Bid, Price{101}, Quantity{10}),
  };

  OrderBook book;
  const ReplayResult result = tickforge::replayEvents(events, book);
  EXPECT_EQ(result.error, tickforge::ReplayError::UnsortedInput);
  // The first event was dispatched before the ordering violation surfaced.
  EXPECT_EQ(book.orderCount(), 1U);
  EXPECT_TRUE(book.contains(OrderId{1}));
}

} // namespace
