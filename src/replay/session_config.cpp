// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Session configuration implementation: validation, canonical
// serialization, deserialization, and deterministic hashing.

#include "tickforge/replay/session_config.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>
#include <vector>

namespace tickforge {

std::string_view toString(MatchingRule rule) noexcept {
  switch (rule) {
  case MatchingRule::PriceTimePriority:
    return "PriceTimePriority";
  }
  return "Unknown";
}

std::string_view toString(ConfigError error) noexcept {
  switch (error) {
  case ConfigError::Ok:
    return "Ok";
  case ConfigError::EmptyInstrument:
    return "EmptyInstrument";
  case ConfigError::InvalidInstrument:
    return "InvalidInstrument";
  case ConfigError::InvalidTickSize:
    return "InvalidTickSize";
  case ConfigError::InvalidLotSize:
    return "InvalidLotSize";
  case ConfigError::UnknownMatchingRule:
    return "UnknownMatchingRule";
  }
  return "Unknown";
}

std::string_view toString(ConfigParseError error) noexcept {
  switch (error) {
  case ConfigParseError::Ok:
    return "Ok";
  case ConfigParseError::BadSchema:
    return "BadSchema";
  case ConfigParseError::MalformedLine:
    return "MalformedLine";
  case ConfigParseError::DuplicateField:
    return "DuplicateField";
  case ConfigParseError::UnknownField:
    return "UnknownField";
  case ConfigParseError::MissingField:
    return "MissingField";
  case ConfigParseError::InvalidValue:
    return "InvalidValue";
  }
  return "Unknown";
}

namespace {

// A decimal string is one or more ASCII digits, optionally followed by
// '.' and one or more ASCII digits. Locale-independent; never parsed
// to floating point.
[[nodiscard]] bool isDecimal(std::string_view text) noexcept {
  if (text.empty()) {
    return false;
  }
  std::size_t i = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    ++i;
  }
  if (i == 0) {
    return false;
  }
  if (i == text.size()) {
    return true;
  }
  if (text[i] != '.') {
    return false;
  }
  ++i;
  const std::size_t fracStart = i;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    ++i;
  }
  return i == text.size() && i > fracStart;
}

[[nodiscard]] bool isSerializableInstrument(std::string_view instrument) noexcept {
  return std::ranges::all_of(instrument, [](char c) { return c != '=' && c != '\n' && c != '\r'; });
}

} // namespace

ConfigError validateConfig(const SessionConfig& config) noexcept {
  if (config.instrument.empty()) {
    return ConfigError::EmptyInstrument;
  }
  if (!isSerializableInstrument(config.instrument)) {
    return ConfigError::InvalidInstrument;
  }
  if (config.tickSize.has_value() && !isDecimal(*config.tickSize)) {
    return ConfigError::InvalidTickSize;
  }
  if (config.lotSize.has_value() && !isDecimal(*config.lotSize)) {
    return ConfigError::InvalidLotSize;
  }
  if (config.matchingRule != MatchingRule::PriceTimePriority) {
    return ConfigError::UnknownMatchingRule;
  }
  return ConfigError::Ok;
}

std::string serializeConfig(const SessionConfig& config) {
  // Canonical key order (byte-wise): instrument, lot-size,
  // matching-rule, mode, prng-seed, tick-size. Absent optionals are
  // omitted. Assumes a validated config; see validateConfig.
  std::string out = "tickforge-config/1\n";
  out += "instrument=" + config.instrument + "\n";
  if (config.lotSize.has_value()) {
    out += "lot-size=" + *config.lotSize + "\n";
  }
  out += "matching-rule=";
  out +=
      (config.matchingRule == MatchingRule::PriceTimePriority ? "price-time-priority" : "unknown");
  out += "\n";
  out += "mode=";
  out += (config.mode == ReplayMode::Lenient ? "lenient" : "strict");
  out += "\n";
  if (config.prngSeed.has_value()) {
    out += "prng-seed=" + std::to_string(*config.prngSeed) + "\n";
  }
  if (config.tickSize.has_value()) {
    out += "tick-size=" + *config.tickSize + "\n";
  }
  return out;
}

// Tracks which fields have been seen while parsing, so duplicates are
// rejected.
struct SeenFields {
  bool instrument = false;
  bool tickSize = false;
  bool lotSize = false;
  bool matchingRule = false;
  bool mode = false;
  bool prngSeed = false;
};

// Splits text into '\n'-terminated lines. Returns false when trailing
// content lacks its terminating newline (not canonical).
[[nodiscard]] bool splitLines(std::string_view text, std::vector<std::string_view>& lines) {
  std::size_t start = 0;
  while (true) {
    const std::size_t nl = text.find('\n', start);
    if (nl == std::string_view::npos) {
      return start == text.size();
    }
    lines.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
}

[[nodiscard]] ConfigParseError applySeed(std::string_view value, SessionConfig& config) {
  std::uint64_t seed = 0;
  const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), seed);
  if (ec != std::errc() || ptr != value.data() + value.size()) {
    return ConfigParseError::InvalidValue;
  }
  config.prngSeed = seed;
  return ConfigParseError::Ok;
}

