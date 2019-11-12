vec2 velocity(ivec2 pos) {
	const ivec2 size = imageSize(in_vel);
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
	return imageLoad(in_vel, clamped).xy;

	// dirichlet boundary condition (slip)
	// vec2 vel = imageLoad(image, clamped).xy;
	// return vec2(equal(pos, clamped)) * vel;

	// dirichlet boundary condition (no slip)
	// if(pos != clamped) {
	// 	return vec2(0.0);
	// }
	// return imageLoad(image, clamped).xy;
}

