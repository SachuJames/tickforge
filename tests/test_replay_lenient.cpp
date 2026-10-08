// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Lenient replay mode tests (SPEC.md section 11): invalid events are
// skipped, counted, and reported; the mode is explicitly selected and
// recorded in the result. Stream-level invariants (ordering, instrument
// scoping) still abort in lenient mode.

#include "tickforge/book/order_book.hpp"
#include "tickforge/event/event.hpp"
#include "tickforge/replay/event_processor.hpp"
#include "tickforge/replay/replay.hpp"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using tickforge::Event;
using tickforge::EventProcessor;
using tickforge::EventType;
using tickforge::OrderId;
using tickforge::Price;
using tickforge::Quantity;
using tickforge::ReplayError;
using tickforge::ReplayMode;
using tickforge::ReplayResult;
using tickforge::Sequence;
using tickforge::Side;
using tickforge::Timestamp;

Event makeEvent(Timestamp timestamp, Sequence seq, EventType type, OrderId id) {
  Event event;
  event.timestamp = timestamp;
  event.sequence = seq;
  event.type = type;
  event.instrument = "AAPL";
  event.orderId = id;
  event.side = Side::Bid;
  if (type != EventType::CancelOrder) {
    event.price = Price{100};
    event.quantity = Quantity{10};
  }
  return event;
}

// Processor that accepts everything and records what it saw.
class RecordingProcessor : public EventProcessor {
public:
  bool onEvent(const Event& event) override {
    seen.push_back(event.sequence);
    return true;
  }
  std::vector<Sequence> seen;
};

// Processor that rejects CancelOrder (simulates unknown-order dispatch
// failure) but accepts the rest.
class CancelRejectingProcessor : public EventProcessor {
public:
  bool onEvent(const Event& event) override {
    if (event.type == EventType::CancelOrder) {
      return false;
    }
    seen.push_back(event.sequence);
    return true;
  }
  std::vector<Sequence> seen;
};

TEST(ReplayLenientTest, AllValidEventsNoSkips) {
  RecordingProcessor processor;
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
      makeEvent(Timestamp{200}, Sequence{1}, EventType::NewOrder, OrderId{2}),
  };
  const ReplayResult result = tickforge::replayEvents(events, processor, ReplayMode::Lenient);

  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.error, ReplayError::Ok);
  EXPECT_EQ(result.skippedCount, 0U);
  EXPECT_EQ(result.mode, ReplayMode::Lenient);
  EXPECT_EQ(processor.seen.size(), 2U);
}

TEST(ReplayLenientTest, SkipsInvalidValidationEvent) {
  RecordingProcessor processor;
  // Second event has zero quantity: fails validateEvent().
  Event bad = makeEvent(Timestamp{200}, Sequence{1}, EventType::NewOrder, OrderId{2});
  bad.quantity = Quantity{0};
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
      bad,
      makeEvent(Timestamp{300}, Sequence{2}, EventType::NewOrder, OrderId{3}),
  };
  const ReplayResult result = tickforge::replayEvents(events, processor, ReplayMode::Lenient);

  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.skippedCount, 1U);
  EXPECT_EQ(result.mode, ReplayMode::Lenient);
  // The bad event never reached the processor; the stream continued.
  EXPECT_EQ(processor.seen.size(), 2U);
  EXPECT_EQ(processor.seen[0], Sequence{0});
  EXPECT_EQ(processor.seen[1], Sequence{2});
}

TEST(ReplayLenientTest, SkipsDispatchRejection) {
  CancelRejectingProcessor processor;
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
      makeEvent(Timestamp{200}, Sequence{1}, EventType::CancelOrder, OrderId{1}),
      makeEvent(Timestamp{300}, Sequence{2}, EventType::NewOrder, OrderId{2}),
  };
  const ReplayResult result = tickforge::replayEvents(events, processor, ReplayMode::Lenient);

  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.skippedCount, 1U);
  EXPECT_EQ(processor.seen.size(), 2U);
}

TEST(ReplayLenientTest, CountsMultipleSkips) {
  RecordingProcessor processor;
  Event bad1 = makeEvent(Timestamp{200}, Sequence{1}, EventType::NewOrder, OrderId{2});
  bad1.quantity = Quantity{0};
  Event bad2 = makeEvent(Timestamp{300}, Sequence{2}, EventType::NewOrder, OrderId{3});
  bad2.price = Price{-5};
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
      bad1,
      bad2,
      makeEvent(Timestamp{400}, Sequence{3}, EventType::NewOrder, OrderId{4}),
  };
  const ReplayResult result = tickforge::replayEvents(events, processor, ReplayMode::Lenient);

  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.skippedCount, 2U);
  EXPECT_EQ(processor.seen.size(), 2U);
}

