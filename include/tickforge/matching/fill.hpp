// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Fill: the deterministic execution record produced by the matching
// engine (SPEC.md sections 4.2, 5.4, and 9, stage 6).
//
// A Fill is NOT a normalized market event. SPEC.md 2.3 is explicit:
// "Trade events do NOT appear in the order flow." Fills are execution
// results, emitted alongside the updated market state.
//
// Execution price is the resting order's price (price-time priority:
// the aggressor receives price improvement up to its limit). This is
// definitional to the limit-order-book model in SPEC.md sections 1.1,
// 5.4, and 6.
//
// Timestamp and sequence identify WHEN the fill happened: they are the
// aggressor event's timestamp and seq. One input event may generate many
// fills; those fills share timestamp/sequence, and their deterministic
// order is the order they appear in the results vector: best eligible
// price first, then FIFO (arrivalSeq) within a price.

#pragma once

#include "tickforge/event/event.hpp"

namespace tickforge {

struct Fill {
  OrderId aggressorId; // incoming order that crossed the book
  OrderId restingId;   // resting order that was matched
  Side side;           // aggressor's side: the direction of the trade
  Price price;         // execution price in ticks: the resting order's price
  Quantity quantity;   // execution quantity in lots: always positive
  Timestamp timestamp; // aggressor event's timestamp
  Sequence sequence;   // aggressor event's seq
};

} // namespace tickforge
