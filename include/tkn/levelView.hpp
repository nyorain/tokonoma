#pragma once

#include <tkn/types.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/matOps.hpp>
#include <cmath>

// Implements a 2D camera-like system that allows easy movement
// and scaling.
// TODO: add support for rotation (add 'float rot' to LevelView)

namespace tkn {

// == 2D coordinate transformations ==
// A rectangular view of the level, in level coordinates.
struct LevelView {
	nytl::Vec2f center; // center of the view in level coords
	nytl::Vec2f size; // total size of the view in level coords
};

// Returns a matrix mapping from the given view into level space to
// normalized coordinates ([-1, 1], for rendering).
// yup: whether the y axis is going up in level coordinates.
inline nytl::Mat4f levelMatrix(const LevelView& view, bool yup = true) {
	// the matrix gets level coordinates as input and returns
	// normalized window coordinates ([-1, 1])
	auto mat = nytl::identity<4, float>();
	nytl::Vec2f scale = {
		2.f / view.size.x,
		(yup ? -2.f : 2.f) / view.size.y,
	};

	// scale
	mat[0][0] = scale.x;
	mat[1][1] = scale.y;

	// translation; has to acknowledge scale
	mat[0][3] = -view.center.x * scale.x;
	mat[1][3] = -view.center.y * scale.y;

	return mat;
}

// Returns the viewSize into the level for the given widnowSize, when
// the larger window dimension should show `longSide` in level coordinate
// units.
// windowRatio: windowSize.x / windowSize.y
inline nytl::Vec2f levelViewSize(float windowRatio, float longSide) {
	nytl::Vec2f viewSize = {longSide, longSide};
	if (windowRatio > 1.f) {
		viewSize.y *= (1 / windowRatio);
	} else {
		viewSize.x *= windowRatio;
	}
	return viewSize;
}

// Returns a matrix mapping from the given view into level space to
// normalized coordinates ([-1, 1], for rendering).
// windowRatio: windowSize.x / windowSize.y
// centerOn: level position to center on; translation. If a point
//   is drawn in this position, it will be in the mid of the center
// longSide: how many coordinate units of the level the larger window will cover.
// yup: whether the y axis is going up in level coordinates.
inline nytl::Mat4f levelMatrix(float windowRatio, nytl::Vec2f center,
		float longSide, bool yup = true) {
	return levelMatrix({center, levelViewSize(windowRatio, longSide)}, yup);
}

// Can e.g. be used to translate clicks on the window to level coordinates.
// yup: whether the level (!) coordinate system is assumed to have a y axis
//   going upwards. Window coordinates are always expected to have their
//   origin in the top left.
// NOTE: you usually don't need the matrix, see the windowToLevel function
// below.
inline nytl::Mat4f windowToLevelMatrix(nytl::Vec2ui windowSize,
		const LevelView& view, bool yup = true) {
	auto mat = nytl::identity<4, float>();
	auto vs = nytl::Vec2f {view.size.x, yup ? -view.size.y : view.size.y};

	mat[0][0] = vs.x / windowSize.x;
	mat[1][1] = vs.y / windowSize.y;

	mat[0][3] = view.center.x - 0.5f * vs.x;
	mat[1][3] = view.center.y - 0.5f * vs.y;

	return mat;
}

inline nytl::Vec2f windowToLevel(nytl::Vec2ui windowSize,
		const LevelView& view, nytl::Vec2i input, bool yup = true) {
	auto vs = nytl::Vec2f {view.size.x, yup ? -view.size.y : view.size.y};
	return {
		(input.x / float(windowSize.x) - 0.5f) * vs.x + view.center.x,
		(input.y / float(windowSize.y) - 0.5f) * vs.y + view.center.y,
	};
}

// TODO: implement
// void scaleAroundWindow(LevelView& view, nytl::Vec2ui windowPos);
// void scaleAroundLevel(LevelView& view, nytl::Vec2f levelPos);
//
// // View coordinates relative inside the view, in range [-1, 1]
// void scaleAroundView(LevelView& view, nytl::Vec2f viewPos);


} // namespace tkn

