Notes and todo list for the deferred renderer and the passes.
Also see the individual todos in the pass headers/sources they are often
enough not listed here

## features to implement:

- lens flare
- better dof implementations, current impl is naive
  probably best to use own pass, see gpugems article
  bokeh blurring
    - make dof etc as physically based as possible
      note where it's not possible
- reflectance probe rendering/dynamic creation
- support switching between environment maps
- temporal anti aliasing (TAA), tolksvig maps
  e.g. https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/course-notes-moving-frostbite-to-pbr-v2.pdf

## urgent todos

- scene/light is messed up. Constructor takes unused, weird descriptor
  set
- re-add lost features: light/object picking, dynamically adding lights etc
  just see TODOs and commented out sections
- timeWidget currently somewhat hacked together, working
  around rvg quirks triggering re-record while recording...
  probably better to add passes to timeWidget (getting an id)
  in initPasses and then use that id when recording to record
  a timestamp. Shouldn't be too hard given that initPasses currently
  also records the passes, just capture by value
- fix diretional light shadow in screenshot/probe
- better transparency light model. Respect aoFactor and apply
  lightning from environment maps
- not sure if this whole transparency sorting is worth it.
  doesn't work in a lot of cases.
  merge oit somehow? allow to use that?
  the sorting isn't really expensive but the whole "one buffer indirect"
  is way harder to manage (try to implement dynamic loading
  of primitives?). Probably not bad for performance though,
  AZDO, allows culling (TODO: implement basic culling).
  alternative to transparency sorting: just allow one layer of transparency
 group initial layout transitions from undefined layout to general
  per frame? could do it externally, undefined -> targetScope()
  for luminance (compute), ssr, ssao (compute)
  Could check in FrameGraph if initialLayout (add something like that)
  equals finalLayout and if not so, add initial barrier
- fix ldv value for point light scattering
  i.e. view-dir dependent scattering only based on attenuation?
  probably makes no sense, then properly document it in settings
- fix light attenuation for point lights.
  old notes (some of that implemented now, some of that was a bad idea)
  make it independent from radius by normalizing distance from light
  (dividing by radius) before caluclating attunation.
  currently done, has some problems as well though, fix those.
    -> how to handle light attenuation for point lights?
     see scene.glsl:attenuation, does it make sense to give each light
     its own parameters? normalize distance by radius before passing
     it to attenuation? parameters that work fine for a normalized
     distance are (1, 8, 128), but then lights are rather weak
- deferred light initialization, re-implement adding lights
- rgba16snorm (normalsFormat) isn't guaranteed to be supported
  as color attachment... i guess we could fall back to rgba16sint
  which is guaranteed to be supported? and then encode it
- we don't need one extra ubo/ds for environment.
  the whole fixedMatrix stuff is rather bad, we could just add
  the position in skybox.vert to viewPos and use the normal matrix

## further ideas

- blur bloom/apply dof depending on light? the brighter,
  the more blur (e.g. dof), is it maybe possible to merge bloom and dof?
- we could render the skybox implicitly in the ao pass (we
  can calculate the view direction there in world space) instead of
  Environment:render. Would abolish the need for a ds/dslayout in
  Environment and additional render call (not sure how to stay
  compatible with ddd though).
- support destroying pass render buffers like previously
  done on deferred. Only create them is needed/pass is active.
- support dynamic shader reloading (from file) on pass (re-)creation?
- improve ssr performance. Try out linear sampling, at-least-one-stepsize,
  maybe we can work out a variable step size algorithm?
- blur ssr in extra pass (with compute shader shared memory,
  see ssrBlur.comp wip)? Might improve performance
  and allow for greater blur. Also distribute factors (compute guass +
  depth difference) via shared variables.
  maybe just generate mipmaps for light buffer, i guess that is how
  it's usually done.
  probably not worth it though. have to improve ssr performance in another way
  Also see combine pass, it's really only needed for ssr!
- do we really need depth mip levels anywhere? test if it really
  brings ssao performance improvement. Otherwise we are wasting this
  whole "depth mip map levels" thing
