// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Unit tests for the deterministic replay driver: ordering verification,
// instrument scoping, the validation gate, dispatch, and the strict-mode
// error contract (SPEC.md sections 3.3 and 11).

#include "tickforge/event/event.hpp"
#include "tickforge/replay/event_processor.hpp"
#include "tickforge/replay/replay.hpp"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tickforge::Event;
using tickforge::EventProcessor;
using tickforge::EventType;
using tickforge::EventValidationError;
using tickforge::OrderId;
using tickforge::Price;
using tickforge::Quantity;
using tickforge::ReplayError;
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
  // CancelOrder carries no price or quantity (validateEvent enforces this).
  if (type != EventType::CancelOrder) {
    event.price = Price{100};
    event.quantity = Quantity{10};
  }
  return event;
}

class RecordingProcessor : public EventProcessor {
public:
  bool onEvent(const Event& event) override {
    seen.push_back(event.sequence);
    return true;
  }
  std::vector<Sequence> seen;
};

class RejectingProcessor : public EventProcessor {
public:
  bool onEvent(const Event& event) override {
    (void)event;
    return false;
  }
};

TEST(ReplayTest, EmptySpanReturnsOk) {
  RecordingProcessor processor;
  const std::vector<Event> events;
  const ReplayResult result = tickforge::replayEvents(events, processor);
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.error, ReplayError::Ok);
  EXPECT_TRUE(processor.seen.empty());
}

TEST(ReplayTest, DispatchesInStreamOrder) {
  RecordingProcessor processor;
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
      makeEvent(Timestamp{200}, Sequence{1}, EventType::NewOrder, OrderId{2}),
      makeEvent(Timestamp{300}, Sequence{2}, EventType::CancelOrder, OrderId{1}),
  };
  const ReplayResult result = tickforge::replayEvents(events, processor);
  EXPECT_TRUE(result.ok());
  ASSERT_EQ(processor.seen.size(), 3U);
  EXPECT_EQ(processor.seen[0], Sequence{0});
  EXPECT_EQ(processor.seen[1], Sequence{1});
  EXPECT_EQ(processor.seen[2], Sequence{2});
}

TEST(ReplayTest, SameTimestampDispatchedBySequence) {
  RecordingProcessor processor;
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{1}, EventType::NewOrder, OrderId{1}),
      makeEvent(Timestamp{100}, Sequence{2}, EventType::NewOrder, OrderId{2}),
      makeEvent(Timestamp{100}, Sequence{3}, EventType::NewOrder, OrderId{3}),
  };
  const ReplayResult result = tickforge::replayEvents(events, processor);
  EXPECT_TRUE(result.ok());
  ASSERT_EQ(processor.seen.size(), 3U);
  EXPECT_EQ(processor.seen[0], Sequence{1});
  EXPECT_EQ(processor.seen[1], Sequence{2});
  EXPECT_EQ(processor.seen[2], Sequence{3});
}

TEST(ReplayTest, UnsortedInputIsRejected) {
  RecordingProcessor processor;
  const std::vector<Event> events = {
      makeEvent(Timestamp{200}, Sequence{1}, EventType::NewOrder, OrderId{1}),
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{2}),
  };
  const ReplayResult result = tickforge::replayEvents(events, processor);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error, ReplayError::UnsortedInput);
  EXPECT_EQ(result.failedAt, Sequence{0});
  // The first event was dispatched before the violation was found.
  ASSERT_EQ(processor.seen.size(), 1U);
  EXPECT_EQ(processor.seen[0], Sequence{1});
}

TEST(ReplayTest, DuplicateKeyIsRejected) {
  RecordingProcessor processor;
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{2}),
  };
  const ReplayResult result = tickforge::replayEvents(events, processor);
  EXPECT_EQ(result.error, ReplayError::UnsortedInput);
  EXPECT_EQ(result.failedAt, Sequence{0});
}

TEST(ReplayTest, InvalidEventAbortsWithReason) {
  RecordingProcessor processor;
  std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
      makeEvent(Timestamp{200}, Sequence{1}, EventType::NewOrder, OrderId{2}),
  };
  events[1].quantity = Quantity{0}; // invalid: zero quantity on NewOrder
  const ReplayResult result = tickforge::replayEvents(events, processor);
  EXPECT_EQ(result.error, ReplayError::InvalidEvent);
  EXPECT_EQ(result.failedAt, Sequence{1});
  EXPECT_EQ(result.validationReason, EventValidationError::InvalidQuantity);
  ASSERT_EQ(processor.seen.size(), 1U);
}

TEST(ReplayTest, UnknownOrderOnCancel) {
  RejectingProcessor processor;
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{5}, EventType::CancelOrder, OrderId{99}),
  };
  const ReplayResult result = tickforge::replayEvents(events, processor);
  EXPECT_EQ(result.error, ReplayError::UnknownOrder);
  EXPECT_EQ(result.failedAt, Sequence{5});
}

TEST(ReplayTest, UnknownOrderOnModify) {
  RejectingProcessor processor;
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{6}, EventType::ModifyOrder, OrderId{99}),
  };
  // A modify carrying a price change is otherwise valid.
  const ReplayResult result = tickforge::replayEvents(events, processor);
  EXPECT_EQ(result.error, ReplayError::UnknownOrder);
  EXPECT_EQ(result.failedAt, Sequence{6});
}

TEST(ReplayTest, DuplicateOrderOnNew) {
  RejectingProcessor processor;
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{7}, EventType::NewOrder, OrderId{1}),
  };
  const ReplayResult result = tickforge::replayEvents(events, processor);
  EXPECT_EQ(result.error, ReplayError::DuplicateOrder);
  EXPECT_EQ(result.failedAt, Sequence{7});
}

TEST(ReplayTest, InstrumentMismatchIsRejected) {
  RecordingProcessor processor;
  std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
      makeEvent(Timestamp{200}, Sequence{1}, EventType::NewOrder, OrderId{2}),
  };
  events[1].instrument = "MSFT";
  const ReplayResult result = tickforge::replayEvents(events, processor);
  EXPECT_EQ(result.error, ReplayError::InstrumentMismatch);
  EXPECT_EQ(result.failedAt, Sequence{1});
  ASSERT_EQ(processor.seen.size(), 1U);
}

TEST(ReplayTest, ErrorToString) {
  EXPECT_EQ(tickforge::toString(ReplayError::Ok), std::string_view{"Ok"});
  EXPECT_EQ(tickforge::toString(ReplayError::UnsortedInput), std::string_view{"UnsortedInput"});
  EXPECT_EQ(tickforge::toString(ReplayError::InvalidEvent), std::string_view{"InvalidEvent"});
  EXPECT_EQ(tickforge::toString(ReplayError::UnknownOrder), std::string_view{"UnknownOrder"});
  EXPECT_EQ(tickforge::toString(ReplayError::DuplicateOrder), std::string_view{"DuplicateOrder"});
  EXPECT_EQ(tickforge::toString(ReplayError::InstrumentMismatch),
            std::string_view{"InstrumentMismatch"});
}

} // namespace
