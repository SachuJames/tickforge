// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// TickForge command-line interface (SPEC.md section 13).
//
// Replays one CSV event file through the library pipeline: parse the
// stream with CsvParser, verify the stream read completely, then run
// the configured replay over a MatchingEngine and print a summary.
// main() only translates arguments and exceptions into the documented
// exit codes; all semantics live in the library.

#include "tickforge/book/order_book.hpp"
#include "tickforge/market_data/csv_parser.hpp"
#include "tickforge/matching/matching_engine.hpp"
#include "tickforge/replay/event_processor.hpp"
#include "tickforge/replay/replay.hpp"
#include "tickforge/replay/session_config.hpp"
#include "tickforge/version.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Exit codes (SPEC.md 13.2).
constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitInput = 3;
constexpr int kExitParse = 4;
constexpr int kExitConfigOrReplay = 5;
constexpr int kExitInternal = 6;

struct CliOptions {
  std::optional<std::string> input;
  std::optional<std::string> instrument;
  tickforge::ReplayMode mode = tickforge::ReplayMode::Strict;
  bool help = false;
  bool version = false;
};

// Accumulates every fill a replay produces. MatchingEngine::fills()
// exposes only the most recent event's fills, so the total the CLI
// reports must be collected per event.
class FillCollector final : public tickforge::EventProcessor {
public:
  explicit FillCollector(tickforge::MatchingEngine& engine) : engine_(engine) {}

  bool onEvent(const tickforge::Event& event) override {
    const bool applied = engine_.onEvent(event);
    const auto& produced = engine_.fills();
    fills_.insert(fills_.end(), produced.begin(), produced.end());
    return applied;
  }

  [[nodiscard]] const std::vector<tickforge::Fill>& fills() const noexcept {
    return fills_;
  }

private:
  tickforge::MatchingEngine& engine_;
  std::vector<tickforge::Fill> fills_;
};

void printUsage(std::ostream& out) {
  out << "Usage: tickforge --input PATH --instrument SYMBOL [--mode strict|lenient]\n"
         "       tickforge --help\n"
         "       tickforge --version\n"
         "\n"
         "Replay a CSV event file deterministically.\n"
         "\n"
         "Options:\n"
         "  --input PATH        CSV event file to replay (required)\n"
         "  --instrument SYMBOL Session instrument, e.g. AAPL (required)\n"
         "  --mode MODE         Replay mode: strict (default) or lenient\n"
         "  --help              Print this usage information\n"
         "  --version           Print the TickForge version\n";
}

// Parses argv into options. On error, prints the diagnostic and usage
// to stderr and returns false.
bool parseArguments(const std::vector<std::string_view>& args, CliOptions& options) {
  bool modeSeen = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    auto fail = [&arg](const std::string& message) {
      std::cerr << "error: " << message << ": '" << arg << "'\n";
      printUsage(std::cerr);
      return false;
    };
    auto takeValue = [&](std::string& value) {
      if (i + 1 >= args.size() || args[i + 1].starts_with("--")) {
        return fail("missing value for option");
      }
      value = std::string(args[++i]);
      if (value.empty()) {
        return fail("empty value for option");
      }
      return true;
    };
    if (arg == "--help") {
      if (options.help) {
        return fail("duplicate option");
      }
      options.help = true;
    } else if (arg == "--version") {
      if (options.version) {
        return fail("duplicate option");
      }
      options.version = true;
    } else if (arg == "--input") {
      if (options.input.has_value()) {
        return fail("duplicate option");
      }
      std::string value;
      if (!takeValue(value)) {
        return false;
      }
      options.input = value;
    } else if (arg == "--instrument") {
      if (options.instrument.has_value()) {
        return fail("duplicate option");
      }
      std::string value;
      if (!takeValue(value)) {
        return false;
      }
      options.instrument = value;
    } else if (arg == "--mode") {
      if (modeSeen) {
        return fail("duplicate option");
      }
      modeSeen = true;
      std::string value;
      if (!takeValue(value)) {
        return false;
      }
      if (value == "strict") {
        options.mode = tickforge::ReplayMode::Strict;
      } else if (value == "lenient") {
        options.mode = tickforge::ReplayMode::Lenient;
      } else {
        return fail("invalid mode (expected strict or lenient)");
      }
    } else {
      return fail("unknown argument");
    }
  }
  return true;
}

