#version 450

layout(location = 0) in vec3 uvw;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform samplerCube cubemap;

// TODO: rather use blue noise for anti-banding dithering
float random(vec2 v) {
    float a = 43758.5453;
    float b = 12.9898;
    float c = 78.233;
    float dt = dot(v, vec2(b, c));
    float sn = mod(dt, 3.14);
    return fract(sin(sn) * a);
}

void main() {
	// use lod 0, the skybox might have multiple mip levels, e.g.
	// specular ibl convolutions
	outColor = textureLod(cubemap, uvw, 0);
	outColor.a = 1.0;

	// for debugging positions
	// // uvw = normalize(uvw); // smooth
	// outColor = vec4(0.5 + 0.5 * uvw, 1.0);

	// TODO: make optional via specialization constant?
	// simple tonemap, obviously only needed if the skybox is rendered
	// after tonemap step (shouldn't be case)
	// float exposure = 1.0;
	
	float exposure = 3.255e-05; // sunny16
	exposure /= 0.00001; // fp16 scale
	outColor.rgb *= exposure;
	// outColor.rgb = 1.0 - exp(-outColor.rgb * exposure);

	float gamma = 2.2f;
	// outColor.rgb = pow(outColor.rgb, vec3(1 / gamma)); // bad linear to srgb

	// anti-banding
	// important that we do this after conversion to srgb, i.e. on the
	// real, final 8-bit pixel values
	// Also important: no bias here
	float rnd1 = random(gl_FragCoord.xy + 0.17);
	float rnd2 = random(gl_FragCoord.xy + 0.85);
	float dither = 0.5 * (rnd1 + rnd2) - 0.5;
	// outColor.rgb += dither / 255.f;
}
