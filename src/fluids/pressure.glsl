float pressure(ivec2 center, ivec2 off) {
	const ivec2 size = imageSize(in_pressure);
	ivec2 pos = center + off;

	if(obstacle(pos)) {
		// make sure derivate over boundary is 0
		return imageLoad(in_pressure, center).x;
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
	return imageLoad(in_pressure, clamp(pos, ivec2(0), size - 1)).x;
}

