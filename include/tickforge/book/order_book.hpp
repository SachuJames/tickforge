// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// MBO order book (SPEC.md section 5): individually tracked resting orders,
// organized by side and price level. This is state, not matching: the book
// stores and retrieves; the future matching engine will consume it.
//
// Representation:
//   * orders_: OrderId -> resting order (+ its queue iterator), for O(1)
//     lookup by id.
//   * bids_/asks_: price -> FIFO queue of order ids at that price, ordered
//     highest-first for bids and lowest-first for asks, so bestBid() and
//     bestAsk() are O(1).
//   * Within a price level, queue order is arrivalSeq order (FIFO). Order
//     ids never determine priority: ids identify, sequences order.
//
// Day 03 scope notes:
//   * The book performs no matching itself. Matching is the
//     MatchingEngine's job (Day 04); the engine drives the book through
//     the public mutation API below. OrderBook::onEvent retains Day 03
//     semantics: a NewOrder rests without matching. Aggressive handling
//     belongs to the engine.
//   * ModifyOrder follows SPEC.md 5.5 exactly: a price change or quantity
//     increase is cancel/replace (loses time priority, arrivalSeq becomes
//     the modify event's seq); a quantity decrease at the same price keeps
//     its queue position.
//   * The book enforces live-uniqueness of order ids. Session-wide id
//     uniqueness is the parser's job (SPEC.md 5.1); the book cannot
//     distinguish a reused id from a fresh one after cancellation.
//   * onEvent assumes validated events (the replay driver guarantees it).

#pragma once

#include "tickforge/event/event.hpp"
#include "tickforge/replay/event_processor.hpp"

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace tickforge {

// Public, read-only view of a resting order.
struct RestingOrder {
  OrderId id;
  Side side{Side::Bid};
  Price price;
  Quantity quantity;   // remaining lots
  Sequence arrivalSeq; // seq of the NewOrder event: the time-priority key
};

class OrderBook : public EventProcessor {
public:
  OrderBook() = default;

  // Applies one validated event to the book. Returns false when the event
  // is rejected: cancel/modify of a non-resting order, or a new order
  // whose id is already live.
  bool onEvent(const Event& event) override;

  // Read-only queries. None of them mutate the book.
  [[nodiscard]] bool contains(OrderId id) const;
  [[nodiscard]] std::optional<RestingOrder> find(OrderId id) const;
  [[nodiscard]] std::optional<Price> bestBid() const;
  [[nodiscard]] std::optional<Price> bestAsk() const;
  [[nodiscard]] std::size_t orderCount() const noexcept;
  [[nodiscard]] std::size_t priceLevelCount(Side side) const noexcept;
  // Order ids resting at the level, in FIFO (arrivalSeq) order.
  // Empty when the level does not exist.
  [[nodiscard]] std::vector<OrderId> ordersAtLevel(Side side, Price price) const;
  // All price levels on the side, in priority order (best first).
  [[nodiscard]] std::vector<Price> priceLevels(Side side) const;

  // Controlled mutation API for the matching engine (Day 04). These are
  // the only way to mutate the book besides onEvent; the engine must not
  // reach into the book's containers.
  //
  // Adds a resting order directly. Returns false if the id is already
  // live. The caller provides the arrivalSeq (time-priority key).
  bool addRestingOrder(OrderId id, Side side, Price price, Quantity quantity, Sequence arrivalSeq);
  // Removes the order. Returns false if not found.
  bool removeOrder(OrderId id);
  // Reduces the order's remaining quantity by lots, preserving its queue
  // position and arrivalSeq. Returns false if not found or if lots
  // exceeds the remaining quantity. A reduction to zero removes the order.
  bool reduceQuantity(OrderId id, Quantity lots);

private:
  using Queue = std::list<OrderId>;
  struct Level {
    Queue queue;
  };
  struct Entry {
    RestingOrder order;
    Queue::iterator queueIt; // position within its price-level queue
  };

  // Price levels, ordered so the best price is always *begin().
  std::map<Price, Level, std::greater<>> bids_;
  std::map<Price, Level, std::less<>> asks_;
  std::unordered_map<OrderId, Entry> orders_;

  bool applyNew(const Event& event);
  bool applyCancel(const Event& event);
  bool applyModify(const Event& event);

  template <typename LevelMap>
  void insertIntoLevel(LevelMap& levels, Entry& entry);
  template <typename LevelMap>
  void removeFromLevel(LevelMap& levels, Entry& entry);
};

} // namespace tickforge
