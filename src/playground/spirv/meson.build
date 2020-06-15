spirv_src = files('main.cpp')

spirv_as = find_program('spirv-as')
if spirv_as.found()
	cmd = [spirv_as,
		'--target-env', 'spv1.3',
		'-o', '@OUTPUT@',
		'@INPUT@']
	spirv_src += custom_target(
		'comp.spv',
		output: 'comp.spv',
		input: 'comp.spvasm',
		command: cmd)

	executable('spirv',
		sources: spirv_src,
		dependencies: tkn_dep)
else
	warning('Couldn\'t find spirv-as, not building spirv application')
endif
