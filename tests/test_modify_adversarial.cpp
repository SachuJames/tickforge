// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Adversarial modify-as-cancel/replace tests (SPEC.md 5.5).
//
// Verifies the modification contract under adversarial price and
// quantity combinations: cancel/replace vs in-place priority rules,
// spread-crossing replacements, multi-level sweeps, queue-priority
// reassignment, FIFO placement, fill-level correctness, and
// production-vs-reference differential testing.
//
// Every case is deterministic. Hand-calculated scenarios establish the
// expected results from the specification, not from engine output.

#include "tests/support/event_generator.hpp"
#include "tests/support/reference_model.hpp"
#include "tickforge/analytics/execution_statistics.hpp"
#include "tickforge/analytics/queue_tracker.hpp"
#include "tickforge/book/order_book.hpp"
#include "tickforge/matching/fill.hpp"
#include "tickforge/matching/matching_engine.hpp"
#include "tickforge/replay/replay.hpp"

#include <cstdlib>
#include <gtest/gtest.h>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace tickforge::test {
namespace {

// Wraps MatchingEngine to accumulate fills across all events.
class ModifyFillCollector : public EventProcessor {
public:
  explicit ModifyFillCollector(MatchingEngine& engine) : engine_(engine) {}

  bool onEvent(const Event& event) override {
    const bool applied = engine_.onEvent(event);
    if (applied) {
      const auto& fills = engine_.fills();
      allFills_.insert(allFills_.end(), fills.begin(), fills.end());
    }
    return applied;
  }

  [[nodiscard]] const std::vector<Fill>& allFills() const noexcept {
    return allFills_;
  }

private:
  MatchingEngine& engine_;
  std::vector<Fill> allFills_;
};

struct ModifyOutcome {
  ReplayResult result;
  std::vector<Fill> fills;
  std::size_t orderCount = 0;
  std::optional<Price> bestBid;
  std::optional<Price> bestAsk;
};

ModifyOutcome runModify(const std::vector<Event>& events, ReplayMode mode) {
  OrderBook book;
  MatchingEngine engine(book);
  ModifyFillCollector collector(engine);
  ModifyOutcome out;
  out.result = replayEvents(events, collector, mode);
  out.fills = collector.allFills();
  out.orderCount = book.orderCount();
  out.bestBid = book.bestBid();
  out.bestAsk = book.bestAsk();
  return out;
}

// Event builders for hand-calculated scenarios.

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): test helper with clear arg order.
Event makeNew(std::uint64_t id,
              Side side,
              std::int64_t price,
              std::int64_t qty,
              std::int64_t ts,
              std::uint64_t seq) {
  Event e;
  e.type = EventType::NewOrder;
  e.orderId = OrderId{id};
  e.side = side;
  e.price = Price{price};
  e.quantity = Quantity{qty};
  e.timestamp = Timestamp{ts};
  e.sequence = Sequence{seq};
  e.instrument = "AAPL";
  return e;
}

// Modify: zero price/qty means "unchanged" per the event contract.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): test helper with clear arg order.
Event makeModify(
    std::uint64_t id, std::int64_t price, std::int64_t qty, std::int64_t ts, std::uint64_t seq) {
  Event e;
  e.type = EventType::ModifyOrder;
  e.orderId = OrderId{id};
  e.price = Price{price};
  e.quantity = Quantity{qty};
  e.timestamp = Timestamp{ts};
  e.sequence = Sequence{seq};
  e.instrument = "AAPL";
  return e;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): test helper with clear arg order.
Event makeCancel(std::uint64_t id, std::int64_t ts, std::uint64_t seq) {
  Event e;
  e.type = EventType::CancelOrder;
  e.orderId = OrderId{id};
  e.timestamp = Timestamp{ts};
  e.sequence = Sequence{seq};
  e.instrument = "AAPL";
  return e;
}

// --- Part 3: Hand-calculated baseline scenarios ---

