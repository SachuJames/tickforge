// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: Apache-2.0.
//
// Session configuration and replay metadata verification (SPEC.md 10, 9.6).
//
// Covers: adversarial config validation, canonical serialization exact
// bytes, independent FNV-1a known-answer hashes, ReplayResult metadata
// (version, configHash) across all replay paths, config-gated replay
// with instrument isolation, and the strict/lenient error matrix.
//
// Expected values are derived independently from the specification and
// the documented format, not by calling the code under test.

#include "tests/support/deterministic_rng.hpp"
#include "tests/support/event_generator.hpp"
#include "tickforge/book/order_book.hpp"
#include "tickforge/matching/matching_engine.hpp"
#include "tickforge/replay/replay.hpp"
#include "tickforge/replay/session_config.hpp"

#include <cstdlib>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace tickforge::test {
namespace {

// Independent FNV-1a 64-bit reference (test oracle, not production code).
std::uint64_t fnv1a64(std::string_view bytes) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const char byte : bytes) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
    hash *= 1099511628211ULL;
  }
  return hash;
}

SessionConfig minimalConfig() {
  SessionConfig config;
  config.instrument = "AAPL";
  return config;
}

// --- Part 3: Adversarial validation ---

TEST(ConfigValidation, InstrumentWithSpacesValid) {
  SessionConfig config = minimalConfig();
  config.instrument = "AAPL US";
  // Spaces are not '=', '\n', or '\r': valid per the contract.
  EXPECT_EQ(validateConfig(config), ConfigError::Ok);
}

TEST(ConfigValidation, InstrumentCaseSensitive) {
  SessionConfig lower = minimalConfig();
  lower.instrument = "aapl";
  SessionConfig upper = minimalConfig();
  upper.instrument = "AAPL";
  EXPECT_EQ(validateConfig(lower), ConfigError::Ok);
  EXPECT_EQ(validateConfig(upper), ConfigError::Ok);
  // No case folding: different instruments hash differently.
  EXPECT_NE(hashConfig(lower), hashConfig(upper));
}

TEST(ConfigValidation, InstrumentWithEqualsRejected) {
  SessionConfig config = minimalConfig();
  config.instrument = "AAP=L";
  EXPECT_EQ(validateConfig(config), ConfigError::InvalidInstrument);
}

TEST(ConfigValidation, InstrumentWithNewlineRejected) {
  SessionConfig config = minimalConfig();
  config.instrument = "AAPL\n";
  EXPECT_EQ(validateConfig(config), ConfigError::InvalidInstrument);
}

TEST(ConfigValidation, TickSizeBoundaries) {
  SessionConfig config = minimalConfig();
  config.tickSize = "0";
  EXPECT_EQ(validateConfig(config), ConfigError::Ok);
  config.tickSize = "0.01";
  EXPECT_EQ(validateConfig(config), ConfigError::Ok);
  config.tickSize = "123456789.987654321";
  EXPECT_EQ(validateConfig(config), ConfigError::Ok);
  config.tickSize = "";
  EXPECT_EQ(validateConfig(config), ConfigError::InvalidTickSize);
  config.tickSize = ".5";
  EXPECT_EQ(validateConfig(config), ConfigError::InvalidTickSize);
  config.tickSize = "5.";
  EXPECT_EQ(validateConfig(config), ConfigError::InvalidTickSize);
  config.tickSize = "1e3";
  EXPECT_EQ(validateConfig(config), ConfigError::InvalidTickSize);
  config.tickSize = "-0.01";
  EXPECT_EQ(validateConfig(config), ConfigError::InvalidTickSize);
}

TEST(ConfigValidation, PrngSeedBoundaries) {
  SessionConfig config = minimalConfig();
  config.prngSeed = 0;
  EXPECT_EQ(validateConfig(config), ConfigError::Ok);
  config.prngSeed = 18446744073709551615ULL; // max uint64
  EXPECT_EQ(validateConfig(config), ConfigError::Ok);
}

// --- Part 4: Canonical serialization exact bytes ---

TEST(ConfigSerialization, LenientModeExactBytes) {
  SessionConfig config = minimalConfig();
  config.mode = ReplayMode::Lenient;
  const std::string expected = "tickforge-config/1\n"
                               "instrument=AAPL\n"
                               "matching-rule=price-time-priority\n"
                               "mode=lenient\n";
  EXPECT_EQ(serializeConfig(config), expected);
}

