// Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
//
// Minimizer implementation. See minimizer.hpp for the contract.

#include "tests/support/minimizer.hpp"

#include <algorithm>

namespace tickforge::test {

std::vector<Event> renormalize(const std::vector<Event>& events) {
  std::vector<Event> out = events;
  std::int64_t timestamp = 1'000'000;
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i].sequence = Sequence{i};
    out[i].timestamp = Timestamp{timestamp};
    timestamp += 100;
  }
  return out;
}

std::vector<Event>
minimizeFailingSequence(const std::vector<Event>& events,
                        const std::function<bool(const std::vector<Event>&)>& stillFails) {
  std::vector<Event> current = renormalize(events);
  if (current.empty() || !stillFails(current)) {
    return current;
  }
  for (std::size_t chunk = current.size() / 2; chunk >= 1;) {
    bool progress = true;
    while (progress) {
      progress = false;
      for (std::size_t start = 0; start < current.size();) {
        const std::size_t end = std::min(start + chunk, current.size());
        std::vector<Event> candidate;
        candidate.reserve(current.size() - (end - start));
        candidate.insert(
            candidate.end(), current.begin(), current.begin() + static_cast<std::ptrdiff_t>(start));
        candidate.insert(
            candidate.end(), current.begin() + static_cast<std::ptrdiff_t>(end), current.end());
        candidate = renormalize(candidate);
        if (!candidate.empty() && stillFails(candidate)) {
          current = std::move(candidate);
          progress = true;
          // Retry at the same start; the vector shrank.
        } else {
          start = end;
        }
      }
    }
    if (chunk == 1) {
      break;
    }
    chunk /= 2;
  }
  return current;
}

} // namespace tickforge::test
