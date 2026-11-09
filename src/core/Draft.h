// A draft is one version of a file's result text with the decisions that produced it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace branch_truce {

enum class DecisionValue { Undecided, Accepted, Ignored };
struct Decision {
    DecisionValue value = DecisionValue::Undecided;
    // Explicit combination order, supplied by the editing command. Only an
    // accepted decision has one.
    std::optional<std::uint64_t> order;
    bool operator==(const Decision &) const = default;
};

struct Draft {
    std::string text;
    std::map<std::string, Decision> decisions;
    // FileSession::markers().count() for this text. FileSession recomputes it
    // for every revision it stores and ignores a supplied value.
    std::size_t unresolvedMarkers = 0;
    bool operator==(const Draft &) const = default;
};

}
