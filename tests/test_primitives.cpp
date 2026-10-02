// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Tests for the strongly typed domain primitives: Timestamp, Sequence,
// OrderId, Price, Quantity. These types are pure values; semantic
// validation lives in validateEvent(), not in the constructors.

#include "tickforge/event/types.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <unordered_set>

namespace {

using tickforge::OrderId;
using tickforge::Price;
using tickforge::Quantity;
using tickforge::Sequence;
using tickforge::Timestamp;

TEST(TimestampTest, ConstructionRoundTrips) {
  const Timestamp ts{1234567890};
  EXPECT_EQ(ts.count(), 1234567890);
}

TEST(TimestampTest, DefaultIsZero) {
  EXPECT_EQ(Timestamp{}.count(), 0);
}

TEST(TimestampTest, NegativeValuesRepresentPreEpoch) {
  const Timestamp ts{-1};
  EXPECT_EQ(ts.count(), -1);
  EXPECT_LT(ts, Timestamp{0});
}

TEST(TimestampTest, EqualityAndInequality) {
  EXPECT_EQ(Timestamp{100}, Timestamp{100});
  EXPECT_NE(Timestamp{100}, Timestamp{101});
  EXPECT_FALSE(Timestamp{100} == Timestamp{101});
}

TEST(TimestampTest, Ordering) {
  EXPECT_LT(Timestamp{99}, Timestamp{100});
  EXPECT_GT(Timestamp{100}, Timestamp{99});
  EXPECT_LE(Timestamp{100}, Timestamp{100});
  EXPECT_GE(Timestamp{100}, Timestamp{100});
}

TEST(TimestampTest, MinMaxBoundaries) {
  const auto minNs = (std::numeric_limits<std::int64_t>::min)();
  const auto maxNs = (std::numeric_limits<std::int64_t>::max)();
  EXPECT_EQ(Timestamp::min().count(), minNs);
  EXPECT_EQ(Timestamp::max().count(), maxNs);
  EXPECT_LT(Timestamp::min(), Timestamp::max());
  EXPECT_LT(Timestamp::min(), Timestamp{0});
  EXPECT_GT(Timestamp::max(), Timestamp{0});
}

TEST(TimestampTest, CopiesCompareEqual) {
  const Timestamp original{42};
  const Timestamp copy{original};
  EXPECT_EQ(original, copy);
}

TEST(SequenceTest, ConstructionRoundTrips) {
  const Sequence seq{7};
  EXPECT_EQ(seq.value(), 7u);
}

TEST(SequenceTest, DefaultIsZero) {
  EXPECT_EQ(Sequence{}.value(), 0u);
}

TEST(SequenceTest, EqualityAndOrdering) {
  EXPECT_EQ(Sequence{3}, Sequence{3});
  EXPECT_NE(Sequence{3}, Sequence{4});
  EXPECT_LT(Sequence{3}, Sequence{4});
  EXPECT_GT(Sequence{4}, Sequence{3});
}

TEST(SequenceTest, MaxBoundary) {
  const auto maxSeq = (std::numeric_limits<std::uint64_t>::max)();
  EXPECT_EQ(Sequence::max().value(), maxSeq);
  EXPECT_GT(Sequence::max(), Sequence{0});
}

TEST(OrderIdTest, ConstructionRoundTrips) {
  const OrderId id{12345};
  EXPECT_EQ(id.value(), 12345u);
}

TEST(OrderIdTest, EqualityOrderingAndHash) {
  EXPECT_EQ(OrderId{1}, OrderId{1});
  EXPECT_NE(OrderId{1}, OrderId{2});
  EXPECT_LT(OrderId{1}, OrderId{2});
}

TEST(OrderIdTest, UsableAsUnorderedKey) {
  std::unordered_set<OrderId> ids;
  ids.insert(OrderId{1});
  ids.insert(OrderId{2});
  ids.insert(OrderId{1});
  EXPECT_EQ(ids.size(), 2u);
  EXPECT_TRUE(ids.contains(OrderId{1}));
  EXPECT_FALSE(ids.contains(OrderId{99}));
}

TEST(PriceTest, IntegerTicksRoundTrip) {
  const Price price{2500};
  EXPECT_EQ(price.ticks(), 2500);
}

TEST(PriceTest, DefaultIsZeroTicks) {
  EXPECT_EQ(Price{}.ticks(), 0);
}

TEST(PriceTest, EqualityAndOrdering) {
  EXPECT_EQ(Price{100}, Price{100});
  EXPECT_NE(Price{100}, Price{101});
  EXPECT_LT(Price{99}, Price{100});
  EXPECT_GT(Price{101}, Price{100});
}

TEST(PriceTest, TypeItselfDoesNotValidate) {
  // Negative and zero prices are representable values; rejecting them is
  // the explicit job of validateEvent(), keeping construction separate
  // from validation.
  EXPECT_EQ(Price{-5}.ticks(), -5);
  EXPECT_EQ(Price{0}.ticks(), 0);
}

TEST(QuantityTest, IntegerLotsRoundTrip) {
  const Quantity qty{300};
  EXPECT_EQ(qty.lots(), 300);
}

TEST(QuantityTest, DefaultIsZeroLots) {
  EXPECT_EQ(Quantity{}.lots(), 0);
}

TEST(QuantityTest, EqualityAndOrdering) {
  EXPECT_EQ(Quantity{10}, Quantity{10});
  EXPECT_NE(Quantity{10}, Quantity{11});
  EXPECT_LT(Quantity{10}, Quantity{11});
  EXPECT_GT(Quantity{11}, Quantity{10});
}

TEST(QuantityTest, TypeItselfDoesNotValidate) {
  EXPECT_EQ(Quantity{-1}.lots(), -1);
}

} // namespace
