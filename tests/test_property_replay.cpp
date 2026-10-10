// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Property tests for replay: determinism over generated sequences,
// strict-mode abort semantics, and lenient-mode skip semantics with
// generated negative events spliced into valid sequences.
//
// The invalid events used here target documented rejection behavior
// only (SPEC.md 11, replay.hpp): unknown cancel/modify, invalid
// quantity, duplicate live order id.

#include "tests/support/event_generator.hpp"
#include "tickforge/analytics/execution_statistics.hpp"
#include "tickforge/analytics/queue_tracker.hpp"
#include "tickforge/book/order_book.hpp"
#include "tickforge/event/event.hpp"
#include "tickforge/matching/fill.hpp"
#include "tickforge/matching/matching_engine.hpp"
#include "tickforge/replay/replay.hpp"

#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <vector>

namespace tickforge::test {
namespace {

struct ReplayOutcome {
  ReplayResult result;
  std::vector<Fill> fills;
  std::size_t orderCount;
  std::optional<Price> bestBid;
  std::optional<Price> bestAsk;
};

ReplayOutcome runReplay(const std::vector<Event>& events, ReplayMode mode) {
  OrderBook book;
  MatchingEngine engine(book);
  QueueTracker tracker;
  ExecutionStatistics stats;
  ReplayOutcome outcome;
  outcome.result = replayEvents(events, engine, mode);
  outcome.fills = engine.fills();
  // Note: engine.fills() only retains the last event's fills; the
  // determinism comparison below focuses on result + book state.
  outcome.orderCount = book.orderCount();
  outcome.bestBid = book.bestBid();
  outcome.bestAsk = book.bestAsk();
  (void)tracker;
  (void)stats;
  return outcome;
}

std::string describeOutcome(const ReplayOutcome& o) {
  std::ostringstream out;
  out << "error=" << static_cast<int>(o.result.error) << " skipped=" << o.result.skippedCount
      << " orders=" << o.orderCount;
  return out.str();
}

// Strict replay of a generated valid sequence is deterministic: two
// fresh runs agree on result metadata and final book state.
TEST(ReplayProperty, StrictDeterminism) {
  for (const std::uint64_t seed : {1, 2, 3, 42, 7, 99}) {
    GeneratorConfig config;
    config.seed = seed;
    config.eventCount = 80;
    const auto events = EventGenerator(config).generate();
    const auto first = runReplay(events, ReplayMode::Strict);
    const auto second = runReplay(events, ReplayMode::Strict);
    EXPECT_TRUE(first.result.ok()) << "seed=" << seed;
    EXPECT_TRUE(second.result.ok()) << "seed=" << seed;
    EXPECT_EQ(describeOutcome(first), describeOutcome(second)) << "seed=" << seed;
    EXPECT_EQ(first.bestBid, second.bestBid) << "seed=" << seed;
    EXPECT_EQ(first.bestAsk, second.bestAsk) << "seed=" << seed;
  }
}

Event makeInvalidCancel(std::uint64_t id, std::size_t seq, std::int64_t ts) {
  Event e;
  e.type = EventType::CancelOrder;
  e.orderId = OrderId{id}; // never a live id
  e.timestamp = Timestamp{ts};
  e.sequence = Sequence{seq};
  e.instrument = "AAPL";
  return e;
}

Event makeZeroQuantityNew(std::uint64_t id, std::size_t seq, std::int64_t ts) {
  Event e;
  e.type = EventType::NewOrder;
  e.orderId = OrderId{id};
  e.side = Side::Bid;
  e.price = Price{100};
  e.quantity = Quantity{0}; // invalid
  e.timestamp = Timestamp{ts};
  e.sequence = Sequence{seq};
  e.instrument = "AAPL";
  return e;
}

// Lenient mode skips invalid events, counts them, and still processes
// the valid events around them.
TEST(ReplayProperty, LenientSkipsAndCounts) {
  GeneratorConfig config;
  config.seed = 5;
  config.eventCount = 40;
  auto events = EventGenerator(config).generate();

  // Splice invalid events at deterministic positions. They need valid
  // timestamps/seqs to keep the stream ordered; use gaps between neighbors.
  std::vector<Event> mixed;
  std::size_t invalidCount = 0;
  for (std::size_t i = 0; i < events.size(); ++i) {
    mixed.push_back(events[i]);
    if (i % 10 == 9) {
      const std::int64_t ts = events[i].timestamp.count() + 1;
      mixed.push_back(makeInvalidCancel(9000 + invalidCount, 0, ts));
      ++invalidCount;
    }
  }
  // Reassign dense seqs and strictly increasing timestamps.
  for (std::size_t i = 0; i < mixed.size(); ++i) {
    mixed[i].sequence = Sequence{i};
    mixed[i].timestamp = Timestamp{2'000'000 + static_cast<std::int64_t>(i)};
  }

  OrderBook book;
  MatchingEngine engine(book);
  const auto result = replayEvents(mixed, engine, ReplayMode::Lenient);
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.mode, ReplayMode::Lenient);
  EXPECT_EQ(result.skippedCount, invalidCount);

  // The valid events were all processed: compare against replaying the
  // valid subsequence alone in strict mode.
  std::vector<Event> validOnly;
  for (const auto& e : events) {
    validOnly.push_back(e);
  }
  for (std::size_t i = 0; i < validOnly.size(); ++i) {
    validOnly[i].sequence = Sequence{i};
    validOnly[i].timestamp = Timestamp{2'000'000 + static_cast<std::int64_t>(i) * 2};
  }
  OrderBook refBook;
  MatchingEngine refEngine(refBook);
  const auto refResult = replayEvents(validOnly, refEngine, ReplayMode::Strict);
  EXPECT_TRUE(refResult.ok());
  EXPECT_EQ(book.orderCount(), refBook.orderCount());
  EXPECT_EQ(book.bestBid(), refBook.bestBid());
  EXPECT_EQ(book.bestAsk(), refBook.bestAsk());
}

// Strict mode aborts on the first invalid event with the documented error.
TEST(ReplayProperty, StrictAbortsOnInvalid) {
  GeneratorConfig config;
  config.seed = 6;
  config.eventCount = 20;
  auto events = EventGenerator(config).generate();
  // Append an unknown cancel after the valid prefix.
  Event bad = makeInvalidCancel(9999, events.size(), 3'000'000);
  events.push_back(bad);

  OrderBook book;
  MatchingEngine engine(book);
  const auto result = replayEvents(events, engine, ReplayMode::Strict);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error, ReplayError::UnknownOrder);
  EXPECT_EQ(result.failedAt, bad.sequence);
}

// Zero-quantity NewOrder fails validation: InvalidEvent in strict,
// skipped in lenient.
TEST(ReplayProperty, InvalidQuantityHandling) {
  GeneratorConfig config;
  config.seed = 7;
  config.eventCount = 10;
  auto events = EventGenerator(config).generate();
  Event bad = makeZeroQuantityNew(8888, events.size(), 3'000'000);
  events.push_back(bad);

  {
    OrderBook book;
    MatchingEngine engine(book);
    const auto r = replayEvents(events, engine, ReplayMode::Strict);
    EXPECT_EQ(r.error, ReplayError::InvalidEvent);
    EXPECT_EQ(r.validationReason, EventValidationError::InvalidQuantity);
  }
  {
    OrderBook book;
    MatchingEngine engine(book);
    const auto r = replayEvents(events, engine, ReplayMode::Lenient);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.skippedCount, 1u);
  }
}

} // namespace
} // namespace tickforge::test
