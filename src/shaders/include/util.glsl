// https://www.khronos.org/opengl/wiki/Template:Cubemap_layer_face_ordering
// https://www.khronos.org/registry/OpenGL/specs/gl/glspec42.core.pdf
// section 3.9.10
// (there is also a vulkan spec section on this stating the same)
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

// Finds two normalized, orthogonal vectors to the given one
void basis(vec3 dir, out vec3 x, out vec3 y) {
	// TODO: probably better way to do this, right?
	const vec3 anyv = normalize(vec3(-56.345, 12.1234, 1.23445));
	float diff = dot(anyv, dir);
	vec3 c = anyv;
	if(abs(1.0 - diff) < 0.1) {
		c.xyz = c.yzx;
	}

	x = normalize(cross(dir, c));
	y = normalize(cross(dir, x));
}
