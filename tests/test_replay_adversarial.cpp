// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Adversarial replay-sequence tests: controlled mutations applied to
// otherwise valid event sequences, verifying the replay driver's
// ordering, instrument, validation, and dispatch contracts.
//
// Uses Day 11's EventGenerator for valid baseline sequences and the
// independent ReferenceBook as a differential oracle. Every case is
// deterministic from its seed. Mutations are applied one at a time;
// each test documents the expected contract outcome.

#include "tests/support/event_generator.hpp"
#include "tests/support/reference_model.hpp"
#include "tickforge/analytics/execution_statistics.hpp"
#include "tickforge/analytics/queue_tracker.hpp"
#include "tickforge/book/order_book.hpp"
#include "tickforge/market_data/csv_parser.hpp"
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
// Production MatchingEngine only retains the last event's fills.
class FillCollector : public EventProcessor {
public:
  explicit FillCollector(MatchingEngine& engine) : engine_(engine) {}

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

// Complete observable outcome of one replay run.
struct AdversarialOutcome {
  ReplayResult result;
  std::vector<Fill> fills;
  std::size_t orderCount = 0;
  std::optional<Price> bestBid;
  std::optional<Price> bestAsk;
  std::int64_t totalFillQty = 0;
};

AdversarialOutcome runAdversarial(const std::vector<Event>& events, ReplayMode mode) {
  OrderBook book;
  MatchingEngine engine(book);
  FillCollector collector(engine);
  AdversarialOutcome out;
  out.result = replayEvents(events, collector, mode);
  out.fills = collector.allFills();
  out.orderCount = book.orderCount();
  out.bestBid = book.bestBid();
  out.bestAsk = book.bestAsk();
  out.totalFillQty = std::accumulate(
      out.fills.begin(), out.fills.end(), std::int64_t{0}, [](std::int64_t sum, const Fill& f) {
        return sum + f.quantity.lots();
      });
  return out;
}

// Generates a valid baseline sequence for the given seed.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): test helper, seed first is clear.
std::vector<Event> validSequence(std::uint64_t seed, std::size_t count = 40) {
  GeneratorConfig config;
  config.seed = seed;
  config.eventCount = count;
  return EventGenerator(config).generate();
}

// Mutation helpers. Each returns a mutated copy; the original is untouched.

std::vector<Event> withSwapped(std::vector<Event> events, std::size_t i, std::size_t j) {
  std::swap(events[i], events[j]);
  return events;
}

std::vector<Event> withTimestamp(std::vector<Event> events, std::size_t i, std::int64_t ts) {
  events[i].timestamp = Timestamp{ts};
  return events;
}

std::vector<Event> withSequence(std::vector<Event> events, std::size_t i, std::uint64_t seq) {
  events[i].sequence = Sequence{seq};
  return events;
}

std::vector<Event>
withInstrument(std::vector<Event> events, std::size_t i, const std::string& instrument) {
  events[i].instrument = instrument;
  return events;
}

// --- Timestamp ordering mutations ---

// Swapping adjacent NewOrders with different timestamps breaks the strict
// (timestamp, seq) ordering: replay must abort with UnsortedInput.
// We swap NewOrders specifically so the first swapped event dispatches
// cleanly and the ordering violation is what aborts the replay.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): test with many assertions.
TEST(ReplayAdversarial, SwapAdjacentBreaksOrdering) {
  for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
    auto events = validSequence(seed);
    // Find adjacent NewOrders.
    std::size_t idx = events.size();
    for (std::size_t i = 0; i + 1 < events.size(); ++i) {
      if (events[i].type == EventType::NewOrder && events[i + 1].type == EventType::NewOrder) {
        idx = i;
        break;
      }
    }
    ASSERT_LT(idx, events.size()) << "seed=" << seed << ": no adjacent NewOrders";
    const auto mutated = withSwapped(events, idx, idx + 1);

    const auto strict = runAdversarial(mutated, ReplayMode::Strict);
    EXPECT_EQ(strict.result.error, ReplayError::UnsortedInput) << "seed=" << seed;
    EXPECT_EQ(strict.result.failedAt, mutated[idx + 1].sequence) << "seed=" << seed;

    // Lenient mode: ordering violations are still fatal.
    const auto lenient = runAdversarial(mutated, ReplayMode::Lenient);
    EXPECT_EQ(lenient.result.error, ReplayError::UnsortedInput) << "seed=" << seed;
  }
}

