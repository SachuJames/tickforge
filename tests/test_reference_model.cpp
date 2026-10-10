// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Validates the independent reference model (test oracle) against
// hand-calculated scenarios BEFORE it is used for differential testing.
// If any of these fail, the oracle is wrong, not production.

#include "tests/support/event_generator.hpp"
#include "tests/support/reference_model.hpp"
#include "tickforge/event/event.hpp"

#include <gtest/gtest.h>

namespace tickforge::test {
namespace {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Event makeNew(std::uint64_t id, Side side, std::int64_t price, std::int64_t qty, std::size_t seq) {
  Event e;
  e.type = EventType::NewOrder;
  e.orderId = OrderId{id};
  e.side = side;
  e.price = Price{price};
  e.quantity = Quantity{qty};
  e.timestamp = Timestamp{1'000'000 + static_cast<std::int64_t>(seq)};
  e.sequence = Sequence{seq};
  e.instrument = "AAPL";
  return e;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Event makeCancel(std::uint64_t id, std::size_t seq) {
  Event e;
  e.type = EventType::CancelOrder;
  e.orderId = OrderId{id};
  e.timestamp = Timestamp{1'000'000 + static_cast<std::int64_t>(seq)};
  e.sequence = Sequence{seq};
  e.instrument = "AAPL";
  return e;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Event makeModify(std::uint64_t id, std::int64_t price, std::int64_t qty, std::size_t seq) {
  Event e;
  e.type = EventType::ModifyOrder;
  e.orderId = OrderId{id};
  e.price = Price{price};     // 0 = unchanged
  e.quantity = Quantity{qty}; // 0 = unchanged
  e.timestamp = Timestamp{1'000'000 + static_cast<std::int64_t>(seq)};
  e.sequence = Sequence{seq};
  e.instrument = "AAPL";
  return e;
}

// A resting buy then a crossing sell: partial fill at the resting price.
TEST(ReferenceModel, CrossingSellPartiallyFills) {
  ReferenceBook book;
  auto r1 = book.apply(makeNew(1, Side::Bid, 100, 10, 0));
  EXPECT_TRUE(r1.applied);
  EXPECT_TRUE(r1.fills.empty());

  auto r2 = book.apply(makeNew(2, Side::Ask, 100, 4, 1));
  EXPECT_TRUE(r2.applied);
  ASSERT_EQ(r2.fills.size(), 1U);
  EXPECT_EQ(r2.fills[0].aggressorId, OrderId{2});
  EXPECT_EQ(r2.fills[0].restingId, OrderId{1});
  EXPECT_EQ(r2.fills[0].side, Side::Ask);
  EXPECT_EQ(r2.fills[0].price, Price{100}); // resting price rule
  EXPECT_EQ(r2.fills[0].quantity, Quantity{4});

  const auto remaining = book.find(OrderId{1});
  ASSERT_TRUE(remaining.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
  EXPECT_EQ(remaining->quantity, Quantity{6});
  EXPECT_FALSE(book.find(OrderId{2}).has_value()); // fully filled, not resting
}

// FIFO: earlier order at the same price fills first.
TEST(ReferenceModel, FifoAtSamePrice) {
  ReferenceBook book;
  book.apply(makeNew(1, Side::Bid, 100, 5, 0));
  book.apply(makeNew(2, Side::Bid, 100, 5, 1));
  auto r = book.apply(makeNew(3, Side::Ask, 100, 7, 2));
  ASSERT_EQ(r.fills.size(), 2U);
  EXPECT_EQ(r.fills[0].restingId, OrderId{1});
  EXPECT_EQ(r.fills[0].quantity, Quantity{5});
  EXPECT_EQ(r.fills[1].restingId, OrderId{2});
  EXPECT_EQ(r.fills[1].quantity, Quantity{2});
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): order exists by construction.
  EXPECT_EQ(book.find(OrderId{2})->quantity, Quantity{3});
}

// Price priority: better price fills before worse price.
TEST(ReferenceModel, PricePriorityAcrossLevels) {
  ReferenceBook book;
  book.apply(makeNew(1, Side::Bid, 99, 5, 0));
  book.apply(makeNew(2, Side::Bid, 100, 5, 1));
  auto r = book.apply(makeNew(3, Side::Ask, 99, 8, 2));
  ASSERT_EQ(r.fills.size(), 2U);
  EXPECT_EQ(r.fills[0].restingId, OrderId{2}); // best bid first
  EXPECT_EQ(r.fills[0].price, Price{100});
  EXPECT_EQ(r.fills[1].restingId, OrderId{1});
  EXPECT_EQ(r.fills[1].price, Price{99});
}

// Residual rests after a sweep.
TEST(ReferenceModel, ResidualRests) {
  ReferenceBook book;
  book.apply(makeNew(1, Side::Ask, 101, 3, 0));
  auto r = book.apply(makeNew(2, Side::Bid, 105, 10, 1));
  ASSERT_EQ(r.fills.size(), 1U);
  EXPECT_EQ(r.fills[0].quantity, Quantity{3});
  const auto residual = book.find(OrderId{2});
  ASSERT_TRUE(residual.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
  EXPECT_EQ(residual->quantity, Quantity{7});
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
  EXPECT_EQ(residual->price, Price{105});
}

// Cancel removes only the targeted order.
TEST(ReferenceModel, CancelIsolation) {
  ReferenceBook book;
  book.apply(makeNew(1, Side::Bid, 100, 5, 0));
  book.apply(makeNew(2, Side::Bid, 100, 5, 1));
  auto r = book.apply(makeCancel(1, 2));
  EXPECT_TRUE(r.applied);
  EXPECT_FALSE(book.find(OrderId{1}).has_value());
  EXPECT_TRUE(book.find(OrderId{2}).has_value());
  // Unknown cancel is rejected.
  EXPECT_FALSE(book.apply(makeCancel(99, 3)).applied);
}

// Price-changing modify loses priority (cancel/replace).
TEST(ReferenceModel, ModifyPriceChangeLosesPriority) {
  ReferenceBook book;
  book.apply(makeNew(1, Side::Bid, 100, 5, 0));
  book.apply(makeNew(2, Side::Bid, 100, 5, 1));
  // Order 1 moves to 101: cancel/replace, now behind order 2 at 100.
  auto r = book.apply(makeModify(1, 101, 0, 2));
  EXPECT_TRUE(r.applied);
  EXPECT_TRUE(r.fills.empty());
  EXPECT_EQ(book.bestBid(), Price{101});
  const auto ids = book.ordersAtLevel(Side::Bid, Price{100});
  ASSERT_EQ(ids.size(), 1U);
  EXPECT_EQ(ids[0], OrderId{2});
}

// Quantity decrease keeps priority; increase is cancel/replace.
TEST(ReferenceModel, ModifyQuantitySemantics) {
  ReferenceBook book;
  book.apply(makeNew(1, Side::Bid, 100, 10, 0));
  book.apply(makeNew(2, Side::Bid, 100, 10, 1));
  // Decrease: stays ahead.
  book.apply(makeModify(1, 0, 4, 2));
  auto ids = book.ordersAtLevel(Side::Bid, Price{100});
  ASSERT_EQ(ids.size(), 2U);
  EXPECT_EQ(ids[0], OrderId{1});
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): order exists by construction.
  EXPECT_EQ(book.find(OrderId{1})->quantity, Quantity{4});
  // Increase: cancel/replace, goes behind.
  book.apply(makeModify(1, 0, 8, 3));
  ids = book.ordersAtLevel(Side::Bid, Price{100});
  ASSERT_EQ(ids.size(), 2U);
  EXPECT_EQ(ids[0], OrderId{2});
  EXPECT_EQ(ids[1], OrderId{1});
}

// The generator must only emit valid events.
TEST(EventGenerator, EmitsValidSequences) {
  for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL, 42ULL, 999ULL}) {
    GeneratorConfig config;
    config.seed = seed;
    config.eventCount = 100;
    EventGenerator gen(config);
    const auto events = gen.generate();
    ASSERT_EQ(events.size(), 100U);
    for (const auto& event : events) {
      EXPECT_EQ(validateEvent(event), EventValidationError::Ok)
          << "seed=" << seed << " event: " << EventGenerator::describe(event);
    }
  }
}

// Same config -> same sequence (determinism).
TEST(EventGenerator, DeterministicAcrossRuns) {
  GeneratorConfig config;
  config.seed = 12345;
  config.eventCount = 75;
  const auto first = EventGenerator(config).generate();
  const auto second = EventGenerator(config).generate();
  ASSERT_EQ(first.size(), second.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(EventGenerator::describe(first[i]), EventGenerator::describe(second[i]));
  }
}

// Different seeds -> different sequences (the generator explores).
TEST(EventGenerator, SeedsDiverge) {
  GeneratorConfig a;
  a.seed = 1;
  a.eventCount = 50;
  GeneratorConfig b = a;
  b.seed = 2;
  const auto seqA = EventGenerator(a).generate();
  const auto seqB = EventGenerator(b).generate();
  bool differ = false;
  for (std::size_t i = 0; i < seqA.size(); ++i) {
    if (EventGenerator::describe(seqA[i]) != EventGenerator::describe(seqB[i])) {
      differ = true;
      break;
    }
  }
  EXPECT_TRUE(differ);
}

} // namespace
} // namespace tickforge::test