// A parsed "key=value" line.
struct ParsedField {
  std::string_view key;
  std::string_view value;
};

[[nodiscard]] std::optional<ParsedField> parseLine(std::string_view line) {
  const std::size_t eq = line.find('=');
  if (eq == std::string_view::npos || line.find('=', eq + 1) != std::string_view::npos) {
    return std::nullopt;
  }
  return ParsedField{line.substr(0, eq), line.substr(eq + 1)};
}

// Applies one of the plain string fields (instrument, tick-size,
// lot-size). Returns UnknownField when the key is not one of them.
[[nodiscard]] ConfigParseError
applySimpleField(const ParsedField& field, SessionConfig& config, SeenFields& seen) {
  if (field.key == "instrument") {
    if (seen.instrument) {
      return ConfigParseError::DuplicateField;
    }
    seen.instrument = true;
    config.instrument = std::string(field.value);
  } else if (field.key == "tick-size") {
    if (seen.tickSize) {
      return ConfigParseError::DuplicateField;
    }
    seen.tickSize = true;
    config.tickSize = std::string(field.value);
  } else if (field.key == "lot-size") {
    if (seen.lotSize) {
      return ConfigParseError::DuplicateField;
    }
    seen.lotSize = true;
    config.lotSize = std::string(field.value);
  } else {
    return ConfigParseError::UnknownField;
  }
  return ConfigParseError::Ok;
}

// Applies one of the validated fields (matching-rule, mode, prng-seed).
// Returns UnknownField when the key is not one of them.
[[nodiscard]] ConfigParseError
applyRuledField(const ParsedField& field, SessionConfig& config, SeenFields& seen) {
  if (field.key == "matching-rule") {
    if (seen.matchingRule) {
      return ConfigParseError::DuplicateField;
    }
    seen.matchingRule = true;
    if (field.value != "price-time-priority") {
      return ConfigParseError::InvalidValue;
    }
    config.matchingRule = MatchingRule::PriceTimePriority;
  } else if (field.key == "mode") {
    if (seen.mode) {
      return ConfigParseError::DuplicateField;
    }
    seen.mode = true;
    if (field.value == "strict") {
      config.mode = ReplayMode::Strict;
    } else if (field.value == "lenient") {
      config.mode = ReplayMode::Lenient;
    } else {
      return ConfigParseError::InvalidValue;
    }
  } else if (field.key == "prng-seed") {
    if (seen.prngSeed) {
      return ConfigParseError::DuplicateField;
    }
    seen.prngSeed = true;
    return applySeed(field.value, config);
  } else {
    return ConfigParseError::UnknownField;
  }
  return ConfigParseError::Ok;
}

// Applies one "key=value" line to the config being built.
[[nodiscard]] ConfigParseError
applyField(std::string_view line, SessionConfig& config, SeenFields& seen) {
  const std::optional<ParsedField> field = parseLine(line);
  if (!field.has_value()) {
    return ConfigParseError::MalformedLine;
  }
  const ConfigParseError simple = applySimpleField(*field, config, seen);
  if (simple != ConfigParseError::UnknownField) {
    return simple;
  }
  return applyRuledField(*field, config, seen);
}

ConfigParseResult deserializeConfig(std::string_view text) {
  ConfigParseResult result;

  std::vector<std::string_view> lines;
  if (!splitLines(text, lines)) {
    result.error = ConfigParseError::MalformedLine;
    return result;
  }

  if (lines.empty() || lines[0] != "tickforge-config/1") {
    result.error = ConfigParseError::BadSchema;
    return result;
  }

  SessionConfig config;
  SeenFields seen;
  for (std::size_t i = 1; i < lines.size(); ++i) {
    const ConfigParseError fieldError = applyField(lines[i], config, seen);
    if (fieldError != ConfigParseError::Ok) {
      result.error = fieldError;
      return result;
    }
  }

  if (!seen.instrument) {
    result.error = ConfigParseError::MissingField;
    return result;
  }
  if (validateConfig(config) != ConfigError::Ok) {
    result.error = ConfigParseError::InvalidValue;
    return result;
  }
  result.config = std::move(config);
  return result;
}

std::uint64_t hashConfig(const SessionConfig& config) {
  // FNV-1a 64-bit over the canonical serialization bytes.
  const std::string canonical = serializeConfig(config);
  std::uint64_t hash = 14695981039346656037ULL; // offset basis
  for (const char byte : canonical) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
    hash *= 1099511628211ULL; // FNV prime
  }
  return hash;
}

std::string formatConfigHash(std::uint64_t hash) {
  constexpr std::array<char, 16> digits = {
      '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = digits[hash & 0xFULL];
    hash >>= 4;
  }
  return out;
}

} // namespace tickforge
