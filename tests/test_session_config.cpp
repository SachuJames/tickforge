// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Session configuration tests (SPEC.md sections 10 and 9.6):
// construction, validation, canonical serialization, deserialization,
// deterministic hashing, and result metadata.

#include "tickforge/book/order_book.hpp"
#include "tickforge/event/event.hpp"
#include "tickforge/matching/matching_engine.hpp"
#include "tickforge/replay/event_processor.hpp"
#include "tickforge/replay/replay.hpp"
#include "tickforge/replay/session_config.hpp"
#include "tickforge/version.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tickforge::ConfigError;
using tickforge::ConfigParseError;
using tickforge::Event;
using tickforge::EventProcessor;
using tickforge::EventType;
using tickforge::MatchingRule;
using tickforge::OrderId;
using tickforge::Price;
using tickforge::Quantity;
using tickforge::ReplayError;
using tickforge::ReplayMode;
using tickforge::ReplayResult;
using tickforge::Sequence;
using tickforge::SessionConfig;
using tickforge::Side;
using tickforge::Timestamp;

SessionConfig makeConfig() {
  SessionConfig config;
  config.instrument = "AAPL";
  config.tickSize = "0.01";
  config.lotSize = "1";
  return config;
}

TEST(SessionConfigTest, MinimalValidConfig) {
  SessionConfig config;
  config.instrument = "AAPL";
  EXPECT_EQ(tickforge::validateConfig(config), ConfigError::Ok);
  // Documented defaults.
  EXPECT_EQ(config.matchingRule, MatchingRule::PriceTimePriority);
  EXPECT_EQ(config.mode, ReplayMode::Strict);
  EXPECT_FALSE(config.tickSize.has_value());
  EXPECT_FALSE(config.lotSize.has_value());
  EXPECT_FALSE(config.prngSeed.has_value());
}

TEST(SessionConfigTest, FullValidConfig) {
  SessionConfig config = makeConfig();
  config.prngSeed = 42U;
  config.mode = ReplayMode::Lenient;
  EXPECT_EQ(tickforge::validateConfig(config), ConfigError::Ok);
}

TEST(SessionConfigTest, EmptyInstrumentRejected) {
  const SessionConfig config;
  EXPECT_EQ(tickforge::validateConfig(config), ConfigError::EmptyInstrument);
}

TEST(SessionConfigTest, UnserializableInstrumentRejected) {
  SessionConfig config;
  config.instrument = "AA=PL";
  EXPECT_EQ(tickforge::validateConfig(config), ConfigError::InvalidInstrument);
  config.instrument = "AA\nPL";
  EXPECT_EQ(tickforge::validateConfig(config), ConfigError::InvalidInstrument);
}

TEST(SessionConfigTest, InvalidTickSizeRejected) {
  SessionConfig config = makeConfig();
  for (const char* bad : {"", "abc", ".5", "1.", "1.2.3", " 1", "1 "}) {
    config.tickSize = bad;
    EXPECT_EQ(tickforge::validateConfig(config), ConfigError::InvalidTickSize) << bad;
  }
}

TEST(SessionConfigTest, ValidTickSizeAccepted) {
  SessionConfig config = makeConfig();
  for (const char* good : {"1", "0.01", "100.5"}) {
    config.tickSize = good;
    EXPECT_EQ(tickforge::validateConfig(config), ConfigError::Ok) << good;
  }
}

TEST(SessionConfigTest, InvalidLotSizeRejected) {
  SessionConfig config = makeConfig();
  config.lotSize = "lots";
  EXPECT_EQ(tickforge::validateConfig(config), ConfigError::InvalidLotSize);
}

TEST(SessionConfigTest, Equality) {
  EXPECT_EQ(makeConfig(), makeConfig());
  SessionConfig other = makeConfig();
  other.instrument = "MSFT";
  EXPECT_NE(makeConfig(), other);
  other = makeConfig();
  other.mode = ReplayMode::Lenient;
  EXPECT_NE(makeConfig(), other);
  // Absent vs present optional is a different configuration.
  other = makeConfig();
  other.tickSize = "0.02";
  EXPECT_NE(makeConfig(), other);
}

