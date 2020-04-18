#pragma once

#include <tkn/texture.hpp>
#include <tkn/scene/light.hpp>
#include <tkn/scene/scene.hpp>
#include <tkn/scene/environment.hpp>

#include <vpp/handles.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>

// TODO, WIP: connects all parts to render a scene, lights and environment
// TODO: rename from meta to scene renderer?

namespace tkn {

struct SceneRenderer {
	struct {
		vpp::Sampler linear;
		vpp::Sampler nearest;
	} samplers;

	struct {
		bool anisotropy;
		bool multiview;
		bool depthClamp;
		bool multiDrawIndirect;
	} features;

	vpp::ViewableImage dummyTex;

	vpp::SubBuffer cameraUbo;
	vpp::TrDs cameraDs;
	vpp::TrDsLayout cameraDsLayout;
	vpp::PipelineLayout pipeLayout;

	vpp::TrDsLayout aoDsLayout;
	vpp::TrDs aoDs;
	vpp::SubBuffer aoUbo;

	tkn::Scene scene;
	tkn::ShadowData shadow;
	tkn::Environment environment;
};

} // namespace tkn
