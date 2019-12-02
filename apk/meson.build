setupsh = files('setup.sh')
buildsh = files('build.sh')

# NOTE: workaround for https://github.com/mesonbuild/meson/issues/3589
# seems like inputs from files() are always passed as relative paths.
# But we change the folder in the build script, breaking those paths
# manifest = meson.current_source_dir() / 'AndroidManifest.xml.in'
manifest = meson.current_source_dir() / 'AndroidManifest32.xml.in'
font = meson.current_source_dir() / '..' / 'assets' / 'Roboto-Regular.ttf'
setup_apk = custom_target('setup_apk',
	command: [setupsh, manifest, '@OUTDIR@', font],
	output: ['AndroidManifest.xml.in', 'base.tkn.apk'],
	# TODO: kinda redundant, pass them as args?
	# also not all deps listed here
	depends: tkn_lib,
	depend_files: manifest,
)

apks = {
	'rays': rays,
	'fluid': fluid,
	'particles': particles,
	'volume': volume,
	'br': br,
}

foreach name, mod : apks
	pname = name + '_apk'
	apk = custom_target(pname,
		command: [buildsh, '@INPUT@', '@OUTDIR@'],
		depends: [setup_apk, mod],
		input: mod.full_path(),
		output: name + '.tkn.apk',
	)
	set_variable(pname, apk)
endforeach
