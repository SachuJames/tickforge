// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: Apache-2.0.
//
// Executable process-boundary verification for the CLI contract in
// SPEC.md section 13. The harness launches the real `tickforge`
// binary with fork/exec and pipes (no shell), captures stdout and
// stderr separately, and reaps the child with a bounded wait.
//
// POSIX only: the project's CI runs on Ubuntu.

#include "tests/support/event_generator.hpp"
#include "tests/support/reference_model.hpp"
#include "tickforge/replay/session_config.hpp"
#include "tickforge/version.hpp"

#include <array>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>

#ifndef TICKFORGE_EXE_PATH
#error "TICKFORGE_EXE_PATH must be defined by CMake (path to the tickforge executable)"
#endif

namespace {

struct ProcResult {
  int exitCode = -1;
  bool timedOut = false;
  std::string out;
  std::string err;
};

std::string readAll(int fd) {
  std::string data;
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t n = ::read(fd, buffer.data(), buffer.size());
    if (n <= 0) {
      break;
    }
    data.append(buffer.data(), static_cast<std::size_t>(n));
  }
  return data;
}

// Runs the executable with `args` (excluding argv[0]). If `cwd` is
// non-empty, the child runs with that working directory.
ProcResult runExe(const std::vector<std::string>& args, const std::string& cwd = "") {
  ProcResult result;
  std::array<int, 2> outPipe{};
  std::array<int, 2> errPipe{};
  if (::pipe(outPipe.data()) != 0 || ::pipe(errPipe.data()) != 0) {
    return result;
  }

  const pid_t pid = ::fork();
  if (pid == 0) {
    // Child: wire pipes to stdout/stderr, then exec.
    ::dup2(outPipe[1], STDOUT_FILENO);
    ::dup2(errPipe[1], STDERR_FILENO);
    ::close(outPipe[0]);
    ::close(outPipe[1]);
    ::close(errPipe[0]);
    ::close(errPipe[1]);
    if (!cwd.empty()) {
      // Best effort: if chdir fails the exec still runs and the
      // parent-side assertions on output will fail visibly.
      const int rc = ::chdir(cwd.c_str());
      (void)rc;
    }
    std::vector<std::string> all{TICKFORGE_EXE_PATH};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(all.size() + 1);
    for (auto& s : all) {
      argv.push_back(s.data());
    }
    argv.push_back(nullptr);
    ::execv(TICKFORGE_EXE_PATH, argv.data());
    ::_exit(127);
  }

  // Parent: capture output, reap with a bounded wait.
  ::close(outPipe[1]);
  ::close(errPipe[1]);
  result.out = readAll(outPipe[0]);
  result.err = readAll(errPipe[0]);
  ::close(outPipe[0]);
  ::close(errPipe[0]);

  int status = 0;
  for (int i = 0; i < 1000; ++i) {
    const pid_t done = ::waitpid(pid, &status, WNOHANG);
    if (done == pid) {
      if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
      }
      return result;
    }
    ::usleep(10000); // 10 ms, up to ~10 s total
  }
  result.timedOut = true;
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &status, 0);
  return result;
}

constexpr const char* kHeader = "timestamp,event_type,order_id,side,price,quantity\n";
constexpr const char* kCrossCsv = "timestamp,event_type,order_id,side,price,quantity\n"
                                  "1000,N,1,A,101,5\n"
                                  "2000,N,2,B,100,3\n"
                                  "3000,N,3,B,101,5\n";

// Per-test temporary directory for input fixtures.
class CliFixture : public ::testing::Test {
protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() / (std::string("tf_cli_") + info->name());
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  // Writes `contents` to a file in the fixture directory, returns its path.
  std::string writeFile(const std::string& name, const std::string& contents) {
    const auto path = dir_ / name;
    std::ofstream out(path);
    out << contents;
    return path.string();
  }

  // Runs the CLI against `path` with --instrument AAPL plus extras.
  ProcResult runOn(const std::string& path, const std::vector<std::string>& extra = {}) {
    std::vector<std::string> args{"--input", path, "--instrument", "AAPL"};
    args.insert(args.end(), extra.begin(), extra.end());
    return runExe(args);
  }

  std::filesystem::path dir_;
};

} // namespace

// ---------------------------------------------------------------------
// Help and version
// ---------------------------------------------------------------------

TEST(CliHelp, PrintsUsageAndExitsZero) {
  const ProcResult result = runExe({"--help"});
  EXPECT_EQ(result.exitCode, 0);
  EXPECT_TRUE(result.err.empty());
  EXPECT_NE(result.out.find("Usage:"), std::string::npos);
  EXPECT_NE(result.out.find("--input"), std::string::npos);
  EXPECT_NE(result.out.find("--instrument"), std::string::npos);
}

TEST(CliHelp, UnknownOptionStillFails) {
  // Options are validated before help is honored (SPEC.md 13.1).
  const ProcResult result = runExe({"--bogus", "--help"});
  EXPECT_EQ(result.exitCode, 2);
  EXPECT_TRUE(result.out.empty());
}

