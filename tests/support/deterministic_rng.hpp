// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Deterministic test RNG (test infrastructure only, never production).
//
// splitmix64: a small, well-defined, portable PRNG. Same seed ->
// same sequence on every platform and compiler. Chosen over
// std::mt19937 because its algorithm is fully specified by a few
// lines of code below, leaving no implementation-defined behavior.

#pragma once

#include <cstdint>

namespace tickforge::test {

class DeterministicRng {
public:
  explicit DeterministicRng(std::uint64_t seed) noexcept : state_(seed) {}

  // Next 64-bit output.
  [[nodiscard]] std::uint64_t next() noexcept {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  // Uniform in [0, bound). Requires bound > 0. Modulo bias is
  // irrelevant for test generation.
  [[nodiscard]] std::uint64_t nextBounded(std::uint64_t bound) noexcept {
    return next() % bound;
  }

  // Uniform in [lo, hi]. Requires lo <= hi.
  [[nodiscard]] std::uint64_t nextRange(std::uint64_t lo, std::uint64_t hi) noexcept {
    return lo + nextBounded(hi - lo + 1);
  }

  [[nodiscard]] bool nextBool() noexcept {
    return (next() & 1ULL) != 0;
  }

private:
  std::uint64_t state_;
};

} // namespace tickforge::test
