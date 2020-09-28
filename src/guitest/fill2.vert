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
// When indirect multidraw is supported by the device, we will use it.
// But since vulkan does not require it, we need a custom workaround. In
// this case we simply use a push constant.
// Either way, we will define 'uint cmdIndex' that holds the command id.
#ifndef MULTIDRAW

layout(push_constant) uniform DrawID {
	uint cmdIndex;
};

#else // MULTIDRAW

/*const*/ uint cmdIndex = gl_DrawID;

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
				vec3 plane = clipPlanes[planeStart + i];	
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
	gl_Position = pos;

	vec3 ppos = mat3(paints[cmd.paint].transform) * vec3(inPos, 1.0);
	outPaintPos = ppos.xy / ppos.z;

	// fill.frag expects *all* colors in linear space.
	// polygon specifies that it - as everything in rvg - expects colors
	// in srgb space so we have to linearize it here.
	vec3 rgb = toLinearColor(inColor.rgb);
	outColor = vec4(rgb, inColor.a);
}
