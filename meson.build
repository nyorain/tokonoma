project('tokonoma', ['cpp', 'c'],
	license: 'BSD',
	version: '0.1.0',
	meson_version: '>=0.51',
	default_options: [
		'cpp_std=c++17',
		'c_std=c11',
  		'warning_level=3',
  		'werror=false'])

# options
profiling = get_option('profiling')
with_playground = get_option('playground')

if profiling
  add_global_arguments('-pg', language: 'cpp')
endif

# dummy/playground projects use this dependency.
# quick way to disable building them at all
if with_playground
	dep_playground = []
else
	dep_playground = disabler()
endif

cc = meson.get_compiler('cpp')
android = (cc.get_define('__ANDROID__') != '')
exetarget = 'executable'

if android
	aapiv = cc.get_define('__ANDROID_API__')

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
	# shared_module instead of shared_library so that they export 'main'
	# which is needed by ny
	exetarget = 'shared_module'
endif

# warnings
warnings = [
	'-Wpedantic',
	'-Wduplicated-cond',
	'-Wrestrict',
	'-Wnull-dereference',
	'-Wundef',
	'-Wlogical-op',

	# for clang
	'-Wno-missing-braces',

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
defs = [
	'-DDLG_BASE_PATH="' + source_root + '/"',
	# '-DNYTL_BYTES_ASSERT(x)=dlg_assert(x)',
]
add_project_arguments(defs, language: 'cpp')
add_project_arguments('-DASIO_STANDALONE', language: 'cpp')

# required dependencies
# TODO: remove ugly MASTER hacks that prevent installed version to be found
dep_vpp = dependency('vpp', fallback: ['vpp', 'vpp_dep'])
dep_nytl = dependency('nytl MASTER', fallback: ['nytl', 'nytl_dep'])
dep_dlg = dependency('dlg MASTER', fallback: ['dlg', 'dlg_dep'])
dep_rvg = dependency('rvg', fallback: ['rvg', 'rvg_dep'])
dep_vui = dependency('vui', fallback: ['vui', 'vui_dep'])
dep_vulkan = dependency('vulkan')
dep_png = dependency('libpng', fallback: ['png', 'png_dep'])
dep_swa = dependency('swa', fallback: ['swa', 'swa_dep'])
dep_glslang = dependency('glslang', fallback: ['glslang', 'glslang_dep'])

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
	disabler: true,
	required: get_option('b2d'))
with_b2d = dep_b2d.found()

# bullet for 3D physics
# optional as well, the wrap i wrote ships pre-built binaries (since
# writing meson.build files for it would be quite some work i guess)
# but not for every platform.
dep_bullet = dependency('bullet',
	fallback: ['bullet', 'bullet_dep'],
	disabler: true,
	required: get_option('bullet'))
with_bullet = dep_bullet.found()

# cubeb for audio
# when not available (e.g. not possible to build), the audio
# projects simply won't be built
dep_cubeb = dependency('cubeb',
	fallback: ['cubeb', 'cubeb_dep'],
	disabler: true,
	required: get_option('audio'))
with_audio = dep_cubeb.found()

# steamaudio, optional, for 3D audio processing
# the library isn't free/libre or open source!
# But has a fairly permissive license.
dep_steamaudio = disabler()
if with_audio
	dep_steamaudio = dependency('steamaudio',
		fallback: ['steamaudio', 'steamaudio_dep'],
		disabler: true,
		required: get_option('steamaudio'))

endif
with_audio3D = dep_steamaudio.found()

# pulseaudio, optional, for linux audio recording/interaction
dep_pulse_simple = dependency('libpulse-simple',
	required: false,
	disabler: true)
dep_pulse_simple_optional = []
if dep_pulse_simple.found()
	dep_pulse_simple_optional = [dep_pulse_simple]
endif

cd = configuration_data()
cd.set('TKN_LINUX', build_machine.system() == 'linux')
cd.set('TKN_WITH_AUDIO', with_audio)
cd.set('TKN_WITH_AUDIO3D', with_audio3D)
cd.set('TKN_WITH_BULLET', with_bullet)
cd.set('TKN_WITH_B2D', with_b2d)
cd.set('TKN_WITH_PULSE_SIMPLE', dep_pulse_simple.found())
subdir('include/tkn')

external_inc = include_directories('external/include')

subdir('src/shaders')
subdir('src/tkn')
subdir('docs/tests')

# projects
subdir('src/smooth_shadow')
subdir('src/pursuers')
subdir('src/fluids')
subdir('src/mists')
subdir('src/sss')
subdir('src/iro')
subdir('src/br')
subdir('src/dm')
subdir('src/lpgi')
subdir('src/taa')
subdir('src/deferred')
subdir('src/cloth')

