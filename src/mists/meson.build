dep_b2d = meson.get_compiler('cpp').find_library('Box2D', dirs: libpath)

# exe
mists_src = [
	'main.cpp',
	'physics.cpp',
]

executable('mists',
	sources: mists_src,
	include_directories: external_inc,
	dependencies: [stage_dep, dep_b2d])
