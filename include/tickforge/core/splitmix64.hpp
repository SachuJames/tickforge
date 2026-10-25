// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// SplitMix64 PRNG (SPEC.md 14.1): the project's single deterministic
// random source. Production code and test infrastructure share this
// implementation; there is no second copy.
//
// The state is a uint64 initialized to the seed verbatim. Every
// uint64 value is a valid seed, including 0 and 2^64 - 1. All
// arithmetic is unsigned, so the output stream is a pure function of
// the seed and the number of draws consumed, identical on every
// platform. No entropy source, no global state: each instance owns
// its stream.

#pragma once

#include <cstdint>

namespace tickforge {

class SplitMix64 {
public:
  explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  // Next 64-bit output.
  [[nodiscard]] std::uint64_t next() noexcept {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  // Uniform in [0, bound) via modulo reduction (SPEC.md 14.2). The
  // mapping has a small bias for bounds that do not divide 2^64;
  // that bias is documented and contractual. Requires bound > 0.
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

} // namespace tickforge
