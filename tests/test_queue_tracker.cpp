// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// QueueTracker tests: position and quantity-ahead computation, queue
// advancement on cancel/fill, lifecycle states, modify semantics, and
// determinism. The tracker observes events and fills; positions are
// recomputed from the live book.

#include "tickforge/analytics/queue_tracker.hpp"
#include "tickforge/book/order_book.hpp"
#include "tickforge/event/event.hpp"
#include "tickforge/matching/fill.hpp"
#include "tickforge/matching/matching_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

namespace {

using tickforge::Event;
using tickforge::EventType;
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

// Test fixture: engine + tracker wired together. Each onEvent feeds the
// tracker the event and the resulting fills, mirroring the intended
// driver flow.
struct TrackedEngine {
  OrderBook book;
  MatchingEngine engine{book};
  QueueTracker tracker;

  bool onEvent(const Event& event) {
    const bool ok = engine.onEvent(event);
    tracker.onEvent(event);
    tracker.onFills(engine.fills());
    return ok;
  }

  void designate(OrderId id) {
    tracker.designate(id);
  }
};

TEST(QueueTrackerTest, FirstOrderHasNoQuantityAhead) {
  TrackedEngine te;
  te.designate(OrderId{1});
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));

  const auto pos =
      te.tracker.queuePosition(te.book, OrderId{1}).value_or(QueueTracker::QueuePosition{});
  EXPECT_EQ(pos.rank, 1U);
  EXPECT_EQ(pos.quantityAhead, Quantity{0});
  EXPECT_EQ(pos.quantityRemaining, Quantity{10});
  EXPECT_EQ(pos.price, Price{100});
  EXPECT_EQ(pos.side, Side::Bid);
  EXPECT_EQ(te.tracker.lifecycle(te.book, OrderId{1}), QueueTracker::Lifecycle::Resting);
}

TEST(QueueTrackerTest, SecondOrderSeesFirstQuantityAhead) {
  TrackedEngine te;
  te.designate(OrderId{2});
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{30})));
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));

  const auto pos =
      te.tracker.queuePosition(te.book, OrderId{2}).value_or(QueueTracker::QueuePosition{});
  EXPECT_EQ(pos.rank, 2U);
  EXPECT_EQ(pos.quantityAhead, Quantity{30});
}

TEST(QueueTrackerTest, ThirdOrderAccumulatesQuantityAhead) {
  TrackedEngine te;
  te.designate(OrderId{3});
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{30})));
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{20})));
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{2}, OrderId{3}, Side::Bid, Price{100}, Quantity{10})));

  const auto pos =
      te.tracker.queuePosition(te.book, OrderId{3}).value_or(QueueTracker::QueuePosition{});
  EXPECT_EQ(pos.rank, 3U);
  EXPECT_EQ(pos.quantityAhead, Quantity{50});
}

TEST(QueueTrackerTest, DifferentPriceLevelsDoNotInterfere) {
  TrackedEngine te;
  te.designate(OrderId{2});
  // Better-priced order at 101 does not count as "ahead" at level 100.
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{101}, Quantity{999})));
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));

  const auto pos =
      te.tracker.queuePosition(te.book, OrderId{2}).value_or(QueueTracker::QueuePosition{});
  EXPECT_EQ(pos.rank, 1U);
  EXPECT_EQ(pos.quantityAhead, Quantity{0});
  EXPECT_EQ(pos.price, Price{100});
}

TEST(QueueTrackerTest, OrdersBehindDoNotMoveDesignatedOrder) {
  TrackedEngine te;
  te.designate(OrderId{1});
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{20})));
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{2}, OrderId{3}, Side::Bid, Price{100}, Quantity{30})));

  const auto pos =
      te.tracker.queuePosition(te.book, OrderId{1}).value_or(QueueTracker::QueuePosition{});
  EXPECT_EQ(pos.rank, 1U);
  EXPECT_EQ(pos.quantityAhead, Quantity{0});
}

