setupsh = files('setup.sh')
buildsh = find_program('build.sh')
manifest = files('AndroidManifest.xml.in')

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
