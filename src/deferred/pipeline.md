- geomLight
- [optional] ssao: depends on geomLight (normals, depth)
- ao: depends on geomLight(mergable), ssao
- [optional] bloom: depends on geomLight (emission, light)
- [optional] ssr: depends on geomLight (normals, depth)
- (fwd: depends on nothing... geomLight i guess, mergable with it)
	- it shouldn't even overwrite the depth buffer the other passes use.
	  i guess we can always (if we don't want special 2nd ssr/ssao or sth
	  for transparency) merge it with geomLight, right?
	- problem is though that we probably *first* want to apply bloom,
	  ssr, ssao etc for correct blending... So this pass should probably
	  depends on combine, right?
	  otoh we could use the alpha channel to store accumulated transparency
	  alpha information and then blend with that in combine? i.e.
	  multiply the additional buffers (ssr, bloom mainly i guess)
	  with (1 - transparencyAlpha). In fwd pass multiply-blend alpha
	  (nvm, mulitply-blend doesn't exist in core vulkan)
- combine: depends on ao, ssr, bloom, (geomLight)
	- !ssr: mergable dependency on ao
- pp: depends on combine [no dependees]
	- !dof: mergable dependency on combine (and !luminance)
- [optional] luminance: depends on combine [no dependees]

---

concept: render/frame/pass graph

```
RenderGraph graph;
auto& geomLight = graph.addPass();
auto& emissionTarget = geomLight.addOut(RenderScope::flex);
auto& lightTarget = geomLight.addOut(RenderScope::flex);
auto& ldepthTarget = geomLight.addOut(RenderScope::flex);
auto& albeoTarget = geomLight.addOut(RenderScope::flex);
auto& normalsTarget = geomLight.addOut(RenderScope::flex);

if(ssrPass) {
	auto& ssr = graph.addPass();
	ssr.addIn(normalsTarget, ssr.dstScopeNormals());
	ssr.addIn(ldepthTarget, ssr.dstScopeDepth());
	auto ssrTarget = ssr.addOut(ssr.srcScopeTarget());
}

auto* postInputTarget = &lightTarget;
if(combinePass) {
	auto& combine = graph.addPass();
	if(bloomPass) {
		combine.addIn(bloomTarget, combine.dstScopeBloom());
	}
	if(ssrPass) {
		combine.addIn(ssrTarget, combine.dstScopeSSR());
	}

	auto& combined = combine.addInOut(emissionTarget, combine.scopeTarget());
	postInputTarget = &combined;
}

auto& pp = graph.addPass();
pp.addIn(*postInputTarget, pp.dstScopeInput());
if(debugMode) {
	pp.addIn(lightTarget, pp.dstScopeInput());
	// ...
}

// get barriers for flexible outputs (usually render passes)
// already create render passes using those barriers
geomLight_.init(albedoTarget.nextScope(), normalsTarget.nextScope(), ...);

// later, when recording
graph.record();
```

---

notes below are outdated, they were the first concept for a full pipeline

doesn't handle transparency yet.

# concept
some passes are entirely optional (2 for bloom; 3+4 for ssao)

- renderpass: render all primitives into gbufs
- renderpass: blur emission *compure passes?*
	- first subpass: horizontal blur into new framebuffer
	- second subpass: vertical blur into orginal framebuffer
	  *or user smaller framebuffer here? is blurred anyway now*
- renderpass: screen space: ssao *compute pass?*
- rp: blur ssao buffer *compute passes?*
	- first subpass: horizontal blur into new framebuffer
	- second subpass: vertical blur into orginal framebuffer
	  *or user smaller framebuffer here? is blurred anyway now*
- rp: shading: using gbuffers, render a hdr image (16f)
- rp: light scattering
- rp: ssr
- compute passes: bloom blur, on different mipmap levels
- rp: post processing *compute pass?*
	- uses hdr buffer from 5th rp as input
	- combine with bloom
	- tonemap the 16f image onto the final swapchain image
	- also slightly blur and apply light scattering
	  when this is enabled, we must get the hdr image as sampler instead
	  of an input attachment

compute passes probably don't work as well for tiled renderers...
when we disable ssao and bloom we could fit passes 1,5,7 into one
renderpass.

# notes

- alpha channel of light pass currently unused
	- 16f channel, expensive
- only .w of inEmission used in light.frag
	- we could put that in inNormal somehow and put objectID somewhere else...
	  but inNormal has higher precision and need that for objectID (later on)
