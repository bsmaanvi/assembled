#include "rule_collection.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace intraday {
namespace {
auto find_rule(std::vector<QueueRule>& rules, int id) {
    const auto found = std::find_if(rules.begin(), rules.end(),
        [id](const QueueRule& rule) { return rule.id == id; });
    if (found == rules.end()) throw std::invalid_argument("No rule with that ID.");
    return found;
}
} // namespace

// constructor with rules passed
RuleCollection::RuleCollection(std::vector<QueueRule> rules) {
    std::set<int> ids;
    for (const auto& rule : rules) {
        if (rule.id <= 0 || !ids.insert(rule.id).second)
            throw std::invalid_argument("Rule IDs must be positive and unique.");
        validate_definition(rule.definition);
    }
    rules_ = std::move(rules);
}
 
// read the stored rules
const std::vector<QueueRule>& RuleCollection::rules() const { return rules_; }


void RuleCollection::validate_definition(const RuleDefinition& definition) {
    std::visit([](const auto& value) {
        const auto& id = value.queue_id;
        if (id != "billing" && id != "tier_2" && id != "vip")
            throw std::invalid_argument("Unknown queue ID.");
    }, definition);
    if (const auto* backlog = std::get_if<BacklogRule>(&definition);
        backlog && backlog->tickets_waiting_threshold < 0)
        throw std::invalid_argument("Threshold must be nonnegative.");
}
// stretch goal 
// I distinguish restoring trusted application state from importing user input.
// Restoration fails visibly rather than silently dropping rules. Partial imports would 
// require an explicit result showing exactly what was accepted and rejected.

int RuleCollection::create(RuleDefinition definition) {
    validate_definition(definition);
    int maximum = 0;
    for (const auto& rule : rules_) maximum = std::max(maximum, rule.id);
    if (maximum == std::numeric_limits<int>::max())
        throw std::runtime_error("No rule IDs available.");
    const int id = maximum + 1;
    rules_.push_back({id, true, std::move(definition)});
    return id;
}

void RuleCollection::edit(int id, RuleDefinition definition) {
    const auto found = find_rule(rules_, id);
    validate_definition(definition);
    found->definition = std::move(definition);
}

void RuleCollection::set_enabled(int id, bool enabled) {
    find_rule(rules_, id)->enabled = enabled;
}

} // namespace intraday
