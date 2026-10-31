// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: Apache-2.0.
//
// Adversarial CSV stream verification (SPEC.md 9, parser boundary).
//
// Covers: fragmented stream delivery (chunking invariance), split
// token boundaries, EOF and final-record handling, line numbers,
// BOM and embedded-NUL bytes, control characters, multi-megabyte
// lines, injected I/O failures, and parser-to-replay equivalence.
//
// The parser reads with std::getline. Chunked delivery must not change
// results for identical bytes. Note: StreamResult cannot distinguish a
// clean EOF from an underlying read failure (badbit); these tests pin
// that behavior and the caller-visible stream-state difference.

#include "tests/support/csv_generator.hpp"
#include "tests/support/deterministic_rng.hpp"
#include "tickforge/book/order_book.hpp"
#include "tickforge/market_data/csv_parser.hpp"
#include "tickforge/matching/matching_engine.hpp"
#include "tickforge/replay/replay.hpp"

#include <cstdlib>
#include <gtest/gtest.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace tickforge::test {
namespace {

// Test-only stream buffer: delivers `data` in chunks whose sizes cycle
// through `schedule`. If failAfter is set, underflow throws once that
// many bytes have been delivered, which the istream turns into badbit
// (standard streambuf failure mechanism).
class ChunkedBuf : public std::streambuf {
public:
  ChunkedBuf(std::string data, std::vector<std::size_t> schedule)
      : data_(std::move(data)), schedule_(std::move(schedule)) {}