TEST(CliVersion, PrintsVersionAndExitsZero) {
  const ProcResult result = runExe({"--version"});
  EXPECT_EQ(result.exitCode, 0);
  EXPECT_EQ(result.out, std::string("TickForge v") + std::string(tickforge::kVersion) + "\n");
  EXPECT_TRUE(result.err.empty());
}

// ---------------------------------------------------------------------
// Argument validation (all exit 2, diagnostics on stderr only)
// ---------------------------------------------------------------------

TEST(CliArguments, NoArguments) {
  const ProcResult result = runExe({});
  EXPECT_EQ(result.exitCode, 2);
  EXPECT_TRUE(result.out.empty());
  EXPECT_NE(result.err.find("required"), std::string::npos);
}

TEST(CliArguments, MissingInputValue) {
  EXPECT_EQ(runExe({"--input"}).exitCode, 2);
  // A following option is not consumed as the value.
  EXPECT_EQ(runExe({"--input", "--instrument", "AAPL"}).exitCode, 2);
}

TEST(CliArguments, UnknownOption) {
  const ProcResult result = runExe({"--bogus"});
  EXPECT_EQ(result.exitCode, 2);
  EXPECT_NE(result.err.find("unknown argument"), std::string::npos);
}

TEST(CliArguments, PositionalArgumentRejected) {
  const ProcResult result = runExe({"events.csv", "--instrument", "AAPL"});
  EXPECT_EQ(result.exitCode, 2);
}

TEST(CliArguments, InvalidMode) {
  const ProcResult result = runExe({"--mode", "turbo"});
  EXPECT_EQ(result.exitCode, 2);
  EXPECT_NE(result.err.find("invalid mode"), std::string::npos);
}

TEST(CliArguments, DuplicateInput) {
  const ProcResult result = runExe({"--input", "a.csv", "--input", "b.csv"});
  EXPECT_EQ(result.exitCode, 2);
}

TEST(CliArguments, EmptyInputValue) {
  const ProcResult result = runExe({"--input", "", "--instrument", "AAPL"});
  EXPECT_EQ(result.exitCode, 2);
}

TEST(CliArguments, MissingInstrumentCheckedBeforeFiles) {
  // A nonexistent input plus a missing instrument is an argument
  // error (2), not a file error (3): required options come first.
  const ProcResult result = runExe({"--input", "/nonexistent/definitely/missing.csv"});
  EXPECT_EQ(result.exitCode, 2);
}

// ---------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------

TEST_F(CliFixture, MissingFileIsInputError) {
  const ProcResult result = runOn((dir_ / "missing.csv").string());
  EXPECT_EQ(result.exitCode, 3);
  EXPECT_NE(result.err.find("cannot open input file"), std::string::npos);
  EXPECT_TRUE(result.out.empty());
}

TEST_F(CliFixture, DirectoryIsReadFailureNotParseError) {
  // A directory opens as a stream on Linux but reads fail: the bad
  // bit check classifies it as exit 3, not a header parse error.
  const ProcResult result = runOn(dir_.string());
  EXPECT_EQ(result.exitCode, 3);
  EXPECT_NE(result.err.find("failed while reading input file"), std::string::npos);
  EXPECT_TRUE(result.out.empty());
}

TEST_F(CliFixture, UnreadableFileIsInputError) {
  if (::geteuid() == 0) {
    GTEST_SKIP() << "running as root: permission bits are bypassed";
  }
  const std::string path = writeFile("locked.csv", kCrossCsv);
  ::chmod(path.c_str(), 0000);
  const ProcResult result = runOn(path);
  ::chmod(path.c_str(), 0600);
  EXPECT_EQ(result.exitCode, 3);
  EXPECT_NE(result.err.find("cannot open input file"), std::string::npos);
}

TEST_F(CliFixture, EmptyFileIsParseError) {
  const ProcResult result = runOn(writeFile("empty.csv", ""));
  EXPECT_EQ(result.exitCode, 4);
  EXPECT_NE(result.err.find("bad header"), std::string::npos);
  EXPECT_NE(result.err.find("line 1"), std::string::npos);
  EXPECT_TRUE(result.out.empty());
}

TEST_F(CliFixture, MalformedHeaderIsParseError) {
  const ProcResult result = runOn(writeFile("badheader.csv", "a,b,c\n1000,N,1,B,100,1\n"));
  EXPECT_EQ(result.exitCode, 4);
  EXPECT_NE(result.err.find("bad header"), std::string::npos);
}

TEST_F(CliFixture, MalformedRecordReportsLineAndNoSummary) {
  const std::string csv = std::string(kHeader) + "1000,N,1,B,100,1\nbroken\n";
  const ProcResult result = runOn(writeFile("bad.csv", csv));
  EXPECT_EQ(result.exitCode, 4);
  EXPECT_NE(result.err.find("line 3"), std::string::npos);
  EXPECT_NE(result.err.find("wrong field count"), std::string::npos);
  EXPECT_TRUE(result.out.empty());
}

