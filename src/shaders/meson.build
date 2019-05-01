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

scene_glsl = files(
	'include/pbr.glsl',
	'include/scene.glsl',
	'include/scene.frag.glsl',
)

deferred_glsl = include_glsl + scene_glsl + files(
	'../deferred/light.glsl'
)

# shader specifications
# contains multiple shader groups, specified like this:
# { 'name': [['shader.vert', glsl_inc, 'outname.vert', ['-DDEF_STH']], ... ], ... }
# everything but the shader source is optional. The last argument (array)
# contains additional arguments for glslangValidator (like defines).
# The shader will be looked for in '../<groupname>/<shadername>'
shaders_src = {
	'stage': [
		['shaders/shadowmap.vert', scene_glsl],
		['shaders/shadowmap.frag', scene_glsl],
		['shaders/shadowmapCube.vert', scene_glsl],
		['shaders/shadowmapCube.frag', scene_glsl],
		['shaders/skybox.vert'],
		['shaders/skybox.frag'],
		['shaders/fullscreen.vert'],
		['shaders/fullscreen_transform.vert'],
		['shaders/equirectToCube.frag'],
		['shaders/incolor.frag'],
		['shaders/texture.frag'],
		['shaders/simple.vert'],
	],
	'smooth_shadow': [
		['light.vert'],
		['light.frag', include_glsl],
		['shadow.vert', include_glsl],
		['shadow.frag', include_glsl],
		['light_pp.frag'],
	],
	'pursuers': [
		['line.vert'],
		['line.frag'],
	],
	'fluids': [
		['advect.comp', [], 'advect.vel.comp', ['-DVELOCITY']],
		['advect.comp', [], 'advect.dens.comp', ['-DDENSITY']],
		['pressure.comp'],
		['divergence.comp'],
		['project.comp'],
		['diffuse.comp', [], 'diffuse.dens.comp'],
		['fluid_texture.frag'],
	],
	'automaton': [
		['hex.vert', include_glsl],
		['hex_line.vert', include_glsl],
		['predprey.comp'],
		['ant.comp'],
	],
	'particles': [
		['particle.vert'],
		['particle.frag'],
		['particle.comp'],
	],
	'normals': [
		['texn.frag'],
	],
	'ps': [
		['ps.vert'],
		['ps.frag'],
		# ['ps.comp'], # always dynamically compiled
	],
	'sentient': [
		['sentient.vert'],
		['sentient.frag'],
	],
	'sss': [
		['sss_shadow.vert'],
		['sss_shadow.frag'],
	],
	'ddd': [
		['model.vert', scene_glsl],
		['model.frag', scene_glsl],
	],
	'iro': [
		['iro.vert', include_glsl],
		['iro.frag'],
		['iro_building.vert'],
		['iro_texture.frag'],
		['iro.comp', include_glsl],
		['iro_outline.vert', include_glsl],
		['iro_outline.frag', include_glsl],
	],
	'sen': [
	  ['sen.frag', include_glsl],
	  ['senr.vert'],
	  ['senr.frag'],
	  ['senpt.frag', include_glsl],
	  ['sen.comp', include_glsl],
	],
	'lith': [
	  ['lith.frag', include_glsl],
	  ['lith.vert'],
	  ['lith.comp', include_glsl],
	],
	'deferred': [
	  ['gbuf.vert', deferred_glsl],
	  ['gbuf.frag', deferred_glsl],
	  ['pp.frag', deferred_glsl],
	  ['pointLight.frag', deferred_glsl],
	  ['pointLight.vert', deferred_glsl],
	  ['dirLight.frag', deferred_glsl],
	  ['ssao.frag', deferred_glsl],
	  ['pointScatter.frag', deferred_glsl],
	  ['dirScatter.frag', deferred_glsl],
	  ['gblur.comp', deferred_glsl],
	  ['ssr.comp', deferred_glsl],
	],
}

# build them
shader_inc_dir = meson.current_source_dir() + '/include'
glslang = find_program('glslangValidator')
foreach group, sources : shaders_src
	shaders = get_variable(group + '_shaders', [])

	foreach shader : sources
		# file: full (relative) filename of shader
		# name: name of output shader, defaults to input name
		file = '../' + group + '/' + shader[0]
		name = shader.get(2, shader[0])

		# replace shaders/ dir if it exists in shader path
		# allows projects to group shaders in a 'shader/' dir, like
		# stage itself does it
		name = ''.join(name.split('shaders/'))

		# if we allow additional subfolders in name we could add
		# something that works like 'name.replace('/', '.')'
		# name = '.'.join(name.split('/'))

		data_name = group + '_' + name.underscorify() + '_data'
		args = [glslang, '-V', '@INPUT@',
			'-o', '@OUTPUT@',
			'--target-env', 'vulkan1.1',
			'--vn', data_name,
			'-I' + shader_inc_dir]
		args += shader.get(3, [])

		header = custom_target(
			group + '.' + name + '.spv',
			output: group + '.' + name + '.h',
			input: files(file),
			depend_files: shader.get(1, []),
			command: args)

		shaders += [header]
	endforeach

	set_variable(group + '_shaders', shaders)
endforeach
