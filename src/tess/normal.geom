#version 450

layout(triangles) in;
layout(line_strip, max_vertices = 14) out;

layout(location = 0) in vec3 inNormal[];

layout(location = 0) out vec3 outColor;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 vp;
} scene;

const float normalLength = 0.1f;
vec4 toGlobal(vec4 pos) {
	// pos = scene.vp * pos;
	// pos.y = -pos.y;
	return pos;
}

void emitVertexNormal(uint i) {
	vec4 pos = gl_in[i].gl_Position;
	gl_Position = toGlobal(pos);
	EmitVertex();

	vec4 off = pos + normalLength * vec4(inNormal[i], 0.0);
	gl_Position = toGlobal(off);
	EmitVertex();
	EndPrimitive();
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
	EndPrimitive();

	gl_Position = toGlobal(v1);
	EmitVertex();

	gl_Position = toGlobal(v2);
	EmitVertex();
	EndPrimitive();

	gl_Position = toGlobal(v2);
	EmitVertex();

	gl_Position = toGlobal(v0);
	EmitVertex();
	EndPrimitive();

	/*
	// emit per-vertex normals, using red
	outColor = vec3(1.0, 0.2, 0.2);
	emitVertexNormal(0);
	emitVertexNormal(1);
	emitVertexNormal(2);

	// emit per-face normal, using green
	outColor = vec3(0.2, 1.0, 0.2);
	vec4 center = (v0 + v1 + v2) / 3;
	gl_Position = toGlobal(center);
	EmitVertex();

	// i guess this cross order is needed since we are still
	// in LHS at the moment right?
	vec3 fn = normalize(cross((v1 - v0).xyz, (v2 - v0).xyz));
	gl_Position = toGlobal(center + normalLength * vec4(fn, 0.0));
	EmitVertex();
	EndPrimitive();
	*/
}
