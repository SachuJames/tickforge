// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Seeded generation contract tests (SPEC.md 14).
//
// The known-answer vectors below were derived independently of the
// C++ implementation, from the published splitmix64 algorithm
// (state += 0x9E3779B97F4A7C15, then the Stafford mix constants
// 0xBF58476D1CE4E5B9 and 0x94D049BB133111EB), computed in a separate
// implementation. They pin generation version 1: any change to the
// algorithm, the bounded mapping, or the generator's draw order must
// update SPEC.md 14 and these vectors deliberately, never silently.

#include "tests/support/deterministic_rng.hpp"
#include "tests/support/event_generator.hpp"
#include "tests/support/reference_model.hpp"
#include "tickforge/book/order_book.hpp"
#include "tickforge/event/event.hpp"
#include "tickforge/market_data/csv_parser.hpp"
#include "tickforge/matching/matching_engine.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <vector>

namespace tickforge::test {
namespace {

// ---------------------------------------------------------------------
// PRNG known answers
// ---------------------------------------------------------------------

void expectStream(std::uint64_t seed, const std::uint64_t (&expected)[5]) {
  DeterministicRng rng(seed);
  for (const std::uint64_t want : expected) {
    EXPECT_EQ(rng.next(), want) << "seed " << seed;
  }
}

TEST(DeterministicRngKnownAnswer, SeedZeroStream) {
  expectStream(0,
               {0xE220A8397B1DCDAFULL,
                0x6E789E6AA1B965F4ULL,
                0x06C45D188009454FULL,
                0xF88BB8A8724C81ECULL,
                0x1B39896A51A8749BULL});
}

TEST(DeterministicRngKnownAnswer, SeedOneStream) {
  expectStream(1,
               {0x910A2DEC89025CC1ULL,
                0xBEEB8DA1658EEC67ULL,
                0xF893A2EEFB32555EULL,
                0x71C18690EE42C90BULL,
                0x71BB54D8D101B5B9ULL});
}

TEST(DeterministicRngKnownAnswer, Seed1901Stream) {
  expectStream(1901,
               {0xB4D5251375829524ULL,
                0x929CAEA9D48DCE64ULL,
                0x43CFE2BDB6D4F19EULL,
                0xAB3E9756A385630AULL,
                0x44614E7F9454CEDFULL});
}

TEST(DeterministicRngKnownAnswer, SeedMaxStream) {
  expectStream(UINT64_MAX,
               {0xE4D971771B652C20ULL,
                0xE99FF867DBF682C9ULL,
                0x382FF84CB27281E9ULL,
                0x6D1DB36CCBA982D2ULL,
                0xB4A0472E578069AEULL});
}

TEST(DeterministicRngKnownAnswer, BoundedMappingIsModulo) {
  // SPEC.md 14.2: nextBounded(b) = next() % b, no rejection sampling.
  DeterministicRng rng(1901);
  for (const std::uint64_t want : {76ULL, 44ULL, 22ULL, 10ULL, 51ULL}) {
    EXPECT_EQ(rng.nextBounded(100), want);
  }
}

TEST(DeterministicRngKnownAnswer, RangeMappingIsOffsetModulo) {
  DeterministicRng rng(1901);
  for (const std::uint64_t want : {96ULL, 98ULL, 104ULL, 102ULL, 104ULL}) {
    EXPECT_EQ(rng.nextRange(95, 105), want);
  }
}

TEST(DeterministicRngKnownAnswer, BoolIsLowBit) {
  DeterministicRng rng(1901);
  for (const bool want : {false, false, false, false, true}) {
    EXPECT_EQ(rng.nextBool(), want);
  }
}

TEST(DeterministicRngKnownAnswer, BoundedOneIsAlwaysZero) {
  DeterministicRng rng(42);
  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(rng.nextBounded(1), 0ULL);
  }
}

TEST(DeterministicRngKnownAnswer, StreamDependsOnlyOnSeedAndDrawCount) {
  DeterministicRng advanced(1901);
  DeterministicRng fresh(1901);
  (void)advanced.next();
  (void)advanced.next();
  // The fresh stream's third output equals the advanced stream's
  // third output: state is per instance, with no shared state.
  (void)fresh.next();
  (void)fresh.next();
  EXPECT_EQ(advanced.next(), fresh.next());
  // And both equal the known-answer third value for this seed.
  DeterministicRng check(1901);
  (void)check.next();
  (void)check.next();
  EXPECT_EQ(check.next(), 0x43CFE2BDB6D4F19EULL);
}

// ---------------------------------------------------------------------
// Multi-seed generated sessions
// ---------------------------------------------------------------------

GeneratorConfig sessionConfig(std::uint64_t seed) {
  GeneratorConfig config;
  config.seed = seed;
  config.eventCount = 400;
  config.minPrice = 80;
  config.maxPrice = 120;
  config.minQuantity = 1;
  config.maxQuantity = 50;
  config.newWeight = 55;
  config.cancelWeight = 20;
  config.modifyWeight = 25;
  return config;
}

std::string eventsToCsv(const std::vector<Event>& events) {
  std::string csv = "timestamp,event_type,order_id,side,price,quantity\n";
  for (const Event& e : events) {
    csv += std::to_string(e.timestamp.count());
    csv += ',';
    csv += (e.type == EventType::NewOrder ? 'N' : (e.type == EventType::ModifyOrder ? 'M' : 'C'));
    csv += ',';
    csv += std::to_string(e.orderId.value());
    csv += ',';
    csv += (e.side == Side::Bid ? 'B' : 'A');
    csv += ',';
    if (e.price.ticks() != 0) {
      csv += std::to_string(e.price.ticks());
    }
    csv += ',';
    if (e.type != EventType::CancelOrder && e.quantity.lots() != 0) {
      csv += std::to_string(e.quantity.lots());
    }
    csv += '\n';
  }
  return csv;
}

bool sameEvent(const Event& a, const Event& b) {
  return a.timestamp == b.timestamp && a.sequence == b.sequence && a.type == b.type &&
         a.orderId == b.orderId && a.side == b.side && a.price == b.price &&
         a.quantity == b.quantity && a.instrument == b.instrument;
}

bool sameFill(const Fill& a, const Fill& b) {
  return a.aggressorId == b.aggressorId && a.restingId == b.restingId && a.side == b.side &&
         a.price == b.price && a.quantity == b.quantity && a.timestamp == b.timestamp &&
         a.sequence == b.sequence;
}

void verifySeededSession(std::uint64_t seed) {
  const GeneratorConfig config = sessionConfig(seed);
  EventGenerator generator(config);
  const std::vector<Event> events = generator.generate();
  ASSERT_EQ(events.size(), config.eventCount) << "seed " << seed;

  // Validity and ordering contract (SPEC.md 14.3).
  for (std::size_t i = 0; i < events.size(); ++i) {
    EXPECT_EQ(validateEvent(events[i]), EventValidationError::Ok) << "seed " << seed << " i " << i;
    EXPECT_EQ(events[i].sequence.value(), i);
    if (i > 0) {
      EXPECT_GT(events[i].timestamp.count(), events[i - 1].timestamp.count());
    }
  }

  // CSV round trip preserves every field.
  const std::string csv = eventsToCsv(events);
  std::istringstream input(csv);
  const CsvParser parser{"AAPL"};
  const StreamResult parsed = parser.parseStream(input);
  ASSERT_EQ(parsed.error, ParseError::Ok) << "seed " << seed;
  ASSERT_EQ(parsed.events.size(), events.size());
  for (std::size_t i = 0; i < events.size(); ++i) {
    EXPECT_TRUE(sameEvent(parsed.events[i], events[i])) << "seed " << seed << " i " << i;
  }

  // Differential: production engine vs independent ReferenceBook,
  // per event, all fill fields plus book state.
  OrderBook book;
  MatchingEngine engine(book);
  ReferenceBook model;
  for (const Event& event : parsed.events) {
    const bool applied = engine.onEvent(event);
    const ReferenceBook::ApplyResult expected = model.apply(event);
    EXPECT_TRUE(applied) << "seed " << seed;
    EXPECT_TRUE(expected.applied) << "seed " << seed;
    const auto& fills = engine.fills();
    ASSERT_EQ(fills.size(), expected.fills.size()) << "seed " << seed;
    for (std::size_t f = 0; f < fills.size(); ++f) {
      EXPECT_TRUE(sameFill(fills[f], expected.fills[f])) << "seed " << seed;
    }
    EXPECT_EQ(book.orderCount(), model.orderCount());
    EXPECT_EQ(book.bestBid(), model.bestBid());
    EXPECT_EQ(book.bestAsk(), model.bestAsk());
  }
}

TEST(SeededGeneration, BoundaryAndTypicalSeedsAreValidAndMatchReference) {
  const std::vector<std::uint64_t> seeds = {0, 1, 42, 1901, UINT64_MAX};
  for (const std::uint64_t seed : seeds) {
    verifySeededSession(seed);
  }
}

TEST(SeededGeneration, FreshInstancesReproduceIdenticalCsv) {
  const GeneratorConfig config = sessionConfig(1901);
  EventGenerator first(config);
  EventGenerator second(config);
  EXPECT_EQ(eventsToCsv(first.generate()), eventsToCsv(second.generate()));
}

TEST(SeededGeneration, InstancesDoNotShareState) {
  const GeneratorConfig config = sessionConfig(7);
  EventGenerator a(config);
  const std::vector<Event> aEvents = a.generate();
  EventGenerator b(config);
  const std::vector<Event> bEvents = b.generate();
  ASSERT_EQ(aEvents.size(), bEvents.size());
  for (std::size_t i = 0; i < aEvents.size(); ++i) {
    EXPECT_TRUE(sameEvent(aEvents[i], bEvents[i])) << "i " << i;
  }
}

TEST(SeededGeneration, DifferentSeedsProduceDifferentSessions) {
  // The splitmix64 finalizer is a bijection and the two seeds give
  // different states after the first step (pinned by the known
  // answers above), so the streams differ from the first draw; over
  // 400 events the serialized sessions differ. This pair is the
  // verified witness for that property.
  EventGenerator zero(sessionConfig(0));
  EventGenerator other(sessionConfig(1901));
  EXPECT_NE(eventsToCsv(zero.generate()), eventsToCsv(other.generate()));
}

} // namespace
} // namespace tickforge::test
