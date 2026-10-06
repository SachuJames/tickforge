// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// CsvParser unit tests: valid records, syntax errors, numeric errors,
// semantic rejection via validateEvent, and determinism.

#include "tickforge/event/event.hpp"
#include "tickforge/market_data/csv_parser.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

namespace {

using tickforge::CsvParser;
using tickforge::EventType;
using tickforge::ParseError;
using tickforge::ParseResult;
using tickforge::Sequence;
using tickforge::Side;
using tickforge::StreamResult;

CsvParser makeParser() {
  return CsvParser{"AAPL"};
}

TEST(CsvParserTest, ValidNewOrderBid) {
  const CsvParser parser = makeParser();
  const ParseResult result = parser.parseRecord("1000000,N,101,B,10000,5", Sequence{0});

  EXPECT_EQ(result.error, ParseError::Ok);
  EXPECT_EQ(result.event.timestamp.count(), 1000000);
  EXPECT_EQ(result.event.sequence.value(), 0U);
  EXPECT_EQ(result.event.type, EventType::NewOrder);
  EXPECT_EQ(result.event.instrument, "AAPL");
  EXPECT_EQ(result.event.orderId.value(), 101U);
  EXPECT_EQ(result.event.side, Side::Bid);
  EXPECT_EQ(result.event.price.ticks(), 10000);
  EXPECT_EQ(result.event.quantity.lots(), 5);
}

TEST(CsvParserTest, ValidNewOrderAsk) {
  const CsvParser parser = makeParser();
  const ParseResult result = parser.parseRecord("1000010,N,102,A,10010,3", Sequence{7});

  EXPECT_EQ(result.error, ParseError::Ok);
  EXPECT_EQ(result.event.side, Side::Ask);
  EXPECT_EQ(result.event.sequence.value(), 7U);
}

TEST(CsvParserTest, ValidCancelOrder) {
  const CsvParser parser = makeParser();
  const ParseResult result = parser.parseRecord("1000020,C,101,B,,", Sequence{2});

  EXPECT_EQ(result.error, ParseError::Ok);
  EXPECT_EQ(result.event.type, EventType::CancelOrder);
  EXPECT_EQ(result.event.price.ticks(), 0);
  EXPECT_EQ(result.event.quantity.lots(), 0);
}

TEST(CsvParserTest, ValidModifyPriceOnly) {
  const CsvParser parser = makeParser();
  const ParseResult result = parser.parseRecord("1000030,M,101,B,9999,", Sequence{3});

  EXPECT_EQ(result.error, ParseError::Ok);
  EXPECT_EQ(result.event.type, EventType::ModifyOrder);
  EXPECT_EQ(result.event.price.ticks(), 9999);
  EXPECT_EQ(result.event.quantity.lots(), 0);
}

TEST(CsvParserTest, ValidModifyQuantityOnly) {
  const CsvParser parser = makeParser();
  const ParseResult result = parser.parseRecord("1000040,M,101,B,,7", Sequence{4});

  EXPECT_EQ(result.error, ParseError::Ok);
  EXPECT_EQ(result.event.quantity.lots(), 7);
}

TEST(CsvParserTest, EmptyRecordRejected) {
  const CsvParser parser = makeParser();
  const ParseResult result = parser.parseRecord("", Sequence{0});
  EXPECT_EQ(result.error, ParseError::EmptyRecord);
  const ParseResult spaces = parser.parseRecord("   ", Sequence{0});
  EXPECT_EQ(spaces.error, ParseError::EmptyRecord);
}

TEST(CsvParserTest, WrongFieldCountRejected) {
  const CsvParser parser = makeParser();
  EXPECT_EQ(parser.parseRecord("1000000,N,101,B,10000", Sequence{0}).error,
            ParseError::WrongFieldCount);
  EXPECT_EQ(parser.parseRecord("1000000,N,101,B,10000,5,extra", Sequence{0}).error,
            ParseError::WrongFieldCount);
}

TEST(CsvParserTest, InvalidEventTypeRejected) {
  const CsvParser parser = makeParser();
  EXPECT_EQ(parser.parseRecord("1000000,X,101,B,10000,5", Sequence{0}).error,
            ParseError::InvalidEventType);
  EXPECT_EQ(parser.parseRecord("1000000,,101,B,10000,5", Sequence{0}).error,
            ParseError::InvalidEventType);
}

TEST(CsvParserTest, InvalidSideRejected) {
  const CsvParser parser = makeParser();
  EXPECT_EQ(parser.parseRecord("1000000,N,101,X,10000,5", Sequence{0}).error,
            ParseError::InvalidSide);
  EXPECT_EQ(parser.parseRecord("1000000,N,101,,10000,5", Sequence{0}).error,
            ParseError::InvalidSide);
}

TEST(CsvParserTest, MalformedTimestampRejected) {
  const CsvParser parser = makeParser();
  EXPECT_EQ(parser.parseRecord("abc,N,101,B,10000,5", Sequence{0}).error,
            ParseError::InvalidTimestamp);
  EXPECT_EQ(parser.parseRecord("10.5,N,101,B,10000,5", Sequence{0}).error,
            ParseError::InvalidTimestamp);
  EXPECT_EQ(parser.parseRecord(",N,101,B,10000,5", Sequence{0}).error,
            ParseError::InvalidTimestamp);
}

TEST(CsvParserTest, MalformedOrderIdRejected) {
  const CsvParser parser = makeParser();
  EXPECT_EQ(parser.parseRecord("1000000,N,abc,B,10000,5", Sequence{0}).error,
            ParseError::InvalidOrderId);
  EXPECT_EQ(parser.parseRecord("1000000,N,,B,10000,5", Sequence{0}).error,
            ParseError::InvalidOrderId);
  // Negative order_id is invalid (uint64).
  EXPECT_EQ(parser.parseRecord("1000000,N,-5,B,10000,5", Sequence{0}).error,
            ParseError::InvalidOrderId);
}

TEST(CsvParserTest, MalformedPriceRejected) {
  const CsvParser parser = makeParser();
  EXPECT_EQ(parser.parseRecord("1000000,N,101,B,abc,5", Sequence{0}).error,
            ParseError::InvalidPrice);
  EXPECT_EQ(parser.parseRecord("1000000,N,101,B,10.5,5", Sequence{0}).error,
            ParseError::InvalidPrice);
}

TEST(CsvParserTest, MalformedQuantityRejected) {
  const CsvParser parser = makeParser();
  EXPECT_EQ(parser.parseRecord("1000000,N,101,B,10000,abc", Sequence{0}).error,
            ParseError::InvalidQuantity);
}

TEST(CsvParserTest, ZeroPriceNewOrderRejected) {
  const CsvParser parser = makeParser();
  // Parser accepts the syntax; validateEvent rejects the semantics.
  const ParseResult result = parser.parseRecord("1000000,N,101,B,0,5", Sequence{0});
  EXPECT_EQ(result.error, ParseError::EventRejected);
}

TEST(CsvParserTest, ZeroQuantityNewOrderRejected) {
  const CsvParser parser = makeParser();
  const ParseResult result = parser.parseRecord("1000000,N,101,B,10000,0", Sequence{0});
  EXPECT_EQ(result.error, ParseError::EventRejected);
}

TEST(CsvParserTest, NegativeQuantityRejected) {
  const CsvParser parser = makeParser();
  const ParseResult result = parser.parseRecord("1000000,N,101,B,10000,-5", Sequence{0});
  EXPECT_EQ(result.error, ParseError::EventRejected);
}

TEST(CsvParserTest, NegativePriceRejected) {
  const CsvParser parser = makeParser();
  const ParseResult result = parser.parseRecord("1000000,N,101,B,-100,5", Sequence{0});
  EXPECT_EQ(result.error, ParseError::EventRejected);
}

TEST(CsvParserTest, EmptyModifyRejected) {
  const CsvParser parser = makeParser();
  // Neither price nor quantity set: validateEvent EmptyModify.
  const ParseResult result = parser.parseRecord("1000000,M,101,B,,", Sequence{0});
  EXPECT_EQ(result.error, ParseError::EventRejected);
}

TEST(CsvParserTest, CancelWithPayloadRejected) {
  const CsvParser parser = makeParser();
  const ParseResult result = parser.parseRecord("1000000,C,101,B,100,5", Sequence{0});
  EXPECT_EQ(result.error, ParseError::EventRejected);
}

TEST(CsvParserTest, TimestampOverflowRejected) {
  const CsvParser parser = makeParser();
  // 2^63 exceeds int64 max.
  const ParseResult result = parser.parseRecord("9223372036854775808,N,101,B,10000,5", Sequence{0});
  EXPECT_EQ(result.error, ParseError::NumericOverflow);
}

TEST(CsvParserTest, TimestampMaxAccepted) {
  const CsvParser parser = makeParser();
  const ParseResult result = parser.parseRecord("9223372036854775807,N,101,B,10000,5", Sequence{0});
  EXPECT_EQ(result.error, ParseError::Ok);
  EXPECT_EQ(result.event.timestamp.count(), 9223372036854775807LL);
}

TEST(CsvParserTest, TimestampMinAccepted) {
  const CsvParser parser = makeParser();
  const ParseResult result =
      parser.parseRecord("-9223372036854775808,N,101,B,10000,5", Sequence{0});
  EXPECT_EQ(result.error, ParseError::Ok);
  EXPECT_EQ(result.event.timestamp.count(), -9223372036854775807LL - 1);
}

TEST(CsvParserTest, OrderIdOverflowRejected) {
  const CsvParser parser = makeParser();
  // 2^64 exceeds uint64 max.
  const ParseResult result =
      parser.parseRecord("1000000,N,18446744073709551616,B,10000,5", Sequence{0});
  EXPECT_EQ(result.error, ParseError::NumericOverflow);
}

TEST(CsvParserTest, OrderIdMaxAccepted) {
  const CsvParser parser = makeParser();
  const ParseResult result =
      parser.parseRecord("1000000,N,18446744073709551615,B,10000,5", Sequence{0});
  EXPECT_EQ(result.error, ParseError::Ok);
  EXPECT_EQ(result.event.orderId.value(), 18446744073709551615ULL);
}

TEST(CsvParserTest, DeterministicParsing) {
  const CsvParser parser = makeParser();
  const std::string line = "1000000,N,101,B,10000,5";
  const ParseResult first = parser.parseRecord(line, Sequence{3});
  const ParseResult second = parser.parseRecord(line, Sequence{3});
  EXPECT_EQ(first.error, ParseError::Ok);
  EXPECT_EQ(second.error, ParseError::Ok);
  EXPECT_EQ(first.event, second.event);
}

TEST(CsvParserTest, WhitespaceTrimmed) {
  const CsvParser parser = makeParser();
  const ParseResult result =
      parser.parseRecord("  1000000 , N , 101 , B , 10000 , 5  ", Sequence{0});
  EXPECT_EQ(result.error, ParseError::Ok);
  EXPECT_EQ(result.event.orderId.value(), 101U);
}

TEST(CsvParserTest, ParseStreamAssignsDenseSequences) {
  const CsvParser parser = makeParser();
  std::istringstream input("timestamp,event_type,order_id,side,price,quantity\n"
                           "1000000,N,1,B,100,10\n"
                           "1000010,N,2,A,101,5\n"
                           "1000020,C,1,B,,\n");
  const StreamResult result = parser.parseStream(input);

  EXPECT_EQ(result.error, ParseError::Ok);
  EXPECT_EQ(result.events.size(), 3U);
  EXPECT_EQ(result.events[0].sequence.value(), 0U);
  EXPECT_EQ(result.events[1].sequence.value(), 1U);
  EXPECT_EQ(result.events[2].sequence.value(), 2U);
  // Timestamps preserved exactly.
  EXPECT_EQ(result.events[0].timestamp.count(), 1000000);
  EXPECT_EQ(result.events[1].timestamp.count(), 1000010);
}

TEST(CsvParserTest, ParseStreamRejectsBadHeader) {
  const CsvParser parser = makeParser();
  std::istringstream input("bad,header\n1000000,N,1,B,100,10\n");
  const StreamResult result = parser.parseStream(input);
  EXPECT_EQ(result.error, ParseError::BadHeader);
  EXPECT_EQ(result.lineNumber, 1U);
}

TEST(CsvParserTest, ParseStreamStopsAtFirstError) {
  const CsvParser parser = makeParser();
  std::istringstream input("timestamp,event_type,order_id,side,price,quantity\n"
                           "1000000,N,1,B,100,10\n"
                           "bad-line-here\n"
                           "1000020,N,3,B,100,5\n");
  const StreamResult result = parser.parseStream(input);
  EXPECT_EQ(result.error, ParseError::WrongFieldCount);
  EXPECT_EQ(result.lineNumber, 3U);
  // Only the first record was accepted; nothing partial leaks.
  EXPECT_EQ(result.events.size(), 1U);
}

TEST(CsvParserTest, ParseStreamEmptyInput) {
  const CsvParser parser = makeParser();
  std::istringstream input("");
  const StreamResult result = parser.parseStream(input);
  EXPECT_EQ(result.error, ParseError::BadHeader);
}

TEST(CsvParserTest, ErrorCarriesDiagnostic) {
  const CsvParser parser = makeParser();
  const ParseResult result = parser.parseRecord("1000000,X,101,B,10000,5", Sequence{0});
  EXPECT_EQ(result.error, ParseError::InvalidEventType);
  EXPECT_FALSE(result.diagnostic.empty());
  EXPECT_NE(result.diagnostic.find("N, M, or C"), std::string::npos);
}

} // namespace