TEST_F(CliFixture, NumericOverflowRecordIsParseError) {
  const std::string csv = std::string(kHeader) + "1000,N,1,B,99999999999999999999,1\n";
  const ProcResult result = runOn(writeFile("overflow.csv", csv));
  EXPECT_EQ(result.exitCode, 4);
  EXPECT_NE(result.err.find("numeric overflow"), std::string::npos);
}

TEST_F(CliFixture, IncompleteFinalRecordIsParseError) {
  const std::string csv = std::string(kHeader) + "1000,N,1,B,100\n";
  const ProcResult result = runOn(writeFile("short.csv", csv));
  EXPECT_EQ(result.exitCode, 4);
}

TEST_F(CliFixture, InvalidInstrumentIsConfigError) {
  // '=' is rejected by SessionConfig validation before any file work.
  const std::string path = writeFile("ok.csv", kCrossCsv);
  const ProcResult result = runExe({"--input", path, "--instrument", "AAP=L"});
  EXPECT_EQ(result.exitCode, 5);
  EXPECT_NE(result.err.find("invalid session configuration"), std::string::npos);
  EXPECT_TRUE(result.out.empty());
}

TEST_F(CliFixture, PathWithSpacesWorks) {
  const ProcResult result = runOn(writeFile("my events.csv", kCrossCsv));
  EXPECT_EQ(result.exitCode, 0);
  EXPECT_NE(result.out.find("Events processed: 3"), std::string::npos);
}

// ---------------------------------------------------------------------
// Replay correctness through the executable
// ---------------------------------------------------------------------

TEST_F(CliFixture, ValidFileProducesExactSummary) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  const ProcResult result = runOn(path);
  ASSERT_EQ(result.exitCode, 0);
  EXPECT_TRUE(result.err.empty());
  const std::string expected = std::string("TickForge v") + std::string(tickforge::kVersion) +
                               "\nInput: " + path +
                               "\nInstrument: AAPL\n"
                               "Replay mode: strict\n"
                               "Events processed: 3\n"
                               "Events skipped: 0\n"
                               "Fills: 1\n"
                               "Resting orders: 1\n"
                               "Best bid: 100\n"
                               "Best ask: none\n"
                               "Config hash: 5dddb109ead805bc\n"
                               "Replay status: success\n";
  EXPECT_EQ(result.out, expected);
}

TEST_F(CliFixture, HeaderOnlyFileSucceedsWithZeroEvents) {
  const ProcResult result = runOn(writeFile("header.csv", kHeader));
  ASSERT_EQ(result.exitCode, 0);
  EXPECT_NE(result.out.find("Events processed: 0"), std::string::npos);
  EXPECT_NE(result.out.find("Fills: 0"), std::string::npos);
  EXPECT_NE(result.out.find("Best bid: none"), std::string::npos);
  EXPECT_NE(result.out.find("Best ask: none"), std::string::npos);
}

TEST_F(CliFixture, NoFinalNewlineStillSucceeds) {
  std::string csv = kCrossCsv;
  csv.pop_back(); // drop the trailing newline
  const ProcResult result = runOn(writeFile("noeol.csv", csv));
  EXPECT_EQ(result.exitCode, 0);
  EXPECT_NE(result.out.find("Events processed: 3"), std::string::npos);
}

TEST_F(CliFixture, StrictRejectsUnknownOrder) {
  const std::string csv = std::string(kHeader) + "1000,C,9,B,,\n";
  const ProcResult result = runOn(writeFile("unknown.csv", csv));
  EXPECT_EQ(result.exitCode, 5);
  EXPECT_NE(result.err.find("UnknownOrder"), std::string::npos);
  EXPECT_TRUE(result.out.empty());
}

TEST_F(CliFixture, LenientSkipsUnknownOrder) {
  const std::string csv = std::string(kHeader) + "1000,C,9,B,,\n";
  const ProcResult result = runOn(writeFile("unknown.csv", csv), {"--mode", "lenient"});
  ASSERT_EQ(result.exitCode, 0);
  EXPECT_NE(result.out.find("Replay mode: lenient"), std::string::npos);
  EXPECT_NE(result.out.find("Events processed: 0"), std::string::npos);
  EXPECT_NE(result.out.find("Events skipped: 1"), std::string::npos);
}

TEST_F(CliFixture, LenientDoesNotRelaxCsvParsing) {
  const std::string csv = std::string(kHeader) + "broken\n";
  const ProcResult result = runOn(writeFile("bad.csv", csv), {"--mode", "lenient"});
  EXPECT_EQ(result.exitCode, 4);
  EXPECT_TRUE(result.out.empty());
}