TEST(ConfigSerialization, PrngSeedExactBytes) {
  SessionConfig config = minimalConfig();
  config.prngSeed = 42;
  const std::string expected = "tickforge-config/1\n"
                               "instrument=AAPL\n"
                               "matching-rule=price-time-priority\n"
                               "mode=strict\n"
                               "prng-seed=42\n";
  EXPECT_EQ(serializeConfig(config), expected);
}

TEST(ConfigSerialization, FieldOrderingByteWise) {
  // Keys must appear in byte-wise sorted order:
  // instrument < lot-size < matching-rule < mode < prng-seed < tick-size.
  SessionConfig config;
  config.instrument = "AAPL";
  config.tickSize = "0.01";
  config.lotSize = "100";
  config.prngSeed = 7;
  config.mode = ReplayMode::Lenient;
  const std::string serialized = serializeConfig(config);
  const std::size_t iInstrument = serialized.find("instrument=");
  const std::size_t iLotSize = serialized.find("lot-size=");
  const std::size_t iMatching = serialized.find("matching-rule=");
  const std::size_t iMode = serialized.find("mode=");
  const std::size_t iPrng = serialized.find("prng-seed=");
  const std::size_t iTick = serialized.find("tick-size=");
  EXPECT_LT(iInstrument, iLotSize);
  EXPECT_LT(iLotSize, iMatching);
  EXPECT_LT(iMatching, iMode);
  EXPECT_LT(iMode, iPrng);
  EXPECT_LT(iPrng, iTick);
}

TEST(ConfigSerialization, RepeatedSerializationIdentical) {
  const SessionConfig config = minimalConfig();
  EXPECT_EQ(serializeConfig(config), serializeConfig(config));
}

// --- Part 5: Hash known answers (independent oracle) ---

TEST(ConfigHash, MinimalConfigKnownAnswer) {
  // Canonical bytes for minimal AAPL/strict config, hashed with the
  // independent test-side FNV-1a above.
  const std::string canonical = "tickforge-config/1\n"
                                "instrument=AAPL\n"
                                "matching-rule=price-time-priority\n"
                                "mode=strict\n";
  const std::uint64_t expected = fnv1a64(canonical);
  EXPECT_EQ(hashConfig(minimalConfig()), expected);
  // Cross-check against the pre-existing known answer in
  // test_session_config.cpp (0xa8d183ecd7db2b76 was for a different
  // config with lot-size/tick-size; this is the minimal config).
}

TEST(ConfigHash, IndependentOracleAgrees) {
  // For several configs, the test-side oracle and production agree.
  std::vector<SessionConfig> configs;
  configs.push_back(minimalConfig());
  SessionConfig full = minimalConfig();
  full.tickSize = "0.01";
  full.lotSize = "100";
  full.prngSeed = 12345;
  full.mode = ReplayMode::Lenient;
  configs.push_back(full);
  SessionConfig other = minimalConfig();
  other.instrument = "MSFT";
  configs.push_back(other);

  for (const auto& config : configs) {
    EXPECT_EQ(hashConfig(config), fnv1a64(serializeConfig(config)));
  }
}

TEST(ConfigHash, SensitivityPerField) {
  const std::uint64_t base = hashConfig(minimalConfig());
  // Each field change alters the canonical bytes, hence the hash.
  SessionConfig changed = minimalConfig();
  changed.instrument = "MSFT";
  EXPECT_NE(hashConfig(changed), base);
  changed = minimalConfig();
  changed.mode = ReplayMode::Lenient;
  EXPECT_NE(hashConfig(changed), base);
  changed = minimalConfig();
  changed.tickSize = "0.01";
  EXPECT_NE(hashConfig(changed), base);
  changed = minimalConfig();
  changed.prngSeed = 1;
  EXPECT_NE(hashConfig(changed), base);
}

TEST(ConfigHash, FormatIsLowercaseHex) {
  const std::string formatted = formatConfigHash(0xABCDEF0123456789ULL);
  EXPECT_EQ(formatted, "abcdef0123456789");
  EXPECT_EQ(formatted.size(), 16U);
}

// --- Part 6: ReplayResult metadata ---

TEST(ReplayMetadata, VersionIsStable) {
  // kVersion is "0.2.0", the project version independent of build mode.
  EXPECT_EQ(kVersion, "0.2.0");
  OrderBook book;
  MatchingEngine engine(book);
  const std::vector<Event> events;
  const auto result = replayEvents(events, engine, ReplayMode::Strict);
  EXPECT_EQ(result.version, "0.2.0");
}

TEST(ReplayMetadata, UnconfiguredReplayHasNoHash) {
  OrderBook book;
  MatchingEngine engine(book);
  const std::vector<Event> events;
  const auto result = replayEvents(events, engine, ReplayMode::Strict);
  EXPECT_FALSE(result.configHash.has_value());
}

