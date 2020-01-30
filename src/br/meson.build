br_deps = [tkn_dep, gltf_dep]

# optional dependency on steamaudio for 3d audio
if tkn_audio3D_dep.found()
	br_deps += [tkn_audio3D_dep]
endif

br = build_target('br', [
		'main.cpp',
		br_shaders,
	], dependencies: br_deps,
	target_type: exetarget)



