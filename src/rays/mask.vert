#include "shared.glsl"

layout(location = 0) out vec2 outNormal;

layout(set = 0, binding = 0, row_major) uniform Scene {
	UboData scene;
};

layout(set = 0, binding = 1) buffer Segments {
	Segment segments[];
};

layout(push_constant) uniform PCR {
	layout(offset = 0) vec2 offset;
	layout(offset = 8) bool down;
} pcr;

void main() {
	const uint i = gl_VertexIndex / 2;
	const bool first = gl_VertexIndex % 2 == 0;

	Segment seg = segments[i];
	vec2 line = normalize(seg.end - seg.start);
	vec2 normal = vec2(-line.y, line.x);

	vec2 pos = first ? seg.start : seg.end;
	gl_Position = scene.transform * vec4(pos, 0, 1);

	vec2 offset = pcr.offset;
	if(pcr.down) {
		normal *= -1;
	}


	vec2 snormal = normalize((scene.transform * vec4(normal, 0, 0)).xy);
	vec2 sline = normalize((scene.transform * vec4(line, 0, 0)).xy);

	if(first) {
		sline *= -1;
	} else {
		sline *= 1;
	}

	gl_Position.xy += offset * snormal;
	gl_Position.xy += 2 * offset * sline;

	outNormal = snormal;
}
