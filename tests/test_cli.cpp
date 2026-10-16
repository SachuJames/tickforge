// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Executable process-boundary verification.
//
// Contract audit (SPEC.md, ARCHITECTURE.md, CMakeLists.txt, main.cpp):
// SPEC.md defines no command-line interface: no arguments, flags, file
// input, or exit-code contract exists. The executable target is the
// Day 01 entry point ("proves the build works, nothing more"): it
// prints the version banner and exits 0, ignoring argv entirely.
// These tests pin that actual behavior at the process boundary so a
// future CLI lands against a documented baseline. They do not invent
// a CLI contract the project has not specified.
//
// POSIX only: the project's CI runs on Ubuntu. The harness uses
// fork/exec with pipes (no shell, no quoting hazards).

#include "tickforge/version.hpp"

#include <gtest/gtest.h>
#include <array>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

std::string expectedBanner() {
  return std::string("TickForge v") + std::string(tickforge::kVersion) +
         "\nEngine initialized successfully.\n";
}

} // namespace

TEST(CliProcess, NoArgsPrintsBannerAndExitsZero) {
  const ProcResult result = runExe({});
  EXPECT_FALSE(result.timedOut);
  EXPECT_EQ(result.exitCode, 0);
  EXPECT_EQ(result.out, expectedBanner());
  EXPECT_TRUE(result.err.empty());
}

TEST(CliProcess, ArgumentsAreNotInterpretedYet) {
  // The Day 01 entry point ignores argv: unknown flags, mode-like
  // options, and file paths (existing or not) change nothing. This
  // pins the baseline a future CLI will replace.
  const ProcResult base = runExe({});
  for (const std::vector<std::string>& args :
       {std::vector<std::string>{"--help"},
        std::vector<std::string>{"--mode", "lenient"},
        std::vector<std::string>{"events.csv"},
        std::vector<std::string>{"/nonexistent/definitely/missing.csv"},
        std::vector<std::string>{"-"}}) {
    const ProcResult result = runExe(args);
    EXPECT_EQ(result.exitCode, 0) << "args[0]=" << args[0];
    EXPECT_EQ(result.out, base.out) << "args[0]=" << args[0];
    EXPECT_TRUE(result.err.empty()) << "args[0]=" << args[0];
  }
}

TEST(CliProcess, DoesNotReadInputFiles) {
  // A malformed CSV passed as an argument has no effect: no file is
  // opened by the current executable.
  const auto dir = std::filesystem::temp_directory_path() / "tickforge_cli_test";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const auto file = dir / "malformed.csv";
  {
    std::ofstream out(file);
    out << "this,is,not,a,valid,header\nbroken\n";
  }
  const ProcResult result = runExe({file.string()});
  EXPECT_EQ(result.exitCode, 0);
  EXPECT_EQ(result.out, expectedBanner());
  std::filesystem::remove_all(dir, ec);
}

TEST(CliProcess, DeterministicAcrossRuns) {
  const ProcResult first = runExe({});
  for (int i = 0; i < 3; ++i) {
    const ProcResult again = runExe({});
    EXPECT_EQ(again.exitCode, first.exitCode);
    EXPECT_EQ(again.out, first.out);
    EXPECT_EQ(again.err, first.err);
  }
}

TEST(CliProcess, IndependentOfWorkingDirectory) {
  const ProcResult home = runExe({});
  const ProcResult root = runExe({}, "/");
  EXPECT_EQ(root.exitCode, home.exitCode);
  EXPECT_EQ(root.out, home.out);
  EXPECT_EQ(root.err, home.err);
}
