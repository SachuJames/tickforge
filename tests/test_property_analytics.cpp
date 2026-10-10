// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Property tests for QueueTracker and ExecutionStatistics, cross-checked
// against independently computed expectations over generated event
// sequences.
//
// QueueTracker: every designated resting order's queuePosition (rank,
// quantityAhead, quantityRemaining) is compared against values computed
// from the independent ReferenceBook. Lifecycle is checked as Resting
// iff the order rests in the reference book.
//
// ExecutionStatistics: expected values are computed from the captured
// fills with independent portable 128-bit arithmetic (no production
// VWAP helper). The exact VWAP rational is compared, not a float.

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
  void addProduct(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t aLo = a & 0xFFFFFFFFULL;
    const std::uint64_t aHi = a >> 32;
    const std::uint64_t bLo = b & 0xFFFFFFFFULL;
    const std::uint64_t bHi = b >> 32;
    const std::uint64_t p0 = aLo * bLo;
    const std::uint64_t p1 = aLo * bHi;
    const std::uint64_t p2 = aHi * bLo;
    const std::uint64_t p3 = aHi * bHi;
    // p0 contributes to lo; p1/p2 shifted by 32; p3 shifted by 64.
    const std::uint64_t mid = p1 + p2;
    const std::uint64_t midCarry = (mid < p1) ? 1ULL : 0ULL; // overflow of p1+p2
    add(p0);
    add((mid & 0xFFFFFFFFULL) << 32);
    hi += (mid >> 32) + (midCarry << 32) + p3;
  }

  bool operator==(const Uint128& other) const {
    return lo == other.lo && hi == other.hi;
  }
};

std::string runAnalyticsProperties(const GeneratorConfig& config) {
  OrderBook book;
  MatchingEngine engine(book);
  ReferenceBook ref;
  QueueTracker tracker;
  ExecutionStatistics stats;

  const auto events = EventGenerator(config).generate();
  // Designate every order id that will appear.
  for (const auto& e : events) {
    if (e.type == EventType::NewOrder) {
      tracker.designate(e.orderId);
    }
  }

  std::vector<Fill> allFills;
  std::ostringstream problems;

  for (std::size_t i = 0; i < events.size(); ++i) {
    const Event& event = events[i];
    engine.onEvent(event);
    const std::vector<Fill> fills = engine.fills();
    const auto refResult = ref.apply(event);

    tracker.onEvent(event);
    tracker.onFills(fills);
    for (const auto& f : fills) {
      stats.addFill(f);
      allFills.push_back(f);
    }

    // QueueTracker vs independent reference computation.
    for (const Side side : {Side::Bid, Side::Ask}) {
      for (const Price price : ref.priceLevels(side)) {
        const auto ids = ref.ordersAtLevel(side, price);
        std::int64_t ahead = 0;
        for (std::size_t rank = 0; rank < ids.size(); ++rank) {
          const auto refOrder = ref.find(ids[rank]);
          const auto pos = tracker.queuePosition(book, ids[rank]);
          if (!pos.has_value()) {
            problems << " eventIdx=" << i << " id=" << ids[rank].value()
                     << " missing queuePosition";
          } else {
            if (pos->rank != rank + 1) {
              problems << " eventIdx=" << i << " id=" << ids[rank].value()
                       << " rank prod=" << pos->rank << " ref=" << (rank + 1);
            }
            if (pos->quantityAhead.lots() != ahead) {
              problems << " eventIdx=" << i << " id=" << ids[rank].value()
                       << " qtyAhead prod=" << pos->quantityAhead.lots() << " ref=" << ahead;
            }
            if (pos->quantityRemaining != refOrder->quantity) {
              problems << " eventIdx=" << i << " id=" << ids[rank].value()
                       << " qtyRemaining differs";
            }
            if (pos->side != side || pos->price != price) {
              problems << " eventIdx=" << i << " id=" << ids[rank].value() << " side/price differ";
            }
          }
          ahead += refOrder->quantity.lots();
          // Lifecycle: resting iff in the reference book.
          const auto lc = tracker.lifecycle(book, ids[rank]);
          if (lc != QueueTracker::Lifecycle::Resting) {
            problems << " eventIdx=" << i << " id=" << ids[rank].value()
                     << " lifecycle not Resting";
          }
        }
      }
    }
    if (!problems.str().empty()) {
      break;
    }
  }

  // ExecutionStatistics vs independent computation from captured fills.
  std::int64_t totalQty = 0, buyQty = 0, sellQty = 0;
  std::optional<std::int64_t> minP, maxP;
  Uint128 pqSum;
  for (const auto& f : allFills) {
    totalQty += f.quantity.lots();
    if (f.side == Side::Bid) {
      buyQty += f.quantity.lots();
    } else {
      sellQty += f.quantity.lots();
    }
    if (!minP.has_value() || f.price.ticks() < *minP) {
      minP = f.price.ticks();
    }
    if (!maxP.has_value() || f.price.ticks() > *maxP) {
      maxP = f.price.ticks();
    }
    pqSum.addProduct(static_cast<std::uint64_t>(f.price.ticks()),
                     static_cast<std::uint64_t>(f.quantity.lots()));
  }

  if (stats.fillCount() != allFills.size()) {
    problems << " fillCount prod=" << stats.fillCount() << " ref=" << allFills.size();
  }
  if (stats.totalQuantity().lots() != totalQty) {
    problems << " totalQty prod=" << stats.totalQuantity().lots() << " ref=" << totalQty;
  }
  if (stats.buyQuantity().lots() != buyQty || stats.sellQuantity().lots() != sellQty) {
    problems << " buy/sell qty differ";
  }
  if (allFills.empty()) {
    if (stats.minPrice().has_value() || stats.maxPrice().has_value() ||
        stats.averagePrice().has_value()) {
      problems << " empty stats should have no min/max/vwap";
    }
  } else {
    if (!stats.minPrice().has_value() || stats.minPrice()->ticks() != *minP) {
      problems << " minPrice differs";
    }
    if (!stats.maxPrice().has_value() || stats.maxPrice()->ticks() != *maxP) {
      problems << " maxPrice differs";
    }
    const auto vwap = stats.averagePrice();
    if (!vwap.has_value()) {
      problems << " vwap missing";
    } else if (vwap->priceQuantitySumLo != pqSum.lo || vwap->priceQuantitySumHi != pqSum.hi) {
      problems << " vwap rational differs";
    }
  }

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
  for (const std::uint64_t seed : {1, 2, 3, 42, 7, 99}) {
    GeneratorConfig config;
    config.seed = seed;
    config.eventCount = 100;
    const std::string report = runAnalyticsProperties(config);
    EXPECT_TRUE(report.empty()) << report;
  }
}

} // namespace
} // namespace tickforge::test
