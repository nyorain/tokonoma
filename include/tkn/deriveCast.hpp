#pragma once

#include <dlg/dlg.hpp>
#include <type_traits>

namespace tkn {

/// Like a mixture of static_cast and dynamic_cast.
/// Will assert (i.e. no-op in release mode) that the given pointer
/// can be casted to the requested type and then cast it.
/// If the given pointer is null, simply returns null.
/// The given args will be forwarded to the asserts error message.
/// Prefer this (or the variations below) over dynamic_cast or static_cast if
/// you are... "sure" that the given base pointer has a type.
/// It at least outputs a error in debug mode and has no overhead in release mode.
template<typename T, typename O, typename... Args>
std::enable_if_t<std::is_pointer_v<T>, T> deriveCast(O* ptr, Args&&... args) {
	static_assert(std::is_base_of_v<O, std::remove_pointer_t<T>>);
	dlg_asserttm(("deriveCast"), !ptr || dynamic_cast<T>(ptr), std::forward<Args>(args)...);
	return static_cast<T>(ptr);
}

/// Like the deriveCast overload above but expects the given pointer
/// to be non-null and therefore also returns a reference.
template<typename T, typename O, typename... Args>
std::enable_if_t<std::is_reference_v<T>, T> deriveCast(O* ptr, Args&&... args) {
	using Raw = std::remove_reference_t<T>;
	static_assert(std::is_base_of_v<O, Raw>);

	dlg_asserttm(("deriveCast"), dynamic_cast<Raw*>(ptr), std::forward<Args>(args)...);
	return static_cast<T>(*ptr);
}

/// Like a mixture of static_cast and dynamic_cast.
/// Will assert (i.e. no-op in release mode) that the given reference
/// can be casted to the requested type and then cast it.
template<typename T, typename O, typename... Args>
std::enable_if_t<std::is_reference_v<T>, T> deriveCast(O& ref, Args&&... args) {
	using Raw = std::remove_reference_t<T>;
	static_assert(std::is_base_of_v<O, Raw>);

	dlg_asserttm(("deriveCast"), dynamic_cast<Raw*>(&ref), std::forward<Args>(args)...);
	return static_cast<T>(ref);
}

} // namespace tkn
