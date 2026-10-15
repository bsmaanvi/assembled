#pragma once

#include <string>
#include <variant>

namespace intraday {

struct BacklogRule {
    std::string queue_id;
    int tickets_waiting_threshold;
    bool operator==(const BacklogRule&) const = default;
};

struct SlaBreachRule {
    std::string queue_id;
    // The SLA target comes from queue snapshots, not this configuration.
    bool operator==(const SlaBreachRule&) const = default;
};

using RuleDefinition = std::variant<BacklogRule, SlaBreachRule>;

struct QueueRule {
    int id;
    bool enabled = true;
    RuleDefinition definition;
    bool operator==(const QueueRule&) const = default;
};

} // namespace intraday
