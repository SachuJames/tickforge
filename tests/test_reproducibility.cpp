// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// End-to-end reproducibility audit (SPEC.md section 4).
//
// Pipeline under audit:
//   CSV -> CsvParser -> SessionConfig -> replayEvents ->
//   MatchingEngine -> ExecutionStatistics (+ QueueTracker)
//
// The test runs the complete pipeline twice from the same inputs and
// asserts the SPEC.md 4.2 determinism contract: byte-identical fills,
// identical book state, identical statistics, and identical result
// metadata (version + configuration hash). It also asserts metadata
// consistency: the embedded hash equals the hash of the configuration
// actually used.
//
// This proves the correctness of the existing pipeline rather than
// adding functionality. Input-stream identity (a hash of the event
// stream itself) is deliberately not asserted: SPEC.md 9.3 defines the
// normalized stream as the reproducibility boundary but requires no
// stream identifier; SPEC.md 9.6 requires only version + config hash.

#include "tickforge/analytics/execution_statistics.hpp"
#include "tickforge/analytics/queue_tracker.hpp"
#include "tickforge/book/order_book.hpp"
#include "tickforge/event/event.hpp"
#include "tickforge/market_data/csv_parser.hpp"
#include "tickforge/matching/fill.hpp"
#include "tickforge/matching/matching_engine.hpp"
#include "tickforge/replay/event_processor.hpp"
#include "tickforge/replay/replay.hpp"
#include "tickforge/replay/session_config.hpp"
#include "tickforge/version.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tickforge::CsvParser;
using tickforge::Event;
using tickforge::EventProcessor;
using tickforge::ExecutionStatistics;
using tickforge::Fill;
using tickforge::MatchingEngine;
using tickforge::OrderBook;
using tickforge::OrderId;
using tickforge::ParseError;
using tickforge::Price;
using tickforge::QueueTracker;
using tickforge::ReplayResult;
using tickforge::SessionConfig;
using tickforge::Side;

// A session that crosses the spread, partially fills, cancels, and
// modifies: exercises matching, book mutation, and statistics.
const char* session_csv = "timestamp,event_type,order_id,side,price,quantity\n"
                          "1000000,N,1,B,100,10\n"
                          "1000010,N,2,B,100,5\n"
                          "1000020,N,3,A,101,8\n"
                          "1000030,N,4,A,99,12\n" // crosses: lifts bids 1 and 2
                          "1000040,C,3,A,,\n"     // cancel resting ask
                          "1000050,N,5,B,98,7\n"
                          "1000060,M,5,B,99,\n"    // price improvement, no cross
                          "1000070,N,6,A,99,20\n"; // crosses: lifts 1 (rest), 5

// Adapts MatchingEngine to a full-stream replay: drains each event's
// fills into a single ordered vector and feeds statistics.
class AuditedEngine : public EventProcessor {
public:
  AuditedEngine(MatchingEngine& engine, ExecutionStatistics& stats)
      : engine_(engine), stats_(stats) {}

  bool onEvent(const Event& event) override {
    const bool applied = engine_.onEvent(event);
    if (applied) {
      const auto& fills = engine_.fills();
      allFills_.insert(allFills_.end(), fills.begin(), fills.end());
      stats_.addFills(fills);
    }
    return applied;
  }

  std::vector<Fill> allFills_;

private:
  MatchingEngine& engine_;
  ExecutionStatistics& stats_;
};

// Everything the SPEC.md 4.2 determinism contract covers, captured in
// comparable form.
struct PipelineOutcome {
  std::vector<Fill> fills;
  std::size_t bookOrderCount = 0;
  std::optional<Price> bestBid;
  std::optional<Price> bestAsk;
  std::vector<Price> bidLevels;
  std::vector<Price> askLevels;
  // (orderId, remainingLots) per level, in book queue order.
  std::vector<std::vector<std::pair<OrderId, std::int64_t>>> bidQueues;
  std::vector<std::vector<std::pair<OrderId, std::int64_t>>> askQueues;
  std::size_t statFillCount = 0;
  std::int64_t statTotalLots = 0;
  std::int64_t statBuyLots = 0;
  std::int64_t statSellLots = 0;
  std::optional<Price> statMinPrice;
  std::optional<Price> statMaxPrice;
  std::optional<ExecutionStatistics::WeightedAveragePrice> statVwap;
  QueueTracker::Lifecycle trackerLifecycle1 = QueueTracker::Lifecycle::Unknown;
  QueueTracker::Lifecycle trackerLifecycle5 = QueueTracker::Lifecycle::Unknown;
  ReplayResult result;
};

