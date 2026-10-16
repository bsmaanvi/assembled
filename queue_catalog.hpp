#pragma once
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>
#include <utility>

#include "errors.hpp"
#include "queue.hpp"

namespace intraday {
// The default queues are defined once. Rule/event validation and UI use this catalog.
class QueueCatalog {
public:
    QueueCatalog() : QueueCatalog(std::vector<Queue>{
        {"billing", "Billing"}, {"tier_2", "Tier 2 support"}, {"vip", "VIP"}}) {}
    explicit QueueCatalog(std::vector<Queue> queues) {
        for (auto& queue : queues) add(std::move(queue));
    }
    const std::vector<Queue>& queues() const { return queues_; }
    bool contains(const std::string& id) const { return ids_.contains(id); }
    void add(Queue queue) {
        if (queue.id.empty() || queue.id.size() > 64 ||
            !std::isalnum(static_cast<unsigned char>(queue.id.front())))
            throw DomainError(ErrorCode::InvalidInput, "Queue ID must start with a lowercase letter or digit and contain 1–64 characters.");
        for (unsigned char character : queue.id)
            if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_' || character == '-'))
                throw DomainError(ErrorCode::InvalidInput, "Queue ID may contain lowercase letters, digits, underscores, and hyphens.");
        if (queue.name.empty() || queue.name.size() > 80 || queue.name.find_first_not_of(" \t\r\n") == std::string::npos)
            throw DomainError(ErrorCode::InvalidInput, "Queue name must contain 1–80 characters and cannot be blank.");
        for (unsigned char character : queue.name)
            if (character < 32 || character == 127)
                throw DomainError(ErrorCode::InvalidInput, "Queue name cannot contain control characters.");
        if (contains(queue.id)) throw DomainError(ErrorCode::Conflict, "Queue ID already exists.");
        queues_.push_back(std::move(queue));
        ids_.insert(queues_.back().id);
    }
private:
    std::vector<Queue> queues_;
    std::unordered_set<std::string> ids_;
};
} // namespace intraday
