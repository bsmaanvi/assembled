#pragma once

#include <cstdint>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

#include "third_party/json.hpp"
#include "notification_engine.hpp"

namespace intraday {
using Json = nlohmann::json;

inline std::int64_t integer(const Json& value, const std::string& field,
                            std::int64_t minimum = 0,
                            std::int64_t maximum = std::numeric_limits<int>::max()) {
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(maximum)))
        throw std::invalid_argument(field + " must be a whole number in range.");
    const auto result = value.get<std::int64_t>();
    if (result < minimum || result > maximum)
        throw std::invalid_argument(field + " is outside its allowed range.");
    return result;
}

inline std::int64_t parse_timestamp(const std::string& text) {
    std::tm parsed{};
    std::istringstream input(text);
    input >> std::get_time(&parsed, "%Y-%m-%dT%H:%M:%SZ");
    if (input.fail() || input.peek() != std::char_traits<char>::eof())
        throw std::invalid_argument("ts must use UTC YYYY-MM-DDTHH:MM:SSZ format.");
    const auto seconds = ::timegm(&parsed);
    std::ostringstream normalized;
    normalized << std::put_time(&parsed, "%Y-%m-%dT%H:%M:%SZ");
    if (seconds < 0 || normalized.str() != text)
        throw std::invalid_argument("ts is not a valid UTC date.");
    return seconds;
}

inline Json rule_json(const QueueRule& rule) {
    Json result{{"id", rule.id}, {"enabled", rule.enabled}};
    if (const auto* backlog = std::get_if<BacklogRule>(&rule.definition)) {
        result["type"] = "backlog";
        result["queue_id"] = backlog->queue_id;
        result["threshold"] = backlog->tickets_waiting_threshold;
    } else {
        result["type"] = "sla";
        result["queue_id"] = std::get<SlaBreachRule>(rule.definition).queue_id;
    }
    return result;
}

inline RuleDefinition definition_from_json(const Json& value) {
    const auto type = value.at("type").get<std::string>();
    const auto queue = value.at("queue_id").get<std::string>();
    if (type == "backlog")
        return BacklogRule{queue, static_cast<int>(integer(value.at("threshold"), "threshold"))};
    if (type == "sla") return SlaBreachRule{queue};
    throw std::invalid_argument("Rule type must be backlog or sla.");
}

inline QueueRule rule_from_json(const Json& value) {
    return {static_cast<int>(integer(value.at("id"), "rule ID", 1)),
            value.at("enabled").get<bool>(), definition_from_json(value)};
}

inline Json snapshot_json(const QueueSnapshot& value) {
    return {{"event_id", value.event_id}, {"observed_at_sec", value.observed_at_sec},
            {"queue_id", value.queue_id}, {"tickets_waiting", value.tickets_waiting},
            {"longest_wait_sec", value.longest_wait_sec}, {"sla_target_sec", value.sla_target_sec}};
}

inline QueueSnapshot snapshot_from_json(const Json& value) {
    if (value.contains("type") && value.at("type") != "queue_snapshot")
        throw std::invalid_argument("Only queue_snapshot events are supported in this MVP.");
    const auto timestamp = value.contains("ts") ? parse_timestamp(value.at("ts").get<std::string>())
        : integer(value.at("observed_at_sec"), "observation time", 0, 253402300799LL);
    return {value.at("event_id").get<std::string>(), timestamp,
            value.at("queue_id").get<std::string>(),
            static_cast<int>(integer(value.at("tickets_waiting"), "tickets_waiting")),
            static_cast<int>(integer(value.at("longest_wait_sec"), "longest_wait_sec")),
            static_cast<int>(integer(value.at("sla_target_sec"), "sla_target_sec", 1))};
}

inline Json notification_json(const Notification& value) {
    return {{"id", value.id}, {"rule_id", value.rule_id}, {"event_id", value.event_id},
            {"queue_id", value.queue_id}, {"observed_at_sec", value.observed_at_sec},
            {"observed_value", value.observed_value}, {"threshold", value.threshold},
            {"explanation", value.explanation}, {"close_reason", value.close_reason},
            {"trigger", value.trigger}, {"rule_type", value.rule_type}};
}
} // namespace intraday
