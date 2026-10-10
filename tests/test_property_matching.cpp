// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Differential property test: production (OrderBook + MatchingEngine)
// versus the independent ReferenceBook, over generated valid event
// sequences. After EVERY event the test compares applied status, all
// fill fields, and the full observable book state (order count, best
// bid/ask, price levels best-first, FIFO order at each level, and every
// resting order's side/price/quantity/arrivalSeq).
//
// On failure the report carries the seed, event index, the failing
// event, and the full printable sequence, sufficient to reproduce
// without the original environment. A failing sequence can be copied
// into a fixed regression test via EventGenerator::describe.

#include "tests/support/event_generator.hpp"
#include "tests/support/reference_model.hpp"
#include "tickforge/book/order_book.hpp"
#include "tickforge/event/event.hpp"
#include "tickforge/matching/fill.hpp"
#include "tickforge/matching/matching_engine.hpp"

#include <cstdlib>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <vector>

namespace tickforge::test {
namespace {

// Compares two fills field by field. Returns an empty string on match.
std::string compareFills(const Fill& prod, const Fill& ref, std::size_t fillIndex) {
  std::ostringstream out;
  const auto check = [&](const char* field, auto p, auto r) {
    if (p != r) {
      out << " fill[" << fillIndex << "]." << field;
    }
  };
  check("aggressorId", prod.aggressorId.value(), ref.aggressorId.value());
  check("restingId", prod.restingId.value(), ref.restingId.value());
  check("side", static_cast<int>(prod.side), static_cast<int>(ref.side));
  check("price", prod.price.ticks(), ref.price.ticks());
  check("quantity", prod.quantity.lots(), ref.quantity.lots());
  check("timestamp", prod.timestamp.count(), ref.timestamp.count());
  check("sequence", prod.sequence.value(), ref.sequence.value());
  return out.str();
}

// Compares full observable book state. Returns empty on match.
std::string compareBooks(const OrderBook& prod, const ReferenceBook& ref) {
  std::ostringstream out;
  if (prod.orderCount() != ref.orderCount()) {
    out << " orderCount prod=" << prod.orderCount() << " ref=" << ref.orderCount();
  }
  if (prod.bestBid() != ref.bestBid()) {
    out << " bestBid differs";
  }
  if (prod.bestAsk() != ref.bestAsk()) {
    out << " bestAsk differs";
  }
  for (const Side side : {Side::Bid, Side::Ask}) {
    const auto prodLevels = prod.priceLevels(side);
    const auto refLevels = ref.priceLevels(side);
    if (prodLevels.size() != refLevels.size()) {
      out << " levelCount side=" << (side == Side::Bid ? "Bid" : "Ask")
          << " prod=" << prodLevels.size() << " ref=" << refLevels.size();
      continue;
    }
    for (std::size_t i = 0; i < prodLevels.size(); ++i) {
      if (prodLevels[i] != refLevels[i]) {
        out << " level[" << i << "] price differs";
        continue;
      }
      const auto prodIds = prod.ordersAtLevel(side, prodLevels[i]);
      const auto refIds = ref.ordersAtLevel(side, refLevels[i]);
      if (prodIds.size() != refIds.size()) {
        out << " level " << prodLevels[i].ticks() << " order count differs";
        continue;
      }
      for (std::size_t j = 0; j < prodIds.size(); ++j) {
        if (prodIds[j] != refIds[j]) {
          out << " FIFO order differs at level " << prodLevels[i].ticks();
          break;
        }
        const auto p = prod.find(prodIds[j]);
        const auto r = ref.find(refIds[j]);
        if (!p.has_value() || !r.has_value()) {
          out << " find() missing for id " << prodIds[j].value();
          continue;
        }
        if (p->side != r->side || p->price != r->price || p->quantity != r->quantity ||
            p->arrivalSeq != r->arrivalSeq) {
          out << " order " << prodIds[j].value() << " fields differ";
        }
      }
    }
  }
  return out.str();
}

// Runs one generated sequence through both implementations, comparing
// after every event. Returns true on full agreement.
bool runDifferential(const GeneratorConfig& config, std::string& failureReport) {
  OrderBook book;
  MatchingEngine engine(book);
  ReferenceBook ref;
  const auto events = EventGenerator(config).generate();

  for (std::size_t i = 0; i < events.size(); ++i) {
    const Event& event = events[i];
    const bool prodApplied = engine.onEvent(event);
    const std::vector<Fill> prodFills = engine.fills();
    const auto refResult = ref.apply(event);

    std::ostringstream problems;
    if (prodApplied != refResult.applied) {
      problems << " applied prod=" << prodApplied << " ref=" << refResult.applied;
    }
    if (prodFills.size() != refResult.fills.size()) {
      problems << " fillCount prod=" << prodFills.size() << " ref=" << refResult.fills.size();
    } else {
      for (std::size_t f = 0; f < prodFills.size(); ++f) {
        problems << compareFills(prodFills[f], refResult.fills[f], f);
      }
    }
    problems << compareBooks(book, ref);

    if (!problems.str().empty()) {
      std::ostringstream report;
      report << "DIVERGENCE seed=" << config.seed << " eventIndex=" << i
             << "\n  event: " << EventGenerator::describe(event)
             << "\n  problems:" << problems.str() << "\n  sequence:";
      for (const auto& e : events) {
        report << "\n    " << EventGenerator::describe(e);
      }
      failureReport = report.str();
      return false;
    }
  }
  return true;
}

std::vector<std::uint64_t> ciSeeds() {
  return {1, 2, 3, 42, 7, 99};
}

TEST(MatchingProperty, DifferentialAgainstReferenceModel) {
  for (const auto seed : ciSeeds()) {
    GeneratorConfig config;
    config.seed = seed;
    config.eventCount = 100;
    std::string report;
    EXPECT_TRUE(runDifferential(config, report)) << report;
  }
}

// Extended local stress: TICKFORGE_EXTENDED=1 enables 20 seeds x 500
// events. Not run in ordinary CI.
TEST(MatchingProperty, ExtendedStress) {
  if (std::getenv("TICKFORGE_EXTENDED") == nullptr) {
    GTEST_SKIP() << "Set TICKFORGE_EXTENDED=1 to run the extended stress profile.";
  }
  for (std::uint64_t seed = 1000; seed < 1020; ++seed) {
    GeneratorConfig config;
    config.seed = seed;
    config.eventCount = 500;
    std::string report;
    EXPECT_TRUE(runDifferential(config, report)) << report;
  }
}

} // namespace
} // namespace tickforge::test
