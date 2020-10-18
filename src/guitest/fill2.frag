#version 460

#extension GL_GOOGLE_include_directive : require
#include "layout.glsl"

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec2 inPaintPos;
layout(location = 3) in vec4 inColor;
layout(location = 4) in flat uint inCmdIndex;

layout(location = 0) out vec4 outColor;

void doClipping(int planeStart, int planeCount) {
#ifdef VERTEX_CLIP_DISTANCE
	// If vertex plane clipping is supported, we already performed clipping
	// for the first planes (the maximum supported number) in the vertex
	// shader. Ideally, we won't have to do any clipping here.
	planeStart += int(maxClipPlanes);
	planeCount -= int(maxClipPlanes);
#endif // VERTEX_CLIP_DISTANCE

	while(planeCount > 0) {
		vec3 plane = clipPlanes[planeStart].xyz;
		if(dot(plane.xy, inPos) - plane.z < 0.0) {
			discard;
		}

		++planeStart;
		--planeCount;
	}
}

vec4 paintColor(vec2 coords, PaintData paint, vec4 col) {
	uint type = uint(paint.transform[3][3]);
	if(type == paintTypeColor) {
		return paint.inner;
	} else if(type == paintTypeLinGrad) {
		vec2 start = paint.custom.xy;
		vec2 end = paint.custom.zw;
		vec2 dir = end - start;
		float fac = dot(coords - start, dir) / dot(dir, dir);
		return mix(paint.inner, paint.outer, clamp(fac, 0, 1));
	} else if(type == paintTypeRadGrad) {
		vec2 center = paint.custom.xy;
		float r1 = paint.custom.z;
		float r2 = paint.custom.w;
		float fac = (length(coords - center) - r1) / (r2 - r1);
		return mix(paint.inner, paint.outer, clamp(fac, 0, 1));
	} else if(type == paintTypeTexRGBA) {
		return paint.inner * texture(textures[uint(paint.custom.r)], coords);
	} else if(type == paintTypeTexA) {
		return paint.inner * texture(textures[uint(paint.custom.r)], coords).a;
	} else if(type == paintTypePointColor) {
		return col.rgba;
	}

	// Invalid paint type, return easy-to-spot debug color (purple)
	return vec4(1, 0, 1, 1);
}

void main() {
	DrawCommand cmd = cmds[inCmdIndex];
	doClipping(int(cmd.clipStart), int(cmd.clipCount));

	PaintData paint = paints[cmd.paint];

	outColor = paintColor(inPaintPos, paint, inColor);
	if(cmd.type == drawTypeText) {
		outColor.a *= texture(fontAtlas, inUV).a;
	}

	if(cmd.type == drawTypeStroke) {
		float fw = cmd.uvFadeWidth;
		// reference/alternative formulation for y antialiasing:
		// min(1.0, 1.0 - (abs(inUV.y) - (1 - 1 / fw)) * fw)
		float aaFacY = min(1.0, (1.0 - abs(inUV.y)) * fw);
		float aaFacX = inUV.x;

		outColor.a *= aaFacX * aaFacY;
		// outColor.rgb = vec3(aaFacX * abs(inUV.y));
		// outColor.a = 1.0;
	}
}
