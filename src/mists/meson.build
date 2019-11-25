if dep_b2d.found()
	mists_src = [
		'main.cpp',
		'physics.cpp',
	]

	executable('mists',
		sources: mists_src,
		include_directories: external_inc,
		dependencies: [tkn_dep, dep_b2d])
endif
