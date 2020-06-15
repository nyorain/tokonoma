Todo
=====

- Fix all meson warnings
- Make sure to use TKN_BASE_DIR instead of hardcoding asset paths
- Make sure to include SSE and Neon in the speex (cubeb) build
  currently probably not included due to config
  (in own meson.build of cubeb)
- why do we compile tinygltf into own lib? at least make that
  lib static i guess...
- could add a (cpu-)profiler again, would be really useful in
  some cases.
  	- could re-add easy_profiler, that seems alright i guess

- split tkn into smaller, static libraries for dependencies
	- e.g. libtkn-audio libtkn-audio3D libtkn-physics2D libtkn-physics3D
	  and so on. Make them optional. tkn/br shouldn't fail to compile
	  because something is wrong with bullet, Box2D or some audio code.
	- allows to use/propagate disabler objects from the dependencies

## Full Android support

- test audio with simply dummy audio-only app (that creates a window though)
	- make sure all AudioSoruce implementations can read from memory/custom
	  android asset file objects
- implement image loading (especially ktx for envmap/brdflut) from
  android assets. Allow loading from buffer.
  Also allow loading from callback (or just use std::FILE? as that is
  already a decent abstraction and most loaders use fopen internally
  anyways?)
- factor out the touch controls from tkn:volume into tkn/camera.hpp.
  we should probably keep the both methods; name them 'direct' and 'continous'
  or something like this
  	- make input curves somewhat configurable? At least factor and power?
- add abstraction layer (simple function(s) in tkn::App?) for loading assets.
  On desktop, it uses TKN_BASE_DIR, on android it uses the AppContext's
  asset manager
- Automatically switch between 32-bit (arm v7) and 64-bit android builds,
  abolish manual switching (see AndroidManifest32.xml.in) in apk/meson.build.
  Also propagate the arch to build.sh and setup.sh
- add android_fopen utility
- for 32-bit: use hacks as base for real fixes in vpp for 32-bit vulkan.
  Somewhat nasty since it just uses uint64_t as non-dispatchable handles
