# common/shader glsl files
geom_glsl = files('geometry.glsl')

# shader specifications
# contains multiple shader groups, specified like this:
# [ <group var>, [ <shader source>, { <dependencies> }], {...} ]
# a variable named <group var> will afterwards contain the compiled
# shader headers
shaders_src = [
	['common_shaders', [
		['fullscreen.vert'],
	]],
	['smooth_shadow_shaders', [
		['light.vert'],
		['light.frag', geom_glsl],
		['shadow.vert', geom_glsl],
		['shadow.frag', geom_glsl],
		['light_pp.frag'],
	]],
	['pursuers_shaders', [
		['line.vert'],
		['line.frag'],
	]]
]

# build them
glslang = find_program('glslangValidator')
foreach shader_group : shaders_src
	group_name = shader_group[0]
	shaders = get_variable(group_name, [])

	foreach shader : shader_group[1]
		s = shader[0]
		name = s.underscorify() + '_data'
		args = [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@', '--vn', name]
		header = custom_target(
			s + '_spv',
			output: s + '.h',
			input: s,
			depend_files: shader.get(1, []),
			command: args)

		shaders += [header]
	endforeach

	set_variable(group_name, shaders)
endforeach
