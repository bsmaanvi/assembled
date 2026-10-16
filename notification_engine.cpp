#include "notification_engine.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <limits>
#include <unordered_set>

#include "rule_collection.hpp"

namespace intraday {
namespace {
const std::string& queue_of(const QueueRule& rule) {
    return std::visit([](const auto& definition) -> const std::string& {
        return definition.queue_id;
    }, rule.definition);
}

// ensuring the values are valid - non-negative or within range
void validate_snapshot(const QueueSnapshot& snapshot, const QueueCatalog& catalog) {
    if (snapshot.event_id.empty() || snapshot.event_id.size() > 256)
        throw DomainError(ErrorCode::InvalidInput, "Event ID must contain 1 to 256 characters.");
    if (!catalog.contains(snapshot.queue_id))
        throw DomainError(ErrorCode::InvalidInput, "Unknown queue ID.");
    if (snapshot.observed_at_sec < 0 || snapshot.observed_at_sec > 253402300799LL)
        throw DomainError(ErrorCode::InvalidInput, "Observation time must be Unix seconds between 1970 and 9999.");
    if (snapshot.tickets_waiting < 0 || snapshot.longest_wait_sec < 0)
        throw DomainError(ErrorCode::InvalidInput, "Queue metrics must be nonnegative.");
    for (const auto& metric : {snapshot.agents_available, snapshot.agents_on_call,
                              snapshot.volume_last_15m, snapshot.volume_forecast_next_15m}) {
        if (metric && *metric < 0)
            throw DomainError(ErrorCode::InvalidInput, "Staffing and volume metrics must be nonnegative when reported.");
    }
    if (snapshot.sla_target_sec <= 0)
        throw DomainError(ErrorCode::InvalidInput, "SLA target must be positive.");
}
} // namespace

// ends a rule’s current violation episode
void NotificationEngine::close_episode(int id, CloseReason reason) {
    const auto found = rule_states_.find(id);
    if (found == rule_states_.end()) return;
    if (found->second.firing && found->second.notification_id > 0)
        notifications_.at(notification_positions_.at(found->second.notification_id)).close_reason = reason;
    found->second.firing = false;
    found->second.notification_id = 0;
}

// evaluate one rule against the snapshot we received and open a new violation and pop a notification if required
std::vector<Notification> NotificationEngine::evaluate(
        const QueueRule& rule, const QueueSnapshot& snapshot, Trigger trigger) {
    const auto existing = rule_states_.find(rule.id);
    if (existing != rule_states_.end()) {
        // A new entity or rule type is a different problem. Threshold-only edits
        // retain an existing episode when the latest value still qualifies.
        // we check rule_edits
        if (queue_of(existing->second.configuration) != queue_of(rule) ||
            existing->second.configuration.definition.index() != rule.definition.index()) {
            close_episode(rule.id, CloseReason::RuleChanged);
        }
        existing->second.configuration = rule;
    }
    //adds the rule if we need to
    auto [position, inserted] = rule_states_.try_emplace(rule.id, RuleState{rule, false, 0});
    (void)inserted;
    auto& state = position->second;
    if (!rule.enabled) {
        close_episode(rule.id, CloseReason::RuleDisabled);
        return {};
    }
    // checks for the violation
    const auto* backlog = std::get_if<BacklogRule>(&rule.definition);
    const int observed = backlog ? snapshot.tickets_waiting : snapshot.longest_wait_sec;
    const int threshold = backlog ? backlog->tickets_waiting_threshold : snapshot.sla_target_sec;
    if (observed <= threshold) {
        close_episode(rule.id, trigger == Trigger::RuleChange ? CloseReason::RuleChanged : CloseReason::Recovered);
        return {};
    }
    //already fired
    if (state.firing) return {};
    if (next_notification_id_ >= static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("Notification ID space exhausted.");
    // building explanation and constructing the notification
    const std::string explanation = backlog
        ? "Tickets waiting " + std::to_string(observed) + " exceed threshold " + std::to_string(threshold)
        : "Oldest wait " + std::to_string(observed) + "s exceeds SLA target " + std::to_string(threshold) + "s";
    Notification notification{next_notification_id_, rule.id, snapshot.event_id,
        snapshot.queue_id, snapshot.observed_at_sec, observed, threshold, explanation, CloseReason::Active, trigger, backlog ? RuleKind::Backlog : RuleKind::Sla};
    notification.context = snapshot;
    notifications_.push_back(notification);
    notification_positions_[notification.id] = notifications_.size() - 1;
    ++next_notification_id_;
    state.firing = true;
    state.notification_id = notification.id;
    return {notification};
}

// process validates the snapshot and then evalautes it against the rules we have
SnapshotResult NotificationEngine::process_validated(const QueueSnapshot& snapshot,
                                          const RuleCollection& rules) {
    validate_snapshot(snapshot, rules.catalog());
    // Configurations were validated on create/edit/restore, not on every event.
    // detect duplicate or conflicting events
    if (const auto found = events_.find(snapshot.event_id); found != events_.end()) {
        if (found->second != snapshot)
            throw DomainError(ErrorCode::Conflict, "Event ID was already used with different data.");
        return {SnapshotOutcome::Duplicate, {}};
    }
    events_.emplace(snapshot.event_id, snapshot);
    // reject older observations (stale)
    if (const auto found = latest_observations_.find(snapshot.queue_id);
        found != latest_observations_.end() && snapshot.observed_at_sec < found->second)
        return {SnapshotOutcome::Stale, {}};
    // update queue's latest state
    latest_observations_[snapshot.queue_id] = snapshot.observed_at_sec;
    latest_snapshots_[snapshot.queue_id] = snapshot;

    // Rule removals are reconciled at configuration boundaries, not by a nested
    // scan on every observation. The application currently exposes no deletion.
    SnapshotResult result{SnapshotOutcome::Processed, {}};
    for (int id : rules.for_queue(snapshot.queue_id)) {
        const auto& rule = rules.at(id);
        if (!rule.enabled) {
            close_episode(rule.id, CloseReason::RuleDisabled);
            // Keep configuration current even for disabled rules.
            rule_states_.insert_or_assign(rule.id, RuleState{rule, false, 0});
        } else if (queue_of(rule) == snapshot.queue_id) {
            auto created = evaluate(rule, snapshot, Trigger::Event);
            result.new_notifications.insert(result.new_notifications.end(), created.begin(), created.end());
        }
    }
    return result;
}
// checks one rule against data we already have, usually because the lead created, edited, enabled, or disabled it.
void NotificationEngine::reevaluate_validated(int id, const RuleCollection& rules) {
    const auto* found = &rules.at(id);
    // rule disabled close the state and reason
    if (!found->enabled) {
        close_episode(id, CloseReason::RuleDisabled);
        rule_states_.insert_or_assign(id, RuleState{*found, false, 0});
        return;
    }
    // check the rule against the recent snapshot and close or evaluate the snapshot accordingly
    const auto snapshot = latest_snapshots_.find(queue_of(*found));
    if (snapshot == latest_snapshots_.end()) {
        close_episode(id, CloseReason::RuleChanged);
        rule_states_.insert_or_assign(id, RuleState{*found, false, 0});
    } else {
        evaluate(*found, snapshot->second, Trigger::RuleChange);
    }
}
// read functions
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
    close_episode(id, CloseReason::RuleChanged);
    rule_states_.erase(id);
}

std::size_t NotificationEngine::active_notification_id(int rule_id) const {
    const auto found = rule_states_.find(rule_id);
    return found != rule_states_.end() && found->second.firing ? found->second.notification_id : 0;
}

// Convenience boundary for standalone callers/tests. The application passes its
// already-validated RuleCollection to the indexed overload above.
SnapshotResult NotificationEngine::process(const QueueSnapshot& snapshot, const std::vector<QueueRule>& values) {
    RuleCollection rules(values);
    std::vector<int> removed;
    for (const auto& [id, state] : rule_states_) {
        (void)state;
        if (!rules.contains(id)) removed.push_back(id);
    }
    for (int id : removed) { close_episode(id, CloseReason::RuleRemoved); rule_states_.erase(id); }
    return process_validated(snapshot, rules);
}
void NotificationEngine::reevaluate(int id, const std::vector<QueueRule>& rules) {
    reevaluate_validated(id, RuleCollection(rules));
}

void NotificationEngine::compact(std::size_t history_limit, std::size_t event_limit) {
    // IDs remain stable and monotonically increasing after pruning.
    if (notifications_.size() > history_limit) {
        const auto recent_start = notifications_.size() - history_limit;
        std::vector<Notification> retained;
        for (std::size_t index = 0; index < notifications_.size(); ++index)
            if (index >= recent_start || notifications_[index].close_reason == CloseReason::Active)
                retained.push_back(std::move(notifications_[index]));
        notifications_ = std::move(retained);
        notification_positions_.clear();
        for (std::size_t index = 0; index < notifications_.size(); ++index)
            notification_positions_[notifications_[index].id] = index;
    }
    if (events_.size() <= event_limit) return;
    std::unordered_set<std::string> keep;
    for (const auto& notification : notifications_) keep.insert(notification.event_id);
    // Preserve all events at the current watermark, including equal-time arrivals.
    // Older evicted events can only return stale, never mutate current episodes.
    for (const auto& [id, event] : events_)
        if (event.observed_at_sec >= latest_observations_.at(event.queue_id)) keep.insert(id);
    std::vector<const QueueSnapshot*> recent;
    for (const auto& [id, event] : events_) { (void)id; recent.push_back(&event); }
    std::sort(recent.begin(), recent.end(), [](const auto* a, const auto* b) {
        return a->observed_at_sec != b->observed_at_sec ? a->observed_at_sec > b->observed_at_sec : a->event_id < b->event_id;
    });
    for (std::size_t index = 0; index < std::min(event_limit, recent.size()); ++index)
        keep.insert(recent[index]->event_id);
    std::erase_if(events_, [&](const auto& entry) { return !keep.contains(entry.first); });
}
} // namespace intraday
