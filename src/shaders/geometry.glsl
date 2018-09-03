struct Line {
	vec2 point;
	vec2 dir;
};

struct Circle {
	vec2 center;
	float radius;
};

struct LineCircleIntersection {
	int num; // 0,1,2 
	vec2 a;
	vec2 b;
};

LineCircleIntersection intersections(Line line, Circle circle) {
	float dx = line.dir.x;
	float dy = line.dir.y;

	float a = dot(line.dir, line.dir);
	float b = 2 * dot(line.dir, line.point - circle.center);
	float c = dot(line.point - circle.center, line.point - circle.center);
	c -= circle.radius * circle.radius;

	float det = b * b - 4 * a * c;

	LineCircleIntersection ret;
	if(det < 0) {
		ret.num = 0;
	} else if(det == 0) {
		ret.num = 1;
		float t = -b / (2 * a);
		ret.a = line.point + t * line.dir;
	} else { // det > 0
		ret.num = 2;
		float t = (-b - sqrt(det)) / (2 * a);
		ret.a = line.point + t * line.dir;
		t = (-b + sqrt(det)) / (2 * a);
		ret.b = line.point + t * line.dir;
	}

	return ret;
}

float distance(vec2 point, Line line) {
	vec2 n = normalize(vec2(line.dir.y, -line.dir.x));
	return abs(dot(n, point - line.point));
}

float cross(vec2 a, vec2 b) {
	return a.x * b.y - a.y * b.x;
}

float projectFac(vec2 point, Line line) {
	return dot(point - line.point, line.dir) / dot(line.dir, line.dir);
}

vec2 project(vec2 point, Line line) {
	return line.point + projectFac(point, line) * line.dir;
}

vec2 mirror(vec2 point, Line line) {
	return 2 * project(point, line) - point;
}

#define OriLeft 0
#define OriRight 1

int orientation(vec2 point, Line line) {
	// TODO: what about collinear orientation?
	float det = cross(line.point - point, line.point + line.dir - point);
	return det > 0 ? OriLeft : OriRight;
}

// Returns the point on the light (light.position, light.radius) circle
// for which the tangent goes through point. If minMaxFac is -1, will return
// the minimum point (math-rotation wise), for minMaxFac == 1 the max point.
vec2 circlePoint(Circle circle, vec2 point, float minMaxFac) {
	// NOTE: might be possible to implement this more efficiently
	// currently solved by "rotating" the vector op by the angle
	// between op and (point - [searched point on circle]).
	// The angle is found in the triangle between circle.center, point
	// and [searched point on circle]

	vec2 op = point - circle.center;
	float lop = length(op);
	float c = circle.radius / lop;
	float s = sqrt(lop * lop - circle.radius * circle.radius) / lop;

	vec2 q = vec2(c * op.x - minMaxFac * s * op.y,
		minMaxFac * s * op.x + c * op.y);
	return circle.center + (circle.radius / lop) * q;
}

// Returns the factors on the given lines of their intersection point
vec2 intersectionFacs(Line a, Line b) {
	vec2 ab = b.point - a.point;
	float det = cross(a.dir, b.dir);
	// TODO: if det == 0 the lines are collinear
	return (1 / det) * vec2(cross(ab, b.dir), cross(ab, a.dir));
}

// A vertex of a smooth shadow polygon.
// An opacity (first component) <0 means that it cannot be performed from the
// vertex shader and should be computed in the fragment shader,
// otherwise it can be used to interpolation.
// The second component of it means irrelevance.
struct ShadowVertex {
	vec2 pos;
	vec2 opacity;
};

vec2 normal(vec2 v) {
	return vec2(-v.y, v.x);
}

