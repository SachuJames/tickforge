// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Parser-to-replay integration tests.
//
// Verifies the contract boundary between CSV parsing and replay:
//   - Valid parsed streams reach replay correctly
//   - Parser errors are distinct from replay errors
//   - Strict/lenient replay semantics apply to parsed events
//   - Failed parses do not contaminate subsequent parses

#include "tests/support/csv_generator.hpp"
#include "tickforge/book/order_book.hpp"
#include "tickforge/market_data/csv_parser.hpp"
#include "tickforge/matching/matching_engine.hpp"
#include "tickforge/replay/replay.hpp"

#include <gtest/gtest.h>
#include <sstream>

namespace tickforge::test {
namespace {

// A valid CSV stream with orders that will match.
std::string makeMatchingStream() {
  return "timestamp,event_type,order_id,side,price,quantity\n"
         "1000000,N,1,B,100,10\n" // bid rests
         "1000001,N,2,A,100,5\n"  // ask crosses, fills 5
         "1000002,N,3,A,101,3\n"; // ask rests (no cross)
}

// Parses and replays strictly. Returns the replay result and book.
struct IntegrationResult {
  StreamResult parseResult;
  ReplayResult replayResult;
  std::size_t orderCount = 0;
  std::optional<Price> bestBid;
  std::optional<Price> bestAsk;
};

IntegrationResult parseAndReplay(const std::string& csv, ReplayMode mode) {
  IntegrationResult out;
  const CsvParser parser("AAPL");
  std::istringstream input(csv);
  out.parseResult = parser.parseStream(input);
  if (out.parseResult.error != ParseError::Ok) {
    return out;
  }
  OrderBook book;
  MatchingEngine engine(book);
  out.replayResult = replayEvents(out.parseResult.events, engine, mode);
  out.orderCount = book.orderCount();
  out.bestBid = book.bestBid();
  out.bestAsk = book.bestAsk();
  return out;
}

TEST(ParserReplay, ValidStreamReplays) {
  const auto result = parseAndReplay(makeMatchingStream(), ReplayMode::Strict);
  EXPECT_EQ(result.parseResult.error, ParseError::Ok);
  EXPECT_TRUE(result.replayResult.ok());
  // Order 1: 10 - 5 filled = 5 remaining. Order 3: 3 resting.
  EXPECT_EQ(result.orderCount, 2U);
  EXPECT_EQ(result.bestBid, Price{100});
  EXPECT_EQ(result.bestAsk, Price{101});
}

TEST(ParserReplay, GeneratedValidStreamsReplay) {
  for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
    CsvGeneratorConfig config;
    config.seed = seed;
    config.recordCount = 25;
    config.malformedPercent = 0;
    CsvGenerator gen(config);
    const auto result = parseAndReplay(gen.generate(), ReplayMode::Strict);

    EXPECT_EQ(result.parseResult.error, ParseError::Ok) << "seed=" << seed;
    // Replay may fail if generated modifies/cancels target non-live orders
    // (the CSV generator doesn't track live state). That's expected;
    // the parser's job is done once events are structurally valid.
    // We verify the parser produced valid events, not that replay succeeds.
    for (const auto& e : result.parseResult.events) {
      EXPECT_EQ(validateEvent(e), EventValidationError::Ok) << "seed=" << seed;
    }
  }
}

// Parser error is distinct from replay error: a malformed CSV never
// reaches replay.
TEST(ParserReplay, ParserErrorBlocksReplay) {
  const std::string csv = "timestamp,event_type,order_id,side,price,quantity\n"
                          "1000000,N,1,B,100,10\n"
                          "1000001,X,2,A,100,5\n"; // bad event type
  const auto result = parseAndReplay(csv, ReplayMode::Strict);

  EXPECT_EQ(result.parseResult.error, ParseError::InvalidEventType);
  EXPECT_EQ(result.parseResult.events.size(), 1U); // partial results
  // Replay was not attempted (parse failed).
  EXPECT_EQ(result.replayResult.error, ReplayError::Ok); // default, not run
  EXPECT_EQ(result.orderCount, 0U);
}

// Strict replay aborts on dispatch failure (unknown cancel).
// The CSV is valid; the replay fails.
TEST(ParserReplay, StrictReplayAbortsOnUnknownCancel) {
  const std::string csv = "timestamp,event_type,order_id,side,price,quantity\n"
                          "1000000,N,1,B,100,10\n"
                          "1000001,C,999,B,0,0\n"; // cancel unknown order
  const auto result = parseAndReplay(csv, ReplayMode::Strict);

  EXPECT_EQ(result.parseResult.error, ParseError::Ok); // CSV is fine
  EXPECT_FALSE(result.replayResult.ok());
  EXPECT_EQ(result.replayResult.error, ReplayError::UnknownOrder);
}

// Lenient replay skips the unknown cancel and continues.
TEST(ParserReplay, LenientReplaySkipsUnknownCancel) {
  const std::string csv = "timestamp,event_type,order_id,side,price,quantity\n"
                          "1000000,N,1,B,100,10\n"
                          "1000001,C,999,B,0,0\n"  // skipped
                          "1000002,N,2,A,101,5\n"; // processed
  const auto result = parseAndReplay(csv, ReplayMode::Lenient);

  EXPECT_EQ(result.parseResult.error, ParseError::Ok);
  EXPECT_TRUE(result.replayResult.ok());
  EXPECT_EQ(result.replayResult.skippedCount, 1U);
  EXPECT_EQ(result.orderCount, 2U); // orders 1 and 2
}

// Instrument mismatch is fatal in both modes, even with valid CSV.
TEST(ParserReplay, InstrumentMismatchFatal) {
  const CsvParser parser("AAPL");
  std::istringstream input(makeMatchingStream());
  const StreamResult parseResult = parser.parseStream(input);
  ASSERT_EQ(parseResult.error, ParseError::Ok);

  // Tamper with the instrument (simulates a stream mixing sources).
  std::vector<Event> events = parseResult.events;
  events[0].instrument = "GOOG";

  OrderBook book;
  MatchingEngine engine(book);
  const auto strictResult = replayEvents(events, engine, ReplayMode::Strict);
  EXPECT_EQ(strictResult.error, ReplayError::InstrumentMismatch);

  OrderBook book2;
  MatchingEngine engine2(book2);
  const auto lenientResult = replayEvents(events, engine2, ReplayMode::Lenient);
  EXPECT_EQ(lenientResult.error, ReplayError::InstrumentMismatch);
}

// State isolation: a failed parse does not affect the next parse.
TEST(ParserReplay, FailedParseIsolatesState) {
  const CsvParser parser("AAPL");

  // First: malformed stream.
  {
    std::istringstream bad("timestamp,event_type,order_id,side,price,quantity\n"
                           "1000000,X,1,B,100,10\n");
    const StreamResult r = parser.parseStream(bad);
    EXPECT_NE(r.error, ParseError::Ok);
  }

  // Second: valid stream must parse cleanly (no stale state).
  {
    std::istringstream good(makeMatchingStream());
    const StreamResult r = parser.parseStream(good);
    EXPECT_EQ(r.error, ParseError::Ok);
    EXPECT_EQ(r.events.size(), 3U);
    // Sequences restart from 0 (not continuing from failed parse).
    EXPECT_EQ(r.events[0].sequence, Sequence{0});
  }
}

// Sequences are dense and restart for each parseStream call.
TEST(ParserReplay, SequencesRestartPerStream) {
  const CsvParser parser("AAPL");
  for (int i = 0; i < 2; ++i) {
    std::istringstream input(makeMatchingStream());
    const StreamResult r = parser.parseStream(input);
    ASSERT_EQ(r.error, ParseError::Ok);
    for (std::size_t j = 0; j < r.events.size(); ++j) {
      EXPECT_EQ(r.events[j].sequence, Sequence{j});
    }
  }
}

} // namespace
} // namespace tickforge::test