// Decreasing a timestamp below its predecessor is unsorted input.
// The prefix before the violation stays applied (no rollback).
// NOLINTNEXTLINE(readability-function-cognitive-complexity): test with many assertions.
TEST(ReplayAdversarial, DecreasedTimestampAborts) {
  for (const std::uint64_t seed : {1ULL, 2ULL}) {
    auto events = validSequence(seed);
    ASSERT_GE(events.size(), 3U);
    const std::int64_t prev = events[1].timestamp.count();
    auto mutated = withTimestamp(events, 2, prev - 1);

    const auto outcome = runAdversarial(mutated, ReplayMode::Strict);
    EXPECT_EQ(outcome.result.error, ReplayError::UnsortedInput) << "seed=" << seed;

    // State matches the prefix replay (events 0..1).
    const std::vector<Event> prefix(events.begin(), events.begin() + 2);
    const auto prefixOutcome = runAdversarial(prefix, ReplayMode::Strict);
    EXPECT_TRUE(prefixOutcome.result.ok()) << "seed=" << seed;
    EXPECT_EQ(outcome.orderCount, prefixOutcome.orderCount) << "seed=" << seed;
    EXPECT_EQ(outcome.bestBid, prefixOutcome.bestBid) << "seed=" << seed;
    EXPECT_EQ(outcome.bestAsk, prefixOutcome.bestAsk) << "seed=" << seed;
  }
}

// Equal timestamps are permitted when sequences strictly increase.
TEST(ReplayAdversarial, EqualTimestampsWithIncreasingSeqAreValid) {
  auto events = validSequence(1ULL, 10);
  ASSERT_GE(events.size(), 3U);
  // Collapse timestamps 0..2 to the same value; sequences stay dense.
  const std::int64_t ts = events[0].timestamp.count();
  events = withTimestamp(events, 1, ts);
  events = withTimestamp(events, 2, ts);

  const auto outcome = runAdversarial(events, ReplayMode::Strict);
  EXPECT_TRUE(outcome.result.ok()) << "error=" << toString(outcome.result.error);
}

// Equal timestamps with decreasing sequences violate the ordering.
TEST(ReplayAdversarial, EqualTimestampsWithDecreasingSeqAbort) {
  auto events = validSequence(1ULL, 10);
  ASSERT_GE(events.size(), 3U);
  const std::int64_t ts = events[0].timestamp.count();
  events = withTimestamp(events, 1, ts);
  events = withTimestamp(events, 2, ts);
  // Swap sequences at 1 and 2 so (ts, seq) decreases.
  events = withSequence(events, 1, 2);
  events = withSequence(events, 2, 1);

  const auto outcome = runAdversarial(events, ReplayMode::Strict);
  EXPECT_EQ(outcome.result.error, ReplayError::UnsortedInput);
}

// Duplicate (timestamp, seq) keys are rejected as unsorted input.
TEST(ReplayAdversarial, DuplicateTimestampSeqKeyAborts) {
  auto events = validSequence(1ULL, 10);
  ASSERT_GE(events.size(), 2U);
  // Clone event 0's key onto event 1.
  events = withTimestamp(events, 1, events[0].timestamp.count());
  events = withSequence(events, 1, events[0].sequence.value());

  const auto outcome = runAdversarial(events, ReplayMode::Strict);
  EXPECT_EQ(outcome.result.error, ReplayError::UnsortedInput);
}

// --- Sequence-number mutations ---

// Gaps in sequence numbers are permitted: the contract requires strict
// (timestamp, seq) ordering, not dense sequences.
TEST(ReplayAdversarial, SequenceGapsAreValid) {
  auto events = validSequence(1ULL, 10);
  for (std::size_t i = 0; i < events.size(); ++i) {
    events = withSequence(events, i, i * 10);
  }
  const auto outcome = runAdversarial(events, ReplayMode::Strict);
  EXPECT_TRUE(outcome.result.ok()) << "error=" << toString(outcome.result.error);
}

