// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Session configuration (SPEC.md sections 10 and 9.6).
//
// Responsibility: the single, validated description of a simulation run.
// A SessionConfig names the instrument, its tick/lot declarations, the
// matching rule, the PRNG seed (when a model needs randomness, SPEC.md
// 4.3), and the replay error mode. It is a plain value type: no
// inheritance, no global state, no hidden mutation.
//
// Field provenance (SPEC.md 10):
//   instrument     - required; exactly one per session (SPEC.md 2.2)
//   tickSize       - optional decimal string; declared per instrument
//                    (SPEC.md 2.4). Absent means "not declared".
//   lotSize        - optional decimal string; same as tickSize.
//   matchingRule   - required; PriceTimePriority is the only rule today.
//                    SPEC.md 6 requires the selection to exist so future
//                    rules (e.g. pro-rata) are explicit.
//   prngSeed       - optional; present only when a model needs randomness
//                    (SPEC.md 4.3). TickForge has no stochastic models.
//   mode           - required; Strict default, Lenient on explicit
//                    request (SPEC.md 11).
//
// Microstructure model parameters are intentionally absent: TickForge
// implements no microstructure models (latency, fees), so there is no
// schema to configure. They will join the contract when such a model
// does (SPEC.md 10 lists the category; this documents the gap).
//
// Effective configuration: the validated SessionConfig with documented
// defaults applied (matchingRule, mode). Optional fields that are
// absent stay absent; absence is part of the configuration's identity.
// The configuration hash is computed over the effective configuration's
// canonical serialization, so equivalent user inputs hash identically.

#pragma once

#include "tickforge/event/event.hpp"
#include "tickforge/replay/replay_mode.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tickforge {

// Matching rule selection (SPEC.md 6 and 10). Only price-time priority
// exists; the enum exists so any future rule is an explicit, named
// selection rather than a silent behavior change.
enum class MatchingRule : std::uint8_t { PriceTimePriority };

[[nodiscard]] std::string_view toString(MatchingRule rule) noexcept;

struct SessionConfig {
  Instrument instrument; // required, non-empty
  // Optional decimal strings, e.g. "0.01". Validated as decimal, never
  // parsed to floating point (SPEC.md 2.4 forbids float in the core).
  std::optional<std::string> tickSize;
  std::optional<std::string> lotSize;
  MatchingRule matchingRule = MatchingRule::PriceTimePriority;
  std::optional<std::uint64_t> prngSeed; // absent: no randomness in use
  ReplayMode mode = ReplayMode::Strict;

  [[nodiscard]] bool operator==(const SessionConfig&) const = default;
};

enum class ConfigError : std::uint8_t {
  Ok = 0,
  EmptyInstrument,    // instrument is empty
  InvalidInstrument,  // instrument contains '=' or a newline (unserializable)
  InvalidTickSize,    // not a valid decimal string
  InvalidLotSize,     // not a valid decimal string
  UnknownMatchingRule // not a known MatchingRule value
};

[[nodiscard]] std::string_view toString(ConfigError error) noexcept;

// Validates a user-supplied configuration. Returns Ok only when the
// configuration is usable as an effective configuration. Does not
// mutate its argument.
[[nodiscard]] ConfigError validateConfig(const SessionConfig& config) noexcept;

// Canonical serialization (SPEC.md 10: "MUST be serializable").
//
// Format (this is the file format; writing the string to a file is the
// serialization):
//   line 1: "tickforge-config/1"            (schema identifier + version)
//   lines 2..n: "key=value", one per field, keys in byte-wise sorted
//     order: instrument, lot-size, matching-rule, mode, prng-seed,
//     tick-size. Absent optional fields are omitted.
//   lines end with '\n' (LF only); no trailing whitespace.
//
// Deterministic: equivalent effective configurations produce byte-identical
// output. Locale-independent. Integer-exact. No memory addresses.
[[nodiscard]] std::string serializeConfig(const SessionConfig& config);

enum class ConfigParseError : std::uint8_t {
  Ok = 0,
  BadSchema,      // first line is not "tickforge-config/1"
  MalformedLine,  // a line without exactly one '=' separator
  DuplicateField, // the same key appears twice
  UnknownField,   // a key outside the schema
  MissingField,   // a required key is absent
  InvalidValue    // a value fails validateConfig
};

[[nodiscard]] std::string_view toString(ConfigParseError error) noexcept;

struct ConfigParseResult {
  ConfigParseError error = ConfigParseError::Ok;
  SessionConfig config; // valid only when error == Ok
};

// Parses canonical serialization back into a configuration. The result
// is validated before it is returned: deserialize(serialize(c)) == c
// for every valid c. Malformed input is rejected, never repaired.
[[nodiscard]] ConfigParseResult deserializeConfig(std::string_view text);

// Deterministic configuration hash (SPEC.md 10: outputs "embed its
// hash"). FNV-1a 64-bit over the canonical serialization bytes.
//
// Purpose: reproducibility and identity, NOT security. Documented
// choice: SPEC.md names no algorithm; FNV-1a is simple, dependency-free,
// and deterministic across processes and platforms. Do not treat the
// hash as collision-free identity.
[[nodiscard]] std::uint64_t hashConfig(const SessionConfig& config);

// Formats a config hash as 16 lowercase hex characters.
[[nodiscard]] std::string formatConfigHash(std::uint64_t hash);

} // namespace tickforge
