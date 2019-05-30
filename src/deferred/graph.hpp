// wip, highly experimental
// always thought something like this was just bullshit, introducing a
// lot of complexibility but when you dynamically want to select
// the passes you need, it's really no fun (or good idea) to do it
// manually...

#pragma once

#include "pass.hpp"
#include <vpp/vk.hpp>
#include <vector>
#include <deque>
#include <functional>
#include <unordered_set>

// TODO: support non-preserving barriers (preserve bool in SyncScope?)
// only relevant as dst scope though
// TODO: allow byRegion dependencies? not sure if it really has an
// effect between multiple distict passes though

class FrameGraph;
class FramePass;
struct SyncScopeFlexTag {} syncScopeFlex;

struct RenderData {
	vk::CommandBuffer cb;
	vk::Framebuffer fb;
	vk::Extent2D size;
	void* data {}; // additional
};

/// Represents immutable frame target.
/// There can be multiple targets for the same image.
struct FrameTarget {
	const vk::Image* target {};
	vk::ImageSubresourceRange subres {vk::ImageAspectBits::color, 0, 1, 0, 1};

	// passes that read this content
	struct Slot {
		FramePass* pass {};
		SyncScope scope {};
		bool flex {};
	};
	Slot producer;
	std::deque<Slot> consumers;
	FramePass* end {}; // optional: pass that modifies it

	// recording
	bool available {};
	bool barriered {};
	SyncScope current {};
};

class FramePass {
public:
	std::function<void(const RenderData& data)> record;

public:
	void addIn(FrameTarget& target, SyncScope scope) {
		target.consumers.push_back({this, scope, false});
		in_.push_back({&target, &target.consumers.back()});
	}

	void addIn(FrameTarget& target, SyncScopeFlexTag) {
		target.consumers.push_back({this, SyncScope {}, true});
		in_.push_back({&target, &target.consumers.back()});
	}

	FrameTarget& addOut(SyncScope scope, const vk::Image&);
	FrameTarget& addOut(SyncScopeFlexTag, const vk::Image&);

	// TODO: suport flex sync scopes?
	FrameTarget& addInOut(FrameTarget& target, SyncScope scope);
	FrameTarget& addInOut(FrameTarget&, SyncScope dst, SyncScope src);

protected:
	friend class FrameGraph;
	FramePass(FrameGraph& graph) : graph_(&graph) {}
	FrameGraph* graph_;

	struct Content {
		FrameTarget* target;
		FrameTarget::Slot* slot;
	};

	std::vector<Content> in_;
	std::vector<FrameTarget*> out_;
	bool done_ {}; // recording
};

class FrameGraph {
public:
	void compute() {
		// reset
		for(auto& pass : passes_) {
			pass.done_ = false;
		}

		for(auto& target : targets_) {
			target.available = false;
		}

		// record
		bool next = true;
		while(next) {
			next = false;
			// add smarter pass selection algorithms/heuristics at least...
			// maybe first try to find a pass that wouldn't require an
			// additional barrier? make sure to execute all of those.
			// otherwise execute those executable passes that can
			// be grouped into one big barrier?
			for(auto& pass : passes_) {
				if(!pass.done_) {
					next |= !add(pass);
				}
			}
		}
	}

	bool add(FramePass& pass) {
		for(auto& in : pass.in_) {
			if(!in.target->available) {
				return false;
			}

			// if this pass modifies the given input, we also have to
			// make sure that all other passes consuming it have
			// finished.
			if(&pass == in.target->end) {
				for(auto& other : in.target->consumers) {
					if(&pass != other.pass && !other.pass->done_) {
						return false;
					}
				}
			}
		}

		auto& record = order_.emplace_back();
		record.pass = &pass;

		// for all inputs: make sure there is a barrier
		for(auto& in : pass.in_) {
			auto needed = in.slot->scope;
			auto current = in.target->current;

			if(in.slot->flex) {
				if(!in.target->barriered && in.target->producer.flex) {
					// in this case producer and consumer are both
					// flexible. We just decide here to use the general
					// layout in between.
					in.target->producer.scope = {
						vk::PipelineStageBits::bottomOfPipe,
						vk::ImageLayout::general, {}
					};
					in.target->current = in.target->producer.scope;

					// TODO: the allCommands here may be
					// expensive, not sure how to solve it. Weird that
					// the most flexible case would be the most expensive...
					// probably best to require SyncScopes even for flexible
					// producers/consumers just for this case...
					in.slot->scope = {
						vk::PipelineStageBits::allCommands,
						vk::ImageLayout::general, {}
					};
				} else {
					in.slot->scope = in.target->current;
				}

				in.target->barriered = true;
				continue;
			}

			// already barriered
			// whoever barriered it made sure to include us since
			// we have the same layout!
			// except when this passes changes it, then a barrier
			// is obviously needed (even when there is no transition,
			// e.g. for general -> general)
			if(in.target->barriered && needed.layout == current.layout &&
					&pass != in.target->end) {
				dlg_assert(current.access & needed.access);
				dlg_assert(current.stages & needed.stages);
				continue;
			}

			// add barrier, try to include as many other passes as possible
			for(auto& out : in.target->consumers) {
				if(out.scope.layout != needed.layout) {
					continue;
				}

				needed |= out.scope;
			}

			// if the producer of this target is flex and there hasn't been
			// a barrier yet, set the required barrier and don't push
			// a manual barrier
			if(!in.target->barriered && in.target->producer.flex) {
				in.target->producer.scope = needed;
			} else {
				Barrier barrier;
				barrier.src = current;
				barrier.dst = needed;
				barrier.target = in.target;
				record.barriers.push_back(barrier);
			}

			in.target->barriered = true;
			in.target->current = needed;
		}

		pass.done_ = true;

		for(auto& out : pass.out_) {
			out->available = true;
			out->current = out->producer.scope;
		}

		// NOTE: this is optional.
		// check if any direct dependencies can be run
		// this recursive calls means we try depth first where possible
		// maybe only do it for passes that wouldn't require an additional
		// barrier?
		// for(auto& outTarget : pass.out_) {
		// 	for(auto& outPass : outTarget->consumers) {
		// 		if(!outPass.pass->done_) {
		// 			add(*outPass.pass);
		// 		}
		// 	}
		// }

		return true;
	}

