#pragma once
#include <string_view>
#include <stdexcept>

namespace intraday {
enum class CloseReason { Active, Recovered, RuleChanged, RuleDisabled, RuleRemoved };
enum class Trigger { Event, RuleChange };
enum class RuleKind { Backlog, Sla };

inline std::string_view name(CloseReason reason) {
    switch (reason) {
        case CloseReason::Active: return "";
        case CloseReason::Recovered: return "recovered";
        case CloseReason::RuleChanged: return "rule_changed";
        case CloseReason::RuleDisabled: return "rule_disabled";
        case CloseReason::RuleRemoved: return "rule_removed";
    }
    throw std::logic_error("Unknown closing reason");
}
inline std::string_view name(Trigger trigger) {
    switch (trigger) {
        case Trigger::Event: return "event";
        case Trigger::RuleChange: return "rule_change";
    }
    throw std::logic_error("Unknown trigger");
}
inline std::string_view name(RuleKind kind) {
    switch (kind) {
        case RuleKind::Backlog: return "backlog";
        case RuleKind::Sla: return "sla";
    }
    throw std::logic_error("Unknown rule kind");
}
inline CloseReason close_reason_from_name(std::string_view value) {
    for (auto reason : {CloseReason::Active, CloseReason::Recovered, CloseReason::RuleChanged,
                        CloseReason::RuleDisabled, CloseReason::RuleRemoved})
        if (name(reason) == value) return reason;
    throw std::invalid_argument("Unknown closing reason");
}
inline Trigger trigger_from_name(std::string_view value) {
    if (value == "event") return Trigger::Event;
    if (value == "rule_change") return Trigger::RuleChange;
    throw std::invalid_argument("Unknown trigger");
}
inline RuleKind rule_kind_from_name(std::string_view value) {
    if (value == "backlog") return RuleKind::Backlog;
    if (value == "sla") return RuleKind::Sla;
    throw std::invalid_argument("Unknown rule kind");
}
} // namespace intraday
