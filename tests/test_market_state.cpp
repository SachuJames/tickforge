// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// MarketStateView unit tests: derived level aggregates over the MBO
// OrderBook. The view observes; it never mutates the book and never
// invents order identity.

#include "tickforge/book/order_book.hpp"
#include "tickforge/event/event.hpp"
#include "tickforge/market_data/market_state.hpp"

#include <gtest/gtest.h>
#include <optional>

namespace {

using tickforge::LevelAggregate;
using tickforge::MarketStateView;
using tickforge::OrderBook;
using tickforge::OrderId;
using tickforge::Price;
using tickforge::Quantity;
using tickforge::Sequence;
using tickforge::Side;

// Build a book directly through the controlled mutation API.
OrderBook makeBook() {
  OrderBook book;
  EXPECT_TRUE(book.addRestingOrder(OrderId{1}, Side::Bid, Price{100}, Quantity{10}, Sequence{0}));
  EXPECT_TRUE(book.addRestingOrder(OrderId{2}, Side::Bid, Price{100}, Quantity{5}, Sequence{1}));
  EXPECT_TRUE(book.addRestingOrder(OrderId{3}, Side::Bid, Price{99}, Quantity{8}, Sequence{2}));
  EXPECT_TRUE(book.addRestingOrder(OrderId{4}, Side::Ask, Price{101}, Quantity{7}, Sequence{3}));
  EXPECT_TRUE(book.addRestingOrder(OrderId{5}, Side::Ask, Price{102}, Quantity{3}, Sequence{4}));
  return book;
}

TEST(MarketStateViewTest, EmptyBook) {
  const OrderBook book;
  const MarketStateView view{book};

  EXPECT_EQ(view.bestBid(), std::nullopt);
  EXPECT_EQ(view.bestAsk(), std::nullopt);
  EXPECT_EQ(view.orderCount(), 0U);
  EXPECT_TRUE(view.levels(Side::Bid).empty());
  EXPECT_TRUE(view.levels(Side::Ask).empty());
  EXPECT_EQ(view.level(Side::Bid, Price{100}), std::nullopt);
}

TEST(MarketStateViewTest, BestBidAndAsk) {
  const OrderBook book = makeBook();
  const MarketStateView view{book};

  EXPECT_EQ(view.bestBid(), std::optional<Price>(Price{100}));
  EXPECT_EQ(view.bestAsk(), std::optional<Price>(Price{101}));
  EXPECT_EQ(view.orderCount(), 5U);
}

TEST(MarketStateViewTest, LevelAggregateSumsQuantities) {
  const OrderBook book = makeBook();
  const MarketStateView view{book};

  EXPECT_EQ(view.level(Side::Bid, Price{100}),
            std::optional<LevelAggregate>(LevelAggregate{Price{100}, Quantity{15}, 2}));
}

TEST(MarketStateViewTest, BidLevelsBestFirst) {
  const OrderBook book = makeBook();
  const MarketStateView view{book};

  const std::vector<LevelAggregate> expected{
      LevelAggregate{Price{100}, Quantity{15}, 2},
      LevelAggregate{Price{99}, Quantity{8}, 1},
  };
  EXPECT_EQ(view.levels(Side::Bid), expected);
}

TEST(MarketStateViewTest, AskLevelsBestFirst) {
  const OrderBook book = makeBook();
  const MarketStateView view{book};

  const std::vector<LevelAggregate> expected{
      LevelAggregate{Price{101}, Quantity{7}, 1},
      LevelAggregate{Price{102}, Quantity{3}, 1},
  };
  EXPECT_EQ(view.levels(Side::Ask), expected);
}

TEST(MarketStateViewTest, UnknownLevelIsEmpty) {
  const OrderBook book = makeBook();
  const MarketStateView view{book};

  EXPECT_EQ(view.level(Side::Bid, Price{50}), std::nullopt);
  EXPECT_EQ(view.level(Side::Ask, Price{200}), std::nullopt);
}

TEST(MarketStateViewTest, CancelRemovesLevel) {
  OrderBook book = makeBook();
  EXPECT_TRUE(book.removeOrder(OrderId{3}));
  const MarketStateView view{book};

  EXPECT_EQ(view.levels(Side::Bid).size(), 1U);
  EXPECT_EQ(view.level(Side::Bid, Price{99}), std::nullopt);
}

TEST(MarketStateViewTest, ReduceQuantityUpdatesAggregate) {
  OrderBook book = makeBook();
  EXPECT_TRUE(book.reduceQuantity(OrderId{1}, Quantity{4}));
  const MarketStateView view{book};

  EXPECT_EQ(view.level(Side::Bid, Price{100}),
            std::optional<LevelAggregate>(LevelAggregate{Price{100}, Quantity{11}, 2}));
}

TEST(MarketStateViewTest, ReduceToZeroRemovesLevel) {
  OrderBook book = makeBook();
  EXPECT_TRUE(book.reduceQuantity(OrderId{3}, Quantity{8}));
  const MarketStateView view{book};

  EXPECT_EQ(view.level(Side::Bid, Price{99}), std::nullopt);
  EXPECT_EQ(view.levels(Side::Bid).size(), 1U);
}

TEST(MarketStateViewTest, ViewDoesNotMutateBook) {
  const OrderBook book = makeBook();
  const std::size_t before = book.orderCount();
  {
    const MarketStateView view{book};
    (void)view.levels(Side::Bid);
    (void)view.levels(Side::Ask);
    (void)view.bestBid();
    (void)view.bestAsk();
  }
  EXPECT_EQ(book.orderCount(), before);
}

TEST(MarketStateViewTest, DeterministicAcrossViews) {
  const OrderBook book = makeBook();
  const MarketStateView first{book};
  const MarketStateView second{book};

  EXPECT_EQ(first.levels(Side::Bid), second.levels(Side::Bid));
  EXPECT_EQ(first.levels(Side::Ask), second.levels(Side::Ask));
  EXPECT_EQ(first.bestBid(), second.bestBid());
  EXPECT_EQ(first.bestAsk(), second.bestAsk());
}

TEST(MarketStateViewTest, ReflectsBookChanges) {
  OrderBook book;
  EXPECT_TRUE(book.addRestingOrder(OrderId{1}, Side::Bid, Price{100}, Quantity{10}, Sequence{0}));
  const MarketStateView view{book};
  EXPECT_EQ(view.level(Side::Bid, Price{100}),
            std::optional<LevelAggregate>(LevelAggregate{Price{100}, Quantity{10}, 1}));

  // The view is a lens: it sees the book's current state.
  EXPECT_TRUE(book.addRestingOrder(OrderId{2}, Side::Bid, Price{100}, Quantity{5}, Sequence{1}));
  EXPECT_EQ(view.level(Side::Bid, Price{100}),
            std::optional<LevelAggregate>(LevelAggregate{Price{100}, Quantity{15}, 2}));
}

} // namespace
