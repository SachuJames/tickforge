// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Deterministic valid event generator (test infrastructure only).
//
// Generates bounded sequences of valid TickForge events using a
// DeterministicRng and an internal ReferenceBook to track live orders.
// Because the generator applies each event to its own reference book,
// cancels and modifies always target live orders with consistent
// quantities, so every generated sequence is valid by construction.
// Invalid-event testing belongs in a separate negative family.
//
// Same GeneratorConfig -> same event sequence, on every platform.

#pragma once

#include "tests/support/deterministic_rng.hpp"
#include "tests/support/reference_model.hpp"
#include "tickforge/event/event.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tickforge::test {

struct GeneratorConfig {
  std::uint64_t seed = 0;
  std::size_t eventCount = 50;
  // Price/quantity bounds (inclusive). Tight default spread encourages
  // both resting and crossing orders.
  std::int64_t minPrice = 95;
  std::int64_t maxPrice = 105;
  std::int64_t minQuantity = 1;
  std::int64_t maxQuantity = 10;
  // Operation weights (relative; need not sum to 100).
  int newWeight = 50;
  int cancelWeight = 25;
  int modifyWeight = 25;
  std::string instrument = "AAPL";
};

class EventGenerator {
public:
  explicit EventGenerator(const GeneratorConfig& config);

  [[nodiscard]] const GeneratorConfig& config() const noexcept {
    return config_;
  }

  // Generates the deterministic event sequence.
  [[nodiscard]] std::vector<Event> generate();

  // Human-readable rendering of an event for failure reports.
  [[nodiscard]] static std::string describe(const Event& event);

private:
  GeneratorConfig config_;
  DeterministicRng rng_;
  ReferenceBook book_; // tracks live orders for valid cancel/modify
  std::uint64_t nextOrderId_ = 1;
  std::int64_t timestamp_ = 1'000'000;

  [[nodiscard]] Event makeNewOrder();
  [[nodiscard]] Event makeCancelOrder();
  [[nodiscard]] Event makeModifyOrder();
  [[nodiscard]] Side randomSide();
  [[nodiscard]] Price randomPrice();
  [[nodiscard]] Quantity randomQuantity();
  // Picks a random live order id. Precondition: book not empty.
  [[nodiscard]] OrderId randomLiveId();
  // Picks a random live order with its current details.
  // Precondition: book not empty.
  [[nodiscard]] ReferenceOrder randomLiveOrder();
};

} // namespace tickforge::test
