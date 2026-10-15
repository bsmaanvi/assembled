#include <array>
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string>

#include "queue.hpp"
#include "notification_engine.hpp"
#include "rule_store.hpp"

namespace {
const std::array<intraday::Queue, 3> queues{{
    {"billing", "Billing"},
    {"tier_2", "Tier 2 support"},
    {"vip", "VIP"},
}};

struct EndOfInput {};

std::string read_text(const std::string& prompt) {
    std::cout << prompt;
    std::string input;
    if (!std::getline(std::cin, input)) throw EndOfInput{};
    return input;
}

int read_integer(const std::string& prompt) {
    const std::string input = read_text(prompt);
    int value = 0;
    const auto [end, error] =
        std::from_chars(input.data(), input.data() + input.size(), value);
    if (error != std::errc{} || end != input.data() + input.size())
        throw std::invalid_argument("Enter a whole number without extra characters.");
    return value;
}

const intraday::Queue& select_queue() {
    for (std::size_t i = 0; i < queues.size(); ++i)
        std::cout << i + 1 << ". " << queues[i].name << '\n';
    const int selection = read_integer("Select queue (1-3): ");
    if (selection < 1 || selection > 3)
        throw std::invalid_argument("Please choose a queue from 1 to 3.");
    const auto& queue = queues[static_cast<std::size_t>(selection - 1)];
    return queue;
}

intraday::RuleDefinition configure_rule() {
    const auto& queue = select_queue();
    std::cout << "1. Backlog\n2. SLA breach\n";
    const int type = read_integer("Select rule (1-2): ");
    if (type == 1) {
        const int threshold = read_integer("Notify when tickets waiting exceed: ");
        if (threshold < 0) throw std::invalid_argument("Threshold must be nonnegative.");
        return intraday::BacklogRule{queue.id, threshold};
    }
    if (type == 2) return intraday::SlaBreachRule{queue.id};
    throw std::invalid_argument("Please choose rule 1 or 2.");
}

intraday::QueueSnapshot read_snapshot() {
    const auto& queue = select_queue();
    const std::string event = read_text("Event ID: ");
    const std::string time = read_text("Observed at (Unix seconds, e.g. 1779804000): ");
    std::int64_t timestamp = 0;
    const auto [end, error] = std::from_chars(time.data(), time.data() + time.size(), timestamp);
    if (error != std::errc{} || end != time.data() + time.size())
        throw std::invalid_argument("Observation time must be whole Unix seconds.");
    const int waiting = read_integer("Tickets waiting: ");
    const int oldest = read_integer("Oldest ticket wait (seconds): ");
    const int sla = read_integer("SLA target (seconds): ");
    return {event, timestamp, queue.id, waiting, oldest, sla};
}

void print_notification(const intraday::Notification& notification) {
    std::cout << "NOTIFICATION #" << notification.id << " | rule #"
              << notification.rule_id << " | " << notification.queue_id
              << " | observed at " << notification.observed_at_sec
              << " | event " << notification.event_id << '\n'
              << notification.explanation << '\n';
}

void list_rules(const intraday::RuleStore& store) {
    if (store.rules().empty()) {
        std::cout << "No rules configured yet.\n";
        return;
    }
    for (const auto& rule : store.rules()) {
        std::cout << '#' << rule.id << " ["
                  << (rule.enabled ? "enabled" : "disabled") << "] ";
        if (const auto* backlog = std::get_if<intraday::BacklogRule>(&rule.definition)) {
            std::cout << backlog->queue_id << ": more than "
                      << backlog->tickets_waiting_threshold << " tickets waiting\n";
        } else {
            const auto& sla = std::get<intraday::SlaBreachRule>(rule.definition);
            std::cout << sla.queue_id << ": oldest ticket wait exceeds SLA target\n";
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "Usage: intraday-config [rules-file]\n";
        return 1;
    }
    try {
        intraday::RuleStore store(argc == 2 ? argv[1] : "rules.txt");
        intraday::NotificationEngine engine;
        std::cout << "Team lead: queue notification configuration\n"
                  << "Rules are saved immediately. Notifications are session-only.\n";
        while (true) {
            std::cout << "\n1. Create rule\n2. List rules\n3. Edit rule\n"
                      << "4. Enable rule\n5. Disable rule\n6. Exit\n"
                      << "7. Submit queue snapshot\n8. Notification inbox\n";
            try {
                const int action = read_integer("Choose action: ");
                switch (action) {
                    case 1: {
                        const int id = store.create(configure_rule());
                        engine.reevaluate(id, store.rules());
                        std::cout << "Saved rule #" << id << " (enabled).\n";
                        break;
                    }
                    case 2: list_rules(store); break;
                    case 3: {
                        list_rules(store);
                        const int id = read_integer("Rule ID to edit: ");
                        bool found = false;
                        for (const auto& rule : store.rules()) found |= rule.id == id;
                        if (!found) throw std::invalid_argument("No rule with that ID.");
                        store.edit(id, configure_rule());
                        engine.reevaluate(id, store.rules());
                        std::cout << "Saved changes to rule #" << id << ".\n";
                        break;
                    }
                    case 4:
                    case 5: {
                        const int id = read_integer("Rule ID: ");
                        store.set_enabled(id, action == 4);
                        engine.reevaluate(id, store.rules());
                        std::cout << "Saved rule #" << id
                                  << (action == 4 ? " as enabled.\n" : " as disabled.\n");
                        break;
                    }
                    case 6: return 0;
                    case 7: {
                        const auto result = engine.process(read_snapshot(), store.rules());
                        if (result.outcome == intraday::SnapshotOutcome::Duplicate)
                            std::cout << "Duplicate event ignored.\n";
                        else if (result.outcome == intraday::SnapshotOutcome::Stale)
                            std::cout << "Older observation ignored.\n";
                        else if (result.new_notifications.empty())
                            std::cout << "Snapshot processed; no new notifications.\n";
                        for (const auto& notification : result.new_notifications)
                            print_notification(notification);
                        break;
                    }
                    case 8:
                        if (engine.notifications().empty()) std::cout << "No notifications yet.\n";
                        for (const auto& notification : engine.notifications()) print_notification(notification);
                        break;
                    default: throw std::invalid_argument("Choose an action from 1 to 8.");
                }
            } catch (const std::exception& error) {
                std::cerr << "Not applied: " << error.what() << '\n';
            }
        }
    } catch (const EndOfInput&) {
        std::cout << "\nSession ended. Previously saved rules are retained.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Could not open configuration: " << error.what() << '\n';
        return 1;
    }
}
