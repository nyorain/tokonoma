#version 450

layout(set = 0, binding = 0) uniform Params {
	float idstep;
	float amp;
	uint frameCount;
};

layout(set = 0, binding = 1) buffer Audio {
	float stereo[];
};

void main() {
	float at = min(gl_VertexIndex * idstep, frameCount - 1);
	uint id0 = uint(floor(at));
	uint id1 = uint(ceil(at));
	float f = at - id0;

	float x = amp * mix(stereo[2 * id0], stereo[2 * id1], f);
	float y = amp * mix(stereo[2 * id0 + 1], stereo[2 * id1 + 1], f);

	gl_Position = vec4(x, y, 0.0, 1.0);
	gl_PointSize = 1.f;
}