TEST(ReplayLenientTest, AllInvalidStillOk) {
  RecordingProcessor processor;
  Event bad = makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1});
  bad.quantity = Quantity{0};
  const std::vector<Event> events = {bad};
  const ReplayResult result = tickforge::replayEvents(events, processor, ReplayMode::Lenient);

  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.skippedCount, 1U);
  EXPECT_TRUE(processor.seen.empty());
}

TEST(ReplayLenientTest, UnsortedInputStillAborts) {
  RecordingProcessor processor;
  const std::vector<Event> events = {
      makeEvent(Timestamp{200}, Sequence{1}, EventType::NewOrder, OrderId{1}),
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{2}),
  };
  const ReplayResult result = tickforge::replayEvents(events, processor, ReplayMode::Lenient);

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error, ReplayError::UnsortedInput);
  EXPECT_EQ(result.failedAt, Sequence{0});
}

TEST(ReplayLenientTest, InstrumentMismatchStillAborts) {
  RecordingProcessor processor;
  Event other = makeEvent(Timestamp{200}, Sequence{1}, EventType::NewOrder, OrderId{2});
  other.instrument = "MSFT";
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
      other,
  };
  const ReplayResult result = tickforge::replayEvents(events, processor, ReplayMode::Lenient);

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error, ReplayError::InstrumentMismatch);
}

TEST(ReplayLenientTest, StrictModeUnchangedByDefault) {
  RecordingProcessor processor;
  Event bad = makeEvent(Timestamp{200}, Sequence{1}, EventType::NewOrder, OrderId{2});
  bad.quantity = Quantity{0};
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
      bad,
  };
  // No mode argument: strict is the default.
  const ReplayResult result = tickforge::replayEvents(events, processor);

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error, ReplayError::InvalidEvent);
  EXPECT_EQ(result.mode, ReplayMode::Strict);
  EXPECT_EQ(result.skippedCount, 0U);
  EXPECT_EQ(processor.seen.size(), 1U);
}

TEST(ReplayLenientTest, DeterministicAcrossRuns) {
  const auto runOnce = []() {
    RecordingProcessor processor;
    Event bad = makeEvent(Timestamp{200}, Sequence{1}, EventType::NewOrder, OrderId{2});
    bad.quantity = Quantity{0};
    const std::vector<Event> events = {
        makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
        bad,
        makeEvent(Timestamp{300}, Sequence{2}, EventType::NewOrder, OrderId{3}),
    };
    const ReplayResult result = tickforge::replayEvents(events, processor, ReplayMode::Lenient);
    return std::make_pair(result.skippedCount, processor.seen);
  };

  const auto first = runOnce();
  const auto second = runOnce();
  EXPECT_EQ(first.first, second.first);
  EXPECT_EQ(first.second, second.second);
}

TEST(ReplayLenientTest, ModeToString) {
  EXPECT_EQ(tickforge::toString(ReplayMode::Strict), "Strict");
  EXPECT_EQ(tickforge::toString(ReplayMode::Lenient), "Lenient");
}

TEST(ReplayLenientTest, IntegrationWithOrderBook) {
  // Duplicate NewOrder id: syntactically valid, passes validateEvent,
  // but the book rejects it. Strict aborts; lenient skips and continues.
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
      makeEvent(Timestamp{200}, Sequence{1}, EventType::NewOrder, OrderId{1}),
      makeEvent(Timestamp{300}, Sequence{2}, EventType::NewOrder, OrderId{2}),
  };

  tickforge::OrderBook strict_book;
  const ReplayResult strict_result = tickforge::replayEvents(events, strict_book);
  EXPECT_FALSE(strict_result.ok());
  EXPECT_EQ(strict_result.error, ReplayError::DuplicateOrder);
  EXPECT_EQ(strict_book.orderCount(), 1U);

  tickforge::OrderBook lenient_book;
  const ReplayResult lenient_result =
      tickforge::replayEvents(events, lenient_book, ReplayMode::Lenient);
  EXPECT_TRUE(lenient_result.ok());
  EXPECT_EQ(lenient_result.skippedCount, 1U);
  EXPECT_EQ(lenient_result.mode, ReplayMode::Lenient);
  EXPECT_EQ(lenient_book.orderCount(), 2U);
  EXPECT_TRUE(lenient_book.contains(OrderId{1}));
  EXPECT_TRUE(lenient_book.contains(OrderId{2}));
}

} // namespace