// Price modification, non-crossing: cancel/replace, no fills, new
// arrival priority at the destination level.
TEST(ModifyAdversarial, PriceChangeNonCrossing) {
  std::vector<Event> events;
  events.push_back(makeNew(1, Side::Bid, 100, 10, 1000000, 0));
  events.push_back(makeNew(2, Side::Ask, 102, 5, 1000001, 1));
  events.push_back(makeModify(1, 101, 0, 1000002, 2)); // price 100->101, qty unchanged

  const auto out = runModify(events, ReplayMode::Strict);
  ASSERT_TRUE(out.result.ok());
  EXPECT_EQ(out.fills.size(), 0U);
  EXPECT_EQ(out.orderCount, 2U);
  EXPECT_EQ(out.bestBid, Price{101});
  EXPECT_EQ(out.bestAsk, Price{102});
}

// Quantity decrease at the same price: in-place, priority preserved.
// id=1 stays ahead of id=2 in the FIFO queue.
TEST(ModifyAdversarial, QuantityDecreaseKeepsPriority) {
  std::vector<Event> events;
  events.push_back(makeNew(1, Side::Bid, 100, 10, 1000000, 0));
  events.push_back(makeNew(2, Side::Bid, 100, 5, 1000001, 1));
  events.push_back(makeModify(1, 0, 4, 1000002, 2)); // qty 10->4, price unchanged

  OrderBook book;
  MatchingEngine engine(book);
  ModifyFillCollector collector(engine);
  const auto result = replayEvents(events, collector, ReplayMode::Strict);
  ASSERT_TRUE(result.ok());

  // id=1 must still be first at level 100.
  const auto ids = book.ordersAtLevel(Side::Bid, Price{100});
  ASSERT_EQ(ids.size(), 2U);
  EXPECT_EQ(ids[0], OrderId{1});
  EXPECT_EQ(ids[1], OrderId{2});
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
  const auto resting = book.find(OrderId{1});
  ASSERT_TRUE(resting.has_value());
  EXPECT_EQ(resting->quantity, Quantity{4});
}

// Quantity increase at the same price: cancel/replace, priority lost.
// id=1 moves to the back of the level behind id=2.
TEST(ModifyAdversarial, QuantityIncreaseLosesPriority) {
  std::vector<Event> events;
  events.push_back(makeNew(1, Side::Bid, 100, 10, 1000000, 0));
  events.push_back(makeNew(2, Side::Bid, 100, 5, 1000001, 1));
  events.push_back(makeModify(1, 0, 15, 1000002, 2)); // qty 10->15, price unchanged

  OrderBook book;
  MatchingEngine engine(book);
  ModifyFillCollector collector(engine);
  const auto result = replayEvents(events, collector, ReplayMode::Strict);
  ASSERT_TRUE(result.ok());

  const auto ids = book.ordersAtLevel(Side::Bid, Price{100});
  ASSERT_EQ(ids.size(), 2U);
  EXPECT_EQ(ids[0], OrderId{2});
  EXPECT_EQ(ids[1], OrderId{1});
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
  const auto resting = book.find(OrderId{1});
  ASSERT_TRUE(resting.has_value());
  EXPECT_EQ(resting->quantity, Quantity{15});
}

// Crossing replacement: partial fill, residual rests.
// Bid 99x10 modified to 100; ask 100x5 resting.
TEST(ModifyAdversarial, CrossingReplacementPartialFill) {
  std::vector<Event> events;
  events.push_back(makeNew(1, Side::Ask, 100, 5, 1000000, 0));
  events.push_back(makeNew(2, Side::Ask, 101, 5, 1000001, 1));
  events.push_back(makeNew(3, Side::Bid, 99, 10, 1000002, 2));
  events.push_back(makeModify(3, 100, 0, 1000003, 3)); // price 99->100, crosses

  const auto out = runModify(events, ReplayMode::Strict);
  ASSERT_TRUE(out.result.ok());
  ASSERT_EQ(out.fills.size(), 1U);
  const auto& f = out.fills[0];
  EXPECT_EQ(f.aggressorId, OrderId{3});
  EXPECT_EQ(f.restingId, OrderId{1});
  EXPECT_EQ(f.side, Side::Bid);
  EXPECT_EQ(f.price, Price{100}); // resting price
  EXPECT_EQ(f.quantity, Quantity{5});
  EXPECT_EQ(f.timestamp, Timestamp{1000003});
  EXPECT_EQ(f.sequence, Sequence{3});
  // Residual: id=3 now bid 100 x5. id=1 gone. id=2 still ask 101 x5.
  EXPECT_EQ(out.orderCount, 2U);
  EXPECT_EQ(out.bestBid, Price{100});
  EXPECT_EQ(out.bestAsk, Price{101});
}

