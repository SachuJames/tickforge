// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// TickForge CSV market-data parser (SPEC.md section 9, stage 2).
//
// The parser is the input boundary: it converts one external CSV format
// into normalized TickForge Events. It owns sequence assignment,
// field validation, and rejection of malformed records with reasons.
// It is the ONLY place allowed to know about the CSV format.
//
// Supported format (documented in ARCHITECTURE.md section 11):
//   Header (required, exact): timestamp,event_type,order_id,side,price,quantity
//   One record per line, comma-separated, no quoting:
//     timestamp  : signed 64-bit integer, nanoseconds since Unix epoch
//     event_type : N (NewOrder), M (ModifyOrder), C (CancelOrder)
//     order_id   : unsigned 64-bit integer, source-assigned
//     side       : B (Bid), A (Ask)
//     price      : signed 64-bit integer ticks; NewOrder > 0, Modify >= 0
//                  (0 = unchanged), Cancel must be 0/empty
//     quantity   : signed 64-bit integer lots; same rules as price
//
// Design rules:
//   * Prices and quantities are integer ticks/lots. The format does not
//     carry decimal prices, so no conversion or rounding exists.
//   * Sequence numbers are assigned by the parser in input order from 0,
//     dense (SPEC.md 3.2). The source does not provide them.
//   * The instrument is a parser parameter (SPEC.md 2.2: exactly one per
//     session), not a per-record field.
//   * All numeric parsing uses std::from_chars: locale-independent,
//     no exceptions, exact overflow detection.
//   * After constructing an Event, the parser runs the existing
//     validateEvent() gate. Domain rules are not duplicated.
//   * Malformed records never produce events. parseStream() stops at the
//     first error (strict mode); callers needing lenient handling use
//     parseRecord() directly and collect reasons themselves.

#pragma once

#include "tickforge/event/event.hpp"

#include <cstdint>
#include <istream>
#include <string>
#include <string_view>
#include <vector>

namespace tickforge {

// Reason a CSV record was rejected. Every dropped record gets one;
// parsers must not silently drop records (SPEC.md section 11).
enum class ParseError : std::uint8_t {
  Ok = 0,
  EmptyRecord,
  BadHeader,
  WrongFieldCount,
  InvalidTimestamp,
  InvalidEventType,
  InvalidOrderId,
  InvalidSide,
  InvalidPrice,
  InvalidQuantity,
  NumericOverflow,
  EventRejected, // validateEvent() refused the constructed event
};

[[nodiscard]] std::string_view toString(ParseError error) noexcept;

// Result of parsing one record. On success, error == Ok and event holds
// the normalized event. On failure, event is default-constructed and
// diagnostic describes the problem.
struct ParseResult {
  ParseError error{ParseError::Ok};
  Event event{};
  std::uint64_t lineNumber{0};
  EventValidationError validationError{EventValidationError::Ok};
  std::string diagnostic;
};

// Result of parsing a stream. On success, error == Ok and events holds
// the normalized stream in input order with dense sequences from 0.
// On failure, events holds the records parsed before the error.
struct StreamResult {
  ParseError error{ParseError::Ok};
  std::vector<Event> events;
  std::uint64_t lineNumber{0};
  EventValidationError validationError{EventValidationError::Ok};
  std::string diagnostic;
};

class CsvParser {
public:
  explicit CsvParser(Instrument instrument);

  // Parse one CSV data record (no header). The caller supplies the
  // sequence number; parseStream() assigns dense sequences from 0.
  [[nodiscard]] ParseResult parseRecord(std::string_view line, Sequence sequence) const;

  // Parse a full stream: exact header line, then records in order.
  // Strict mode: stops at the first malformed record.
  [[nodiscard]] StreamResult parseStream(std::istream& input) const;

  [[nodiscard]] const Instrument& instrument() const noexcept;

private:
  Instrument instrument_;

  // Field parsers: return ParseError::Ok on success, or the specific error.
  // On failure, diagnostic is set. These are private helpers to keep
  // parseRecord's cognitive complexity within limits.
  [[nodiscard]] static ParseError
  parseTimestampField(std::string_view field, Event& event, std::string& diagnostic) noexcept;
  [[nodiscard]] static ParseError
  parseEventTypeField(std::string_view field, Event& event, std::string& diagnostic) noexcept;
  [[nodiscard]] static ParseError
  parseOrderIdField(std::string_view field, Event& event, std::string& diagnostic) noexcept;
  [[nodiscard]] static ParseError
  parseSideField(std::string_view field, Event& event, std::string& diagnostic) noexcept;
  [[nodiscard]] static ParseError
  parsePriceField(std::string_view field, Event& event, std::string& diagnostic) noexcept;
  [[nodiscard]] static ParseError
  parseQuantityField(std::string_view field, Event& event, std::string& diagnostic) noexcept;
};

} // namespace tickforge
