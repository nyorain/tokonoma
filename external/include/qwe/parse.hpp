#pragma once

#include "common.hpp"
#include "data.hpp"

namespace qwe {

struct Location {
	unsigned line {};
	unsigned col {};
	std::vector<std::string_view> nest {};
};

struct Parser {
	std::string_view input;
	Location location {};
};

enum class ErrorType {
	none,
	unexpectedEnd,
	highIndentation,
	duplicateName,
	emptyName,
	mixedTableArray,
	emptyTableArray
};

struct Error {
	ErrorType type;
	Location location;
	std::string_view data {}; // dependent on 'type'
};

struct NamedValue {
	Value value;
	std::string_view name {}; // optional, might be empty
};

using ParseResult = std::variant<NamedValue, Error>;

ParseResult parse(Parser& parser);
ParseResult parseTableOrArray(Parser& parser) {
	using std::move;

	std::optional<bool> isTable;
	Table table;
	Vector vector;
	while(!parser.input.empty()) {
		auto after = parser.input;

		auto first = after.find_first_not_of('\t');
		if(first == after.npos) {
			parser.location.col += first;
			parser.input = {}; // reached end of document
			break;
		}

		// Comment, skip to next line.
		// Notice how this comes before the indent check.
		// Comments don't have to be aligned, we don't care.
		if(after[first] == '#') {
			auto nl = after.find('\n');
			if(nl == after.npos) {
				// reached end of document
				parser.location.col += parser.input.size();
				parser.input = {};
				break;
			}

			++parser.location.line;
			parser.location.col = 0u;
			parser.input = after.substr(nl + 1);
			continue;
		}

		// Empty lines are also always allowed
		if(after[first] == '\n') {
			++parser.location.line;
			parser.location.col = 0u;
			parser.input = after.substr(first + 1);
			continue;
		}

		// indentation is suddenly too high
		if(first > parser.location.nest.size()) {
			return Error{ErrorType::highIndentation, parser.location};
		}

		// indentation is too low, line does not belong to this value anymore
		if(first < parser.location.nest.size()) {
			break;
		}

		parser.location.col += first;
		parser.input = after.substr(first);
		if(parser.input.empty()) {
			return Error{ErrorType::unexpectedEnd, parser.location};
		}

		// NOTE: extended array-nest syntax
		if(parser.input[0] == '-' && parser.input[1] == '\n') {
			if(isTable && *isTable) {
				return Error{ErrorType::mixedTableArray, parser.location};
			}

			parser.input = parser.input.substr(2);

			parser.location.col = 0u;
			++parser.location.line;
			parser.location.nest.push_back(std::to_string(vector.size()));
			isTable = {false};

			auto res = parseTableOrArray(parser);
			if(auto err = std::get_if<Error>(&res)) {
				return {*err};
			}

			parser.location.nest.pop_back();
			auto& nv = std::get<NamedValue>(res);
			vector.push_back(std::make_unique<Value>(std::move(nv.value)));
			continue;
		}

		auto ploc = parser.location; // save it for later
		auto res = parse(parser);
		if(auto err = std::get_if<Error>(&res)) {
			return {*err};
		}

		auto& nv = std::get<NamedValue>(res);
		if(isTable && *isTable == nv.name.empty()) {
			return Error{ErrorType::mixedTableArray, ploc};
		}

		auto v = std::make_unique<Value>(move(nv.value));
		if(nv.name.empty()) {
			isTable = {false};
			vector.push_back(move(v));
		} else {
			isTable = {true};
			if(!table.emplace(nv.name, std::move(v)).second) {
				return Error{ErrorType::duplicateName, ploc, nv.name};
			}
		}
	}

	if(!isTable) {
		return Error{ErrorType::mixedTableArray, parser.location};
	}

	NamedValue nv;
	nv.value = *isTable ? Value{{move(table)}} : Value{{move(vector)}};
	return nv;
}

ParseResult parse(Parser& parser) {
	using std::move;
	// constexpr auto whitespace = "\n\t\f\r\v "; // as by std::isspace

	if(parser.input.empty()) {
		return Error{ErrorType::unexpectedEnd, parser.location};
	}

	auto nl = parser.input.find("\n");
	auto line = parser.input;
	auto after = std::string_view {};
	Location afterLoc = parser.location;
	if(nl != parser.input.npos) {
		line = parser.input.substr(0, nl);
		after = parser.input.substr(nl + 1);

		afterLoc.col = 0u;
		++afterLoc.line;
	}

	auto sep = line.find(":");
	if(sep == parser.input.npos) {
		parser.input = after;
		parser.location = afterLoc;
		return NamedValue{Value{std::string(line)}};
	}

	auto [name, val] = split(line, sep);

	// Strip all beginning whitespace from 'val' and all ending whitespace
	// from 'name'.
	// auto vws = val.find_first_not_of(whitespace);
	// if(vws != val.npos) {
	// 	val.remove_prefix(vws);
	// }

	// auto nws = name.find_last_not_of(whitespace);
	// if(nws != val.npos) {
	// 	name = name.substr(0, nws + 1);
	// }

	if(!val.empty() && val[0] == ' ') {
		val.remove_prefix(1);
	}

	if(name.empty()) {
		return Error{ErrorType::emptyName, parser.location};
	}

	if(!val.empty()) { // table assignment
		parser.input = after;
		parser.location = afterLoc;
		return NamedValue{Value{std::string(val)}, name};
	}

	// if it's neither a table assignment or an array value,
	// we have a nested table.
	parser.input = after;
	parser.location = afterLoc;
	parser.location.nest.push_back(name);

	auto res = parseTableOrArray(parser);
	assert(parser.location.nest.back() == name);
	parser.location.nest.pop_back();

	if(auto err = std::get_if<Error>(&res)) {
		return *err;
	}

	auto& nv = std::get<NamedValue>(res);
	assert(nv.name.empty());

	nv.name = name;
	return {std::move(nv)};
}

} // namespace qwe