// Multi-level sweep: replacement crosses two levels, partial on third.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): test with many assertions.
TEST(ModifyAdversarial, CrossingReplacementMultiLevelSweep) {
  std::vector<Event> events;
  events.push_back(makeNew(1, Side::Ask, 100, 3, 1000000, 0));
  events.push_back(makeNew(2, Side::Ask, 101, 4, 1000001, 1));
  events.push_back(makeNew(3, Side::Ask, 102, 5, 1000002, 2));
  events.push_back(makeNew(4, Side::Bid, 99, 10, 1000003, 3));
  events.push_back(makeModify(4, 102, 0, 1000004, 4)); // price 99->102

  const auto out = runModify(events, ReplayMode::Strict);
  ASSERT_TRUE(out.result.ok());
  ASSERT_EQ(out.fills.size(), 3U);
  // Fill order: best price first.
  EXPECT_EQ(out.fills[0].restingId, OrderId{1});
  EXPECT_EQ(out.fills[0].price, Price{100});
  EXPECT_EQ(out.fills[0].quantity, Quantity{3});
  EXPECT_EQ(out.fills[1].restingId, OrderId{2});
  EXPECT_EQ(out.fills[1].price, Price{101});
  EXPECT_EQ(out.fills[1].quantity, Quantity{4});
  EXPECT_EQ(out.fills[2].restingId, OrderId{3});
  EXPECT_EQ(out.fills[2].price, Price{102});
  EXPECT_EQ(out.fills[2].quantity, Quantity{3}); // 10 - 3 - 4 = 3
  for (const auto& fill : out.fills) {
    EXPECT_EQ(fill.aggressorId, OrderId{4});
    EXPECT_EQ(fill.side, Side::Bid);
    EXPECT_EQ(fill.timestamp, Timestamp{1000004});
    EXPECT_EQ(fill.sequence, Sequence{4});
  }
  // id=4 fully filled (3+4+3=10). id=3 has 2 left.
  EXPECT_EQ(out.orderCount, 1U);
  EXPECT_EQ(out.bestAsk, Price{102});
  EXPECT_FALSE(out.bestBid.has_value());
}

// Unknown-order modification: processor rejects, strict aborts.
TEST(ModifyAdversarial, UnknownOrderModifyStrict) {
  std::vector<Event> events;
  events.push_back(makeNew(1, Side::Bid, 100, 10, 1000000, 0));
  events.push_back(makeModify(999, 101, 0, 1000001, 1));

  const auto out = runModify(events, ReplayMode::Strict);
  EXPECT_EQ(out.result.error, ReplayError::UnknownOrder);
  EXPECT_EQ(out.result.failedAt, Sequence{1});
}

// Unknown-order modification: lenient skips and continues.
TEST(ModifyAdversarial, UnknownOrderModifyLenient) {
  std::vector<Event> events;
  events.push_back(makeNew(1, Side::Bid, 100, 10, 1000000, 0));
  events.push_back(makeModify(999, 101, 0, 1000001, 1));
  events.push_back(makeCancel(1, 1000002, 2));

  const auto out = runModify(events, ReplayMode::Lenient);
  EXPECT_TRUE(out.result.ok());
  EXPECT_EQ(out.result.skippedCount, 1U);
  EXPECT_EQ(out.orderCount, 0U); // cancel of id=1 still applied
}

