# NOTE: the shaders in the sviewer/ subfolder are not compiled here,
# but dynamically loaded (to allow reloading)
add_project_arguments(
	'-DDOI_SHADER_SRC="@0@"'.format(meson.current_source_dir()),
	language: 'cpp')

# common/shader glsl files
include_glsl = files(
  'include/geometry.glsl',
  'include/util.glsl',
  'include/hex.glsl',
  'include/math.glsl',
  'include/color.glsl',
  'include/noise.glsl',
  'include/ray.glsl',
)

# shader specifications
# contains multiple shader groups, specified like this:
# [ <group var>, [ <shader source>, { <dependencies> }], {...} ]
# a variable named <group var> will afterwards contain the compiled
# shader headers
shaders_src = [
	['common_shaders', [
		['fullscreen.vert'],
		['fullscreen_transform.vert'],
		['incolor.frag'],
		['texture.frag'],
		['simple.vert'],
	]],
	['smooth_shadow_shaders', [
		['light.vert'],
		['light.frag', include_glsl],
		['shadow.vert', include_glsl],
		['shadow.frag', include_glsl],
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
		['fluid_texture.frag'],
	]],
	['automaton_shaders', [
		['hex.vert', include_glsl],
		['hex_line.vert', include_glsl],
		['predprey.comp'],
		['ant.comp'],
	]],
	['particle_shaders', [
		['particle.vert'],
		['particle.frag'],
		['particle.comp'],
	]],
	['normals_shaders', [
		['texn.frag'],
	]],
	['ps_shaders', [
		['ps.vert'],
		['ps.frag'],
		# ['ps.comp'], # always dynamically compiled
	]],
	['sentient_shaders', [
		['sentient.vert'],
		['sentient.frag'],
	]],
	['sss_shaders', [
		['sss_shadow.vert'],
		['sss_shadow.frag'],
	]],
	['ddd_shaders', [
		['model.vert'],
		['model.frag'],
		['shadowmap.vert'],
		['skybox.vert'],
		['skybox.frag'],
	]],
	['iro_shaders', [
		['iro.vert', include_glsl],
		['iro.frag'],
		['iro_building.vert'],
		['iro_texture.frag'],
		['iro.comp', include_glsl],
		['iro_outline.vert', include_glsl],
		['iro_outline.frag', include_glsl],
	]],
	['sen_shaders', [
	  ['sen.frag', include_glsl],
	  ['senr.vert'],
	  ['senr.frag'],
	  ['senpt.frag', include_glsl],
	  ['sen.comp', include_glsl],
	]]
]

# build them
shader_inc_dir = meson.current_source_dir() + '/include'
glslang = find_program('glslangValidator')
foreach shader_group : shaders_src
	group_name = shader_group[0]
	shaders = get_variable(group_name, [])

	foreach shader : shader_group[1]
		file = shader[0]
		name = shader.get(2, file)

		data_name = name.underscorify() + '_data'
		args = [glslang, '-V', '@INPUT@',
			'-o', '@OUTPUT@',
			'--vn', data_name,
			'-I' + shader_inc_dir]
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
