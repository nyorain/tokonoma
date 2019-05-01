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