void captureLevelQueues(const OrderBook& book,
                        Side side,
                        std::vector<Price>& levels,
                        std::vector<std::vector<std::pair<OrderId, std::int64_t>>>& queues) {
  levels = book.priceLevels(side);
  for (const Price price : levels) {
    std::vector<std::pair<OrderId, std::int64_t>> queue;
    for (const OrderId id : book.ordersAtLevel(side, price)) {
      const auto order = book.find(id);
      if (order.has_value()) {
        queue.emplace_back(id, order->quantity.lots());
      }
    }
    queues.push_back(std::move(queue));
  }
}

PipelineOutcome runPipeline(const SessionConfig& config) {
  PipelineOutcome outcome;

  const CsvParser parser("AAPL");
  std::istringstream input(session_csv);
  const auto parsed = parser.parseStream(input);
  EXPECT_EQ(parsed.error, ParseError::Ok);
  if (parsed.error != ParseError::Ok) {
    return outcome;
  }

  OrderBook book;
  MatchingEngine engine(book);
  ExecutionStatistics stats;
  AuditedEngine audited(engine, stats);
  QueueTracker tracker;
  tracker.designate(OrderId{1});
  tracker.designate(OrderId{5});

  // Feed the tracker in stream order alongside the replay.
  class TrackingAdapter : public EventProcessor {
  public:
    TrackingAdapter(AuditedEngine& inner, QueueTracker& tracker, MatchingEngine& engine)
        : inner_(inner), tracker_(tracker), engine_(engine) {}
    bool onEvent(const Event& event) override {
      tracker_.onEvent(event);
      const bool applied = inner_.onEvent(event);
      if (applied) {
        tracker_.onFills(engine_.fills());
      }
      return applied;
    }

  private:
    AuditedEngine& inner_;
    QueueTracker& tracker_;
    MatchingEngine& engine_;
  };
  TrackingAdapter adapter(audited, tracker, engine);

  outcome.result = tickforge::replayEvents(parsed.events, adapter, config);
  EXPECT_TRUE(outcome.result.ok());
  if (!outcome.result.ok()) {
    return outcome;
  }

  outcome.fills = audited.allFills_;
  outcome.bookOrderCount = book.orderCount();
  outcome.bestBid = book.bestBid();
  outcome.bestAsk = book.bestAsk();
  captureLevelQueues(book, Side::Bid, outcome.bidLevels, outcome.bidQueues);
  captureLevelQueues(book, Side::Ask, outcome.askLevels, outcome.askQueues);

  outcome.statFillCount = stats.fillCount();
  outcome.statTotalLots = stats.totalQuantity().lots();
  outcome.statBuyLots = stats.buyQuantity().lots();
  outcome.statSellLots = stats.sellQuantity().lots();
  outcome.statMinPrice = stats.minPrice();
  outcome.statMaxPrice = stats.maxPrice();
  outcome.statVwap = stats.averagePrice();

  outcome.trackerLifecycle1 = tracker.lifecycle(book, OrderId{1});
  outcome.trackerLifecycle5 = tracker.lifecycle(book, OrderId{5});
  return outcome;
}

[[nodiscard]] bool fillsEqual(const Fill& a, const Fill& b) {
  return a.aggressorId == b.aggressorId && a.restingId == b.restingId && a.side == b.side &&
         a.price == b.price && a.quantity == b.quantity && a.timestamp == b.timestamp &&
         a.sequence == b.sequence;
}