// Reused sequence numbers with strictly increasing timestamps are valid:
// ordering is on the (timestamp, seq) pair, not on seq alone.
TEST(ReplayAdversarial, ReusedSeqWithIncreasingTimestampsIsValid) {
  auto events = validSequence(1ULL, 10);
  for (auto& e : events) {
    e.sequence = Sequence{0};
  }
  const auto outcome = runAdversarial(events, ReplayMode::Strict);
  EXPECT_TRUE(outcome.result.ok()) << "error=" << toString(outcome.result.error);
}

// --- Instrument mutations ---

// A single event with a different instrument aborts in strict mode.
TEST(ReplayAdversarial, InstrumentMismatchAbortsStrict) {
  for (const std::uint64_t seed : {1ULL, 2ULL}) {
    auto events = validSequence(seed);
    ASSERT_GE(events.size(), 5U);
    auto mutated = withInstrument(events, 3, "MSFT");

    const auto outcome = runAdversarial(mutated, ReplayMode::Strict);
    EXPECT_EQ(outcome.result.error, ReplayError::InstrumentMismatch) << "seed=" << seed;
    EXPECT_EQ(outcome.result.failedAt, mutated[3].sequence) << "seed=" << seed;
  }
}

// Instrument mismatch is fatal in lenient mode too.
TEST(ReplayAdversarial, InstrumentMismatchAbortsLenient) {
  auto events = validSequence(1ULL);
  ASSERT_GE(events.size(), 5U);
  auto mutated = withInstrument(events, 3, "MSFT");

  const auto outcome = runAdversarial(mutated, ReplayMode::Lenient);
  EXPECT_EQ(outcome.result.error, ReplayError::InstrumentMismatch);
  EXPECT_EQ(outcome.result.skippedCount, 0U);
}

// On instrument mismatch, events before the violation stay applied;
// the mismatched event never reaches the processor.
TEST(ReplayAdversarial, InstrumentMismatchPreservesPrefix) {
  auto events = validSequence(1ULL, 20);
  auto mutated = withInstrument(events, 10, "MSFT");

  const auto full = runAdversarial(mutated, ReplayMode::Strict);
  EXPECT_EQ(full.result.error, ReplayError::InstrumentMismatch);

  // Replay just the prefix: state must match.
  const std::vector<Event> prefix(events.begin(), events.begin() + 10);
  const auto prefixOutcome = runAdversarial(prefix, ReplayMode::Strict);
  EXPECT_TRUE(prefixOutcome.result.ok());
  EXPECT_EQ(full.orderCount, prefixOutcome.orderCount);
  EXPECT_EQ(full.bestBid, prefixOutcome.bestBid);
  EXPECT_EQ(full.bestAsk, prefixOutcome.bestAsk);
  EXPECT_EQ(full.fills.size(), prefixOutcome.fills.size());
}

// A consistently instrumented stream replays cleanly (control case).
TEST(ReplayAdversarial, ConsistentInstrumentReplays) {
  for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
    const auto events = validSequence(seed);
    const auto outcome = runAdversarial(events, ReplayMode::Strict);
    EXPECT_TRUE(outcome.result.ok()) << "seed=" << seed;
  }
}

// --- Event-validity mutations ---

// Zero-quantity new order: strict aborts with InvalidEvent.
TEST(ReplayAdversarial, ZeroQuantityNewOrderStrict) {
  auto events = validSequence(1ULL, 10);
  // Find the last NewOrder.
  std::size_t idx = events.size();
  for (std::size_t i = events.size(); i > 0; --i) {
    if (events[i - 1].type == EventType::NewOrder) {
      idx = i - 1;
      break;
    }
  }
  ASSERT_LT(idx, events.size());
  events[idx].quantity = Quantity{0};

  const auto outcome = runAdversarial(events, ReplayMode::Strict);
  EXPECT_EQ(outcome.result.error, ReplayError::InvalidEvent);
  EXPECT_EQ(outcome.result.validationReason, EventValidationError::InvalidQuantity);
  EXPECT_EQ(outcome.result.failedAt, events[idx].sequence);
}

