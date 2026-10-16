#pragma once
#include <stdexcept>
#include <string>

namespace intraday {
enum class ErrorCode { InvalidInput, NotFound, Conflict };

// Domain errors carry HTTP-independent categories; messages are for people.
class DomainError : public std::invalid_argument {
public:
    DomainError(ErrorCode code, const std::string& message, int existing_rule = 0)
        : std::invalid_argument(message), code(code), existing_rule_id(existing_rule) {}
    ErrorCode code;
    int existing_rule_id;
};
} // namespace intraday
