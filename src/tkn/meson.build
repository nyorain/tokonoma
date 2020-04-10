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
	dep_swa,
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
	'qcamera.cpp',
	'geometry.cpp',
	'texture.cpp',
	'render.cpp',
	'headless.cpp',
	'defer.cpp',
	'f16.cpp',
	'util.cpp',
	'headeronly.cpp',
	'timeWidget.cpp',

	'scene/scene.cpp',
	'scene/material.cpp',
	'scene/shape.cpp',
	'scene/light.cpp',
	'scene/environment.cpp',
	'scene/pbr.cpp',

	'image.cpp',
	'image/png.cpp',
	'image/ktx.cpp',

	'kissfft/kiss_fft.c',

	'app2.cpp',
	'singlePassApp.cpp',

	tkn_shaders,
]

# TODO: remove that. use own library
# but tbh, the whole component is shit anyways
if dep_b2d.found()
	tkn_src += 'physics.cpp'
	tkn_deps += dep_b2d
endif

if dep_turbojpeg.found()
	tkn_src += 'image/jpeg_turbo.cpp'
	tkn_deps += dep_turbojpeg
else
	tkn_src += 'image/jpeg_dummy.cpp'
endif

tkn_lib = library('tkn',
	sources: tkn_src,
	dependencies: tkn_deps,
	include_directories: tkn_inc)

tkn_dep = declare_dependency(
	link_with: tkn_lib,
	dependencies: tkn_deps,
	include_directories: tkn_inc)

# additional/optional libraries
tkn_audio_dep = disabler()
tkn_audio_dep_optional = []
if with_audio
	tkn_audio_lib = static_library('tkn_audio',
		sources: [
			'audio.cpp',
			'sound.cpp',
			'sampling.cpp',
		],
		dependencies: [tkn_dep, dep_cubeb]
	)

	tkn_audio_dep = declare_dependency(
		link_with: tkn_audio_lib,
		dependencies: dep_cubeb,
		include_directories: tkn_inc)
	tkn_audio_dep_optional = [tkn_audio_dep]
endif

tkn_audio3D_dep = disabler()
tkn_audio3D_dep_optional = []
if with_audio3D
	tkn_audio3D_lib = static_library('tkn_audio3D',
		sources: ['audio3D.cpp'],
		dependencies: [tkn_dep, tkn_audio_dep, dep_steamaudio]
	)

	tkn_audio3D_dep = declare_dependency(
		link_with: tkn_audio3D_lib,
		dependencies: dep_steamaudio,
		include_directories: tkn_inc)

	tkn_audio3D_dep_optional = [tkn_audio3D_dep]
endif