// Empty modify (both fields zero): validation rejects.
TEST(ModifyAdversarial, EmptyModifyInvalid) {
  std::vector<Event> events;
  events.push_back(makeNew(1, Side::Bid, 100, 10, 1000000, 0));
  events.push_back(makeModify(1, 0, 0, 1000001, 1)); // nothing changes

  const auto strict = runModify(events, ReplayMode::Strict);
  EXPECT_EQ(strict.result.error, ReplayError::InvalidEvent);
  EXPECT_EQ(strict.result.validationReason, EventValidationError::EmptyModify);

  const auto lenient = runModify(events, ReplayMode::Lenient);
  EXPECT_TRUE(lenient.result.ok());
  EXPECT_EQ(lenient.result.skippedCount, 1U);
}

// --- Part 4: Adversarial price combinations ---

// Non-crossing replacement to an occupied destination level: the
// replacement joins the back of the FIFO queue (cancel/replace loses
// priority per SPEC 5.5).
TEST(ModifyAdversarial, NonCrossingToOccupiedLevel) {
  std::vector<Event> events;
  events.push_back(makeNew(1, Side::Bid, 100, 10, 1000000, 0));
  events.push_back(makeNew(2, Side::Bid, 101, 5, 1000001, 1));
  events.push_back(makeNew(3, Side::Bid, 99, 8, 1000002, 2));
  events.push_back(makeModify(3, 101, 0, 1000003, 3)); // 99->101, non-crossing

  OrderBook book;
  MatchingEngine engine(book);
  ModifyFillCollector collector(engine);
  const auto result = replayEvents(events, collector, ReplayMode::Strict);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(collector.allFills().size(), 0U);

  // Destination level 101: [id=2 (earlier), id=3 (replacement at back)].
  const auto ids = book.ordersAtLevel(Side::Bid, Price{101});
  ASSERT_EQ(ids.size(), 2U);
  EXPECT_EQ(ids[0], OrderId{2});
  EXPECT_EQ(ids[1], OrderId{3});
  EXPECT_EQ(book.bestBid(), Price{101});
}

// Spread-crossing at exactly the opposing best price.
TEST(ModifyAdversarial, CrossingAtOpposingBest) {
  std::vector<Event> events;
  events.push_back(makeNew(1, Side::Ask, 100, 7, 1000000, 0));
  events.push_back(makeNew(2, Side::Bid, 99, 10, 1000001, 1));
  events.push_back(makeModify(2, 100, 0, 1000002, 2)); // 99->100, exactly at ask

  const auto out = runModify(events, ReplayMode::Strict);
  ASSERT_TRUE(out.result.ok());
  ASSERT_EQ(out.fills.size(), 1U);
  EXPECT_EQ(out.fills[0].quantity, Quantity{7});
  EXPECT_EQ(out.fills[0].price, Price{100});
  // Residual: id=2 bid 100 x3.
  EXPECT_EQ(out.orderCount, 1U);
  EXPECT_EQ(out.bestBid, Price{100});
  EXPECT_FALSE(out.bestAsk.has_value());
}

// Replacement crossing the entire opposing book.
TEST(ModifyAdversarial, CrossingEntireBook) {
  std::vector<Event> events;
  events.push_back(makeNew(1, Side::Ask, 100, 2, 1000000, 0));
  events.push_back(makeNew(2, Side::Ask, 105, 3, 1000001, 1));
  events.push_back(makeNew(3, Side::Bid, 90, 20, 1000002, 2));
  events.push_back(makeModify(3, 110, 0, 1000003, 3)); // sweeps everything

  const auto out = runModify(events, ReplayMode::Strict);
  ASSERT_TRUE(out.result.ok());
  ASSERT_EQ(out.fills.size(), 2U);
  EXPECT_EQ(out.fills[0].price, Price{100});
  EXPECT_EQ(out.fills[0].quantity, Quantity{2});
  EXPECT_EQ(out.fills[1].price, Price{105});
  EXPECT_EQ(out.fills[1].quantity, Quantity{3});
  // Residual: id=3 bid 110 x15 rests (no more asks).
  EXPECT_EQ(out.orderCount, 1U);
  EXPECT_EQ(out.bestBid, Price{110});
  EXPECT_FALSE(out.bestAsk.has_value());
}

