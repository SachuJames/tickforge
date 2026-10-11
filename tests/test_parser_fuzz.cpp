// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Parser fuzz tests: deterministic adversarial CSV inputs.
//
// Uses the Day 11 DeterministicRng and the new CsvGenerator. Every case
// is reproducible from its seed. Tests verify:
//   - Valid generated streams parse successfully
//   - Malformed inputs produce the expected error (not success)
//   - Malformed records never leak into successful results
//   - Numeric boundaries are handled per the contract

#include "tests/support/csv_generator.hpp"
#include "tickforge/market_data/csv_parser.hpp"

#include <gtest/gtest.h>
#include <sstream>

namespace tickforge::test {
namespace {

// Valid generated streams must parse cleanly with dense sequences.
TEST(ParserFuzz, ValidStreamsParse) {
  for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL, 42ULL}) {
    CsvGeneratorConfig config;
    config.seed = seed;
    config.recordCount = 30;
    config.malformedPercent = 0;
    CsvGenerator gen(config);
    const std::string csv = gen.generate();

    CsvParser parser("AAPL");
    std::istringstream input(csv);
    const StreamResult result = parser.parseStream(input);

    EXPECT_EQ(result.error, ParseError::Ok) << "seed=" << seed << " diag=" << result.diagnostic;
    EXPECT_EQ(result.events.size(), 30U) << "seed=" << seed;
    // Dense sequences from 0.
    for (std::size_t i = 0; i < result.events.size(); ++i) {
      EXPECT_EQ(result.events[i].sequence, Sequence{i}) << "seed=" << seed;
    }
    // All events carry the parser's instrument.
    for (const auto& e : result.events) {
      EXPECT_EQ(e.instrument, "AAPL") << "seed=" << seed;
    }
  }
}

// Malformed streams must either fail cleanly or parse valid records.
// They must never produce invalid events or crash.
TEST(ParserFuzz, MalformedStreamsBehave) {
  for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
    CsvGeneratorConfig config;
    config.seed = seed;
    config.recordCount = 20;
    config.malformedPercent = 100;
    CsvGenerator gen(config);
    const std::string csv = gen.generate();

    CsvParser parser("AAPL");
    std::istringstream input(csv);
    const StreamResult result = parser.parseStream(input);

    // Every returned event must pass validation (no invalid events leak).
    for (const auto& e : result.events) {
      EXPECT_EQ(validateEvent(e), EventValidationError::Ok) << "seed=" << seed;
    }
    // If it failed, the error must be non-Ok with a diagnostic.
    if (result.error != ParseError::Ok) {
      EXPECT_FALSE(result.diagnostic.empty()) << "seed=" << seed;
    }
  }
}

// The malformed table: each case must fail with a non-Ok error.
TEST(ParserFuzz, MalformedTable) {
  const auto cases = generateMalformedTable();
  ASSERT_FALSE(cases.empty());
  for (const auto& c : cases) {
    CsvParser parser("AAPL");
    std::istringstream input(c.input);
    const StreamResult result = parser.parseStream(input);
    EXPECT_NE(result.error, ParseError::Ok) << "case: " << c.description;
  }
}

// Header-only stream is valid (empty event stream), not malformed.
TEST(ParserFuzz, HeaderOnlyStreamIsValid) {
  CsvParser parser("AAPL");
  std::istringstream input("timestamp,event_type,order_id,side,price,quantity\n");
  const StreamResult r = parser.parseStream(input);
  EXPECT_EQ(r.error, ParseError::Ok);
  EXPECT_TRUE(r.events.empty());
}

// Numeric boundary: max int64 timestamp accepted, overflow rejected.
TEST(ParserFuzz, TimestampBoundaries) {
  CsvParser parser("AAPL");
  const std::string header = "timestamp,event_type,order_id,side,price,quantity\n";

  // Max int64.
  {
    std::istringstream input(header + "9223372036854775807,N,1,B,100,10\n");
    const StreamResult r = parser.parseStream(input);
    EXPECT_EQ(r.error, ParseError::Ok);
  }
  // Overflow by 1.
  {
    std::istringstream input(header + "9223372036854775808,N,1,B,100,10\n");
    const StreamResult r = parser.parseStream(input);
    EXPECT_EQ(r.error, ParseError::NumericOverflow);
  }
  // Min int64.
  {
    std::istringstream input(header + "-9223372036854775808,N,1,B,100,10\n");
    const StreamResult r = parser.parseStream(input);
    EXPECT_EQ(r.error, ParseError::Ok);
  }
}

// Numeric boundary: max uint64 order_id accepted, overflow rejected.
TEST(ParserFuzz, OrderIdBoundaries) {
  CsvParser parser("AAPL");
  const std::string header = "timestamp,event_type,order_id,side,price,quantity\n";

  {
    std::istringstream input(header + "1000000,N,18446744073709551615,B,100,10\n");
    const StreamResult r = parser.parseStream(input);
    EXPECT_EQ(r.error, ParseError::Ok);
  }
  {
    std::istringstream input(header + "1000000,N,18446744073709551616,B,100,10\n");
    const StreamResult r = parser.parseStream(input);
    EXPECT_EQ(r.error, ParseError::NumericOverflow);
  }
  // Negative order_id rejected (unsigned field).
  {
    std::istringstream input(header + "1000000,N,-1,B,100,10\n");
    const StreamResult r = parser.parseStream(input);
    EXPECT_EQ(r.error, ParseError::InvalidOrderId);
  }
}

