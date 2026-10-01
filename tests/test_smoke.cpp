// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Day 01 smoke test: proves the test binary builds, links against the
// TickForge core headers, runs under CTest, and sees the project version.

#include <string>

#include <gtest/gtest.h>

#include "tickforge/version.hpp"

namespace {

TEST(SmokeTest, EngineInitializes) {
  // Day 01: the engine has no subsystems yet. Reaching this point proves the
  // test executable links and runs against the TickForge core headers.
  SUCCEED();
}

TEST(VersionTest, VersionStringIsWellFormed) {
  const std::string version{tickforge::kVersion};
  EXPECT_FALSE(version.empty());
  EXPECT_EQ(version, "0.1.0");
}

TEST(VersionTest, VersionComponentsMatchString) {
  EXPECT_EQ(tickforge::kVersionMajor, 0);
  EXPECT_EQ(tickforge::kVersionMinor, 1);
  EXPECT_EQ(tickforge::kVersionPatch, 0);
}

TEST(VersionTest, ProjectNameIsTickForge) {
  EXPECT_EQ(tickforge::kProjectName, "TickForge");
}

}  // namespace
