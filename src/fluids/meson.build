fluid_software_src = [
	'software.cpp',
]

executable('software',
	sources: fluid_software_src,
	dependencies: tkn_dep)

fluid_src = [
	fluids_shaders,
	'main.cpp'
]

fluid = build_target('fluid',
	sources: fluid_src,
	dependencies: tkn_dep,
	target_type: exetarget)