// Returns the vertexID-th (mod 6) vertex of the smooth shadow vertex from
// the given light and segment.
// If oneSided is true, the segment will only generate shadow if
// lighted from the left side (shadow to the right side).
ShadowVertex smoothShadowVertex(int vertexID, vec2 segA, vec2 segB,
		Circle light, bool oneSided) {

	vertexID = vertexID % 6;
	Line line = {segA, segB - segA};

	// TODO: correct projections (not just the 'proj * x' stuff)
	const float proj = 1e2;
	const bool alwaysFullscreen = false; // debugging

	// TODO: more efficient check (without normalize/sqrt)
	if(alwaysFullscreen || distance(light.center, line) < light.radius) {
		// test if line intersects light
		float pf = clamp(projectFac(light.center, line), 0, 1);
		vec2 diff2 = (line.point + pf * line.dir) - light.center;
		if(alwaysFullscreen || dot(diff2, diff2) < light.radius * light.radius) {
			// fullscreen... ?
			const vec2[] values = {
				{-proj, proj},
				{proj, -proj},
				{proj, proj},
				{-proj, proj},
				{-proj, proj},
				{-proj, proj},
			};

			ShadowVertex ret = {values[vertexID % 6], vec2(-1, 0)};

			// just no shadow at all?
			// ShadowVertex ret = {vec2(0, 0), vec2(0, 0)};
			return ret;
		}

		// a is always the near point.
		vec2 da = segA - light.center;
		vec2 db = segB - light.center;
		bool swapl = dot(da, da) > dot(db, db);
		vec2 a = swapl ? segB : segA;
		vec2 b = swapl ? segA : segB;

		ShadowVertex ret;
		ret.opacity = vec2(-1, 0);

		switch(vertexID) {
			case 0:
				ret.pos = a + proj * (a - circlePoint(light, a,
					(oneSided && !swapl) ? -1 : 1));
				break;
			case 1:
				ret.pos = a;
				break;
			case 2:
				ret.pos = oneSided ?
					b + proj * (b - a) :
					a + proj * (a - circlePoint(light, a, -1));
				break;
			case 3:
			case 4:
			case 5:
				ret.pos = a;
				break;
			default:
				ret.pos = vec2(0, 0); // ERROR
				break;
		}

		return ret;
	}

	// TODO: the caller probably already did the orientation computation...
	bool swap = (orientation(light.center, line) == OriRight);
	if(oneSided && !swap) {
		// in this case we just generate an degenerate triangle
		return ShadowVertex(vec2(0, 2), vec2(-1, 0));
	}

	// the order of a,b matters because of circlePoint minMax
	// make sure a comes before b counter-clockwise (from lights pov)
	vec2 a = swap ? segB : segA;
	vec2 b = swap ? segA : segB;

	ShadowVertex ret;
	ret.opacity = vec2(-1, 0);

	switch(vertexID) {
		case 0:
			ret.pos = a + proj * (a - circlePoint(light, a, 1));
			break;
		case 1:
			ret.pos = b + proj * (b - circlePoint(light, b, -1));
			break;
		case 2:
			ret.pos = a;
			break;
		case 3:
		case 4:
		case 5:
			ret.pos = b;
			break;
		default:
			ret.pos = vec2(0, 0); // ERROR
			break;
	}

	return ret;
}

// returns the light strength falloff at the given point (for the given
// light position and strength).
// Good source: https://imdoingitwrong.wordpress.com/2011/01/31/light-attenuation/
float lightFalloff(vec2 lightPos, vec2 point, float radius, float strength,
		vec3 p, float cutoff, bool constantInner) {

	float d = length(point - lightPos) - radius;
	if(constantInner) {
		d = max(d, 0);
	}

	float f = strength / (p.x + p.y * d + p.z * d * d);
	f = max((f - cutoff) / (1 - cutoff), 0.0);
	return f;
}

float lightCutoff(float radius, float strength, float lightThresh) {
	return radius * (sqrt(strength / lightThresh ) - 1);
}

float lightFalloff(vec2 lightPos, vec2 point, float radius, float strength) {
	const vec3 p = vec3(1, 2 / radius, 1 / (radius * radius));
	const float lightTresh = 0.005; // corresponds to value in light.cpp
	return lightFalloff(lightPos, point, radius, strength, p,
		lightTresh, true);
}

bool normed(float val) {
	return val >= 0.f && val <= 1.f;
}

bool normed(vec2 val) {
	return normed(val.x) && normed(val.y);
}

