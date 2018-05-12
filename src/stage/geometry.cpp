#include <stage/geometry.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/approx.hpp>
#include <nytl/approxVec.hpp>
#include <nytl/rectOps.hpp>
#include <nytl/math.hpp>
#include <dlg/dlg.hpp>
#include <algorithm>

namespace kyo {

LineIntersection intersection(Line2f lineA, Line2f lineB) {
    dlg_assert(lineB.direction != nytl::approx(nytl::Vec2f {0, 0}));
    dlg_assert(lineA.direction != nytl::approx(nytl::Vec2f {0, 0}));

    auto ret = LineIntersection {};

    auto ab = lineB.point - lineA.point;
    auto dira = lineA.direction;
    auto dirb = lineB.direction;
    auto det = cross(dira, dirb);

    // if lines are parrallel
    if(det == 0.0) {
        // if they don't lay on the same line, they cannot intersect
        ret.parrallel = true;
        if(cross(dira, ab) != nytl::approx(0.f)) {
        	return ret;
        }

        ret.point = lineA.point;
        ret.facA = 0.f;
        ret.facB = -dot(ab, dirb) / dot(dirb, dirb);

	    dlg_check({
	    	auto pb = lineB.point + ret.facB * dirb;
		    dlg_assertm(pb == nytl::approx(ret.point, epsilon),
		    	"{}, {}", pb, ret.point);
	    });
    } else {
	    ret.facA = cross(ab, dirb) / det;
	   	ret.facB = cross(ab, dira) / det;
	    ret.point = lineA.point + ret.facA * dira;

	    dlg_check({
	    	auto pa = lineA.point + ret.facA * dira;
	    	auto pb = lineB.point + ret.facB * dirb;
		    dlg_assertm(pa == nytl::approx(ret.point, 10 * epsilon),
		    	"{}, {}", pa, ret.point);
		    dlg_assertm(pb == nytl::approx(ret.point, 10 * epsilon),
		    	"{}, {}", pb, ret.point);
	    });
    }

    ret.intersect = true;
    return ret;
}

LineIntersection intersection(Segment2f segA, Segment2f segB) {
	auto li = intersection(
		Line2f{segA.a, direction(segA)},
		Line2f{segB.a, direction(segB)});

	if(li.parrallel && li.intersect) {
		// basically: map all 4 endpoints to a one dimensional scale
		// along direction(segA), where 0.f is point a.a
		constexpr static auto aa = 0.f;
		constexpr static auto ab = 1.f;
		auto ba = dot(segB.a - segA.a, direction(segA));
		auto bb = dot(segB.b - segA.a, direction(segA));

		auto [minb, maxb] = std::minmax(ba, bb);
		if(inRange(aa, minb, maxb)) { // aa in b
			li.point = segA.a;
			li.facA = 0.f;
			li.facB = dot(segA.a - segB.a, direction(segB));
		} else if(inRange(minb, aa, ab)) { // minb in a
			li.point = segA.a + minb * direction(segA);
			li.facA = minb;
			li.facB = dot(li.point - segB.a, direction(segB));
		} else {
			li.intersect = false;
		}
	} else {
		constexpr auto eps = epsilon;
		li.intersect = li.intersect && inRange(li.facA, -eps, 1 + eps) &&
			inRange(li.facB, -eps, 1 + eps);
	}

	return li;
}

LineIntersection intersection(Ray2f rayA, Ray2f rayB) {
	auto li = intersection(
		Line2f{rayA.origin, rayA.direction},
		Line2f{rayB.origin, rayB.direction});

	if(li.parrallel && li.intersect) {
		auto a = dot(rayA.origin - rayB.origin, rayB.direction);
		auto b = dot(rayB.origin - rayA.origin, rayA.direction);

		if(a > 0) {
			li.point = rayA.origin;
			li.facA = 0.f;
			li.facB = a;
		} else if(b > 0) {
			li.point = rayB.origin;
			li.facA = b;
			li.facB = 0.f;
		} else {
			li.intersect = false;
		}
	} else {
		constexpr auto eps = epsilon;
		li.intersect = li.intersect && li.facA >= -eps && li.facB >= -eps;
	}

	return li;
}

LineIntersection intersection(Segment2f seg, Line2f line) {
	// no parrallel handling needed since seg.a will be chosen
	// in this case which is correct
	constexpr auto eps = epsilon;
	auto li = intersection(Line2f{seg.a, direction(seg)}, line);
	li.intersect = li.intersect && inRange(li.facA, -eps, 1 + eps);
	return li;
}

LineIntersection intersection(Ray2f ray, Segment2f seg) {
	constexpr auto eps = epsilon;
	auto li = intersection(
		Line2f{ray.origin, ray.direction},
		Line2f{seg.a, direction(seg)});

	if(li.parrallel && li.intersect) {
		auto origin = dot(ray.origin - seg.a, direction(seg));
		auto dd = dot(ray.direction, ray.direction);
		auto a = dot(seg.a - ray.origin, ray.direction) / dd;
		auto b = dot(seg.b - ray.origin, ray.direction) / dd;

		if(inRange(origin, -eps, 1 + eps)) {
			// origin lays in segment
			li.facA = 0.f;
			li.facB = origin;
			li.point = ray.origin;
		} else if(a > -eps && a < b) {
			li.facA = a;
			li.facB = 0.f;
			li.point = seg.a;
		} else if(b > -eps) {
			li.facA = b;
			li.facB = 1.f;
			li.point = seg.b;
		} else {
			li.intersect = false;
		}
	} else {
		li.intersect = li.intersect && li.facA >= -eps &&
			inRange(li.facB, -eps, 1 + eps);
	}

	return li;
}

LineIntersection intersection(Ray2f ray, Line2f line) {
	// no parrallel handling needed since ray.a will be chosen
	// in this case which is correct
	constexpr auto eps = epsilon;
	auto li = intersection(Line2f{ray.origin, ray.direction}, line);
	li.intersect = li.intersect && li.facA >= -eps;
	return li;
}

nytl::Vec2f project(nytl::Vec2f point, Line2f line, float& fac) {
	auto ab = line.direction;
	auto ap = point - line.point;
	fac = nytl::dot(ap, ab) / nytl::dot(ab, ab);
	return line.point + fac * ab;
}

nytl::Vec2f project(nytl::Vec2f point, Segment2f segment, float& fac) {
	auto ab = direction(segment);
	auto ap = point - segment.a;
	fac = nytl::dot(ap, ab) / nytl::dot(ab, ab);
	return segment.a + std::clamp(fac, 0.f, 1.f) * ab;
}

nytl::Vec2f mirror(nytl::Vec2f point, Line2f line) {
	return 2 * project(point, line) - point;
}

Orientation orientation(nytl::Vec2f point, const Line2f& line) {
	auto eps = std::numeric_limits<float>::epsilon() * 15; // TODO: important!
	auto det = cross(line.point - point, line.point + line.direction - point);
	if(det == nytl::approx(0.0, eps)) {
		return Orientation::collinear;
	} else if(det > 0) {
		return Orientation::left;
	} else {
		return Orientation::right;
	}
}

Order order(nytl::Vec2f point, const Line2f& line, nytl::Vec2f dir) {
	// NOTE: they may be a simpler, more efficient implementation
	auto o = orientation(point, line);
	auto d = cross(line.direction, dir);
	if(o == Orientation::collinear || d == 0) {
		return Order::equal;
	}

	// d > 0: dir roughly in same direction as lnormal of line
	// o == left: point on left side of line
	if((d > 0) == (o == Orientation::left)) {
		return Order::back;
	} else {
		return Order::front;
	}
}

nytl::Vec2f rectProject(nytl::Vec2f origin, nytl::Vec2f point, nytl::Rect2f rect) {
    dlg_assert(nytl::contains(rect, origin));
    auto dir = point - origin;
    dlg_assert(dir != nytl::approx(nytl::Vec2f {0, 0}));

    auto& pos = rect.position;
    nytl::Vec2f p;

    if(dir[0] < -0.0001) {
        p = point + ((pos[0] - point[0]) / dir[0]) * dir;
    } else if(dir[0] > 0.0001) {
        p = point + (((pos[0] + rect.size[0]) - point[0]) / dir[0]) * dir;
    } else { // dir == approx(0)
        return {point[0], dir[1] > 0 ? pos[0] + rect.size[0] : pos[0]};
    }

    auto diff = p[1] - std::clamp(p[1], pos[1], pos[1] + rect.size[1]);
    if(std::abs(diff) > 0.0) {
        dlg_assert(dir[1] != nytl::approx(0.f));
        p -= (diff / dir[1]) * dir;
    }

    return p;
}


nytl::Vec2f circlePoint(nytl::Vec2f circleCenter, float circleRadius,
		nytl::Vec2f point, float minMaxFac) {

	auto op = point - circleCenter;
	float lop = length(op);
	float c = circleRadius / lop;
	float s = sqrt(lop * lop - circleRadius * circleRadius) / lop;

	auto q = nytl::Vec2f{c * op[0] - minMaxFac * s * op[1],
		minMaxFac * s * op[0] + c * op[1]};
	return circleCenter + (circleRadius / lop) * q;
}

} // namespace kyo
