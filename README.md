# Projects

Most of the projects are work in progess, they were mainly written for
the purpose of learning. So they may be useful as a resource of learning
but usually leave quite some room for improvement. Often, files
(especially the main file of a project) contains various todos
and notes about what could be done next.

- bezier: simple playground for bezier curves and anything related to it
- br: minimal gltf pbr forward renderer (including IBL)
  Most other 3D (forward) rendering projects are built from this
- cloth: Simple 2D cloth animation using verlet integration, cpu and
  gpu variants
- fluids: fluid simulation on the gpu
- iro: small game idea based on cellular automatas.
  Uses the floating point determinism defined by vulkan spirv shaders
  to work over networks
- iv: simple image viewer using the shared image functionality
  can view default file types as well as supported KTX files
  (with all mipmaps and array layers). Can also visualize cubemaps.
- normals: implements the purely 2D normal mapping effect as
  describes [here](https://github.com/mattdesl/lwjgl-basics/wiki/ShaderLesson6)
- particles: extremely simple 2D particle system using the mouse
  as attractor. Built for scalability, can run many million particles
  even on not high-end hardware.
- lpgi: light probe based global illumination, using spherical harmonics
  light probes can be set and refreshed at runtime supporting
  multiple bounces
- shv: visualize spherical harmonics coefficients
- deferred: deferred renderer implementing various (mainly screen space)
  effects. Bloom, SSR, SSAO, Luminance based exposure adaption,
  light scattering, simple DOF
- taa: 3D temporal antialiasing renderer (supporting dynamic scenes)
- volume: volume rendering via marching cubes


## Licenses

`assets/LiberationSans-*.ttf`: [SIL Open Font License 1.1](https://scripts.sil.org/cms/scripts/page.php?site_id=nrsi&id=OFL#5667e9e4)
`assets/Roboto-*.ttf`: [Apache License, Version 2.0](http://www.apache.org/licenses/LICENSE-2.0)
`assets/OpenSans-*.ttf`: [Apache License, Version 2.0](http://www.apache.org/licenses/LICENSE-2.0)
Those licenses can also be found in assets/licenses.

`assets/gravel_*`: CC0, from [https://cc0textures.com/view.php?tex=Gravel08](!)
The files in `assets/gltf` have their copyright embedded.

On windows: `meson build/win --default-library static --backend ninja`,
using the latest MinGW. Might not always be tested on windows.
