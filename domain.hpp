#pragma once

#include <string_view>

namespace intraday {

// A person's responsibility in the application.
enum class Role {
    Agent,
    TeamLead,
};

// What a support agent is currently doing.
enum class AgentStatus {
    Available,
    OnCall,
    OnBreak,
    InMeeting,
    Offline,
};

// Convert our typed values into text for display.
constexpr std::string_view to_string(Role role) {
    switch (role) {
        case Role::Agent: return "Agent";
        case Role::TeamLead: return "Team lead";
    }
    return "Unknown role";
}

constexpr std::string_view to_string(AgentStatus status) {
    switch (status) {
        case AgentStatus::Available: return "Available";
        case AgentStatus::OnCall: return "On call";
        case AgentStatus::OnBreak: return "On break";
        case AgentStatus::InMeeting: return "In meeting";
        case AgentStatus::Offline: return "Offline";
    }
    return "Unknown status";
}

} // namespace intraday