// NOTE: WIP
float shadowValue2(Circle light, vec2 point, vec2 segA, vec2 segB) {
	// NOTE: ideally, the algorithm below would treat this correctly
	// TODO: test for finished algorithm
	// point inside light
	vec2 cp = point - light.center;
	if(dot(cp, cp) < light.radius * light.radius) {
		return 0.f;
	}

	Line seg = {segA, segB - segA};
	vec2 minProj = circlePoint(light, point, -1);
	vec2 maxProj = circlePoint(light, point, 1);
	Line minSeg = {minProj, point - minProj};
	Line maxSeg = {maxProj, point - maxProj};
	vec2 minFacs = intersectionFacs(seg, minSeg);
	vec2 maxFacs = intersectionFacs(seg, maxSeg);

	// case 1: segment fully outside of light cone
	if((minFacs.x < 0 && maxFacs.x < 0) || (minFacs.x > 1 && maxFacs.x > 1) ||
	   (minFacs.y < 0 && maxFacs.y < 0) || (minFacs.y > 1 && maxFacs.y > 1) ||
	   (minFacs.y < 0 && maxFacs.y > 1) || (minFacs.y > 1 && maxFacs.y < 0)) {
		return 0.0f;
	}

	Line minMax = {minProj, maxProj - minProj};
	Line maxMin = {maxProj, minProj - maxProj};

	// now we know that the segment intersects/lies in light cone
	// find intersection point
	if(normed(minFacs)) {
		if(normed(maxFacs)) {
			// in this case the line segments intersect the light cone twice.
			// this means no light gets to the point
			
			// TODO: performance, some redundant checks
			// special case: segment intersects light
			float pf = clamp(projectFac(light.center, seg), 0, 1);
			vec2 diff2 = (seg.point + pf * seg.dir) - light.center;
			if(dot(diff2, diff2) < light.radius * light.radius) {
				LineCircleIntersection is = intersections(seg, light);
				float l1 = length(is.a - is.b);

				vec2 p1 = minSeg.point + minFacs.y * minSeg.dir;
				vec2 p2 = maxSeg.point + maxFacs.y * maxSeg.dir;
				float l2 = length(p1 - p2);

				return 1 - l1 / l2;
			}

			return 1.f;
		}

		vec2 end = maxFacs.x < 0 ? segA : segB;

		// TODO: correct endpoint in all cases
		// also in else if branch below
		if(!normed(maxFacs.x)) {
			vec2 end = maxFacs.x > 1 ? segA : segB;
		} else if(!normed(maxFacs.y)) {
			vec2 end = maxFacs.x > minFacs.x ? segA : segB;
		}

		Line pl = {end, end - point};
		return intersectionFacs(pl, minMax).y;
	} else if(normed(maxFacs)) {
		vec2 end = minFacs.x < 0 ? segA : segB;

		Line pl = {end, end - point};
		return intersectionFacs(pl, maxMin).y;
	}

	// yeah, we should never get here.
	// set to 0.9 so we should see places where it is contant 0.9
	return 0.9f;
}

// returns by how much the given point is occluded by the given segment
// from the given light.
// TODO: rework this to a more simple (and generally working) algorithm
float shadowValue(Circle light, vec2 point, vec2 segA, vec2 segB) {
	// TODO: just for testing
	/*
	vec2 d = light.center - point;
	if(dot(d, d) < light.radius * light.radius) {
		return 0.5f;
	}
	*/

	// start
	vec2 minProj = circlePoint(light, point, -1);
	vec2 maxProj = circlePoint(light, point, 1);
	Line lightSeg = {minProj, maxProj - minProj};

	Line la;
	la.point = point;
	la.dir = segA - point;
	vec2 facsA = intersectionFacs(lightSeg, la);
	float facA = facsA.x;

	Line lb;
	lb.point = point;
	lb.dir = segB - point;
	vec2 facsB = intersectionFacs(lightSeg, lb);
	float facB = facsB.x;

	// possible corner cases:
	// usually (not in special case), facA <= facB
	// - facA < 0 && facB > 1: fully in shadow
	// - facA < 0 && facB < 0: fully in light (left)
	// - facA > 1 && facB > 1: fully in light (right)
	// - facA < 0 && 0 < facB < 1: partly in light #1
	// - facB > c1 && 0 < facA < 1: partly in light #2
	// - 0 < facB < 1 && 0 < facA < 1: partly in light #3

	// not sure if right in all cases
	// clamp them if they don't really intersect
	if(facsA.y < 0) {
		facA = facB > facA ? 2 : -1;
	}

	if(facsB.y < 0) {
		facB = facB > facsA.x ? -1 : 2;
	}

	// special case. Simply swap facA, facB
	if(facA > facB) {
		float tmp = facA;
		facA = facB;
		facB = tmp;
	}

	// should usually not happen, completely out of shadow range
	if((facA <= 0 && facB <= 0) || (facA >= 1 && facB >= 1)) return 0.f;
	if(facA <= 0 && facB >= 1) return 1.f;

	// we know: at least one of facA, facB is in range [0, 1], facA <= facB
	return min(facB, 1) - max(facA, 0);
}

// Correctly interprets the interpolation input, where the second
// component is the irrelevance component.
// Allows to interpolate just between two vertices.
float interpolate(vec2 value) {
	return value.x / (1 - value.y);
}