# playground/dummy projects
if with_playground
	subdir('src/playground')
endif

# erase shader dependencies only needed on android
if not android
	atmosphere_shaders = [atmosphere_shaders[0]]
	tess_shaders = []
	subd_shaders = []
endif

# inline (for small projects)
executable('pendulum',
	'src/pendulum/main.cpp',
	dependencies: tkn_dep)

executable('dpend',
	'src/dpend/main.cpp',
	dependencies: tkn_dep)

executable('soundfont',
	'src/soundfont/main.cpp',
	dependencies: [tkn_dep, tkn_audio_dep])

executable('dummy',
	'src/dummy/main.cpp',
	dependencies: tkn_dep)

executable('guitest',
	'src/guitest/main.cpp',
	dependencies: [tkn_dep, dep_playground])

executable('rvgscene', [
		'src/guitest/rvgscene.cpp',
		'src/guitest/scene.cpp',
		'src/guitest/buffer.cpp',
		'src/guitest/context.cpp',
		guitest_shaders,
	], dependencies: [tkn_dep, dep_playground])

particles = build_target('particles', [
		'src/particles/main.cpp',
		particles_shaders,
	], dependencies: [
		tkn_dep,
		tkn_audio_dep_optional,
		dep_pulse_simple_optional
	], target_type: exetarget)

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

executable('visualizer', [
		'src/visualizer/main.cpp',
	], dependencies: [tkn_dep, tkn_audio_dep])

executable('ps', [
		'src/ps/main.cpp',
		ps_shaders,
	], dependencies: tkn_dep)

executable('sen', [
		'src/sen/main.cpp',
		sen_shaders,
	], dependencies: tkn_dep)

executable('gen', [
		'src/gen/main.cpp',
		tkn_shaders,
	], dependencies: tkn_dep)

executable('gen_ltc', [
		'src/gen/fitLTC/fit.cpp',
		tkn_shaders,
	], dependencies: tkn_dep,
	cpp_args: ['-O3', '-march=native'])

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

bezier = build_target('bezier', [
		'src/bezier/main.cpp',
		bezier_shaders,
	], dependencies: tkn_dep,
	target_type: exetarget)

executable('tta',
	'src/playground/tta.cpp',
	dependencies: tkn_dep)

atmosphere = build_target('atmosphere', [
		'src/atmosphere/main.cpp',
		atmosphere_shaders,
	], dependencies: tkn_dep,
	target_type: exetarget)

fem = build_target('fem', [
		'src/fem/main.cpp',
		fem_shaders,
	], dependencies: tkn_dep,
	target_type: exetarget)

hair = build_target('hair', [
		'src/hair/main.cpp',
	], dependencies: tkn_dep,
	target_type: exetarget)

repro = build_target('repro', [
		'src/repro/main.cpp',
		repro_shaders,
	], dependencies: tkn_dep,
	target_type: exetarget)

rays = build_target('rays', [
		'src/rays/main.cpp',
	], dependencies: tkn_dep,
	target_type: exetarget)

tess = build_target('tess', [
		'src/tess/main.cpp',
		tess_shaders,
	], dependencies: [tkn_dep, dep_playground],
	target_type: exetarget)

subd = build_target('subd', [
		'src/subd/main.cpp',
		subd_shaders,
	], dependencies: [tkn_dep],
	target_type: exetarget)

scatter = build_target('scatter', [
		'src/scatter/main.cpp',
		scatter_shaders,
	], dependencies: [tkn_dep],
	target_type: exetarget)

clouds = build_target('clouds', [
		'src/clouds/main.cpp',
	], dependencies: [tkn_dep],
	target_type: exetarget)

sky = build_target('sky', [
		'src/sky/main.cpp',
	], dependencies: [tkn_dep],
	target_type: exetarget)

oscil = build_target('oscil', [
		'src/oscil/main.cpp',
		oscil_shaders,
	], dependencies: [tkn_dep, tkn_audio_dep, dep_pulse_simple_optional],
	target_type: exetarget)

planet = build_target('planet', [
		'src/planet/main.cpp',
		planet_shaders,
	], dependencies: [tkn_dep],
	target_type: exetarget)

# TODO: no longer needed
planet_gen = build_target('planet_gen', [
		'src/planet/gen.cpp',
		planet_shaders,
	], dependencies: [tkn_dep],
	target_type: exetarget)

terrain = build_target('terrain', [
		'src/terrain/main.cpp',
		'src/terrain/atmosphere.cpp',
	], dependencies: [tkn_dep],
	target_type: exetarget)

if android
	subdir('apk')
endif
