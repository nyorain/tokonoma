// requires previously declared
// sample2D heightmap

vec3 displace(vec3 pos) {
	float height = texture(heightmap, 0.5 + 0.5 * pos.xz).r;
	pos.y += height;
	return pos;
}

