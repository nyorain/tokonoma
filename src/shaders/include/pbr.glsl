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
