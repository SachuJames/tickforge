// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Property-style tests for the canonical (timestamp, sequence) ordering.
// These invariants are foundational: the deterministic replay of Day 03+
// depends on this ordering being a strict weak ordering with stable,
// repeatable sorts.

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "tickforge/event/event.hpp"

namespace {

using tickforge::Event;
using tickforge::EventType;
using tickforge::OrderId;
using tickforge::Price;
using tickforge::Quantity;
using tickforge::Sequence;
using tickforge::Side;
using tickforge::Timestamp;

Event makeEvent(std::int64_t nanos, std::uint64_t seq) {
  Event event;
  event.timestamp = Timestamp{nanos};
  event.sequence = Sequence{seq};
  event.type = EventType::NewOrder;
  event.instrument = "AAPL";
  event.orderId = OrderId{seq};
  event.side = Side::Bid;
  event.price = Price{100};
  event.quantity = Quantity{10};
  return event;
}

std::vector<Event> sampleEvents() {
  return {
      makeEvent(100, 5),
      makeEvent(99, 100),
      makeEvent(100, 4),
      makeEvent(100, 4),  // duplicate key, distinct payload identity
      makeEvent(-50, 0),
      makeEvent(100, 6),
      makeEvent(101, 0),
  };
}

TEST(EventOrderingTest, Irreflexive) {
  for (const Event& e : sampleEvents()) {
    EXPECT_FALSE(e < e);
  }
}

TEST(EventOrderingTest, Transitive) {
  const std::vector<Event> events = sampleEvents();
  for (const Event& a : events) {
    for (const Event& b : events) {
      for (const Event& c : events) {
        if (a < b && b < c) {
          EXPECT_LT(a, c);
        }
      }
    }
  }
}

TEST(EventOrderingTest, EqualityConsistentWithOrdering) {
  const std::vector<Event> events = sampleEvents();
  for (const Event& a : events) {
    for (const Event& b : events) {
      if (a == b) {
        EXPECT_FALSE(a < b);
        EXPECT_FALSE(b < a);
      }
      // Antisymmetry: both directions cannot hold at once.
      EXPECT_FALSE(a < b && b < a);
    }
  }
}

TEST(EventOrderingTest, TotalOrderRespectsKey) {
  // Every event sorts exactly by (timestamp, sequence).
  std::vector<Event> events = sampleEvents();
  std::sort(events.begin(), events.end());
  for (std::size_t i = 1; i < events.size(); ++i) {
    const Event& prev = events[i - 1];
    const Event& curr = events[i];
    EXPECT_TRUE(prev.timestamp < curr.timestamp ||
                (prev.timestamp == curr.timestamp && prev.sequence <= curr.sequence));
  }
  EXPECT_EQ(events.front().timestamp, Timestamp{-50});
  EXPECT_EQ(events.back().timestamp, Timestamp{101});
}

TEST(EventOrderingTest, DeterministicSort) {
  // Sorting the same events twice, from different starting permutations,
  // must produce the exact same key sequence.
  auto keysOf = [](std::vector<Event> events) {
    std::sort(events.begin(), events.end());
    std::vector<std::uint64_t> keys;
    keys.reserve(events.size());
    for (const Event& e : events) {
      keys.push_back(e.sequence.value());
    }
    return keys;
  };

  std::vector<Event> first = sampleEvents();
  std::vector<Event> second = sampleEvents();
  std::shuffle(first.begin(), first.end(), std::mt19937{42});
  std::shuffle(second.begin(), second.end(), std::mt19937{1337});

  EXPECT_EQ(keysOf(first), keysOf(second));
  EXPECT_EQ(keysOf(first), keysOf(sampleEvents()));
}

TEST(EventOrderingTest, DuplicateKeysAreEquivalentForOrdering) {
  const Event a = makeEvent(100, 4);
  const Event b = makeEvent(100, 4);
  EXPECT_FALSE(a < b);
  EXPECT_FALSE(b < a);
  EXPECT_LE(a, b);
  EXPECT_GE(a, b);
}

}  // namespace
