project('tokonoma', ['cpp', 'c'],
	license: 'BSD',
	version: '0.1.0',
	meson_version: '>=0.51',
	default_options: [
		'cpp_std=c++17',
		'c_std=c99',
  		'warning_level=3',
		# TODO: re-enable
  		'werror=false'])

# options
profiling = get_option('profiling')
if profiling
  add_global_arguments('-pg', language: 'cpp')
endif

cc = meson.get_compiler('cpp')
android = (cc.get_define('__ANDROID__') != '')
exetarget = 'executable'

if android
	# Make sure that we dispatch vulkan dynamically, that is needed
	# on android. I guess libvulkan are just stubs for everything
	# but instance creation proc address loading
	add_global_arguments('-DVKPP_DYNAMIC_DISPATCH', language: ['cpp', 'c'])

	# See https://github.com/android/ndk/issues/289
	add_global_link_arguments([
		'-lgcc',
		'-Wl,--exclude-libs,libgcc_real.a',
		'-Wl,--exclude-libs,libunwind.a',
	], language: ['cpp', 'c'])

	# there are no native executables for apk's on android
	# shared_module instead of shared_library so that export 'main'
	# which is needed by ny
	exetarget = 'shared_module'
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
add_project_arguments(
	cc.get_supported_arguments(warnings),
	language: 'cpp')

# mainly needed for shaders and assets and stuff
source_root = '/'.join(meson.source_root().split('\\'))
add_project_arguments(
	'-DTKN_BASE_DIR="@0@"'.format(source_root),
	language: 'cpp')

# project-specific stuff
flag_dlg = '-DDLG_BASE_PATH="' + source_root + '/"'
add_project_arguments(flag_dlg, language: 'cpp')
add_project_arguments('-DASIO_STANDALONE', language: 'cpp')

# required dependencies
dep_vpp = dependency('vpp', fallback: ['vpp', 'vpp_dep'])
dep_nytl = dependency('nytl MASTER', fallback: ['nytl', 'nytl_dep'])
dep_dlg = dependency('dlg MASTER', fallback: ['dlg', 'dlg_dep'])
dep_rvg = dependency('rvg', fallback: ['rvg', 'rvg_dep'])
dep_vui = dependency('vui', fallback: ['vui', 'vui_dep'])
dep_vulkan = dependency('vulkan')
dep_png = dependency('libpng', fallback: ['png', 'png_dep'])
dep_ny = dependency('ny',
	fallback: ['ny', 'ny_dep'],
	default_options: 'android=@0@'.format(android))

# turbojpeg is optional, there exists no meson wrap for it.
# if we can't find it, we just fall back to the stbi loader, i.e. make
# tkn::JpegLoader a dummy that never works.
# fall back to default libjpeg, add (non-turbo) libjpeg loader?
dep_turbojpeg = dependency('libturbojpeg',
	required: false,
	fallback: ['turbojpeg', 'jpeg_dep'])

# box2D is not required, we simply don't build 2D physics stuff if not found
# We don't pass 'Box2D' as name to dependency because we don't to use
# the system-installed version. We use features that were added since
# the last release (which was 2014 as of now)
dep_b2d = dependency('b2d',
	fallback: ['b2d', 'b2d_dep'],
	required: false)

# bullet for 3D physics
# optional as well, the wrap i wrote ships pre-built binaries (since
# writing meson.build files for it would be quite some work i guess)
# but not for every platform.
dep_bullet = dependency('bullet',
	fallback: ['bullet', 'bullet_dep'],
	required: false)

# cubeb for audio
dep_cubeb = dependency('cubeb',
	fallback: ['cubeb', 'cubeb_dep'],
	required: false)

# steamaudio, optional, for 3D audio processing
# the library isn't free/libre or open source!
# But has a fairly permissive license.
dep_steamaudio = dependency('steamaudio',
	fallback: ['steamaudio', 'steamaudio_dep'],
	required: false)

external_inc = include_directories('external/include')
# libpath = join_paths(meson.source_root(), 'external', 'lib', host_machine.system())
# message('libpath: ' + libpath)

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
subdir('src/spirv')

# inline (for small projects)
executable('pendulum',
	'src/pendulum/main.cpp',
	dependencies: tkn_dep)

executable('dpend',
	'src/dpend/main.cpp',
	dependencies: tkn_dep)

if tkn_audio
	executable('audio',
		'src/audio/main.cpp',
		dependencies: tkn_dep)

	executable('soundfont',
		'src/soundfont/main.cpp',
		dependencies: tkn_dep)
endif

executable('dummy',
	'src/dummy/main.cpp',
	dependencies: tkn_dep)

executable('guitest',
	'src/guitest/main.cpp',
	dependencies: tkn_dep)

particles = build_target('particles', [
		'src/particles/main.cpp',
		particles_shaders,
	], dependencies: tkn_dep,
	target_type: exetarget)

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

volume = build_target('volume', [
		'src/volume/main.cpp',
		volume_shaders,
	], dependencies: tkn_dep,
	target_type: exetarget)

executable('bezier', [
		'src/bezier/main.cpp',
		bezier_shaders,
	], dependencies: tkn_dep)

executable('tta',
	'src/playground/tta.cpp',
	dependencies: tkn_dep)

executable('terrain', [
		'src/terrain/main.cpp',
		terrain_shaders,
	], dependencies: tkn_dep)

executable('fem', [
		'src/fem/main.cpp',
		fem_shaders,
	], dependencies: tkn_dep)

executable('hair', [
		'src/hair/main.cpp',
	], dependencies: tkn_dep)

executable('repro', [
		'src/repro/main.cpp',
		repro_shaders,
	], dependencies: tkn_dep)

rays = build_target('rays', [
		'src/rays/main.cpp',
		rays_shaders,
	], dependencies: tkn_dep,
	target_type: exetarget)

executable('pg',
	'src/playground/main6.cpp',
	dependencies: tkn_dep)

if dep_bullet.found()
	executable('pg-bullet',
		'src/playground/bullet.cpp',
		dependencies: [tkn_dep, dep_bullet])
endif

if dep_steamaudio.found()
	executable('pg-steamaudio',
		'src/playground/steamaudio.cpp',
		dependencies: [tkn_dep, dep_steamaudio])
endif

if android
	subdir('apk')
endif
