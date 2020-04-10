// See https://jadkhoury.github.io/files/MasterThesisFinal.pdf
// and the corresponding paper for more details and code.

// Converts a single subdivision bit into the corresponding
// barycentric matrix.
// We have changed it in comparison to the paper so that subdivision
// preserves ccw orientation of triangles.
mat3 bitToMat(in bool bit) {
	float s = (bit ? 0.5f : -0.5f);
	vec3 c1 = vec3(-0.5, -s, 0);
	vec3 c2 = vec3(s, -0.5, 0);
	// vec3 c1 = vec3(s, -0.5, 0);
	// vec3 c2 = vec3(-0.5, -s, 0);
	vec3 c3 = vec3(0.5, 0.5, 1);
	return mat3(c1, c2, c3);
}

// Converts a subdivision id (key) into the barycentric matrix, describing
// the subdivided triangle associated with the key
mat3 keyToMat(in uint key) {
	mat3 xf = mat3(1);
	while(key > 1u) {
		xf = bitToMat(bool(key & 1u)) * xf;
		key = key >> 1u;
	}

	return xf;
}

// barycentric interpolation in the triangle formed by the three
// given points v, via the second two barycentric coordinates u.
vec3 berp(in vec3 v[3], in vec2 u) {
	return v[0] + u.x * (v[1] - v[0]) + u.y * (v[2] - v[0]);
}

const vec3 subd_bvecs[] = vec3[](
	vec3(0, 0, 1),
	vec3(1, 0, 1),
	vec3(0, 1, 1)
);

// Given the original triangle v_in and the given subdivision id/key,
// returns the associated subdivided triangle in v_out
// Also see the other subd overload.
void subd(in uint key, in vec3 v_in[3], out vec3 v_out[3]) {
	mat3 xf = keyToMat(key);
	v_out[0] = berp(v_in, (xf * subd_bvecs[0]).xy);
	v_out[1] = berp(v_in, (xf * subd_bvecs[1]).xy);
	v_out[2] = berp(v_in, (xf * subd_bvecs[2]).xy);
}

// Returns the vertex with id vid from the subdivded triangle with
// id 'key' in the given outer triangle v_in.
// Also see the other subd overload.
vec3 subd(in uint key, in vec3 v_in[3], in uint vid) {
	mat3 xf = keyToMat(key);
	return berp(v_in, (xf * subd_bvecs[vid]).xy);
}



// wip
// from https://www.iquilezles.org/www/articles/gradientnoise/gradientnoise.htm
vec3 noised( in vec2 p ) {
    vec2 i = floor( p );
    vec2 f = fract( p );

    vec2 u = f*f*f*(f*(f*6.0-15.0)+10.0);
    vec2 du = 30.0*f*f*(f*(f-2.0)+1.0);
    
    vec2 ga = random2( i + vec2(0.0,0.0) );
    vec2 gb = random2( i + vec2(1.0,0.0) );
    vec2 gc = random2( i + vec2(0.0,1.0) );
    vec2 gd = random2( i + vec2(1.0,1.0) );
    
    float va = dot( ga, f - vec2(0.0,0.0) );
    float vb = dot( gb, f - vec2(1.0,0.0) );
    float vc = dot( gc, f - vec2(0.0,1.0) );
    float vd = dot( gd, f - vec2(1.0,1.0) );

    return vec3( va + u.x*(vb-va) + u.y*(vc-va) + u.x*u.y*(va-vb-vc+vd),   // value
                 ga + u.x*(gb-ga) + u.y*(gc-ga) + u.x*u.y*(ga-gb-gc+gd) +  // derivatives
                 du * (u.yx*(va-vb-vc+vd) + vec2(vb,vc) - va));
}

