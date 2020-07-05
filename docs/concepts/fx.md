- define techniques in separate qwe file?

```
technique1:
	stages:
		# alternatively, pass spv modules
		# we probably want to use a compile cache anyways
		# just hash the source strings I guess
		# Maybe also allow to specify shader defines/compiler params
		# here
		vertex: model.vert
		fragment: model.frag
	specialization:
		...
	depthStencil:	
		depthTestEnable: true
		depthWriteEnable: true
		depthCompareOp: lessOrEqual
	assembly:
		topology: triangleList
	rasterization: 
		polygonMode: fill
	vertex:
		bindings:
			-
				rate: instance
				stride: sizeof(Vec3f)
			-
				rate: vertex
				stride: 32
		attributes:
			-
				binding: 0
				# alternatively, format: vec4
				format: r32g32b32a32Sfloat
			-
				binding: 1
				format: dvec4
				offset: 4
```

main problem of having multiple shader programs/stages in
	one file: how to declare inputs/descriptors etc
There probably isn't an advantage to just using one file that
makes solving this valuable. Solving this would (in some way or
another) involve glsl/spirv codegen, which is at least not trivial.
We already have shader includes, should be enough to provide everything
we get from using a single file for multiple stages.

What we could do: declare inputs/outputs in the technique file.
Has the advantage that we don't declare them redundantly in
multiple stages.
