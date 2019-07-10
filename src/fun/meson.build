dep_b2d = meson.get_compiler('cpp').find_library('Box2D', dirs: libpath)

# exe
fun_src = [
	'main.cpp',
]

executable('fun',
	sources: fun_src,
	include_directories: external_inc,
	dependencies: [tkn_dep, dep_b2d])