TEST(QueueTrackerTest, CancellationAheadAdvancesPosition) {
  TrackedEngine te;
  te.designate(OrderId{3});
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{30})));
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{20})));
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{2}, OrderId{3}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(te.onEvent(cancelEvent(Sequence{3}, OrderId{1})));

  const auto pos =
      te.tracker.queuePosition(te.book, OrderId{3}).value_or(QueueTracker::QueuePosition{});
  EXPECT_EQ(pos.rank, 2U);
  EXPECT_EQ(pos.quantityAhead, Quantity{20});
}

TEST(QueueTrackerTest, PartialFillAheadReducesQuantityAhead) {
  TrackedEngine te;
  te.designate(OrderId{2});
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{30})));
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));
  // Aggressor takes 10 from order 1; 20 remains ahead of order 2.
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{2}, OrderId{3}, Side::Ask, Price{100}, Quantity{10})));

  const auto pos =
      te.tracker.queuePosition(te.book, OrderId{2}).value_or(QueueTracker::QueuePosition{});
  EXPECT_EQ(pos.rank, 2U);
  EXPECT_EQ(pos.quantityAhead, Quantity{20});
}

TEST(QueueTrackerTest, FullFillAheadRemovesQuantityAhead) {
  TrackedEngine te;
  te.designate(OrderId{2});
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{30})));
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));
  // Aggressor fully consumes order 1.
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{2}, OrderId{3}, Side::Ask, Price{100}, Quantity{30})));

  const auto pos =
      te.tracker.queuePosition(te.book, OrderId{2}).value_or(QueueTracker::QueuePosition{});
  EXPECT_EQ(pos.rank, 1U);
  EXPECT_EQ(pos.quantityAhead, Quantity{0});
}

TEST(QueueTrackerTest, PartialFillOfDesignatedPreservesPosition) {
  TrackedEngine te;
  te.designate(OrderId{1});
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{30})));
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));
  // Partial fill of the designated order itself.
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{2}, OrderId{3}, Side::Ask, Price{100}, Quantity{10})));

  const auto pos =
      te.tracker.queuePosition(te.book, OrderId{1}).value_or(QueueTracker::QueuePosition{});
  EXPECT_EQ(pos.rank, 1U);
  EXPECT_EQ(pos.quantityAhead, Quantity{0});
  EXPECT_EQ(pos.quantityRemaining, Quantity{20});
  EXPECT_EQ(te.tracker.lifecycle(te.book, OrderId{1}), QueueTracker::Lifecycle::Resting);
}

TEST(QueueTrackerTest, FullFillRemovesFromTracking) {
  TrackedEngine te;
  te.designate(OrderId{1});
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Ask, Price{100}, Quantity{10})));

  EXPECT_FALSE(te.tracker.queuePosition(te.book, OrderId{1}).has_value());
  EXPECT_EQ(te.tracker.lifecycle(te.book, OrderId{1}), QueueTracker::Lifecycle::Filled);
}

TEST(QueueTrackerTest, CancelDesignatedOrder) {
  TrackedEngine te;
  te.designate(OrderId{1});
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(te.onEvent(cancelEvent(Sequence{1}, OrderId{1})));

  EXPECT_FALSE(te.tracker.queuePosition(te.book, OrderId{1}).has_value());
  EXPECT_EQ(te.tracker.lifecycle(te.book, OrderId{1}), QueueTracker::Lifecycle::Cancelled);
}

TEST(QueueTrackerTest, PriceChangingModifyResetsPosition) {
  TrackedEngine te;
  te.designate(OrderId{1});
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{101}, Quantity{10})));
  // Price change: cancel/replace. Order 1 moves to 101, behind order 2.
  EXPECT_TRUE(te.onEvent(modifyEvent(Sequence{2}, OrderId{1}, Price{101}, Quantity{0})));

  const auto pos =
      te.tracker.queuePosition(te.book, OrderId{1}).value_or(QueueTracker::QueuePosition{});
  EXPECT_EQ(pos.price, Price{101});
  EXPECT_EQ(pos.rank, 2U);
  EXPECT_EQ(pos.quantityAhead, Quantity{10});
}

