stage_inc = [
	include_directories('../../include'),
	include_directories('..'),
	external_inc,
]

# tinygltf
gltf_lib = library('tinygltf',
	sources: 'tinygltf.cpp',
	include_directories: stage_inc)

gltf_dep = declare_dependency(
	link_with: gltf_lib,
	include_directories: stage_inc)

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
  dep_turbojpeg,
  dep_png,
  gltf_dep,
]

stage_src = [
	'app.cpp',
	'audio.cpp',
	# 'render.cpp',
	'window.cpp',
	'camera.cpp',
	'geometry.cpp',
	'physics.cpp',
	# 'texture.cpp',
	'texture2.cpp',
	'image.cpp',

	'scene/scene.cpp',
	'scene/material.cpp',
	'scene/primitive.cpp',
	'scene/shape.cpp',
	'scene/light.cpp',
	'scene/skybox.cpp',

	stage_shaders,
]

stage_lib = library('stage',
	sources: stage_src,
	dependencies: stage_deps,
	include_directories: stage_inc)

stage_dep = declare_dependency(
	link_with: stage_lib,
	dependencies: stage_deps,
	include_directories: stage_inc)

