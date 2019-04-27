layout(set = 3, binding = 0) uniform SSAOSamplerBuf {
	vec4 samples[ssaoSampleCount];
} ssao;
layout(set = 3, binding = 1) uniform sampler2D noiseTex;