- allow light scattering per light?
  we could still apply if after lightning pass
  probably best to just use one buffer for all lights then
  probably not a bad idea to use that buffer with additional blending
  as additional attachment in the light pass, since we render
  point light box there already.
  We probably can get away with even less samples if we use the better
  4+1 blur (that will even be faster!) in combine

low prio:

- look at adding smaa. That needs multiple post processing
  passes though... Add it as new class
- more an idea:
  shadow cube debug mode (where cubemap is rendered, one can
  look around). Maybe use moving the camera then for moving the light.
  can be activated via vui when light is selcted.
  something comparable for dir lights? where direction can be set by
  moving camera around?

## optimization-oriented ideas/todos

most of these are more of a guess, have to test/benchmark

- look into using textureProj in shaders for w division?
- better shadow map allocation/re-use shadow maps.
  one is theoretically enough with a deferred renderer, try out
  if rendering like that (pass switching) has an significant impact on
  performance when there are a couple of lights
- don't recreate renderpasses, layouts and pipelines on
  every initPasses, only re-create what is really needed.
- correctly use byRegion dependency flag where possible?
  already using it in geomLight pass where it should have largest
  effect, e.g. for tiled renderers. Not sure if it has any effect
  across multiple render/compute passes.
- updateDevice flag that updates all pass parameters.
  doesn't need to happen every frame
- when ssr is disabled (and/or others?) we can probably do
  combine and pp pass in one pass.
- the shadow pipelines currently bind and pass through
  both tex coords, also have normals declared as input (which they don't
  need). implement alternative primitive rendering mode, where it
  just binds the texCoords buffer needed for albedo, and then use
  a vertex input with just one texCoord and without normals in light
- look into textureOffset/textureLodOffset functions for
  shaders. We use this functionality really often, might get
  something for free there
- when shaderStorageImageExtendedFormats is supported,
  we can perform pretty much all fullscreen passes (ssao, ssr, combining,
  ssao blurring) as compute shaders. For tknng it with post processing/
  tonemapping we'd also need to create the swapchain with storage image
  caps *and* know that it uses a format that is supported... so probably
  stick with fullscreen render pass there.
  Some of the above mentioned don't need extended formats, those should
  definietly be moved to compute passes.
  NOTE: for some passes (and their inputs) that means giving up
  input attachments though. Is that a performance loss anywhere?
  input attachments are probably only relevent when using multiple
  subpasses, right?
- try out other mechanisms for better ssao cache coherency
  we should somehow use a higher mipmap level i guess
- we could theoretically just use one shadow map at all.
  requires splitting the light passes though (rendering with light pipe)
  so might have disadvantage. so not high prio
- we might be able to re-use more gbuffers for rendering later on
  in some cases (like we do with emission for the combine pass)
- more efficient point light shadow cube map
  rendering: only render those sides that we can actually see...
  -> nothing culling-related implemented at all at the moment
- we currently don't use the a component of the
  bloom targets. We could basically get a free (strong) blur there...
  maybe good for ssao or something?
- just treat light scattering as emission? get the strong blur for free.
  might be way too strong though... not as alpha (see todo above) but simply
  add it to the emission color

# further notes

- investigate/compare deferred lightning elements?
  http://gameangst.com/?p=141
- we always use ssao, even when object/material has ao texture.
  In that case both are multiplied. That's how its usually done, see
  https://docs.unrealengine.com/en-us/Engine/Rendering/LightingAndShadows/AmbientOcclusion
- ssao currently independent from *any* lights at all, only has effect
  on ambient occlusion
- we currently completely ignore tangents and compute that
  in the shader via derivates
- could investigate reversed z buffer
  http://dev.theomader.com/depth-precision/
  https://nlguillemot.wordpress.com/2016/12/07/reversed-z-in-opengl/
- ssr needs hdr format since otherwise we get artefacts for storing
  the uv value when the window has more than 255 pixels.
- we currently apply the base lod (mipmap level 0) of the emission
  texture in the combine pass (before ssr) but only apply the other
  emission lods (blooms, mipmap levels >0) after/together with ssr
  in the pp pass. Bloom and ssr don't work well together but we still
  want emissive objects to be reflected. They just won't have bloom
  in the reflection. Could be fixed by reading the emission buffer
  during ssr but that's probably really not worth it!

