struct Line {
	vec2 point;
	vec2 dir;
};

struct Circle {
	vec2 center;
	float radius;
};

float distance(vec2 point, Line line) {
	vec2 n = normalize(vec2(line.dir.y, -line.dir.x));
	return abs(dot(n, point - line.point));
}

float cross(vec2 a, vec2 b) {
	return a.x * b.y - a.y * b.x;
}

vec2 project(vec2 point, Line line) {
	vec2 ab = line.dir;
	vec2 ap = point - line.point;
	float fac = dot(ap, ab) / dot(ab, ab);
	return line.point + fac * ab;
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

	// TODO: correct projections (not just the '1000 * x' stuff)
	const float proj = 1000;

	// special case: light is (almost) collinear with the segment
	// this results in shadows on both side of the segment
	// Line circleLine = {light.center, normal(line.dir)};
	// vec2 facs = intersectionFacs(line, circleLine);

	// TODO: sqrt avoidable here?
	// float ll = length(line.dir);
	// if(abs(facs.y / ll) < light.radius) {
	
	if(distance(light.center, line) < light.radius) {
			/*
		if(abs(facs.x - clamp(facs.x, 0, 1)) < (light.radius / ll)) {
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
			ShadowVertex ret = {vec2(0, 0), vec2(0, 0)};
			return ret;
		}
			*/

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
	return lightFalloff(lightPos, point, radius, strength, p,
		0.01, true);
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

