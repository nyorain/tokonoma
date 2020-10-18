#pragma once

#include <vpp/fwd.hpp>
#include <cstdint>

namespace rvg2 {

class Context;
class UpdateContext;

using u32 = std::uint32_t;
static constexpr auto invalid = u32(0xFFFFFFFFu);

enum class UpdateFlags {
	none = 0,
	rerec = (1u << 0u),
	rebatch = (1u << 1u),
	descriptors = (1u << 2u),
};

#define RVG_FLAG_OPS(T) \
	constexpr T operator|(T a, T b) noexcept { return T(std::underlying_type_t<T>(a) | std::underlying_type_t<T>(b)); } \
	constexpr T operator&(T a, T b) noexcept { return T(std::underlying_type_t<T>(a) & std::underlying_type_t<T>(b)); } \
	constexpr T operator^(T a, T b) noexcept { return T(std::underlying_type_t<T>(a) ^ std::underlying_type_t<T>(b)); } \
	constexpr T operator~(T bit) noexcept { return T(~std::underlying_type_t<T>(bit)); } \
	constexpr T operator|=(T& a, T b) noexcept { return (a = (a | b)); } \
	constexpr T operator&=(T& a, T b) noexcept { return (a = (a & b)); } \
	constexpr T operator^=(T& a, T b) noexcept { return (a = (a ^ b)); }

RVG_FLAG_OPS(UpdateFlags)

template<typename ...Ts>
struct Visitor : Ts...  {
    Visitor(const Ts&... args) : Ts(args)...  {}
    using Ts::operator()...;
};

} // namespace rvg2