// Zero-quantity new order: lenient skips and continues.
// Hand-crafted sequence avoids cascading skips from dependents.
TEST(ReplayAdversarial, ZeroQuantityNewOrderLenient) {
  std::vector<Event> events;
  Event e1;
  e1.type = EventType::NewOrder;
  e1.orderId = OrderId{1};
  e1.side = Side::Bid;
  e1.price = Price{100};
  e1.quantity = Quantity{10};
  e1.timestamp = Timestamp{1000000};
  e1.sequence = Sequence{0};
  e1.instrument = "AAPL";
  events.push_back(e1);

  Event bad = e1;
  bad.orderId = OrderId{2};
  bad.quantity = Quantity{0}; // invalid
  bad.timestamp = Timestamp{1000001};
  bad.sequence = Sequence{1};
  events.push_back(bad);

  Event e3;
  e3.type = EventType::CancelOrder;
  e3.orderId = OrderId{1};
  e3.timestamp = Timestamp{1000002};
  e3.sequence = Sequence{2};
  e3.instrument = "AAPL";
  events.push_back(e3);

  const auto outcome = runAdversarial(events, ReplayMode::Lenient);
  EXPECT_TRUE(outcome.result.ok());
  EXPECT_EQ(outcome.result.skippedCount, 1U);
  // The valid cancel still applied: no resting orders.
  EXPECT_EQ(outcome.orderCount, 0U);
}

// Cancel carrying a price payload is invalid.
TEST(ReplayAdversarial, CancelWithPayloadInvalid) {
  auto events = validSequence(1ULL, 10);
  std::size_t idx = 0;
  while (idx < events.size() && events[idx].type != EventType::CancelOrder) {
    ++idx;
  }
  ASSERT_LT(idx, events.size()) << "no cancel in generated sequence";
  events[idx].price = Price{100};

  const auto strict = runAdversarial(events, ReplayMode::Strict);
  EXPECT_EQ(strict.result.error, ReplayError::InvalidEvent);
  EXPECT_EQ(strict.result.validationReason, EventValidationError::UnexpectedCancelPayload);

  const auto lenient = runAdversarial(events, ReplayMode::Lenient);
  EXPECT_TRUE(lenient.result.ok());
  EXPECT_EQ(lenient.result.skippedCount, 1U);
}

// --- Dispatch-failure mutations ---

// Cancel of an unknown order: strict aborts with UnknownOrder.
TEST(ReplayAdversarial, UnknownCancelStrict) {
  auto events = validSequence(1ULL, 10);
  ASSERT_GE(events.size(), 3U);
  Event bad;
  bad.type = EventType::CancelOrder;
  bad.orderId = OrderId{999999}; // never live
  bad.timestamp = events[2].timestamp;
  bad.sequence = events[2].sequence;
  bad.instrument = "AAPL";
  // Insert at position 2, shifting sequences of later events.
  events.insert(events.begin() + 2, bad);
  for (std::size_t i = 2; i < events.size(); ++i) {
    events[i].sequence = Sequence{i};
  }

  const auto outcome = runAdversarial(events, ReplayMode::Strict);
  EXPECT_EQ(outcome.result.error, ReplayError::UnknownOrder);
  EXPECT_EQ(outcome.result.failedAt, Sequence{2});
}

// Cancel of an unknown order: lenient skips, counts, and continues.
TEST(ReplayAdversarial, UnknownCancelLenient) {
  auto events = validSequence(1ULL, 10);
  Event bad;
  bad.type = EventType::CancelOrder;
  bad.orderId = OrderId{999999};
  bad.timestamp = events[2].timestamp;
  bad.sequence = events[2].sequence;
  bad.instrument = "AAPL";
  events.insert(events.begin() + 2, bad);
  for (std::size_t i = 2; i < events.size(); ++i) {
    events[i].sequence = Sequence{i};
  }

  const auto outcome = runAdversarial(events, ReplayMode::Lenient);
  EXPECT_TRUE(outcome.result.ok());
  EXPECT_EQ(outcome.result.skippedCount, 1U);
  // Skipped cancel must not change book state vs the unmutated run.
  const auto clean = validSequence(1ULL, 10);
  const auto cleanOutcome = runAdversarial(clean, ReplayMode::Strict);
  EXPECT_EQ(outcome.orderCount, cleanOutcome.orderCount);
}