[[nodiscard]] bool fillVectorsEqual(const std::vector<Fill>& a, const std::vector<Fill>& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!fillsEqual(a[i], b[i])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool bookEqual(const PipelineOutcome& a, const PipelineOutcome& b) {
  return a.bookOrderCount == b.bookOrderCount && a.bestBid == b.bestBid && a.bestAsk == b.bestAsk &&
         a.bidLevels == b.bidLevels && a.askLevels == b.askLevels && a.bidQueues == b.bidQueues &&
         a.askQueues == b.askQueues;
}

[[nodiscard]] bool vwapEqual(const std::optional<ExecutionStatistics::WeightedAveragePrice>& a,
                             const std::optional<ExecutionStatistics::WeightedAveragePrice>& b) {
  if (a.has_value() != b.has_value()) {
    return false;
  }
  if (!a.has_value()) {
    return true;
  }
  return a->priceQuantitySumHi == b->priceQuantitySumHi &&
         a->priceQuantitySumLo == b->priceQuantitySumLo && a->quantitySum == b->quantitySum;
}

[[nodiscard]] bool statsEqual(const PipelineOutcome& a, const PipelineOutcome& b) {
  return a.statFillCount == b.statFillCount && a.statTotalLots == b.statTotalLots &&
         a.statBuyLots == b.statBuyLots && a.statSellLots == b.statSellLots &&
         a.statMinPrice == b.statMinPrice && a.statMaxPrice == b.statMaxPrice &&
         vwapEqual(a.statVwap, b.statVwap);
}

[[nodiscard]] bool metadataEqual(const PipelineOutcome& a, const PipelineOutcome& b) {
  return a.result.ok() && b.result.ok() && a.result.error == b.result.error &&
         a.result.skippedCount == b.result.skippedCount && a.result.mode == b.result.mode &&
         a.result.version == b.result.version && a.result.configHash == b.result.configHash;
}

void expectFillsEqual(const std::vector<Fill>& a, const std::vector<Fill>& b) {
  EXPECT_TRUE(fillVectorsEqual(a, b));
}

void expectOutcomesEqual(const PipelineOutcome& a, const PipelineOutcome& b) {
  // SPEC.md 4.2: fills byte-identical, book state identical, statistics
  // identical, metadata identical.
  EXPECT_TRUE(fillVectorsEqual(a.fills, b.fills));
  EXPECT_TRUE(bookEqual(a, b));
  EXPECT_TRUE(statsEqual(a, b));
  EXPECT_TRUE(a.trackerLifecycle1 == b.trackerLifecycle1);
  EXPECT_TRUE(a.trackerLifecycle5 == b.trackerLifecycle5);
  EXPECT_TRUE(metadataEqual(a, b));
}

SessionConfig makeAuditConfig() {
  SessionConfig config;
  config.instrument = "AAPL";
  config.tickSize = "0.01";
  config.lotSize = "1";
  return config;
}

TEST(ReproducibilityAuditTest, FullPipelineDeterministic) {
  const SessionConfig config = makeAuditConfig();
  const PipelineOutcome first = runPipeline(config);
  const PipelineOutcome second = runPipeline(config);

  ASSERT_TRUE(first.result.ok());
  ASSERT_TRUE(second.result.ok());
  expectOutcomesEqual(first, second);

  // The session actually did work: guards against a vacuous pass.
  EXPECT_GT(first.fills.size(), 0U);
  EXPECT_GT(first.statFillCount, 0U);
}

TEST(ReproducibilityAuditTest, MetadataConsistentWithConfig) {
  const SessionConfig config = makeAuditConfig();
  const PipelineOutcome outcome = runPipeline(config);

  ASSERT_TRUE(outcome.result.ok());
  // SPEC.md 9.6: version and config hash identify the run.
  EXPECT_EQ(outcome.result.version, tickforge::kVersion);
  EXPECT_EQ(outcome.result.configHash, std::optional<std::uint64_t>(tickforge::hashConfig(config)));
  // The hash is over the canonical serialization, not memory.
  EXPECT_EQ(outcome.result.configHash,
            std::optional<std::uint64_t>(tickforge::hashConfig(
                tickforge::deserializeConfig(tickforge::serializeConfig(config)).config)));
}

TEST(ReproducibilityAuditTest, DifferentConfigDifferentHash) {
  const SessionConfig config = makeAuditConfig();
  const PipelineOutcome first = runPipeline(config);

  SessionConfig lenient = makeAuditConfig();
  lenient.mode = tickforge::ReplayMode::Lenient;
  const PipelineOutcome second = runPipeline(lenient);

  ASSERT_TRUE(first.result.ok());
  ASSERT_TRUE(second.result.ok());
  // Same events, different configuration: identical execution, but the
  // metadata records the different configuration.
  expectFillsEqual(first.fills, second.fills);
  EXPECT_EQ(first.bookOrderCount, second.bookOrderCount);
  EXPECT_NE(first.result.configHash, second.result.configHash);
  EXPECT_EQ(second.result.mode, tickforge::ReplayMode::Lenient);
}

} // namespace
