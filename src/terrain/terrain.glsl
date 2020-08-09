vec3 computeNormal(sampler2D heightmap, vec2 baseCoord) {
	// compute 1D normals on this texel's edges
	const vec4 heights = textureGather(heightmap, baseCoord);
	const float dx0 = heights[2] - heights[3];
	const float dx1 = heights[1] - heights[0];
	const float dy0 = heights[0] - heights[3];
	const float dy1 = heights[1] - heights[2];

	// linearly interpolate normal
	const ivec2 texSize = textureSize(heightmap, 0);
	const vec2 f = fract(texSize * baseCoord - 0.5);
	const float dx = mix(dx0, dx1, f.x) * texSize.x;
	const float dy = mix(dy0, dy1, f.y) * texSize.y;
	return normalize(vec3(0, 1, 0) - dx * vec3(1, 0, 0) - dy * vec3(0, 0, 1));
}
