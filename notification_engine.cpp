#include "notification_engine.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "rule_collection.hpp"

namespace intraday {
namespace {
const std::string& queue_of(const QueueRule& rule) {
    return std::visit([](const auto& definition) -> const std::string& {
        return definition.queue_id;
    }, rule.definition);
}
void validate_snapshot(const QueueSnapshot& snapshot) {
    if (snapshot.event_id.empty() || snapshot.event_id.size() > 256)
        throw std::invalid_argument("Event ID must contain 1 to 256 characters.");
    if (snapshot.queue_id != "billing" && snapshot.queue_id != "tier_2" && snapshot.queue_id != "vip")
        throw std::invalid_argument("Unknown queue ID.");
    if (snapshot.observed_at_sec < 0 || snapshot.observed_at_sec > 253402300799LL)
        throw std::invalid_argument("Observation time must be Unix seconds between 1970 and 9999.");
    if (snapshot.tickets_waiting < 0 || snapshot.longest_wait_sec < 0)
        throw std::invalid_argument("Queue metrics must be nonnegative.");
    for (const auto& metric : {snapshot.agents_available, snapshot.agents_on_call,
                              snapshot.volume_last_15m, snapshot.volume_forecast_next_15m}) {
        if (metric && *metric < 0)
            throw std::invalid_argument("Staffing and volume metrics must be nonnegative when reported.");
    }
    if (snapshot.sla_target_sec <= 0)
        throw std::invalid_argument("SLA target must be positive.");
}
} // namespace

void NotificationEngine::close_episode(int id, const std::string& reason) {
    const auto found = rule_states_.find(id);
    if (found == rule_states_.end()) return;
    if (found->second.firing && found->second.notification_id > 0)
        notifications_.at(found->second.notification_id - 1).close_reason = reason;
    found->second.firing = false;
    found->second.notification_id = 0;
}

std::vector<Notification> NotificationEngine::evaluate(
        const QueueRule& rule, const QueueSnapshot& snapshot, const std::string& trigger) {
    const auto existing = rule_states_.find(rule.id);
    if (existing != rule_states_.end()) {
        // A new entity or rule type is a different problem. Threshold-only edits
        // retain an existing episode when the latest value still qualifies.
        if (queue_of(existing->second.configuration) != queue_of(rule) ||
            existing->second.configuration.definition.index() != rule.definition.index()) {
            close_episode(rule.id, "rule_changed");
        }
        existing->second.configuration = rule;
    }
    auto [position, inserted] = rule_states_.try_emplace(rule.id, RuleState{rule, false, 0});
    (void)inserted;
    auto& state = position->second;
    if (!rule.enabled) {
        close_episode(rule.id, "rule_disabled");
        return {};
    }
    const auto* backlog = std::get_if<BacklogRule>(&rule.definition);
    const int observed = backlog ? snapshot.tickets_waiting : snapshot.longest_wait_sec;
    const int threshold = backlog ? backlog->tickets_waiting_threshold : snapshot.sla_target_sec;
    if (observed <= threshold) {
        close_episode(rule.id, trigger == "rule_change" ? "rule_changed" : "recovered");
        return {};
    }
    if (state.firing) return {};
    const std::string explanation = backlog
        ? "Tickets waiting " + std::to_string(observed) + " exceed threshold " + std::to_string(threshold)
        : "Oldest wait " + std::to_string(observed) + "s exceeds SLA target " + std::to_string(threshold) + "s";
    Notification notification{notifications_.size() + 1, rule.id, snapshot.event_id,
        snapshot.queue_id, snapshot.observed_at_sec, observed, threshold, explanation, "", trigger, backlog ? "backlog" : "sla"};
    notification.context = snapshot;
    notifications_.push_back(notification);
    state.firing = true;
    state.notification_id = notification.id;
    return {notification};
}

SnapshotResult NotificationEngine::process(const QueueSnapshot& snapshot,
                                          const std::vector<QueueRule>& rules) {
    validate_snapshot(snapshot);
    const RuleCollection validated(rules);
    if (const auto found = events_.find(snapshot.event_id); found != events_.end()) {
        if (found->second != snapshot)
            throw std::invalid_argument("Event ID was already used with different data.");
        return {SnapshotOutcome::Duplicate, {}};
    }
    events_.emplace(snapshot.event_id, snapshot);
    if (const auto found = latest_observations_.find(snapshot.queue_id);
        found != latest_observations_.end() && snapshot.observed_at_sec < found->second)
        return {SnapshotOutcome::Stale, {}};
    latest_observations_[snapshot.queue_id] = snapshot.observed_at_sec;
    latest_snapshots_[snapshot.queue_id] = snapshot;

    std::vector<int> removed;
    for (const auto& [id, state] : rule_states_) {
        (void)state;
        if (std::none_of(rules.begin(), rules.end(), [id](const auto& rule) { return rule.id == id; }))
            removed.push_back(id);
    }
    for (int id : removed) {
        close_episode(id, "rule_removed");
        rule_states_.erase(id);
    }
    SnapshotResult result{SnapshotOutcome::Processed, {}};
    for (const auto& rule : validated.rules()) {
        if (!rule.enabled) {
            close_episode(rule.id, "rule_disabled");
            // Keep configuration current even for disabled rules.
            rule_states_.insert_or_assign(rule.id, RuleState{rule, false, 0});
        } else if (queue_of(rule) == snapshot.queue_id) {
            auto created = evaluate(rule, snapshot, "event");
            result.new_notifications.insert(result.new_notifications.end(), created.begin(), created.end());
        }
    }
    return result;
}

void NotificationEngine::reevaluate(int id, const std::vector<QueueRule>& rules) {
    const auto found = std::find_if(rules.begin(), rules.end(), [id](const auto& rule) { return rule.id == id; });
    if (found == rules.end()) throw std::invalid_argument("No rule with that ID.");
    if (!found->enabled) {
        close_episode(id, "rule_disabled");
        rule_states_.insert_or_assign(id, RuleState{*found, false, 0});
        return;
    }
    const auto snapshot = latest_snapshots_.find(queue_of(*found));
    if (snapshot == latest_snapshots_.end()) {
        close_episode(id, "rule_changed");
        rule_states_.insert_or_assign(id, RuleState{*found, false, 0});
    } else {
        evaluate(*found, snapshot->second, "rule_change");
    }
}

const std::vector<Notification>& NotificationEngine::notifications() const { return notifications_; }
const std::unordered_map<std::string, QueueSnapshot>& NotificationEngine::latest_snapshots() const {
    return latest_snapshots_;
}
std::vector<int> NotificationEngine::active_rule_ids() const {
    std::vector<int> ids;
    for (const auto& [id, state] : rule_states_) if (state.firing) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    return ids;
}
void NotificationEngine::forget_rule(int id) {
    close_episode(id, "rule_changed");
    rule_states_.erase(id);
}
} // namespace intraday
