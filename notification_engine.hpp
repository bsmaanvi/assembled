#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "queue_rule.hpp"
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
    std::string close_reason; // Empty while active; history retains original evidence.
    std::string trigger = "event";
    std::string rule_type = "backlog";
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
    const std::vector<Notification>& notifications() const;
    void forget_rule(int id); // Legacy CLI hook.
    void reevaluate(int id, const std::vector<QueueRule>& rules);
    const std::unordered_map<std::string, QueueSnapshot>& latest_snapshots() const;
    std::vector<int> active_rule_ids() const;
    std::string export_state() const;
    static NotificationEngine restore(const std::string& state,
                                      const std::vector<QueueRule>& rules);

private:
    struct RuleState {
        QueueRule configuration;
        bool firing = false;
        std::size_t notification_id = 0;
    };
    std::vector<Notification> evaluate(const QueueRule& rule, const QueueSnapshot& snapshot,
                                       const std::string& trigger);
    void close_episode(int id, const std::string& reason);
    std::unordered_map<int, RuleState> rule_states_;
    std::unordered_map<std::string, QueueSnapshot> latest_snapshots_;
    std::unordered_map<std::string, QueueSnapshot> events_;
    std::unordered_map<std::string, std::int64_t> latest_observations_;
    std::vector<Notification> notifications_;
};

} // namespace intraday
