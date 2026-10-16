#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <optional>

#include "queue_rule.hpp"
#include "rule_collection.hpp"
#include "lifecycle.hpp"
#include "queue_snapshot.hpp"

namespace intraday {

enum class SnapshotOutcome { Processed, Duplicate, Stale };

struct Notification {
    std::size_t id;
    int rule_id;
    std::string event_id;
    std::string queue_id;
    std::int64_t observed_at_sec;
    int observed_value;
    int threshold;
    std::string explanation;
    CloseReason close_reason = CloseReason::Active; // Active until closed; history retains original evidence.
    Trigger trigger = Trigger::Event;
    RuleKind rule_type = RuleKind::Backlog;
    // Evidence at notification creation, not the queue's later/current state.
    std::optional<QueueSnapshot> context = std::nullopt;
};

struct SnapshotResult {
    SnapshotOutcome outcome;
    std::vector<Notification> new_notifications;
};

// Rule evaluation and episode state; persistence is handled by Application.
class NotificationEngine {
public:
    SnapshotResult process(const QueueSnapshot& snapshot,
                           const std::vector<QueueRule>& rules);
    SnapshotResult process_validated(const QueueSnapshot& snapshot, const RuleCollection& rules);
    void reevaluate_validated(int id, const RuleCollection& rules);
    const std::vector<Notification>& notifications() const;
    void forget_rule(int id); // Explicit reset; normal configuration edits use reevaluate().
    void reevaluate(int id, const std::vector<QueueRule>& rules);
    const std::unordered_map<std::string, QueueSnapshot>& latest_snapshots() const;
    std::vector<int> active_rule_ids() const;
    std::size_t active_notification_id(int rule_id) const;
    // Keep recent history/events, always protecting active alerts and their evidence.
    void compact(std::size_t history_limit = 1000, std::size_t event_limit = 10000);
    std::string export_state() const;
    static NotificationEngine restore(const std::string& state,
                                      const std::vector<QueueRule>& rules,
                                      const QueueCatalog& catalog = QueueCatalog{});

private:
    struct RuleState {
        QueueRule configuration;
        bool firing = false; // differentiates new viilation from the old one
        std::size_t notification_id = 0;
    };
    std::vector<Notification> evaluate(const QueueRule& rule, const QueueSnapshot& snapshot,
                                       Trigger trigger);
    void close_episode(int id, CloseReason reason);
    std::unordered_map<int, RuleState> rule_states_; // rule id rule state
    std::unordered_map<std::string, QueueSnapshot> latest_snapshots_; //queue ID - latest accepted snapshot for that queue
    std::unordered_map<std::string, QueueSnapshot> events_; //event_id snapshot received under that event id
    std::unordered_map<std::string, std::int64_t> latest_observations_; // queue_id latest accepted observation timestamp
    std::vector<Notification> notifications_;
    std::unordered_map<std::size_t, std::size_t> notification_positions_;
    std::size_t next_notification_id_ = 1;
};

} // namespace intraday
