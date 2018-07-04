#pragma once

#include "part.hpp"
#include <unordered_map>

namespace kyo {

/// Can be used as static constant-time part lookup map.
/// Works around the C++ limitation that there is no polymorphism for
/// pointer to member objects.
/// Assumes that (private) members have static offset (technically
/// not guaranteed by the standard)
template<typename C>
class StaticPartMap {
public:
	template<typename... P>
	StaticPartMap(P&&... args) {
		add(args...);
	}

	template<typename P>
	usize offset(P C::* ptr) {
		return reinterpret_cast<usize>(&(static_cast<C*>(nullptr)->*ptr));
	}

	template<typename F, typename... R>
	void add(const std::type_info& typeinfo, F C::* ptr, R&&... rest) {
		static_assert(std::is_base_of_v<Part, F>);
		auto type = Part::Type {typeinfo};
		dlg_assert(offsets_.find(type) == offsets_.end());
		offsets_[type] = offset(ptr);
		add(rest...);
	}

	void add() {}

	Part* get(C& obj, Part::Type type) const {
		auto it = offsets_.find(type);
		if(it == offsets_.end()) {
			return nullptr;
		}

		auto p = reinterpret_cast<byte*>(&obj) + it->second;
		return reinterpret_cast<Part*>(p);
	}

	const Part* get(const C& obj, Part::Type type) const {
		auto it = offsets_.find(type);
		if(it == offsets_.end()) {
			return nullptr;
		}

		auto p = reinterpret_cast<const byte*>(&obj) + it->second;
		return reinterpret_cast<const Part*>(p);
	}

protected:
	std::unordered_map<Part::Type, usize> offsets_;
};

} // namespace kyo
