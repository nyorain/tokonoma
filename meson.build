project('stage', ['cpp', 'c'],
	license: 'BSD',
	version: '0.1.0',
	meson_version: '>=0.43',
	default_options: [
		'cpp_std=c++1z',
		'c_std=c99',
  		'warning_level=3',
  		'werror=true'])

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
]

# default arrguments
# warnings and stuff
cc = meson.get_compiler('cpp')
add_project_arguments(
	cc.get_supported_arguments(warnings),
	language: 'cpp')

# project-specific stuff
source_root = '/'.join(meson.source_root().split('\\'))
flag_dlg = '-DDLG_BASE_PATH="' + source_root + '/"'
add_project_arguments(flag_dlg, language: 'cpp')

dep_vpp = dependency('vpp', fallback: ['vpp', 'vpp_dep'])
dep_nytl = dependency('nytl MASTER', fallback: ['nytl', 'nytl_dep'])
dep_dlg = dependency('dlg MASTER', fallback: ['dlg', 'dlg_dep'])
dep_ny = dependency('ny', fallback: ['ny', 'ny_dep'])
dep_rvg = dependency('rvg', fallback: ['rvg', 'rvg_dep'])
dep_vui = dependency('vui', fallback: ['vui', 'vui_dep'])
dep_vulkan = dependency('vulkan')

external_inc = include_directories('external/include')
libpath = join_paths(meson.source_root(), 'external', 'lib', build_machine.system())
message('libpath: ' + libpath)

subdir('src/shaders')
subdir('src/stage')

# projects
subdir('src/smooth-shadow')
subdir('src/pursuers')
subdir('src/fluids')
subdir('src/mists')
subdir('src/fun')

# inline (for small projects)
executable('pendulum',
	'src/pendulum/main.cpp',
	dependencies: stage_dep)

executable('dpend',
	'src/dpend/main.cpp',
	dependencies: stage_dep)

executable('audio',
	'src/audio/main.cpp',
	dependencies: stage_dep)

executable('dummy',
	'src/dummy/main.cpp',
	dependencies: stage_dep)

executable('automaton', [
	  'src/automaton/main.cpp',
	  'src/automaton/automaton.cpp',
	  automaton_shaders,
	  common_shaders,
	], dependencies: stage_dep)

# executable('pg',
# 	'src/playground/main2.cpp',
# 	dependencies: stage_dep)
