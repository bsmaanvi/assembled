#pragma once

#include <filesystem>
#include <mutex>

#include "json_codec.hpp"
#include "rule_collection.hpp"

namespace intraday {

// The web application's single writer: propose a complete state, save it,
// then publish it in memory. Reads and writes share the same mutex.
class Application {
public:
    explicit Application(std::filesystem::path path);
    Json view() const;
    Json create_rule(const Json& input);
    Json update_rule(int id, const Json& input);
    Json ingest(const Json& input);
    Json replay(const std::string& jsonl);
    Json next_demo();

private:
    struct State {
        RuleCollection rules;
        NotificationEngine engine;
        std::int64_t demo_cursor = 0;
    };
    void commit(State candidate);
    static Json encode(const State& state);
    std::filesystem::path path_;
    State state_;
    mutable std::mutex mutex_;
};
} // namespace intraday
