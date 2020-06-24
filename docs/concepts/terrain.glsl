layout(set = 0, binding = 0) sampler2DArray heightmap;
layout(set = 0, binding = 1) uniform UBO {
    uint center; // tileID of the center tile
    uint nTilesPD; // number of tiles per dimension
} ubo;

const float pi = 3.14159265;

// Same as tkn::cubemap::faces, see transform.hpp.
// https://www.khronos.org/opengl/wiki/Template:Cubemap_layer_face_ordering
// https://www.khronos.org/registry/OpenGL/specs/gl/glspec42.core.pdf, section 3.9.10
// vulkan spec: chap15.html#_cube_map_face_selection_and_transformations
const vec3 x = vec3(1.f, 0.f, 0.f);
const vec3 y = vec3(0.f, 1.f, 0.f);
const vec3 z = vec3(0.f, 0.f, 1.f);
const struct Face {
    nytl::Vec3f dir;
    nytl::Vec3f s;
    nytl::Vec3f t;
} faces[] = {
    {+x, -z, -y},
    {-x, +z, -y},
    {+y, +x, +z},
    {-y, +x, -z},
    {+z, +x, -y},
    {-z, -x, -y},
};

// TODO: fix border conditions
int cubeFace(vec3 dir, out vec2 suv) {
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

// Number of tiles per cube face
const uint nTilesPF = ubo.nTilesPD * ubo.nTilesPD;

vec3 tileCenter(uint tile) {
    uint fid = mod(tile, nTilesPF);
    uint inface = tile - (fid * nTilesPF);
    Face face = faces[face];

    float ux = mod(inface, ubo.nTilesPD) / float(ubo.nTilesPD);
    float uy = floor(inface / ubo.nTilesPD) / float(ubo.nTilesPD);

    return normalize(face.dir +
        (-1.0 + 2.0 * ux) * face.s +
        (-1.0 + 2.0 * uy) * face.t);
}

// The minimum length on a unit sphere a pixel covers.
// TODO: should be precomputed on cpu i guess
float minDist() {
    float tileSize = textureSize(heightmap, 0).x / 3;
    float sPixSize = 2 / (ubo.nTilesPD * tileSize);
    return atan(1 / (1 - sPixSize)) - pi / 4;
}

// shortest distance on sphere (for a, b normalized)
// https://en.wikipedia.org/wiki/Great-circle_distance
float sphereDist(vec3 a, vec3 b) {
    return acos(dot(a, b));
}

uint tileID(uvec3 tpos) {
    return tpos.z * ubo.nTilesPD * ubo.nTilesPD
        + tpos.y * ubo.nTilesPD
        + tpos.x;
}

uvec3 tileID(vec3 pos, out vec2 tuv) {
    vec2 suv;
    int face = cubeFace(pos, suv);

    vec2 uv = 0.5 + 0.5 * suv; // range [0, 1]
    uvec2 tid = floor(uv * ubo.nTiledPD);
    tuv = mod(uv, vec2(1.0 / ubo.nTilePD));
    return uvec3(uint(face), tid);
}

uvec3 tilePos(uint id) {
    uint inFace = mod(id, ubo.nTilesPD * ubo.nTilesPD);
    return uvec3(
        mod(inFace, ubo.nTilesPD) // x
        inFace / ubo.nTilesPD, // y (floor)
        id / ubo.nTilesPD * ubo.nTiledPD, // face (floor)
    )
}

// relative to center face
ivec2 flipFor(uint f1, ivec2 uv) {
    uint f0 = ubo.center / ubo.nTilesPD * ubo.nTiledPD; // floor

    float ss = dot(faces[f0].s, faces[f1].s);
    float st = dot(faces[f0].s, faces[f1].t);
    float ts = dot(faces[f0].t, faces[f1].s);
    float tt = dot(faces[f0].t, faces[f1].t);
    float dsds = dot(faces[f0].dir, faces[f1].s) * dot(faces[f1].dir, faces[f0].s);
    float dsdt = dot(faces[f0].dir, faces[f1].s) * dot(faces[f1].dir, faces[f0].t);
    float dtdt = dot(faces[f0].dir, faces[f1].t) * dot(faces[f1].dir, faces[f0].t)
    float dtds = dot(faces[f0].dir, faces[f1].t) * dot(faces[f1].dir, faces[f0].s)

    return ivec2(
        (ss + dsds) * uv.x + (ts + dsdt) * uv.y,
        (tt + dtdt) * uv.y + (st + dtds) * uv.x,
    );
}

// from center
ivec2 tileOff(uvec3 to) {
    uvec3 from = tilePos(ubo.center);
    if(from.z == to.z) {
        return ivec2(to.xy) - ivec2(from.xy);
    }

    uint f0 = from.z;
    uint t1 = to.z;

    int ds = dot(faces[f0].dir, faces[f1].s);
    int dt = dot(faces[f0].dir, faces[f1].t);

    ivec2 toBorder = ds > 0 ? ubo.nTiledPD - from.xy : ds * from.xy;
    return toBorder + flipFor(to.z, to.xy);
}

float sampleLod(uint lod, uvec3 tile, vec2 tuv) {
    float nTiles = 1 + pow(2, lod);
    float tileSize = textureSize(heightmap, 0).x / nTiles;
    vec2 uv = 0.5 + tileSize * tileOff(tile);
    return texture(heightmap, vec3(uv, lod)).r;
}

float height(vec3 pos) {
    // get tile id
    vec2 tuv;
    uvec3 tid = tileID(pos, tuv);

    // caclulate the mipmap level
    vec3 spos = normalize(pos);
    vec3 tc = tileCenter(ubo.center); // TODO: could be precomputed
    float tileDist = sphereDist(spos, tc);
    float lodDist = max(distance(pos, tc), tileDist);

	// NOTE: not sure if correct. important
    float lod = log2(lodDist / minDist());

    // linear mipmap interpolation
    uint lod0 = uint(floor(lod));
    uint lod1 = uint(ceil(lod));
    float f = fract(lod);
    return mix(sampleLod(lod0, tid, tuv), sampleLod(lod1, tid, tuv), f);
}
