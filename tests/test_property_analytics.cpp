// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Property tests for QueueTracker and ExecutionStatistics, cross-checked
// against independently computed expectations over generated event
// sequences.

#include "tests/support/event_generator.hpp"
#include "tests/support/reference_model.hpp"
#include "tickforge/analytics/execution_statistics.hpp"
#include "tickforge/analytics/queue_tracker.hpp"
#include "tickforge/book/order_book.hpp"
#include "tickforge/event/event.hpp"
#include "tickforge/matching/fill.hpp"
#include "tickforge/matching/matching_engine.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <vector>

namespace tickforge::test {
namespace {

// Portable 128-bit accumulator for the independent VWAP oracle.
// (Test-only; production uses its own portable representation.)
struct Uint128 {
  std::uint64_t lo = 0;
  std::uint64_t hi = 0;

  void add(std::uint64_t v) {
    const std::uint64_t old = lo;
    lo += v;
    if (lo < old) {
      ++hi;
    }
  }

  // Adds a * b (both 64-bit) without overflow.
  void addProduct(std::uint64_t a, // NOLINT(bugprone-easily-swappable-parameters)
                  std::uint64_t b) {
    const std::uint64_t aLo = a & 0xFFFFFFFFULL;
    const std::uint64_t aHi = a >> 32;
    const std::uint64_t bLo = b & 0xFFFFFFFFULL;
    const std::uint64_t bHi = b >> 32;
    const std::uint64_t p0 = aLo * bLo;
    const std::uint64_t p1 = aLo * bHi;
    const std::uint64_t p2 = aHi * bLo;
    const std::uint64_t p3 = aHi * bHi;
    const std::uint64_t mid = p1 + p2;
    const std::uint64_t midCarry = (mid < p1) ? 1ULL : 0ULL;
    add(p0);
    add((mid & 0xFFFFFFFFULL) << 32);
    hi += (mid >> 32) + (midCarry << 32) + p3;
  }