TEST(SessionConfigTest, CanonicalSerializationExactBytes) {
  const std::string expected = "tickforge-config/1\n"
                               "instrument=AAPL\n"
                               "lot-size=1\n"
                               "matching-rule=price-time-priority\n"
                               "mode=strict\n"
                               "tick-size=0.01\n";
  EXPECT_EQ(tickforge::serializeConfig(makeConfig()), expected);
}

TEST(SessionConfigTest, AbsentOptionalsOmitted) {
  SessionConfig config;
  config.instrument = "AAPL";
  const std::string expected = "tickforge-config/1\n"
                               "instrument=AAPL\n"
                               "matching-rule=price-time-priority\n"
                               "mode=strict\n";
  EXPECT_EQ(tickforge::serializeConfig(config), expected);
}

TEST(SessionConfigTest, PrngSeedSerialized) {
  SessionConfig config;
  config.instrument = "AAPL";
  config.prngSeed = 12345678901234567890ULL;
  const std::string serialized = tickforge::serializeConfig(config);
  EXPECT_NE(serialized.find("prng-seed=12345678901234567890\n"), std::string::npos);
}

TEST(SessionConfigTest, RoundTrip) {
  const SessionConfig config = makeConfig();
  const auto parsed = tickforge::deserializeConfig(tickforge::serializeConfig(config));
  EXPECT_EQ(parsed.error, ConfigParseError::Ok);
  EXPECT_EQ(parsed.config, config);
}

TEST(SessionConfigTest, RoundTripMinimal) {
  SessionConfig config;
  config.instrument = "AAPL";
  config.mode = ReplayMode::Lenient;
  const auto parsed = tickforge::deserializeConfig(tickforge::serializeConfig(config));
  EXPECT_EQ(parsed.error, ConfigParseError::Ok);
  EXPECT_EQ(parsed.config, config);
}

TEST(SessionConfigTest, DeserializeBadSchema) {
  EXPECT_EQ(tickforge::deserializeConfig("").error, ConfigParseError::BadSchema);
  EXPECT_EQ(tickforge::deserializeConfig("tickforge-config/2\ninstrument=AAPL\n").error,
            ConfigParseError::BadSchema);
  EXPECT_EQ(tickforge::deserializeConfig("instrument=AAPL\n").error, ConfigParseError::BadSchema);
}

TEST(SessionConfigTest, DeserializeMalformedLine) {
  EXPECT_EQ(tickforge::deserializeConfig("tickforge-config/1\ninstrument\n").error,
            ConfigParseError::MalformedLine);
  EXPECT_EQ(tickforge::deserializeConfig("tickforge-config/1\ninstrument=A=B\n").error,
            ConfigParseError::MalformedLine);
  // Trailing content without a newline is not canonical.
  EXPECT_EQ(tickforge::deserializeConfig("tickforge-config/1\ninstrument=AAPL").error,
            ConfigParseError::MalformedLine);
}

TEST(SessionConfigTest, DeserializeDuplicateField) {
  const std::string text = "tickforge-config/1\ninstrument=AAPL\ninstrument=MSFT\n";
  EXPECT_EQ(tickforge::deserializeConfig(text).error, ConfigParseError::DuplicateField);
}

TEST(SessionConfigTest, DeserializeUnknownField) {
  const std::string text = "tickforge-config/1\ninstrument=AAPL\nlatency=5\n";
  EXPECT_EQ(tickforge::deserializeConfig(text).error, ConfigParseError::UnknownField);
}

TEST(SessionConfigTest, DeserializeMissingInstrument) {
  const std::string text = "tickforge-config/1\nmode=strict\n";
  EXPECT_EQ(tickforge::deserializeConfig(text).error, ConfigParseError::MissingField);
}

TEST(SessionConfigTest, DeserializeInvalidValue) {
  EXPECT_EQ(tickforge::deserializeConfig("tickforge-config/1\ninstrument=AAPL\nmode=chaos\n").error,
            ConfigParseError::InvalidValue);
  EXPECT_EQ(
      tickforge::deserializeConfig("tickforge-config/1\ninstrument=AAPL\ntick-size=abc\n").error,
      ConfigParseError::InvalidValue);
  EXPECT_EQ(
      tickforge::deserializeConfig("tickforge-config/1\ninstrument=AAPL\nprng-seed=xyz\n").error,
      ConfigParseError::InvalidValue);
  // Numeric overflow on the seed.
  EXPECT_EQ(tickforge::deserializeConfig(
                "tickforge-config/1\ninstrument=AAPL\nprng-seed=99999999999999999999999\n")
                .error,
            ConfigParseError::InvalidValue);
}