TEST_F(CliFixture, RepeatedRunsAreByteIdentical) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  const ProcResult first = runOn(path);
  for (int i = 0; i < 3; ++i) {
    const ProcResult again = runOn(path);
    EXPECT_EQ(again.exitCode, first.exitCode);
    EXPECT_EQ(again.out, first.out);
    EXPECT_EQ(again.err, first.err);
  }
}

TEST_F(CliFixture, RelativeAndAbsolutePaths) {
  const std::string absolute = writeFile("cross.csv", kCrossCsv);
  // Relative: run with the fixture dir as cwd and a bare file name.
  const ProcResult rel = runExe({"--input", "cross.csv", "--instrument", "AAPL"}, dir_.string());
  ASSERT_EQ(rel.exitCode, 0);
  EXPECT_NE(rel.out.find("Input: cross.csv"), std::string::npos);
  EXPECT_NE(rel.out.find("Events processed: 3"), std::string::npos);
  // Absolute from another directory: identical except the Input line.
  const ProcResult abs = runExe({"--input", absolute, "--instrument", "AAPL"}, "/");
  ASSERT_EQ(abs.exitCode, 0);
  EXPECT_NE(abs.out.find("Events processed: 3"), std::string::npos);
}

// ---------------------------------------------------------------------
// Session-scale end-to-end (Day 19)
// ---------------------------------------------------------------------

namespace {

// Serializes events in the parser's CSV format (zero price/quantity on
// modify/cancel records become empty fields).
std::string serializeEvents(const std::vector<tickforge::Event>& events) {
  std::ostringstream out;
  out << "timestamp,event_type,order_id,side,price,quantity\n";
  for (const auto& e : events) {
    out << e.timestamp.count() << ','
        << (e.type == tickforge::EventType::NewOrder      ? 'N'
            : e.type == tickforge::EventType::ModifyOrder ? 'M'
                                                          : 'C')
        << ',' << e.orderId.value() << ',' << (e.side == tickforge::Side::Bid ? 'B' : 'A') << ',';
    if (e.price.ticks() != 0) {
      out << e.price.ticks();
    }
    out << ',';
    if (e.quantity.lots() != 0) {
      out << e.quantity.lots();
    }
    out << '\n';
  }
  return out.str();
}

tickforge::test::GeneratorConfig cliSessionConfig() {
  tickforge::test::GeneratorConfig config;
  config.seed = 1901;
  config.eventCount = 6000;
  config.minPrice = 80;
  config.maxPrice = 120;
  config.minQuantity = 1;
  config.maxQuantity = 50;
  config.newWeight = 55;
  config.cancelWeight = 20;
  config.modifyWeight = 25;
  config.instrument = "AAPL";
  return config;
}

// Parses "Key: value" summary lines into a map.
std::map<std::string, std::string> summaryFields(const std::string& out) {
  std::map<std::string, std::string> fields;
  std::istringstream lines(out);
  std::string line;
  while (std::getline(lines, line)) {
    const auto colon = line.find(": ");
    if (colon != std::string::npos) {
      fields[line.substr(0, colon)] = line.substr(colon + 2);
    }
  }
  return fields;
}

} // namespace

TEST_F(CliFixture, FillCountAccumulatesAcrossEvents) {
  // Regression: fills produced by different events must all be
  // counted. MatchingEngine::fills() is per-event; a CLI that reads
  // it after the replay sees only the last event's fills (here the
  // last event rests, so the wrong answer would be 0).
  const std::string csv = std::string(kHeader) + "1000,N,1,A,101,5\n"
                                                 "2000,N,2,B,101,5\n"
                                                 "3000,N,3,A,102,4\n"
                                                 "4000,N,4,B,102,4\n"
                                                 "5000,N,5,B,99,1\n";
  const ProcResult result = runOn(writeFile("two_cross.csv", csv));
  ASSERT_EQ(result.exitCode, 0);
  EXPECT_NE(result.out.find("Fills: 2"), std::string::npos);
  EXPECT_NE(result.out.find("Resting orders: 1"), std::string::npos);
}

TEST_F(CliFixture, GeneratedSessionMatchesReferenceSummary) {
  const auto events = tickforge::test::EventGenerator(cliSessionConfig()).generate();
  // Independent expectations from the reference model.
  tickforge::test::ReferenceBook ref;
  std::size_t expectedFills = 0;
  for (const auto& e : events) {
    auto applied = ref.apply(e);
    ASSERT_TRUE(applied.applied);
    expectedFills += applied.fills.size();
  }

  const std::string path = writeFile("session.csv", serializeEvents(events));
  const ProcResult result = runOn(path);
  ASSERT_EQ(result.exitCode, 0);
  EXPECT_TRUE(result.err.empty());
  const auto fields = summaryFields(result.out);
  EXPECT_EQ(fields.at("Events processed"), "6000");
  EXPECT_EQ(fields.at("Events skipped"), "0");
  EXPECT_EQ(fields.at("Fills"), std::to_string(expectedFills));
  EXPECT_EQ(fields.at("Resting orders"), std::to_string(ref.orderCount()));
  const auto bestBid = ref.bestBid();
  const auto bestAsk = ref.bestAsk();
  EXPECT_EQ(fields.at("Best bid"), bestBid.has_value() ? std::to_string(bestBid->ticks()) : "none");
  EXPECT_EQ(fields.at("Best ask"), bestAsk.has_value() ? std::to_string(bestAsk->ticks()) : "none");
  EXPECT_EQ(fields.at("Config hash"), "5dddb109ead805bc");
  EXPECT_EQ(fields.at("Replay status"), "success");

  // Byte-identical on a repeated run with the same path.
  const ProcResult again = runOn(path);
  EXPECT_EQ(again.exitCode, 0);
  EXPECT_EQ(again.out, result.out);
}

