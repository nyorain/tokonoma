#ifndef FXAA_REDUCE_MIN
    #define FXAA_REDUCE_MIN (1.0/ 128.0)
#endif
#ifndef FXAA_REDUCE_MUL
    #define FXAA_REDUCE_MUL (1.0 / 8.0)
#endif
#ifndef FXAA_SPAN_MAX
    #define FXAA_SPAN_MAX 8.0
#endif

vec4 fxaa(sampler2D tex, vec2 uvm, vec2 uvnw, vec2 uvne, vec2 uvsw, vec2 uvse) {
	const vec3 luma = vec3(0.299, 0.587, 0.114);
	vec2 invSize = 1.0 / textureSize(tex, 0);

    vec3 rgbNW = texture(tex, uvnw).rgb;
    vec3 rgbNE = texture(tex, uvne).rgb;
    vec3 rgbSW = texture(tex, uvsw).rgb;
    vec3 rgbSE = texture(tex, uvse).rgb;
    vec4 texColor = texture(tex, uvm);
    vec3 rgbM  = texColor.rgb;
    float lumaNW = dot(rgbNW, luma);
    float lumaNE = dot(rgbNE, luma);
    float lumaSW = dot(rgbSW, luma);
    float lumaSE = dot(rgbSE, luma);
    float lumaM  = dot(rgbM,  luma);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    
    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));
    
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) *
                          (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
    
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = min(vec2(FXAA_SPAN_MAX, FXAA_SPAN_MAX),
              max(vec2(-FXAA_SPAN_MAX, -FXAA_SPAN_MAX),
              dir * rcpDirMin)) * invSize;
    
    vec3 rgbA = 0.5 * (
        texture(tex, uvm + dir * (1.0 / 3.0 - 0.5)).xyz +
        texture(tex, uvm + dir * (2.0 / 3.0 - 0.5)).xyz);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        texture(tex, uvm + dir * -0.5).xyz +
        texture(tex, uvm + dir * 0.5).xyz);

    float lumaB = dot(rgbB, luma);
    if ((lumaB < lumaMin) || (lumaB > lumaMax)) {
        return vec4(rgbA, texColor.a);
	} else {
        return vec4(rgbB, texColor.a);
	}
}

vec4 fxaa(sampler2D tex, vec2 fragCoord) {
	vec2 invSize = 1.0 / textureSize(tex, 0);
	vec2 uv = fragCoord * invSize;
	vec2 uvnw = (fragCoord + vec2(-1.0, -1.0)) * invSize;
	vec2 uvne = (fragCoord + vec2(1.0, -1.0)) * invSize;
	vec2 uvsw = (fragCoord + vec2(-1.0, 1.0)) * invSize;
	vec2 uvse = (fragCoord + vec2(1.0, 1.0)) * invSize;
	return fxaa(tex, uv, uvnw, uvne, uvsw, uvse);
}
