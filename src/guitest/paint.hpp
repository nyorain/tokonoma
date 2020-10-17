#pragma once

#include "scene.hpp"
#include <nytl/vec.hpp>

namespace rvg2 {

using namespace nytl;

enum class PaintType : std::uint32_t {
	color = 1,
	linGrad = 2,
	radGrad = 3,
	textureRGBA = 4,
	textureA = 5,
	pointColor = 6,
};

PaintData colorPaint(const Vec4f&);
PaintData linearGradient(Vec2f start, Vec2f end,
	const Vec4f& startColor, const Vec4f& endColor);
PaintData radialGradient(Vec2f center, float innerRadius, float outerRadius,
	const Vec4f& innerColor, const Vec4f& outerColor);
PaintData texturePaintRGBA(const nytl::Mat4f& transform);
PaintData texturePaintA(const nytl::Mat4f& transform);
PaintData pointColorPaint();

} // namespace rvg2
