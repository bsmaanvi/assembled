#include "application.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <tuple>
#include <unistd.h>

namespace intraday {
namespace {
const std::string& queue_of(const QueueRule& rule) {
    return std::visit([](const auto& value) -> const std::string& { return value.queue_id; }, rule.definition);
}
std::string outcome_name(SnapshotOutcome outcome) {
    switch (outcome) {
        case SnapshotOutcome::Processed: return "processed";
        case SnapshotOutcome::Duplicate: return "duplicate";
        case SnapshotOutcome::Stale: return "stale";
    }
    throw std::logic_error("Unknown snapshot outcome");
}
Json result_json(const SnapshotResult& result) {
    Json notifications = Json::array();
    for (const auto& notification : result.new_notifications)
        notifications.push_back(notification_json(notification));
    return {{"outcome", outcome_name(result.outcome)}, {"new_notifications", notifications}};
}
} // namespace

Json Application::encode(const State& state) {
    Json rules = Json::array();
    for (const auto& rule : state.rules.rules()) rules.push_back(rule_json(rule));
    return {{"schema_version", 1}, {"rules", rules}, {"demo_cursor", state.demo_cursor},
            {"engine", Json::parse(state.engine.export_state())}};
}

Application::Application(std::filesystem::path path) : path_(std::move(path)) {
    if (!std::filesystem::exists(path_)) return;
    if (!std::filesystem::is_regular_file(path_) || std::filesystem::file_size(path_) > 32 * 1024 * 1024)
        throw std::runtime_error("State must be a regular JSON file under 32 MiB.");
    std::ifstream input(path_);
    if (!input) throw std::runtime_error("Cannot read state file; it was left unchanged.");
    const auto saved = Json::parse(input);
    if (saved.at("schema_version") != 1 || !saved.at("rules").is_array())
        throw std::runtime_error("Unsupported state format; it was left unchanged.");
    std::vector<QueueRule> rules;
    for (const auto& value : saved.at("rules")) rules.push_back(rule_from_json(value));
    state_.rules = RuleCollection(std::move(rules));
    state_.demo_cursor = integer(saved.at("demo_cursor"), "demo cursor");
    state_.engine = NotificationEngine::restore(saved.at("engine").dump(), state_.rules.rules());
}

void Application::commit(State candidate) {
    const std::string payload = encode(candidate).dump(2) + "\n";
    if (payload.size() > 32 * 1024 * 1024)
        throw std::runtime_error("Demo state reached 32 MiB. Archive it before continuing.");
    std::string pattern = path_.string() + ".tmp.XXXXXX";
    int fd = ::mkstemp(pattern.data());
    if (fd == -1) throw std::runtime_error("Cannot create temporary state file.");
    try {
        std::size_t offset = 0;
        while (offset < payload.size()) {
            const auto written = ::write(fd, payload.data() + offset, payload.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::runtime_error("Cannot write state file.");
            offset += static_cast<std::size_t>(written);
        }
        if (::fsync(fd) != 0) throw std::runtime_error("Cannot flush state file.");
        const int closed = ::close(fd);
        fd = -1;
        if (closed != 0) throw std::runtime_error("Cannot close state file.");
        std::filesystem::rename(pattern, path_);
    } catch (...) {
        if (fd != -1) ::close(fd);
        std::error_code ignored;
        std::filesystem::remove(pattern, ignored);
        throw;
    }
    state_ = std::move(candidate);
}

Json Application::view() const {
    const std::lock_guard lock(mutex_);
    Json queues = Json::array(), rules = Json::array(), alerts = Json::array(), notifications = Json::array();
    const auto active = state_.engine.active_rule_ids();
    for (const auto& [id, name] : std::vector<std::pair<std::string, std::string>>{
            {"billing", "Billing"}, {"tier_2", "Tier 2 support"}, {"vip", "VIP"}}) {
        Json queue{{"id", id}, {"name", name}, {"snapshot", nullptr}};
        if (const auto found = state_.engine.latest_snapshots().find(id); found != state_.engine.latest_snapshots().end())
            queue["snapshot"] = snapshot_json(found->second);
        queues.push_back(queue);
    }
    for (const auto& rule : state_.rules.rules()) {
        auto encoded = rule_json(rule);
        encoded["status"] = !rule.enabled ? "disabled" :
            !state_.engine.latest_snapshots().contains(queue_of(rule)) ? "waiting_for_data" :
            std::find(active.begin(), active.end(), rule.id) != active.end() ? "firing" : "clear";
        rules.push_back(encoded);
        if (encoded["status"] == "firing") {
            const auto& snapshot = state_.engine.latest_snapshots().at(queue_of(rule));
            const bool backlog = std::holds_alternative<BacklogRule>(rule.definition);
            encoded["severity"] = backlog ? "warning" : "critical";
            encoded["observed_value"] = backlog ? snapshot.tickets_waiting : snapshot.longest_wait_sec;
            encoded["threshold"] = backlog ? std::get<BacklogRule>(rule.definition).tickets_waiting_threshold : snapshot.sla_target_sec;
            encoded["observed_at_sec"] = snapshot.observed_at_sec;
            for (auto notification = state_.engine.notifications().rbegin(); notification != state_.engine.notifications().rend(); ++notification) {
                if (notification->rule_id == rule.id && notification->close_reason.empty()) {
                    encoded["notification_id"] = notification->id;
                    break;
                }
            }
            alerts.push_back(encoded);
        }
    }
    std::sort(alerts.begin(), alerts.end(), [](const auto& left, const auto& right) {
        if (left.at("severity") != right.at("severity")) return left.at("severity") == "critical";
        return left.at("notification_id") > right.at("notification_id");
    });
    for (const auto& notification : state_.engine.notifications()) {
        auto encoded = notification_json(notification);
        // Preserve the original rule type even if its configuration later changes.
        encoded["severity"] = notification.rule_type == "sla" ? "critical" : "warning";
        notifications.push_back(encoded);
    }
    return {{"queues", queues}, {"rules", rules}, {"active_alerts", alerts},
            {"notifications", notifications}, {"demo_cursor", state_.demo_cursor}};
}

Json Application::create_rule(const Json& input) {
    const std::lock_guard lock(mutex_);
    auto candidate = state_;
    const int id = candidate.rules.create(definition_from_json(input));
    if (input.contains("enabled")) candidate.rules.set_enabled(id, input.at("enabled").get<bool>());
    candidate.engine.reevaluate(id, candidate.rules.rules());
    commit(std::move(candidate));
    return {{"id", id}};
}

Json Application::update_rule(int id, const Json& input) {
    const std::lock_guard lock(mutex_);
    if (!input.is_object() || (!input.contains("type") && !input.contains("enabled")))
        throw std::invalid_argument("Supply rule configuration or enabled state.");
    auto candidate = state_;
    if (input.contains("type")) candidate.rules.edit(id, definition_from_json(input));
    if (input.contains("enabled")) candidate.rules.set_enabled(id, input.at("enabled").get<bool>());
    candidate.engine.reevaluate(id, candidate.rules.rules());
    commit(std::move(candidate));
    return {{"id", id}};
}

Json Application::ingest(const Json& input) {
    const std::lock_guard lock(mutex_);
    auto candidate = state_;
    const auto result = candidate.engine.process(snapshot_from_json(input), candidate.rules.rules());
    if (result.outcome != SnapshotOutcome::Duplicate) commit(std::move(candidate));
    return result_json(result);
}

Json Application::replay(const std::string& jsonl) {
    const std::lock_guard lock(mutex_);
    std::istringstream lines(jsonl);
    std::string line;
    std::vector<QueueSnapshot> events;
    int skipped = 0, line_number = 0;
    while (std::getline(lines, line)) {
        ++line_number;
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        try {
            const auto value = Json::parse(line);
            const auto type = value.at("type").get<std::string>();
            if (type == "adherence_check" || type == "agent_state_change") { ++skipped; continue; }
            events.push_back(snapshot_from_json(value));
        } catch (const std::exception& error) {
            throw std::invalid_argument("JSONL line " + std::to_string(line_number) + ": " + error.what());
        }
    }
    if (events.empty()) throw std::invalid_argument("No queue_snapshot events found.");
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
        return a.observed_at_sec < b.observed_at_sec;
    });
    auto candidate = state_;
    const auto before = candidate.engine.notifications().size();
    int processed = 0, duplicates = 0, stale = 0;
    for (const auto& event : events) {
        const auto result = candidate.engine.process(event, candidate.rules.rules());
        if (result.outcome == SnapshotOutcome::Processed) ++processed;
        if (result.outcome == SnapshotOutcome::Duplicate) ++duplicates;
        if (result.outcome == SnapshotOutcome::Stale) ++stale;
    }
    const auto created = candidate.engine.notifications().size() - before;
    commit(std::move(candidate));
    return {{"processed", processed}, {"duplicates", duplicates}, {"stale", stale},
            {"skipped_agent_events", skipped}, {"notifications_created", created}};
}