TEST_F(CliFixture, GeneratedSessionWithTrailingErrorEvent) {
  const auto events = tickforge::test::EventGenerator(cliSessionConfig()).generate();
  std::string csv = serializeEvents(events);
  // Append a cancel of a never-existing order id (replay-level error,
  // well-formed CSV). Timestamp stays strictly increasing.
  const auto lastTs = events.back().timestamp.count();
  csv += std::to_string(lastTs + 1000) + ",C,999999,B,,\n";
  const std::string path = writeFile("session_err.csv", csv);

  const ProcResult strict = runOn(path);
  EXPECT_EQ(strict.exitCode, 5);
  EXPECT_TRUE(strict.out.empty());

  const ProcResult lenient = runOn(path, {"--mode", "lenient"});
  ASSERT_EQ(lenient.exitCode, 0);
  const auto fields = summaryFields(lenient.out);
  EXPECT_EQ(fields.at("Events processed"), "6000");
  EXPECT_EQ(fields.at("Events skipped"), "1");
}

// ---------------------------------------------------------------------
// CLI session-configuration options (Day 20)
// ---------------------------------------------------------------------

namespace {

// The expected CLI config hash, computed by the library itself from
// the equivalent SessionConfig (serializeConfig/hashConfig are the
// Day 15 contract; the CLI must report the replay result's hash).
std::string libraryHash(const tickforge::SessionConfig& config) {
  return std::string(tickforge::formatConfigHash(tickforge::hashConfig(config)));
}

tickforge::SessionConfig baseConfig() {
  tickforge::SessionConfig config;
  config.instrument = "AAPL";
  return config;
}

} // namespace

TEST_F(CliFixture, TickSizeFlowsIntoConfigHash) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  const ProcResult result = runOn(path, {"--tick-size", "0.01"});
  ASSERT_EQ(result.exitCode, 0);
  auto expected = baseConfig();
  expected.tickSize = "0.01";
  EXPECT_EQ(summaryFields(result.out).at("Config hash"), libraryHash(expected));
  // And it differs from the default configuration's hash.
  EXPECT_NE(libraryHash(expected), libraryHash(baseConfig()));
}

TEST_F(CliFixture, LotSizeAndSeedFlowIntoConfigHash) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  const ProcResult result = runOn(path, {"--lot-size", "10", "--seed", "42"});
  ASSERT_EQ(result.exitCode, 0);
  auto expected = baseConfig();
  expected.lotSize = "10";
  expected.prngSeed = 42;
  EXPECT_EQ(summaryFields(result.out).at("Config hash"), libraryHash(expected));
}

TEST_F(CliFixture, SeedBoundaryValuesAccepted) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  for (const char* seedText : {"0", "18446744073709551615"}) {
    const ProcResult result = runOn(path, {"--seed", seedText});
    ASSERT_EQ(result.exitCode, 0) << "seed " << seedText;
    auto expected = baseConfig();
    expected.prngSeed = std::stoull(std::string(seedText));
    EXPECT_EQ(summaryFields(result.out).at("Config hash"), libraryHash(expected));
  }
}

TEST_F(CliFixture, OptionOrderDoesNotChangeResult) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  const ProcResult a = runExe({"--input",
                               path,
                               "--instrument",
                               "AAPL",
                               "--seed",
                               "7",
                               "--tick-size",
                               "0.5",
                               "--mode",
                               "lenient"});
  const ProcResult b = runExe({"--mode",
                               "lenient",
                               "--tick-size",
                               "0.5",
                               "--seed",
                               "7",
                               "--instrument",
                               "AAPL",
                               "--input",
                               path});
  ASSERT_EQ(a.exitCode, 0);
  ASSERT_EQ(b.exitCode, 0);
  EXPECT_EQ(a.out, b.out);
}

TEST_F(CliFixture, InvalidTickSizeIsConfigError) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  for (const char* bad : {"1.2.3", "-0.5", "abc", "1."}) {
    const ProcResult result = runOn(path, {"--tick-size", bad});
    EXPECT_EQ(result.exitCode, 5) << "tick-size " << bad;
    EXPECT_NE(result.err.find("invalid session configuration"), std::string::npos);
    EXPECT_TRUE(result.out.empty());
  }
}

