#version 450

// wireframe geometry shader for triangles

layout(triangles) in;
layout(line_strip, max_vertices = 4) out;

layout(location = 0) out vec3 outColor;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 vp;
} scene;

const float normalLength = 0.1f;
vec4 toGlobal(vec4 pos) {
	pos = scene.vp * pos;
	pos.y = -pos.y;
	return pos;
}

void main() {
	vec4 v0 = gl_in[0].gl_Position;
	vec4 v1 = gl_in[1].gl_Position;
	vec4 v2 = gl_in[2].gl_Position;

	// emit wireframe, using blue
	outColor = vec3(0.2, 0.2, 1.0);
	gl_Position = toGlobal(v0);
	EmitVertex();

	gl_Position = toGlobal(v1);
	EmitVertex();

	gl_Position = toGlobal(v2);
	EmitVertex();

	gl_Position = toGlobal(v0);
	EmitVertex();
	EndPrimitive();
}
