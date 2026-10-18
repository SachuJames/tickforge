// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Session-scale end-to-end verification.
//
// A deterministic 6000-event session is produced by the Day 11
// EventGenerator (lifecycle-aware, valid by construction), serialized
// to CSV, parsed back with the production CsvParser, and replayed
// through OrderBook + MatchingEngine while the independent Day 11
// ReferenceBook computes the expected outcome of every event. Fills
// are compared field by field after each event, along with resting
// order count and best bid/ask. ExecutionStatistics are checked
// against totals accumulated independently from the reference fills.
//
// The session configuration is fixed, so the workload is identical
// on every run and platform.

#include "tests/support/event_generator.hpp"
#include "tests/support/reference_model.hpp"
#include "tickforge/analytics/execution_statistics.hpp"
#include "tickforge/analytics/queue_tracker.hpp"
#include "tickforge/book/order_book.hpp"
#include "tickforge/market_data/csv_parser.hpp"
#include "tickforge/matching/matching_engine.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <vector>

namespace tickforge::test {
namespace {

GeneratorConfig sessionConfig() {
  GeneratorConfig config;
  config.seed = 1901;
  config.eventCount = 6000;
  config.minPrice = 80;
  config.maxPrice = 120;
  config.minQuantity = 1;
  config.maxQuantity = 50;
  config.newWeight = 55;
  config.cancelWeight = 20;
  config.modifyWeight = 25;
  config.instrument = "AAPL";
  return config;
}

// Serializes events in the parser's CSV format (Day 12 conventions:
// zero price/quantity on modify/cancel records become empty fields).
std::string serializeSession(const std::vector<Event>& events) {
  std::ostringstream out;
  out << "timestamp,event_type,order_id,side,price,quantity\n";
  for (const Event& e : events) {
    out << e.timestamp.count() << ','
        << (e.type == EventType::NewOrder      ? 'N'
            : e.type == EventType::ModifyOrder ? 'M'
                                               : 'C')
        << ',' << e.orderId.value() << ',' << (e.side == Side::Bid ? 'B' : 'A') << ',';
    if (e.price.ticks() != 0) {
      out << e.price.ticks();
    }
    out << ',';
    if (e.quantity.lots() != 0) {
      out << e.quantity.lots();
    }
    out << '\n';
  }
  return out.str();
}

bool sameEvent(const Event& a, const Event& b) {
  return a.type == b.type && a.orderId == b.orderId && a.side == b.side && a.price == b.price &&
         a.quantity == b.quantity && a.timestamp == b.timestamp && a.sequence == b.sequence &&
         a.instrument == b.instrument;
}

std::string fillMismatch(const Fill& prod, const Fill& ref, std::size_t eventIndex) {
  std::ostringstream out;
  auto cmp = [&](const char* field, auto pv, auto rv) {
    if (pv != rv) {
      out << " event " << eventIndex << " fill field " << field << ": production=" << pv
          << " reference=" << rv << ";";
    }
  };
  cmp("aggressorId", prod.aggressorId.value(), ref.aggressorId.value());
  cmp("restingId", prod.restingId.value(), ref.restingId.value());
  cmp("price", prod.price.ticks(), ref.price.ticks());
  cmp("quantity", prod.quantity.lots(), ref.quantity.lots());
  cmp("side", static_cast<int>(prod.side), static_cast<int>(ref.side));
  cmp("timestamp", prod.timestamp.count(), ref.timestamp.count());
  cmp("sequence", prod.sequence.value(), ref.sequence.value());
  return out.str();
}

struct SessionFacts {
  std::size_t newCount = 0;
  std::size_t cancelCount = 0;
  std::size_t modifyCount = 0;
  std::size_t priorityKeepingModifies = 0;
  std::size_t cancelReplaceModifies = 0;
  std::size_t fillCount = 0;
  std::size_t partialRestingFills = 0;
  std::size_t multiFillEvents = 0;
  std::size_t maxResting = 0;
  std::vector<Fill> referenceFills;
};

// Runs the reference model over the session, gathering workload facts
// and the expected fill stream.
SessionFacts runReference(const std::vector<Event>& events) {
  SessionFacts facts;
  ReferenceBook ref;
  for (const Event& e : events) {
    switch (e.type) {
    case EventType::NewOrder:
      facts.newCount++;
      break;
    case EventType::CancelOrder:
      facts.cancelCount++;
      break;
    case EventType::ModifyOrder: {
      facts.modifyCount++;
      const auto current = ref.find(e.orderId);
      if (current.has_value()) {
        const bool priceChanged = e.price.ticks() != 0 && e.price != current->price;
        const bool qtyIncrease = e.quantity.lots() != 0 && e.quantity > current->quantity;
        if (priceChanged || qtyIncrease) {
          facts.cancelReplaceModifies++;
        } else {
          facts.priorityKeepingModifies++;
        }
      }
      break;
    }
    }
    auto result = ref.apply(e);
    if (result.fills.size() > 1) {
      facts.multiFillEvents++;
    }
    for (const Fill& fill : result.fills) {
      facts.fillCount++;
      // A fill is partial for the resting order if that order is
      // still resting afterwards with quantity left.
      if (ref.find(fill.restingId).has_value()) {
        facts.partialRestingFills++;
      }
      facts.referenceFills.push_back(fill);
    }
    facts.maxResting = std::max(facts.maxResting, ref.orderCount());
  }
  return facts;
}

TEST(SessionScale, GenerationIsByteReproducible) {
  const auto eventsA = EventGenerator(sessionConfig()).generate();
  const auto eventsB = EventGenerator(sessionConfig()).generate();
  ASSERT_EQ(eventsA.size(), 6000U);
  ASSERT_EQ(eventsA.size(), eventsB.size());
  for (std::size_t i = 0; i < eventsA.size(); ++i) {
    ASSERT_TRUE(sameEvent(eventsA[i], eventsB[i])) << "event " << i;
  }
  EXPECT_EQ(serializeSession(eventsA), serializeSession(eventsB));

  auto otherConfig = sessionConfig();
  otherConfig.seed = 1902;
  const auto eventsC = EventGenerator(otherConfig).generate();
  EXPECT_NE(serializeSession(eventsA), serializeSession(eventsC));
}

TEST(SessionScale, SessionExercisesAllLifecycles) {
  const auto events = EventGenerator(sessionConfig()).generate();
  const SessionFacts facts = runReference(events);
  // Floors are set well below observed values; they exist to catch a
  // generator change that silently stops exercising a lifecycle.
  EXPECT_GT(facts.newCount, 2500U);
  EXPECT_GT(facts.cancelCount, 500U);
  EXPECT_GT(facts.modifyCount, 800U);
  EXPECT_GT(facts.priorityKeepingModifies, 100U);
  EXPECT_GT(facts.cancelReplaceModifies, 300U);
  EXPECT_GT(facts.fillCount, 800U);
  EXPECT_GT(facts.partialRestingFills, 50U);
  EXPECT_GT(facts.multiFillEvents, 25U);
  EXPECT_GT(facts.maxResting, 15U);
}

TEST(SessionScale, CsvRoundTripMatchesReferenceEventByEvent) {
  const auto events = EventGenerator(sessionConfig()).generate();
  const std::string csv = serializeSession(events);

  std::istringstream input(csv);
  const CsvParser parser{"AAPL"};
  const StreamResult parsed = parser.parseStream(input);
  ASSERT_EQ(parsed.error, ParseError::Ok);
  ASSERT_EQ(parsed.events.size(), events.size());
  for (std::size_t i = 0; i < events.size(); ++i) {
    ASSERT_TRUE(sameEvent(parsed.events[i], events[i])) << "round-trip event " << i;
  }

  OrderBook book;
  MatchingEngine engine(book);
  ReferenceBook ref;
  ExecutionStatistics stats;
  std::uint64_t expectedNumerator = 0;
  std::int64_t expectedTotalLots = 0;
  std::int64_t expectedBuyLots = 0;
  std::int64_t expectedSellLots = 0;
  std::size_t totalFills = 0;

  for (std::size_t i = 0; i < parsed.events.size(); ++i) {
    const Event& e = parsed.events[i];
    ASSERT_TRUE(engine.onEvent(e)) << "event " << i << " rejected by engine";
    auto refResult = ref.apply(e);
    ASSERT_TRUE(refResult.applied) << "event " << i << " rejected by reference";

    // MatchingEngine::fills() exposes this event's fills only.
    const auto& prodFills = engine.fills();
    ASSERT_EQ(prodFills.size(), refResult.fills.size()) << "fill count diverges at event " << i;
    for (std::size_t f = 0; f < refResult.fills.size(); ++f) {
      const Fill& prod = prodFills[f];
      const Fill& want = refResult.fills[f];
      const std::string mismatch = fillMismatch(prod, want, i);
      ASSERT_TRUE(mismatch.empty()) << mismatch;
      stats.addFill(prod);
      expectedNumerator += static_cast<std::uint64_t>(prod.price.ticks()) *
                           static_cast<std::uint64_t>(prod.quantity.lots());
      expectedTotalLots += prod.quantity.lots();
      if (prod.side == Side::Bid) {
        expectedBuyLots += prod.quantity.lots();
      } else {
        expectedSellLots += prod.quantity.lots();
      }
    }
    totalFills += prodFills.size();

    ASSERT_EQ(book.orderCount(), ref.orderCount()) << "event " << i;
    ASSERT_EQ(book.bestBid(), ref.bestBid()) << "event " << i;
    ASSERT_EQ(book.bestAsk(), ref.bestAsk()) << "event " << i;
  }

  // Statistics against totals accumulated from the same fills by
  // independent int64 arithmetic (exact at session scale).
  EXPECT_EQ(stats.fillCount(), totalFills);
  EXPECT_EQ(stats.totalQuantity().lots(), expectedTotalLots);
  EXPECT_EQ(stats.buyQuantity().lots(), expectedBuyLots);
  EXPECT_EQ(stats.sellQuantity().lots(), expectedSellLots);
  const auto avg = stats.averagePrice();
  ASSERT_TRUE(avg.has_value());
  EXPECT_EQ(avg->quantitySum, expectedTotalLots);
  // The session numerator fits in 64 bits (bounded prices, quantities,
  // and fill count), so the high half of the exact pair must be zero.
  EXPECT_EQ(avg->priceQuantitySumHi, 0U);
  EXPECT_EQ(avg->priceQuantitySumLo, expectedNumerator);
  ASSERT_TRUE(stats.minPrice().has_value());
  ASSERT_TRUE(stats.maxPrice().has_value());
  EXPECT_LE(stats.minPrice()->ticks(), stats.maxPrice()->ticks());
}

TEST(SessionScale, SessionReplayIsDeterministic) {
  const auto events = EventGenerator(sessionConfig()).generate();
  std::vector<Fill> firstRun;
  std::vector<Fill> secondRun;
  for (std::vector<Fill>* sink : {&firstRun, &secondRun}) {
    OrderBook book;
    MatchingEngine engine(book);
    for (const Event& e : events) {
      ASSERT_TRUE(engine.onEvent(e));
      // fills() is per-event; accumulate.
      const auto& produced = engine.fills();
      sink->insert(sink->end(), produced.begin(), produced.end());
    }
  }
  ASSERT_EQ(firstRun.size(), secondRun.size());
  for (std::size_t i = 0; i < firstRun.size(); ++i) {
    EXPECT_TRUE(fillMismatch(firstRun[i], secondRun[i], i).empty()) << "fill " << i;
  }
}

// ---------------------------------------------------------------------
// Targeted scenarios (small, hand-verifiable)
// ---------------------------------------------------------------------

Event handEvent(EventType type,
                std::uint64_t id,
                Side side,
                std::int64_t price,
                std::int64_t qty,
                std::uint64_t seq) {
  Event e;
  e.type = type;
  e.orderId = OrderId{id};
  e.side = side;
  e.price = Price{price};
  e.quantity = Quantity{qty};
  e.timestamp = Timestamp{1000000 + static_cast<std::int64_t>(seq) * 1000};
  e.sequence = Sequence{seq};
  e.instrument = "AAPL";
  return e;
}

TEST(SessionTargeted, QueueTrackerFollowsLevelThroughFillsModifiesAndCancel) {
  OrderBook book;
  MatchingEngine engine(book);
  QueueTracker tracker;
  tracker.designate(OrderId{4});
  auto feed = [&](const Event& e) {
    ASSERT_TRUE(engine.onEvent(e));
    tracker.onEvent(e);
    tracker.onFills(engine.fills()); // this event's fills
  };

  feed(handEvent(EventType::NewOrder, 1, Side::Bid, 100, 10, 0));
  feed(handEvent(EventType::NewOrder, 2, Side::Bid, 100, 20, 1));
  feed(handEvent(EventType::NewOrder, 3, Side::Bid, 100, 30, 2));
  feed(handEvent(EventType::NewOrder, 4, Side::Bid, 100, 5, 3));
  auto pos = tracker.queuePosition(book, OrderId{4});
  ASSERT_TRUE(pos.has_value());
  EXPECT_EQ(pos->rank, 4U);
  EXPECT_EQ(pos->quantityAhead.lots(), 60);
  EXPECT_EQ(pos->quantityRemaining.lots(), 5);

  // Sell 25: consumes order 1 (10) and 15 of order 2.
  feed(handEvent(EventType::NewOrder, 5, Side::Ask, 100, 25, 4));
  pos = tracker.queuePosition(book, OrderId{4});
  ASSERT_TRUE(pos.has_value());
  EXPECT_EQ(pos->rank, 3U);
  EXPECT_EQ(pos->quantityAhead.lots(), 35);

  // Quantity reduction on order 3 (30 -> 12) keeps its priority.
  feed(handEvent(EventType::ModifyOrder, 3, Side::Bid, 0, 12, 5));
  pos = tracker.queuePosition(book, OrderId{4});
  ASSERT_TRUE(pos.has_value());
  EXPECT_EQ(pos->rank, 3U);
  EXPECT_EQ(pos->quantityAhead.lots(), 17);

  // Cancelling order 2 leaves only order 3 ahead.
  feed(handEvent(EventType::CancelOrder, 2, Side::Bid, 0, 0, 6));
  pos = tracker.queuePosition(book, OrderId{4});
  ASSERT_TRUE(pos.has_value());
  EXPECT_EQ(pos->rank, 2U);
  EXPECT_EQ(pos->quantityAhead.lots(), 12);
  EXPECT_EQ(tracker.lifecycle(book, OrderId{4}), QueueTracker::Lifecycle::Resting);
}

TEST(SessionTargeted, AddThenCancelLifecycleLeavesBookEmpty) {
  OrderBook book;
  MatchingEngine engine(book);
  EXPECT_TRUE(engine.onEvent(handEvent(EventType::NewOrder, 1, Side::Bid, 100, 10, 0)));
  EXPECT_TRUE(engine.onEvent(handEvent(EventType::NewOrder, 2, Side::Ask, 105, 10, 1)));
  EXPECT_TRUE(engine.fills().empty()); // non-crossing: no fills
  EXPECT_TRUE(engine.onEvent(handEvent(EventType::CancelOrder, 1, Side::Bid, 0, 0, 2)));
  EXPECT_EQ(book.orderCount(), 1U);
  EXPECT_TRUE(engine.onEvent(handEvent(EventType::CancelOrder, 2, Side::Ask, 0, 0, 3)));
  EXPECT_TRUE(book.orderCount() == 0U);
  EXPECT_FALSE(book.bestBid().has_value());
  EXPECT_FALSE(book.bestAsk().has_value());
}

} // namespace
} // namespace tickforge::test
