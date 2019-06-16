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
T read(nytl::Span<std::byte>& span) {
	T ret;
	dlg_assert(std::size_t(span.size()) >= sizeof(ret));
	std::memcpy(&ret, span.data(), sizeof(ret));
	span = span.last(span.size() - sizeof(ret));
	return ret;
}

template<typename T>
T read(nytl::Span<const std::byte>& span) {
	T ret;
	dlg_assert(std::size_t(span.size()) >= sizeof(ret));
	std::memcpy(&ret, span.data(), sizeof(ret));
	span = span.last(span.size() - sizeof(ret));
	return ret;
}

template<typename T>
T& refRead(nytl::Span<std::byte>& span) {
	T ret;
	dlg_assert(std::size_t(span.size()) >= sizeof(ret));
	auto data = span.data();
	span = span.last(span.size() - sizeof(ret));
	return *reinterpret_cast<T*>(data);
}

inline void write(nytl::Span<std::byte>& span, const std::byte* ptr,
		std::size_t size) {
	dlg_assert(std::size_t(span.size()) >= size);
	std::memcpy(span.data(), ptr, size);
	span = span.last(span.size() - size);
}

inline void write(nytl::Span<std::byte>& dst, nytl::Span<const std::byte> src) {
	dlg_assert(dst.size() >= src.size());
	std::memcpy(dst.data(), src.data(), src.size());
	dst = dst.last(dst.size() - src.size());
}

template<typename T>
void write(nytl::Span<std::byte>& span, T&& data) {
	write(span, reinterpret_cast<const std::byte*>(&data), sizeof(data));
}

inline void skip(nytl::Span<std::byte>& span, std::size_t bytes) {
	dlg_assert(std::size_t(span.size()) >= bytes);
	span = span.last(span.size() - bytes);
}

template<typename T>
void write(std::byte*& data, T&& obj) {
	std::memcpy(data, &obj, sizeof(obj));
	data += sizeof(obj);
}

struct DynamicBuffer {
	std::vector<std::byte> buffer;
};

template<typename T>
void write(DynamicBuffer& buffer, T&& obj) {
	buffer.buffer.resize(buffer.buffer.size() + sizeof(obj));
	auto data = buffer.buffer.data() + buffer.buffer.size() - sizeof(obj);
	std::memcpy(data, &obj, sizeof(obj));
}

// TODO: doesn't really fit in here...
template<typename T>
T bit(T value, T bit, bool set) {
	return set ? (value | bit) : (value & ~bit);
}

} // namespace doi
