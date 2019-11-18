setupsh = files('setup.sh')
buildsh = files('build.sh')

# NOTE: workaround for https://github.com/mesonbuild/meson/issues/3589
# seems like inputs from files() are always passed as relative paths.
# But we change the folder in the build script, breaking those paths
manifest = meson.current_source_dir() / 'AndroidManifest.xml.in'
setup_apk = custom_target('setup_apk',
	command: [setupsh, manifest, '@OUTDIR@'],
	output: ['AndroidManifest.xml.in', 'base.tkn.apk'],
)

apks = {
	'rays': rays,
	'fluid': fluid,
}

foreach name, mod : apks
	pname = name + '_apk'
	apk = custom_target(pname,
		command: [buildsh, '@INPUT@', '@OUTDIR@'],
		depends: setup_apk,
		input: mod.full_path(),
		output: name + '.tkn.apk',
	)
	set_variable(pname, apk)
endforeach
