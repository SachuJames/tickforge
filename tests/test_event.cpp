// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Tests for the canonical Event: construction, equality, EventKey
// extraction, and the (timestamp, sequence) ordering from SPEC.md 3.2.

#include "tickforge/event/event.hpp"

#include <cstdint>
#include <gtest/gtest.h>

namespace {

using tickforge::Event;
using tickforge::EventKey;
using tickforge::EventType;
using tickforge::OrderId;
using tickforge::Price;
using tickforge::Quantity;
using tickforge::Sequence;
using tickforge::Side;
using tickforge::Timestamp;

Event makeNewOrder(std::int64_t nanos, std::uint64_t seq, std::uint64_t orderId) {
  Event event;
  event.timestamp = Timestamp{nanos};
  event.sequence = Sequence{seq};
  event.type = EventType::NewOrder;
  event.instrument = "AAPL";
  event.orderId = OrderId{orderId};
  event.side = Side::Bid;
  event.price = Price{100};
  event.quantity = Quantity{10};
  return event;
}

TEST(EventTest, DesignatedInitializers) {
  const Event event{
      .timestamp = Timestamp{100},
      .sequence = Sequence{4},
      .type = EventType::NewOrder,
      .instrument = "AAPL",
      .orderId = OrderId{7},
      .side = Side::Bid,
      .price = Price{100},
      .quantity = Quantity{10},
  };
  EXPECT_EQ(event.timestamp, Timestamp{100});
  EXPECT_EQ(event.sequence, Sequence{4});
  EXPECT_EQ(event.type, EventType::NewOrder);
  EXPECT_EQ(event.instrument, "AAPL");
  EXPECT_EQ(event.orderId, OrderId{7});
  EXPECT_EQ(event.side, Side::Bid);
  EXPECT_EQ(event.price, Price{100});
  EXPECT_EQ(event.quantity, Quantity{10});
  EXPECT_EQ(event.flags, 0u);
}

TEST(EventTest, DefaultConstruction) {
  const Event event{};
  EXPECT_EQ(event.timestamp, Timestamp{0});
  EXPECT_EQ(event.sequence, Sequence{0});
  EXPECT_TRUE(event.instrument.empty());
  EXPECT_EQ(event.flags, 0u);
}

TEST(EventTest, EqualEventsCompareEqual) {
  const Event a = makeNewOrder(100, 4, 7);
  const Event b = makeNewOrder(100, 4, 7);
  EXPECT_EQ(a, b);
  EXPECT_FALSE(a != b);
}

TEST(EventTest, DifferingFieldsCompareUnequal) {
  const Event base = makeNewOrder(100, 4, 7);
  Event other = base;
  other.timestamp = Timestamp{101};
  EXPECT_NE(base, other);

  other = base;
  other.sequence = Sequence{5};
  EXPECT_NE(base, other);

  other = base;
  other.type = EventType::CancelOrder;
  EXPECT_NE(base, other);

  other = base;
  other.orderId = OrderId{8};
  EXPECT_NE(base, other);
}

TEST(EventTest, EventKeyExtraction) {
  const Event event = makeNewOrder(100, 4, 7);
  const EventKey key = tickforge::eventKey(event);
  EXPECT_EQ(key.timestamp, Timestamp{100});
  EXPECT_EQ(key.sequence, Sequence{4});
  const EventKey expected{Timestamp{100}, Sequence{4}};
  EXPECT_EQ(key, expected);
}

TEST(EventTest, SameTimestampOrdersBySequence) {
  // From the Day 02 task: A(ts=100, seq=4) < B(ts=100, seq=5).
  const Event a = makeNewOrder(100, 4, 1);
  const Event b = makeNewOrder(100, 5, 2);
  EXPECT_LT(a, b);
  EXPECT_GT(b, a);
  EXPECT_FALSE(a == b);
}

TEST(EventTest, TimestampDominatesSequence) {
  // From the Day 02 task: C(ts=99, seq=100) < D(ts=100, seq=0).
  const Event c = makeNewOrder(99, 100, 1);
  const Event d = makeNewOrder(100, 0, 2);
  EXPECT_LT(c, d);
  EXPECT_GT(d, c);
}

TEST(EventTest, OrderingIgnoresNonKeyFields) {
  // operator< implements the canonical replay key only; two events with
  // the same key but different payloads are unordered yet unequal.
  Event a = makeNewOrder(100, 4, 7);
  Event b = makeNewOrder(100, 4, 7);
  b.price = Price{999};
  EXPECT_FALSE(a < b);
  EXPECT_FALSE(b < a);
  EXPECT_NE(a, b);
  EXPECT_LE(a, b);
  EXPECT_GE(a, b);
}

} // namespace
