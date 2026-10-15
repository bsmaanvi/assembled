#pragma once

#include <cstdint>
#include <string>

namespace intraday {

// Only the queue metrics needed by our first two rule types.
struct QueueSnapshot {
    std::string event_id;
    std::int64_t observed_at_sec; // Unix timestamp in UTC, not arrival time.
    std::string queue_id;
    int tickets_waiting;
    int longest_wait_sec;
    int sla_target_sec;
    bool operator==(const QueueSnapshot&) const = default;
};

} // namespace intraday
