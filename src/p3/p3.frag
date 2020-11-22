layout(location = 0) in vec4 inColor;

layout(location = 0) out vec4 outFragColor;

void main() {
	float ndistCenter = length(2 * (0.5 - gl_PointCoord));
	if(ndistCenter > 1.0) {
		discard;
	}

	vec4 color = inColor;

	// aa
	color.a *= smoothstep(1.0, 0.95, ndistCenter);

	outFragColor = vec4(color.rgb, color.a);
}
