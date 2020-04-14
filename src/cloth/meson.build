# runs simulation on the cpu, i.e. optimizations and openmp are really
# important here to allow a reasonably sized cloth grid
mp_dep = dependency('openmp')
executable('cloth-software', [
		'software.cpp',
		tkn_shaders,
	], dependencies: [tkn_dep, mp_dep],
	cpp_args: '-O3')

# runs the simulation on the gpu via compute shaders
executable('cloth', [
		'main.cpp',
		tkn_shaders,
		cloth_shaders,
	], dependencies: [tkn_dep])

