#pragma once

#include <tinygltf.hpp>
#include <cstdint>
#include <nytl/vec.hpp>

// various gltf helpers (currently using tinygltf)

namespace doi {

/// Throws std::runtime_error if componentType is not a valid gltf component type
/// Does not check for bounds of address
inline double read(const tinygltf::Buffer& buf, unsigned address,
		unsigned componentType) {
	double v;
	auto t = componentType;
	if(t == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
		v = *reinterpret_cast<const std::uint8_t*>(&buf.data[address]);
	} else if(t == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
		v = *reinterpret_cast<const std::uint32_t*>(&buf.data[address]);
	} else if(t == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
		v = *reinterpret_cast<const std::uint16_t*>(&buf.data[address]);
	} else if(t == TINYGLTF_COMPONENT_TYPE_SHORT) {
		v = *reinterpret_cast<const std::int16_t*>(&buf.data[address]);
	} else if(t == TINYGLTF_COMPONENT_TYPE_BYTE) {
		v = *reinterpret_cast<const std::int8_t*>(&buf.data[address]);
	} else if(t == TINYGLTF_COMPONENT_TYPE_INT) {
		v = *reinterpret_cast<const std::int32_t*>(&buf.data[address]);
	} else if(t == TINYGLTF_COMPONENT_TYPE_FLOAT) {
		v = *reinterpret_cast<const float*>(&buf.data[address]);
	} else if(t == TINYGLTF_COMPONENT_TYPE_DOUBLE) {
		v = *reinterpret_cast<const double*>(&buf.data[address]);
	} else {
		throw std::runtime_error("Invalid gltf component type");
	}

	return v;
}

// Reads as array
template<std::size_t N, typename T = float>
auto read(const tinygltf::Buffer& buf, unsigned address, unsigned type,
		unsigned componentType, T fill = T(0)) {
	std::array<T, N> vals;

	// NOTE: not really bytes though
	unsigned components = tinygltf::GetTypeSizeInBytes(type);
	unsigned valSize = tinygltf::GetComponentSizeInBytes(componentType);
	for(auto i = 0u; i < components; ++i) {
		if(i < components) {
			vals[i] = read(buf, address, componentType);
			address += valSize;
		} else {
			vals[i] = fill;
		}
	}

	return vals;
}

template<std::size_t N, typename T>
struct AccessorIterator {
	static_assert(N > 0);
	using Value = std::conditional_t<N == 1, T, nytl::Vec<N, T>>;

	const tinygltf::Buffer* buffer {};
	std::size_t address {};
	std::size_t stride {};
	unsigned type {};
	unsigned componentType {};

	AccessorIterator(const tinygltf::Model& model,
			const tinygltf::Accessor& accessor) {
		auto& bv = model.bufferViews[accessor.bufferView];
		buffer = &model.buffers[bv.buffer];
		address = accessor.byteOffset + bv.byteOffset;
		stride = accessor.ByteStride(bv);
		type = accessor.type;
		componentType = accessor.componentType;
	}

	AccessorIterator operator+(int value) const {
		auto cpy = *this;
		cpy.address += value * stride;
		return cpy;
	}

	AccessorIterator& operator+=(int value) {
		address += value * stride;
		return *this;
	}

	AccessorIterator operator-(int value) const {
		auto cpy = *this;
		cpy.address -= value * stride;
		return cpy;
	}

	AccessorIterator& operator-=(int value) {
		address -= value * stride;
		return *this;
	}

	AccessorIterator& operator++() {
		address += stride;
		return *this;
	}

	AccessorIterator operator++(int) {
		auto copy = *this;
		address += stride;
		return copy;
	}

	AccessorIterator& operator--() {
		address -= stride;
	}

	AccessorIterator operator--(int) {
		auto copy = *this;
		address -= stride;
		return copy;
	}

	Value operator*() const {
		return convert(read<N, T>(*buffer, address, type, componentType));
	}

	Value convert(const std::array<T, N>& val) const {
		Value ret;
		std::memcpy(&ret, val.data(), val.size() * sizeof(T));
		return ret;
	}
};

template<std::size_t N, typename T>
struct AccessorRange {
	using Iterator = AccessorIterator<N, T>;
	const tinygltf::Model& model;
	const tinygltf::Accessor& accessor;

	Iterator begin() const {
		return {model, accessor};
	}

	Iterator end() const {
		return begin() + accessor.count;
	}
};

template<std::size_t N, typename T>
AccessorRange<N, T> range(const tinygltf::Model& model,
		const tinygltf::Accessor& accessor) {
	return {model, accessor};
}

// TODO: just use <=> in C++20
template<std::size_t N, typename T>
bool operator==(const AccessorIterator<N, T>& a, const AccessorIterator<N, T>& b) {
	return a.buffer == b.buffer && a.address == b.address && a.stride == b.stride;
}

template<std::size_t N, typename T>
bool operator!=(const AccessorIterator<N, T>& a, const AccessorIterator<N, T>& b) {
	return a.buffer != b.buffer || a.address != b.address || a.stride != b.stride;
}

} // namespace doi
