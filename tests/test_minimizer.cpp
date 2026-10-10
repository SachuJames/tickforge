// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Tests for the deterministic minimizer, using a synthetic predicate
// (not the matching engine) so the minimizer itself is validated
// independently.

#include "tests/support/event_generator.hpp"
#include "tests/support/minimizer.hpp"
#include "tickforge/event/event.hpp"

#include <gtest/gtest.h>

namespace tickforge::test {
namespace {

Event makeNew(std::uint64_t id, std::size_t seq) {
  Event e;
  e.type = EventType::NewOrder;
  e.orderId = OrderId{id};
  e.side = Side::Bid;
  e.price = Price{100};
  e.quantity = Quantity{10};
  e.timestamp = Timestamp{1'000'000 + static_cast<std::int64_t>(seq)};
  e.sequence = Sequence{seq};
  e.instrument = "AAPL";
  return e;
}

// Predicate: fails iff the sequence contains a NewOrder with id 7.
bool containsSeven(const std::vector<Event>& events) {
  for (const auto& e : events) {
    if (e.type == EventType::NewOrder && e.orderId == OrderId{7}) {
      return true;
    }
  }
  return false;
}

TEST(Minimizer, ReducesToRelevantEvent) {
  std::vector<Event> events;
  for (std::uint64_t id = 1; id <= 20; ++id) {
    events.push_back(makeNew(id, id - 1));
  }
  const auto minimized = minimizeFailingSequence(events, containsSeven);
  ASSERT_EQ(minimized.size(), 1u);
  EXPECT_EQ(minimized[0].orderId, OrderId{7});
  // Renormalized: dense seq starting at 0.
  EXPECT_EQ(minimized[0].sequence, Sequence{0});
}

TEST(Minimizer, KeepsInputWhenNothingRemovable) {
  std::vector<Event> events{makeNew(7, 0)};
  const auto minimized = minimizeFailingSequence(events, containsSeven);
  ASSERT_EQ(minimized.size(), 1u);
  EXPECT_EQ(minimized[0].orderId, OrderId{7});
}

TEST(Minimizer, Deterministic) {
  GeneratorConfig config;
  config.seed = 11;
  config.eventCount = 30;
  const auto events = EventGenerator(config).generate();
  const auto first = minimizeFailingSequence(events, containsSeven);
  const auto second = minimizeFailingSequence(events, containsSeven);
  ASSERT_EQ(first.size(), second.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(EventGenerator::describe(first[i]), EventGenerator::describe(second[i]));
  }
}

} // namespace
} // namespace tickforge::test
