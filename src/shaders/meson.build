shaders_dep = [
	files('geometry.glsl')
]

shaders_src = [
	'light.vert',
	'light.frag',
	'shadow.vert',
	'shadow.frag'
]

shaders = []
glslang = find_program('glslangValidator')

foreach shader : shaders_src
	name = shader.underscorify() + '_data'
	args = [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@', '--vn', name]
	header = custom_target(
		shader + '_spv',
		output: shader + '.h',
		input: shader,
		depend_files: shaders_dep,
		command: args)

	shaders += [header]
endforeach
