#pragma once

#include <string>

namespace intraday {

struct Queue {
    std::string id;    // Matches queue_id in incoming events.
    std::string name;  // Human-readable label for configuration.
};

} // namespace intraday
