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
		['incolor.frag'],
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
	]],
	['fluid_shaders', [
		['advect.comp', [], 'advect.vel.comp', ['-DVELOCITY']],
		['advect.comp', [], 'advect.dens.comp', ['-DDENSITY']],
		['pressure.comp'],
		['divergence.comp'],
		['project.comp'],
		['diffuse.comp', [], 'diffuse.dens.comp'],
		['texture.frag'],
	]],
	['automaton_shaders', [
		['hex.vert'],
		['predprey.comp'],
	]],

]

# build them
glslang = find_program('glslangValidator')
foreach shader_group : shaders_src
	group_name = shader_group[0]
	shaders = get_variable(group_name, [])

	foreach shader : shader_group[1]
		file = shader[0]
		name = shader.get(2, file)

		data_name = name.underscorify() + '_data'
		args = [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@', '--vn', data_name]
		args += shader.get(3, [])

		header = custom_target(
			name + '_spv',
			output: name + '.h',
			input: file,
			depend_files: shader.get(1, []),
			command: args)

		shaders += [header]
	endforeach

	set_variable(group_name, shaders)
endforeach
