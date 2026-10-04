// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Integration tests for the Day 04 pipeline: normalized events flow
// through the deterministic replay driver into the matching engine,
// producing fills and an updated order book. Also proves end-to-end
// determinism: replaying the same stream twice yields equivalent fills
// and equivalent book state.

#include "tickforge/book/order_book.hpp"
#include "tickforge/event/event.hpp"
#include "tickforge/matching/fill.hpp"
#include "tickforge/matching/matching_engine.hpp"
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
using tickforge::Fill;
using tickforge::MatchingEngine;
using tickforge::OrderBook;
using tickforge::OrderId;
using tickforge::Price;
using tickforge::Quantity;
using tickforge::ReplayError;
using tickforge::ReplayResult;
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

// A fill reduced to comparable values, in fill order.
using FillTuple = std::tuple<std::uint64_t,
                             std::uint64_t,
                             std::uint8_t,
                             std::int64_t,
                             std::int64_t,
                             std::int64_t,
                             std::uint64_t>;

std::vector<FillTuple> fillTuples(const std::vector<Fill>& fills) {
  std::vector<FillTuple> tuples;
  tuples.reserve(fills.size());
  for (const Fill& fill : fills) {
    tuples.emplace_back(fill.aggressorId.value(),
                        fill.restingId.value(),
                        static_cast<std::uint8_t>(fill.side),
                        fill.price.ticks(),
                        fill.quantity.lots(),
                        fill.timestamp.count(),
                        fill.sequence.value());
  }
  return tuples;
}

struct BookSnapshot {
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
    snapshot.orders.emplace_back(id.value(),
                                 static_cast<std::uint8_t>(found->side),
                                 found->price.ticks(),
                                 found->quantity.lots(),
                                 found->arrivalSeq.value());
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

struct RunResult {
  ReplayResult replay;
  std::vector<std::vector<FillTuple>> fillsPerEvent;
  BookSnapshot book;
};

RunResult runStream(const std::vector<Event>& events, const std::vector<OrderId>& ids) {
  OrderBook book;
  MatchingEngine engine(book);
  RunResult result;
  // Replay one event at a time so per-event fills can be captured.
  for (const Event& event : events) {
    const std::vector<Event> single{event};
    const ReplayResult r = tickforge::replayEvents(single, engine);
    if (!r.ok()) {
      result.replay = r;
      result.book = snapshotBook(book, ids);
      return result;
    }
    result.fillsPerEvent.push_back(fillTuples(engine.fills()));
  }
  result.book = snapshotBook(book, ids);
  return result;
}

TEST(ReplayMatchingIntegrationTest, FullPipelineProducesFillsAndBook) {
  const std::vector<Event> events = {
      newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{10}),
      newOrderEvent(Sequence{1}, OrderId{2}, Side::Ask, Price{101}, Quantity{10}),
      newOrderEvent(Sequence{2}, OrderId{3}, Side::Bid, Price{101}, Quantity{15}),
  };
  const std::vector<OrderId> ids = {OrderId{1}, OrderId{2}, OrderId{3}};

  OrderBook book;
  MatchingEngine engine(book);
  const ReplayResult result = tickforge::replayEvents(events, engine);
  EXPECT_TRUE(result.ok());

  // The last event's fills: 10 at 100, 5 at 101.
  ASSERT_EQ(engine.fills().size(), 2U);
  EXPECT_EQ(engine.fills()[0].price, Price{100});
  EXPECT_EQ(engine.fills()[1].price, Price{101});

  EXPECT_FALSE(book.contains(OrderId{1}));
  EXPECT_FALSE(book.contains(OrderId{3}));
  const std::int64_t remainingQty = book.find(OrderId{2}).value_or(RestingOrder{}).quantity.lots();
  EXPECT_EQ(remainingQty, 5);
}

TEST(ReplayMatchingIntegrationTest, ReplayTwiceEquivalent) {
  const std::vector<Event> events = {
      newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{10}),
      newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{99}, Quantity{10}),
      newOrderEvent(Sequence{2}, OrderId{3}, Side::Ask, Price{101}, Quantity{20}),
      newOrderEvent(Sequence{3}, OrderId{4}, Side::Bid, Price{101}, Quantity{25}),
      cancelEvent(Sequence{4}, OrderId{2}),
  };
  const std::vector<OrderId> ids = {OrderId{1}, OrderId{2}, OrderId{3}, OrderId{4}};

  const RunResult first = runStream(events, ids);
  const RunResult second = runStream(events, ids);

  EXPECT_TRUE(first.replay.ok());
  EXPECT_TRUE(second.replay.ok());
  EXPECT_EQ(first.fillsPerEvent, second.fillsPerEvent);
  EXPECT_EQ(first.book, second.book);
}

TEST(ReplayMatchingIntegrationTest, ReplayAbortOnInvalidEvent) {
  const std::vector<Event> events = {
      newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{10}),
      newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10}),
      cancelEvent(Sequence{2}, OrderId{99}),
  };

  OrderBook book;
  const ReplayResult result = tickforge::replayEvents(events, book);
  // Note: replaying into the raw book (no matching) still validates and
  // dispatches; the unknown cancel aborts.
  EXPECT_EQ(result.error, ReplayError::UnknownOrder);
  EXPECT_EQ(result.failedAt, Sequence{2});
}

TEST(ReplayMatchingIntegrationTest, MatchingEngineRejectsUnknownCancel) {
  const std::vector<Event> events = {
      newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{10}),
      cancelEvent(Sequence{1}, OrderId{99}),
  };

  OrderBook book;
  MatchingEngine engine(book);
  const ReplayResult result = tickforge::replayEvents(events, engine);
  EXPECT_EQ(result.error, ReplayError::UnknownOrder);
  EXPECT_EQ(result.failedAt, Sequence{1});
  // The resting ask from the first event remains.
  EXPECT_TRUE(book.contains(OrderId{1}));
}

TEST(ReplayMatchingIntegrationTest, ValidationStillGuardsMatching) {
  std::vector<Event> events = {
      newOrderEvent(Sequence{0}, OrderId{1}, Side::Ask, Price{100}, Quantity{10}),
      newOrderEvent(Sequence{1}, OrderId{2}, Side::Bid, Price{100}, Quantity{10}),
  };
  events[1].quantity = Quantity{0}; // invalid: zero quantity on NewOrder

  OrderBook book;
  MatchingEngine engine(book);
  const ReplayResult result = tickforge::replayEvents(events, engine);
  EXPECT_EQ(result.error, ReplayError::InvalidEvent);
  // The first event was processed (ask rests); the invalid one never ran.
  EXPECT_TRUE(book.contains(OrderId{1}));
  EXPECT_TRUE(engine.fills().empty());
}

} // namespace
