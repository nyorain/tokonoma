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

// TODO: remove the non-const version.
// Only needed due to missing Span 'nonconst -> const' constructor
template<typename T>
T read(nytl::Span<std::byte>& span) {
	T ret;
	dlg_assert(span.size() >= sizeof(ret));
	std::memcpy(&ret, span.data(), sizeof(ret));
	span = span.slice(sizeof(ret), span.size() - sizeof(ret));
	return ret;
}

template<typename T>
T read(nytl::Span<const std::byte>& span) {
	T ret;
	dlg_assert(span.size() >= sizeof(ret));
	std::memcpy(&ret, span.data(), sizeof(ret));
	span = span.slice(sizeof(ret), span.size() - sizeof(ret));
	return ret;
}

inline void write(nytl::Span<std::byte>& span, const std::byte* ptr,
		std::size_t size) {
	dlg_assert(span.size() >= size);
	std::memcpy(span.data(), ptr, size);
	span = span.slice(size, span.size() - size);
}

template<typename T>
void write(nytl::Span<std::byte>& span, T&& data) {
	write(span, reinterpret_cast<const std::byte*>(&data), sizeof(data));
}

inline void skip(nytl::Span<std::byte>& span, std::size_t bytes) {
	dlg_assert(span.size() >= bytes);
	span = span.slice(bytes, span.size() - bytes);
}

template<typename T>
void write(std::byte*& data, T&& obj) {
	std::memcpy(data, &obj, sizeof(obj));
	data += sizeof(obj);
}

} // namespace doi
