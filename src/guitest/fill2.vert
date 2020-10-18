#version 460

#extension GL_GOOGLE_include_directive : require
#include "layout.glsl"

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec2 outPos;
layout(location = 1) out vec2 outUV;
layout(location = 2) out vec2 outPaintPos;
layout(location = 3) out vec4 outColor;
layout(location = 4) out flat uint outCmdIndex;

//////////////////////////////////////////////////////////////////////////////
// - Multidraw implementation -
layout(push_constant) uniform DrawIDPCR {
	// Rendering viewport size
	layout(offset = 0) uvec2 targetSize;
	// We always pass the offset into the bindings buffer (cmds) as push constant.
	// When multidraw is supported, we use gl_DrawID to determine the draw
	// id that we add to the general offer. But since vulkan does not require 
	// multidraw support, we need a custom workaround. In this case we simply update
	// the offset push constant before each draw call.
	// Either way, we will define 'uint cmdIndex' that holds the command id.
	layout(offset = 8) uint cmdOffset;
};

#ifdef MULTIDRAW

uint cmdIndex = cmdOffset + gl_DrawID;

#else // MULTIDRAW

uint cmdIndex = cmdOffset;

#endif // MULTIDRAW


//////////////////////////////////////////////////////////////////////////////
// - Plane clipping implementation -
// The featuer is not required by vulkan and not supported by all
// implementations. We will use it if available (and the number of
// clip planes is small enough) though since this is faster
// than manually doing it in the fragment shader.
// Note how even if the number of planes is too large for gl_ClipDistance
// and we have to clip again in the fragment shader, we already clip what
// we can here.
#ifdef VERTEX_CLIP_DISTANCE
	out float gl_ClipDistance[maxClipPlanes];

	void clipDists(uint planeStart, uint planeCount) {
		// constant loop, can be unrolled
		for(uint i = 0u; i < maxClipPlanes; ++i) {
			if(i >= planeCount) {
				gl_ClipDistance[i] = 1.f;
			} else {
				vec4 plane = clipPlanes[planeStart + i];	
				gl_ClipDistance[i] = dot(plane.xy, inPos) - plane.z;
			}
		}
	}

#else // VERTEX_CLIP_DISTANCE

	void clipDists(uint planeStart, uint planeCount) {}

#endif // VERTEX_CLIP_DISTANCE

//////////////////////////////////////////////////////////////////////////////

void main() {
	outUV = inUV;
	outCmdIndex = cmdIndex;
	outPos = inPos;

	DrawCommand cmd = cmds[cmdIndex];
	clipDists(cmd.clipStart, cmd.clipCount);

	vec4 pos = transforms[cmd.transform] * vec4(inPos, 0.0, 1.0);
	pos.xy = -1 + 2 * pos.xy / targetSize;
	gl_Position = pos;

	vec3 ppos = mat3(paints[cmd.paint].transform) * vec3(inPos, 1.0);
	outPaintPos = ppos.xy / ppos.z;

	// fill.frag expects *all* colors in linear space.
	// polygon specifies that it - as everything in rvg - expects colors
	// in srgb space so we have to linearize it here.
	vec3 rgb = toLinearColor(inColor.rgb);
	outColor = vec4(rgb, inColor.a);
}
