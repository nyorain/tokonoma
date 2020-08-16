// Bicubic filtering
// https://stackoverflow.com/questions/13501081
vec4 cubic(float v) {
    vec4 n = vec4(1.0, 2.0, 3.0, 4.0) - v;
    vec4 s = n * n * n;
    float x = s.x;
    float y = s.y - 4.0 * s.x;
    float z = s.z - 4.0 * s.y + 6.0 * s.x;
    float w = 6.0 - x - y - z;
    return vec4(x, y, z, w) * (1.0 / 6.0);
}

vec4 textureBicubic(sampler2D tex, vec2 texCoords) {
   vec2 texSize = textureSize(tex, 0);
   vec2 invTexSize = 1.0 / texSize;

   texCoords = texCoords * texSize - 0.5;

    vec2 fxy = fract(texCoords);
    texCoords -= fxy;

    vec4 xcubic = cubic(fxy.x);
    vec4 ycubic = cubic(fxy.y);

    vec4 c = texCoords.xxyy + vec2 (-0.5, +1.5).xyxy;

    vec4 s = vec4(xcubic.xz + xcubic.yw, ycubic.xz + ycubic.yw);
    vec4 offset = c + vec4 (xcubic.yw, ycubic.yw) / s;

    offset *= invTexSize.xxyy;

    vec4 sample0 = texture(tex, offset.xz);
    vec4 sample1 = texture(tex, offset.yz);
    vec4 sample2 = texture(tex, offset.xw);
    vec4 sample3 = texture(tex, offset.yw);

    float sx = s.x / (s.x + s.y);
    float sy = s.z / (s.z + s.w);

    return mix(mix(sample3, sample2, sx), mix(sample1, sample0, sx), sy);
}

// Reference implementation using 16 samples (hermite)
// See e.g. https://www.shadertoy.com/view/MllSzX
vec4 cubicLagrange(vec4 A, vec4 B, vec4 C, vec4 D, float t) {
	const float c_x0 = -1.0;
	const float c_x1 =  0.0;
	const float c_x2 =  1.0;
	const float c_x3 =  2.0;

    return
        A * (
            (t - c_x1) / (c_x0 - c_x1) * 
            (t - c_x2) / (c_x0 - c_x2) *
            (t - c_x3) / (c_x0 - c_x3)
        ) + B * (
            (t - c_x0) / (c_x1 - c_x0) * 
            (t - c_x2) / (c_x1 - c_x2) *
            (t - c_x3) / (c_x1 - c_x3)
        ) + C * (
            (t - c_x0) / (c_x2 - c_x0) * 
            (t - c_x1) / (c_x2 - c_x1) *
            (t - c_x3) / (c_x2 - c_x3)
        ) +  D * (
            (t - c_x0) / (c_x3 - c_x0) * 
            (t - c_x1) / (c_x3 - c_x1) *
            (t - c_x2) / (c_x3 - c_x2)
        );
}

vec4 cubicHermite(vec4 A, vec4 B, vec4 C, vec4 D, float t) {
	float t2 = t * t;
    float t3 = t2 * t;
    vec4 a = 0.5 * (-A + 3.0 * B - 3.0 * C + D);
    vec4 b = A + 0.5 * (-5.0 * B + 4.0 * C - D);
    vec4 c = 0.5 * (-A + C);
   	vec4 d = B;
    return a * t3 + b * t2 + c * t + d;
}

vec4 cubic(vec4 A, vec4 B, vec4 C, vec4 D, float t) {
	// return cubicLagrange(A, B, C, D, t);
	return cubicHermite(A, B, C, D, t);
}

vec4 textureBicubicCatmull(sampler2D tex, vec2 texCoords) {
	ivec2 texSize = textureSize(tex, 0);
	vec2 texelSize = 1.f / texSize;
    vec2 pixel = texCoords * texSize + 0.5;
    
    vec2 frac = fract(pixel);
    pixel = floor(pixel) / texSize - vec2(texelSize / 2.0);
    
    vec4 C00 = texture(tex, pixel + vec2(-1, -1) * texelSize);
    vec4 C10 = texture(tex, pixel + vec2( 0, -1) * texelSize);
    vec4 C20 = texture(tex, pixel + vec2( 1, -1) * texelSize);
    vec4 C30 = texture(tex, pixel + vec2( 2, -1) * texelSize);
    
    vec4 C01 = texture(tex, pixel + vec2(-1, 0) * texelSize);
    vec4 C11 = texture(tex, pixel + vec2( 0, 0) * texelSize);
    vec4 C21 = texture(tex, pixel + vec2( 1, 0) * texelSize);
    vec4 C31 = texture(tex, pixel + vec2( 2, 0) * texelSize);
    
    vec4 C02 = texture(tex, pixel + vec2(-1, 1) * texelSize);
    vec4 C12 = texture(tex, pixel + vec2( 0, 1) * texelSize);
    vec4 C22 = texture(tex, pixel + vec2( 1, 1) * texelSize);
    vec4 C32 = texture(tex, pixel + vec2( 2, 1) * texelSize);
    
    vec4 C03 = texture(tex, pixel + vec2(-1, 2) * texelSize);
    vec4 C13 = texture(tex, pixel + vec2( 0, 2) * texelSize);
    vec4 C23 = texture(tex, pixel + vec2( 1, 2) * texelSize);
    vec4 C33 = texture(tex, pixel + vec2( 2, 2) * texelSize);
    
    vec4 CP0X = cubic(C00, C10, C20, C30, frac.x);
    vec4 CP1X = cubic(C01, C11, C21, C31, frac.x);
    vec4 CP2X = cubic(C02, C12, C22, C32, frac.x);
    vec4 CP3X = cubic(C03, C13, C23, C33, frac.x);
    
    return cubic(CP0X, CP1X, CP2X, CP3X, frac.y);
}
