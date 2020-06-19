#pragma once

#include <string_view>
#include <string>
#include <utility>
#include <vector>
#include <memory>
#include <variant>
#include <unordered_map>

namespace qwe {

// Simple but full c++ representation of a config file.
// The top-level object (the whole config file) is just a Value.
struct Value;

using Table = std::unordered_map<std::string, std::unique_ptr<Value>>;
using Vector = std::vector<std::unique_ptr<Value>>;

struct Value {
	std::variant<std::string, Vector, Table> value;
};

} // namespace qwe
