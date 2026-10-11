// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// CSV generator implementation. See csv_generator.hpp for the contract.

#include "tests/support/csv_generator.hpp"

#include <sstream>

namespace tickforge::test {

CsvGenerator::CsvGenerator(const CsvGeneratorConfig& config)
    : config_(config), rng_(config.seed) {}

std::string CsvGenerator::randomNumericField(bool allowEmpty, bool signedValue) {
  if (allowEmpty && rng_.nextBounded(10) == 0) {
    return "";
  }
  // Bounded values to stay within valid ranges for the "valid" generator.
  // Malformed cases use the explicit table instead.
  std::int64_t value = static_cast<std::int64_t>(rng_.nextRange(1, 10000));
  if (signedValue && rng_.nextBool()) {
    value = -value;
  }
  return std::to_string(value);
}

std::string CsvGenerator::randomEventType() {
  const int roll = static_cast<int>(rng_.nextBounded(3));
  return roll == 0 ? "N" : (roll == 1 ? "M" : "C");
}

std::string CsvGenerator::randomSide() {
  return rng_.nextBool() ? "B" : "A";
}

std::string CsvGenerator::generateValidRecord() {
  std::ostringstream out;
  const std::string type = randomEventType();
  out << timestamp_ << "," << type << "," << nextOrderId_ << "," << randomSide() << ",";

  if (type == "N") {
    // NewOrder: price > 0, quantity > 0.
    out << rng_.nextRange(1, 1000) << "," << rng_.nextRange(1, 100);
  } else if (type == "M") {
    // ModifyOrder: must change at least one of price/quantity.
    // 0 means unchanged; ensure at least one is non-zero.
    const bool changePrice = rng_.nextBool();
    if (changePrice) {
      out << rng_.nextRange(1, 1000) << ",";
    } else {
      out << "0,";
    }
    if (!changePrice || rng_.nextBool()) {
      out << rng_.nextRange(1, 100);
    } else {
      out << "0";
    }
  } else {
    // CancelOrder: price and quantity must be 0/empty.
    out << "0,0";
  }

  timestamp_ += static_cast<std::int64_t>(rng_.nextRange(1, 1000));
  ++nextOrderId_;
  return out.str();
}

std::string CsvGenerator::malformRecord(const std::string& validRecord) {
  // Split into fields.
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= validRecord.size()) {
    const std::size_t comma = validRecord.find(',', start);
    if (comma == std::string::npos) {
      fields.push_back(validRecord.substr(start));
      break;
    }
    fields.push_back(validRecord.substr(start, comma - start));
    start = comma + 1;
  }

  const int mutation = static_cast<int>(rng_.nextBounded(12));
  switch (mutation) {
  case 0: // Drop a field
    if (fields.size() > 1) {
      fields.erase(fields.begin() + static_cast<std::ptrdiff_t>(rng_.nextBounded(fields.size())));
    }
    break;
  case 1: // Add an extra field
    fields.push_back("extra");
    break;
  case 2: // Empty the timestamp
    fields[0] = "";
    break;
  case 3: // Invalid event type
    fields[1] = rng_.nextBool() ? "X" : "n";
    break;
  case 4: // Invalid side
    fields[3] = rng_.nextBool() ? "X" : "b";
    break;
  case 5: // Non-numeric price
    fields[4] = "abc";
    break;
  case 6: // Numeric overflow in quantity
    fields[5] = "99999999999999999999999";
    break;
  case 7: // Negative order_id
    fields[2] = "-1";
    break;
  case 8: // Decimal in timestamp
    fields[0] += ".5";
    break;
  case 9: // Whitespace-only field
    fields[4] = "   ";
    break;
  case 10: // Quote character
    fields[1] = "\"N\"";
    break;
  default: // Truncate the record
    if (!fields.empty()) {
      fields.pop_back();
    }
    break;
  }

  std::ostringstream out;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i > 0) {
      out << ",";
    }
    out << fields[i];
  }
  return out.str();
}

std::string CsvGenerator::generate() {
  std::ostringstream out;
  out << "timestamp,event_type,order_id,side,price,quantity\n";
  for (std::size_t i = 0; i < config_.recordCount; ++i) {
    std::string record = generateValidRecord();
    const int roll = static_cast<int>(rng_.nextBounded(100));
    if (roll < config_.malformedPercent) {
      record = malformRecord(record);
    }
    out << record << "\n";
  }
  return out.str();
}

std::vector<MalformedCase> generateMalformedTable() {
  const std::string header = "timestamp,event_type,order_id,side,price,quantity\n";
  const std::string valid = "1000000,N,1,B,100,10";
  return {
      {header + "1000000,N,1,B,100", "missing quantity field"},
      {header + "1000000,N,1,B,100,10,extra", "extra field"},
      {header + ",N,1,B,100,10", "empty timestamp"},
      {header + "1000000,X,1,B,100,10", "invalid event type X"},
      {header + "1000000,n,1,B,100,10", "lowercase event type"},
      {header + "1000000,N,1,X,100,10", "invalid side X"},
      {header + "1000000,N,1,b,100,10", "lowercase side"},
      {header + "1000000,N,1,B,abc,10", "non-numeric price"},
      {header + "1000000,N,1,B,100,99999999999999999999999", "quantity overflow"},
      {header + "1000000,N,-1,B,100,10", "negative order_id"},
      {header + "1000000.5,N,1,B,100,10", "decimal timestamp"},
      {header + "1000000,N,1,B,   ,10", "whitespace price"},
      {header + "1000000,\"N\",1,B,100,10", "quoted event type"},
      {header + "1000000,N,1,B,100", "truncated record"},
      {"timestamp,event_type,order_id,side,price\n1000000,N,1,B,100,10",
       "missing quantity in header"},
      {"TIMESTAMP,event_type,order_id,side,price,quantity\n1000000,N,1,B,100,10",
       "uppercase header"},
      {"", "empty input"},
      {header + "\n", "header with blank line"},
      {header + "1000000,N,1,B,100,10\n\n1000001,N,2,A,101,5",
       "blank line between records"},
  };
}

} // namespace tickforge::test
