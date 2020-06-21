# Tokonoma

Small collection of personal projects related to (mostly real time)
computer graphics, simulation and animation.
Written in C++17, uses Vulkan as graphics api (no other backends, no
huge abstraction layers).
The goal here isn't to write a full graphics engine (although
this repo probably contains lots of typical engine functionality
by now) but rather to develop an environment for extremely quick prototyping
of small, mostly independent 2D and 3D apps for topics I'm currently
interested in.

# Projects

Most of the projects are work in progess and were mainly written for
the purpose of learning and playing around with.
So while they may be useful as a resource of learning,
they usually leave quite some room for improvement. Often, files
(especially the main file of a project) contains various notes, todos and
ideas about what could be done next though (or what is broken at the moment).
List of projects with some sample screenshots (noninclusive):

- automaton: experiments with cellular automata (hexagonal grid)
- bezier: simple playground for 3D bezier curves, b-splines and anything
related to it.
  ![3D B-spline screenshot](./assets/pics/bspline.png)
- br: minimal gltf pbr forward renderer (including IBL).
  Most other 3D (forward) rendering projects are built from this.
  Currently also contains some 3D audio experiments using steamaudio,
  simulating indirect environmental audio effects in realtime.
- cloth: Simple 3D cloth animation using verlet integration, includes cpu and
  gpu variants.
  ![cloth screenshot](./assets/pics/cloth1.png)
- deferred: deferred renderer implementing various (mainly screen space)
  effects. Bloom, SSR, SSAO, Luminance based exposure adaption,
  volumetric light scattering, simple DOF, lens flare, FXAA. HDR pipeline using pbr concepts (for
  lightning, most screen space algorithms are still roughly approximated
  to what looks acceptable. Work in progress)
  ![deferred screenshot](./assets/pics/deferred1.png)
- fluids: fluid simulation on the gpu
  (somewhat outdated, heard a lecture on it since then, to be reworked
  hopefully sooner than later). The screenshot shows the HSV-visualized
  velocity field.
  ![fluids screenshot](./assets/pics/fluidVel.png)
- iro: small game idea based on cellular automata.
  Uses the floating point determinism defined by vulkan spirv shaders
  to serialize/synchronize over a small udp protocol (wip).
  ![iro screenshot](./assets/pics/iro1.png)
- iv: simple image viewer using the shared image functionality.
  Can view default file types as well as supported KTX files
  (with all mipmaps and array layers). Can also visualize cubemaps.
- lpgi: light probe based global illumination, using spherical harmonics.
  Light probes can be set and refreshed at runtime, simulating
  multiple light bounces
  ![lpgi screenshot](./assets/pics/lpgi2.png)