  void failAfter(std::size_t bytes) {
    failAfter_ = bytes;
  }

protected:
  int_type underflow() override {
    if (pos_ >= data_.size()) {
      return traits_type::eof();
    }
    if (failAfter_.has_value() && pos_ >= *failAfter_) {
      throw std::runtime_error("injected read failure");
    }
    std::size_t chunk = schedule_[scheduleIdx_++ % schedule_.size()];
    if (chunk == 0) {
      chunk = 1;
    }
    std::size_t n = std::min(chunk, data_.size() - pos_);
    if (failAfter_.has_value()) {
      n = std::min(n, *failAfter_ - pos_);
    }
    char* begin = data_.data() + pos_;
    setg(begin, begin, begin + n);
    pos_ += n;
    return traits_type::to_int_type(*gptr());
  }

private:
  std::string data_;
  std::vector<std::size_t> schedule_;
  std::size_t scheduleIdx_ = 0;
  std::size_t pos_ = 0;
  std::optional<std::size_t> failAfter_;
};

const std::string kHeaderLine = "timestamp,event_type,order_id,side,price,quantity\n";

std::string validDoc() {
  return kHeaderLine + "1000000,N,1,B,100,10\n" + "1000001,N,2,A,100,5\n" +
         "1000002,M,1,B,101,7\n" + "1000003,C,2,A,,\n";
}

StreamResult parseOrdinary(const std::string& doc) {
  const CsvParser parser{"AAPL"};
  std::istringstream input(doc);
  return parser.parseStream(input);
}

StreamResult parseChunked(const std::string& doc, const std::vector<std::size_t>& schedule) {
  const CsvParser parser{"AAPL"};
  ChunkedBuf buf(doc, schedule);
  std::istream input(&buf);
  return parser.parseStream(input);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): assertion-heavy helper.
void expectEventsEqual(const Event& a, const Event& b) {
  EXPECT_EQ(a.type, b.type);
  EXPECT_EQ(a.orderId.value(), b.orderId.value());
  EXPECT_EQ(a.side, b.side);
  EXPECT_EQ(a.price.ticks(), b.price.ticks());
  EXPECT_EQ(a.quantity.lots(), b.quantity.lots());
  EXPECT_EQ(a.timestamp.count(), b.timestamp.count());
  EXPECT_EQ(a.sequence.value(), b.sequence.value());
  EXPECT_EQ(a.instrument, b.instrument);
}

void expectResultsEqual(const StreamResult& a, const StreamResult& b) {
  EXPECT_EQ(a.error, b.error);
  EXPECT_EQ(a.lineNumber, b.lineNumber);
  EXPECT_EQ(a.validationError, b.validationError);
  EXPECT_EQ(a.diagnostic, b.diagnostic);
  ASSERT_EQ(a.events.size(), b.events.size());
  for (std::size_t i = 0; i < a.events.size(); ++i) {
    expectEventsEqual(a.events[i], b.events[i]);
  }
}

// --- Part 3: Fragmentation invariance ---

TEST(ChunkedDelivery, ValidDocAllChunkSizes) {
  const std::string doc = validDoc();
  const StreamResult base = parseOrdinary(doc);
  ASSERT_EQ(base.error, ParseError::Ok);
  ASSERT_EQ(base.events.size(), 4U);
  for (const std::size_t chunk : {1U, 2U, 3U, 7U, 16U, 64U, 256U, 4096U}) {
    expectResultsEqual(parseChunked(doc, {chunk}), base);
  }
}

TEST(ChunkedDelivery, VariableSchedule) {
  const std::string doc = validDoc();
  const StreamResult base = parseOrdinary(doc);
  expectResultsEqual(parseChunked(doc, {1, 4, 2, 7, 3}), base);
  expectResultsEqual(parseChunked(doc, {13, 1, 1, 29}), base);
}

TEST(ChunkedDelivery, SplitAtEveryBytePosition) {
  const std::string doc = kHeaderLine + "1000000,N,1,B,100,10\n";
  const StreamResult base = parseOrdinary(doc);
  ASSERT_EQ(base.error, ParseError::Ok);
  // Two-chunk delivery split at every possible position.
  for (std::size_t split = 0; split <= doc.size(); ++split) {
    expectResultsEqual(parseChunked(doc, {split, doc.size()}), base);
  }
}

TEST(ChunkedDelivery, CrlfSplitAcrossChunks) {
  const std::string doc = "timestamp,event_type,order_id,side,price,quantity\r\n"
                          "1000000,N,1,B,100,10\r\n";
  const StreamResult base = parseOrdinary(doc);
  ASSERT_EQ(base.error, ParseError::Ok);
  ASSERT_EQ(base.events.size(), 1U);
  // Chunk size 1 splits every CR from its LF.
  expectResultsEqual(parseChunked(doc, {1}), base);
  expectResultsEqual(parseChunked(doc, {3}), base);
}

TEST(ChunkedDelivery, MalformedDocFragmented) {
  const std::string doc =
      kHeaderLine + "1000000,N,1,B,100,10\n" + "1000001,X,2,A,100,5\n" + "1000002,N,3,A,101,3\n";
  const StreamResult base = parseOrdinary(doc);
  ASSERT_EQ(base.error, ParseError::InvalidEventType);
  ASSERT_EQ(base.lineNumber, 3U);
  ASSERT_EQ(base.events.size(), 1U);
  expectResultsEqual(parseChunked(doc, {1}), base);
  expectResultsEqual(parseChunked(doc, {5}), base);
}

// --- Part 4: EOF, final records, line numbers ---

TEST(EofHandling, FinalRecordWithoutNewline) {
  const std::string doc = kHeaderLine + "1000000,N,1,B,100,10";
  const auto result = parseOrdinary(doc);
  EXPECT_EQ(result.error, ParseError::Ok);
  ASSERT_EQ(result.events.size(), 1U);
  EXPECT_EQ(result.events[0].sequence.value(), 0U);
  expectResultsEqual(parseChunked(doc, {1}), result);
}

TEST(EofHandling, HeaderOnlyNoNewline) {
  const auto result = parseOrdinary("timestamp,event_type,order_id,side,price,quantity");
  EXPECT_EQ(result.error, ParseError::Ok);
  EXPECT_TRUE(result.events.empty());
}

TEST(EofHandling, EmptyInput) {
  const auto result = parseOrdinary("");
  EXPECT_EQ(result.error, ParseError::BadHeader);
  EXPECT_EQ(result.lineNumber, 1U);
}

TEST(EofHandling, TrailingBlankLineIsError) {
  const std::string doc = kHeaderLine + "1000000,N,1,B,100,10\n" + "\n";
  const auto result = parseOrdinary(doc);
  EXPECT_EQ(result.error, ParseError::EmptyRecord);
  EXPECT_EQ(result.lineNumber, 3U);
  ASSERT_EQ(result.events.size(), 1U);
  expectResultsEqual(parseChunked(doc, {1}), result);
}

TEST(EofHandling, IncompleteFinalRecord) {
  const std::string doc = kHeaderLine + "1000000,N,1,B,100,10\n" + "1000001,N,2";
  const auto result = parseOrdinary(doc);
  EXPECT_EQ(result.error, ParseError::WrongFieldCount);
  EXPECT_EQ(result.lineNumber, 3U);
  ASSERT_EQ(result.events.size(), 1U);
}

TEST(EofHandling, LineNumberAfterBlankAndCrlf) {
  // Blank line is line 3 (error there), so CRLF record line numbers
  // after it are never reached; check error line on a later record.
  const std::string doc = "timestamp,event_type,order_id,side,price,quantity\r\n"
                          "1000000,N,1,B,100,10\r\n"
                          "1000001,N,2,A,100,5\r\n"
                          "bad,N,3,A,100,5\r\n";
  const auto result = parseOrdinary(doc);
  EXPECT_EQ(result.error, ParseError::InvalidTimestamp);
  EXPECT_EQ(result.lineNumber, 4U);
  ASSERT_EQ(result.events.size(), 2U);
  // Dense sequences on the accepted prefix.
  EXPECT_EQ(result.events[0].sequence.value(), 0U);
  EXPECT_EQ(result.events[1].sequence.value(), 1U);
  expectResultsEqual(parseChunked(doc, {1}), result);
}

// --- Part 5: BOM handling (contract: no BOM support, header is exact) ---

TEST(BomHandling, BomBeforeHeaderRejected) {
  const std::string doc = std::string("\xEF\xBB\xBF") + validDoc();
  const auto result = parseOrdinary(doc);
  EXPECT_EQ(result.error, ParseError::BadHeader);
  EXPECT_EQ(result.lineNumber, 1U);
}

TEST(BomHandling, BomSplitAcrossChunks) {
  const std::string doc = std::string("\xEF\xBB\xBF") + validDoc();
  const auto base = parseOrdinary(doc);
  // One-byte chunks split the 3 BOM bytes across refills.
  expectResultsEqual(parseChunked(doc, {1}), base);
  expectResultsEqual(parseChunked(doc, {2}), base);
}

TEST(BomHandling, BomInDataFieldRejected) {
  const std::string doc = kHeaderLine + std::string("\xEF\xBB\xBF") + "1000000,N,1,B,100,10\n";
  const auto result = parseOrdinary(doc);
  EXPECT_EQ(result.error, ParseError::InvalidTimestamp);
  EXPECT_EQ(result.lineNumber, 2U);
}

TEST(BomHandling, BomInEventTypeRejected) {
  const std::string doc = kHeaderLine + "1000000," + std::string("\xEF\xBB\xBF") + "N,1,B,100,10\n";
  const auto result = parseOrdinary(doc);
  EXPECT_EQ(result.error, ParseError::InvalidEventType);
  EXPECT_EQ(result.lineNumber, 2U);
}

// --- Part 6: NUL and control bytes ---

TEST(NulBytes, NulInTimestampNotTruncated) {
  // If the parser treated the field as a C string it would see "100"
  // and accept. Length-aware parsing must reject instead.
  std::string doc = kHeaderLine + "100";
  doc.push_back('\0');
  doc += "000,N,1,B,100,10\n";
  const auto result = parseOrdinary(doc);
  EXPECT_EQ(result.error, ParseError::InvalidTimestamp);
  EXPECT_EQ(result.lineNumber, 2U);
  EXPECT_TRUE(result.events.empty());
}

TEST(NulBytes, NulLineBetweenRecords) {
  std::string doc = kHeaderLine + "1000000,N,1,B,100,10\n";
  doc.push_back('\0');
  doc += "\n1000001,N,2,A,100,5\n";
  const auto result = parseOrdinary(doc);
  EXPECT_EQ(result.error, ParseError::WrongFieldCount);
  EXPECT_EQ(result.lineNumber, 3U);
  ASSERT_EQ(result.events.size(), 1U); // prefix preserved, suffix not parsed
}

TEST(NulBytes, NulInQuantity) {
  std::string doc = kHeaderLine + "1000000,N,1,B,100,10";
  doc.push_back('\0');
  doc += "\n";
  const auto result = parseOrdinary(doc);
  EXPECT_EQ(result.error, ParseError::InvalidQuantity);
  EXPECT_EQ(result.lineNumber, 2U);
}

TEST(ControlBytes, TabTrimmedButVerticalTabRejected) {
  // Tab is in the trim set; vertical tab (0x0B) is not.
  const std::string tabDoc = kHeaderLine + "\t1000000,N,1,B,100,10\t\n";
  EXPECT_EQ(parseOrdinary(tabDoc).error, ParseError::Ok);
  std::string vt_doc = kHeaderLine;
  vt_doc.push_back('\v');
  vt_doc += "1000000,N,1,B,100,10\n";
  const auto result = parseOrdinary(vt_doc);
  EXPECT_EQ(result.error, ParseError::InvalidTimestamp);
}

TEST(ControlBytes, CrInsideFieldRejected) {
  const std::string doc = kHeaderLine + "100\r0000,N,1,B,100,10\n";
  const auto result = parseOrdinary(doc);
  EXPECT_EQ(result.error, ParseError::InvalidTimestamp);
}

// --- Part 7: Large records ---

TEST(LargeRecords, MegabyteNumericOverflow) {
  const std::string doc =
      kHeaderLine + std::string(std::size_t{1024} * 1024, '9') + ",N,1,B,100,10\n";
  const auto result = parseOrdinary(doc);
  EXPECT_EQ(result.error, ParseError::NumericOverflow);
  EXPECT_EQ(result.lineNumber, 2U);
  EXPECT_TRUE(result.events.empty());
  // Same outcome delivered in small chunks.
  expectResultsEqual(parseChunked(doc, {64}), result);
}

TEST(LargeRecords, LongLeadingZerosAcceptedExactly) {
  // 100k zeros then 100: from_chars yields exactly 100, no truncation.
  const std::string doc = kHeaderLine + std::string(100000, '0') + "100,N,1,B,100,10\n";
  const auto result = parseOrdinary(doc);
  ASSERT_EQ(result.error, ParseError::Ok);
  ASSERT_EQ(result.events.size(), 1U);
  EXPECT_EQ(result.events[0].timestamp.count(), 100);
}

TEST(LargeRecords, ExtraFieldAfterLongPrefix) {
  const std::string doc =
      kHeaderLine + std::string(65536, '0') + "100,N,1,B,100,10,EXTRA\n" + "1000001,N,2,A,100,5\n";
  const auto result = parseOrdinary(doc);
  EXPECT_EQ(result.error, ParseError::WrongFieldCount);
  EXPECT_EQ(result.lineNumber, 2U);
  EXPECT_TRUE(result.events.empty()); // stops at first error
}

TEST(LargeRecords, LargeMalformedFragmented) {
  const std::string doc = kHeaderLine + "1000000,N,1,B,100,10\n" + std::string(262144, 'x') + "\n";
  const auto base = parseOrdinary(doc);
  ASSERT_EQ(base.error, ParseError::WrongFieldCount);
  ASSERT_EQ(base.lineNumber, 3U);
  ASSERT_EQ(base.events.size(), 1U);
  expectResultsEqual(parseChunked(doc, {1024}), base);
}

// --- Part 8: I/O failure injection ---

struct FailingParse {
  StreamResult result;
  bool bad = false;
  bool eof = false;
};

FailingParse parseWithFailure(const std::string& doc, std::size_t failAfter) {
  const CsvParser parser{"AAPL"};
  ChunkedBuf buf(doc, {4096});
  buf.failAfter(failAfter);
  std::istream input(&buf);
  FailingParse out;
  out.result = parser.parseStream(input);
  out.bad = input.bad();
  out.eof = input.eof();
  return out;
}

TEST(IoFailure, FailureBeforeAnyBytesLooksLikeEmptyInput) {
  // StreamResult cannot distinguish failure from empty input; the
  // caller-visible difference is the stream state (bad vs eof).
  const auto failed = parseWithFailure(validDoc(), 0);
  EXPECT_EQ(failed.result.error, ParseError::BadHeader);
  EXPECT_EQ(failed.result.lineNumber, 1U);
  EXPECT_TRUE(failed.bad);

  const CsvParser parser{"AAPL"};
  std::istringstream empty("");
  const auto clean = parser.parseStream(empty);
  EXPECT_EQ(clean.error, ParseError::BadHeader);
  EXPECT_FALSE(empty.bad());
}

TEST(IoFailure, FailureAfterCompleteRecordsPreservesPrefix) {
  const std::string doc = validDoc();
  const std::size_t prefix = kHeaderLine.size() + std::string("1000000,N,1,B,100,10\n").size() +
                             std::string("1000001,N,2,A,100,5\n").size();
  const auto failed = parseWithFailure(doc, prefix);
  // No StreamResult error: the failure is indistinguishable from a
  // clean EOF at this point except through the stream state.
  EXPECT_EQ(failed.result.error, ParseError::Ok);
  ASSERT_EQ(failed.result.events.size(), 2U);
  EXPECT_TRUE(failed.bad);

  const auto clean = parseOrdinary(doc.substr(0, prefix));
  EXPECT_EQ(clean.error, ParseError::Ok);
  ASSERT_EQ(clean.events.size(), 2U);
  expectEventsEqual(failed.result.events[0], clean.events[0]);
  expectEventsEqual(failed.result.events[1], clean.events[1]);
}

TEST(IoFailure, FailureMidRecordDiscardsPartialLine) {
  const std::string doc = validDoc();
  const std::size_t midRecord = kHeaderLine.size() + 10; // inside record 1
  const auto failed = parseWithFailure(doc, midRecord);
  EXPECT_EQ(failed.result.error, ParseError::Ok);
  EXPECT_TRUE(failed.result.events.empty()); // partial line not delivered
  EXPECT_TRUE(failed.bad);
}

TEST(IoFailure, ParseErrorPrecedesLaterFailure) {
  const std::string doc = kHeaderLine + "bad,N,1,B,100,10\n" + "1000001,N,2,A,100,5\n";
  // Failure injected after the malformed record's line: the parser
  // stops at the parse error before reaching the failure point.
  const auto failed = parseWithFailure(doc, doc.size());
  EXPECT_EQ(failed.result.error, ParseError::InvalidTimestamp);
  EXPECT_EQ(failed.result.lineNumber, 2U);
}

TEST(IoFailure, ExceptionMaskPropagates) {
  const CsvParser parser{"AAPL"};
  ChunkedBuf buf(validDoc(), {4096});
  buf.failAfter(10);
  std::istream input(&buf);
  input.exceptions(std::ios::badbit);
  // With badbit in the exception mask, the streambuf failure propagates
  // out of parseStream instead of being folded into stream state.
  // The concrete exception type is standard-library dependent
  // (libstdc++ rethrows the streambuf's exception).
  EXPECT_THROW(static_cast<void>(parser.parseStream(input)), std::exception);
}

TEST(IoFailure, ExceptionMaskCleanEofDoesNotThrow) {
  const CsvParser parser{"AAPL"};
  std::istringstream input(validDoc());
  input.exceptions(std::ios::badbit);
  StreamResult result;
  EXPECT_NO_THROW(result = parser.parseStream(input));
  EXPECT_EQ(result.error, ParseError::Ok);
}

// --- Part 10: Parser-to-replay integration ---

class FillCollector : public EventProcessor {
public:
  explicit FillCollector(MatchingEngine& engine) : engine_(engine) {}

