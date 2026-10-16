#pragma once

#include <vector>
#include <cstdint>
#include <unordered_map>
#include "queue_catalog.hpp"

#include "queue_rule.hpp"

namespace intraday {

// In-memory configuration logic. No files, terminal input, or notifications.
class RuleCollection {
public:
    RuleCollection() = default;
    explicit RuleCollection(std::vector<QueueRule> rules, QueueCatalog catalog = QueueCatalog{});

    const std::vector<QueueRule>& rules() const;
    int create(RuleDefinition definition);
    void edit(int id, RuleDefinition definition);
    void set_enabled(int id, bool enabled);
    const QueueCatalog& catalog() const { return catalog_; }
    void add_queue(Queue queue) { catalog_.add(std::move(queue)); }
    const QueueRule& at(int id) const;
    bool contains(int id) const { return positions_.contains(id); }
    const std::vector<int>& for_queue(const std::string& id) const;

private:
    void validate_definition(const RuleDefinition& definition) const;
    void reject_equivalent(const RuleDefinition& definition, int except_id = 0) const;
    void rebuild_index();
    QueueCatalog catalog_;
    std::int64_t next_id_ = 1;
    std::unordered_map<int, std::size_t> positions_;
    std::unordered_map<std::string, std::vector<int>> queue_rules_;
    std::vector<QueueRule> rules_;
};

} // namespace intraday
