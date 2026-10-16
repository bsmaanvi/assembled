#include "application.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <iostream>
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
// converting the notifications into json
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
    Json queues = Json::array();
    for (const auto& queue : state.rules.catalog().queues()) queues.push_back({{"id", queue.id}, {"name", queue.name}});
    return {{"schema_version", 2}, {"queues", queues}, {"rules", rules}, {"demo_cursor", state.demo_cursor},
            {"engine", Json::parse(state.engine.export_state())}};
}
// checks for an existing file and if it exists parses and validates the json and restores 
// the rules and restores the wrokspace
// A missing file means a fresh workspace; a malformed or inconsistent file causes visible startup failure 
// rather than silently losing configuration or notification history
Application::Application(std::filesystem::path path) : path_(std::move(path)) {
    if (!std::filesystem::exists(path_)) return;
    if (!std::filesystem::is_regular_file(path_) || std::filesystem::file_size(path_) > 32 * 1024 * 1024)
        throw std::runtime_error("State must be a regular JSON file under 32 MiB.");
    std::ifstream input(path_);
    if (!input) throw std::runtime_error("Cannot read state file; it was left unchanged.");
    const auto saved = Json::parse(input);
    if ((saved.at("schema_version") != 1 && saved.at("schema_version") != 2) || !saved.at("rules").is_array())
        throw std::runtime_error("Unsupported state format; it was left unchanged.");
    if (saved.at("schema_version") == 2 && !saved.at("engine").contains("next_notification_id"))
        throw std::runtime_error("Missing next notification ID in schema-2 state.");
    std::vector<QueueRule> rules;
    for (const auto& value : saved.at("rules")) rules.push_back(rule_from_json(value));
    QueueCatalog catalog;
    if (saved.at("schema_version") == 2) {
        if (!saved.at("queues").is_array()) throw std::runtime_error("Invalid queue catalog.");
        std::vector<Queue> queues;
        for (const auto& value : saved.at("queues"))
            queues.push_back({value.at("id").get<std::string>(), value.at("name").get<std::string>()});
        catalog = QueueCatalog(std::move(queues));
    }
    state_.rules = RuleCollection(std::move(rules), std::move(catalog));
    state_.demo_cursor = integer(saved.at("demo_cursor"), "demo cursor");
    state_.engine = NotificationEngine::restore(saved.at("engine").dump(), state_.rules.rules(), state_.rules.catalog());
}

// write the complete proposed state to a temporary file, flush it, and atomically replace the snapshot before 
// publishing it in memory. This avoids partial application writes and keeps failed saves from exposing unpersisted changes.
void Application::commit(State candidate) {
    const std::string payload = encode(candidate).dump(2) + "\n";
    if (payload.size() > 32 * 1024 * 1024)
        throw std::runtime_error("Demo state reached 32 MiB. Archive it before continuing.");
    const auto parent = path_.has_parent_path() ? path_.parent_path() : std::filesystem::path(".");
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
        // Once rename succeeds the commit is published on disk. Do not report
        // rollback for a later directory-sync failure; log the durability limit.
        const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
        if (directory != -1) {
            if (::fsync(directory) != 0) std::cerr << "Warning: state directory sync failed.\n";
            ::close(directory);
        } else std::cerr << "Warning: state directory could not be synced.\n";
    } catch (...) {
        if (fd != -1) ::close(fd);
        std::error_code ignored;
        std::filesystem::remove(pattern, ignored);
        throw;
    }
    state_ = std::move(candidate);
}

