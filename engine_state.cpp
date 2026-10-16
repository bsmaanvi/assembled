#include "notification_engine.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "json_codec.hpp"
#include "rule_collection.hpp"

namespace intraday {
std::string NotificationEngine::export_state() const {
    Json result{{"events", Json::array()}, {"latest", Json::array()},
                {"notifications", Json::array()}, {"rule_states", Json::array()},
                {"next_notification_id", next_notification_id_}};
    for (const auto& [id, event] : events_) {
        (void)id;
        result["events"].push_back(snapshot_json(event));
    }
    for (const auto& [queue, snapshot] : latest_snapshots_) {
        (void)queue;
        result["latest"].push_back(snapshot_json(snapshot));
    }
    for (const auto& notification : notifications_)
        result["notifications"].push_back(notification_json(notification));
    for (const auto& [id, state] : rule_states_)
        result["rule_states"].push_back({{"rule", rule_json(state.configuration)},
            {"firing", state.firing}, {"notification_id", state.notification_id}, {"id", id}});
    return result.dump();
}

NotificationEngine NotificationEngine::restore(const std::string& text,
                                               const std::vector<QueueRule>& rules, const QueueCatalog& catalog) {
    const auto saved = Json::parse(text);
    NotificationEngine engine;
    for (const auto& key : {"events", "latest", "notifications", "rule_states"})
        if (!saved.at(key).is_array()) throw std::runtime_error("Invalid saved engine collection.");
    for (const auto& value : saved.at("events")) {
        const auto event = snapshot_from_json(value);
        // Reuse validation without trusting persisted event fields.
        NotificationEngine validation;
        validation.process_validated(event, RuleCollection({}, catalog));
        if (!engine.events_.emplace(event.event_id, event).second)
            throw std::runtime_error("Duplicate event in saved state.");
    }
    for (const auto& value : saved.at("latest")) {
        const auto event = snapshot_from_json(value);
        const auto found = engine.events_.find(event.event_id);
        if (found == engine.events_.end() || found->second != event ||
            !engine.latest_snapshots_.emplace(event.queue_id, event).second)
            throw std::runtime_error("Invalid latest observation in saved state.");
        engine.latest_observations_[event.queue_id] = event.observed_at_sec;
    }
    for (const auto& [id, event] : engine.events_) {
        (void)id;
        if (!engine.latest_observations_.contains(event.queue_id) ||
            engine.latest_observations_.at(event.queue_id) < event.observed_at_sec)
            throw std::runtime_error("Saved queue watermark does not cover its events.");
    }
    for (const auto& value : saved.at("notifications")) {
        Notification notification{
            static_cast<std::size_t>(integer(value.at("id"), "notification ID", 1)),
            static_cast<int>(integer(value.at("rule_id"), "rule ID", 1)),
            value.at("event_id").get<std::string>(), value.at("queue_id").get<std::string>(),
            integer(value.at("observed_at_sec"), "observation time", 0, 253402300799LL),
            static_cast<int>(integer(value.at("observed_value"), "observed value")),
            static_cast<int>(integer(value.at("threshold"), "threshold")),
            value.at("explanation").get<std::string>(), close_reason_from_name(value.at("close_reason").get<std::string>()),
            trigger_from_name(value.at("trigger").get<std::string>()), rule_kind_from_name(value.at("rule_type").get<std::string>())};
        if ((!engine.notifications_.empty() && notification.id <= engine.notifications_.back().id) ||
            !engine.events_.contains(notification.event_id) ||
            engine.events_.at(notification.event_id).queue_id != notification.queue_id ||
            engine.events_.at(notification.event_id).observed_at_sec != notification.observed_at_sec ||
            std::none_of(rules.begin(), rules.end(), [&](const auto& rule) { return rule.id == notification.rule_id; }))
            throw std::runtime_error("Invalid notification in saved state.");
        const auto& event = engine.events_.at(notification.event_id);
        // Older state files had no notification context. The original event is
        // still available, so restore only what it actually reported.
        notification.context = value.contains("context") && !value.at("context").is_null()
            ? snapshot_from_json(value.at("context")) : event;
        if (*notification.context != event)
            throw std::runtime_error("Notification context disagrees with its source event.");
        if (notification.observed_value != (notification.rule_type == RuleKind::Sla ? event.longest_wait_sec : event.tickets_waiting) ||
            (notification.rule_type == RuleKind::Sla && notification.threshold != event.sla_target_sec) ||
            notification.observed_value <= notification.threshold)
            throw std::runtime_error("Invalid notification evidence in saved state.");
        engine.notification_positions_[notification.id] = engine.notifications_.size();
        engine.notifications_.push_back(std::move(notification));
    }
    std::set<std::size_t> active_notifications;
    for (const auto& value : saved.at("rule_states")) {
        const auto rule = rule_from_json(value.at("rule"));
        const bool firing = value.at("firing").get<bool>();
        const auto notification_id = static_cast<std::size_t>(integer(value.at("notification_id"), "notification ID"));
        const auto found = std::find_if(rules.begin(), rules.end(), [&](const auto& current) { return current == rule; });
        if (found == rules.end() || integer(value.at("id"), "rule ID", 1) != rule.id ||
            (!firing && notification_id != 0) ||
            (firing && (!rule.enabled || notification_id == 0 || !engine.notification_positions_.contains(notification_id))))
            throw std::runtime_error("Invalid rule episode in saved state.");
        const auto& queue = std::visit([](const auto& definition) -> const std::string& {
            return definition.queue_id;
        }, rule.definition);
        const auto latest = engine.latest_snapshots_.find(queue);
        const bool backlog = std::holds_alternative<BacklogRule>(rule.definition);
        const bool expected_firing = rule.enabled && latest != engine.latest_snapshots_.end() &&
            (backlog ? latest->second.tickets_waiting > std::get<BacklogRule>(rule.definition).tickets_waiting_threshold
                     : latest->second.longest_wait_sec > latest->second.sla_target_sec);
        if (firing != expected_firing)
            throw std::runtime_error("Saved episode disagrees with current rule and observation.");
        if (firing) {
            const auto& notification = engine.notifications_.at(engine.notification_positions_.at(notification_id));
            if (notification.rule_id != rule.id || notification.queue_id != queue ||
                notification.rule_type != (backlog ? RuleKind::Backlog : RuleKind::Sla) || notification.close_reason != CloseReason::Active ||
                !active_notifications.insert(notification_id).second)
                throw std::runtime_error("Invalid active notification in saved state.");
        }
        if (!engine.rule_states_.emplace(rule.id, RuleState{rule, firing, notification_id}).second)
            throw std::runtime_error("Duplicate rule episode in saved state.");
    }
    for (const auto& notification : engine.notifications_)
        if (notification.close_reason == CloseReason::Active && !active_notifications.contains(notification.id))
            throw std::runtime_error("Active notification has no saved episode.");
    const auto minimum_next = engine.notifications_.empty() ? 1 : engine.notifications_.back().id + 1;
    engine.next_notification_id_ = saved.contains("next_notification_id")
        ? static_cast<std::size_t>(integer(saved.at("next_notification_id"), "next notification ID", 1))
        : minimum_next;
    if (engine.next_notification_id_ < minimum_next)
        throw std::runtime_error("Invalid next notification ID.");
    return engine;
}
} // namespace intraday
