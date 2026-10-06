// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Market-data integration tests: CSV -> Events -> Replay -> Matching ->
// Analytics. Proves the parser is a clean input boundary producing the
// same results as directly constructed events.

#include "tickforge/analytics/execution_statistics.hpp"
#include "tickforge/analytics/queue_tracker.hpp"
#include "tickforge/book/order_book.hpp"
#include "tickforge/event/event.hpp"
#include "tickforge/market_data/csv_parser.hpp"
#include "tickforge/matching/fill.hpp"
#include "tickforge/matching/matching_engine.hpp"
#include "tickforge/replay/replay.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <vector>

namespace {

using tickforge::CsvParser;
using tickforge::Event;
using tickforge::EventType;
using tickforge::ExecutionStatistics;
using tickforge::Fill;
using tickforge::MatchingEngine;
using tickforge::OrderBook;
using tickforge::OrderId;
using tickforge::ParseError;
using tickforge::Price;
using tickforge::Quantity;
using tickforge::QueueTracker;
using tickforge::Sequence;
using tickforge::Side;
using tickforge::StreamResult;
using tickforge::Timestamp;

// Golden fixture (mirrors testdata/market_data/sample.csv).
const char* sample_csv = "timestamp,event_type,order_id,side,price,quantity\n"
                         "1000000,N,1,B,100,10\n"
                         "1000010,N,2,B,100,5\n"
                         "1000020,N,3,A,101,8\n"
                         "1000030,C,2,B,,\n"
                         "1000040,M,1,B,99,\n"
                         "1000050,N,4,A,99,12\n";

// Directly constructed equivalent of the fixture (no parser).
std::vector<Event> makeDirectEvents() {
  std::vector<Event> events;
  auto add = [&](Timestamp ts,
                 Sequence seq,
                 EventType type,
                 OrderId id,
                 Side side,
                 Price price,
                 Quantity qty) {
    Event e;
    e.timestamp = ts;
    e.sequence = seq;
    e.type = type;
    e.instrument = "AAPL";
    e.orderId = id;
    e.side = side;
    e.price = price;
    e.quantity = qty;
    events.push_back(e);
  };
  add(Timestamp{1000000},
      Sequence{0},
      EventType::NewOrder,
      OrderId{1},
      Side::Bid,
      Price{100},
      Quantity{10});
  add(Timestamp{1000010},
      Sequence{1},
      EventType::NewOrder,
      OrderId{2},
      Side::Bid,
      Price{100},
      Quantity{5});
  add(Timestamp{1000020},
      Sequence{2},
      EventType::NewOrder,
      OrderId{3},
      Side::Ask,
      Price{101},
      Quantity{8});
  add(Timestamp{1000030},
      Sequence{3},
      EventType::CancelOrder,
      OrderId{2},
      Side::Bid,
      Price{0},
      Quantity{0});
  add(Timestamp{1000040},
      Sequence{4},
      EventType::ModifyOrder,
      OrderId{1},
      Side::Bid,
      Price{99},
      Quantity{0});
  add(Timestamp{1000050},
      Sequence{5},
      EventType::NewOrder,
      OrderId{4},
      Side::Ask,
      Price{99},
      Quantity{12});
  return events;
}

struct PipelineResult {
  std::vector<Fill> fills;
  std::size_t orderCount{0};
  std::int64_t totalLots{0};
  std::size_t fillCount{0};
};

// Run events through matching + analytics, collecting outcomes.
PipelineResult runPipeline(const std::vector<Event>& events) {
  PipelineResult out;
  OrderBook book;
  MatchingEngine engine(book);
  QueueTracker tracker;
  ExecutionStatistics stats;
  tracker.designate(OrderId{1});

  for (const Event& e : events) {
    EXPECT_TRUE(engine.onEvent(e));
    tracker.onEvent(e);
    tracker.onFills(engine.fills());
    stats.addFills(engine.fills());
    for (const Fill& f : engine.fills()) {
      out.fills.push_back(f);
    }
  }
  out.orderCount = book.orderCount();
  out.totalLots = stats.totalQuantity().lots();
  out.fillCount = stats.fillCount();
  return out;
}

// Helper: verify dense sequences from 0.
void expectDenseSequences(const std::vector<Event>& events) {
  for (std::size_t i = 0; i < events.size(); ++i) {
    EXPECT_EQ(events[i].sequence.value(), i);
  }
}

TEST(MarketDataIntegrationTest, ParserProducesExpectedEvents) {
  const CsvParser parser{"AAPL"};
  std::istringstream input(sample_csv);
  const StreamResult result = parser.parseStream(input);

  EXPECT_EQ(result.error, ParseError::Ok);
  EXPECT_EQ(result.events.size(), 6U);

  // Spot-check: cancel and modify records.
  EXPECT_EQ(result.events[3].type, EventType::CancelOrder);
  EXPECT_EQ(result.events[3].orderId.value(), 2U);
  EXPECT_EQ(result.events[4].type, EventType::ModifyOrder);
  EXPECT_EQ(result.events[4].price.ticks(), 99);
  expectDenseSequences(result.events);
}

// Helper: verify two event vectors are identical.
void expectEventsEqual(const std::vector<Event>& actual, const std::vector<Event>& expected) {
  EXPECT_EQ(actual.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(actual[i], expected[i]);
  }
}

TEST(MarketDataIntegrationTest, ParsedMatchesDirectConstruction) {
  const CsvParser parser{"AAPL"};
  std::istringstream input(sample_csv);
  const StreamResult result = parser.parseStream(input);
  EXPECT_EQ(result.error, ParseError::Ok);

  expectEventsEqual(result.events, makeDirectEvents());
}

// Helper: verify fill vectors match.
void expectFillsEqual(const std::vector<Fill>& actual, const std::vector<Fill>& expected) {
  EXPECT_EQ(actual.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(actual[i].price, expected[i].price);
    EXPECT_EQ(actual[i].quantity, expected[i].quantity);
  }
}

// Helper: verify two pipeline results match.
void expectPipelineEqual(const PipelineResult& actual, const PipelineResult& expected) {
  EXPECT_EQ(actual.fillCount, expected.fillCount);
  EXPECT_EQ(actual.totalLots, expected.totalLots);
  EXPECT_EQ(actual.orderCount, expected.orderCount);
  expectFillsEqual(actual.fills, expected.fills);
}

TEST(MarketDataIntegrationTest, PipelineEquivalence) {
  const CsvParser parser{"AAPL"};
  std::istringstream input(sample_csv);
  const StreamResult parsed = parser.parseStream(input);
  EXPECT_EQ(parsed.error, ParseError::Ok);

  expectPipelineEqual(runPipeline(parsed.events), runPipeline(makeDirectEvents()));
}

TEST(MarketDataIntegrationTest, FixtureProducesExpectedFills) {
  const CsvParser parser{"AAPL"};
  std::istringstream input(sample_csv);
  const StreamResult parsed = parser.parseStream(input);
  EXPECT_EQ(parsed.error, ParseError::Ok);

  const PipelineResult out = runPipeline(parsed.events);
  // Order 4 (ask 12 @ 99) crosses order 1 (bid 10 @ 99, after modify).
  // One fill: 10 @ 99. Order 4 rests 2 @ 99; order 3 rests 8 @ 101.
  EXPECT_EQ(out.fillCount, 1U);
  EXPECT_EQ(out.totalLots, 10);
  EXPECT_EQ(out.orderCount, 2U);
  EXPECT_EQ(out.fills[0].price.ticks(), 99);
  EXPECT_EQ(out.fills[0].quantity.lots(), 10);
}

TEST(MarketDataIntegrationTest, ReplayTwiceIdentical) {
  const CsvParser parser{"AAPL"};
  const auto runOnce = [&]() {
    std::istringstream input(sample_csv);
    const StreamResult parsed = parser.parseStream(input);
    EXPECT_EQ(parsed.error, ParseError::Ok);
    return runPipeline(parsed.events);
  };
  const PipelineResult first = runOnce();
  const PipelineResult second = runOnce();

  EXPECT_EQ(first.fillCount, second.fillCount);
  EXPECT_EQ(first.totalLots, second.totalLots);
  EXPECT_EQ(first.orderCount, second.orderCount);
}

TEST(MarketDataIntegrationTest, InvalidInputNeverReachesEngine) {
  const CsvParser parser{"AAPL"};
  std::istringstream input("timestamp,event_type,order_id,side,price,quantity\n"
                           "1000000,N,1,B,100,10\n"
                           "1000010,N,2,B,0,5\n"); // invalid price: rejected by parser
  const StreamResult parsed = parser.parseStream(input);

  EXPECT_EQ(parsed.error, ParseError::EventRejected);
  // Only the valid prefix was produced; the bad record never became an event.
  EXPECT_EQ(parsed.events.size(), 1U);

  // The engine only ever sees the valid prefix.
  OrderBook book;
  MatchingEngine engine(book);
  for (const Event& e : parsed.events) {
    EXPECT_TRUE(engine.onEvent(e));
  }
  EXPECT_EQ(book.orderCount(), 1U);
}

TEST(MarketDataIntegrationTest, SourceFieldsPreserved) {
  const CsvParser parser{"AAPL"};
  std::istringstream input("timestamp,event_type,order_id,side,price,quantity\n"
                           "123456789,N,987654321,A,555,77\n");
  const StreamResult parsed = parser.parseStream(input);
  EXPECT_EQ(parsed.error, ParseError::Ok);
  EXPECT_EQ(parsed.events.size(), 1U);

  const Event& e = parsed.events[0];
  EXPECT_EQ(e.timestamp.count(), 123456789);
  EXPECT_EQ(e.orderId.value(), 987654321U);
  EXPECT_EQ(e.side, Side::Ask);
  EXPECT_EQ(e.price.ticks(), 555);
  EXPECT_EQ(e.quantity.lots(), 77);
}

} // namespace