  bool operator==(const Uint128& other) const {
    return lo == other.lo && hi == other.hi;
  }
};

void checkOnePosition(const QueueTracker& tracker,
                      const OrderBook& book,
                      const ReferenceBook& ref,
                      OrderId id,
                      std::size_t rank, // NOLINT(bugprone-easily-swappable-parameters)
                      std::int64_t quantityAhead,
                      std::size_t eventIdx,
                      std::ostringstream& problems) {
  const auto refOrder = ref.find(id);
  const auto pos = tracker.queuePosition(book, id);
  if (!pos.has_value() || !refOrder.has_value()) {
    problems << " eventIdx=" << eventIdx << " id=" << id.value() << " missing state";
    return;
  }
  if (pos->rank != rank + 1) {
    problems << " eventIdx=" << eventIdx << " id=" << id.value() << " rank prod=" << pos->rank
             << " ref=" << (rank + 1);
  }
  if (pos->quantityAhead.lots() != quantityAhead) {
    problems << " eventIdx=" << eventIdx << " id=" << id.value()
             << " qtyAhead prod=" << pos->quantityAhead.lots() << " ref=" << quantityAhead;
  }
  if (pos->quantityRemaining != refOrder->quantity) {
    problems << " eventIdx=" << eventIdx << " id=" << id.value() << " qtyRemaining differs";
  }
  if (tracker.lifecycle(book, id) != QueueTracker::Lifecycle::Resting) {
    problems << " eventIdx=" << eventIdx << " id=" << id.value() << " lifecycle not Resting";
  }
}

void checkTrackerState(const QueueTracker& tracker,
                       const OrderBook& book,
                       const ReferenceBook& ref,
                       std::size_t eventIdx,
                       std::ostringstream& problems) {
  for (const Side side : {Side::Bid, Side::Ask}) {
    for (const Price price : ref.priceLevels(side)) {
      const auto ids = ref.ordersAtLevel(side, price);
      std::int64_t ahead = 0;
      for (std::size_t rank = 0; rank < ids.size(); ++rank) {
        checkOnePosition(tracker, book, ref, ids[rank], rank, ahead, eventIdx, problems);
        const auto refOrder = ref.find(ids[rank]);
        if (refOrder.has_value()) {
          ahead += refOrder->quantity.lots();
        }
      }
    }
  }
}

struct ExpectedStats {
  std::size_t fillCount = 0;
  std::int64_t totalQty = 0;
  std::int64_t buyQty = 0;
  std::int64_t sellQty = 0;
  std::optional<std::int64_t> minPrice;
  std::optional<std::int64_t> maxPrice;
  Uint128 priceQtySum;
};

ExpectedStats computeExpected(const std::vector<Fill>& fills) {
  ExpectedStats expected;
  expected.fillCount = fills.size();
  for (const auto& f : fills) {
    expected.totalQty += f.quantity.lots();
    if (f.side == Side::Bid) {
      expected.buyQty += f.quantity.lots();
    } else {
      expected.sellQty += f.quantity.lots();
    }
    if (!expected.minPrice.has_value() || f.price.ticks() < *expected.minPrice) {
      expected.minPrice = f.price.ticks();
    }
    if (!expected.maxPrice.has_value() || f.price.ticks() > *expected.maxPrice) {
      expected.maxPrice = f.price.ticks();
    }
    expected.priceQtySum.addProduct(static_cast<std::uint64_t>(f.price.ticks()),
                                    static_cast<std::uint64_t>(f.quantity.lots()));
  }
  return expected;
}

void checkStatistics(const ExecutionStatistics& stats,
                     const ExpectedStats& expected,
                     std::ostringstream& problems) {
  if (stats.fillCount() != expected.fillCount) {
    problems << " fillCount prod=" << stats.fillCount() << " ref=" << expected.fillCount;
  }
  if (stats.totalQuantity().lots() != expected.totalQty) {
    problems << " totalQty prod=" << stats.totalQuantity().lots() << " ref=" << expected.totalQty;
  }
  if (stats.buyQuantity().lots() != expected.buyQty ||
      stats.sellQuantity().lots() != expected.sellQty) {
    problems << " buy/sell qty differ";
  }
  if (expected.fillCount == 0) {
    if (stats.minPrice().has_value() || stats.maxPrice().has_value() ||
        stats.averagePrice().has_value()) {
      problems << " empty stats should have no min/max/vwap";
    }
    return;
  }
  const auto statsMin = stats.minPrice();
  const auto statsMax = stats.maxPrice();
  const bool minOk = statsMin.has_value() && expected.minPrice.has_value() &&
                     statsMin->ticks() == expected.minPrice.value();
  if (!minOk) {
    problems << " minPrice differs";
  }
  const bool maxOk = statsMax.has_value() && expected.maxPrice.has_value() &&
                     statsMax->ticks() == expected.maxPrice.value();
  if (!maxOk) {
    problems << " maxPrice differs";
  }
  const auto vwap = stats.averagePrice();
  if (!vwap.has_value()) {
    problems << " vwap missing";
  } else if (vwap->priceQuantitySumLo != expected.priceQtySum.lo ||
             vwap->priceQuantitySumHi != expected.priceQtySum.hi) {
    problems << " vwap rational differs";
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): test driver
// with many assertions; splitting would hurt readability.
std::string runAnalyticsProperties(const GeneratorConfig& config) {
  OrderBook book;
  MatchingEngine engine(book);
  ReferenceBook ref;
  QueueTracker tracker;
  ExecutionStatistics stats;

  const auto events = EventGenerator(config).generate();
  for (const auto& e : events) {
    if (e.type == EventType::NewOrder) {
      tracker.designate(e.orderId);
    }
  }

  std::vector<Fill> all_fills;
  std::ostringstream problems;

  for (std::size_t i = 0; i < events.size(); ++i) {
    const Event& event = events[i];
    engine.onEvent(event);
    const std::vector<Fill> fills = engine.fills();
    ref.apply(event);

    tracker.onEvent(event);
    tracker.onFills(fills);
    for (const auto& f : fills) {
      stats.addFill(f);
      all_fills.push_back(f);
    }

    checkTrackerState(tracker, book, ref, i, problems);
    if (!problems.str().empty()) {
      break;
    }
  }

  checkStatistics(stats, computeExpected(all_fills), problems);

  if (!problems.str().empty()) {
    std::ostringstream report;
    report << "ANALYTICS DIVERGENCE seed=" << config.seed << problems.str() << "\n  sequence:";
    for (const auto& e : events) {
      report << "\n    " << EventGenerator::describe(e);
    }
    return report.str();
  }
  return "";
}

TEST(AnalyticsProperty, TrackerAndStatistics) {
  for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL, 42ULL, 7ULL, 99ULL}) {
    GeneratorConfig config;
    config.seed = seed;
    config.eventCount = 100;
    const std::string report = runAnalyticsProperties(config);
    EXPECT_TRUE(report.empty()) << report;
  }
}

} // namespace
} // namespace tickforge::test