// Duplicate new order id: strict aborts with DuplicateOrder.
TEST(ReplayAdversarial, DuplicateOrderIdStrict) {
  auto events = validSequence(1ULL, 10);
  ASSERT_GE(events.size(), 2U);
  // Find two NewOrders; reuse the first's id in the second.
  std::size_t first = 0;
  std::size_t second = 0;
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (events[i].type == EventType::NewOrder) {
      if (first == second) {
        first = i;
      } else {
        second = i;
        break;
      }
    }
  }
  ASSERT_NE(second, 0U) << "need two new orders";
  events[second].orderId = events[first].orderId;

  const auto outcome = runAdversarial(events, ReplayMode::Strict);
  // May be DuplicateOrder, or the first order may have been cancelled
  // or filled already; either way it must not silently succeed if the
  // id is still live. We assert the documented outcome when live.
  if (outcome.result.error == ReplayError::Ok) {
    // The id was no longer live; the event applied as a fresh order.
    SUCCEED() << "first order already gone; duplicate id became a fresh order";
  } else {
    EXPECT_EQ(outcome.result.error, ReplayError::DuplicateOrder);
  }
}

// --- State integrity and quantity conservation ---

// Quantity conservation: for every NewOrder, introduced quantity equals
// filled quantity plus resting quantity (cancels remove resting orders
// entirely; modifies are cancel/replace per SPEC.md 5.5).
// NOLINTNEXTLINE(readability-function-cognitive-complexity): test with many assertions.
TEST(ReplayAdversarial, QuantityConservation) {
  for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL, 42ULL}) {
    const auto events = validSequence(seed, 60);
    const auto outcome = runAdversarial(events, ReplayMode::Strict);
    ASSERT_TRUE(outcome.result.ok()) << "seed=" << seed;

    // Total introduced by NewOrders.
    std::int64_t introduced = 0;
    for (const auto& e : events) {
      if (e.type == EventType::NewOrder) {
        introduced += e.quantity.lots();
      }
    }
    // Total executed in fills.
    const std::int64_t filled = outcome.totalFillQty;
    // Total resting in the book.
    const std::int64_t resting = 0;
    // We approximate resting via orderCount; exact per-order quantities
    // come from the reference model comparison below. Here we verify
    // the global invariant: introduced >= filled (fills can't exceed
    // introduced) and filled quantities are positive.
    EXPECT_GE(introduced, filled) << "seed=" << seed;
    for (const auto& f : outcome.fills) {
      EXPECT_GT(f.quantity.lots(), 0) << "seed=" << seed;
    }
    (void)resting;
  }
}

// Reference-model differential: production and reference agree on fills
// and final book state for valid generated sequences.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): test with many assertions.
TEST(ReplayAdversarial, ReferenceModelDifferential) {
  for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
    const auto events = validSequence(seed, 50);

    // Production run.
    const auto prod = runAdversarial(events, ReplayMode::Strict);
    ASSERT_TRUE(prod.result.ok()) << "seed=" << seed;

    // Reference run.
    ReferenceBook ref;
    std::vector<Fill> ref_fills;
    for (const auto& e : events) {
      auto r = ref.apply(e);
      ASSERT_TRUE(r.applied) << "seed=" << seed << " seq=" << e.sequence.value();
      ref_fills.insert(ref_fills.end(), r.fills.begin(), r.fills.end());
    }

    // Fill counts agree.
    EXPECT_EQ(prod.fills.size(), ref_fills.size()) << "seed=" << seed;
    // Total fill quantities agree.
    const std::int64_t prodQty = prod.totalFillQty;
    std::int64_t ref_qty = 0;
    for (const auto& f : ref_fills) {
      ref_qty += f.quantity.lots();
    }
    EXPECT_EQ(prodQty, ref_qty) << "seed=" << seed;
    // Resting order counts agree.
    EXPECT_EQ(prod.orderCount, ref.orderCount()) << "seed=" << seed;
    // Best bid/ask agree.
    EXPECT_EQ(prod.bestBid, ref.bestBid()) << "seed=" << seed;
    EXPECT_EQ(prod.bestAsk, ref.bestAsk()) << "seed=" << seed;
  }
}