TEST_F(CliFixture, SeedParseErrorsAreArgumentErrors) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  for (const char* bad : {"18446744073709551616", "-1", "12x", "1.5", "+7", " 7"}) {
    const ProcResult result = runOn(path, {"--seed", bad});
    EXPECT_EQ(result.exitCode, 2) << "seed '" << bad << "'";
    EXPECT_TRUE(result.out.empty());
  }
}

TEST_F(CliFixture, DuplicateConfigOptionsRejected) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  EXPECT_EQ(runOn(path, {"--seed", "1", "--seed", "2"}).exitCode, 2);
  EXPECT_EQ(runOn(path, {"--tick-size", "0.01", "--tick-size", "0.02"}).exitCode, 2);
  EXPECT_EQ(runOn(path, {"--lot-size", "1", "--lot-size", "2"}).exitCode, 2);
}

TEST_F(CliFixture, DefaultsUnchangedByNewOptions) {
  // A command with no new options keeps the Day 18/19 default hash.
  const std::string path = writeFile("cross.csv", kCrossCsv);
  const ProcResult result = runOn(path);
  ASSERT_EQ(result.exitCode, 0);
  EXPECT_EQ(summaryFields(result.out).at("Config hash"), libraryHash(baseConfig()));
  EXPECT_EQ(libraryHash(baseConfig()), "5dddb109ead805bc");
}

// ---------------------------------------------------------------------
// Committed quick-start example (Day 21)
// ---------------------------------------------------------------------

#ifndef TICKFORGE_EXAMPLE_CSV
#error "TICKFORGE_EXAMPLE_CSV must be defined by CMake (path to examples/sample_session.csv)"
#endif

TEST(CliExample, CommittedSampleSessionMatchesDocumentedOutput) {
  // The README's worked example, pinned: any change to the fixture,
  // the summary format, or the default configuration breaks this
  // test before it breaks a new user's first run.
  const ProcResult result = runExe({"--input", TICKFORGE_EXAMPLE_CSV, "--instrument", "AAPL"});
  ASSERT_EQ(result.exitCode, 0);
  EXPECT_TRUE(result.err.empty());
  const std::string expected = std::string("TickForge v") + std::string(tickforge::kVersion) +
                               "\nInput: " + std::string(TICKFORGE_EXAMPLE_CSV) +
                               "\nInstrument: AAPL\n"
                               "Replay mode: strict\n"
                               "Events processed: 5\n"
                               "Events skipped: 0\n"
                               "Fills: 1\n"
                               "Resting orders: 1\n"
                               "Best bid: none\n"
                               "Best ask: 105\n"
                               "Config hash: 5dddb109ead805bc\n"
                               "Replay status: success\n";
  EXPECT_EQ(result.out, expected);
}

// ---------------------------------------------------------------------
// Latency model CLI integration (SPEC.md 13.4, 15)
// ---------------------------------------------------------------------

TEST(CliLatency, EnabledSampleSessionExactOutput) {
  // The section 13.4 example, pinned end to end: the enabled hash
  // 5212b75c2e1ebc7f was reproduced independently from the canonical
  // serialization, and 1413 is the documented seed-42 first sample.
  const ProcResult result = runExe({"--input",
                                    TICKFORGE_EXAMPLE_CSV,
                                    "--instrument",
                                    "AAPL",
                                    "--seed",
                                    "42",
                                    "--latency-min-ns",
                                    "1000",
                                    "--latency-max-ns",
                                    "1999"});
  ASSERT_EQ(result.exitCode, 0);
  EXPECT_TRUE(result.err.empty());
  const std::string expected = std::string("TickForge v") + std::string(tickforge::kVersion) +
                               "\nInput: " + std::string(TICKFORGE_EXAMPLE_CSV) +
                               "\nInstrument: AAPL\n"
                               "Replay mode: strict\n"
                               "Events processed: 5\n"
                               "Events skipped: 0\n"
                               "Fills: 1\n"
                               "Resting orders: 1\n"
                               "Best bid: none\n"
                               "Best ask: 105\n"
                               "Config hash: 5212b75c2e1ebc7f\n"
                               "Replay status: success\n"
                               "Latency model: uniform-latency v1\n"
                               "Latency seed: 42\n"
                               "Latency min (ns): 1000\n"
                               "Latency max (ns): 1999\n"
                               "Latency samples: 1\n"
                               "Latency sample 0 (ns): 1413\n";
  EXPECT_EQ(result.out, expected);
}

TEST(CliLatency, DisabledByDefaultHasNoLatencySection) {
  const ProcResult result = runExe({"--input", TICKFORGE_EXAMPLE_CSV, "--instrument", "AAPL"});
  ASSERT_EQ(result.exitCode, 0);
  EXPECT_EQ(result.out.find("Latency"), std::string::npos);
}

