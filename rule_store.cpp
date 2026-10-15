#include "rule_store.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <unistd.h>

namespace intraday {
RuleStore::RuleStore(std::filesystem::path path) : path_(std::move(path)) { load(); }
const std::vector<QueueRule>& RuleStore::rules() const { return collection_.rules(); }

void RuleStore::load() {
    if (!std::filesystem::exists(path_)) return;
    if (!std::filesystem::is_regular_file(path_))
        throw std::runtime_error("Rule storage must be a regular file.");
    std::ifstream input(path_);
    if (!input) throw std::runtime_error("Cannot read rule storage.");
    std::string line;
    if (!std::getline(input, line) || line != "INTRADAY_RULES_V1")
        throw std::runtime_error("Unrecognized rule file. File left unchanged.");
    std::vector<QueueRule> loaded;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        int id = 0, enabled = 0;
        std::string type, queue;
        if (!(row >> id >> enabled >> type >> std::quoted(queue)) ||
            (enabled != 0 && enabled != 1))
            throw std::runtime_error("Invalid rule record. File left unchanged.");
        RuleDefinition definition;
        if (type == "backlog") {
            int threshold = 0;
            if (!(row >> threshold)) throw std::runtime_error("Invalid backlog threshold.");
            definition = BacklogRule{queue, threshold};
        } else if (type == "sla") {
            definition = SlaBreachRule{queue};
        } else {
            throw std::runtime_error("Unknown rule type.");
        }
        row >> std::ws;
        if (!row.eof()) throw std::runtime_error("Unexpected rule fields.");
        loaded.push_back({id, enabled == 1, std::move(definition)});
    }
    if (input.bad()) throw std::runtime_error("Failed while reading rule storage.");
    collection_ = RuleCollection(std::move(loaded));
}

void RuleStore::commit(RuleCollection candidate) {
    // Same-directory temporary file allows atomic replacement on this platform.
    std::string pattern = path_.string() + ".tmp.XXXXXX";
    const int fd = ::mkstemp(pattern.data());
    if (fd == -1) throw std::runtime_error("Cannot create temporary rule file.");
    ::close(fd);
    const std::filesystem::path temporary(pattern);
    try {
        std::ofstream output(temporary);
        if (!output) throw std::runtime_error("Cannot open temporary rule file.");
        output << "INTRADAY_RULES_V1\n";
        for (const auto& rule : candidate.rules()) {
            output << rule.id << ' ' << (rule.enabled ? 1 : 0) << ' ';
            if (const auto* backlog = std::get_if<BacklogRule>(&rule.definition)) {
                output << "backlog " << std::quoted(backlog->queue_id)
                       << ' ' << backlog->tickets_waiting_threshold;
            } else {
                output << "sla " << std::quoted(std::get<SlaBreachRule>(rule.definition).queue_id);
            }
            output << '\n';
        }
        output.close();
        if (!output) throw std::runtime_error("Could not finish saving rules.");
        std::filesystem::rename(temporary, path_);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    collection_ = std::move(candidate);
}

int RuleStore::create(RuleDefinition definition) {
    auto candidate = collection_;
    const int id = candidate.create(std::move(definition));
    commit(std::move(candidate));
    return id;
}

void RuleStore::edit(int id, RuleDefinition definition) {
    auto candidate = collection_;
    candidate.edit(id, std::move(definition));
    commit(std::move(candidate));
}

void RuleStore::set_enabled(int id, bool enabled) {
    auto candidate = collection_;
    candidate.set_enabled(id, enabled);
    commit(std::move(candidate));
}

} // namespace intraday
