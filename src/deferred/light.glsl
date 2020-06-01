// base shading implementation that just needs the light parameters
// supplied by specific shader in function below.
// This was done to avoid code duplication (especially for sometimes
// changing inputs and their encodings) in different light types
// (point, dir).

// declared by specific shader
// multiply color with attenuation and (1 - shadow) if light source
// supports that. Returns lightDir must be normalized.
void getLightParams(vec3 viewPos, vec3 fragPos,
	out vec3 lightDir, out vec3 color, float linearz);

#include "scene.glsl"
#include "pbr.glsl"

layout(location = 0) noperspective in vec2 uv;
layout(location = 0) out vec4 fragColor;

// scene
layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 proj;
	mat4 invProj;
	vec3 viewPos;
	float near, far;
} scene;

// gbuffers
layout(set = 1, binding = 0, input_attachment_index = 0)
	uniform subpassInput inNormal;
layout(set = 1, binding = 1, input_attachment_index = 1)
	uniform subpassInput inAlbedo;
layout(set = 1, binding = 2, input_attachment_index = 2)
	uniform subpassInput inDepth;

void main() {
	// fragColor = vec4(uv, 0.0, 0.0);
	// vec2 uv = gl_FragCoord.xy / vec2(1920, 1080);
	// return;

	fragColor = vec4(0.0);
	float z = subpassLoad(inDepth).r;
	float depth = ztodepth(z, scene.near, scene.far);
	if(depth >= 1) { // nothing rendered in gbufs here
		discard;
	}

	// reconstruct position from frag coord (uv) and depth
	vec2 suv = 2 * uv - 1; // projected suv
	vec3 fragPos = multPos(scene.invProj, vec3(suv, depth));
	// vec3 fragPos = reconstructWorldPos(uv, scene.invProj, depth);

	vec3 lcolor;
	vec3 ldir;
	getLightParams(scene.viewPos, fragPos, ldir, lcolor, z);

	// fast return; useful for point lights (where pixel is outside radius)
	// and shadow regions. We don't have to do the complete pbr calculation
	// there.
	if(lcolor.r + lcolor.g + lcolor.b <= 0.0) {
		// return;
		discard;
	}

	vec4 sNormal = subpassLoad(inNormal);
	vec4 sAlbedo = subpassLoad(inAlbedo);

	vec3 normal = decodeNormal(sNormal.xy);
	vec3 albedo = sAlbedo.xyz;
	float roughness = sNormal.w;
	float metallic = sNormal.z;

	// for debugging: diffuse only
	// vec3 light = dot(normal, -ldir) * albedo;

	vec3 v = normalize(scene.viewPos - fragPos);
	vec3 light = cookTorrance(normal, -ldir, v, roughness, metallic, albedo);
	vec3 color = max(light * lcolor, 0.0);

	// NOTE: we are currently wasting the alpha component
	fragColor = vec4(color, 0.0);
}
