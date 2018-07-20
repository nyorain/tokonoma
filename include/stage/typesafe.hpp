#pragma once

#include <functional>
#include <ostream>

namespace doi {

/// Typesafe (tagged) wrapper around a value of type 'T'
template<typename Tag, typename T>
struct Typesafe {
	T value;

	constexpr Typesafe& operator++() { ++value; return *this; }
	constexpr Typesafe operator++(int) { auto cpy = *this; ++value; return cpy; }
	constexpr Typesafe& operator--() { --value; return *this; }
	constexpr Typesafe operator--(int) { auto cpy = *this; --value; return cpy; }
};

template<typename Tag, typename T>
constexpr bool operator==(const Typesafe<Tag, T>& a, const Typesafe<Tag, T>& b) {
	return a.value == b.value;
}

template<typename Tag, typename T>
constexpr bool operator!=(const Typesafe<Tag, T>& a, const Typesafe<Tag, T>& b) {
	return a.value != b.value;
}

template<typename Tag, typename T>
std::ostream& operator<<(std::ostream& os, const Typesafe<Tag, T>& a) {
	return (os << a.value);
}

} // namespace doi

// hash specialization
namespace std {

template<typename Tag, typename T>
struct hash<doi::Typesafe<Tag, T>> {
	constexpr auto operator()(const doi::Typesafe<Tag, T>& obj) const {
		return std::hash<T>()(obj.value);
    }
};

} // namespace std
