setupsh = files('setup.sh')
buildsh = files('build.sh')

setup_apk = custom_target('setup_apk',
	command: [setupsh],
	input: files('AndroidManifest.xml.in'),
)

rays_apk = custom_target('rays_apk',
	command: [buildsh, ],
	depends: setup_apk,
)
