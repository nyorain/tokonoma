# libstage
stage_inc = include_directories('../../include')

stage_deps = [
  dep_vpp,
  dep_ny,
  dep_nytl,
  dep_dlg,
  dep_vulkan
]

stage_src = [
	'render.cpp',
	'window.cpp',
]

stage_lib = library('stage',
	sources: stage_src,
	dependencies: stage_deps,
	include_directories: [stage_inc])

stage_dep = declare_dependency(
	link_with: stage_lib,
	dependencies: stage_deps,
	include_directories: stage_inc)