TEST(SessionConfigTest, HashKnownAnswer) {
  // Independent FNV-1a 64-bit computation (Python) over the canonical
  // bytes of makeConfig().
  EXPECT_EQ(tickforge::hashConfig(makeConfig()), 0xa8d183ecd7db2b76ULL);
}

TEST(SessionConfigTest, HashDeterministic) {
  EXPECT_EQ(tickforge::hashConfig(makeConfig()), tickforge::hashConfig(makeConfig()));
}

TEST(SessionConfigTest, HashChangesWithExecutionRelevantField) {
  const std::uint64_t base = tickforge::hashConfig(makeConfig());
  SessionConfig changed = makeConfig();
  changed.mode = ReplayMode::Lenient;
  EXPECT_NE(tickforge::hashConfig(changed), base);
  changed = makeConfig();
  changed.instrument = "MSFT";
  EXPECT_NE(tickforge::hashConfig(changed), base);
}

TEST(SessionConfigTest, HashOmitsAbsentOptionals) {
  SessionConfig config;
  config.instrument = "AAPL";
  // Absent tickSize hashes as absent, not as empty.
  EXPECT_NE(tickforge::hashConfig(config), tickforge::hashConfig(makeConfig()));
}

TEST(SessionConfigTest, FormatConfigHash) {
  EXPECT_EQ(tickforge::formatConfigHash(0xa8d183ecd7db2b76ULL), "a8d183ecd7db2b76");
  EXPECT_EQ(tickforge::formatConfigHash(0ULL), "0000000000000000");
}

TEST(SessionConfigTest, EnumToString) {
  EXPECT_EQ(tickforge::toString(MatchingRule::PriceTimePriority), "PriceTimePriority");
  EXPECT_EQ(tickforge::toString(ConfigError::Ok), "Ok");
  EXPECT_EQ(tickforge::toString(ConfigError::EmptyInstrument), "EmptyInstrument");
  EXPECT_EQ(tickforge::toString(ConfigParseError::Ok), "Ok");
  EXPECT_EQ(tickforge::toString(ConfigParseError::BadSchema), "BadSchema");
  EXPECT_EQ(tickforge::toString(ReplayError::InvalidConfig), "InvalidConfig");
}

// --- Replay integration ---

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

class RecordingProcessor : public EventProcessor {
public:
  bool onEvent(const Event& event) override {
    seen.push_back(event.sequence);
    return true;
  }
  std::vector<Sequence> seen;
};

TEST(SessionConfigReplayTest, MetadataEmbedded) {
  RecordingProcessor processor;
  const SessionConfig config = makeConfig();
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
  };
  const ReplayResult result = tickforge::replayEvents(events, processor, config);

  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.version, tickforge::kVersion);
  EXPECT_EQ(result.configHash, std::optional<std::uint64_t>(tickforge::hashConfig(config)));
  EXPECT_EQ(result.mode, ReplayMode::Strict);
  EXPECT_EQ(processor.seen.size(), 1U);
}

TEST(SessionConfigReplayTest, PlainReplayHasNoConfigHash) {
  RecordingProcessor processor;
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
  };
  const ReplayResult result = tickforge::replayEvents(events, processor);

  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.version, tickforge::kVersion);
  EXPECT_FALSE(result.configHash.has_value());
}

TEST(SessionConfigReplayTest, InvalidConfigRejectedBeforeProcessing) {
  RecordingProcessor processor;
  const SessionConfig config; // empty instrument: invalid
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
  };
  const ReplayResult result = tickforge::replayEvents(events, processor, config);

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error, ReplayError::InvalidConfig);
  EXPECT_FALSE(result.configHash.has_value());
  EXPECT_TRUE(processor.seen.empty());
}

