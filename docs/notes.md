Todo
=====

- Re-enable Werror
- Fix all meson errors
- Make sure to use TKN_BASE_DIR instead of hardcoding asset paths
- Remove most of the external/libs/ stuff.
  Include them as meson subprojects instead.
  Box2D has meson build files, for speex and cubeb i probably have
  to create them myself.
  	- Not sure if the current solution of seperating speex and
	  cubeb is optimal. Cubeb already has the speex symbols,
	  just doesn't export them. Probably best to combine them.
	- Also make sure to include SSE and Neon in the speex build
	  currently probably not included due to config
- Fix turbojpeg dep on non-linux platforms
	- either write own meson.build and include as subproject (couldn't
	  find meson wrap) or fall back to libjpeg instead.
	  Can't hurt to have a jpeg implementation using libjpeg?
	  But otoh stb can already load jpeg well enough.
	  Then just make sure it's disabled cleanly, output a meson warning.
- why do we compile tinygltf into own lib? at least make that
  lib static i guess...
- could add a (cpu-)profiler again, would be really useful in
  some cases.
  	- could re-add easy_profiler, that seems alright i guess
