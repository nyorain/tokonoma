#pragma once

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <dlg/dlg.hpp>
#include <nytl/span.hpp>

namespace tkn {

template<typename T>
std::enable_if_t<std::is_standard_layout_v<T>>
read(T& dst, const std::byte*& data) {
	std::memcpy(&dst, data, sizeof(dst));
	data += sizeof(dst);
}

template<typename T>
std::enable_if_t<std::is_standard_layout_v<T>>
read(T& dst, nytl::Span<std::byte>& span) {
	dlg_assert(std::size_t(span.size()) >= sizeof(dst));
	std::memcpy(&dst, span.data(), sizeof(dst));
	span = span.last(span.size() - sizeof(dst));
}

template<typename T>
std::enable_if_t<std::is_standard_layout_v<T>>
read(T& dst, nytl::Span<const std::byte>& span) {
	dlg_assert(std::size_t(span.size()) >= sizeof(dst));
	std::memcpy(&dst, span.data(), sizeof(dst));
	span = span.last(span.size() - sizeof(dst));
}

template<typename T>
std::enable_if_t<std::is_standard_layout_v<T>, T>
read(const std::byte*& data) {
	T ret;
	read(ret, data);
	return ret;
}

template<typename T>
std::enable_if_t<std::is_standard_layout_v<T>, T>
read(nytl::Span<std::byte>& span) {
	T ret;
	read(ret, span);
	return ret;
}

template<typename T>
std::enable_if_t<std::is_standard_layout_v<T>, T>
read(nytl::Span<const std::byte>& span) {
	T ret;
	read(ret, span);
	return ret;
}

template<typename T>
std::enable_if_t<std::is_standard_layout_v<T>, T&>
refRead(nytl::Span<std::byte>& span) {
	T ret;
	dlg_assert(std::size_t(span.size()) >= sizeof(ret));
	auto data = span.data();
	span = span.last(span.size() - sizeof(ret));
	return *reinterpret_cast<T*>(data);
}

inline std::size_t write(nytl::Span<std::byte>& span, const std::byte* ptr,
		std::size_t size) {
	dlg_assert(std::size_t(span.size()) >= size);
	std::memcpy(span.data(), ptr, size);
	span = span.last(span.size() - size);
	return size;
}

inline std::size_t write(nytl::Span<std::byte>& dst,
		nytl::Span<const std::byte> src) {
	dlg_assert(dst.size() >= src.size());
	std::memcpy(dst.data(), src.data(), src.size());
	dst = dst.last(dst.size() - src.size());
	return src.size();
}

template<typename T>
std::enable_if_t<std::is_standard_layout_v<T>, std::size_t>
write(nytl::Span<std::byte>& span, const T& data) {
	return write(span, reinterpret_cast<const std::byte*>(&data), sizeof(data));
}

inline void skip(nytl::Span<std::byte>& span, std::size_t bytes) {
	dlg_assert(std::size_t(span.size()) >= bytes);
	span = span.last(span.size() - bytes);
}

template<typename T>
std::enable_if_t<std::is_standard_layout_v<T>>
write(std::byte*& data, const T& obj) {
	std::memcpy(data, &obj, sizeof(obj));
	data += sizeof(obj);
}

struct WriteBuffer {
	std::vector<std::byte> buffer;
};

template<typename T>
struct IsInitializerList : public std::false_type {};

template<typename T>
struct IsInitializerList<std::initializer_list<T>> : public std::true_type {};

template<typename T> constexpr auto BytesConvertible =
	std::is_trivially_copyable_v<T> &&
	std::is_standard_layout_v<T> &&
	!IsInitializerList<T>::value;

// TODO: we probably can't assume this (e.g. for span).
// Not sure if overload resolution works if this isn't true
static_assert(!BytesConvertible<std::vector<int>>);
static_assert(!BytesConvertible<nytl::Span<std::byte>>);

template<typename T>
std::enable_if_t<BytesConvertible<T>>
write(WriteBuffer& buffer, const T& obj) {
	buffer.buffer.resize(buffer.buffer.size() + sizeof(obj));
	auto data = buffer.buffer.data() + buffer.buffer.size() - sizeof(obj);
	std::memcpy(data, &obj, sizeof(obj));
}

template<typename T>
std::enable_if_t<BytesConvertible<T>, nytl::Span<const std::byte>>
bytes(const T& val) {
	return {reinterpret_cast<const std::byte*>(&val), sizeof(val)};
}

template<typename T>
std::enable_if_t<BytesConvertible<T>, nytl::Span<std::byte>>
bytes(T& val) {
	return {reinterpret_cast<std::byte*>(&val), sizeof(val)};
}

template<typename T>
nytl::Span<const std::byte> bytes(const std::vector<T>& val) {
	static_assert(BytesConvertible<T>);
	return nytl::as_bytes(nytl::span(val));
}

template<typename T>
nytl::Span<std::byte> bytes(std::vector<T>& val) {
	static_assert(BytesConvertible<T>);
	return nytl::as_writeable_bytes(nytl::span(val));
}

// There is no non-const overload for initializer list since we can't
// ever modify data in it.
template<typename T>
nytl::Span<const std::byte> bytes(const std::initializer_list<T>& val) {
	static_assert(BytesConvertible<T>);
	return nytl::as_bytes(nytl::span(val));
}


} // namespace tkn