// organizes and orders the info for the front end
Json Application::view() const {
    // locks the mutex to stop changes
    const std::lock_guard lock(mutex_);
    Json queues = Json::array(), rules = Json::array(), alerts = Json::array(), notifications = Json::array();

    for (const auto& catalog_queue : state_.rules.catalog().queues()) {
        const auto& id = catalog_queue.id;
        Json queue{{"id", id}, {"name", catalog_queue.name}, {"snapshot", nullptr}};
        if (const auto found = state_.engine.latest_snapshots().find(id); found != state_.engine.latest_snapshots().end())
            queue["snapshot"] = snapshot_json(found->second);
        queues.push_back(queue);
    }
    for (const auto& rule : state_.rules.rules()) {
        auto encoded = rule_json(rule);
        encoded["status"] = !rule.enabled ? "disabled" :
            !state_.engine.latest_snapshots().contains(queue_of(rule)) ? "waiting_for_data" :
            state_.engine.active_notification_id(rule.id) != 0 ? "firing" : "clear";
        rules.push_back(encoded);
        if (encoded["status"] == "firing") {
            const auto& snapshot = state_.engine.latest_snapshots().at(queue_of(rule));
            const bool backlog = std::holds_alternative<BacklogRule>(rule.definition);
            encoded["severity"] = backlog ? "warning" : "critical";
            encoded["observed_value"] = backlog ? snapshot.tickets_waiting : snapshot.longest_wait_sec;
            encoded["threshold"] = backlog ? std::get<BacklogRule>(rule.definition).tickets_waiting_threshold : snapshot.sla_target_sec;
            encoded["observed_at_sec"] = snapshot.observed_at_sec;
            encoded["context"] = snapshot_json(snapshot);
            encoded["notification_id"] = state_.engine.active_notification_id(rule.id);
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
        encoded["severity"] = notification.rule_type == RuleKind::Sla ? "critical" : "warning";
        notifications.push_back(encoded);
    }
    return {{"queues", queues}, {"rules", rules}, {"active_alerts", alerts},
            {"notifications", notifications}, {"demo_cursor", state_.demo_cursor}};
}
// view() builds a consistent read model for the frontend under the application mutex. 
// It combines latest observations, derived rule statuses, prioritized active episodes, 
// and original notification history. The frontend renders those decisions rather than evaluating rules itself.

// Explicit catalog registration; observations cannot silently create queues.
Json Application::create_queue(const Json& input) {
    const std::lock_guard lock(mutex_);
    auto candidate = state_;
    Queue queue{input.at("id").get<std::string>(), input.at("name").get<std::string>()};
    candidate.rules.add_queue(queue);
    commit(std::move(candidate));
    return {{"id", queue.id}, {"name", queue.name}};
}

// Retention is explicit so a routine save never silently deletes user history.
Json Application::compact(const Json& input) {
    const std::lock_guard lock(mutex_);
    if (!input.is_object()) throw DomainError(ErrorCode::InvalidInput, "Retention settings must be an object.");
    const auto history = input.contains("history_limit")
        ? integer(input.at("history_limit"), "history_limit", 0, 10000) : 1000;
    const auto events = input.contains("event_limit")
        ? integer(input.at("event_limit"), "event_limit", 0, 100000) : 10000;
    auto candidate = state_;
    candidate.engine.compact(static_cast<std::size_t>(history), static_cast<std::size_t>(events));
    const auto retained = candidate.engine.notifications().size();
    commit(std::move(candidate));
    return {{"retained_notifications", retained}, {"history_limit", history}, {"event_limit", events}};
}

// lead saves a new rule
Json Application::create_rule(const Json& input) {
    const std::lock_guard lock(mutex_);
    auto candidate = state_;
    const int id = candidate.rules.create(definition_from_json(input));
    if (input.contains("enabled")) candidate.rules.set_enabled(id, input.at("enabled").get<bool>());
    candidate.engine.reevaluate_validated(id, candidate.rules);
    commit(std::move(candidate));
    return {{"id", id}};
}

// Lead edits, enables, or disables a rule
Json Application::update_rule(int id, const Json& input) {
    const std::lock_guard lock(mutex_);
    if (!input.is_object() || (!input.contains("type") && !input.contains("enabled")))
        throw DomainError(ErrorCode::InvalidInput, "Supply rule configuration or enabled state.");
    auto candidate = state_;
    if (input.contains("type")) candidate.rules.edit(id, definition_from_json(input));
    if (input.contains("enabled")) candidate.rules.set_enabled(id, input.at("enabled").get<bool>());
    candidate.engine.reevaluate_validated(id, candidate.rules);
    commit(std::move(candidate));
    return {{"id", id}};
}

// One event arrives
Json Application::ingest(const Json& input) {
    const std::lock_guard lock(mutex_);
    auto candidate = state_;
    const auto result = candidate.engine.process_validated(snapshot_from_json(input), candidate.rules);
    if (result.outcome != SnapshotOutcome::Duplicate) commit(std::move(candidate));
    return result_json(result);
}

// a jsonl file is imported
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
            throw DomainError(ErrorCode::InvalidInput, "JSONL line " + std::to_string(line_number) + ": " + error.what());
        }
    }
    if (events.empty()) throw DomainError(ErrorCode::InvalidInput, "No queue_snapshot events found.");
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
        return a.observed_at_sec < b.observed_at_sec;
    });
    auto candidate = state_;
    const auto before = candidate.engine.notifications().size();
    int processed = 0, duplicates = 0, stale = 0;
    for (const auto& event : events) {
        const auto result = candidate.engine.process_validated(event, candidate.rules);
        if (result.outcome == SnapshotOutcome::Processed) ++processed;
        if (result.outcome == SnapshotOutcome::Duplicate) ++duplicates;
        if (result.outcome == SnapshotOutcome::Stale) ++stale;
    }
    const auto created = candidate.engine.notifications().size() - before;
    commit(std::move(candidate));
    return {{"processed", processed}, {"duplicates", duplicates}, {"stale", stale},
            {"skipped_agent_events", skipped}, {"notifications_created", created}};
}

// Someone clicks “Next demo event”
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
    QueueSnapshot snapshot{"synthetic-demo-" + std::to_string(candidate.demo_cursor),
        timestamp, queue, waiting, oldest, sla};
    // Synthetic context for the demo, not an inference from backlog or SLA.
    snapshot.agents_available = waiting > 20 ? 4 : 6;
    snapshot.agents_on_call = waiting > 20 ? 12 : 10;
    snapshot.volume_last_15m = waiting > 20 ? 78 : 60;
    snapshot.volume_forecast_next_15m = 50;
    const auto result = candidate.engine.process_validated(snapshot, candidate.rules);
    ++candidate.demo_cursor;
    commit(std::move(candidate));
    auto response = result_json(result);
    response["label"] = label;
    return response;
}
} // namespace intraday