TEST(SessionConfigReplayTest, InstrumentMismatch) {
  RecordingProcessor processor;
  SessionConfig config = makeConfig();
  config.instrument = "MSFT";
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
  };
  const ReplayResult result = tickforge::replayEvents(events, processor, config);

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error, ReplayError::InstrumentMismatch);
  EXPECT_TRUE(processor.seen.empty());
}

TEST(SessionConfigReplayTest, ConfigModeRespected) {
  // Lenient config skips the invalid event; strict config aborts.
  Event bad = makeEvent(Timestamp{200}, Sequence{1}, EventType::NewOrder, OrderId{2});
  bad.quantity = Quantity{0};
  const std::vector<Event> events = {
      makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
      bad,
      makeEvent(Timestamp{300}, Sequence{2}, EventType::NewOrder, OrderId{3}),
  };

  const SessionConfig strictConfig = makeConfig();
  RecordingProcessor strict_processor;
  const ReplayResult strictResult = tickforge::replayEvents(events, strict_processor, strictConfig);
  EXPECT_FALSE(strictResult.ok());
  EXPECT_EQ(strictResult.error, ReplayError::InvalidEvent);

  SessionConfig lenient_config = makeConfig();
  lenient_config.mode = ReplayMode::Lenient;
  RecordingProcessor lenient_processor;
  const ReplayResult lenientResult =
      tickforge::replayEvents(events, lenient_processor, lenient_config);
  EXPECT_TRUE(lenientResult.ok());
  EXPECT_EQ(lenientResult.skippedCount, 1U);
  EXPECT_EQ(lenientResult.mode, ReplayMode::Lenient);
  EXPECT_EQ(lenientResult.configHash,
            std::optional<std::uint64_t>(tickforge::hashConfig(lenient_config)));
  EXPECT_EQ(lenient_processor.seen.size(), 2U);
}

TEST(SessionConfigReplayTest, MetadataDeterministic) {
  const auto runOnce = []() {
    RecordingProcessor processor;
    const std::vector<Event> events = {
        makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1}),
        makeEvent(Timestamp{200}, Sequence{1}, EventType::NewOrder, OrderId{2}),
    };
    return tickforge::replayEvents(events, processor, makeConfig());
  };
  const ReplayResult first = runOnce();
  const ReplayResult second = runOnce();
  EXPECT_TRUE(first.ok());
  EXPECT_TRUE(second.ok());
  EXPECT_EQ(first.configHash, second.configHash);
  EXPECT_EQ(first.version, second.version);
}

TEST(SessionConfigReplayTest, ConfiguredReplayMatchesPlainReplay) {
  // A crossing order: the buy lifts the resting ask. Metadata must not
  // alter matching behavior.
  const auto crossingEvents = []() {
    Event ask = makeEvent(Timestamp{100}, Sequence{0}, EventType::NewOrder, OrderId{1});
    ask.side = Side::Ask;
    ask.price = Price{100};
    Event buy = makeEvent(Timestamp{200}, Sequence{1}, EventType::NewOrder, OrderId{2});
    buy.price = Price{100};
    return std::vector<Event>{ask, buy};
  };

  tickforge::OrderBook plain_book;
  tickforge::MatchingEngine plain_engine(plain_book);
  const std::vector<Event> plainEvents = crossingEvents();
  const ReplayResult plainResult = tickforge::replayEvents(plainEvents, plain_engine);
  ASSERT_TRUE(plainResult.ok());

  tickforge::OrderBook configured_book;
  tickforge::MatchingEngine configured_engine(configured_book);
  const std::vector<Event> configuredEvents = crossingEvents();
  const ReplayResult configuredResult =
      tickforge::replayEvents(configuredEvents, configured_engine, makeConfig());
  ASSERT_TRUE(configuredResult.ok());

  // Identical fills.
  ASSERT_EQ(plain_engine.fills().size(), configured_engine.fills().size());
  // Identical final book state.
  EXPECT_EQ(plain_engine.book().orderCount(), configured_engine.book().orderCount());
  // Metadata present only on the configured path.
  EXPECT_FALSE(plainResult.configHash.has_value());
  ASSERT_TRUE(configuredResult.configHash.has_value());
}

} // namespace
