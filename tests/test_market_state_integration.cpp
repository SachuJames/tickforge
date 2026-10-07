// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Market-state integration tests: CSV -> parser -> matching engine ->
// OrderBook -> MarketStateView. Proves the derived state pipeline is
// deterministic and matches the direct event pipeline.

#include "tickforge/book/order_book.hpp"
#include "tickforge/event/event.hpp"
#include "tickforge/market_data/csv_parser.hpp"
#include "tickforge/market_data/market_state.hpp"
#include "tickforge/matching/matching_engine.hpp"

#include <gtest/gtest.h>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using tickforge::CsvParser;
using tickforge::Event;
using tickforge::LevelAggregate;
using tickforge::MarketStateView;
using tickforge::MatchingEngine;
using tickforge::OrderBook;
using tickforge::ParseError;
using tickforge::Price;
using tickforge::Quantity;
using tickforge::Side;
using tickforge::StreamResult;

// Golden fixture (mirrors testdata/market_data/depth_sample.csv).
// No crossing: pure state construction, then a cancel removing a level.
const char* depth_csv = "timestamp,event_type,order_id,side,price,quantity\n"
                        "1000000,N,1,B,100,10\n"
                        "1000010,N,2,B,100,5\n"
                        "1000020,N,3,B,99,8\n"
                        "1000030,N,4,A,101,7\n"
                        "1000040,N,5,A,102,3\n"
                        "1000050,C,3,B,,\n";

std::vector<Event> parseDepthFixture() {
  const CsvParser parser{"AAPL"};
  std::istringstream input(depth_csv);
  const StreamResult result = parser.parseStream(input);
  EXPECT_EQ(result.error, ParseError::Ok);
  return result.events;
}

void applyAll(MatchingEngine& engine, const std::vector<Event>& events) {
  for (const Event& event : events) {
    EXPECT_TRUE(engine.onEvent(event));
  }
}

void expectFinalState(const MarketStateView& view) {
  EXPECT_EQ(view.bestBid(), std::optional<Price>(Price{100}));
  EXPECT_EQ(view.bestAsk(), std::optional<Price>(Price{101}));

  const std::vector<LevelAggregate> expectedBids{
      LevelAggregate{Price{100}, Quantity{15}, 2},
  };
  EXPECT_EQ(view.levels(Side::Bid), expectedBids);

  const std::vector<LevelAggregate> expectedAsks{
      LevelAggregate{Price{101}, Quantity{7}, 1},
      LevelAggregate{Price{102}, Quantity{3}, 1},
  };
  EXPECT_EQ(view.levels(Side::Ask), expectedAsks);
}

TEST(MarketStateIntegrationTest, FixtureProducesExpectedState) {
  OrderBook book;
  MatchingEngine engine(book);
  applyAll(engine, parseDepthFixture());

  const MarketStateView view{book};
  expectFinalState(view);
}

TEST(MarketStateIntegrationTest, StateAfterPartialPrefix) {
  OrderBook book;
  MatchingEngine engine(book);
  const std::vector<Event> events = parseDepthFixture();

  EXPECT_TRUE(engine.onEvent(events[0]));
  EXPECT_TRUE(engine.onEvent(events[1]));

  const MarketStateView view{book};
  EXPECT_EQ(view.levels(Side::Bid).size(), 1U);
  EXPECT_EQ(view.level(Side::Bid, Price{100}),
            std::optional<LevelAggregate>(LevelAggregate{Price{100}, Quantity{15}, 2}));
  EXPECT_EQ(view.bestAsk(), std::nullopt);
}

TEST(MarketStateIntegrationTest, CancelRemovesLevel) {
  OrderBook book;
  MatchingEngine engine(book);
  applyAll(engine, parseDepthFixture());

  const MarketStateView view{book};
  EXPECT_EQ(view.level(Side::Bid, Price{99}), std::nullopt);
}

TEST(MarketStateIntegrationTest, DeterministicAcrossRuns) {
  const auto runOnce = []() {
    OrderBook book;
    MatchingEngine engine(book);
    applyAll(engine, parseDepthFixture());
    const MarketStateView view{book};
    return view.levels(Side::Bid);
  };

  EXPECT_EQ(runOnce(), runOnce());
}

TEST(MarketStateIntegrationTest, MatchesDirectBookConstruction) {
  OrderBook parsed_book;
  MatchingEngine parsed_engine(parsed_book);
  applyAll(parsed_engine, parseDepthFixture());

  OrderBook direct_book;
  MatchingEngine direct_engine(direct_book);
  applyAll(direct_engine, parseDepthFixture());

  const MarketStateView parsedView{parsed_book};
  const MarketStateView directView{direct_book};
  EXPECT_EQ(parsedView.levels(Side::Bid), directView.levels(Side::Bid));
  EXPECT_EQ(parsedView.levels(Side::Ask), directView.levels(Side::Ask));
}

} // namespace
