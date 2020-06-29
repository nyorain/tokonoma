// The number of tiles per dimension of a face. The number of tiles per
// face will be this squared.
layout(constant_id = 5) const uint nTilesPD = 256;

// Number of tiles per face
const uint nTilesPF = nTilesPD * nTilesPD;

// Same as tkn::cubemap::faces, see transform.hpp.
// https://www.khronos.org/opengl/wiki/Template:Cubemap_layer_face_ordering
// https://www.khronos.org/registry/OpenGL/specs/gl/glspec42.core.pdf, section 3.9.10
// vulkan spec: chap15.html#_cube_map_face_selection_and_transformations
const vec3 d_x = vec3(1.f, 0.f, 0.f);
const vec3 d_y = vec3(0.f, 1.f, 0.f);
const vec3 d_z = vec3(0.f, 0.f, 1.f);
const struct Face {
    vec3 dir;
    vec3 s;
    vec3 t;
} cubeFaces[] = {
    {+d_x, -d_z, -d_y},
    {-d_x, +d_z, -d_y},
    {+d_y, +d_x, +d_z},
    {-d_y, +d_x, -d_z},
    {+d_z, +d_x, -d_y},
    {-d_z, -d_x, -d_y},
};

// Given 'dir', returns the face this direction will be
// projected on by a cube projection. Additionally stored the on-face
// signed uv coordinates (in range [-1, 1]) in 'suv'.
uint cubeFace(vec3 dir, out vec2 suv) {
	vec3 ad = abs(dir);
	if(ad.x >= ad.y && ad.x >= ad.z) {
		if(dir.x > 0) {
			suv = vec2(-dir.z, -dir.y) / ad.x;
			return 0;
		} else {
			suv = vec2(dir.z, -dir.y) / ad.x;
			return 1;
		}
	} else if(ad.y > ad.x && ad.y >= ad.z) {
		if(dir.y > 0) {
			suv = vec2(dir.x, dir.z) / ad.y;
			return 2;
		} else {
			suv = vec2(dir.x, -dir.z) / ad.y;
			return 3;
		}
	} else { // z is largest
		if(dir.z > 0) {
			suv = vec2(dir.x, -dir.y) / ad.z;
			return 4;
		} else {
			suv = vec2(-dir.x, -dir.y) / ad.z;
			return 5;
		}
	}
}

// Returns whether the two faces are opposite to each other.
bool opposite(uint face1, uint face2) {
	return ((face1 & 1u) == 0u) ? face1 + 1 == face2 : face2 + 1 == face1;
}

// Given 'dir', returns the tilePos (face, x, y) and the in-tile coordinates
// in range [0, 1] via 'tuv'.
uvec3 tilePos(vec3 dir, out vec2 tuv) {
    vec2 suv;
    uint face = cubeFace(dir, suv);
    vec2 uv = 0.5 + 0.5 * suv; // range [0, 1]
	vec2 tid;
	tuv = modf(uv * nTilesPD, tid);
    return uvec3(tid, uint(face));
}

// For flipping uv coordinates on face1 to uv coordinates on face0
mat2 flipUVMatrix(uint face0, uint face1, out ivec2 off) {
	// all of these values are in {-1, 0, 1}
	Face f0 = cubeFaces[face0];
	Face f1 = cubeFaces[face1];

    float ss = dot(f0.s, f1.s);
    float st = dot(f0.s, f1.t);
    float ts = dot(f0.t, f1.s);
    float tt = dot(f0.t, f1.t);

    float d0s1 = dot(f0.dir, f1.s);
    float d0t1 = dot(f0.dir, f1.t);

    float d1s0 = dot(f1.dir, f0.s);
    float d1t0 = dot(f1.dir, f0.t);

	float dsds = d0s1 * d1s0;
	float dsdt = d0s1 * d1t0;
	float dtdt = d0t1 * d1t0;
	float dtds = d0t1 * d1s0;

	// NOTE: might be possible to further optimize this
	// off values are one of {-1, 0, 1, 2}
	off = ivec2(0, 0);
	off.x += int(abs(dsds) * (int(d0s1 == d1s0) + d1s0));
	off.x += int(abs(dtds) * (int(d0t1 == d1s0) + d1s0));
	off.x += (ss == -1 || st == -1) ? 1 : 0;

	off.y += int(abs(dtdt) * (int(d0t1 == d1t0) + d1t0));
	off.y += int(abs(dsdt) * (int(d0s1 == d1t0) + d1t0));
	off.y += (ts == -1 || tt == -1) ? 1 : 0;

	// integer matrix
	return transpose(mat2(
		ss - dsds, st - dtds,
		ts - dsdt, tt - dtdt
	));
}