vec3 noisedp( in vec2 p, in vec2 per) {
    ivec2 i = ivec2(floor( p ));
    vec2 f = fract( p );

    vec2 u = f*f*f*(f*(f*6.0-15.0)+10.0);
    vec2 du = 30.0*f*f*(f*(f-2.0)+1.0);

    // vec2 ga = random2( i + vec2(0.0,0.0));
    // vec2 gb = random2( i + vec2(1.0,0.0));
    // vec2 gc = random2( i + vec2(0.0,1.0));
    // vec2 gd = random2( i + vec2(1.0,1.0));
    
    vec2 ga = random2( mod(i + ivec2(0,0), ivec2(per)) );
    vec2 gb = random2( mod(i + ivec2(1,0), ivec2(per)) );
    vec2 gc = random2( mod(i + ivec2(0,1), ivec2(per)) );
    vec2 gd = random2( mod(i + ivec2(1,1), ivec2(per)) );
    
    float va = dot( ga, f - vec2(0.0,0.0) );
    float vb = dot( gb, f - vec2(1.0,0.0) );
    float vc = dot( gc, f - vec2(0.0,1.0) );
    float vd = dot( gd, f - vec2(1.0,1.0) );

    return vec3( va + u.x*(vb-va) + u.y*(vc-va) + u.x*u.y*(va-vb-vc+vd),   // value
                 ga + u.x*(gb-ga) + u.y*(gc-ga) + u.x*u.y*(ga-gb-gc+gd) +  // derivatives
                 du * (u.yx*(va-vb-vc+vd) + vec2(vb,vc) - va));
}

vec4 noised( in vec3 x )
{
    // grid
    vec3 p = floor(x);
    vec3 w = fract(x);
    
    // quintic interpolant
    vec3 u = w*w*w*(w*(w*6.0-15.0)+10.0);
    vec3 du = 30.0*w*w*(w*(w-2.0)+1.0);
    
    // gradients
    vec3 ga = random3( p+vec3(0.0,0.0,0.0) );
    vec3 gb = random3( p+vec3(1.0,0.0,0.0) );
    vec3 gc = random3( p+vec3(0.0,1.0,0.0) );
    vec3 gd = random3( p+vec3(1.0,1.0,0.0) );
    vec3 ge = random3( p+vec3(0.0,0.0,1.0) );
    vec3 gf = random3( p+vec3(1.0,0.0,1.0) );
    vec3 gg = random3( p+vec3(0.0,1.0,1.0) );
    vec3 gh = random3( p+vec3(1.0,1.0,1.0) );
    
    // projections
    float va = dot( ga, w-vec3(0.0,0.0,0.0) );
    float vb = dot( gb, w-vec3(1.0,0.0,0.0) );
    float vc = dot( gc, w-vec3(0.0,1.0,0.0) );
    float vd = dot( gd, w-vec3(1.0,1.0,0.0) );
    float ve = dot( ge, w-vec3(0.0,0.0,1.0) );
    float vf = dot( gf, w-vec3(1.0,0.0,1.0) );
    float vg = dot( gg, w-vec3(0.0,1.0,1.0) );
    float vh = dot( gh, w-vec3(1.0,1.0,1.0) );
	
    // interpolation
    float v = va + 
              u.x*(vb-va) + 
              u.y*(vc-va) + 
              u.z*(ve-va) + 
              u.x*u.y*(va-vb-vc+vd) + 
              u.y*u.z*(va-vc-ve+vg) + 
              u.z*u.x*(va-vb-ve+vf) + 
              u.x*u.y*u.z*(-va+vb+vc-vd+ve-vf-vg+vh);
              
    vec3 d = ga + 
             u.x*(gb-ga) + 
             u.y*(gc-ga) + 
             u.z*(ge-ga) + 
             u.x*u.y*(ga-gb-gc+gd) + 
             u.y*u.z*(ga-gc-ge+gg) + 
             u.z*u.x*(ga-gb-ge+gf) + 
             u.x*u.y*u.z*(-ga+gb+gc-gd+ge-gf-gg+gh) +   
             
             du * (vec3(vb-va,vc-va,ve-va) + 
                   u.yzx*vec3(va-vb-vc+vd,va-vc-ve+vg,va-vb-ve+vf) + 
                   u.zxy*vec3(va-vb-ve+vf,va-vb-vc+vd,va-vc-ve+vg) + 
                   u.yzx*u.zxy*(-va+vb+vc-vd+ve-vf-vg+vh));
                   
    return vec4( v, d );                   
}

vec3 gfbm(vec2 st) {
	vec3 sum = vec3(0.f);
	float lacunarity = 1.5;
	float gain = 0.2;

	float amp = 0.5f; // ampliture
	float mod = 1.f; // modulation
	for(int i = 0; i < 3; ++i) {
		// sum += amp * noised(mod * st);
		vec3 nd = noised(mod * st);
		sum.x += amp * nd.x;
		sum.yz += amp * mod * nd.yz;
		mod *= lacunarity;
		amp *= gain;
		st = lacunarity * st;
	}

	return sum;
}

