float depthtoz(mat4 invProj, float depth) {
	// think of invProj * (dontcare, dontcare, depth, 1).
	// glsl column major matrix access
	float z = invProj[2][2] * depth + invProj[3][2];
	float w = invProj[2][3] * depth + invProj[3][3];

	// negate the result since the z values in view space are negative
	return -z / w;
}

