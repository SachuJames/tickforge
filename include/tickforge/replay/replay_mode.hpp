// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Replay error mode (SPEC.md section 11). Strict is the default; lenient
// must be explicitly selected by the caller. Lives in its own header so
// both the replay driver and the session configuration can name it
// without a circular include.

#pragma once

#include <cstdint>
#include <string_view>

namespace tickforge {

enum class ReplayMode : std::uint8_t { Strict, Lenient };

[[nodiscard]] std::string_view toString(ReplayMode mode) noexcept;

} // namespace tickforge
