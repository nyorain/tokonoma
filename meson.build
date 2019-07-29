project('tokonoma', ['cpp', 'c'],
	license: 'BSD',
	version: '0.1.0',
	meson_version: '>=0.47',
	default_options: [
		'cpp_std=c++1z',
		'c_std=c99',
  		'warning_level=3',
		# TODO: re-enable
  		'werror=false'])

# options
profiling = get_option('profiling')
if profiling
  add_global_arguments('-pg', language: 'cpp')
endif

# warnings
warnings = [
	'-Wall',
	'-Wextra',
	'-Wpedantic',
	'-Wduplicated-cond',
	'-Wrestrict',
	'-Wnull-dereference',
	'-Wundef',
	'-Wlogical-op',

	# workaround for ny
	'-Wno-non-virtual-dtor',

	# woraround for asio
	'-Wno-undef'
]

# default arrguments
# warnings and stuff
cc = meson.get_compiler('cpp')
add_project_arguments(
	cc.get_supported_arguments(warnings),
	language: 'cpp')

# mainly needed for shaders and assets and stuff
add_project_arguments(
	'-DDOI_BASE_DIR="@0@"'.format(meson.current_source_dir()),
	language: 'cpp')

# project-specific stuff
source_root = '/'.join(meson.source_root().split('\\'))
flag_dlg = '-DDLG_BASE_PATH="' + source_root + '/"'
add_project_arguments(flag_dlg, language: 'cpp')
add_project_arguments('-DASIO_STANDALONE', language: 'cpp')

dep_vpp = dependency('vpp', fallback: ['vpp', 'vpp_dep'])
dep_nytl = dependency('nytl MASTER', fallback: ['nytl', 'nytl_dep'])
dep_dlg = dependency('dlg MASTER', fallback: ['dlg', 'dlg_dep'])
dep_ny = dependency('ny', fallback: ['ny', 'ny_dep'])
dep_rvg = dependency('rvg', fallback: ['rvg', 'rvg_dep'])
dep_vui = dependency('vui', fallback: ['vui', 'vui_dep'])
dep_vulkan = dependency('vulkan')
dep_turbojpeg = dependency('libturbojpeg', fallback: ['turbojpeg', 'jpeg_dep'])
dep_png = dependency('libpng', fallback: ['png', 'png_dep'])

external_inc = include_directories('external/include')
libpath = join_paths(meson.source_root(), 'external', 'lib', build_machine.system())
message('libpath: ' + libpath)

subdir('src/shaders')
subdir('src/tkn')
subdir('docs/tests')

# projects
subdir('src/smooth_shadow')
subdir('src/pursuers')
subdir('src/fluids')
subdir('src/mists')
subdir('src/fun')
subdir('src/sentient')
subdir('src/sss')
subdir('src/iro')
subdir('src/br')
subdir('src/lpgi')
subdir('src/taa')
subdir('src/deferred')
subdir('src/cloth')

# inline (for small projects)
executable('pendulum',
	'src/pendulum/main.cpp',
	dependencies: tkn_dep)

executable('dpend',
	'src/dpend/main.cpp',
	dependencies: tkn_dep)

executable('audio',
	'src/audio/main.cpp',
	dependencies: tkn_dep)

executable('dummy',
	'src/dummy/main.cpp',
	dependencies: tkn_dep)

executable('guitest',
	'src/guitest/main.cpp',
	dependencies: tkn_dep)

executable('particles', [
		'src/particles/main.cpp',
		particles_shaders,
	], dependencies: tkn_dep)

executable('automaton', [
	  'src/automaton/main.cpp',
	  'src/automaton/automaton.cpp',
	  automaton_shaders,
	], dependencies: tkn_dep)

executable('normals', [
		'src/normals/main.cpp',
		normals_shaders,
	], dependencies: tkn_dep)

executable('sviewer', [
		'src/sviewer/main.cpp',
	], dependencies: tkn_dep)

executable('curves', [
		'src/curves/main.cpp',
	], dependencies: tkn_dep)

executable('ps', [
		'src/ps/main.cpp',
		ps_shaders,
	], dependencies: tkn_dep)

executable('sen', [
		'src/sen/main.cpp',
		sen_shaders,
	], dependencies: tkn_dep)

executable('pbr', [
		'src/pbr/main.cpp',
		tkn_shaders,
	], dependencies: tkn_dep)

executable('iv',
	'src/iv/main.cpp',
	dependencies: tkn_dep)

executable('shv', [
		'src/shv/main.cpp',
		shv_shaders,
	], dependencies: tkn_dep)

executable('volume', [
		'src/volume/main.cpp',
		volume_shaders,
	], dependencies: tkn_dep)

executable('bezier', [
		'src/bezier/main.cpp',
		bezier_shaders,
	], dependencies: tkn_dep)

executable('tta',
	'src/playground/tta.cpp',
	dependencies: tkn_dep)

executable('terrain',
	'src/playground/terrain.cpp',
	dependencies: tkn_dep)

executable('pg',
	'src/playground/main6.cpp',
	dependencies: tkn_dep)
