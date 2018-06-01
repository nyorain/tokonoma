fluid_software_src = [
	'software.cpp',
]

executable('software',
	sources: fluid_software_src,
	dependencies: stage_dep)

fluid_src = [
	common_shaders,
	fluid_shaders,
	'main.cpp'
]

executable('fluid',
	sources: fluid_src,
	dependencies: stage_dep)
