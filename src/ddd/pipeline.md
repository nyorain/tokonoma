see src/stage/gltf/pipeline.md for material/primitive

vertex input:
	- buffer 0:
		- position (always)
		- normal (always)
	- buffer 1:
		- uv (may be garbage if material uses no textures)
		  in that case we e.g. just rebind the position buffer as uv buffer
		  we always load textures in the fragment shader but if the
		  uv values are garbage, a 1-pixel repeat texture is loaded anyways

fragment input:
	- inPos: position in world space
	- inNormal: forwarded normal
	- inUV: uv coord (might be garbage, see uv vertex input)
	- inLightPos: position in light space of first light
	  required for shadow map lookup
	
scene/per-level:
	- expects descriptor 0 for per-scene data like vertex ubo, lights
	  and viewer position
