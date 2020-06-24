// See https://jadkhoury.github.io/files/MasterThesisFinal.pdf
// and the corresponding paper for more details and code.

// Instead of the usual (a, b, c) representation (describing 
// a * tri[0] + b * tri[1] + c * tri[2]), barycentric coordinates are 
// represented as just (b, c), a can be recovered as 1 - b - c.
// For homogeneous coords: (b, c, 1)

// Converts a single subdivision bit into the corresponding
// barycentric matrix.
// We have changed it in comparison to the paper so that subdivision
// preserves ccw orientation of triangles.
mat3 bitToMat(in bool bit) {
	float s = (bit ? 0.5f : -0.5f);
	vec3 c1 = vec3(-0.5, -s, 0);
	vec3 c2 = vec3(s, -0.5, 0);
	// vec3 c1 = vec3(s, -0.5, 0);
	// vec3 c2 = vec3(-0.5, -s, 0);
	vec3 c3 = vec3(0.5, 0.5, 1);
	return mat3(c1, c2, c3);
}

// Converts a subdivision id (key) into the barycentric matrix, describing
// the subdivided triangle associated with the key
mat3 keyToMat(in uint key) {
	mat3 xf = mat3(1);
	while(key > 1u) {
		xf = bitToMat(bool(key & 1u)) * xf;
		key = key >> 1u;
	}

	return xf;
}

// barycentric interpolation in the triangle formed by the three
// given points v, via the second two barycentric coordinates u.
vec3 berp(in vec3 v[3], in vec2 u) {
	return v[0] + u.x * (v[1] - v[0]) + u.y * (v[2] - v[0]);
}

const vec3 subd_bvecs[] = vec3[](
	vec3(0, 0, 1),
	vec3(1, 0, 1),
	vec3(0, 1, 1)
);

// Given the original triangle v_in and the given subdivision id/key,
// returns the associated subdivided triangle in v_out
// Also see the other subd overload.
void subd(in uint key, in vec3 v_in[3], out vec3 v_out[3]) {
	mat3 xf = keyToMat(key);
	v_out[0] = berp(v_in, (xf * subd_bvecs[0]).xy);
	v_out[1] = berp(v_in, (xf * subd_bvecs[1]).xy);
	v_out[2] = berp(v_in, (xf * subd_bvecs[2]).xy);
}

// Returns the vertex with id vid from the subdivded triangle with
// id 'key' in the given outer triangle v_in.
// Also see the other subd overload.
vec3 subd(in uint key, in vec3 v_in[3], in uint vid) {
	mat3 xf = keyToMat(key);
	return berp(v_in, (xf * subd_bvecs[vid]).xy);
}
