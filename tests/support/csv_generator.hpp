// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Deterministic CSV generator for parser testing (test infrastructure
// only, never production).
//
// Generates two classes of inputs:
//   A. Valid CSV streams conforming to the audited parser contract.
//   B. Malformed/adversarial streams via controlled mutations.
//
// Uses the Day 11 DeterministicRng. Same config -> same output.
// Every generated case is reproducible from seed + config.

#pragma once

#include "tests/support/deterministic_rng.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tickforge::test {

struct CsvGeneratorConfig {
  std::uint64_t seed = 0;
  std::size_t recordCount = 20;
  // Mutation rate for malformed streams: 0 = all valid, 100 = all malformed.
  int malformedPercent = 0;
};

class CsvGenerator {
public:
  explicit CsvGenerator(const CsvGeneratorConfig& config);

  [[nodiscard]] const CsvGeneratorConfig& config() const noexcept {
    return config_;
  }

  // Generates a complete CSV stream (header + records).
  [[nodiscard]] std::string generate();

  // Generates a single valid record line (no header).
  [[nodiscard]] std::string generateValidRecord();

  // Applies one deterministic malformation to a valid record.
  [[nodiscard]] std::string malformRecord(const std::string& validRecord);

private:
  CsvGeneratorConfig config_;
  DeterministicRng rng_;
  std::uint64_t nextOrderId_ = 1;
  std::int64_t timestamp_ = 1'000'000;

  [[nodiscard]] std::string randomNumericField(bool allowEmpty, bool signedValue);
  [[nodiscard]] std::string randomEventType();
  [[nodiscard]] std::string randomSide();
};

// A single malformed input case with its expected outcome category.
struct MalformedCase {
  std::string input;       // Full CSV stream
  std::string description; // Human-readable description
};

// Generates a table of malformed inputs covering structural, enum,
// numeric, and boundary errors. Deterministic.
[[nodiscard]] std::vector<MalformedCase> generateMalformedTable();

} // namespace tickforge::test