// --- Part 5: Adversarial quantity combinations ---

// Quantity decrease to the minimum valid quantity (1 lot).
TEST(ModifyAdversarial, QuantityDecreaseToMinimum) {
  std::vector<Event> events;
  events.push_back(makeNew(1, Side::Bid, 100, 10, 1000000, 0));
  events.push_back(makeModify(1, 0, 1, 1000001, 1));

  OrderBook book;
  MatchingEngine engine(book);
  ModifyFillCollector collector(engine);
  ASSERT_TRUE(replayEvents(events, collector, ReplayMode::Strict).ok());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
  const auto resting = book.find(OrderId{1});
  ASSERT_TRUE(resting.has_value());
  EXPECT_EQ(resting->quantity, Quantity{1});
}

// Zero quantity modify: "unchanged" per the event contract, so this is
// an empty modify and invalid.
TEST(ModifyAdversarial, ZeroQuantityModifyInvalid) {
  std::vector<Event> events;
  events.push_back(makeNew(1, Side::Bid, 100, 10, 1000000, 0));
  // price=0 and qty=0 both mean "unchanged" -> EmptyModify.
  events.push_back(makeModify(1, 0, 0, 1000001, 1));

  const auto out = runModify(events, ReplayMode::Strict);
  EXPECT_EQ(out.result.error, ReplayError::InvalidEvent);
  EXPECT_EQ(out.result.validationReason, EventValidationError::EmptyModify);
}

// Negative quantity: invalid price/quantity per validateEvent.
TEST(ModifyAdversarial, NegativeQuantityModifyInvalid) {
  std::vector<Event> events;
  events.push_back(makeNew(1, Side::Bid, 100, 10, 1000000, 0));
  events.push_back(makeModify(1, 0, -5, 1000001, 1));

  const auto out = runModify(events, ReplayMode::Strict);
  EXPECT_EQ(out.result.error, ReplayError::InvalidEvent);
  EXPECT_EQ(out.result.validationReason, EventValidationError::InvalidQuantity);
}

// --- Part 6: Queue priority and FIFO ---

// Destination-level FIFO under a marketable sweep: the replacement is
// at the back, so it is the last to be consumed at its level.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): test with many assertions.
TEST(ModifyAdversarial, ReplacementAtBackOfQueue) {
  std::vector<Event> events;
  events.push_back(makeNew(1, Side::Ask, 100, 5, 1000000, 0));
  events.push_back(makeNew(2, Side::Ask, 100, 5, 1000001, 1));
  events.push_back(makeNew(3, Side::Ask, 99, 5, 1000002, 2));
  events.push_back(makeModify(3, 100, 0, 1000003, 3)); // 99->100, joins back
  // Sweep the 100 level with a bid.
  events.push_back(makeNew(4, Side::Bid, 100, 12, 1000004, 4));

  const auto out = runModify(events, ReplayMode::Strict);
  ASSERT_TRUE(out.result.ok());
  ASSERT_EQ(out.fills.size(), 3U);
  // FIFO: id=1, then id=2, then id=3 (replacement at back).
  EXPECT_EQ(out.fills[0].restingId, OrderId{1});
  EXPECT_EQ(out.fills[1].restingId, OrderId{2});
  EXPECT_EQ(out.fills[2].restingId, OrderId{3});
  EXPECT_EQ(out.fills[2].quantity, Quantity{2}); // 12 - 5 - 5 = 2
  // id=4 fully filled. Residual: id=3 ask 100 x3.
  EXPECT_EQ(out.orderCount, 1U);
  EXPECT_EQ(out.bestAsk, Price{100});
  EXPECT_FALSE(out.bestBid.has_value());
}

