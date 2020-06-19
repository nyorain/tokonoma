#pragma once

#include "data.hpp"
#include "parse.hpp"
#include "common.hpp"
#include <stdexcept>

// The best to/from string conversion api.
// We don't use it everywhere since it's barely implemented in c++ std libs.
#include <charconv>

// TODO: better error reporting/debugging
#include <dlg/dlg.hpp>

namespace qwe {

std::string* asString(Value& value) {
	return std::get_if<std::string>(&value.value);
}
Table* asTable(Value& value) {
	return std::get_if<Table>(&value.value);
}
Vector* asVector(Value& value) {
	return std::get_if<Vector>(&value.value);
}

const std::string* asString(const Value& value) {
	return std::get_if<std::string>(&value.value);
}
const Table* asTable(const Value& value) {
	return std::get_if<Table>(&value.value);
}
const Vector* asVector(const Value& value) {
	return std::get_if<Vector>(&value.value);
}

// enum class AtError {
// 	none,
// 	// table entry does not exist
// 	invalidEntry,
// 	// Table entry does not exist, but can interpret it as integer.
// 	// The integer is out of range of the vector though.
// 	arrayIDOutOfRange,
// };

// TODO: would be useful to return error code from this
const Value* at(const Value& value, std::string_view name) {
	if(name.empty()) {
		return &value;
	}

	auto table = asTable(value);
	if(!table) {
		// check if it can be parsed as index
		auto vec = asVector(value);
		if(!vec) {
			return nullptr;
		}

		size_t id = 0u;
		auto res = std::from_chars(&*name.begin(), &*name.end(), id, 10u);
		if(res.ec != std::errc()) {
			return nullptr;
		}

		// make sure the next character is a separator
		if(res.ptr < &*name.end() && res.ptr[0] != '.') {
			return nullptr;
		}

		if(vec->size() <= id) {
			return nullptr;
		}

		auto rest = name.substr(res.ptr - name.data());
		return at(*(*vec)[id], rest);
	}

	auto p = name.find_first_of('.');
	auto [first, rest] = splitIf(name, p);

	// TODO: with c++20 we don't have to construct a string here.
	auto it = table->find(std::string(first));
	if(it == table->end()) {
		return nullptr;
	}

	return at(*it->second, rest);
}

const Value& atT(const Value& value, std::string_view name) {
	auto r = at(value, name);
	if(!r) {
		throw std::runtime_error("Value does not exist");
	}

	return *r;
}

// Fallback
template<typename T>
struct ValueParser {
	static std::optional<T> call(const Value& value) {
		auto str = asString(value);
		if(!str) {
			return std::nullopt;
		}

		if constexpr(std::is_same_v<T, bool>) {
			if(*str == "1" || *str == "true") return true;
			if(*str == "0" || *str == "false") return false;
			return std::nullopt;
		} else if constexpr(std::is_integral_v<T>) {
			char* end {};
			auto cstr = str->c_str();
			auto v = std::strtoll(cstr, &end, 10u);
			return end == cstr ? std::nullopt : std::optional(T(v));
		} else if constexpr(std::is_floating_point_v<T>) {
			char* end {};
			auto cstr = str->c_str();
			auto v = std::strtold(cstr, &end);
			return end == cstr ? std::nullopt : std::optional(T(v));
		} else {
			static_assert(templatize<T>(false), "Can't parse type");
		}
	}
};

template<>
struct ValueParser<std::string> {
	static std::optional<std::string> call(const Value& value) {
		auto str = asString(value);
		return str ? std::optional(*str) : std::nullopt;
	}
};

template<>
struct ValueParser<std::string_view> {
	static std::optional<std::string_view> call(const Value& value) {
		auto str = asString(value);
		return str ? std::optional(std::string_view(*str)) : std::nullopt;
	}
};

template<typename T>
std::optional<std::vector<T>> asVector(const Vector& vector) {
	std::vector<T> ret;
	ret.reserve(vector.size());
	for(auto& v : vector) {
		auto parsed = ValueParser<T>::call(*v);
		if(!parsed) {
			return std::nullopt;
		}

		ret.emplace_back(std::move(*parsed));
	}

	return ret;
}

template<typename It>
bool readRange(It begin, It end, const Vector& vector,
		bool allowMore = false, bool allowLess = false) {
	for(auto& src : vector) {
		if(begin == end) {
			return false;
		}

		auto& dst = *begin;
		if(!read(dst, *src)) {
			return allowMore;
		}

		++begin;
	}

	if(begin != end) {
		return allowLess;
	}

	return true;
}

template<typename It>
bool readRange(It begin, It end, const Value& value,
		bool allowMore = false, bool allowLess = false) {
	auto* vec = asVector(value);
	if(!vec) {
		return false;
	}

	return readRange(begin, end, *vec, allowMore, allowLess);
}

template<typename T>
struct ValueParser<std::vector<T>> {
	static std::optional<std::vector<T>> call(const Value& value) {
		auto* vec = asVector(value);
		return vec ? asVector<T>(*vec) : std::nullopt;
	}
};

template<typename T, std::size_t N>
struct ValueParser<std::array<T, N>> {
	static std::optional<std::array<T, N>> call(const Value& value) {
		auto* vec = asVector(value);
		if(!vec) {
			return std::nullopt;
		}

		// Not strictly needed, readRange checks it.
		// For better for performance to check it beforehand.
		if(vec->size() != N) {
			return std::nullopt;
		}

		std::array<T, N> res;
		if(!readRange(res.begin(), res.end(), *vec)) {
			return false;
		}

		return {res};
	}
};

template<typename T>
std::optional<T> as(const Value& value) {
	return ValueParser<T>::call(value);
}

template<typename T>
bool read(T& dst, const Value& src) {
	auto opt = as<T>(src);
	if(!opt) {
		return false;
	}

	dst = *opt;
	return true;
}

template<typename T>
std::optional<T> as(const Value& value, std::string_view field) {
	auto v = at(value, field);
	return v ? as<T>(*v) : std::nullopt;
}

// If the field `at` exists in `src` (if at.empty(), src itself is used),
// and can be parsed as type `T`, will store its value in dst and return true.
// Otherwise returns false and does nothing.
template<typename T>
bool read(T& dst, const qwe::Value& src, std::string_view at) {
	auto v = qwe::as<T>(src, at);
	if(!v) {
		return false;
	}

	dst = *v;
	return true;
}

template<typename T>
T asT(const Value& value) {
	auto optT = ValueParser<T>::call(value);
	if(!optT) {
		throw std::runtime_error("Can't parse value");
	}

	return *optT;
}

template<typename T>
void readT(T& dst, const qwe::Value& src) {
	dst = asT<T>(src);
}

template<typename T>
T asT(const Value& src, std::string_view field) {
	auto v = at(src, field);
	if(!v) {
		std::string msg = dlg::format("Invalid Value access: {}", field);
		throw std::runtime_error(msg);
	}

	auto optT = as<T>(*v);
	if(!optT) {
		std::string msg = dlg::format("Can't parse value: {}", field);
		throw std::runtime_error(msg);
	}

	return *optT;
}

template<typename T>
void readT(T& dst, const qwe::Value& src, std::string_view at) {
	dst = asT<T>(src, at);
}

template<typename T>
Value print(const T& val);

template<typename T, typename D = ValueParser<T>>
struct PodParser {
	static std::optional<T> parse(const Value& value) {
		T res;
		auto map = D::map(res);

		bool error = for_each_or(map, [&](auto& entry) {
			auto& v = entry.val;
			using V = std::decay_t<decltype(v)>;

			auto val = at(value, entry.name);
			if(!val) {
				return entry.required;
			}

			auto pv = as<V>(*val);
			if(!pv) {
				return true;
			}

			v = *pv;
			return false;
		});

		if(error) {
			return std::nullopt;
		}

		return res;
	}