Json Application::next_demo() {
    const std::lock_guard lock(mutex_);
    auto candidate = state_;
    if (candidate.demo_cursor >= std::numeric_limits<int>::max()) throw std::runtime_error("Demo sequence exhausted.");
    static const std::vector<std::tuple<std::string, int, int, int, std::string>> steps{
        {"billing", 10, 30, 120, "Billing baseline"},
        {"tier_2", 12, 180, 600, "Tier 2 baseline"},
        {"vip", 2, 20, 60, "VIP baseline"},
        {"billing", 23, 90, 120, "Billing backlog crosses 20"},
        {"billing", 30, 150, 120, "Billing also breaches SLA"},
        {"billing", 32, 170, 120, "Ongoing violations: no repeated alerts"},
        {"billing", 10, 140, 120, "Backlog recovers; SLA remains breached"},
        {"billing", 5, 30, 120, "Both Billing conditions recover"},
        {"billing", 25, 160, 120, "New Billing violation episodes"},
    };
    const auto& [queue, waiting, oldest, sla, label] = steps[static_cast<std::size_t>(candidate.demo_cursor) % steps.size()];
    std::int64_t timestamp = 1779804000;
    for (const auto& [id, snapshot] : candidate.engine.latest_snapshots()) {
        (void)id;
        timestamp = std::max(timestamp, snapshot.observed_at_sec + 30);
    }
    const auto result = candidate.engine.process({"synthetic-demo-" + std::to_string(candidate.demo_cursor),
        timestamp, queue, waiting, oldest, sla}, candidate.rules.rules());
    ++candidate.demo_cursor;
    commit(std::move(candidate));
    auto response = result_json(result);
    response["label"] = label;
    return response;
}
} // namespace intraday
