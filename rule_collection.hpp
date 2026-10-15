#pragma once

#include <vector>

#include "queue_rule.hpp"

namespace intraday {

// In-memory configuration logic. No files, terminal input, or notifications.
class RuleCollection {
public:
    RuleCollection() = default;
    explicit RuleCollection(std::vector<QueueRule> rules);

    const std::vector<QueueRule>& rules() const;
    int create(RuleDefinition definition);
    void edit(int id, RuleDefinition definition);
    void set_enabled(int id, bool enabled);

private:
    static void validate_definition(const RuleDefinition& definition);
    std::vector<QueueRule> rules_;
};

} // namespace intraday
