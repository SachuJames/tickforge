// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Deterministic failing-sequence minimizer (test infrastructure only).
//
// Given a failing event sequence and a predicate that reports whether
// a candidate subsequence still reproduces the failure, removes
// irrelevant events. Tries removing contiguous chunks, largest first,
// then single events. Fully deterministic for a given input and
// predicate; no randomness involved.
//
// The predicate receives renormalized events (dense seqs, strictly
// increasing timestamps) so it can replay them directly. The predicate
// must return false for candidates that do not reproduce the failure,
// including candidates that became invalid after removal.

#pragma once

#include "tickforge/event/event.hpp"

#include <cstddef>
#include <functional>
#include <vector>

namespace tickforge::test {

// Renumbers seqs densely (0, 1, 2, ...) and makes timestamps strictly
// increasing, preserving event order. Used after removing events so
// the candidate remains a well-formed stream.
[[nodiscard]] std::vector<Event> renormalize(const std::vector<Event>& events);

// Returns a (usually much smaller) subsequence that still satisfies
// stillFails, or the renormalized input when nothing can be removed.
[[nodiscard]] std::vector<Event>
minimizeFailingSequence(const std::vector<Event>& events,
                        const std::function<bool(const std::vector<Event>&)>& stillFails);

} // namespace tickforge::test
