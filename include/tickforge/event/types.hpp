// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Strongly typed domain primitives for the TickForge normalized event
// model (SPEC.md section 2). Each wrapper exists so the compiler rejects
// accidental mixing of timestamps, sequence numbers, order ids, prices,
// and quantities. The wrappers are trivial value types: constexpr,
// totally ordered, and heap-free. They perform no semantic validation;
// that is the separate, explicit job of validateEvent().

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

namespace tickforge {

// Timestamp: signed 64-bit nanoseconds since the Unix epoch (SPEC.md 3.1).
// Signedness is deliberate: pre-epoch and relative-time fixtures stay
// representable and timestamp arithmetic cannot underflow.
class Timestamp {
public:
  using ValueType = std::int64_t;

  constexpr Timestamp() noexcept : nanos_(0) {}
  explicit constexpr Timestamp(std::int64_t nanos) noexcept : nanos_(nanos) {}

  [[nodiscard]] constexpr std::int64_t count() const noexcept {
    return nanos_;
  }

  static constexpr Timestamp min() noexcept {
    return Timestamp{(std::numeric_limits<std::int64_t>::min)()};
  }
  static constexpr Timestamp max() noexcept {
    return Timestamp{(std::numeric_limits<std::int64_t>::max)()};
  }

  constexpr bool operator==(const Timestamp&) const noexcept = default;
  constexpr auto operator<=>(const Timestamp&) const noexcept = default;

private:
  std::int64_t nanos_;
};

// Sequence: parser-assigned, dense uint64 sequence number (SPEC.md 3.2).
// Breaks ties between events sharing a timestamp.
class Sequence {
public:
  using ValueType = std::uint64_t;

  constexpr Sequence() noexcept : value_(0) {}
  explicit constexpr Sequence(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return value_;
  }

  static constexpr Sequence min() noexcept {
    return Sequence{0};
  }
  static constexpr Sequence max() noexcept {
    return Sequence{(std::numeric_limits<std::uint64_t>::max)()};
  }

  constexpr bool operator==(const Sequence&) const noexcept = default;
  constexpr auto operator<=>(const Sequence&) const noexcept = default;

private:
  std::uint64_t value_;
};

// OrderId: source-assigned order identity, unique within a session
// (SPEC.md 5.1). The identity key of the future MBO order book.
class OrderId {
public:
  using ValueType = std::uint64_t;

  constexpr OrderId() noexcept : value_(0) {}
  explicit constexpr OrderId(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return value_;
  }

  constexpr bool operator==(const OrderId&) const noexcept = default;
  constexpr auto operator<=>(const OrderId&) const noexcept = default;

private:
  std::uint64_t value_;
};

// Price: integer count of the instrument's minimum price tick
// (SPEC.md 2.4). Floating point prices are forbidden in the core model.
class Price {
public:
  using ValueType = std::int64_t;

  constexpr Price() noexcept : ticks_(0) {}
  explicit constexpr Price(std::int64_t ticks) noexcept : ticks_(ticks) {}

  [[nodiscard]] constexpr std::int64_t ticks() const noexcept {
    return ticks_;
  }

  constexpr bool operator==(const Price&) const noexcept = default;
  constexpr auto operator<=>(const Price&) const noexcept = default;

private:
  std::int64_t ticks_;
};

// Quantity: integer count of the instrument's minimum lot size
// (SPEC.md 2.4). Floating point quantities are forbidden in the core model.
class Quantity {
public:
  using ValueType = std::int64_t;

  constexpr Quantity() noexcept : lots_(0) {}
  explicit constexpr Quantity(std::int64_t lots) noexcept : lots_(lots) {}

  [[nodiscard]] constexpr std::int64_t lots() const noexcept {
    return lots_;
  }

  constexpr bool operator==(const Quantity&) const noexcept = default;
  constexpr auto operator<=>(const Quantity&) const noexcept = default;

private:
  std::int64_t lots_;
};

} // namespace tickforge

namespace std {

// Lets OrderId serve as a key in unordered containers (the future order
// book indexes resting orders by id).
template <>
struct hash<tickforge::OrderId> {
  std::size_t operator()(tickforge::OrderId id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value());
  }
};

} // namespace std
