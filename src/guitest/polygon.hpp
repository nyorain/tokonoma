#pragma once

#include "draw.hpp"
#include <nytl/matOps.hpp>

namespace rvg2 {

// NOTE: only works for convex polygons at the moment.
// For non-convex ones you can triangulate them and manually
// use the VertexPool + IndexPool api.

/// Specifies in which way a polygon can be drawn.
struct DrawMode {
	/// Whether polygon/shape can be filled.
	/// While this is set to false for a polygon/shape you must not
	/// use a command buffer that fills the polygon/shape.
	/// So you want to set this to true whenever the polygon might
	/// be filled. If you then don't want to fill it, use the disable
	/// functionality.
	bool fill {};

	/// Whether this polygon/shape can be stroked (and how thick).
	/// Semantics are similar to the fill attribute but you
	/// additionally have to specify the stroke thickness. Setting
	/// this to 0.f means that the shape/polygon must never be used
	/// for stroking. Otherwise its stroke data will be maintained
	/// for the given thickness so it can be stroked.
	/// Must not be negative.
	float stroke {};

	/// In which direction to extrude the stroke:
	/// -1: purely inwards
	///  0: equally inwards and outwards (making the given points the center)
	///  1: purely outwards
	float strokeExtrude {};

	/// Whether to loop the stroked points
	/// Has no effect for filling.
	bool loop {};

	/// Defines per-point color values.
	/// If the polygon is then filled/stroked with a pointColorPaint,
	/// will use those points.
	struct {
		/// The per-point color values. Should have the same size
		/// as the points span passed to the polygon.
		/// As all colors, are expected to be in srgb space.
		std::vector<Vec4u8> points {};
		bool fill {}; /// whether they can be used when filling
		bool stroke {}; /// whether they can be used when stroking
	} color {};

	/// Whether to enable anti aliased fill
	/// May have large performance impact.
	bool aaFill {};
};

// Polygon
class Polygon {
public:
	struct Draw {
		unsigned vertexStart {invalid};
		unsigned indexStart {invalid};
		unsigned vertexCount {};
		unsigned indexCount {};
	};

	static constexpr auto fringe = 1.f;

public:
	Polygon() = default;
	Polygon(IndexPool&, VertexPool&);
	~Polygon();

	Polygon(Polygon&& rhs) { swap(*this, rhs); }
	Polygon& operator=(Polygon rhs) {
		swap(*this, rhs);
		return *this;
	}

	/// Can be called at any time, computes the polygon from the given
	/// points and draw mode. The DrawMode specifies whether this polygon
	/// can be used for filling or stroking and their properties.
	/// Automatically registers this object for the next updateDevice call.
	void update(Span<const Vec2f> points, const DrawMode&);

	/// Records commands to fill this polygon into the given DrawRecorder.
	/// Undefined behaviour if it was updated without fill support in
	/// the DrawMode.
	DrawInstance fill(DrawRecorder& recorder) const;

	/// Records commands to stroke this polygon into the given DrawRecorder.
	/// Undefined behaviour if it was updates without stroke support in
	/// the DrawMode.
	DrawInstance stroke(DrawRecorder& recorder) const;

	const auto& indexPool() const { return *indexPool_; }
	const auto& vertexPool() const { return *vertexPool_; }

	const auto& fillDraw() const { return fill_; }
	const auto& strokeDraw() const { return stroke_; }

	friend void swap(Polygon& a, Polygon& b);

protected:
	IndexPool* indexPool_ {};
	VertexPool* vertexPool_ {};

	Draw fill_;
	Draw stroke_;
	float strokeWidth_ {};
};

template<typename T>
struct ObjectDraws {
	T object;

	DrawInstance stroke;
	DrawInstance fill;
};

} // namespace rvg2
