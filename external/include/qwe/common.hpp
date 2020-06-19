#pragma once

#include <variant>
#include <string_view>
#include <cassert>
#include <tuple>

namespace qwe {

template<typename ...Ts>
struct Visitor : Ts...  {
    Visitor(const Ts&... args) : Ts(args)...  {}
    using Ts::operator()...;
};

inline std::pair<std::string_view, std::string_view> split(
		std::string_view src, std::string_view::size_type pos) {
	assert(pos != src.npos && pos < src.size()); // TODO
	auto first = src;
	auto second = src;
	second.remove_prefix(pos + 1);
	first.remove_suffix(src.size() - pos);
	return {first, second};
}

inline std::pair<std::string_view, std::string_view> splitIf(
		std::string_view src, std::string_view::size_type pos) {
	return (pos == src.npos) ? std::pair{src, std::string_view{}} : split(src, pos);
}

template<std::size_t... I, typename F, typename Tuple>
bool for_each_or(std::index_sequence<I...>, Tuple& tup, F&& func) {
	return (func(std::get<I>(tup)) || ...);
}

template<typename F, typename Tuple>
bool for_each_or(Tuple& tup, F&& func) {
	return for_each_or(std::make_index_sequence<std::tuple_size_v<Tuple>>(),
		tup, std::forward<F>(func));
}

template<typename T>
struct MapEntry {
	std::string_view name;
	T& val;
	bool required {};
	bool done {}; // found/printed
};

template<typename T> MapEntry(std::string_view, T&) -> MapEntry<T>;
template<typename T> MapEntry(std::string_view, T&, bool) -> MapEntry<T>;

template<typename T, typename B>
auto templatize(B&& val) {
	return val;
}

} // namespace qwe
