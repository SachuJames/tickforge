// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Day 01 entry point. Proves the project configures, compiles, links, and
// runs. Simulation subsystems are specified in SPEC.md and arrive later.

#include <iostream>

#include "tickforge/version.hpp"

int main() {
  std::cout << "TickForge v" << tickforge::kVersion << "\n";
  std::cout << "Engine initialized successfully.\n";
  return 0;
}
