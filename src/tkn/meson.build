tkn_inc = [
	include_directories('../../include'),
	include_directories('..'),
	external_inc,
]

# tinygltf
gltf_lib = library('tinygltf',
	sources: 'tinygltf.cpp',
	include_directories: tkn_inc)

gltf_dep = declare_dependency(
	link_with: gltf_lib,
	include_directories: tkn_inc)

# libtkn
dep_soundio = meson.get_compiler('cpp').find_library('soundio', dirs: libpath)
dep_b2d = meson.get_compiler('cpp').find_library('Box2D', dirs: libpath)
tkn_deps = [
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

tkn_src = [
	'app.cpp',
	'audio.cpp',
	'window.cpp',
	'camera.cpp',
	'geometry.cpp',
	'physics.cpp',
	'texture.cpp',
	'render.cpp',
	'headless.cpp',
	'defer.cpp',
	'f16.cpp',
	'util.cpp',

	'scene/scene.cpp',
	'scene/material.cpp',
	'scene/shape.cpp',
	'scene/light.cpp',
	'scene/environment.cpp',
	'scene/pbr.cpp',

	'image.cpp',
	'image/jpeg_turbo.cpp',
	'image/png.cpp',
	'image/ktx.cpp',

	tkn_shaders,
]

tkn_args = []
if build_machine.system() == 'linux'
	tkn_args += '-DDOI_LINUX'
endif

tkn_lib = library('tkn',
	sources: tkn_src,
	dependencies: tkn_deps,
	cpp_args: tkn_args,
	include_directories: tkn_inc)

tkn_dep = declare_dependency(
	link_with: tkn_lib,
	dependencies: tkn_deps,
	include_directories: tkn_inc)
