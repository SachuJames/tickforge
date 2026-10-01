// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Day 01 entry point. Proves the project configures, compiles, links, and
// runs. Simulation subsystems are specified in SPEC.md and arrive later.

#include "tickforge/version.hpp"

#include <iostream>

int main() {
  std::cout << "TickForge v" << tickforge::kVersion << "\n";
  std::cout << "Engine initialized successfully.\n";
  return 0;
}
