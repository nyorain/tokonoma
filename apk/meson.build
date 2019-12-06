apks = {
	'rays': rays,
	'fluid': fluid,
	'particles': particles,
	'volume': volume,
	'br': br,
	'daudio': daudio,
	'daudiorc': daudiorc,
	'daudiosin': daudiosin,
	'daudioravg': daudioravg,
}

setupsh = files('setup.sh')
buildsh = files('build.sh')

arch = host_machine.cpu()
bt = meson.get_cross_property('tkn_android_buildtools')
platform = meson.get_cross_property('tkn_android_platform')
suffix = meson.get_cross_property('tkn_android_suffix')

# NOTE: workaround for https://github.com/mesonbuild/meson/issues/3589
# seems like inputs from files() are always passed as relative paths.
# But we change the folder in the build script, breaking those paths
# manifest = meson.current_source_dir() / 'AndroidManifest32.xml.in'
manifest = meson.current_source_dir() / 'AndroidManifest.xml.in'
font = meson.current_source_dir() / '..' / 'assets' / 'Roboto-Regular.ttf'

setup_apk = custom_target('setup_apk',
	command: [setupsh,
		arch,
		bt,
		platform,
		manifest,
		'@OUTDIR@',
		font,
	],
	output: ['AndroidManifest.xml.in', 'base.tkn.apk'],
	# NOTE: kinda redundant, pass them as args?
	# all deps listed here but tkn will be relinked when
	# any other dependency changes so that shouldn't be a problem
	depends: tkn_lib,
	depend_files: [manifest, font],
)

foreach name, mod : apks
	pname = name + '_apk'
	apk = custom_target(pname,
		command: [buildsh,
			arch,
			bt,
			platform,
			# '@INPUT@',
			mod.full_path(),
			'@OUTDIR@',
			suffix,
		],
		depends: [setup_apk, mod],
		output: name + '.tkn.apk',
	)
	set_variable(pname, apk)
endforeach