// Runs one replay from validated options; returns the exit code.
int runReplay(const CliOptions& options) {
  tickforge::SessionConfig config;
  config.instrument = *options.instrument;
  config.mode = options.mode;
  if (const auto cfgError = tickforge::validateConfig(config);
      cfgError != tickforge::ConfigError::Ok) {
    std::cerr << "error: invalid session configuration: " << tickforge::toString(cfgError) << "\n";
    return kExitConfigOrReplay;
  }

  std::ifstream input{*options.input};
  if (!input.is_open()) {
    std::cerr << "error: cannot open input file: " << *options.input << "\n";
    return kExitInput;
  }

  const tickforge::CsvParser parser{*options.instrument};
  const tickforge::StreamResult parsed = parser.parseStream(input);
  // StreamResult cannot distinguish clean EOF from a read failure
  // (ARCHITECTURE.md 19.2): an Ok prefix on a failed stream is not a
  // complete file, so the bad bit decides before the parse result.
  if (input.bad()) {
    std::cerr << "error: failed while reading input file: " << *options.input << "\n";
    return kExitInput;
  }
  if (parsed.error != tickforge::ParseError::Ok) {
    std::cerr << "error: CSV parse failed at line " << parsed.lineNumber << ": "
              << tickforge::toString(parsed.error);
    if (parsed.validationError != tickforge::EventValidationError::Ok) {
      std::cerr << " (" << tickforge::toString(parsed.validationError) << ")";
    }
    if (!parsed.diagnostic.empty()) {
      std::cerr << ": " << parsed.diagnostic;
    }
    std::cerr << "\n";
    return kExitParse;
  }

  tickforge::OrderBook book;
  tickforge::MatchingEngine engine{book};
  FillCollector collector{engine};
  const tickforge::ReplayResult result = tickforge::replayEvents(parsed.events, collector, config);
  if (!result.ok()) {
    std::cerr << "error: replay failed: " << tickforge::toString(result.error) << " at sequence "
              << result.failedAt.value();
    if (result.validationReason != tickforge::EventValidationError::Ok) {
      std::cerr << " (" << tickforge::toString(result.validationReason) << ")";
    }
    std::cerr << "\n";
    return kExitConfigOrReplay;
  }

  const std::uint64_t processed = parsed.events.size() - result.skippedCount;
  std::cout << "TickForge v" << tickforge::kVersion << "\n";
  std::cout << "Input: " << *options.input << "\n";
  std::cout << "Instrument: " << *options.instrument << "\n";
  std::cout << "Replay mode: "
            << (options.mode == tickforge::ReplayMode::Strict ? "strict" : "lenient") << "\n";
  std::cout << "Events processed: " << processed << "\n";
  std::cout << "Events skipped: " << result.skippedCount << "\n";
  std::cout << "Fills: " << collector.fills().size() << "\n";
  std::cout << "Resting orders: " << book.orderCount() << "\n";
  std::cout << "Best bid: ";
  if (const auto bestBid = book.bestBid(); bestBid.has_value()) {
    std::cout << bestBid->ticks() << "\n";
  } else {
    std::cout << "none\n";
  }
  std::cout << "Best ask: ";
  if (const auto bestAsk = book.bestAsk(); bestAsk.has_value()) {
    std::cout << bestAsk->ticks() << "\n";
  } else {
    std::cout << "none\n";
  }
  std::cout << "Config hash: " << tickforge::formatConfigHash(*result.configHash) << "\n";
  std::cout << "Replay status: success\n";
  return kExitOk;
}

} // namespace

int main(int argc, char* argv[]) {
  try {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    CliOptions options;
    if (!parseArguments(args, options)) {
      return kExitUsage;
    }
    if (options.help) {
      printUsage(std::cout);
      return kExitOk;
    }
    if (options.version) {
      std::cout << "TickForge v" << tickforge::kVersion << "\n";
      return kExitOk;
    }
    if (!options.input.has_value() || !options.instrument.has_value()) {
      std::cerr << "error: --input and --instrument are required for replay\n";
      printUsage(std::cerr);
      return kExitUsage;
    }
    return runReplay(options);
  } catch (const std::exception& e) {
    std::cerr << "error: internal failure: " << e.what() << "\n";
    return kExitInternal;
  } catch (...) {
    std::cerr << "error: internal failure\n";
    return kExitInternal;
  }
}
