#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "notification_engine.hpp"

namespace {
using namespace intraday;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Action>
void rejects(Action action) {
    try { action(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Expected invalid_argument");
}

QueueSnapshot snapshot(const std::string& id, std::int64_t time, int waiting,
                       int oldest = 0, int sla = 120, std::string queue = "billing") {
    return {id, time, std::move(queue), waiting, oldest, sla};
}

// Backlog fires only above the threshold; equality counts as clear.
void backlog_boundary() {
    NotificationEngine engine;
    const std::vector<QueueRule> rules{{1, true, BacklogRule{"billing", 20}}};
    check(engine.process(snapshot("below", 1, 19), rules).new_notifications.empty(), "Below threshold");
    check(engine.process(snapshot("equal", 2, 20), rules).new_notifications.empty(), "At threshold");
    const auto result = engine.process(snapshot("above", 3, 21), rules);
    check(result.new_notifications.size() == 1, "Above threshold must notify");
    const auto& notification = result.new_notifications[0];
    check(notification.rule_id == 1 && notification.queue_id == "billing" &&
          notification.event_id == "above" && notification.observed_at_sec == 3 &&
          notification.observed_value == 21 && notification.threshold == 20,
          "Notification must preserve triggering evidence");
    check(!notification.explanation.empty(), "Notification must explain its cause");
}

// SLA uses the target in each snapshot, with a strict greater-than comparison.
void sla_boundary_and_target_changes() {
    NotificationEngine engine;
    const std::vector<QueueRule> rules{{1, true, SlaBreachRule{"billing"}}};
    check(engine.process(snapshot("equal", 1, 0, 120), rules).new_notifications.empty(), "At SLA target");
    check(engine.process(snapshot("above", 2, 0, 121), rules).new_notifications.size() == 1, "SLA breach");
    check(engine.process(snapshot("new-target", 3, 0, 121, 180), rules).new_notifications.empty(), "New target clears");
    const auto result = engine.process(snapshot("again", 4, 0, 181, 180), rules);
    check(result.new_notifications.size() == 1 && result.new_notifications[0].threshold == 180,
          "Retrigger must use current target");
}

// Repeated violations stay quiet; observed recovery allows a new notification.
void suppresses_and_retriggers() {
    NotificationEngine engine;
    const std::vector<QueueRule> rules{{1, true, BacklogRule{"billing", 20}}};
    engine.process(snapshot("first", 1, 21), rules);
    check(engine.process(snapshot("still-bad", 2, 30), rules).new_notifications.empty(), "Suppress ongoing episode");
    check(engine.process(snapshot("clear", 3, 20), rules).new_notifications.empty(), "Recovery must not alert");
    const auto result = engine.process(snapshot("second", 4, 25), rules);
    check(result.new_notifications.size() == 1 && engine.notifications().size() == 2, "New episode must notify");
    check(result.new_notifications[0].id == 2 && engine.notifications()[0].observed_value == 21,
          "History must retain original evidence and distinct IDs");
}

// A snapshot evaluates only enabled rules for its own queue.
void matches_queue_and_enabled_state() {
    NotificationEngine engine;
    const std::vector<QueueRule> rules{
        {1, false, BacklogRule{"billing", 20}}, {2, true, BacklogRule{"vip", 20}},
    };
    check(engine.process(snapshot("billing", 1, 100), rules).new_notifications.empty(), "Disabled and other queue");
    const auto result = engine.process(snapshot("vip", 1, 100, 0, 120, "vip"), rules);
    check(result.new_notifications.size() == 1 && result.new_notifications[0].rule_id == 2, "Correct queue rule");
}

// Different rules maintain independent episodes, even on the same queue.
void rules_are_independent() {
    NotificationEngine engine;
    const std::vector<QueueRule> rules{
        {1, true, BacklogRule{"billing", 20}}, {2, true, SlaBreachRule{"billing"}},
    };
    check(engine.process(snapshot("both", 1, 30, 200), rules).new_notifications.size() == 2, "Both rules fire");
    engine.process(snapshot("backlog-clear", 2, 10, 200), rules);
    const auto result = engine.process(snapshot("backlog-again", 3, 30, 200), rules);
    check(result.new_notifications.size() == 1 && result.new_notifications[0].rule_id == 1, "Only backlog retriggers");
}

// Retrying an identical event does not re-evaluate it or add to the inbox.
void duplicate_is_ignored() {
    NotificationEngine engine;
    const std::vector<QueueRule> rules{{1, true, BacklogRule{"billing", 20}}};
    const auto event = snapshot("event", 1, 30);
    engine.process(event, rules);
    const auto result = engine.process(event, rules);
    check(result.outcome == SnapshotOutcome::Duplicate && result.new_notifications.empty() &&
          engine.notifications().size() == 1, "Duplicate must be a no-op");
}

// Reusing an event ID with different values is rejected without clearing an episode.
void conflicting_event_is_rejected() {
    NotificationEngine engine;
    const std::vector<QueueRule> rules{{1, true, BacklogRule{"billing", 20}}};
    engine.process(snapshot("event", 2, 30), rules);
    rejects([&] { engine.process(snapshot("event", 3, 0), rules); });
    check(engine.process(snapshot("next", 4, 30), rules).new_notifications.empty(), "Conflict must not clear episode");
    check(engine.notifications().size() == 1, "Conflict must not alter history");
}

// A stale recovery cannot clear a newer violation; stale IDs remain deduplicated.
void stale_recovery_is_ignored() {
    NotificationEngine engine;
    const std::vector<QueueRule> rules{{1, true, BacklogRule{"billing", 20}}};
    engine.process(snapshot("new", 10, 30), rules);
    const auto old = snapshot("old", 5, 0);
    check(engine.process(old, rules).outcome == SnapshotOutcome::Stale, "Older event must be stale");
    check(engine.process(old, rules).outcome == SnapshotOutcome::Duplicate, "Stale retry is duplicate");
    check(engine.process(snapshot("newer", 11, 30), rules).new_notifications.empty(), "Episode must remain open");
}

// Timestamp ordering belongs to each queue independently, not the whole feed.
void queue_watermarks_are_independent() {
    NotificationEngine engine;
    const std::vector<QueueRule> rules{{1, true, BacklogRule{"vip", 20}}};
    engine.process(snapshot("billing", 100, 0), rules);
    const auto result = engine.process(snapshot("vip", 1, 30, 0, 120, "vip"), rules);
    check(result.outcome == SnapshotOutcome::Processed && result.new_notifications.size() == 1,
          "Earlier time on another queue is valid");
}

// Distinct events with equal timestamps are processed in arrival order.
void equal_timestamps_use_arrival_order() {
    NotificationEngine engine;
    const std::vector<QueueRule> rules{{1, true, BacklogRule{"billing", 20}}};
    engine.process(snapshot("first", 10, 30), rules);
    engine.process(snapshot("clear", 10, 0), rules);
    check(engine.process(snapshot("again", 10, 30), rules).new_notifications.size() == 1,
          "Equal-time recovery permits retrigger");
}

// Invalid snapshots neither consume their event IDs nor advance queue time.
void invalid_snapshots_do_not_change_state() {
    NotificationEngine engine;
    const std::vector<QueueRule> rules{{1, true, BacklogRule{"billing", 20}}};
    const std::vector<QueueSnapshot> invalid{
        snapshot("", 100, 30), snapshot("event", -1, 30), snapshot("event", 100, -1),
        snapshot("event", 100, 30, -1), snapshot("event", 100, 30, 0, 0),
        snapshot("event", 100, 30, 0, -1), snapshot("event", 100, 30, 0, 120, "unknown"),
    };
    for (const auto& event : invalid) rejects([&] { engine.process(event, rules); });
    const auto result = engine.process(snapshot("event", 1, 30), rules);
    check(result.outcome == SnapshotOutcome::Processed && result.new_notifications.size() == 1,
          "Corrected snapshot must still be accepted");
}

// Changing the rule configuration resets its episode on the next fresh snapshot.
void edits_reset_episode() {
    NotificationEngine engine;
    std::vector<QueueRule> rules{{1, true, BacklogRule{"billing", 20}}};
    engine.process(snapshot("first", 1, 30, 200), rules);
    rules[0].definition = SlaBreachRule{"billing"};
    check(engine.process(snapshot("edited", 2, 30, 200), rules).new_notifications.size() == 1,
          "Edited rule must evaluate as a new episode");
    check(engine.notifications()[0].threshold == 20, "Old evidence remains unchanged");
}

// Disabling suppresses alerts; enabling later evaluates the next fresh snapshot.
void disabling_and_reenabling() {
    NotificationEngine engine;
    std::vector<QueueRule> rules{{1, true, BacklogRule{"billing", 20}}};
    engine.process(snapshot("first", 1, 30), rules);
    rules[0].enabled = false;
    check(engine.process(snapshot("disabled", 2, 30), rules).new_notifications.empty(), "Disabled rule stays quiet");
    rules[0].enabled = true;
    check(engine.process(snapshot("enabled", 3, 30), rules).new_notifications.size() == 1,
          "Re-enabled rule starts new episode");
}

// Explicit reset supports menu edits/disable actions between snapshots and retains history.
void explicit_reset_preserves_history() {
    NotificationEngine engine;
    const std::vector<QueueRule> rules{{1, true, BacklogRule{"billing", 20}}};
    engine.process(snapshot("first", 1, 30), rules);
    engine.forget_rule(1);
    check(engine.notifications().size() == 1, "Reset must keep notifications");
    check(engine.process(snapshot("again", 2, 30), rules).new_notifications.size() == 1, "Reset allows fresh evaluation");
}

// Removing a rule drops its episode so reintroducing it later does not inherit suppression.
void removed_rules_drop_state() {
    NotificationEngine engine;
    const std::vector<QueueRule> rules{{1, true, BacklogRule{"billing", 20}}};
    engine.process(snapshot("first", 1, 30), rules);
    engine.process(snapshot("removed", 2, 30), {});
    check(engine.process(snapshot("readded", 3, 30), rules).new_notifications.size() == 1,
          "Removed episode must not survive");
}

// Invalid rule configuration is rejected before an event is marked processed.
void invalid_rules_do_not_consume_events() {
    NotificationEngine engine;
    const auto event = snapshot("event", 1, 30);
    rejects([&] { engine.process(event, {{1, true, BacklogRule{"billing", -1}}}); });
    check(engine.process(event, {{1, true, BacklogRule{"billing", 20}}}).new_notifications.size() == 1,
          "Valid retry must be processed");
}
// Alert evidence keeps its triggering context while current queue metrics update.
void context_is_captured_without_changing_suppression() {
    NotificationEngine engine;
    const std::vector<QueueRule> rules{{1, true, BacklogRule{"billing", 20}}};
    auto first = snapshot("first", 1, 23);
    first.agents_available = 4;
    first.agents_on_call = 12;
    first.volume_last_15m = 78;
    first.volume_forecast_next_15m = 50;
    const auto result = engine.process(first, rules);
    check(result.new_notifications.size() == 1 &&
          result.new_notifications[0].context == first, "Notification must capture source snapshot");
    auto next = first;
    next.event_id = "next";
    next.observed_at_sec = 2;
    next.agents_available = 0;
    next.volume_last_15m = 90;
    check(engine.process(next, rules).new_notifications.empty(), "Context change alone must not repeat alert");
    check(engine.latest_snapshots().at("billing").agents_available == 0, "Current dashboard context updates");
    check(engine.notifications()[0].context->agents_available == 4 &&
          engine.notifications()[0].context->volume_last_15m == 78, "History must not use newer context");
}

// Missing context is unknown, zeros are valid, and negative counts are rejected.
void optional_context_validation() {
    NotificationEngine engine;
    auto missing = snapshot("missing", 1, 0);
    engine.process(missing, {});
    check(!engine.latest_snapshots().at("billing").agents_available, "Missing must not become zero");
    auto negative = snapshot("retry", 2, 0);
    negative.agents_available = -1;
    rejects([&] { engine.process(negative, {}); });
    negative.agents_available = 0;
    negative.agents_on_call = 0;
    negative.volume_last_15m = 0;
    negative.volume_forecast_next_15m = 0;
    check(engine.process(negative, {}).outcome == SnapshotOutcome::Processed,
          "Rejected context must not consume event ID; zero counts are valid");
}
// The indexed production path evaluates explicitly registered queues.
void indexed_path_accepts_registered_queue() {
    RuleCollection rules;
    rules.add_queue({"refunds", "Refunds"});
    const int id = rules.create(BacklogRule{"refunds", 20});
    NotificationEngine engine;
    const auto result = engine.process_validated(snapshot("refunds-event", 1, 21, 0, 120, "refunds"), rules);
    check(result.new_notifications.size() == 1 && engine.active_notification_id(id) == 1,
          "New queue evaluates through indexed rules");
    check(result.new_notifications[0].close_reason == CloseReason::Active &&
          result.new_notifications[0].trigger == Trigger::Event &&
          result.new_notifications[0].rule_type == RuleKind::Backlog,
          "Lifecycle values must be typed");
}

// Compaction protects active evidence and stable IDs; later recovery uses the ID index.
void retention_preserves_active_episodes_and_ids() {
    RuleCollection rules;
    rules.create(BacklogRule{"billing", 20});
    NotificationEngine engine;
    for (int index = 0; index < 3; ++index) {
        engine.process_validated(snapshot("open-" + std::to_string(index), index * 2 + 1, 23), rules);
        engine.process_validated(snapshot("close-" + std::to_string(index), index * 2 + 2, 0), rules);
    }
    engine.process_validated(snapshot("active", 7, 23), rules);
    engine.compact(0, 0);
    check(engine.notifications().size() == 1 && engine.notifications()[0].id == 4,
          "Active notification survives even zero retention target");
    check(engine.active_notification_id(1) == 4, "Stable ID remains linked");
    engine.process_validated(snapshot("recovery", 8, 0), rules);
    check(engine.notifications()[0].close_reason == CloseReason::Recovered,
          "Recovery works when vector index is no longer ID minus one");
    const auto next = engine.process_validated(snapshot("next", 9, 23), rules);
    check(next.new_notifications[0].id == 5, "IDs must not be reused after pruning");
    check(engine.process_validated(snapshot("open-0", 1, 23), rules).outcome == SnapshotOutcome::Stale,
          "Evicted old event returns stale, cannot reopen current episode");
}
} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"backlog boundary and evidence", backlog_boundary},
        {"SLA boundary and changing targets", sla_boundary_and_target_changes},
        {"suppression and recovery", suppresses_and_retriggers},
        {"queue matching and disabled rules", matches_queue_and_enabled_state},
        {"independent rule episodes", rules_are_independent},
        {"duplicate events", duplicate_is_ignored},
        {"conflicting event IDs", conflicting_event_is_rejected},
        {"stale recovery", stale_recovery_is_ignored},
        {"per-queue ordering", queue_watermarks_are_independent},
        {"equal timestamps", equal_timestamps_use_arrival_order},
        {"invalid snapshots", invalid_snapshots_do_not_change_state},
        {"edited rules", edits_reset_episode},
        {"disable and re-enable", disabling_and_reenabling},
        {"explicit reset", explicit_reset_preserves_history},
        {"removed rules", removed_rules_drop_state},
        {"invalid rule input", invalid_rules_do_not_consume_events},
        {"notification context preserves evidence", context_is_captured_without_changing_suppression},
        {"optional context validation", optional_context_validation},
        {"indexed custom queue path", indexed_path_accepts_registered_queue},
        {"retention protects IDs and active evidence", retention_preserves_active_episodes_and_ids},
    };
    int failures = 0;
    for (const auto& [name, run] : tests) {
        try { run(); std::cout << "PASS: " << name << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " notification tests passed\n";
    return failures == 0 ? 0 : 1;
}
