#include "paint.hpp"

namespace rvg2 {

PaintData colorPaint(const Vec4f& color) {
	PaintData ret;
	ret.transform = nytl::identity<4, float>();
	ret.transform[3][3] = float(PaintType::color);
	ret.inner = color;
	return ret;
}

PaintData linearGradient(Vec2f start, Vec2f end,
		const Vec4f& startColor, const Vec4f& endColor) {
	PaintData ret;
	ret.transform = nytl::identity<4, float>();

	ret.transform[3][3] = float(PaintType::linGrad);
	ret.inner = startColor;
	ret.outer = endColor;
	ret.custom = {start.x, start.y, end.x, end.y};

	return ret;
}

PaintData radialGradient(Vec2f center, float innerRadius, float outerRadius,
		const Vec4f& innerColor, const Vec4f& outerColor) {
	PaintData ret;
	ret.transform = nytl::identity<4, float>();
	ret.transform[3][3] = float(PaintType::radGrad);

	ret.inner = innerColor;
	ret.outer = outerColor;
	ret.custom = {center.x, center.y, innerRadius, outerRadius};

	return ret;
}

PaintData texturePaintRGBA(const nytl::Mat4f& transform) {
	PaintData ret;
	ret.transform = transform;
	ret.transform[3][3] = float(PaintType::textureRGBA);
	ret.inner = {1.f, 1.f, 1.f, 1.f};
	return ret;
}

PaintData texturePaintA(const nytl::Mat4f& transform) {
	PaintData ret;
	ret.transform = transform;
	ret.transform[3][3] = float(PaintType::textureA);
	ret.inner = {1.f, 1.f, 1.f, 1.f};
	return ret;
}

PaintData pointColorPaint() {
	PaintData ret;
	ret.transform = nytl::identity<4, float>();
	ret.transform[3][3] = float(PaintType::pointColor);
	return ret;
}

} // namespace rvg2
