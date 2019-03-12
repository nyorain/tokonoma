// https://www.khronos.org/opengl/wiki/Template:Cubemap_layer_face_ordering
// https://www.khronos.org/registry/OpenGL/specs/gl/glspec42.core.pdf
// section 3.9.10
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