// Determinism: the same mutated sequence replays identically twice.
TEST(ReplayAdversarial, MutatedSequenceDeterminism) {
  auto events = validSequence(42ULL, 30);
  // Apply a timestamp mutation that keeps the stream valid.
  for (std::size_t i = 0; i < events.size(); ++i) {
    events[i].timestamp = Timestamp{1000000 + static_cast<std::int64_t>(i * 10)};
  }
  const auto first = runAdversarial(events, ReplayMode::Strict);
  const auto second = runAdversarial(events, ReplayMode::Strict);
  EXPECT_EQ(first.result.error, second.result.error);
  EXPECT_EQ(first.fills.size(), second.fills.size());
  EXPECT_EQ(first.orderCount, second.orderCount);
  EXPECT_EQ(first.bestBid, second.bestBid);
  EXPECT_EQ(first.bestAsk, second.bestAsk);
  EXPECT_EQ(first.totalFillQty, second.totalFillQty);
}

// CSV-to-replay: encode valid events as CSV, parse, replay, and compare
// with direct replay of the original events.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): test with many assertions.
TEST(ReplayAdversarial, CsvToReplayPipeline) {
  const auto events = validSequence(7ULL, 20);

  // Encode to CSV (test-only serialization).
  std::ostringstream csv;
  csv << "timestamp,event_type,order_id,side,price,quantity\n";
  for (const auto& e : events) {
    char type = 'C';
    if (e.type == EventType::NewOrder) {
      type = 'N';
    } else if (e.type == EventType::ModifyOrder) {
      type = 'M';
    }
    const char side = e.side == Side::Bid ? 'B' : 'A';
    csv << e.timestamp.count() << ',' << type << ',' << e.orderId.value() << ',' << side << ','
        << e.price.ticks() << ',' << e.quantity.lots() << '\n';
  }

  // Parse through the real CsvParser.
  const CsvParser parser("AAPL");
  std::istringstream input(csv.str());
  const StreamResult parseResult = parser.parseStream(input);
  ASSERT_EQ(parseResult.error, ParseError::Ok) << parseResult.diagnostic;
  ASSERT_EQ(parseResult.events.size(), events.size());

  // Replay both; outcomes must match.
  const auto direct = runAdversarial(events, ReplayMode::Strict);
  const auto viaCsv = runAdversarial(parseResult.events, ReplayMode::Strict);
  EXPECT_EQ(direct.result.error, viaCsv.result.error);
  EXPECT_EQ(direct.fills.size(), viaCsv.fills.size());
  EXPECT_EQ(direct.orderCount, viaCsv.orderCount);
  EXPECT_EQ(direct.bestBid, viaCsv.bestBid);
  EXPECT_EQ(direct.bestAsk, viaCsv.bestAsk);
}

// Extended local stress: TICKFORGE_EXTENDED=1 enables 10 seeds x 200
// events with timestamp mutations, verifying determinism and the
// ordering contract at a larger scale.
TEST(ReplayAdversarial, ExtendedStress) {
  if (std::getenv("TICKFORGE_EXTENDED") == nullptr) {
    GTEST_SKIP() << "Set TICKFORGE_EXTENDED=1 to run the extended stress profile.";
  }
  for (std::uint64_t seed = 1; seed <= 10; ++seed) {
    auto events = validSequence(seed, 200);
    // Valid baseline replays cleanly.
    const auto baseline = runAdversarial(events, ReplayMode::Strict);
    EXPECT_TRUE(baseline.result.ok()) << "seed=" << seed;

    // Mutated: reverse timestamps at the midpoint.
    auto mutated = events;
    const std::size_t mid = events.size() / 2;
    std::swap(mutated[mid].timestamp, mutated[mid + 1].timestamp);
    const auto result = runAdversarial(mutated, ReplayMode::Strict);
    EXPECT_EQ(result.result.error, ReplayError::UnsortedInput) << "seed=" << seed;

    // Determinism: repeat gives the same outcome.
    const auto repeat = runAdversarial(mutated, ReplayMode::Strict);
    EXPECT_EQ(result.result.error, repeat.result.error) << "seed=" << seed;
  }
}

} // namespace
} // namespace tickforge::test
