// Light.flags
const uint lightDir = (1u << 0); // otherwise point
const uint lightPcf = (1u << 1); // use pcf

// MaterialPcr.flags
const uint normalMap = (1u << 0);
const uint doubleSided = (1u << 1);

// struct Light {
// 	vec3 pos; // position for point light, direction of dir light
// 	uint type; // point or dir
// 	vec3 color;
// 	uint pcf;
// };

struct DirLight {
	vec3 color; // w: pcf
	uint flags;
	vec3 dir;
	float _; // unused padding
	mat4 proj; // global -> light space
};

struct PointLight {
	vec3 color;
	uint flags;
	vec3 pos;
	float farPlane;
	mat4 proj[6]; // global -> cubemap [side i] light space
};

struct MaterialPcr {
	vec4 albedo;
	float roughness;
	float metallic;
	uint flags;
	float alphaCutoff;
};

// computes shadow for directional light
// returns (1 - shadow), i.e. the light factor
float dirShadow(sampler2DShadow shadowMap, vec3 pos, int range) {
	if(pos.z > 1.0) {
		return 1.0;
	}

	vec2 texelSize = 1.f / textureSize(shadowMap, 0);
	float sum = 0.f;
	for(int x = -range; x <= range; ++x) {
		for(int y = -range; y <= range; ++y) {
			// sampler has builtin comparison
			vec3 off = vec3(texelSize * vec2(x, y),  0);
			sum += texture(shadowMap, pos + off).r;
		}
	}

	float total = ((2 * range + 1) * (2 * range + 1));
	return sum / total;
}

// from https://learnopengl.com/Advanced-Lighting/Shadows/Point-Shadows
vec3 pointShadowOffsets[20] = vec3[](
   vec3( 1,  1,  1), vec3( 1, -1,  1), vec3(-1, -1,  1), vec3(-1,  1,  1), 
   vec3( 1,  1, -1), vec3( 1, -1, -1), vec3(-1, -1, -1), vec3(-1,  1, -1),
   vec3( 1,  1,  0), vec3( 1, -1,  0), vec3(-1, -1,  0), vec3(-1,  1,  0),
   vec3( 1,  0,  1), vec3(-1,  0,  1), vec3( 1,  0, -1), vec3(-1,  0, -1),
   vec3( 0,  1,  1), vec3( 0, -1,  1), vec3( 0, -1, -1), vec3( 0,  1, -1)
);

float pointShadow(samplerCubeShadow shadowCube, vec3 lightPos,
		float lightFarPlane, vec3 fragPos) {
	vec3 dist = fragPos - lightPos;
	float d = length(dist) / lightFarPlane;
	return texture(shadowCube, vec4(dist, d)).r;
}

float pointShadowSmooth(samplerCubeShadow shadowCube, vec3 lightPos,
		float lightFarPlane, vec3 fragPos, float radius) {
	vec3 dist = fragPos - lightPos;
	float d = length(dist) / lightFarPlane;
	uint sampleCount = 20u;
	float accum = 0.f;
	for(uint i = 0; i < sampleCount; ++i) {
		vec3 dir = dist + radius * pointShadowOffsets[i];
		accum += texture(shadowCube, vec4(dir, d)).r;
	}

	return accum / sampleCount;
}
	
// manual comparison
float pointShadow(samplerCube shadowCube, vec3 lightPos, 
		float lightFarPlane, vec3 fragPos) {
	vec3 dist = fragPos - lightPos;
	float d = length(dist) / lightFarPlane;
	float closest = texture(shadowCube, dist).r * lightFarPlane;
	float current = length(dist);
	// const float bias = 0.01f;
	const float bias = 0.f;
	return current < (closest + bias) ? 1.f : 0.f;
}

vec3 multPos(mat4 transform, vec3 pos) {
	vec4 v = transform * vec4(pos, 1.0);
	return vec3(v) / v.w;
}

// == screen space light scatter algorithms ==
vec3 sceneMap(mat4 proj, vec3 pos) {
	vec4 pos4 = proj * vec4(pos, 1.0);
	vec3 mapped = pos4.xyz / pos4.w;
	mapped.y *= -1; // invert y
	mapped.xy = 0.5 + 0.5 * mapped.xy; // normalize for texture access
	return mapped;
}

/*
// purely screen space old-school light scattering rays
// couldn't we do something like this in light/shadow space as well?
float lightScatterDepth(vec3 pos) {
	vec3 lrdir = (light.type == pointLight) ?
		light.pd - scene.viewPos :
		-light.pd;
	float fac = dot(normalize(pos - scene.viewPos), normalize(lrdir));
	if(fac < 0) {
		return 0.f;
	}

	// fac *= fac;

	vec2 rayStart = sceneMap(pos).xy;
	vec3 rayEnd = sceneMap(light.pd); // TODO: light.position
	// rayEnd.xy = clamp(rayEnd.xy, 0.0, 1.0);
	float accum = 0.f;
	uint steps = 25u;
	vec2 ray = rayEnd.xy - rayStart;
	vec2 step = ray / steps;

	vec2 ipos = rayStart;
	for(uint i = 0u; i < steps; ++i) {
		// the z value of the lookup position is the value we compare with
		// accum += texture(depthTex, vec3(ipos, rayEnd.z)).r;
		float depth = texture(depthTex, ipos).r;
		accum += depth <= rayEnd.z ? 0.f : 1.f;
		ipos += step;
		if(ipos != clamp(ipos, 0, 1)) {
			break;
		}
	}

	// accum *= 0.25 * fac;
	accum *= 0.1 * fac;
	accum /= steps; // only done steps?
	accum = clamp(accum, 0.0, 1.0);
	return accum;
}
*/

/*
// TODO: fix. Needs some serious changes for point lights
float lightScatterShadow(Light light, vec3 pos) {
	// NOTE: first attempt at light scattering
	// http://www.alexandre-pestana.com/volumetric-lights/
	// problem with this is that it doesn't work if background
	// is clear.
	vec3 rayStart = scene.viewPos;
	vec3 rayEnd = pos;
	vec3 ray = rayEnd - rayStart;

	float rayLength = length(ray);
	vec3 rayDir = ray / rayLength;
	rayLength = min(rayLength, 4.f);
	ray = rayDir * rayLength;

	vec3 lrdir = (light.type == pointLight) ?
		light.pd - scene.viewPos :
		-light.pd;
	float fac = dot(normalize(lrdir), rayDir);

	// offset slightly for smoother but noisy results
	// TODO: instead use per-pixel dither pattern and smooth out
	// in pp pass. Needs additional scattering attachment though
	rayStart += 0.1 * random(rayEnd) * rayDir;

	const uint steps = 25u;
	vec3 step = ray / steps;
	float accum = 0.0;
	pos = rayStart;
	for(uint i = 0u; i < steps; ++i) {
		// position in light space
		vec4 pls = light.matrix * vec4(pos, 1.0);
		pls.xyz /= pls.w;
		pls.y *= -1; // invert y
		pls.xy = 0.5 + 0.5 * pls.xy; // normalize for texture access
		// float fac = max(dot(normalize(light.pd - pos), rayDir), 0.0); // TODO: light.position
		// accum += fac * texture(shadowTex, pls.xyz).r;
		// TODO: implement/use mie scattering function
		accum += texture(shadowTex, pls.xyz).r;
		pos += step;
	}

	// 0.25: random factor, should be configurable
	accum *= 0.25 * fac;
	accum /= steps;
	accum = clamp(accum, 0.0, 1.0);
	return accum;
}
*/