TEST(CliLatency, HelpListsLatencyOptions) {
  const ProcResult result = runExe({"--help"});
  ASSERT_EQ(result.exitCode, 0);
  EXPECT_NE(result.out.find("--latency-min-ns"), std::string::npos);
  EXPECT_NE(result.out.find("--latency-max-ns"), std::string::npos);
}

TEST_F(CliFixture, LatencyMissingOneBoundIsConfigError) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  const ProcResult minOnly = runOn(path, {"--seed", "42", "--latency-min-ns", "1000"});
  EXPECT_EQ(minOnly.exitCode, 5);
  EXPECT_TRUE(minOnly.out.empty());
  const ProcResult maxOnly = runOn(path, {"--seed", "42", "--latency-max-ns", "1999"});
  EXPECT_EQ(maxOnly.exitCode, 5);
  EXPECT_TRUE(maxOnly.out.empty());
}

TEST_F(CliFixture, LatencyBoundsWithoutSeedIsConfigError) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  const ProcResult result = runOn(path, {"--latency-min-ns", "1000", "--latency-max-ns", "1999"});
  EXPECT_EQ(result.exitCode, 5);
  EXPECT_TRUE(result.out.empty());
}

TEST_F(CliFixture, LatencyNegativeAndReversedBoundsAreConfigErrors) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  const ProcResult negative =
      runOn(path, {"--seed", "42", "--latency-min-ns", "-5", "--latency-max-ns", "1999"});
  EXPECT_EQ(negative.exitCode, 5);
  const ProcResult reversed =
      runOn(path, {"--seed", "42", "--latency-min-ns", "2000", "--latency-max-ns", "1000"});
  EXPECT_EQ(reversed.exitCode, 5);
}

TEST_F(CliFixture, LatencyMalformedBoundsAreUsageErrors) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  for (const char* bad : {"abc", "10x", "+5", "99999999999999999999", "1.5"}) {
    const ProcResult result =
        runOn(path, {"--seed", "42", "--latency-min-ns", bad, "--latency-max-ns", "1999"});
    EXPECT_EQ(result.exitCode, 2) << "value: " << bad;
    EXPECT_TRUE(result.out.empty());
  }
}

TEST_F(CliFixture, LatencyMissingOrDuplicateValueIsUsageError) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  const ProcResult missing = runOn(path, {"--seed", "42", "--latency-min-ns"});
  EXPECT_EQ(missing.exitCode, 2);
  const ProcResult duplicate = runOn(path,
                                     {"--seed",
                                      "42",
                                      "--latency-min-ns",
                                      "1000",
                                      "--latency-min-ns",
                                      "1000",
                                      "--latency-max-ns",
                                      "1999"});
  EXPECT_EQ(duplicate.exitCode, 2);
  const ProcResult empty =
      runOn(path, {"--seed", "42", "--latency-min-ns", "", "--latency-max-ns", "1999"});
  EXPECT_EQ(empty.exitCode, 2);
}

TEST_F(CliFixture, LatencyInvalidSeedIsUsageError) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  const ProcResult result =
      runOn(path, {"--seed", "abc", "--latency-min-ns", "1000", "--latency-max-ns", "1999"});
  EXPECT_EQ(result.exitCode, 2);
}

TEST_F(CliFixture, LatencyEqualAndExtremeBoundsAccepted) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  const ProcResult equal =
      runOn(path, {"--seed", "42", "--latency-min-ns", "5000", "--latency-max-ns", "5000"});
  ASSERT_EQ(equal.exitCode, 0);
  EXPECT_NE(equal.out.find("Latency samples: 1\n"), std::string::npos);
  EXPECT_NE(equal.out.find("Latency sample 0 (ns): 5000\n"), std::string::npos);

  const ProcResult extreme = runOn(
      path, {"--seed", "42", "--latency-min-ns", "0", "--latency-max-ns", "9223372036854775807"});
  ASSERT_EQ(extreme.exitCode, 0);
  EXPECT_NE(extreme.out.find("Latency max (ns): 9223372036854775807\n"), std::string::npos);
  EXPECT_NE(extreme.out.find("Latency samples: 1\n"), std::string::npos);
}

TEST_F(CliFixture, LatencyMultiFillSamplesMatchKnownAnswers) {
  // Three crossing bids against one resting ask: three fills, and
  // the first three seed-42 samples at [1000, 1999] are the
  // documented 1413, 1291, 1858 in fill order.
  const std::string csv = std::string(kHeader) + "1000,N,1,A,105,10\n"
                                                 "2000,N,2,B,105,2\n"
                                                 "3000,N,3,B,105,3\n"
                                                 "4000,N,4,B,105,1\n";
  const ProcResult result =
      runOn(writeFile("fills.csv", csv),
            {"--seed", "42", "--latency-min-ns", "1000", "--latency-max-ns", "1999"});
  ASSERT_EQ(result.exitCode, 0);
  EXPECT_NE(result.out.find("Fills: 3\n"), std::string::npos);
  EXPECT_NE(result.out.find("Latency samples: 3\n"), std::string::npos);
  EXPECT_NE(result.out.find("Latency sample 0 (ns): 1413\n"), std::string::npos);
  EXPECT_NE(result.out.find("Latency sample 1 (ns): 1291\n"), std::string::npos);
  EXPECT_NE(result.out.find("Latency sample 2 (ns): 1858\n"), std::string::npos);
}

