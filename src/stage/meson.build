# libstage
dep_soundio = meson.get_compiler('cpp').find_library('soundio', dirs: libpath)
dep_b2d = meson.get_compiler('cpp').find_library('Box2D', dirs: libpath)
stage_deps = [
  dep_vpp,
  dep_ny,
  dep_nytl,
  dep_dlg,
  dep_rvg,
  dep_vui,
  dep_vulkan,
  dep_soundio,
  dep_b2d,
]

stage_src = [
	'render.cpp',
	'window.cpp',
	'geometry.cpp',
	'app.cpp',
	'audio.cpp',
	'physics.cpp',
	'texture.cpp',
]

stage_inc = [
	include_directories('../../include'),
	include_directories('..'),
	external_inc,
]

stage_lib = library('stage',
	sources: stage_src,
	dependencies: stage_deps,
	include_directories: stage_inc)

stage_dep = declare_dependency(
	link_with: stage_lib,
	dependencies: stage_deps,
	include_directories: stage_inc)