vec3 heightmapCoords(vec3 pos, uvec3 centerTile, uint lodOff, out bool valid,
		out vec3 worldS, out vec3 worldT) {
    vec2 tuv;
    uvec3 tpos = tilePos(pos, tuv);

	// find offset of tpos relative to center tile
	ivec2 toff;
    if(centerTile.z == tpos.z) {
		// The easy case: both are on the same face.
        toff = ivec2(tpos.xy) - ivec2(centerTile.xy);
		worldS = cubeFaces[centerTile.z].s;
		worldT = cubeFaces[centerTile.z].t;
    } else {
		// TODO: not really sure how to store opposite face. Maybe an extra
		// last lod just for it?
		if(opposite(tpos.z, centerTile.z)) {
			// return vec4(0.0, normalize(pos));
			valid = false;
			return vec3(0.0);
		}

		ivec2 off;
		mat2 fm = flipUVMatrix(centerTile.z, tpos.z, off);
		tuv = fract(fm * tuv); // TODO: not sure if correct. Use modified off?
		toff = ivec2(trunc(fm * (tpos.xy + 0.5) + int(nTilesPD) * off)) - ivec2(centerTile.xy);

		worldS = 
			fm[0][0] * cubeFaces[tpos.z].s +
			fm[1][0] * cubeFaces[tpos.z].t;
		worldT =
			fm[0][1] * cubeFaces[tpos.z].s +
			fm[1][1] * cubeFaces[tpos.z].t;
	}

	// find lowest mipmap that contains the tile
	int m = max(abs(toff.x), abs(toff.y));
	int lod = int(ceil(log2(max(m, 1)))) + int(lodOff);

	// TODO: linear interpolation (between lod and lod + 1 then, though)

	float tileSize = 1.f / (1 + 2 * exp2(lod));
	vec2 htsuv = -0.5 + tuv;
	vec2 suvt = toff + htsuv;
	vec2 uv = 0.5 + tileSize * suvt;
	if(uv != fract(uv)) {
		valid = false;
		return vec3(0.0);
	}

	worldT = cross(pos, worldS);
	worldS = -cross(pos, worldT);

	valid = true;
	return vec3(uv, lod);
}

vec4 height(vec3 pos, uvec3 centerTile, sampler2DArray heightmap) {
	bool valid;
	vec3 a, b;
	vec3 c = heightmapCoords(pos, centerTile, 0, valid, a, b);
	if(!valid) {
		return vec4(1, 0, 1, 1);
	}

	return texture(heightmap, c);
}

const float displaceStrength = 0.01;
vec3 displace(vec3 pos, uvec3 centerTile, sampler2DArray heightmap) {
	pos = normalize(pos);
	float h = height(pos, centerTile, heightmap).r;
	return 6360 * (1 + displaceStrength * h) * pos;
}

// Given a base unit vector (positive or negative), returns the
// associated cubemap face ID. Returns an invalid value otherwise.
int faceFromDir(vec3 dir) {
	// #1
	// int sum = int(dir.x + 2 * dir.y + 3 * dir.z);

	// #1a. Problem: potential out of range access for invalid dirs
	// const int table[] = {5, 3, 1, 0, 2, 4};
	// return table[3 + sum]

	// #1b
	// return int(-1.5 + 2 * abs(sum) - 0.5 * sign(sum));

	// #1c
	// switch(sum) {
	// 	case 1: return 0;
	// 	case -1: return 1;
	// 	case 2: return 2;
	// 	case -2: return 3;
	// 	case 3: return 4;
	// 	case -3: return 5;
	// 	default: return -1;
	// }

	// #2: easiest to read
	if(dir.x == 1) return 0;
	if(dir.x == -1) return 1;
	if(dir.y == 1) return 2;
	if(dir.y == -1) return 3;
	if(dir.z == 1) return 4;
	if(dir.z == -1) return 5;
	return -1;

	// #3
	// return int((dx != 0.0) ? 0.5 - 0.5 * dx :
	// 	(dy != 0.0) ? 2.5 - 0.5 * dy :
	// 	(dz != 0.0) ? 4.5 - 0.5 * dz : -1.0);
}

vec3 heightmapPos(uvec3 centerTile, vec2 uv, uint lod, out bool valid) {
	int nhTiles = int(exp2(lod)); // number of tiles on either size
	int nTiles = 1 + 2 * nhTiles; // total number of tiles in lod

	vec2 tid;
	vec2 tuv = modf(uv * nTiles, tid);
	ivec2 fid = ivec2(centerTile.xy) + ivec2(tid) - nhTiles;

	// should be equivalent
	// vec2 suv = -1 + 2 * uv;
	// vec2 scaled = 0.5 * suv * nTiles;
	// vec2 tid = round(scaled);
	// vec2 tuv = 0.5 + scaled - tid;
	// ivec2 fid = ivec2(centerTile.xy) + ivec2(tid);

	// -1: out of range along the negative axis
	//  0: valid range, i.e. not out of face
	//  1: out of range along the positive axis
	int x = int((step(0, fid.x) - 1) + step(int(nTilesPD), fid.x));
	int y = int((step(0, fid.y) - 1) + step(int(nTilesPD), fid.y));

	// In this case, we are outside the centerTile in x and y direction.
	// This means we are in an invalid portion of the heightmap.
	if(x * y != 0) {
		valid = false;
		return vec3(0.0);
	}

	int faceID = int(centerTile.z);
	Face face = cubeFaces[faceID];

	if(x != 0) {
		faceID = faceFromDir(x * cubeFaces[centerTile.z].s);
		// fid.x -= x * nhTiles;
	} else if(y != 0) {
		faceID = faceFromDir(y * cubeFaces[centerTile.z].t);
		// fid.y -= y * nhTiles;
	}

	if(x + y != 0) {
		face = cubeFaces[faceID];

		ivec2 toff;
		mat2 fm = flipUVMatrix(faceID, centerTile.z, toff);
		tuv = fract(fm * tuv); // TODO: not sure if correct, use modified toff?
		fid = ivec2(trunc(fm * (fid + 0.5) + int(nTilesPD) * toff));
	}

	// We must be inside the face now. Otherwise we are on the backside
	// of the cube (from centerTile), can't compute that.
	x = int((step(0, fid.x) - 1) + step(nTilesPD, fid.x));
	y = int((step(0, fid.y) - 1) + step(nTilesPD, fid.y));
	// assert(x == 0 && y == 0);
	if(x != 0 || y != 0) {
		valid = false;
		return vec3(0.0);
	}

	valid = true;
	vec2 fuv = -1 + 2 * ((fid + tuv) / float(nTilesPD));
	return normalize(face.dir + fuv.x * face.s + fuv.y * face.t);
}