  bool onEvent(const Event& event) override {
    const bool ok = engine_.onEvent(event);
    fills_.insert(fills_.end(), engine_.fills().begin(), engine_.fills().end());
    return ok;
  }

  [[nodiscard]] const std::vector<Fill>& fills() const {
    return fills_;
  }

private:
  MatchingEngine& engine_;
  std::vector<Fill> fills_;
};

TEST(StreamReplayIntegration, FragmentedParseReplaysIdentically) {
  const std::string doc =
      kHeaderLine + "1000000,N,1,B,100,10\n" + "1000001,N,2,A,100,6\n" + "1000002,N,3,A,101,3\n";
  const auto base = parseOrdinary(doc);
  ASSERT_EQ(base.error, ParseError::Ok);

  OrderBook base_book;
  MatchingEngine base_engine(base_book);
  FillCollector base_collector(base_engine);
  const auto baseReplay = replayEvents(base.events, base_collector, ReplayMode::Strict);
  ASSERT_TRUE(baseReplay.ok());
  ASSERT_EQ(base_collector.fills().size(), 1U);

  const auto chunked = parseChunked(doc, {1});
  expectResultsEqual(chunked, base);
  OrderBook book;
  MatchingEngine engine(book);
  FillCollector collector(engine);
  const auto replay = replayEvents(chunked.events, collector, ReplayMode::Strict);
  EXPECT_TRUE(replay.ok());
  ASSERT_EQ(collector.fills().size(), base_collector.fills().size());
  const Fill& fill = collector.fills()[0];
  const Fill& base_fill = base_collector.fills()[0];
  EXPECT_EQ(fill.aggressorId.value(), base_fill.aggressorId.value());
  EXPECT_EQ(fill.restingId.value(), base_fill.restingId.value());
  EXPECT_EQ(fill.side, base_fill.side);
  EXPECT_EQ(fill.price.ticks(), base_fill.price.ticks());
  EXPECT_EQ(fill.quantity.lots(), base_fill.quantity.lots());
  EXPECT_EQ(fill.timestamp.count(), base_fill.timestamp.count());
  EXPECT_EQ(fill.sequence.value(), base_fill.sequence.value());
  EXPECT_EQ(book.orderCount(), base_book.orderCount());
  EXPECT_EQ(book.bestBid(), base_book.bestBid());
  EXPECT_EQ(book.bestAsk(), base_book.bestAsk());
}

// --- Part 11: Deterministic property tests ---

TEST(StreamProperty, GeneratedDocsChunkInvariant) {
  for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL, 4ULL, 5ULL}) {
    CsvGeneratorConfig config;
    config.seed = seed;
    CsvGenerator generator(config);
    const std::string doc = generator.generate();
    const StreamResult base = parseOrdinary(doc);
    expectResultsEqual(parseChunked(doc, {1}), base);
    expectResultsEqual(parseChunked(doc, {7}), base);
    DeterministicRng rng(seed);
    std::vector<std::size_t> schedule;
    schedule.reserve(8);
    for (int i = 0; i < 8; ++i) {
      schedule.push_back(rng.nextBounded(31) + 1);
    }
    expectResultsEqual(parseChunked(doc, schedule), base);
  }
}

TEST(StreamProperty, ExtendedStress) {
  if (std::getenv("TICKFORGE_EXTENDED") == nullptr) {
    GTEST_SKIP() << "Set TICKFORGE_EXTENDED=1 to run the extended stress profile.";
  }
  for (std::uint64_t seed = 1; seed <= 50; ++seed) {
    CsvGeneratorConfig config;
    config.seed = seed;
    CsvGenerator generator(config);
    const std::string doc = generator.generate();
    const StreamResult base = parseOrdinary(doc);
    expectResultsEqual(parseChunked(doc, {1}), base);
    expectResultsEqual(parseChunked(doc, {3}), base);
    // Split-at scan on a bounded prefix of positions.
    for (std::size_t split = 0; split <= std::min<std::size_t>(doc.size(), 120); ++split) {
      expectResultsEqual(parseChunked(doc, {split, doc.size()}), base);
    }
  }
}

} // namespace
} // namespace tickforge::test