// QueueTracker: modification lifecycle and queue position.
TEST(ModifyAdversarial, QueueTrackerModifyLifecycle) {
  std::vector<Event> events;
  events.push_back(makeNew(1, Side::Bid, 100, 10, 1000000, 0));
  events.push_back(makeNew(2, Side::Bid, 100, 5, 1000001, 1));

  OrderBook book;
  MatchingEngine engine(book);
  QueueTracker tracker;
  tracker.designate(OrderId{1});
  tracker.designate(OrderId{2});

  for (const auto& e : events) {
    tracker.onEvent(e);
    ASSERT_TRUE(engine.onEvent(e));
    tracker.onFills(engine.fills());
  }
  // Before modify: id=1 at rank 1.
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
    const auto pos = tracker.queuePosition(book, OrderId{1});
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(pos->rank, 1U);
    EXPECT_EQ(pos->quantityAhead, Quantity{0});
  }
  // Quantity increase -> cancel/replace -> back of queue.
  const Event mod = makeModify(1, 0, 15, 1000002, 2);
  tracker.onEvent(mod);
  ASSERT_TRUE(engine.onEvent(mod));
  tracker.onFills(engine.fills());

  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
    const auto pos = tracker.queuePosition(book, OrderId{1});
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
    ASSERT_TRUE(pos.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
    EXPECT_EQ(pos->rank, 2U);
    EXPECT_EQ(pos->quantityAhead, Quantity{5}); // id=2's 5 lots ahead
    EXPECT_EQ(pos->quantityRemaining, Quantity{15});
  }
  EXPECT_EQ(tracker.lifecycle(book, OrderId{1}), QueueTracker::Lifecycle::Resting);
}

// --- Part 7: Fill-level differential testing ---

// Full fill-by-fill differential: production vs reference model.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): test helper with many assertions.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): test helper, seed first is clear.
void checkFillDifferential(const std::vector<Event>& events, std::uint64_t seed) {
  const auto prod = runModify(events, ReplayMode::Strict);
  ASSERT_TRUE(prod.result.ok()) << "seed=" << seed;

  ReferenceBook ref;
  std::vector<Fill> ref_fills;
  for (const auto& e : events) {
    auto r = ref.apply(e);
    ASSERT_TRUE(r.applied) << "seed=" << seed;
    ref_fills.insert(ref_fills.end(), r.fills.begin(), r.fills.end());
  }

  ASSERT_EQ(prod.fills.size(), ref_fills.size()) << "seed=" << seed;
  for (std::size_t i = 0; i < prod.fills.size(); ++i) {
    const auto& p = prod.fills[i];
    const auto& r = ref_fills[i];
    EXPECT_EQ(p.aggressorId, r.aggressorId) << "seed=" << seed << " fill=" << i;
    EXPECT_EQ(p.restingId, r.restingId) << "seed=" << seed << " fill=" << i;
    EXPECT_EQ(p.side, r.side) << "seed=" << seed << " fill=" << i;
    EXPECT_EQ(p.price, r.price) << "seed=" << seed << " fill=" << i;
    EXPECT_EQ(p.quantity, r.quantity) << "seed=" << seed << " fill=" << i;
    EXPECT_EQ(p.timestamp, r.timestamp) << "seed=" << seed << " fill=" << i;
    EXPECT_EQ(p.sequence, r.sequence) << "seed=" << seed << " fill=" << i;
  }

  // Full resting-order comparison.
  EXPECT_EQ(prod.orderCount, ref.orderCount()) << "seed=" << seed;
  EXPECT_EQ(prod.bestBid, ref.bestBid()) << "seed=" << seed;
  EXPECT_EQ(prod.bestAsk, ref.bestAsk()) << "seed=" << seed;
}

TEST(ModifyAdversarial, FillDifferentialGenerated) {
  for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL, 42ULL}) {
    GeneratorConfig config;
    config.seed = seed;
    config.eventCount = 60;
    const auto events = EventGenerator(config).generate();
    checkFillDifferential(events, seed);
  }
}

