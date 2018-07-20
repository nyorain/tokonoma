#pragma once

#include <cstddef>
#include <cstring>
#include <dlg/dlg.hpp>
#include <nytl/span.hpp>

namespace doi {

template<typename T>
T read(const std::byte*& data) {
	T ret;
	std::memcpy(&ret, data, sizeof(ret));
	data += sizeof(ret);
	return ret;
}

template<typename T>
void write(nytl::Span<std::byte>& span, T&& data) {
	dlg_assert(span.size() >= sizeof(data));
	std::memcpy(span.data(), &data, sizeof(data));
	span = span.slice(sizeof(data), span.size() - sizeof(data));
}

template<typename T>
void write(std::byte*& data, T&& obj) {
	std::memcpy(data, &obj, sizeof(obj));
	data += sizeof(obj);
}

} // namespace doi