	static Value print(const T& val) {
		auto map = D::map(val);
		Table table;
		for_each_or(map, [&](auto& entry) {
			table.emplace(entry.name, print(entry.val));
		});

		return {table};
	}
};


std::string& asStringT(Value& value) {
	return std::get<std::string>(value.value);
}
Table& asTableT(Value& value) {
	return std::get<Table>(value.value);
}
Vector& asVectorT(Value& value) {
	return std::get<Vector>(value.value);
}

const std::string& asStringT(const Value& value) {
	return std::get<std::string>(value.value);
}
const Table& asTableT(const Value& value) {
	return std::get<Table>(value.value);
}
const Vector& asVectorT(const Value& value) {
	return std::get<Vector>(value.value);
}

std::string print(const Error& err) {
	std::string res;
	res.reserve(50);

	// #1 location
	res += "[";

	if(!err.location.nest.empty()) {
		auto sep = "";
		for(auto& n : err.location.nest) {
			res += sep;
			res += n;
			sep = ".";
		}

		res += ", ";
	}

	res += std::to_string(err.location.line);
	res += ":";
	res += std::to_string(err.location.col);
	res += "]: ";

	// #2 message
	switch(err.type) {
		case ErrorType::unexpectedEnd:
			res += "Unexpected input end";
			break;
		case ErrorType::highIndentation:
			res += "Unexpected high indentation level";
			break;
		case ErrorType::duplicateName:
			res += "Duplicate table identifier '";
			res += err.data;
			res += "'";
			break;
		case ErrorType::emptyName:
			res += "Empty name not allowed";
			break;
		case ErrorType::mixedTableArray:
			res += "Mixing table and array not allowed";
			break;
		case ErrorType::emptyTableArray:
			res += "Empty value/table/array not allowed";
			break;
		case ErrorType::none:
			res += "No error. Data: ";
			res += err.data;
			break;
	}

	return res;
}

}
