#pragma once

#include <nytl/stringParam.hpp>
#include <nytl/vec.hpp>
#include <nytl/math.hpp>
#include <dlg/dlg.hpp>
#include <cstdlib>
#include <string>
#include <fstream>
#include <algorithm>
#include <cmath>

// file: general utility that hasn't found a better place yet (but
// likely needs once)

namespace tkn {

constexpr auto fullLumEfficacy = 683.f;
constexpr auto f16Scale = 0.00001f;

template<typename T>
bool stoi(nytl::StringParam string, T& val, unsigned base = 10) {
	char* end {};
	auto str = string.c_str();
	auto v = std::strtoll(str, &end, base);
	if(end == str) {
		return false;
	}

	val = v;
	return true;
}

// Expects: height, width > 0
// Uses clampToEdge
template<typename T>
T bilerp(float s, float t, unsigned width, unsigned height, T* vals) {
	s *= (width - 1);
	t *= (height - 1);

	unsigned s0 = std::floor(s);
	unsigned t0 = std::floor(t);

	unsigned x0 = std::clamp(s0, 0u, width - 1u);
	unsigned x1 = std::clamp(s0 + 1, 0u, width - 1u);
	unsigned y0 = std::clamp(t0, 0u, height - 1u);
	unsigned y1 = std::clamp(t0 + 1, 0u, height - 1u);

	float fx = s - s0;
	float fy = t - t0;
	return nytl::mix(
		nytl::mix(vals[y0 * width + x0], vals[y0 * width + x1], fx),
		nytl::mix(vals[y1 * width + x0], vals[y1 * width + x1], fx), fy);
}

// S1, S2 are expected to be string-like types.
template<typename C, typename CT>
inline bool hasSuffix(std::basic_string_view<C, CT> str,
		std::basic_string_view<C, CT> suffix) {
    return str.size() >= suffix.size() &&
        str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// case-insensitive char traits
// see https://stackoverflow.com/questions/11635
struct CharTraitsCI : public std::char_traits<char> {
    static bool eq(char c1, char c2) { return toupper(c1) == toupper(c2); }
    static bool ne(char c1, char c2) { return toupper(c1) != toupper(c2); }
    static bool lt(char c1, char c2) { return toupper(c1) <  toupper(c2); }
    static int compare(const char* s1, const char* s2, size_t n) {
        while(n-- != 0) {
            if(toupper(*s1) < toupper(*s2)) return -1;
            if(toupper(*s1) > toupper(*s2)) return 1;
            ++s1; ++s2;
        }
        return 0;
    }
    static const char* find(const char* s, int n, char a) {
        while(n-- > 0 && toupper(*s) != toupper(a)) {
            ++s;
        }
        return s;
    }
};

// case-insensitive
inline bool hasSuffixCI(std::string_view cstr, std::string_view csuffix) {
	using CIView = std::basic_string_view<char, CharTraitsCI>;
	auto str = CIView(cstr.data(), cstr.size());
	auto suffix = CIView(csuffix.data(), csuffix.size());
	return hasSuffix(str, suffix);
}

// Splits the given string view at the given position.
// None of the returned strings will have the seperator.
inline std::pair<std::string_view, std::string_view> split(
		std::string_view src, std::string_view::size_type pos) {
	dlg_assert(pos != src.npos && pos < src.size());
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

/// Sets/Unsets the given bit in the given bitfield.
template<typename T>
T bit(T value, T bit, bool set) {
	return set ? (value | bit) : (value & ~bit);
}

// Returns ceil(num / denom), efficiently, only using integer division.
inline constexpr unsigned ceilDivide(unsigned num, unsigned denom) {
	return (num + denom - 1) / denom;
}

inline std::string readFile(nytl::StringParam filename) {
	auto openmode = std::ios::ate;
	std::ifstream ifs(filename.c_str(), openmode);
	ifs.exceptions(std::ostream::failbit | std::ostream::badbit);

	auto size = ifs.tellg();
	ifs.seekg(0, std::ios::beg);

	std::string buffer;
	buffer.resize(size);
	auto data = reinterpret_cast<char*>(buffer.data());
	ifs.read(data, size);

	return buffer;
}

std::string_view skipWhitespace(std::string_view source) {
	constexpr auto whitespace = "\n\t\f\r\v "; // as by std::isspace
	auto fnws = source.find_first_not_of(whitespace);
	if(fnws == source.npos) {
		return {};
	}

	return source.substr(fnws);
}

} // namespace tkn
