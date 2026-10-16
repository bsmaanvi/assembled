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
    // Missing means not reported, rather than zero. Older saved snapshots remain valid.
    std::optional<int> agents_available = std::nullopt;
    std::optional<int> agents_on_call = std::nullopt;
    std::optional<int> volume_last_15m = std::nullopt;
    std::optional<int> volume_forecast_next_15m = std::nullopt;
    bool operator==(const QueueSnapshot&) const = default;
};

} // namespace intraday
