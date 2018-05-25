#version 450

layout(location = 0) in vec2 in_pos;
layout(location = 0) out float out_alpha;

const float pointCount = 256.f;

void main() {
	out_alpha = 1.f - (gl_VertexIndex / pointCount);
	gl_Position = vec4(in_pos, 0.0, 1.0);
}

