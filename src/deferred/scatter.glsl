// TODO: make mie param configurable

// TODO: maybe more efficient to pass pattern as texture?
const float ditherPattern[4][4] = {
	{ 0.0f, 0.5f, 0.125f, 0.625f},
	{ 0.75f, 0.22f, 0.875f, 0.375f},
	{ 0.1875f, 0.6875f, 0.0625f, 0.5625},
	{ 0.9375f, 0.4375f, 0.8125f, 0.3125}};

// fragPos and lightPos are in screen space
// ldv: dot(lightDir, viewDir)
// TODO: remove or use z component of ipos; fragDepth
float lightScatterDepth(vec2 fragPos, vec2 lightPos, float lightDepth,
		float ldv, sampler2D depthTex, float fragDepth) {
	vec3 ray = vec3(lightPos, lightDepth) - vec3(fragPos, fragDepth);
	// NOTE: making the number of steps dependent on the ray length
	// is probably a bad idea. When light an pixel (geometry) are close, the
	// chance for artefacts is the highest i guess
	// float l = dot(ray, ray); // max: 2
	// uint steps = uint(clamp(10 * l, 5, 15));

	uint steps = 10;
	vec3 step = ray / steps;
	float accum = 0.f;
	vec3 ipos = vec3(fragPos, fragDepth);

	vec2 ppixel = mod(fragPos * textureSize(depthTex, 0), vec2(4, 4));
	float ditherValue = ditherPattern[int(ppixel.x)][int(ppixel.y)];
	ipos.xy += ditherValue * step.xy; // TODO: z too?

	// NOTE: instead of the dithering we could use a fully random
	// offset. Doesn't seem to work as well though.
	// Offset to step probably a bad idea
	// ipos += 0.7 * random(fragPos) * step;
	// step += 0.01 * (2 * random(step) - 1) * step;
	
	// sampling gets more important the closer we get to the light
	// important to not allow light to shine through completely
	// closed walls (because there e.g. is only one sample in it)
	float importance = 0.01;
	float total = 0.f;

	int lod = 0;
	// like ssao: we manually choose the lod since the default
	// derivative-based lod mechanisms aren't of much use here.
	// the larger the step size is, the less detail with need,
	// therefore larger step size -> high mipmap level
	// NOTE: nope, that doesn't work, destroys our random sampling
	// basically, that only works on higher levels...
	// vec2 stepPx = step * textureSize(depthTex, 0);
	// float stepLength = length(stepPx); // correct one, below are guesses
	// float stepLength = max(abs(stepPx.x), abs(stepPx.y));
	// float stepLength = min(abs(stepPx.x), abs(stepPx.y));
	// float stepLength = dot(stepPx, stepPx);
	// float lod = clamp(log2(stepLength) - 1, 0.0, 4.0);

	for(uint i = 0u; i < steps; ++i) {
		// sampler2DShadow: z value is the value we compare with
		// accum += texture(depthTex, vec3(ipos, rayEnd.z)).r;

		float depth = textureLod(depthTex, ipos.xy, lod).r;

		// TODO: don't make it binary for second condition
		// if(depth > lightDepth || depth < ipos.z) {
		if(depth >= lightDepth) {
			accum += importance;
		}

		ipos += step;
		total += importance;
		// importance *= 2;
	}

	// accum *= fac / steps;
	accum /= total;
	// accum = clamp(accum, 0.0, 1.0);
	// accum = smoothstep(0.1, 1.0, accum);

	// NOTE: random factors currently. We store it at 8 bit unorm
	// so we use factors here and when sampling to make sure we really
	// use the full precision (even if all scattering values were
	// rather around 0.05 or sth)
	// currently tuned for directional light
	// float fac = 10 * mieScattering(ldv, -0.7);
	float fac = 25 * phase_mie(ldv, -0.4);
	fac *= ldv;

	// nice small "sun" in addition to the all around scattering
	// fac += mieScattering(ldv, 0.95);

	// Make sure light gradually fades when light gets outside of screen
	// instead of suddenly jumping to 0 because of 'if' at beginning.
	// fac *= pow(lightPos.x * (1 - lightPos.x), 0.9);
	// fac *= pow(lightPos.y * (1 - lightPos.y), 0.9);
	fac *= 4 * lightPos.x * (1 - lightPos.x);
	fac *= 4 * lightPos.y * (1 - lightPos.y);

	return fac * accum;
}

float shadowMap(vec3 worldPos);

// - viewPos: camera position in world space
// - pos: sampled position for the given pixel (mapped uv + depth buffer)
// - ldv: light dot view vector
// - pixel: current pixel (integer, not normalized)
// To work, the shadowMap(worldPos) function has to be defined in the
// calling shader.
float lightScatterShadow(vec3 viewPos, vec3 pos, float ldv, vec2 pixel) {
	// first attempt at shadow-map based light scattering
	// http://www.alexandre-pestana.com/volumetric-lights/
	// TODO: here we probably really profit from different mipmaps
	// or some other optimizations... takes really long atm.
	vec3 rayStart = viewPos;
	vec3 rayEnd = pos;
	vec3 ray = rayEnd - rayStart;

	float rayLength = length(ray);
	vec3 rayDir = ray / rayLength;
	rayLength = min(rayLength, 10.f);
	ray = rayDir * rayLength;

	const uint steps = 10u;
	vec3 step = ray / steps;
	// rayStart += 0.01 * random(rayEnd) * step;

	float accum = 0.0;
	vec3 ipos = rayStart;

	// random dithering, we smooth it out later on
	vec2 ppixel = mod(pixel, vec2(4, 4));
	float ditherValue = ditherPattern[int(ppixel.x)][int(ppixel.y)];
	ipos.xyz += ditherValue * step.xyz;

	// TODO: falloff over time somehow?
	for(uint i = 0u; i < steps; ++i) {
		accum += shadowMap(ipos);
		ipos += step;
	}

	accum *= phase_mie(ldv, 0.25);
	accum /= steps;
	accum = clamp(accum, 0.0, 1.0);
	return accum;
}

