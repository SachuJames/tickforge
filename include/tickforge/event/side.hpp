// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Order side (SPEC.md 2.2). A proper enum class: never bool, int, char,
// or string in the core model.

#pragma once

#include <cstdint>
#include <string_view>

namespace tickforge {

enum class Side : std::uint8_t { Bid, Ask };

[[nodiscard]] std::string_view toString(Side side) noexcept;
[[nodiscard]] Side opposite(Side side) noexcept;

} // namespace tickforge