	void record(const RenderData& data) {
		auto cb = data.cb;
		for(auto& pass : order_) {
			if(!pass.barriers.empty()) {
				std::vector<vk::ImageMemoryBarrier> vkBarriers;
				vk::PipelineStageFlags srcStages = {};
				vk::PipelineStageFlags dstStages = {};
				vkBarriers.reserve(pass.barriers.size());
				for(auto& b : pass.barriers) {
					dlg_assert(b.target->target);
					dlg_assert(*b.target->target);

					srcStages |= b.src.stages;
					dstStages |= b.dst.stages;

					auto& barrier = vkBarriers.emplace_back();
					barrier.image = *b.target->target;
					barrier.srcAccessMask = b.src.access;
					barrier.oldLayout = b.src.layout;
					barrier.dstAccessMask = b.dst.access;
					barrier.newLayout = b.dst.layout;
					barrier.subresourceRange = b.target->subres;
				}

				vk::cmdPipelineBarrier(cb, srcStages, dstStages, {}, {}, {},
					vkBarriers);
			}

			pass.pass->record(data);
		}
	}

	// Returns whether the current graph has any cycles or unsatisfiable
	// dependencies (e.g. a pass depends on two versions of a target).
	bool check() {
		std::unordered_set<FramePass*> seen;
		for(auto& pass : passes_) {
			seen.clear();
			if(!check(pass, seen)) {
				return false;
			}
		}

		return true;
	}

	bool check(FramePass& pass, std::unordered_set<FramePass*>& seen) {

		auto [it, inserted] = seen.insert(&pass);
		if(!inserted) {
			dlg_info("Detected FrameGraph cycle");
			return false;
		}

		// check whether two inputs are the same image
		std::unordered_set<const vk::Image*> seenImages;
		for(auto& in : pass.in_) {
			auto [it, inserted] = seenImages.insert(in.target->target);
			if(!inserted) {
				dlg_info("Detected FrameGraph pass that depends on multiple "
					"version of the same target");
				return false;
			}
		}

		// check all outgoing edges
		for(auto& out : pass.out_) {
			for(auto& consumer : out->consumers) {
				std::unordered_set<FramePass*> copy = seen;
				if(!check(*consumer.pass, copy)) {
					return false;
				}
			}
		}

		return true;
	}

	FramePass& addPass() { passes_.push_back({*this}); return passes_.back(); }
	FrameTarget& addTarget() { return targets_.emplace_back(); }

	// TODO: mainly for testing
	struct Barrier {
		SyncScope src;
		SyncScope dst;
		FrameTarget* target;
	};

	struct Pass {
		std::vector<Barrier> barriers;
		FramePass* pass;
	};

	const auto& order() const { return order_; }

protected:
	std::deque<FramePass> passes_;
	std::deque<FrameTarget> targets_;

	std::vector<Pass> order_;
};


inline FrameTarget& FramePass::addOut(SyncScope scope, const vk::Image& img) {
	auto& target = graph_->addTarget();
	target.target = &img;
	target.producer = {this, scope, false};
	out_.push_back(&target);
	return target;
}

inline FrameTarget& FramePass::addOut(SyncScopeFlexTag, const vk::Image& img) {
	auto& target = graph_->addTarget();
	target.target = &img;
	target.producer = {this, {}, true};
	out_.push_back(&target);
	return target;
}

inline FrameTarget& FramePass::addInOut(FrameTarget& target, SyncScope scope) {
	target.consumers.push_back({this, scope, false});
	dlg_assertm(!target.end, "Target can only be modified by one pass");
	target.end = this;
	in_.push_back({&target, &target.consumers.back()});

	auto& next = graph_->addTarget();
	next.target = target.target;
	next.producer = {this, scope, false};
	out_.push_back(&next);
	return next;
}

inline FrameTarget& FramePass::addInOut(FrameTarget& target,
		SyncScope dst, SyncScope src) {
	target.consumers.push_back({this, dst, false});
	dlg_assertm(!target.end, "Target can only be modified by one pass");
	target.end = this;
	in_.push_back({&target, &target.consumers.back()});

	auto& next = graph_->addTarget();
	next.target = target.target;
	next.producer = {this, src, false};
	out_.push_back(&next);
	return next;
}
