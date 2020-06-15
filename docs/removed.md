List of moved or removed projects/code
======================================

- The first great purge, 14. & 15. June 2020
	- last commit containing them: b752a6d6806f35cf75fc9f15caebcbb092044a34
	- Some breaking changes were done (mainly vpp and tkn texture/image api)
	  as well as deprecations because of new, better APIs (deprecating
	  the old camera api, ny-based App) so that many projects were left
	  not compilable. Removing projects for which maintaining is a burden
	  without them really being useful.
	- audio (moved to playground/audio):
	  Small non-gui program that plays a simple sine wave via 
	  the tkn audio infrastructure
	  moved for: no real merit, more playground-like test
	- curves: tried to model a somewhat realistic looking "band" between
	  two points, where one point could be moved with the mouse. Used
	  only rvg for rendering.
	  removed for: low quality, no real merit, wasn't fun. Project was
	  	also badly named.
	- daudio: Various audio tests (mainly written for android), testing
	  often cubeb directly. Moved to playground
	  moved for: no real merit, more like test-applications
	- sentient: Was planned as a 100% dynamic, maybe physically motivated,
	  animation of dynamic (2D) creatures. Nothing ever came of it,
	  to be re-added in future. Just removed the skeleton, basically.
	- fun: Simple rvg and Box2D based graph drawer that allowed a small entity
	  to explore the graph by "driving" over it. Hacked together as a sketch
	  using Box2D and rvg, was not pleasant to use or look at.
	  removed for: no merit, no fun, low quality
	- fluids/software: Software implementation of fluid simulation.
	  Also used software rendering, making it basically useless.
	  Porting it to the new app class would have been significant work.
	  Did only implement the gauss-seidel iteration anyways, not a lot
	  better than the hardware jacobi iteration.
	  Also likely contained some not self-written code.
	  removed for: hard to maintain, better alternative with the hardware
	  	implementation.
	- spirv: Simple non-gui program loading a compute shader written in spirv.
	  moved to playground for: no real merit, more of test/play project
	- old camera class: the old tkn::Camera and tkn::QuatCamera classes and
	  apis were removed in favor of the camera utility from tkn/camera2.hpp
	  and tkn/ccam.hpp.
	- old App class: old ny-based app class was removed in favor of tkn/app2
	  and tkn/singlePassApp (derived from the App implemented by app2).
	  They use swa as window api and divide responsibility.
	  Apps that create their own renderpass, buffers and so on can do so
	  by just deriving from App (instead of SinglePassApp) without the
	  awkwardness of the now unneeded resources from the old App class.
	  But: it contained several utilities, especially android workarounds,
	  that are not yet present/tested in app2, might need to check it out later.
