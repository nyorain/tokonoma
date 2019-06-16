const float pi = 3.1415926535897932;

// n: normal vector of the surface, normalized
// v: vector from point on surface to camera (view vector/dir), normalized
// l: vector from point on surface to light (light vector/dir), normalized
//    in case of a directional light this is simply the light direction
// h: half vector between l and v, normalize(l + v)

// Approximation of the fresnel equation.
// Returns how reflective the surface is (rgb tuple); in [0, 1]
// - cosTheta: dot(h, v)
// - f0 depends on the material (and optionally surface color)
vec3 fresnelSchlick(float cosTheta, vec3 f0) {
	return f0 + (1.0 - f0) * pow(1.0 - cosTheta, 5.0);
}

// https://learnopengl.com/PBR/IBL/Diffuse-irradiance
// https://seblagarde.wordpress.com/2011/08/17/hello-world/
vec3 fresnelSchlickRoughness(float cosTheta, vec3 f0, float roughness) {
    return f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - cosTheta, 5.0);
}  

// Normal distribution function (ndf), Trowbridge-Reitz GGX.
// Returns relative area of surface (microfacets) aligned like H; in [0, 1]
float distributionGGX(vec3 n, vec3 h, float roughness) {
	// NOTE: originally a (usually called alpha) was the roughness of
	// the material but for better visual results, roughness * roughness
	// is often used.
	// float a = roughness;
	float a = roughness * roughness;

	float a2 = a * a;
	float ndh = max(dot(n, h), 0.0);
	float ndh2 = ndh * ndh;

	float denom = ndh2 * (a2 - 1.0) + 1.0;
	denom = pi * denom * denom;
	return a2 / denom;
}


float geometrySchlickGGX(float ndv, float k) {
	return ndv / (ndv * (1.0 - k) + k);
}
  
// Geometry function. Returns factor of overshadowing; in [0, 1]
// - ndv: dot(n, v)
// - ndl: dot(n, l)
float geometrySmith(float ndv, float ndl, float roughness) {
	float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
	float ggx1 = geometrySchlickGGX(ndv, k);
	float ggx2 = geometrySchlickGGX(ndl, k);
	return ggx1 * ggx2;
}

// full cook-torrance brdf
vec3 cookTorrance(vec3 n, vec3 l, vec3 v, float roughness,
		float metallic, vec3 albedo) {
	vec3 f0 = vec3(0.04); // NOTE: good enough, could be made material property
	f0 = mix(f0, albedo, metallic);

	vec3 h = normalize(l + v);
	float ndv = max(dot(n, v), 0.001);
	float ndl = max(dot(n, l), 0.001);

	float ndf = distributionGGX(n, h, roughness);
	float g = geometrySmith(ndv, ndl, roughness);
	vec3 f = fresnelSchlick(clamp(dot(h, v), 0.0, 1.0), f0);

	vec3 specular = (ndf * g * f) / max(4.0 * ndv * ndl, 0.001);
	vec3 diffuse = (1.0 - f) * (1.0 - metallic) * albedo / pi;
	return (specular + diffuse) * ndl;
}


// low discrepancy sequence
vec2 hammersley(uint i, uint N) {
	// radical inverse based on
	// http://holger.dammertz.org/stuff/notes_HammersleyOnHemisphere.html
	uint bits = (i << 16u) | (i >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	float rdi = float(bits) * 2.3283064365386963e-10; // / 0x100000000
	return vec2(float(i) /float(N), rdi);
}

// https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf
vec3 importanceSampleGGX(vec2 xi, vec3 normal, float roughness) {
    float a = roughness*roughness;
	
    float phi = 2.0 * pi * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a*a - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta*cosTheta);
	
    // from spherical coordinates to cartesian coordinates
    vec3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;
	
    // from tangent-space vector to world-space sample vector
    vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);
	
    vec3 sampleVec = tangent * H.x + bitangent * H.y + normal * H.z;
    return normalize(sampleVec);
}  

const uint flagDiffuseIBL = (1u << 0u);
const uint flagSpecularIBL = (1u << 1u);
vec3 ao(uint flags, vec3 viewDir, vec3 normal, vec3 albedo, float metallic, 
		float roughness, samplerCube irradianceMap, samplerCube envMap,
		sampler2D brdfLut, uint envLods) {

	if((flags & (flagDiffuseIBL | flagSpecularIBL)) == 0) {
		return 0.25 * albedo;
	}

	// apply ao
	vec3 f0 = vec3(0.04); // NOTE: good enough, could be made material property
	f0 = mix(f0, albedo, metallic);

	float cosTheta = max(dot(normal, -viewDir), 0.0);
	vec3 kS = fresnelSchlickRoughness(cosTheta, f0, roughness);
	vec3 kD = 1.0 - kS;
	kD *= 1.0 - metallic;

	// ambient irradiance
	vec3 ambient = vec3(0.0);
	if((flags & flagDiffuseIBL) != 0) {
		vec3 irradiance = texture(irradianceMap, normal).rgb;
		vec3 diffuse = kD * irradiance * albedo;
		ambient += diffuse;
	} else {
		ambient += 0.125 * albedo;
	}

	// specular
	if((flags & flagSpecularIBL) != 0) {
		vec3 R = reflect(viewDir, normal);
		float lod = roughness * envLods;
		vec3 filtered = textureLod(envMap, R, lod).rgb;   
		vec2 brdfParams = vec2(cosTheta, roughness);
		vec2 brdf = texture(brdfLut, brdfParams).rg;
		vec3 specular = filtered * (kS * brdf.x + brdf.y);
		ambient += specular;
	} else {
		ambient += 0.125 * albedo;
	}

	return ambient;
}

