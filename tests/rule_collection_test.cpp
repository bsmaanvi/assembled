#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "rule_collection.hpp"

namespace {
using namespace intraday;

// Checks throw on failure even in builds with NDEBUG (unlike assert).
void check(bool condition, const std::string& explanation) {
    if (!condition) throw std::runtime_error(explanation);
}

template <typename Exception, typename Action>
void expect_throw(Action action) {
    try {
        action();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error("Expected exception was not thrown");
}

const BacklogRule& backlog(const RuleCollection& collection, std::size_t index = 0) {
    return std::get<BacklogRule>(collection.rules().at(index).definition);
}

// A newly created collection contains no configured rules.
void starts_empty() {
    RuleCollection collection;
    check(collection.rules().empty(), "New collection must be empty");
}

// Creating backlog and SLA rules retains their settings and assigns unique IDs.
// Both rules start enabled, and adding the second keeps the first.
void creates_both_rule_types() {
    RuleCollection collection;
    const int first = collection.create(BacklogRule{"billing", 20});
    const int second = collection.create(SlaBreachRule{"vip"});
    check(first == 1 && second == 2, "IDs must be distinct and sequential");
    check(collection.rules().size() == 2, "Creating a rule must retain earlier rules");
    check(collection.rules()[0].enabled && collection.rules()[1].enabled,
          "New rules must start enabled");
    check(backlog(collection).queue_id == "billing" &&
          backlog(collection).tickets_waiting_threshold == 20,
          "Backlog configuration must be retained");
    check(std::get<SlaBreachRule>(collection.rules()[1].definition).queue_id == "vip",
          "SLA configuration must be retained");
}

// Both rule types can be configured for Billing, Tier 2, and VIP.
void accepts_all_catalog_queues() {
    RuleCollection collection;
    for (const auto* queue : {"billing", "tier_2", "vip"}) {
        collection.create(BacklogRule{queue, 20});
        collection.create(SlaBreachRule{queue});
    }
    check(collection.rules().size() == 6, "Both templates must support every demo queue");
}

// Backlog thresholds accept zero and the largest value an int can hold.
void accepts_threshold_boundaries() {
    RuleCollection collection;
    collection.create(BacklogRule{"billing", 0});
    collection.create(BacklogRule{"vip", std::numeric_limits<int>::max()});
    check(backlog(collection).tickets_waiting_threshold == 0, "Zero is valid");
    check(backlog(collection, 1).tickets_waiting_threshold == std::numeric_limits<int>::max(),
          "Largest representable nonnegative threshold is valid");
}

// Invalid queues or negative thresholds are rejected without adding a rule,
// changing existing settings, or consuming the next rule ID.
void invalid_creation_does_not_change_state_or_consume_id() {
    RuleCollection collection;
    collection.create(BacklogRule{"billing", 20});
    expect_throw<std::invalid_argument>([&] { collection.create(BacklogRule{"billing", -1}); });
    expect_throw<std::invalid_argument>([&] { collection.create(BacklogRule{"unknown", 20}); });
    expect_throw<std::invalid_argument>([&] { collection.create(SlaBreachRule{""}); });
    check(collection.rules().size() == 1, "Invalid creates must not add rules");
    check(backlog(collection).tickets_waiting_threshold == 20, "Existing rule must be unchanged");
    check(collection.create(SlaBreachRule{"vip"}) == 2, "Invalid creates must not consume IDs");
}

// Editing changes only the selected rule's settings, preserving its ID and
// enabled flag and leaving other rules untouched.
void edits_only_selected_rule_and_preserves_metadata() {
    RuleCollection collection;
    const int first = collection.create(BacklogRule{"billing", 20});
    const int second = collection.create(BacklogRule{"vip", 10});
    collection.set_enabled(first, false);
    collection.edit(first, BacklogRule{"tier_2", 25});
    check(collection.rules()[0].id == first && !collection.rules()[0].enabled,
          "Edit must retain ID and disabled state");
    check(backlog(collection).queue_id == "tier_2" &&
          backlog(collection).tickets_waiting_threshold == 25, "Edit must update definition");
    check(collection.rules()[1].id == second && collection.rules()[1].enabled &&
          backlog(collection, 1).tickets_waiting_threshold == 10,
          "Editing one rule must not change another");
}

// A rule can change from backlog to SLA and back without changing its ID
// or adding another rule to the collection.
void switches_rule_type_in_both_directions() {
    RuleCollection collection;
    const int id = collection.create(BacklogRule{"billing", 20});
    collection.edit(id, SlaBreachRule{"vip"});
    check(std::get<SlaBreachRule>(collection.rules()[0].definition).queue_id == "vip",
          "Backlog can become an SLA rule");
    collection.edit(id, BacklogRule{"tier_2", 5});
    check(backlog(collection).tickets_waiting_threshold == 5, "SLA can become backlog");
    check(collection.rules().size() == 1 && collection.rules()[0].id == id,
          "Changing type must not create another rule");
}

// An invalid replacement queue or threshold leaves the original rule unchanged.
void rejects_invalid_edit_without_mutation() {
    RuleCollection collection;
    const int id = collection.create(BacklogRule{"billing", 20});
    expect_throw<std::invalid_argument>([&] { collection.edit(id, BacklogRule{"billing", -5}); });
    expect_throw<std::invalid_argument>([&] { collection.edit(id, SlaBreachRule{"unknown"}); });
    check(backlog(collection).queue_id == "billing" &&
          backlog(collection).tickets_waiting_threshold == 20 && collection.rules()[0].enabled,
          "Rejected edits must preserve the original rule");
}

// Repeated enable/disable calls are safe and affect only the selected rule's
// enabled flag, not its configuration or other rules.
void toggles_are_idempotent_and_isolated() {
    RuleCollection collection;
    const int id = collection.create(BacklogRule{"billing", 20});
    collection.create(SlaBreachRule{"vip"});
    collection.set_enabled(id, false);
    collection.set_enabled(id, false);
    check(!collection.rules()[0].enabled && collection.rules()[1].enabled,
          "Disabling must affect only the selected rule");
    collection.set_enabled(id, true);
    collection.set_enabled(id, true);
    check(collection.rules()[0].enabled && backlog(collection).tickets_waiting_threshold == 20,
          "Enabling must not alter the definition");
}

// Editing or toggling a nonexistent ID throws without changing existing rules.
void unknown_ids_do_not_mutate_rules() {
    RuleCollection collection;
    collection.create(SlaBreachRule{"vip"});
    for (int id : {-1, 0, 99}) {
        expect_throw<std::invalid_argument>([&] { collection.edit(id, BacklogRule{"billing", 10}); });
        expect_throw<std::invalid_argument>([&] { collection.set_enabled(id, false); });
    }
    check(collection.rules().size() == 1 && collection.rules()[0].enabled &&
          std::holds_alternative<SlaBreachRule>(collection.rules()[0].definition),
          "Unknown IDs must leave existing rules untouched");
}

// New IDs follow the highest existing ID, even when initial rules are unsorted
// and their IDs have gaps; existing enabled flags are retained.
void uses_maximum_id_not_vector_position() {
    RuleCollection collection({
        {9, false, BacklogRule{"billing", 20}},
        {2, true, SlaBreachRule{"vip"}},
    });
    check(collection.create(SlaBreachRule{"tier_2"}) == 10,
          "Next ID must follow the highest ID even with unsorted initial rules");
    check(!collection.rules()[0].enabled, "Initial enabled state must be preserved");
}

// Constructing a collection rejects duplicate/nonpositive IDs, unknown queues,
// and negative backlog thresholds.
void rejects_invalid_initial_collections() {
    for (int id : {0, -1}) {
        expect_throw<std::invalid_argument>([&] {
            RuleCollection collection({{id, true, SlaBreachRule{"vip"}}});
        });
    }
    expect_throw<std::invalid_argument>([] {
        RuleCollection collection({{1, true, SlaBreachRule{"vip"}},
                                   {1, false, BacklogRule{"billing", 20}}});
    });
    expect_throw<std::invalid_argument>([] {
        RuleCollection collection({{1, true, SlaBreachRule{"unknown"}}});
    });
    expect_throw<std::invalid_argument>([] {
        RuleCollection collection({{1, true, BacklogRule{"billing", -1}}});
    });
}

// When the largest int ID is already used, creation fails without adding a rule.
void prevents_id_overflow() {
    RuleCollection collection({{std::numeric_limits<int>::max(), true, SlaBreachRule{"vip"}}});
    expect_throw<std::runtime_error>([&] { collection.create(BacklogRule{"billing", 20}); });
    check(collection.rules().size() == 1, "Exhausted IDs must not add a rule");
}

// Editing a candidate copy does not affect the original collection, allowing
// RuleStore to keep its current rules until a save succeeds.
void candidate_changes_do_not_mutate_original() {
    RuleCollection original;
    const int id = original.create(BacklogRule{"billing", 20});
    auto candidate = original;
    candidate.edit(id, SlaBreachRule{"vip"});
    candidate.set_enabled(id, false);
    candidate.create(BacklogRule{"tier_2", 5});
    check(original.rules().size() == 1 && original.rules()[0].enabled &&
          backlog(original).tickets_waiting_threshold == 20,
          "A proposed copy must not change the original before commit");
}
} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"starts empty", starts_empty},
        {"creates both rule types", creates_both_rule_types},
        {"accepts catalog queues", accepts_all_catalog_queues},
        {"accepts threshold boundaries", accepts_threshold_boundaries},
        {"rejects invalid creation without mutation", invalid_creation_does_not_change_state_or_consume_id},
        {"edit preserves metadata and other rules", edits_only_selected_rule_and_preserves_metadata},
        {"switches rule types", switches_rule_type_in_both_directions},
        {"rejects invalid edits without mutation", rejects_invalid_edit_without_mutation},
        {"toggles are idempotent and isolated", toggles_are_idempotent_and_isolated},
        {"rejects unknown IDs", unknown_ids_do_not_mutate_rules},
        {"uses highest existing ID", uses_maximum_id_not_vector_position},
        {"rejects invalid initial collections", rejects_invalid_initial_collections},
        {"prevents ID overflow", prevents_id_overflow},
        {"candidate copy is isolated", candidate_changes_do_not_mutate_original},
    };
    int failures = 0;
    for (const auto& [name, run] : tests) {
        try {
            run();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " unit tests passed\n";
    return failures == 0 ? 0 : 1;
}