// Plus sign is rejected by from_chars (only minus is allowed).
TEST(ParserFuzz, NumericFormats) {
  CsvParser parser("AAPL");

  {
    const ParseResult r = parser.parseRecord("+1000000,N,1,B,+100,+10", Sequence{0});
    EXPECT_EQ(r.error, ParseError::InvalidTimestamp) << r.diagnostic;
  }
  // Leading zeros.
  {
    const ParseResult r = parser.parseRecord("0001000000,N,0001,B,00100,00010", Sequence{0});
    EXPECT_EQ(r.error, ParseError::Ok) << r.diagnostic;
    EXPECT_EQ(r.event.timestamp, Timestamp{1000000});
    EXPECT_EQ(r.event.orderId, OrderId{1});
  }
  // Decimal point rejected.
  {
    const ParseResult r = parser.parseRecord("1000000.0,N,1,B,100,10", Sequence{0});
    EXPECT_EQ(r.error, ParseError::InvalidTimestamp);
  }
  // Hex rejected.
  {
    const ParseResult r = parser.parseRecord("0x1000000,N,1,B,100,10", Sequence{0});
    EXPECT_EQ(r.error, ParseError::InvalidTimestamp);
  }
  // Extremely long numeric token (overflow).
  {
    const std::string longNum(100, '9');
    const ParseResult r = parser.parseRecord("1000000,N,1,B," + longNum + ",10", Sequence{0});
    EXPECT_EQ(r.error, ParseError::NumericOverflow);
  }
}

// CRLF line endings are handled (CR is trimmed).
TEST(ParserFuzz, CrlfLineEndings) {
  CsvParser parser("AAPL");
  const std::string csv =
      "timestamp,event_type,order_id,side,price,quantity\r\n"
      "1000000,N,1,B,100,10\r\n"
      "1000001,N,2,A,101,5\r\n";
  std::istringstream input(csv);
  const StreamResult r = parser.parseStream(input);
  EXPECT_EQ(r.error, ParseError::Ok) << r.diagnostic;
  EXPECT_EQ(r.events.size(), 2U);
}

// Trailing newline vs no trailing newline: same events.
TEST(ParserFuzz, TrailingNewline) {
  CsvParser parser("AAPL");
  const std::string base =
      "timestamp,event_type,order_id,side,price,quantity\n"
      "1000000,N,1,B,100,10\n"
      "1000001,N,2,A,101,5";

  std::istringstream withNewline(base + "\n");
  std::istringstream withoutNewline(base);
  const StreamResult r1 = parser.parseStream(withNewline);
  const StreamResult r2 = parser.parseStream(withoutNewline);

  EXPECT_EQ(r1.error, ParseError::Ok);
  EXPECT_EQ(r2.error, ParseError::Ok);
  ASSERT_EQ(r1.events.size(), r2.events.size());
  for (std::size_t i = 0; i < r1.events.size(); ++i) {
    EXPECT_EQ(r1.events[i].orderId, r2.events[i].orderId);
    EXPECT_EQ(r1.events[i].price, r2.events[i].price);
  }
}

// Stream stops at first error; earlier events are preserved.
TEST(ParserFuzz, StreamStopsAtFirstError) {
  CsvParser parser("AAPL");
  const std::string csv =
      "timestamp,event_type,order_id,side,price,quantity\n"
      "1000000,N,1,B,100,10\n"
      "1000001,N,2,A,101,5\n"
      "1000002,X,3,B,100,10\n" // bad event type
      "1000003,N,4,B,100,10\n"; // never processed
  std::istringstream input(csv);
  const StreamResult r = parser.parseStream(input);

  EXPECT_EQ(r.error, ParseError::InvalidEventType);
  EXPECT_EQ(r.lineNumber, 4U); // 1-based: header=1, records=2,3, bad=4
  EXPECT_EQ(r.events.size(), 2U); // partial results preserved
}

// Failed records do not leak events: parseRecord returns default event.
TEST(ParserFuzz, FailedRecordYieldsNoEvent) {
  CsvParser parser("AAPL");
  const ParseResult r = parser.parseRecord("1000000,X,1,B,100,10", Sequence{5});
  EXPECT_EQ(r.error, ParseError::InvalidEventType);
  // Event is default-constructed except sequence/instrument which are set upfront.
  EXPECT_EQ(r.event.sequence, Sequence{5});
  EXPECT_EQ(r.event.type, EventType::NewOrder); // default, not X
}

// Fresh parser state: parsing the same input twice gives same result.
TEST(ParserFuzz, RepeatableParsing) {
  CsvParser parser("AAPL");
  const std::string csv =
      "timestamp,event_type,order_id,side,price,quantity\n"
      "1000000,N,1,B,100,10\n"
      "1000001,C,1,,,\n";

  std::istringstream input1(csv);
  std::istringstream input2(csv);
  const StreamResult r1 = parser.parseStream(input1);
  const StreamResult r2 = parser.parseStream(input2);

  EXPECT_EQ(r1.error, r2.error);
  ASSERT_EQ(r1.events.size(), r2.events.size());
  for (std::size_t i = 0; i < r1.events.size(); ++i) {
    EXPECT_EQ(r1.events[i].orderId, r2.events[i].orderId);
    EXPECT_EQ(r1.events[i].sequence, r2.events[i].sequence);
  }
}

} // namespace
} // namespace tickforge::test
