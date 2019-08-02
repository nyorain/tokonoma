// TODO: rather temporary workaround, not sure about obstact and 
// boundary handling (difference) with a normal grid...
// TODO: switch to staggered grid, finally
bool obstacle(ivec2 pos) {
	// currently simple a small box
	return clamp(pos.x, 81, 119) == pos.x &&
		clamp(pos.y, 51, 79) == pos.y;
}

bool boundary(ivec2 pos) {
	// currently simple a small box
	return clamp(pos.x, 80, 120) == pos.x &&
		clamp(pos.y, 50, 80) == pos.y;
}

float pressure(readonly image2D image, ivec2 center, ivec2 off) {
	const ivec2 size = imageSize(image);
	ivec2 pos = center + off;

	if(obstacle(pos)) {
		// make sure derivate over boundary is 0
		return imageLoad(image, center).x;
	} /*else if(pos.x < 0) { // inlet
		// skip; default
	} else if(pos.x >= size.x) { // outlet
		return 0.0;
	}
	*/

	// everything an outlet
	if(pos != clamp(pos, ivec2(0), size - 1)) {
		return 0.0;
	}

	// pressure boundary condition: neumann -> clamp
	return imageLoad(image, clamp(pos, ivec2(0), size - 1)).x;
}

vec2 velocity(sampler2D tex, vec2 pos) {
	vec2 clamped = clamp(pos, 0, 1);

	if(obstacle(ivec2(pos * textureSize(tex, 0)))) {
		// return vec2(0.0); // TODO: currently no-slip
	}/* else if(pos.x < 0) { // inlet
		return vec2(0.1, 0);
	} else if(pos.x > 1) { // outlet
		return texture(tex, clamped).xy;
	}
	*/

	// everything is an outlet
	return texture(tex, clamped).xy;

	// dirichlet boundary condition (slip)
	// vec2 vel = texture(tex, clamped).xy;
	// return vec2(equal(pos, clamped)) * vel;

	// dirichlet boundary condition (no slip)
	// if(pos != clamped) {
	// 	return vec2(0.0);
	// }
	// return texture(tex, clamped).xy;
}

vec2 velocity(readonly image2D image, ivec2 pos) {
	const ivec2 size = imageSize(image);
	ivec2 clamped = clamp(pos, ivec2(0), size - 1);
	
	if(obstacle(pos)) {
		// return vec2(0.0); // TODO: currently no-slip
	}/* else if(pos.x < 0) { // inlet
		return vec2(0.1, 0);
	} else if(pos.x >= size.x) { // outlet
		return imageLoad(image, clamped).xy;
	}
	*/

	// everyhing outlet
	return imageLoad(image, clamped).xy;

	// dirichlet boundary condition (slip)
	// vec2 vel = imageLoad(image, clamped).xy;
	// return vec2(equal(pos, clamped)) * vel;

	// dirichlet boundary condition (no slip)
	// if(pos != clamped) {
	// 	return vec2(0.0);
	// }
	// return imageLoad(image, clamped).xy;
}
