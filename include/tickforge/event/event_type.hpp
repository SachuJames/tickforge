// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Normalized event categories (SPEC.md 2.3). Exactly these three types:
// Trade events do not appear in the order flow.

#pragma once

#include <cstdint>
#include <string_view>

namespace tickforge {

enum class EventType : std::uint8_t { NewOrder, ModifyOrder, CancelOrder };

[[nodiscard]] std::string_view toString(EventType type) noexcept;

}  // namespace tickforge
