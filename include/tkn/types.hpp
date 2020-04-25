#pragma once

#include <cstdint>
#include <nytl/vec.hpp>
#include <nytl/mat.hpp>

namespace tkn {
inline namespace types {

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using f32 = float;
using f64 = double;

using nytl::Vec2f;
using nytl::Vec3f;
using nytl::Vec4f;

using nytl::Vec2i;
using nytl::Vec3i;
using nytl::Vec4i;

using nytl::Vec2ui;
using nytl::Vec3ui;
using nytl::Vec4ui;

using nytl::Mat2f;
using nytl::Mat3f;
using nytl::Mat4f;

} // namespace types
} // namespace tkn