// ExecutionStatistics agree with collected fills.
TEST(ModifyAdversarial, StatisticsMatchFills) {
  std::vector<Event> events;
  events.push_back(makeNew(1, Side::Ask, 100, 5, 1000000, 0));
  events.push_back(makeNew(2, Side::Ask, 101, 5, 1000001, 1));
  events.push_back(makeNew(3, Side::Bid, 99, 12, 1000002, 2));
  events.push_back(makeModify(3, 101, 0, 1000003, 3));

  OrderBook book;
  MatchingEngine engine(book);
  ModifyFillCollector collector(engine);
  ASSERT_TRUE(replayEvents(events, collector, ReplayMode::Strict).ok());

  ExecutionStatistics stats;
  stats.addFills(collector.allFills());
  EXPECT_EQ(stats.fillCount(), 2U);
  EXPECT_EQ(stats.totalQuantity(), Quantity{10}); // 5 + 5
  EXPECT_EQ(stats.buyQuantity(), Quantity{10});   // aggressor was Bid
  EXPECT_EQ(stats.sellQuantity(), Quantity{0});
  EXPECT_EQ(stats.minPrice(), Price{100});
  EXPECT_EQ(stats.maxPrice(), Price{101});
  const auto vwap = stats.averagePrice();
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
  ASSERT_TRUE(vwap.has_value());
  // (100*5 + 101*5) / 10 = 100.5 -> numerator 1005, denominator 10.
  EXPECT_EQ(vwap->quantitySum, 10);
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
  const __uint128_t expected =
      static_cast<__uint128_t>(100) * 5 + static_cast<__uint128_t>(101) * 5;
  const __uint128_t actual =
      (static_cast<__uint128_t>(vwap->priceQuantitySumHi) << 64) | vwap->priceQuantitySumLo;
  EXPECT_EQ(actual, expected);
}

// --- Part 8: Deterministic adversarial generation ---

// Generated modification-heavy sequences: differential + determinism.
TEST(ModifyAdversarial, GeneratedModificationsDeterministic) {
  for (const std::uint64_t seed : {5ULL, 6ULL, 7ULL}) {
    GeneratorConfig config;
    config.seed = seed;
    config.eventCount = 80;
    config.modifyWeight = 50; // heavier modification mix
    config.newWeight = 30;
    config.cancelWeight = 20;
    const auto events = EventGenerator(config).generate();

    const auto first = runModify(events, ReplayMode::Strict);
    const auto second = runModify(events, ReplayMode::Strict);
    EXPECT_EQ(first.result.error, second.result.error) << "seed=" << seed;
    EXPECT_EQ(first.fills.size(), second.fills.size()) << "seed=" << seed;
    EXPECT_EQ(first.orderCount, second.orderCount) << "seed=" << seed;

    if (first.result.ok()) {
      checkFillDifferential(events, seed);
    }
  }
}

// Extended stress: TICKFORGE_EXTENDED=1 enables more seeds and longer
// modification-heavy sequences.
TEST(ModifyAdversarial, ExtendedStress) {
  if (std::getenv("TICKFORGE_EXTENDED") == nullptr) {
    GTEST_SKIP() << "Set TICKFORGE_EXTENDED=1 to run the extended stress profile.";
  }
  std::size_t totalModifies = 0;
  for (std::uint64_t seed = 1; seed <= 10; ++seed) {
    GeneratorConfig config;
    config.seed = seed;
    config.eventCount = 150;
    config.modifyWeight = 50;
    config.newWeight = 30;
    config.cancelWeight = 20;
    const auto events = EventGenerator(config).generate();
    for (const auto& e : events) {
      if (e.type == EventType::ModifyOrder) {
        ++totalModifies;
      }
    }
    checkFillDifferential(events, seed);
  }
  EXPECT_GT(totalModifies, 0U);
}

} // namespace
} // namespace tickforge::test
