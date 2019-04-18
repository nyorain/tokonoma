transparency not handled (yet)
pipeline consists of 3 stages:
- render all primitives into into gbuffers
- for each light, add to render hdr image using gbuffers
- post processing hdr image -> swapchain image, use tone mapping