TEST(QueueTrackerTest, SamePriceQuantityDecreaseKeepsPosition) {
  TrackedEngine te;
  te.designate(OrderId{1});
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{30})));
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(te.onEvent(modifyEvent(Sequence{2}, OrderId{1}, Price{0}, Quantity{20})));

  const auto pos =
      te.tracker.queuePosition(te.book, OrderId{1}).value_or(QueueTracker::QueuePosition{});
  EXPECT_EQ(pos.rank, 1U);
  EXPECT_EQ(pos.quantityRemaining, Quantity{20});
}

TEST(QueueTrackerTest, ReusedIdAfterCancelStartsFresh) {
  TrackedEngine te;
  te.designate(OrderId{1});
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));
  EXPECT_TRUE(te.onEvent(cancelEvent(Sequence{1}, OrderId{1})));
  EXPECT_EQ(te.tracker.lifecycle(te.book, OrderId{1}), QueueTracker::Lifecycle::Cancelled);
  // Id reused: tracker treats it as a new order.
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{2}, OrderId{1}, Side::Ask, Price{200}, Quantity{5})));

  EXPECT_EQ(te.tracker.lifecycle(te.book, OrderId{1}), QueueTracker::Lifecycle::Resting);
  const auto pos =
      te.tracker.queuePosition(te.book, OrderId{1}).value_or(QueueTracker::QueuePosition{});
  EXPECT_EQ(pos.side, Side::Ask);
  EXPECT_EQ(pos.price, Price{200});
}

TEST(QueueTrackerTest, UntrackedIdHasNoPosition) {
  TrackedEngine te;
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{10})));

  EXPECT_FALSE(te.tracker.isTracked(OrderId{1}));
  EXPECT_FALSE(te.tracker.queuePosition(te.book, OrderId{1}).has_value());
  EXPECT_EQ(te.tracker.lifecycle(te.book, OrderId{1}), QueueTracker::Lifecycle::Unknown);
}

TEST(QueueTrackerTest, ReleaseStopsTracking) {
  TrackedEngine te;
  te.designate(OrderId{1});
  EXPECT_TRUE(te.tracker.isTracked(OrderId{1}));
  te.tracker.release(OrderId{1});
  EXPECT_FALSE(te.tracker.isTracked(OrderId{1}));
}

TEST(QueueTrackerTest, DeterministicAcrossRuns) {
  const auto run = []() {
    TrackedEngine te;
    te.designate(OrderId{2});
    te.onEvent(newOrderEvent(Sequence{0}, OrderId{1}, Side::Bid, Price{100}, Quantity{30}));
    te.onEvent(newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10}));
    te.onEvent(newOrderEvent(Sequence{2}, OrderId{3}, Side::Ask, Price{100}, Quantity{15}));
    return te.tracker.queuePosition(te.book, OrderId{2}).value_or(QueueTracker::QueuePosition{});
  };
  const auto first = run();
  const auto second = run();
  EXPECT_EQ(first.rank, second.rank);
  EXPECT_EQ(first.quantityAhead, second.quantityAhead);
  EXPECT_EQ(first.quantityRemaining, second.quantityRemaining);
}

TEST(QueueTrackerTest, FifoNotIdOrder) {
  TrackedEngine te;
  te.designate(OrderId{10});
  // Larger id arrives first; smaller id arrives second.
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{0}, OrderId{99}, Side::Bid, Price{100}, Quantity{30})));
  EXPECT_TRUE(
      te.onEvent(newOrderEvent(Sequence{1}, OrderId{10}, Side::Bid, Price{100}, Quantity{10})));

  const auto pos =
      te.tracker.queuePosition(te.book, OrderId{10}).value_or(QueueTracker::QueuePosition{});
  EXPECT_EQ(pos.rank, 2U);
  EXPECT_EQ(pos.quantityAhead, Quantity{30});
}

} // namespace