vec4 gfbm(vec3 st) {
	vec4 sum = vec4(0.f);
	float lacunarity = 2.0;
	float gain = 0.5;

	float amp = 0.5f; // ampliture
	float mod = 1.f; // modulation
	for(int i = 0; i < 3; ++i) {
		// vec3 nd = noised(mod * st);
		// sum.x += amp * nd.x;
		// sum.yz += amp * mod * nd.yz;
		sum += noised(mod * st);
		mod *= lacunarity;
		amp *= gain;
		st = lacunarity * st;
	}

	return sum;
}

vec3 mfbm(vec2 st, vec2 dom) {
	vec3 sum = vec3(0.f);
	float lacunarity = 2.0;
	float gain = 0.3;

	float amp = 0.5f; // ampliture
	float mod = 1.f; // modulation
	for(int i = 0; i < 4; ++i) {
		vec3 tmp = amp * noisedp(mod * st, dom);
		tmp.yz *= mod;
		sum += tmp;
		mod *= lacunarity;
		dom *= lacunarity;
		amp *= gain;
	}

	return sum;
}

const float pi = 3.1415;
// Transform of coords on unit sphere to spherical coordinates (theta, phi)
vec2 sph2(vec3 pos) {
	// TODO: undefined output in that case...
	// if(abs(pos.y) > 0.999) {
	// 	return vec2(0, (pos.y > 0) ? 0.0001 : pi);
	// }
	float theta = atan(pos.z, pos.x);
	float phi = atan(length(pos.xz), pos.y);
	return vec2(theta, phi);
}

// Derivation of spherical coordinates in euclidean space with respect to theta.
vec3 sph_dtheta(float radius, float theta, float phi) {
	float sp = sin(phi);
	return radius * vec3(-sin(theta) * sp, 0, cos(theta) * sp);
}

// Derivation of spherical coordinates in euclidean space with respect to phi.
vec3 sph_dphi(float radius, float theta, float phi) {
	float cp = cos(phi);
	float sp = sin(phi);
	return radius * vec3(cos(theta) * cp, -sp, sin(theta) * cp);
}

vec3 displace(vec3 pos, out vec3 normal, out float height) {
	const float radius = 5.0;

	pos = normalize(pos);
	vec2 tp = sph2(pos);

	float theta = tp[0];
	float phi = tp[1];
	float fac = 3 * sqrt(radius);

	// TODO
	float freq = 4;
	// float sp = sin(phi);
	float sp = 1;
	vec2 mod1 = freq * vec2(sp, 1);
	vec2 dom = freq * vec2(2 * sp, 1) * pi;
	if(dom.x < 1.001) {
		// mod1.x = 0.0;
		// dom.x = 1.0001;
	}
	vec3 off = (fac / freq) * mfbm(mod1 * vec2(theta, phi), dom);
	off.y = cos(phi) * off.y;
	off.yz *= freq;

	vec3 dphi = sph_dphi(radius, theta, phi);
	vec3 dtheta = sph_dtheta(radius, theta, phi);
	vec3 nn = cross(
		(1 + off.x) * dtheta + off.y * pos, 
		(1 + off.x) * dphi + off.z * pos);
	// vec3 nn = vec3(tp, 1.0);
	// vec3 nn = cross(dtheta, dphi);
	if(abs(pos.y) > 0.999) {
		nn = vec3(0, sign(pos.y), 0);
	}

	pos = radius * pos;
	pos += off.x * normalize(pos);
	height = off.x;

	normal = normalize(nn);
	return pos;
}


// clamps to valid range (e.g. < 31)
float distanceToLOD(float z) {
	// TODO: don't hardcode this stuff
	const float fov = 0.35 * 3.141;
	const float targetPixelSize = 1.f;
	// const float screenResolution = 30.f;
	const float screenResolution = 300.f; 
	float s = z * tan(fov / 2);
	float tmp = s * targetPixelSize / screenResolution;

	// should be 2.0 for all triangles to have equal size
	// if e.g. you want to have a focus on near triangles
	//   make it larger than 2.0, otherwise smaller.
	float fac = 2;
	return clamp(-fac * log2(clamp(tmp, 0.0, 1.0)), 1, 30);
}

