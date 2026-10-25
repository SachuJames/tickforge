// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Deterministic test RNG (test infrastructure only, never production).
//
// This is the production SplitMix64 (SPEC.md 14.1) under its test
// name: one implementation, shared by both. The generation contract
// of SPEC.md 14 (generation version 1) is defined over this exact
// algorithm and mapping, and the known-answer tests pin it.

#pragma once

#include "tickforge/core/splitmix64.hpp"

namespace tickforge::test {

using DeterministicRng = tickforge::SplitMix64;

} // namespace tickforge::test
