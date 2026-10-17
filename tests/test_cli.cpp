// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Executable process-boundary verification for the CLI contract in
// SPEC.md section 13. The harness launches the real `tickforge`
// binary with fork/exec and pipes (no shell), captures stdout and
// stderr separately, and reaps the child with a bounded wait.
//
// POSIX only: the project's CI runs on Ubuntu.

#include "tickforge/version.hpp"

#include <array>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
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