- normals: implements the purely 2D normal mapping effect as
  described [here](https://github.com/mattdesl/lwjgl-basics/wiki/ShaderLesson6)
- particles: simple 2D particle system using the mouse
  as attractor. Built for scalability, can run many million particles
  even on not high-end hardware (the screenshot shows
  10 million transparent particles). Can also function as an audio
  visualizer (for audio files or system audio on linux via pulse) using
  the particles, varying the attraction based on particle properties
  and current audio frequencies.
  ![particles screenshot](./assets/pics/particles1.png)
- gen: command line utility program for stuff required by the pbr renderer projects,
  like generating cubemaps from equirectangular environment maps, baking irradiance maps or
  specular IBL maps and the brdf lookup table for the split-sum approximation.
  Can also project a cubemap to spherical harmonics coefficients (9x) or
  bake multiple daytime layers and metadata of a Hosek-Wilkie sky cubemap.
- pendulum: simulation of a simple pendulum from an exercise in a
  computational engineering and robotics lecture i heard.
  The challenge is to keep the pendulum standing upwards (unstable stationary
  solution of the underlying differential equation)
  ![pendulum screenshot](./assets/pics/pendulum1.png)
- pursuers: small system of particle pursuers resulting in interesting
  patterns
  ![pursuers screenshot](./assets/pics/pursuers1.png)
- rays: 2D light model, rendered using a ray/path tracing approach and line
  rendering.
  ![rays screenshot](./assets/pics/rays.png)
- repro: first prototype of a game with visual perception based on taking
  snapshots of the current view and then depth-reprojecting it as one is
  moving through it
  ![repro screenshot](./assets/pics/repro.png)
- Simulation of light scattering (rayleigh + mie) through the atmosphere.
  ![scatter sunset](./assets/pics/scatter1.png)
  Also allows moving out of the planets atmosphere, observing it from space.
  Uses a quaternion-based free-moving camera to allow intuitive space movement
  (since there is no "up" direction in space).
  ![scatter atmosphere](./assets/pics/scatter2.png)
- sen: first raytracing and path tracing experiments (mainly a cornell-box),
  done via compute shaders (or similar methods) on the gpu.
  Not even physically-based yet, just playing around with the basic concepts.
- shv: visualize spherical harmonics coefficients
- smooth_shadow: analytical 2D smooth shadows via shadow polygons
  ![smooth shadow screenshot](./assets/pics/smooth_shadow1.png)
- sss: Comparable to smooth_shadow but uses a subsurface-scattering-like effect
  to achieve smooth shadows, i.e. the strength of light is "reduced" when traveling
  through an object
- taa: 3D temporal antialiasing renderer (supporting dynamic scenes)
  playground for different effects that have a synergy with TAA
  (mainly based on noise) like stochastic transparency and noisy shadow
  sampling via a poisson disk
- tess: Playground for tessellation. Ditched in favor of the
  dynamic subdivision using compute shaders.
- volume: volume rendering (mainly of 3D functions, no standard
  3D texture/volume file format supported yet) via marching cubes
  ![volume screenshot](./assets/pics/volume1.png)
- subd: Dynamic terrain subdivision based on the 2018 paper "Adaptive GPU
Tessellation with Compute Shaders" by Jad Khoury, Jonathan Dupuy, and
Christophe Riccio. Renders a whole planet with free-moving quaternion camera.
  ![terrain screenshot](./assets/pics/subd.png)

There is some shared code between the projects (like an image loading library,
gltf scene loading and management, various small rendering and computation
helpers as well as general C++ utility that I haven't found a better
place for yet, e.g. 16-bit floats) which is located in the `tkn/`
subfolder, compiled into `libtkn`.

# Screenshots

The gui in all the screenshots uses my (at the time of writing quite
experimental) vulkan-only and retained-mode gui toolkit [vui](https://github.com/nyorain/vui).
As you can see in some screenshots, it includes a component roughly modeled after
the excellent [dat.gui](https://github.com/dataarts/dat.gui).

## Licenses

Unless stated otherwise, the code (in `src`) is available under the MIT license, 
see LICENSE. Some projects (mainly the ones where some time was invested 
or new techniques developed, like `rays`, `repro`, `iro` or `smooth-shadow`) may 
use GPL or other licenses, they have their own LICENSE file in their `src/` folder then.
Additional external licenses for distributed code/assets:

- `assets/LiberationSans-*.ttf`: [SIL Open Font License 1.1](https://scripts.sil.org/cms/scripts/page.php?site_id=nrsi&id=OFL#5667e9e4)
- `assets/Roboto-*.ttf`: [Apache License, Version 2.0](http://www.apache.org/licenses/LICENSE-2.0)
- `assets/OpenSans-*.ttf`: [Apache License, Version 2.0](http://www.apache.org/licenses/LICENSE-2.0)
- `assets/gravel_*`: CC0, from [https://cc0textures.com/view.php?tex=Gravel08](!)

The files in `assets/gltf` have their copyright embedded.
The projects in `external` have their licenses included into the files
or in the `external/licenses` folder.

---

On windows: `meson build/win --backend ninja`, using the latest MinGW (at least gcc 8.x).
Linking shared libraries in different folders on windows is somewhat problematic
since there is no RPATH equivalent on windows. Therefore you have to do one of the following
things for applications to work out of the box (otherwise you'll get `libXXX.dll is missing`
when trying to execute the applications out of the build folder):

- __Recommended__: Just execute the `docs/links.bat` script in the build dir once,
  it will link all required shared libraries into the build dir. Note that since
  windows requires elevated command prompts for creating links by default you
  either have to disable this restriction by enabling developer mode (the __recommended__ way, see 
  e.g. [here](https://www.ghacks.net/2016/12/04/windows-10-creators-update-symlinks-without-elevation/))
  or run it in an elevated prompt (the script is dead simple, just read it beforehand).
- Use `layout=flat` meson option. This will cause all libraries and executables to be
  output into the same folder.
- Use `default_library=static` meson option. This might lead to problems, is not well tested
  and therefore not recommended. Issue reports welcome though.

Not all projects are always well-tested for windows, it's only done once
in a while (or when I'm working on features that are not supported on Linux yet, such
as HDR as of early 2020) so the code may have small issues there. 
But all projects are in general written in a cross-platform manner so those issues are 
usually minor issues or related to dependency management.
