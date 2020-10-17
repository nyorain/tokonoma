const uint drawTypeDefault = 0u;
const uint drawTypeText = 1u;
const uint drawTypeStroke = 2u;

const uint paintTypeColor = 1u;
const uint paintTypeLinGrad = 2u;
const uint paintTypeRadGrad = 3u;
const uint paintTypeTexRGBA = 4u;
const uint paintTypeTexA = 5u;
const uint paintTypePointColor = 6u;

layout(constant_id = 0) const uint maxTextures = 15;

layout(set = 0, binding = 0) buffer ClipSet {
	vec4 clipPlanes[];
};

layout(set = 0, binding = 1, row_major) buffer TransformSet {
	mat4 transforms[];
};

struct PaintData {
	vec4 inner;
	vec4 outer;
	vec4 custom;
	mat4 transform;
};

layout(set = 0, binding = 2, row_major) buffer PaintSet {
	PaintData paints[];
};

layout(set = 0, binding = 3) uniform sampler2D textures[maxTextures];

struct DrawCommand {
	uint transform;
	uint paint;
	uint clipStart;
	uint clipCount;
	uint type;
	float uvFadeWidth;
	uvec2 pad;
};

layout(set = 0, binding = 4) buffer DrawCommands {
	DrawCommand cmds[];
};

layout(set = 0, binding = 5) uniform sampler2D fontAtlas;


// - util -
// Approximations using constant gamma.
// Unless it's ping-pong mapping for just doing something in another
// space (e.g. dithering for quantization), use the real conversion
// functions below.
vec3 toLinearColorCheap(vec3 nonlinear) {
	return pow(nonlinear, vec3(2.2));
}

vec3 toNonlinearColorCheap(vec3 linear) {
	return pow(linear, vec3(1 / 2.2));
}

vec3 toNonlinearColor(vec3 linear) {
	vec3 a = step(linear, vec3(0.0031308));
	vec3 clin = 12.92 * linear;
	vec3 cgamma = 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055;
	return mix(clin, cgamma, a);
}

vec3 toLinearColor(vec3 nonlinear) {
	vec3 a = step(nonlinear, vec3(0.04045));
	vec3 clin = nonlinear / 12.92;
	vec3 cgamma = pow((nonlinear + 0.055) / 1.055, vec3(2.4));
	return mix(clin, cgamma, a);
}

#ifdef VERTEX_CLIP_DISTANCE
	// Vulkan requires implementation that support the feature to support
	// at least 8 planes.
	layout(constant_id = 1) const uint maxClipPlanes = 8;
#endif // VERTEX_CLIP_DISTANCE