TEST_F(CliFixture, LatencyEmptyFillSessionReportsZeroSamples) {
  const std::string csv = std::string(kHeader) + "1000,N,1,A,105,10\n"
                                                 "2000,N,2,B,100,2\n";
  const ProcResult result =
      runOn(writeFile("nofills.csv", csv),
            {"--seed", "42", "--latency-min-ns", "1000", "--latency-max-ns", "1999"});
  ASSERT_EQ(result.exitCode, 0);
  EXPECT_NE(result.out.find("Fills: 0\n"), std::string::npos);
  EXPECT_NE(result.out.find("Latency samples: 0\n"), std::string::npos);
  EXPECT_EQ(result.out.find("Latency sample 0"), std::string::npos);
}

TEST_F(CliFixture, LatencyEnabledRunIsDeterministic) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  const std::vector<std::string> flags{
      "--seed", "42", "--latency-min-ns", "1000", "--latency-max-ns", "1999"};
  const ProcResult first = runOn(path, flags);
  const ProcResult second = runOn(path, flags);
  ASSERT_EQ(first.exitCode, 0);
  EXPECT_EQ(first.out, second.out);
}

TEST_F(CliFixture, LatencyDoesNotChangeMatchingSummary) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  const ProcResult disabled = runOn(path);
  const ProcResult enabled =
      runOn(path, {"--seed", "42", "--latency-min-ns", "1000", "--latency-max-ns", "1999"});
  ASSERT_EQ(disabled.exitCode, 0);
  ASSERT_EQ(enabled.exitCode, 0);
  // Everything except the config hash line and the latency section
  // must be identical.
  auto matchingLines = [](const std::string& out) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : out) {
      if (c == '\n') {
        if (!current.starts_with("Latency") && !current.starts_with("Config hash:")) {
          lines.push_back(current);
        }
        current.clear();
      } else {
        current += c;
      }
    }
    return lines;
  };
  EXPECT_EQ(matchingLines(disabled.out), matchingLines(enabled.out));
  EXPECT_NE(disabled.out.find("Config hash: 5dddb109ead805bc"), std::string::npos);
  EXPECT_EQ(enabled.out.find("Config hash: 5dddb109ead805bc"), std::string::npos);
}

TEST_F(CliFixture, LatencyFailuresPrintNoSummary) {
  const ProcResult missing = runExe({"--input",
                                     "/nonexistent/tf.csv",
                                     "--instrument",
                                     "AAPL",
                                     "--seed",
                                     "42",
                                     "--latency-min-ns",
                                     "1000",
                                     "--latency-max-ns",
                                     "1999"});
  EXPECT_EQ(missing.exitCode, 3);
  EXPECT_TRUE(missing.out.empty());

  const std::string broken = std::string(kHeader) + "1000,N,1,B,100,1\nbroken\n";
  const ProcResult parseFail =
      runOn(writeFile("broken.csv", broken),
            {"--seed", "42", "--latency-min-ns", "1000", "--latency-max-ns", "1999"});
  EXPECT_EQ(parseFail.exitCode, 4);
  EXPECT_TRUE(parseFail.out.empty());

  // A fill happens, then a strict-mode replay error: no summary and
  // no partial latency section may be printed.
  const std::string fillThenError = std::string(kHeader) + "1000,N,1,A,105,10\n"
                                                           "2000,N,2,B,105,2\n"
                                                           "3000,C,99,B,,\n";
  const ProcResult replayFail =
      runOn(writeFile("fillerr.csv", fillThenError),
            {"--seed", "42", "--latency-min-ns", "1000", "--latency-max-ns", "1999"});
  EXPECT_EQ(replayFail.exitCode, 5);
  EXPECT_TRUE(replayFail.out.empty());
}

TEST_F(CliFixture, LatencyCombinesWithOtherOptions) {
  const std::string path = writeFile("cross.csv", kCrossCsv);
  const ProcResult result = runOn(path,
                                  {"--tick-size",
                                   "0.01",
                                   "--lot-size",
                                   "1",
                                   "--mode",
                                   "lenient",
                                   "--seed",
                                   "42",
                                   "--latency-min-ns",
                                   "1000",
                                   "--latency-max-ns",
                                   "1999"});
  ASSERT_EQ(result.exitCode, 0);
  EXPECT_NE(result.out.find("Replay mode: lenient\n"), std::string::npos);
  EXPECT_NE(result.out.find("Latency samples: 1\n"), std::string::npos);
}
