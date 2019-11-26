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
tkn_deps = [
	dep_vpp,
	dep_ny,
	dep_nytl,
	dep_dlg,
	dep_rvg,
	dep_vui,
	dep_vulkan,
	dep_png,
	gltf_dep,
]

tkn_src = [
	'app.cpp',
	'window.cpp',
	'camera.cpp',
	'geometry.cpp',
	'texture.cpp',
	'render.cpp',
	'headless.cpp',
	'defer.cpp',
	'f16.cpp',
	'util.cpp',
	'headeronly.cpp',

	'scene/scene.cpp',
	'scene/material.cpp',
	'scene/shape.cpp',
	'scene/light.cpp',
	'scene/environment.cpp',
	'scene/pbr.cpp',

	'image.cpp',
	'image/png.cpp',
	'image/ktx.cpp',

	tkn_shaders,
]

if dep_turbojpeg.found()
	tkn_src += 'image/jpeg_turbo.cpp'
	tkn_deps += dep_turbojpeg
else
	tkn_src += 'image/jpeg_dummy.cpp'
endif

tkn_audio = false
if dep_cubeb.found()
	tkn_src += [
		'audio.cpp',
		'sound.cpp',
	]
	tkn_deps += dep_cubeb
	tkn_audio = true
endif

if dep_steamaudio.found()
	tkn_src += [
		'audio3D.cpp',
	]
	tkn_deps += dep_steamaudio
endif

if dep_b2d.found()
	tkn_src += 'physics.cpp'
	tkn_deps += dep_b2d
endif

tkn_args = []
if build_machine.system() == 'linux'
	tkn_args += '-DTKN_LINUX'
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
