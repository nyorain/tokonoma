#pragma once

#include <nytl/stringParam.hpp>
#include <cstdlib>

// file: general utility that hasn't found a better place yet

namespace doi {

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

} // namespace doi
