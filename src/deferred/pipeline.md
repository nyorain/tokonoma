doesn't handle transparency yet.
some passes are entirely optional (2 for bloom; 3+4 for ssao)

- first renderpass: render all primitives into gbufs
- second renderpass: blur emission *compure passes?*
	- first subpass: horizontal blur into new framebuffer
	- second subpass: vertical blur into orginal framebuffer
	  *or user smaller framebuffer here? is blurred anyway now*
- third renderpass: screen space: ssao *compute pass?*
- 4th rp: blur ssao buffer *compute passes?*
	- first subpass: horizontal blur into new framebuffer
	- second subpass: vertical blur into orginal framebuffer
	  *or user smaller framebuffer here? is blurred anyway now*
- 5th rp: shading: using gbuffers, bloom and ssao buffer, render
  a hdr image (16f), a component is light scattering
- 7th rp: post processing *compute pass?*
	- uses hdr buffer from 5th rp as input
	- combine with bloom
	- tonemap the 16f image onto the final swapchain image
	- also slightly blur and apply light scattering
	  when this is enabled, we must get the hdr image as sampler instead
	  of an input attachment

compute passes probably don't work as well for tiled renderers...
when we disable ssao and bloom we could fit passes 1,5,7 into one
renderpass.
