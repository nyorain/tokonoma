Todo
=====

- Re-enable Werror
- Fix all meson errors
- Make sure to use TKN_BASE_DIR instead of hardcoding asset paths
- Remove the remaining external/libs/ stuff.
  Include them as meson subprojects instead.
  Box2D has a meson wrap, use it.
- Make sure to include SSE and Neon in the speex build
  currently probably not included due to config
  (in own meson.build of cubeb)
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
