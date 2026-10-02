// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Tests for validateEvent(): the explicit, separate validation step a
// parser runs before normalized events reach the replay engine.

#include "tickforge/event/event.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <string_view>

namespace {

using tickforge::Event;
using tickforge::EventType;
using tickforge::EventValidationError;
using tickforge::OrderId;
using tickforge::Price;
using tickforge::Quantity;
using tickforge::Sequence;
using tickforge::Side;
using tickforge::Timestamp;
using tickforge::validateEvent;

Event makeEvent(EventType type) {
  Event event;
  event.timestamp = Timestamp{100};
  event.sequence = Sequence{1};
  event.type = type;
  event.instrument = "AAPL";
  event.orderId = OrderId{7};
  event.side = Side::Bid;
  return event;
}

Event makeNewOrder(Price price, Quantity quantity) {
  Event event = makeEvent(EventType::NewOrder);
  event.price = price;
  event.quantity = quantity;
  return event;
}

TEST(EventValidationTest, ValidNewOrder) {
  EXPECT_EQ(validateEvent(makeNewOrder(Price{100}, Quantity{10})), EventValidationError::Ok);
}

TEST(EventValidationTest, NewOrderRejectsZeroPrice) {
  EXPECT_EQ(validateEvent(makeNewOrder(Price{0}, Quantity{10})), EventValidationError::InvalidPrice);
}

TEST(EventValidationTest, NewOrderRejectsNegativePrice) {
  EXPECT_EQ(validateEvent(makeNewOrder(Price{-1}, Quantity{10})), EventValidationError::InvalidPrice);
}

TEST(EventValidationTest, NewOrderRejectsZeroQuantity) {
  EXPECT_EQ(validateEvent(makeNewOrder(Price{100}, Quantity{0})), EventValidationError::InvalidQuantity);
}

TEST(EventValidationTest, NewOrderRejectsNegativeQuantity) {
  EXPECT_EQ(validateEvent(makeNewOrder(Price{100}, Quantity{-5})), EventValidationError::InvalidQuantity);
}

TEST(EventValidationTest, RejectsEmptyInstrument) {
  Event event = makeNewOrder(Price{100}, Quantity{10});
  event.instrument.clear();
  EXPECT_EQ(validateEvent(event), EventValidationError::EmptyInstrument);
}

TEST(EventValidationTest, FlagsAreIgnored) {
  // SPEC.md 2.2: flags default to 0 and are ignored by consumers.
  Event event = makeNewOrder(Price{100}, Quantity{10});
  event.flags = 0xFFFFFFFFU;
  EXPECT_EQ(validateEvent(event), EventValidationError::Ok);
}

TEST(EventValidationTest, ValidModifyPriceOnly) {
  Event event = makeEvent(EventType::ModifyOrder);
  event.price = Price{101};
  EXPECT_EQ(validateEvent(event), EventValidationError::Ok);
}

TEST(EventValidationTest, ValidModifyQuantityOnly) {
  Event event = makeEvent(EventType::ModifyOrder);
  event.quantity = Quantity{5};
  EXPECT_EQ(validateEvent(event), EventValidationError::Ok);
}

TEST(EventValidationTest, ValidModifyBoth) {
  Event event = makeEvent(EventType::ModifyOrder);
  event.price = Price{101};
  event.quantity = Quantity{5};
  EXPECT_EQ(validateEvent(event), EventValidationError::Ok);
}

TEST(EventValidationTest, ModifyRejectsEmptyChange) {
  const Event event = makeEvent(EventType::ModifyOrder);
  EXPECT_EQ(validateEvent(event), EventValidationError::EmptyModify);
}

TEST(EventValidationTest, ModifyRejectsNegativePrice) {
  Event event = makeEvent(EventType::ModifyOrder);
  event.price = Price{-1};
  event.quantity = Quantity{5};
  EXPECT_EQ(validateEvent(event), EventValidationError::InvalidPrice);
}

TEST(EventValidationTest, ModifyRejectsNegativeQuantity) {
  Event event = makeEvent(EventType::ModifyOrder);
  event.quantity = Quantity{-1};
  EXPECT_EQ(validateEvent(event), EventValidationError::InvalidQuantity);
}

TEST(EventValidationTest, ValidCancelOrder) {
  const Event event = makeEvent(EventType::CancelOrder);
  EXPECT_EQ(validateEvent(event), EventValidationError::Ok);
}

TEST(EventValidationTest, CancelRejectsStalePrice) {
  Event event = makeEvent(EventType::CancelOrder);
  event.price = Price{100};
  EXPECT_EQ(validateEvent(event), EventValidationError::UnexpectedCancelPayload);
}

TEST(EventValidationTest, CancelRejectsStaleQuantity) {
  Event event = makeEvent(EventType::CancelOrder);
  event.quantity = Quantity{10};
  EXPECT_EQ(validateEvent(event), EventValidationError::UnexpectedCancelPayload);
}

TEST(EventValidationTest, ErrorToString) {
  EXPECT_EQ(tickforge::toString(EventValidationError::Ok), std::string_view{"Ok"});
  EXPECT_EQ(tickforge::toString(EventValidationError::InvalidPrice),
            std::string_view{"InvalidPrice"});
  EXPECT_EQ(tickforge::toString(EventValidationError::UnexpectedCancelPayload),
            std::string_view{"UnexpectedCancelPayload"});
}

} // namespace
