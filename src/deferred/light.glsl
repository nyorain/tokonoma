// base shading implementation that just needs the light parameters
// supplied by specific shader in function below.
// This was done to avoid code duplication (especially for sometimes
// changing inputs and their encodings) in different light types
// (point, dir).

// declared by specific shader
// multiply color with attenuation and (1 - shadow) if light source
// supports that. Returns lightDir must be normalized.
void getLightParams(vec3 viewPos, vec3 fragPos,
	out vec3 lightDir, out vec3 color);

#include "scene.glsl"
#include "pbr.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 outEmission;

// scene
layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 proj;
	mat4 invProj;
	vec3 viewPos;
	float _near, _far;
	uint mode;
} scene;

// gbuffers
layout(set = 1, binding = 0, input_attachment_index = 0)
	uniform subpassInput inNormal;
layout(set = 1, binding = 1, input_attachment_index = 1)
	uniform subpassInput inAlbedo;
layout(set = 1, binding = 2, input_attachment_index = 2)
	uniform subpassInput inEmission;
layout(set = 1, binding = 3, input_attachment_index = 3)
	uniform subpassInput inDepth;

void main() {
	fragColor = vec4(0.0);
	outEmission = vec4(0.0);
	float depth = subpassLoad(inDepth).r;
	if(depth == 1) { // nothing rendered in gbufs here
		return;
	}

	// reconstruct position from frag coord (uv) and depth
	vec2 suv = 2 * uv - 1; // projected suv
	suv.y *= -1.f; // flip y
	vec4 pos4 = scene.invProj * vec4(suv, depth, 1.0);
	vec3 fragPos = pos4.xyz / pos4.w;

	vec4 sNormal = subpassLoad(inNormal);
	vec4 sAlbedo = subpassLoad(inAlbedo);
	vec4 sEmission = subpassLoad(inEmission);

	vec3 normal = decodeNormal(sNormal.xy);
	vec3 albedo = sAlbedo.xyz;
	float roughness = sNormal.w;
	float metallic = sEmission.w;

	// debug output modes
	switch(scene.mode) {
	case 1:
		fragColor = vec4(albedo, 0.0);
		return;
	case 2:
		fragColor = vec4(0.5 * normal + 0.5, 0.0);
		return;
	case 3:
		fragColor = vec4(vec3(pow(depth, 15)), 0.0);
		return;
	case 4:
		fragColor = vec4(vec3(metallic), 0.0);
		return;
	case 5:
		fragColor = vec4(vec3(roughness), 0.0);
		return;
	default:
		break; // continue normally
	}

	vec3 lcolor;
	vec3 ldir;
	getLightParams(scene.viewPos, fragPos, ldir, lcolor);

	// for debugging: diffuse only
	// vec3 light = dot(normal, -ldir) * albedo;

	vec3 v = normalize(scene.viewPos - fragPos);
	vec3 light = cookTorrance(normal, -ldir, v, roughness, metallic,
		albedo);

	// NOTE: we are currently wasting the alpha component
	vec3 color = max(light * lcolor, 0.0);
	fragColor = vec4(color, 0.0);

	// high pass for bloom/emission
	const float highPassThreshold = 0.5;
	// float l = color.r + color.g + color.b;
	float l = length(color);
	// float l = max(color.r, max(color.g, color.b));
	if(l > highPassThreshold) {
		// color *= pow(1 - (highPassThreshold / l), 2);
		// color *= 1 - (highPassThreshold / l);
		// color *= smoothstep(0.0, 1.0, l - highPassThreshold);
		// color *= (l - highPassThreshold);

		// NOTE: this one turns out to be be best.
		// the pow leads to pushing the color vector towards
		// length 1 (higher power means stronger towards 1).
		// this is important since otherwise single well-lit pixels
		// may create really bright bloom which leads to bloom popping
		// when moving
		color *= pow(l - highPassThreshold, 0.4) / l;
		outEmission = vec4(color, 0.0);
	}
}
