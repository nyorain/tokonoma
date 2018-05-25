#pragma once

#include <nytl/vec.hpp>
#include <nytl/rect.hpp>
#include <utility> // std::swap

namespace doi {

/// Epsilon value used for comparisons and tests.
constexpr static auto epsilon = 1e-5f;

/// - The 3 types of 'lines' -
/// Represents an infinite line that goes through point
/// with the given direction. Not a valid line if direction is zero.
template<std::size_t D, typename T>
struct Line {
    nytl::Vec<D, T> point {};
    nytl::Vec<D, T> direction {};
};

/// Represents a ray that has a fixed origin from with
/// it goes infinitetly in its direction. Invalid if direction is zero.
template<std::size_t D, typename T>
struct Ray {
    nytl::Vec<D, T> origin {};
    nytl::Vec<D, T> direction {};
};

/// Represents a finite line segment that has a start and an end point.
/// Invalid if a == b.
template<std::size_t D, typename T>
struct Segment {
    nytl::Vec<D, T> a {};
    nytl::Vec<D, T> b {};
};

template<std::size_t D, typename T>
nytl::Vec<D, T> direction(const Segment<D, T>& seg) {
    return seg.b - seg.a;
}

template<std::size_t D, typename T>
Line<D, T> line(const Segment<D, T>& seg) {
    return {seg.a, direction(seg)};
}

template<std::size_t D, typename T>
Line<D, T> line(const Ray<D, T>& ray) {
    return {ray.point, ray.direction};
}

using Line2f = Line<2, float>;
using Ray2f = Ray<2, float>;
using Segment2f = Segment<2, float>;

using Line2d = Line<2, double>;
using Ray2d = Ray<2, double>;
using Segment2d = Segment<2, double>;

/// Information about the intersection of two lines/rays/segments.
struct LineIntersection {
	bool intersect {}; // Whether they intersect
    bool parrallel {}; // whether both lines are the same
    nytl::Vec2f point {}; // intersection point
    float facA {}; // lineA.point + facA * lineA.direction equals point
    float facB {}; // lineB.point + facB * lineB.direction equals point
};

/// Finds the intersection between the two given (infinite) lines.
/// Returns information about the intersectional properties.
///  - intersect: Whether the lines intersect.
///    Two lines will not intersect iff they are parrallel (and not the same).
///  - same: Whether the given lines are collinear.
///  - point: The point of intersection
///  - facA: The factor on line A to get to the intersection.
///    i.e. lineA.point + facA * lineA.direction will equal point.
///  - facB: The factor on line B to get to the intersection.
///    i.e. lineB.point + facB * lineB.direction will equal point.
/// facA and facB can be used to e.g. check if the intersection
/// occurs in a certain part of the infinite line.
/// If the given lines are collinear, will return lineA.point as
/// intersection and matching facA, facB values.
LineIntersection intersection(Line2f lineA, Line2f lineB);

LineIntersection intersection(Segment2f segA, Segment2f segB);
LineIntersection intersection(Ray2f rayA, Ray2f rayB);

LineIntersection intersection(Segment2f seg, Line2f line);
inline LineIntersection intersection(Line2f line, Segment2f seg) {
	auto li = intersection(seg, line);
    std::swap(li.facA, li.facB);
    return li;
}

LineIntersection intersection(Ray2f ray, Segment2f seg);
inline LineIntersection intersection(Segment2f seg, Ray2f ray) {
	auto li = intersection(ray, seg);
    std::swap(li.facA, li.facB);
    return li;
}

LineIntersection intersection(Ray2f ray, Line2f line);
inline LineIntersection intersection(Line2f line, Ray2f ray) {
	auto li = intersection(ray, line);
    std::swap(li.facA, li.facB);
    return li;
}

/// 2-dimensional cross product.
/// Is the same as the dot of a with the normal of b.
template<typename T>
constexpr T cross(nytl::Vec<2, T> a, nytl::Vec<2, T> b) {
    return a[0] * b[1] - a[1] * b[0];
}

/// Returns whether value is in range of [start,end]
template<typename T1, typename T2, typename T3>
constexpr bool inRange(T1 value, T2 start, T3 end) {
	return (value >= start && value <= end);
}

/// Orhtogonally projects point onto line.
/// The overload with a float reference will set it to the line factor
/// of the returned point, i.e. the returned point will equal
/// line.point + fac * line.direction.
nytl::Vec2f project(nytl::Vec2f point, Line2f line, float& fac);
inline nytl::Vec2f project(nytl::Vec2f point, Line2f line) {
    float fac;
    return project(point, line, fac);
}

/// Orhtogonally projects point onto segment.
/// If it cannot be directly projected onto segment, it will be
/// clamped to one of its endpoints.
/// The overload with a float reference will set it to the unclamped line
/// factor of the returned point which could be used to check if the
/// resulting point was clamped.
nytl::Vec2f project(nytl::Vec2f point, Segment2f segment, float& fac);
inline nytl::Vec2f project(nytl::Vec2f point, Segment2f segment) {
    float fac;
    return project(point, segment, fac);
}

/// Mirrors the given point on the given line.
nytl::Vec2f mirror(nytl::Vec2f point, Line2f line);

/// Represents the orientation a point can have in relation to a directed
/// line or segment.
enum class Orientation {
    collinear,
    right,
    left
};

/// Returns the orientation of the given point to the given line.
Orientation orientation(nytl::Vec2f point, const Line2f& line);

/// Represents the order a point and a line can have from a certain
/// point of view (direction).
enum class Order {
    equal,
    front,
    back
};

/// Returns the order of the given point to the given line in
/// direction of the given vector. If at the point where `point + a * dir`
/// is a point of the line, a is positive, returns Order::front,
/// if a is smaller than zero return Order::back and if a is on
/// the line or the line parrallel to dir, returns Order::equal.
Order order(nytl::Vec2f point, const Line2f& line, nytl::Vec2f dir);

/// Projects the given point onto the given rectangle from the given origin.
/// Has undefined behavior if origin does not lay inside view.
nytl::Vec2f rectProject(nytl::Vec2f origin, nytl::Vec2f point, nytl::Rect2f);

// Returns the point on the given circle for which the tangent goes through
// the given point. If minMaxFac is -1, will return the minimum point
// (math-rotation wise), for minMaxFac == 1 the max point.
nytl::Vec2f circlePoint(nytl::Vec2f circleCenter, float circleRadius,
	nytl::Vec2f point, float minMaxFac);

/// Represents a circle shape.
struct Circle {
	nytl::Vec2f center;
	float radius;
};

bool contains(Circle, nytl::Vec2f);
bool intersects(Circle a, Circle b);
bool intersects(Circle, nytl::Rect2f);
inline bool intersects(nytl::Rect2f rect, Circle circle) {
	return intersects(circle, rect);
}

// Assume mathematical (i.e. top and right is positive) coord system
namespace rhs {

template<std::size_t D, typename T>
auto left(const nytl::Rect<D, T>& rect) {
    return rect.position.x;
}

template<std::size_t D, typename T>
auto right(const nytl::Rect<D, T>& rect) {
    return rect.position.x + rect.size.x;
}

template<std::size_t D, typename T>
auto top(const nytl::Rect<D, T>& rect) {
    return rect.position.y + rect.size.y;
}

template<std::size_t D, typename T>
auto bottom(const nytl::Rect<D, T>& rect) {
    return rect.position.y;
}

template<std::size_t D, typename T>
auto bottomLeft(const nytl::Rect<D, T>& rect) {
    return rect.position;
}

template<std::size_t D, typename T>
auto bottomRight(const nytl::Rect<D, T>& rect) {
    return rect.position + nytl::Vec2f {rect.size[0], T(0)};
}

template<std::size_t D, typename T>
auto topLeft(const nytl::Rect<D, T>& rect) {
    return rect.position + nytl::Vec2f {T(0), rect.size[1]};
}

template<std::size_t D, typename T>
auto topRight(const nytl::Rect<D, T>& rect) {
    return rect.position + rect.size;
}

// Returns the left/right normal of a 2d vector
inline nytl::Vec2f lnormal(nytl::Vec2f vec) { return {-vec[1], vec[0]}; }
inline nytl::Vec2f rnormal(nytl::Vec2f vec) { return {vec[1], -vec[0]}; }

} // namespace rhs
} // namespace doi
