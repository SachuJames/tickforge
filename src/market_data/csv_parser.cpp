// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// CsvParser implementation. All numeric conversion uses std::from_chars:
// locale-independent, exception-free, with exact overflow detection.

#include "tickforge/market_data/csv_parser.hpp"

#include <array>
#include <charconv>
#include <string_view>

namespace tickforge {

namespace {

// Expected header line, byte-exact.
constexpr std::string_view kHeader = "timestamp,event_type,order_id,side,price,quantity";
constexpr std::size_t kFieldCount = 6;

// Field positions.
constexpr std::size_t kTimestampField = 0;
constexpr std::size_t kEventTypeField = 1;
constexpr std::size_t kOrderIdField = 2;
constexpr std::size_t kSideField = 3;
constexpr std::size_t kPriceField = 4;
constexpr std::size_t kQuantityField = 5;

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
  const std::size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    return {};
  }
  const std::size_t last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

// Split a line on commas. Returns false if a quoted field appears
// (quoting is not supported by this format).
[[nodiscard]] bool splitFields(std::string_view line,
                               std::string_view* fields,
                               std::size_t maxFields,
                               std::size_t& count) noexcept {
  count = 0;
  std::size_t start = 0;
  while (start <= line.size()) {
    if (count >= maxFields) {
      return false; // too many fields
    }
    const std::size_t comma = line.find(',', start);
    const std::string_view field =
        (comma == std::string_view::npos) ? line.substr(start) : line.substr(start, comma - start);
    if (field.find('"') != std::string_view::npos) {
      return false; // quoting not supported
    }
    fields[count++] = trim(field);
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return true;
}

// Result of integer parsing.
struct IntParseResult {
  std::int64_t value{0};
  bool ok{false};
  bool overflow{false};
};

// Parse a signed 64-bit integer. If allowEmpty, empty string yields 0.
[[nodiscard]] IntParseResult parseInt64(std::string_view text, bool allowEmpty) noexcept {
  IntParseResult result;
  if (text.empty()) {
    result.value = 0;
    result.ok = allowEmpty;
    return result;
  }
  const auto fc = std::from_chars(text.data(), text.data() + text.size(), result.value);
  if (fc.ec == std::errc::result_out_of_range) {
    result.overflow = true;
    return result;
  }
  result.ok = (fc.ec == std::errc{}) && (fc.ptr == text.data() + text.size());
  return result;
}

// Result of unsigned integer parsing.
struct UintParseResult {
  std::uint64_t value{0};
  bool ok{false};
  bool overflow{false};
};

// Parse an unsigned 64-bit integer. Empty is invalid (order_id required).
[[nodiscard]] UintParseResult parseUint64(std::string_view text) noexcept {
  UintParseResult result;
  if (text.empty()) {
    return result;
  }
  const auto fc = std::from_chars(text.data(), text.data() + text.size(), result.value);
  if (fc.ec == std::errc::result_out_of_range) {
    result.overflow = true;
    return result;
  }
  result.ok = (fc.ec == std::errc{}) && (fc.ptr == text.data() + text.size());
  return result;
}

} // namespace

std::string_view toString(ParseError error) noexcept {
  switch (error) {
  case ParseError::Ok:
    return "ok";
  case ParseError::EmptyRecord:
    return "empty record";
  case ParseError::BadHeader:
    return "bad header";
  case ParseError::WrongFieldCount:
    return "wrong field count";
  case ParseError::InvalidTimestamp:
    return "invalid timestamp";
  case ParseError::InvalidEventType:
    return "invalid event type";
  case ParseError::InvalidOrderId:
    return "invalid order id";
  case ParseError::InvalidSide:
    return "invalid side";
  case ParseError::InvalidPrice:
    return "invalid price";
  case ParseError::InvalidQuantity:
    return "invalid quantity";
  case ParseError::NumericOverflow:
    return "numeric overflow";
  case ParseError::EventRejected:
    return "event rejected by validation";
  }
  return "unknown";
}

CsvParser::CsvParser(Instrument instrument) : instrument_(std::move(instrument)) {}

const Instrument& CsvParser::instrument() const noexcept {
  return instrument_;
}

ParseError CsvParser::parseTimestampField(std::string_view field,
                                          Event& event,
                                          std::string& diagnostic) noexcept {
  const IntParseResult parsed = parseInt64(field, false);
  if (!parsed.ok) {
    diagnostic =
        parsed.overflow ? "timestamp out of int64 range" : "timestamp is not a valid integer";
    return parsed.overflow ? ParseError::NumericOverflow : ParseError::InvalidTimestamp;
  }
  event.timestamp = Timestamp{parsed.value};
  return ParseError::Ok;
}

ParseError CsvParser::parseEventTypeField(std::string_view field,
                                          Event& event,
                                          std::string& diagnostic) noexcept {
  if (field == "N") {
    event.type = EventType::NewOrder;
  } else if (field == "M") {
    event.type = EventType::ModifyOrder;
  } else if (field == "C") {
    event.type = EventType::CancelOrder;
  } else {
    diagnostic = "event_type must be N, M, or C";
    return ParseError::InvalidEventType;
  }
  return ParseError::Ok;
}

ParseError CsvParser::parseOrderIdField(std::string_view field,
                                        Event& event,
                                        std::string& diagnostic) noexcept {
  const UintParseResult parsed = parseUint64(field);
  if (!parsed.ok) {
    diagnostic = parsed.overflow ? "order_id out of uint64 range"
                                 : "order_id is not a valid unsigned integer";
    return parsed.overflow ? ParseError::NumericOverflow : ParseError::InvalidOrderId;
  }
  event.orderId = OrderId{parsed.value};
  return ParseError::Ok;
}

ParseError
CsvParser::parseSideField(std::string_view field, Event& event, std::string& diagnostic) noexcept {
  if (field == "B") {
    event.side = Side::Bid;
  } else if (field == "A") {
    event.side = Side::Ask;
  } else {
    diagnostic = "side must be B or A";
    return ParseError::InvalidSide;
  }
  return ParseError::Ok;
}

ParseError
CsvParser::parsePriceField(std::string_view field, Event& event, std::string& diagnostic) noexcept {
  const IntParseResult parsed = parseInt64(field, true);
  if (!parsed.ok) {
    diagnostic = parsed.overflow ? "price out of int64 range" : "price is not a valid integer";
    return parsed.overflow ? ParseError::NumericOverflow : ParseError::InvalidPrice;
  }
  event.price = Price{parsed.value};
  return ParseError::Ok;
}

ParseError CsvParser::parseQuantityField(std::string_view field,
                                         Event& event,
                                         std::string& diagnostic) noexcept {
  const IntParseResult parsed = parseInt64(field, true);
  if (!parsed.ok) {
    diagnostic =
        parsed.overflow ? "quantity out of int64 range" : "quantity is not a valid integer";
    return parsed.overflow ? ParseError::NumericOverflow : ParseError::InvalidQuantity;
  }
  event.quantity = Quantity{parsed.value};
  return ParseError::Ok;
}

ParseResult CsvParser::parseRecord(std::string_view line, Sequence sequence) const {
  ParseResult result;
  result.lineNumber = 0;
  result.event.sequence = sequence;
  result.event.instrument = instrument_;

  const std::string_view text = trim(line);
  if (text.empty()) {
    result.error = ParseError::EmptyRecord;
    result.diagnostic = "empty record";
    return result;
  }

  std::array<std::string_view, kFieldCount> fields{};
  std::size_t count = 0;
  if (!splitFields(text, fields.data(), kFieldCount, count) || count != kFieldCount) {
    result.error = ParseError::WrongFieldCount;
    result.diagnostic = "expected 6 comma-separated fields";
    return result;
  }

  // Parse each field; helpers set the diagnostic on failure.
  ParseError field_error =
      parseTimestampField(fields[kTimestampField], result.event, result.diagnostic);
  if (field_error == ParseError::Ok) {
    field_error = parseEventTypeField(fields[kEventTypeField], result.event, result.diagnostic);
  }
  if (field_error == ParseError::Ok) {
    field_error = parseOrderIdField(fields[kOrderIdField], result.event, result.diagnostic);
  }
  if (field_error == ParseError::Ok) {
    field_error = parseSideField(fields[kSideField], result.event, result.diagnostic);
  }
  if (field_error == ParseError::Ok) {
    field_error = parsePriceField(fields[kPriceField], result.event, result.diagnostic);
  }
  if (field_error == ParseError::Ok) {
    field_error = parseQuantityField(fields[kQuantityField], result.event, result.diagnostic);
  }
  if (field_error != ParseError::Ok) {
    result.error = field_error;
    return result;
  }

  // Domain gate: the existing validator enforces NewOrder/ModifyOrder/
  // CancelOrder field rules. The parser does not duplicate them.
  const EventValidationError validation = validateEvent(result.event);
  if (validation != EventValidationError::Ok) {
    result.error = ParseError::EventRejected;
    result.validationError = validation;
    result.diagnostic = std::string("validateEvent: ") + std::string(toString(validation));
    return result;
  }

  result.error = ParseError::Ok;
  return result;
}

StreamResult CsvParser::parseStream(std::istream& input) const {
  StreamResult result;
  std::string line;
  std::uint64_t line_number = 0;

  if (!std::getline(input, line)) {
    result.error = ParseError::BadHeader;
    result.lineNumber = 1;
    result.diagnostic = "empty input: missing header";
    return result;
  }
  ++line_number;
  if (trim(line) != kHeader) {
    result.error = ParseError::BadHeader;
    result.lineNumber = line_number;
    result.diagnostic = "first line must be: timestamp,event_type,order_id,side,price,quantity";
    return result;
  }

  std::uint64_t seq = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const ParseResult record = parseRecord(line, Sequence{seq});
    if (record.error != ParseError::Ok) {
      result.error = record.error;
      result.lineNumber = line_number;
      result.validationError = record.validationError;
      result.diagnostic = record.diagnostic;
      return result;
    }
    result.events.push_back(record.event);
    ++seq;
  }

  result.error = ParseError::Ok;
  return result;
}

} // namespace tickforge
