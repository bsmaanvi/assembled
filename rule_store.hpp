#pragma once

#include <filesystem>
#include <vector>

#include "rule_collection.hpp"

namespace intraday {

// Saves each change successfully before updating the in-memory rules.
class RuleStore {
public:
    explicit RuleStore(std::filesystem::path path);
    const std::vector<QueueRule>& rules() const;
    int create(RuleDefinition definition);
    void edit(int id, RuleDefinition definition);
    void set_enabled(int id, bool enabled);

private:
    void load();
    void commit(RuleCollection candidate);
    std::filesystem::path path_;
    RuleCollection collection_;
};

} // namespace intraday
