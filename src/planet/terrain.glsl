// The number of tiles per dimension of a face. The number of tiles per
// face will be this squared.
layout(constant_id = 0) const uint nTilesPD = 8u;

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
// TODO: extend to return complete offset?
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

	// off.x = d1s0 == d0s1 ? d1s0 : 0;
	// off.y = d1t0 == d0t1 ? d1t0 : 0;

	// off.x = d1s0 != d0s1 ? d1s0 : 0;
	// off.y = d1t0 != d0t1 ? d1t0 : 0;

	if(d0s1 * d1s0) {
		off.x = int(d0s1 == d1s0) + d1s0;
	}

	// integer matrix
	return mat2(
		ss - d0s1 * d1s0, st - d0t1 * d1s0,
		tt - d0t1 * d1t0, ts - d0s1 * d1t0,
	);
}

int doff(int d, int tid) {
	switch(d) {
		case 0: return 0;
		case 1: return nTilesPD - tid;
		case -1: return -tid - 1;
		default:
			// can't happen
			return 0;
	}
}

vec4 height(vec3 pos, uvec3 centerTile, sampler2DArray heightmap) {
    vec2 tuv;
    uvec3 tpos = tilePos(pos, tuv);

	// find offset of tpos relative to center tile
	ivec2 toff;
	vec2 suvt = toff + tuv;
    if(centerTile.z == tpos.z) {
		// The easy case: both are on the same face.
        toff = ivec2(tpos.xy) - ivec2(centerTile.xy);
    } else {
		ivec2 off;
		mat2 fm = flipUVMatrix(centerTile.z, tpos.z, off);
		suvt = fm * suvt + off;

		/*
		ivec2 off;
		mat2 fm = flipUVMatrix(centerTile.z, tpos.z, off);
		tuv = fm * tuv + off;

		ivec2 oxy = fm * trunc((tpos.xy + 0.5) + nTilesPD * off);

    	int ds = int(dot(faces[tpos.z].dir, faces[centerTile.z].s));
    	int dt = int(dot(faces[tpos.z].dir, faces[centerTile.z].t));
		toff = ivec2(doff(ds, int(centerTile.x)), doff(dt, int(centerTile.y))) + oxy;
		*/
	}

	// find lowest mipmap that contains the tile
	int m = max(abs(toff.x), abs(toff.y));
	int lod = ceil(log2(max(m, 1)));

	// TODO: linear interpolation (between lod and lod + 1 then, though)

	float tileSize = 1 / (1 + 2 * pow2(lod));
	// vec2 uv = 0.5 + tileSize * (toff + tuv);
	vec2 uv = 0.5 + 0.5 * tileSize * suvt;
	return texture(heightmap, vec3(uv, lod));
}



// inverse
int faceFromDir(vec3 dir) {
	float dx = dot(dir, d_x);
	float dy = dot(dir, d_y);
	float dz = dot(dir, d_z);

	return int(dx ? 0.5 + 0.5 * dx :
		dy ? 2.5 + 0.5 * dy :
		dz ? 4.5 + 0.5 * dz : -1.0);
}

vec3 heightmapPos(uvec3 centerTile, vec2 suv, uint lod, out bool valid) {
	uint nhTiles = pow2(lod);
	uint nTiles = 1 + 2 * nhTiles;
	float tileSize = 1.0 / nTiles;

	ivec2 tid;
	vec2 tuv = mod(0.5 * suv * nTiles, tid);

	ivec2 fid = ivec2(centerTile.xy) + tid;
	int x = (step(0, fid.x) - 1) + step(nTilesPD, fix.x);
	int y = (step(0, fid.y) - 1) + step(nTilesPD, fix.y);

	// In this case, we are outside the centerTile in x and y direction.
	// This means we are in an invalid portion of the heightmap.
	if(x * y != 0) {
		valid = false;
		return vec3(0.0);
	}

	uint faceID = centerTile.face;
	Face face = faces[faceID]
	if(x != 0) {
		faceID = faceFromDir(x * faces[centerTile.face].s);
		fid.x -= x * nhTiles;
	} else if(y != 0) {
		faceID = faceFromDir(y * faces[centerTile.face].t);
		fid.y -= y * nhTiles;
	}

	if(x + y != 0) {
		face = faces[faceID];

		ivec2 toff;
		mat2 fm = flipUVMatrix(faceID, centerTile.z, toff);
		tuv = fm * tuv + off;
		fid = trunc(fm * (fid + 0.5) + nTiles * off);
	}

	vec2 fuv = tileSize * (fid + tuv);
	return normalize(face.dir + fuv.x * face.s + fuv.y + face.t);
}
