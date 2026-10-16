#pragma once

#include <cstdint>
#include <string>
#include <optional>

namespace intraday {

// Queue observations include rule inputs and operational context for the lead.
struct QueueSnapshot {
    std::string event_id;
    std::int64_t observed_at_sec; // Unix timestamp in UTC, not arrival time.
    std::string queue_id;
    int tickets_waiting;
    int longest_wait_sec;
    int sla_target_sec;
    // missing means not reported, not zero, we don't need this info for the rules but we might still use it for context for the team lead
    std::optional<int> agents_available = std::nullopt;
    std::optional<int> agents_on_call = std::nullopt;
    std::optional<int> volume_last_15m = std::nullopt;
    std::optional<int> volume_forecast_next_15m = std::nullopt;
    bool operator==(const QueueSnapshot&) const = default;
};

} // namespace intraday
