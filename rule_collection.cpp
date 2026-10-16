#include "rule_collection.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace intraday {
// constructor with rules passed
RuleCollection::RuleCollection(std::vector<QueueRule> rules, QueueCatalog catalog) : catalog_(std::move(catalog)) {
    std::set<int> ids;
    for (const auto& rule : rules) {
        if (rule.id <= 0 || !ids.insert(rule.id).second)
            throw DomainError(ErrorCode::InvalidInput, "Rule IDs must be positive and unique.");
        validate_definition(rule.definition);
    }
    rules_ = std::move(rules);
    rebuild_index();
}
 
// read the stored rules
const std::vector<QueueRule>& RuleCollection::rules() const { return rules_; }


void RuleCollection::validate_definition(const RuleDefinition& definition) const {
    std::visit([this](const auto& value) {
        const auto& id = value.queue_id;
        if (!catalog_.contains(id))
            throw DomainError(ErrorCode::InvalidInput, "Unknown queue ID.");
    }, definition);
    if (const auto* backlog = std::get_if<BacklogRule>(&definition);
        backlog && backlog->tickets_waiting_threshold < 0)
        throw DomainError(ErrorCode::InvalidInput, "Threshold must be nonnegative.");
}
// stretch goal 
// I distinguish restoring trusted application state from importing user input.
// Restoration fails visibly rather than silently dropping rules. Partial imports would 
// require an explicit result showing exactly what was accepted and rejected.

int RuleCollection::create(RuleDefinition definition) {
    validate_definition(definition);
    reject_equivalent(definition);
    if (next_id_ > std::numeric_limits<int>::max())
        throw std::runtime_error("No rule IDs available.");
    const int id = static_cast<int>(next_id_);
    rules_.push_back({id, true, std::move(definition)});
    ++next_id_;
    rebuild_index();
    return id;
}

void RuleCollection::edit(int id, RuleDefinition definition) {
    const auto position = positions_.find(id);
    if (position == positions_.end()) throw DomainError(ErrorCode::NotFound, "No rule with that ID.");
    auto& found = rules_[position->second];
    validate_definition(definition);
    if (found.definition == definition) return;
    reject_equivalent(definition, id);
    found.definition = std::move(definition);
    rebuild_index();
}

void RuleCollection::set_enabled(int id, bool enabled) {
    const auto position = positions_.find(id);
    if (position == positions_.end()) throw DomainError(ErrorCode::NotFound, "No rule with that ID.");
    rules_[position->second].enabled = enabled;
}

// Existing saved duplicates are preserved; new create/edit operations cannot add them.
void RuleCollection::reject_equivalent(const RuleDefinition& definition, int except_id) const {
    for (const auto& rule : rules_)
        if (rule.id != except_id && rule.definition == definition)
            throw DomainError(ErrorCode::Conflict,
                "Equivalent rule already exists (#" + std::to_string(rule.id) + "). Edit or enable it instead.", rule.id);
}

void RuleCollection::rebuild_index() {
    positions_.clear();
    queue_rules_.clear();
    for (std::size_t index = 0; index < rules_.size(); ++index) {
        const auto& rule = rules_[index];
        positions_[rule.id] = index;
        const auto& queue = std::visit([](const auto& definition) -> const std::string& {
            return definition.queue_id;
        }, rule.definition);
        queue_rules_[queue].push_back(rule.id);
        next_id_ = std::max(next_id_, static_cast<std::int64_t>(rule.id) + 1);
    }
}

const QueueRule& RuleCollection::at(int id) const {
    const auto found = positions_.find(id);
    if (found == positions_.end()) throw DomainError(ErrorCode::NotFound, "No rule with that ID.");
    return rules_.at(found->second);
}

const std::vector<int>& RuleCollection::for_queue(const std::string& id) const {
    static const std::vector<int> empty;
    const auto found = queue_rules_.find(id);
    return found == queue_rules_.end() ? empty : found->second;
}

} // namespace intraday
