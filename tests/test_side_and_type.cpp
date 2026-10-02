// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Tests for the Side and EventType enums.

#include <string_view>

#include <gtest/gtest.h>

#include "tickforge/event/event_type.hpp"
#include "tickforge/event/side.hpp"

namespace {

using tickforge::EventType;
using tickforge::Side;
using tickforge::toString;

TEST(SideTest, BidAndAskAreDistinct) {
  EXPECT_NE(Side::Bid, Side::Ask);
  EXPECT_EQ(Side::Bid, Side::Bid);
  EXPECT_EQ(Side::Ask, Side::Ask);
}

TEST(SideTest, ToString) {
  EXPECT_EQ(toString(Side::Bid), std::string_view{"Bid"});
  EXPECT_EQ(toString(Side::Ask), std::string_view{"Ask"});
}

TEST(SideTest, Opposite) {
  EXPECT_EQ(tickforge::opposite(Side::Bid), Side::Ask);
  EXPECT_EQ(tickforge::opposite(Side::Ask), Side::Bid);
  EXPECT_EQ(tickforge::opposite(tickforge::opposite(Side::Bid)), Side::Bid);
}

TEST(EventTypeTest, ExactlyThreeDistinctTypes) {
  EXPECT_NE(EventType::NewOrder, EventType::ModifyOrder);
  EXPECT_NE(EventType::NewOrder, EventType::CancelOrder);
  EXPECT_NE(EventType::ModifyOrder, EventType::CancelOrder);
  EXPECT_EQ(EventType::NewOrder, EventType::NewOrder);
}

TEST(EventTypeTest, ToString) {
  EXPECT_EQ(toString(EventType::NewOrder), std::string_view{"NewOrder"});
  EXPECT_EQ(toString(EventType::ModifyOrder), std::string_view{"ModifyOrder"});
  EXPECT_EQ(toString(EventType::CancelOrder), std::string_view{"CancelOrder"});
}

}  // namespace
