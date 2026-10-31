// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: Apache-2.0.
//
// Latency model tests (SPEC.md 15) and production SplitMix64 known
// answers. Expected values were derived independently of the C++
// implementation, from the published splitmix64 algorithm computed
// in a separate implementation, then mapped through the section
// 14.2 modulo rule by hand-checkable arithmetic.

#include "tests/support/event_generator.hpp"
#include "tickforge/analytics/execution_statistics.hpp"
#include "tickforge/book/order_book.hpp"
#include "tickforge/core/splitmix64.hpp"
#include "tickforge/matching/matching_engine.hpp"
#include "tickforge/model/latency_model.hpp"
#include "tickforge/model/latency_observer.hpp"
#include "tickforge/replay/replay.hpp"
#include "tickforge/replay/session_config.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace tickforge::test {
namespace {

Fill makeFill(std::uint64_t i) {
  Fill fill;
  fill.aggressorId = OrderId{100 + i};
  fill.restingId = OrderId{200 + i};
  fill.side = Side::Bid;
  fill.price = Price{100};
  fill.quantity = Quantity{3};
  fill.timestamp = Timestamp{1000000 + static_cast<std::int64_t>(i)};
  fill.sequence = Sequence{i};
  return fill;
}

// ---------------------------------------------------------------------
// Production SplitMix64 known answers (same vectors as SPEC.md 14)
// ---------------------------------------------------------------------

TEST(ProductionSplitMix64, MatchesDocumentedStreams) {
  {
    SplitMix64 rng(0);
    EXPECT_EQ(rng.next(), 0xE220A8397B1DCDAFULL);
    EXPECT_EQ(rng.next(), 0x6E789E6AA1B965F4ULL);
    EXPECT_EQ(rng.next(), 0x06C45D188009454FULL);
  }
  {
    SplitMix64 rng(1901);
    EXPECT_EQ(rng.next(), 0xB4D5251375829524ULL);
    EXPECT_EQ(rng.next(), 0x929CAEA9D48DCE64ULL);
    EXPECT_EQ(rng.next(), 0x43CFE2BDB6D4F19EULL);
  }
  {
    SplitMix64 rng(UINT64_MAX);
    EXPECT_EQ(rng.next(), 0xE4D971771B652C20ULL);
    EXPECT_EQ(rng.next(), 0xE99FF867DBF682C9ULL);
  }
  {
    SplitMix64 rng(1901);
    EXPECT_EQ(rng.nextBounded(100), 76ULL);
    EXPECT_EQ(rng.nextRange(95, 105), 98ULL); // second draw of the stream
  }
}

// ---------------------------------------------------------------------
// Latency mapping known answers
// ---------------------------------------------------------------------

TEST(LatencyModel, KnownAnswerSamplesSeed42) {
  LatencyModel model(LatencyModelConfig{42, 1000, 1999});
  const std::int64_t expected[] = {1413, 1291, 1858, 1764, 1250, 1062};
  for (std::uint64_t i = 0; i < 6; ++i) {
    EXPECT_EQ(model.observeFill(makeFill(i)), expected[i]) << "fill " << i;
  }
  EXPECT_EQ(model.sampleCount(), 6U);
}

TEST(LatencyModel, KnownAnswerSamplesFullRangeSeed7) {
  // Bounds [0, INT64_MAX]: span is 2^63, exercising the widest
  // interval the type admits without overflow.
  LatencyModel model(LatencyModelConfig{7, 0, INT64_MAX});
  EXPECT_EQ(model.observeFill(makeFill(0)), 7191089600892374487LL);
  EXPECT_EQ(model.observeFill(makeFill(1)), 309689372594955804LL);
  EXPECT_EQ(model.observeFill(makeFill(2)), 7392729709960833538LL);
}

TEST(LatencyModel, EqualBoundsAlwaysReturnTheBound) {
  LatencyModel model(LatencyModelConfig{99, 5000, 5000});
  for (std::uint64_t i = 0; i < 8; ++i) {
    EXPECT_EQ(model.observeFill(makeFill(i)), 5000);
  }
}

TEST(LatencyModel, NoFillsNoSamples) {
  LatencyModel model(LatencyModelConfig{1, 10, 20});
  EXPECT_EQ(model.sampleCount(), 0U);
  EXPECT_TRUE(model.artifact().samplesNs.empty());
}

TEST(LatencyModel, SamplesStayWithinInclusiveBounds) {
  LatencyModel model(LatencyModelConfig{123, 250, 275});
  for (std::uint64_t i = 0; i < 2000; ++i) {
    const std::int64_t sample = model.observeFill(makeFill(i));
    ASSERT_GE(sample, 250);
    ASSERT_LE(sample, 275);
  }
}

TEST(LatencyModel, ArtifactCarriesInterpretationMetadata) {
  LatencyModel model(LatencyModelConfig{42, 1000, 1999});
  model.observeFill(makeFill(0));
  const LatencyArtifact artifact = model.artifact();
  EXPECT_EQ(artifact.modelName, "uniform-latency");
  EXPECT_EQ(artifact.modelVersion, 1U);
  EXPECT_EQ(artifact.seed, 42U);
  EXPECT_EQ(artifact.minLatencyNs, 1000);
  EXPECT_EQ(artifact.maxLatencyNs, 1999);
  ASSERT_EQ(artifact.samplesNs.size(), 1U);
  EXPECT_EQ(artifact.samplesNs[0], 1413);
}

TEST(LatencyModel, IndependentInstancesReproduceAcrossSeeds) {
  const std::vector<std::uint64_t> seeds = {0, 1, 42, 1901, UINT64_MAX};
  for (const std::uint64_t seed : seeds) {
    LatencyModel a(LatencyModelConfig{seed, 0, 999999});
    LatencyModel b(LatencyModelConfig{seed, 0, 999999});
    for (std::uint64_t i = 0; i < 16; ++i) {
      const Fill fill = makeFill(i);
      EXPECT_EQ(a.observeFill(fill), b.observeFill(fill)) << "seed " << seed;
    }
    EXPECT_EQ(a.artifact(), b.artifact());
  }
}

TEST(LatencyModel, DifferentSeedWitness) {
  // Independently derived first samples at bounds [0, 999999]:
  // seed 0 -> 607535, seed 1901 -> 496676.
  LatencyModel zero(LatencyModelConfig{0, 0, 999999});
  LatencyModel other(LatencyModelConfig{1901, 0, 999999});
  EXPECT_EQ(zero.observeFill(makeFill(0)), 607535);
  EXPECT_EQ(other.observeFill(makeFill(0)), 496676);
}

TEST(LatencyModel, ConfigValidation) {
  EXPECT_EQ(validateLatencyModelConfig(LatencyModelConfig{0, 0, 0}), LatencyModelError::Ok);
  EXPECT_EQ(validateLatencyModelConfig(LatencyModelConfig{0, -1, 5}),
            LatencyModelError::MinNegative);
  EXPECT_EQ(validateLatencyModelConfig(LatencyModelConfig{0, 10, 5}),
            LatencyModelError::MaxLessThanMin);
  EXPECT_EQ(validateLatencyModelConfig(LatencyModelConfig{0, 0, INT64_MAX}), LatencyModelError::Ok);
}

// ---------------------------------------------------------------------
// SessionConfig integration
// ---------------------------------------------------------------------

SessionConfig latencyConfig() {
  SessionConfig config;
  config.instrument = "AAPL";
  config.prngSeed = 42;
  config.latencyMinNs = 1000;
  config.latencyMaxNs = 1999;
  return config;
}

TEST(LatencyConfig, EnabledConfigValidatesAndRoundTrips) {
  const SessionConfig config = latencyConfig();
  EXPECT_EQ(validateConfig(config), ConfigError::Ok);
  const std::string text = serializeConfig(config);
  EXPECT_EQ(text,
            "tickforge-config/1\n"
            "instrument=AAPL\n"
            "latency-max-ns=1999\n"
            "latency-min-ns=1000\n"
            "matching-rule=price-time-priority\n"
            "mode=strict\n"
            "prng-seed=42\n");
  const ConfigParseResult parsed = deserializeConfig(text);
  ASSERT_EQ(parsed.error, ConfigParseError::Ok);
  EXPECT_EQ(parsed.config, config);
  SessionConfig plain;
  plain.instrument = "AAPL";
  EXPECT_NE(hashConfig(config), hashConfig(plain));
}

TEST(LatencyConfig, ValidationErrors) {
  SessionConfig onlyMin;
  onlyMin.instrument = "AAPL";
  onlyMin.prngSeed = 1;
  onlyMin.latencyMinNs = 5;
  EXPECT_EQ(validateConfig(onlyMin), ConfigError::LatencyBoundsIncomplete);

  SessionConfig negative = latencyConfig();
  negative.latencyMinNs = -1;
  EXPECT_EQ(validateConfig(negative), ConfigError::InvalidLatencyBounds);

  SessionConfig reversed = latencyConfig();
  reversed.latencyMinNs = 2000;
  EXPECT_EQ(validateConfig(reversed), ConfigError::InvalidLatencyBounds);

  SessionConfig noSeed = latencyConfig();
  noSeed.prngSeed.reset();
  EXPECT_EQ(validateConfig(noSeed), ConfigError::LatencySeedMissing);
}

TEST(LatencyConfig, DeserializeRejectsBadLatencyValues) {
  const std::string base = "tickforge-config/1\ninstrument=AAPL\n";
  EXPECT_EQ(deserializeConfig(base + "latency-max-ns=10\nlatency-min-ns=-3\nmatching-rule="
                                     "price-time-priority\nmode=strict\nprng-seed=1\n")
                .error,
            ConfigParseError::InvalidValue);
  EXPECT_EQ(deserializeConfig(base + "latency-max-ns=10\nlatency-max-ns=11\nmatching-rule="
                                     "price-time-priority\nmode=strict\n")
                .error,
            ConfigParseError::DuplicateField);
  EXPECT_EQ(deserializeConfig(base + "latency-min-ns=abc\nmatching-rule=price-time-priority\n"
                                     "mode=strict\n")
                .error,
            ConfigParseError::InvalidValue);
}

TEST(LatencyConfig, FromConfigBuildsModelOnlyWhenEnabled) {
  SessionConfig disabled;
  disabled.instrument = "AAPL";
  EXPECT_FALSE(LatencyModel::fromConfig(disabled).has_value());

  auto model = LatencyModel::fromConfig(latencyConfig());
  ASSERT_TRUE(model.has_value());
  EXPECT_EQ(model->config().seed, 42U);
  EXPECT_EQ(model->config().minLatencyNs, 1000);
  EXPECT_EQ(model->config().maxLatencyNs, 1999);
  EXPECT_EQ(model->observeFill(makeFill(0)), 1413);
}

TEST(LatencyConfig, InvalidLatencyConfigFailsReplayBeforeAnyEvent) {
  SessionConfig bad = latencyConfig();
  bad.latencyMaxNs.reset();
  std::vector<Event> events;
  Event e;
  e.instrument = "AAPL";
  e.type = EventType::NewOrder;
  e.side = Side::Bid;
  e.orderId = OrderId{1};
  e.price = Price{100};
  e.quantity = Quantity{1};
  e.timestamp = Timestamp{1000};
  e.sequence = Sequence{0};
  events.push_back(e);

  OrderBook book;
  MatchingEngine engine(book);
  const ReplayResult result = replayEvents(events, engine, bad);
  EXPECT_EQ(result.error, ReplayError::InvalidConfig);
  EXPECT_EQ(book.orderCount(), 0U);
}

// ---------------------------------------------------------------------
// Non-interference differential (SPEC.md 15.3)
// ---------------------------------------------------------------------

TEST(LatencyModel, EnablingTheModelChangesNothingButTheArtifact) {
  GeneratorConfig gen;
  gen.seed = 1901;
  gen.eventCount = 2000;
  gen.minPrice = 80;
  gen.maxPrice = 120;
  gen.minQuantity = 1;
  gen.maxQuantity = 50;
  const std::vector<Event> events = EventGenerator(gen).generate();

  // Run A: plain engine.
  OrderBook bookA;
  MatchingEngine engineA(bookA);
  ExecutionStatistics statsA;
  std::vector<Fill> fillsA;
  for (const Event& event : events) {
    ASSERT_TRUE(engineA.onEvent(event));
    for (const Fill& fill : engineA.fills()) {
      fillsA.push_back(fill);
      statsA.addFill(fill);
    }
  }

  // Run B: same engine behind the latency observer.
  auto model = LatencyModel::fromConfig(latencyConfig());
  ASSERT_TRUE(model.has_value());
  OrderBook bookB;
  MatchingEngine engineB(bookB);
  LatencyObserver observer(engineB, *model);
  ExecutionStatistics statsB;
  std::vector<Fill> fillsB;
  for (const Event& event : events) {
    ASSERT_TRUE(observer.onEvent(event));
    for (const Fill& fill : engineB.fills()) {
      fillsB.push_back(fill);
      statsB.addFill(fill);
    }
  }

  ASSERT_EQ(fillsA.size(), fillsB.size());
  ASSERT_FALSE(fillsA.empty());
  for (std::size_t i = 0; i < fillsA.size(); ++i) {
    const Fill& a = fillsA[i];
    const Fill& b = fillsB[i];
    EXPECT_EQ(a.aggressorId, b.aggressorId);
    EXPECT_EQ(a.restingId, b.restingId);
    EXPECT_EQ(a.side, b.side);
    EXPECT_EQ(a.price, b.price);
    EXPECT_EQ(a.quantity, b.quantity);
    EXPECT_EQ(a.timestamp, b.timestamp);
    EXPECT_EQ(a.sequence, b.sequence);
  }
  EXPECT_EQ(bookA.orderCount(), bookB.orderCount());
  EXPECT_EQ(bookA.bestBid(), bookB.bestBid());
  EXPECT_EQ(bookA.bestAsk(), bookB.bestAsk());
  EXPECT_EQ(statsA.fillCount(), statsB.fillCount());
  EXPECT_EQ(statsA.totalQuantity(), statsB.totalQuantity());
  EXPECT_EQ(statsA.averagePrice()->quantitySum, statsB.averagePrice()->quantitySum);
  EXPECT_EQ(statsA.averagePrice()->priceQuantitySumHi, statsB.averagePrice()->priceQuantitySumHi);
  EXPECT_EQ(statsA.averagePrice()->priceQuantitySumLo, statsB.averagePrice()->priceQuantitySumLo);

  // The artifact: one sample per fill, all within bounds, and the
  // first samples match the independently derived seed-42 sequence.
  const LatencyArtifact artifact = observer.artifact();
  ASSERT_EQ(artifact.samplesNs.size(), fillsA.size());
  for (const std::int64_t sample : artifact.samplesNs) {
    ASSERT_GE(sample, 1000);
    ASSERT_LE(sample, 1999);
  }
  EXPECT_EQ(artifact.samplesNs[0], 1413);
  EXPECT_EQ(artifact.samplesNs[1], 1291);
}

TEST(LatencyModel, ObserverThroughConfiguredReplayProducesArtifact) {
  // The README sample session shape: one fill.
  std::vector<Event> events;
  auto add = [&events](std::int64_t ts,
                       EventType type,
                       std::uint64_t id,
                       Side side,
                       std::int64_t price,
                       std::int64_t qty,
                       std::uint64_t seq) {
    Event e;
    e.instrument = "AAPL";
    e.timestamp = Timestamp{ts};
    e.sequence = Sequence{seq};
    e.type = type;
    e.orderId = OrderId{id};
    e.side = side;
    e.price = Price{price};
    e.quantity = Quantity{qty};
    events.push_back(e);
  };
  add(1000, EventType::NewOrder, 1, Side::Ask, 105, 10, 0);
  add(2000, EventType::NewOrder, 2, Side::Bid, 105, 4, 1);

  OrderBook book;
  MatchingEngine engine(book);
  auto model = LatencyModel::fromConfig(latencyConfig());
  ASSERT_TRUE(model.has_value());
  LatencyObserver observer(engine, *model);
  const ReplayResult result = replayEvents(events, observer, latencyConfig());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(observer.artifact().samplesNs.size(), 1U);
  EXPECT_EQ(observer.artifact().samplesNs[0], 1413);
}

} // namespace
} // namespace tickforge::test