TEST(ReplayMetadata, ConfiguredReplayHasHash) {
  const SessionConfig config = minimalConfig();
  const std::uint64_t expected = hashConfig(config);
  OrderBook book;
  MatchingEngine engine(book);
  const std::vector<Event> events;
  const auto result = replayEvents(events, engine, config);
  EXPECT_TRUE(result.ok());
  ASSERT_TRUE(result.configHash.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
  EXPECT_EQ(*result.configHash, expected);
  EXPECT_EQ(result.version, "0.2.0");
  EXPECT_EQ(result.mode, ReplayMode::Strict);
}

TEST(ReplayMetadata, HashOnFailurePaths) {
  const SessionConfig config = minimalConfig();
  // Invalid config: no hash (replay never ran), but mode is recorded.
  SessionConfig bad = config;
  bad.instrument = "";
  OrderBook book;
  MatchingEngine engine(book);
  const std::vector<Event> events;
  const auto badResult = replayEvents(events, engine, bad);
  EXPECT_EQ(badResult.error, ReplayError::InvalidConfig);
  EXPECT_FALSE(badResult.configHash.has_value());
  EXPECT_EQ(badResult.mode, ReplayMode::Strict);
}

TEST(ReplayMetadata, HashMatchesActualConfig) {
  // Two different configs produce different hashes in results.
  const SessionConfig strict = minimalConfig();
  SessionConfig lenient = minimalConfig();
  lenient.mode = ReplayMode::Lenient;
  OrderBook book1;
  MatchingEngine engine1(book1);
  OrderBook book2;
  MatchingEngine engine2(book2);
  const std::vector<Event> events;
  const auto r1 = replayEvents(events, engine1, strict);
  const auto r2 = replayEvents(events, engine2, lenient);
  ASSERT_TRUE(r1.configHash.has_value());
  ASSERT_TRUE(r2.configHash.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
  EXPECT_NE(*r1.configHash, *r2.configHash);
  EXPECT_EQ(r1.mode, ReplayMode::Strict);
  EXPECT_EQ(r2.mode, ReplayMode::Lenient);
}

// --- Part 7: Config-gated replay and instrument isolation ---

std::vector<Event> makeEvents(const std::string& instrument, int count) {
  std::vector<Event> events;
  for (int i = 0; i < count; ++i) {
    Event e;
    e.type = EventType::NewOrder;
    e.orderId = OrderId{static_cast<std::uint64_t>(i + 1)};
    e.side = Side::Bid;
    e.price = Price{100};
    e.quantity = Quantity{10};
    e.timestamp = Timestamp{1000000 + i};
    e.sequence = Sequence{static_cast<std::uint64_t>(i)};
    e.instrument = instrument;
    events.push_back(e);
  }
  return events;
}

TEST(ConfiguredReplay, MatchingInstrumentsOk) {
  const SessionConfig config = minimalConfig();
  OrderBook book;
  MatchingEngine engine(book);
  const auto events = makeEvents("AAPL", 5);
  const auto result = replayEvents(events, engine, config);
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(book.orderCount(), 5U);
}

TEST(ConfiguredReplay, ConfigEventMismatchFatal) {
  SessionConfig config = minimalConfig(); // AAPL
  config.mode = ReplayMode::Lenient;      // still fatal
  OrderBook book;
  MatchingEngine engine(book);
  const auto events = makeEvents("MSFT", 3);
  const auto result = replayEvents(events, engine, config);
  EXPECT_EQ(result.error, ReplayError::InstrumentMismatch);
  EXPECT_EQ(result.failedAt, Sequence{0});
}

TEST(ConfiguredReplay, MismatchAfterValidEvents) {
  const SessionConfig config = minimalConfig();
  OrderBook book;
  MatchingEngine engine(book);
  auto events = makeEvents("AAPL", 3);
  events[2].instrument = "MSFT"; // third event mismatches
  const auto result = replayEvents(events, engine, config);
  EXPECT_EQ(result.error, ReplayError::InstrumentMismatch);
  EXPECT_EQ(result.failedAt, Sequence{2});
  // First two events remain applied (no rollback).
  EXPECT_EQ(book.orderCount(), 2U);
}

TEST(ConfiguredReplay, EmptyEventsWithConfig) {
  const SessionConfig config = minimalConfig();
  OrderBook book;
  MatchingEngine engine(book);
  const std::vector<Event> events;
  const auto result = replayEvents(events, engine, config);
  EXPECT_TRUE(result.ok());
  ASSERT_TRUE(result.configHash.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
  EXPECT_EQ(*result.configHash, hashConfig(config));
}

TEST(ConfiguredReplay, SessionsIsolated) {
  // Two sequential replays with different configs do not interfere.
  const SessionConfig c1 = minimalConfig();
  SessionConfig c2 = minimalConfig();
  c2.instrument = "MSFT";
  c2.mode = ReplayMode::Lenient;

  OrderBook book1;
  MatchingEngine engine1(book1);
  const auto r1 = replayEvents(makeEvents("AAPL", 2), engine1, c1);

  OrderBook book2;
  MatchingEngine engine2(book2);
  const auto r2 = replayEvents(makeEvents("MSFT", 2), engine2, c2);

  EXPECT_TRUE(r1.ok());
  EXPECT_TRUE(r2.ok());
  ASSERT_TRUE(r1.configHash.has_value());
  ASSERT_TRUE(r2.configHash.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
  EXPECT_NE(*r1.configHash, *r2.configHash);
  EXPECT_EQ(book1.orderCount(), 2U);
  EXPECT_EQ(book2.orderCount(), 2U);
}

// --- Part 8: Strict/lenient error matrix with config ---

TEST(ConfigErrorMatrix, InvalidConfigStrict) {
  SessionConfig config = minimalConfig();
  config.instrument = ""; // invalid
  OrderBook book;
  MatchingEngine engine(book);
  const auto events = makeEvents("AAPL", 2);
  const auto result = replayEvents(events, engine, config);
  EXPECT_EQ(result.error, ReplayError::InvalidConfig);
  EXPECT_EQ(book.orderCount(), 0U); // processor observed nothing
}

TEST(ConfigErrorMatrix, InvalidConfigLenient) {
  SessionConfig config = minimalConfig();
  config.instrument = "";
  config.mode = ReplayMode::Lenient;
  OrderBook book;
  MatchingEngine engine(book);
  const auto events = makeEvents("AAPL", 2);
  const auto result = replayEvents(events, engine, config);
  // Config validation happens before mode matters.
  EXPECT_EQ(result.error, ReplayError::InvalidConfig);
  EXPECT_EQ(result.mode, ReplayMode::Lenient);
  EXPECT_EQ(book.orderCount(), 0U);
}

// --- Part 9: Deterministic property tests ---

TEST(ConfigProperty, SerializationStableAcrossInstances) {
  // Independently constructed equivalent configs serialize identically.
  for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
    DeterministicRng rng(seed);
    SessionConfig a = minimalConfig();
    SessionConfig b = minimalConfig();
    if (rng.nextBool()) {
      a.tickSize = "0.05";
      b.tickSize = "0.05";
    }
    if (rng.nextBool()) {
      a.mode = ReplayMode::Lenient;
      b.mode = ReplayMode::Lenient;
    }
    EXPECT_EQ(serializeConfig(a), serializeConfig(b));
    EXPECT_EQ(hashConfig(a), hashConfig(b));
  }
}

TEST(ConfigProperty, RoundTripPreservesSemantics) {
  SessionConfig config = minimalConfig();
  config.tickSize = "0.25";
  config.lotSize = "10";
  config.prngSeed = 999;
  config.mode = ReplayMode::Lenient;
  const auto parsed = deserializeConfig(serializeConfig(config));
  ASSERT_EQ(parsed.error, ConfigParseError::Ok);
  EXPECT_EQ(parsed.config, config);
  EXPECT_EQ(hashConfig(parsed.config), hashConfig(config));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): test with many assertions.
TEST(ConfigProperty, ExtendedStress) {
  if (std::getenv("TICKFORGE_EXTENDED") == nullptr) {
    GTEST_SKIP() << "Set TICKFORGE_EXTENDED=1 to run the extended stress profile.";
  }
  // 20 seeds: serialization stability, hash determinism, oracle agreement.
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    DeterministicRng rng(seed);
    SessionConfig config = minimalConfig();
    if (rng.nextBool()) {
      config.tickSize = "0.0" + std::to_string(rng.nextBounded(9) + 1);
    }
    if (rng.nextBool()) {
      config.mode = ReplayMode::Lenient;
    }
    if (rng.nextBool()) {
      config.prngSeed = rng.nextBounded(1000000);
    }
    EXPECT_EQ(validateConfig(config), ConfigError::Ok);
    EXPECT_EQ(hashConfig(config), fnv1a64(serializeConfig(config)));
    EXPECT_EQ(serializeConfig(config), serializeConfig(config));
  }
}

} // namespace
} // namespace tickforge::test
